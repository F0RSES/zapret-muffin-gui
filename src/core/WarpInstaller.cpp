#include "WarpInstaller.h"

#include "PrivilegeHelper.h"
#include "WarpManager.h"
#include "ZapretSource.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QTimer>

namespace zapret {

namespace {

// Every fact about where Cloudflare's software comes from, spelled once. These are copied from
// https://pkg.cloudflareclient.com/ (the page the WARP client documentation points an apt host at) and
// from the client's own Windows page; a value that drifts turns the install into "apt could not locate
// package cloudflare-warp" or a 404, and neither failure carries a clue in it. coretest asserts each
// one by name, so the day Cloudflare moves the key the test says which constant was wrong.
const char kPublicKeyUrl[] = "https://pkg.cloudflareclient.com/pubkey.gpg";
const char kRepositoryUrl[] = "https://pkg.cloudflareclient.com/";
const char kKeyringPath[] = "/usr/share/keyrings/cloudflare-warp-archive-keyring.gpg";
const char kSourcesListPath[] = "/etc/apt/sources.list.d/cloudflare-client.list";
const char kPackageName[] = "cloudflare-warp";

// The suite to ask the repository for on a host that names *itself* instead of naming a release the
// repository publishes. Cloudflare builds for upstream Debian and Ubuntu suites only; an installed Kali
// writes VERSION_CODENAME=kali-rolling into /etc/os-release, there is no such directory on the server
// (curl says 404 for it and 200 for bookworm), `apt-get update` fails there, `set -eu` stops the script,
// and the user is left with the exit 100 this constant exists to prevent.
//
// bookworm rather than the newer suite Kali currently tracks: a Debian package built against an older
// glibc runs on every newer one, so the single answer holds for a Kali generation that has since moved
// on, and it is also the suite Cloudflare's own Linux instructions name for a host that cannot be
// pinned. Should that suite ever be dropped, this is the one line to change, and coretest pins it by name.
const char kDerivativeFallbackSuite[] = "bookworm";

// The release-name family that fallback covers: every Kali label — "kali-rolling" from an installed
// system, "kali-<snapshot>" from a live image, bare "kali" from a minimal one — starts with this, and
// not one of them is an apt suite the repository has ever shipped.
const char kDerivativeReleaseFamily[] = "kali";

const char kInstallerUrl[] = "https://downloads.cloudflareclient.com/v1/download/windows/ga";

// The staged MSI's name. Fixed rather than taken from the response's Content-Disposition because a name
// that arrives over a network is a name that can hold anything, and this path is the one handed to
// msiexec. Nothing about the file's real name matters to the install — only its bytes do.
const char kInstallerFileName[] = "Cloudflare-WARP.msi";

// Same reason ZapretDownloader sends one: a bare HTTP request with no User-Agent is answered 403 by
// some CDNs, and the version string is what shows up in Cloudflare's traffic if anyone wonders who
// pulled the installer.
const char kUserAgent[] = "Muffin/0.1";

// What the Windows package calls its two halves. The GUI is the thing this application closes; the
// service is the thing that keeps the tunnel, and killing it is the mistake a careless /IM makes.
const char kGuiImageLegacy[] = "Warp.exe";
const char kGuiImageCurrent[] = "Cloudflare WARP.exe";
const char kInstallDirSuffix[] = "/Cloudflare/Cloudflare WARP";
const char kCliFileName[] = "warp-cli.exe";

// msiexec's own numbers. 0 is the ordinary success; 3010 (ERROR_SUCCESS_REBOOT_REQUIRED) is the package
// installed with a reboot pending — an installed client either way, and calling the second one a
// failure would have the interface offer to install the thing it just installed.
constexpr int kMsiSuccess = 0;
constexpr int kMsiRebootRequired = 3010;

// How much of the package manager's output is kept for a report. apt is chatty, and the part that
// explains a failure is at the end, so the tail is what survives.
constexpr qsizetype kOutputTailChars = 2000;

// The phrases that say a fetch died on the wire rather than on a decision apt made. Each is a string
// curl or apt writes when a connection is the thing that failed — a stall that ran past the timeout, a
// host that refused, a name that would not resolve — and none of them appears in the failures this
// class must NOT call a network problem: a 404 is spelled by the server's status line, a held dpkg lock
// and a missing dependency are apt's own "E:" sentences, and a refused password never reaches a fetch
// at all. Matching on the transport words, not on a generic "E:" or on the exit code, is what keeps
// those three out of the answer. Lower-cased on both sides; see isNetworkFailure() for why.
const char *const kNetworkFailureMarkers[] = {
    "timed out",
    "could not connect",
    "unable to connect",
    "failed to connect",
    "cannot initiate the connection",
    "connection refused",
    "connection reset",
    "network is unreachable",
    "no route to host",
    "temporary failure resolving",
    "could not resolve host",
    "name not resolved",
    "failed to fetch",
    "was not able to reach",
};

} // namespace

WarpInstaller::WarpInstaller(QObject *parent)
    : QObject(parent)
    , m_platform(ZapretSource::hostPlatform())
{
}

WarpInstaller::~WarpInstaller()
{
    // Killing a package manager halfway is exactly the outcome this class refuses to offer a button
    // for — so it happens only when the owning window is going away, where the alternative is a root
    // process holding a pipe nobody will ever read again. Qt's own destructor would do the same thing
    // to a child QProcess; doing it here is what makes the trade-off visible at the site of it.
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    if (m_reply)
        m_reply->abort();
    cleanupStaging();
}

void WarpInstaller::setPlatform(Strategy::Platform platform)
{
    m_platform = platform;
}

Strategy::Platform WarpInstaller::platform() const
{
    return m_platform;
}

void WarpInstaller::setHideWarpGui(bool on)
{
    m_hideWarpGui = on;
}

void WarpInstaller::setRepair(bool repair)
{
    m_repair = repair;
}

// ---- the repository, as Cloudflare publishes it ----

QString WarpInstaller::publicKeyUrl() { return QLatin1String(kPublicKeyUrl); }
QString WarpInstaller::repositoryUrl() { return QLatin1String(kRepositoryUrl); }
QString WarpInstaller::keyringPath() { return QLatin1String(kKeyringPath); }
QString WarpInstaller::sourcesListPath() { return QLatin1String(kSourcesListPath); }
QString WarpInstaller::packageName() { return QLatin1String(kPackageName); }

QString WarpInstaller::repositoryLine(const QString &codename)
{
    // signed-by first, then the URL, then the suite, then the component — the shape the repository
    // documents, and the shape the generated script writes. Built from the accessors rather than from a
    // second copy of the literals, which is what lets a test pin the two to each other.
    return QStringLiteral("deb [signed-by=%1] %2 %3 main")
        .arg(keyringPath(), repositoryUrl(), codename);
}
bool WarpInstaller::isUsableCodename(const QString &codename)
{
    // The value reaches a script a root shell reads, so the grammar allowed is exactly "a suite name":
    // lowercase letters, digits, dot, dash, underscore, at least one of them. No space, no newline, no
    // quote, no `$(`, no slash — and no uppercase, because a repository's Release file names itself in
    // lowercase and an apt host that writes `Noble` gets a suite that exists nowhere.
    //
    // This is a public function rather than a loop inside the generator because the safety of the
    // generated script is the safety of this one value, and a test has to be able to hand in the string
    // it is trying to break with, rather than only trusting that it would be refused.
    if (codename.isEmpty())
        return false;
    for (const QChar c : codename) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                        || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                        || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('_');
        if (!ok)
            return false;
    }
    return true;
}

// The script's generator. Both public spellings come through here, and the only difference between a
// first install and a repair is the handful of lines spliced in after the variables and before the
// tool check: after the variables because the preamble reads `$PACKAGE`, before the tool check because
// it must not be held hostage by a missing curl it does not need.
QString WarpInstaller::composeScript(const QString &codename, const QStringList &preamble)
{
    // The generated script. Its only substitution is `codename`, and the caller has to pass
    // isUsableCodename() first — which is why the two are separate public functions: reading this text
    // is then reading the whole of what a root shell will execute, with nothing assembled at runtime.
    //
    // It runs as root and holds no `sudo`, because pkexec (or an already-root session) put it there;
    // and `set -eu` is not decoration. Without -e an `apt-get update` that could not reach the new
    // repository would be followed by an `install` answering "unable to locate package", and those two
    // failures look nothing alike in a report — the first is a network or a suite name, the second reads
    // as Cloudflare having renamed the package.
    QStringList lines;
    lines << QStringLiteral("#!/bin/sh")
          << QStringLiteral("# Generated by WarpInstaller::linuxScriptBody(). Runs as root, asks "
                           "nothing, presses no keys, and stops at the first failed step.")
          << QStringLiteral("set -eu")
          << QString()
          << QStringLiteral("CODENAME='%1'").arg(codename)
          << QStringLiteral("KEYRING='%1'").arg(keyringPath())
          << QStringLiteral("SOURCES='%1'").arg(sourcesListPath())
          << QStringLiteral("KEY_URL='%1'").arg(publicKeyUrl())
          << QStringLiteral("PACKAGE='%1'").arg(packageName())
          << QString();
    lines << preamble;
    lines
          // The three tools the install cannot do without. Naming the missing one is the difference
          // between a report the user can act on and a shell's bare "command not found" — curl in
          // particular is absent from minimal container images and from a Debian netinst.
          << QStringLiteral("for tool in curl gpg apt-get; do")
          << QStringLiteral("    command -v \"$tool\" >/dev/null 2>&1 || { "
                           "echo \"WARP install: this host has no $tool\" >&2; exit 20; }")
          << QStringLiteral("done")
          << QStringLiteral("mkdir -p \"$(dirname \"$KEYRING\")\"")
          << QString()
          // The two writes go straight to their targets rather than through tee: tee exists to satisfy
          // `sudo tee` from an unprivileged shell, and this shell is already the privileged one.
          << QStringLiteral("curl -fsSL \"$KEY_URL\" | gpg --yes --dearmor --output \"$KEYRING\"")
          // The entry that is already there goes before the new one is written. `printf > "$SOURCES"`
          // would truncate this exact path anyway, and the unlink is here for the case truncation does
          // not cover: a previous attempt — this installer's own, the vendor's published one-liner, or a
          // hand-written line naming a suite the server has never had — leaves an entry that 404s, and
          // apt reads the whole directory. `rm -f` is idempotent, so a first install cannot tell.
          << QStringLiteral("rm -f \"$SOURCES\"")
          << QStringLiteral("printf 'deb [signed-by=%s] %s %s main\\n' "
                            "\"$KEYRING\" '%1' \"$CODENAME\" > \"$SOURCES\"")
                 .arg(repositoryUrl())
          // The update is told to accept changed release information, because the line above can replace
          // a suite apt has already catalogued for this URI — a re-run after a fix, or the switch from
          // the derivative's own name to the upstream suite that is the reason for repositorySuite() —
          // and from apt 2.7 onwards that difference is an error rather than a warning. Without the flag
          // the repair would fail with "changed its 'Suite' value" instead of installing the package.
          << QStringLiteral("apt-get update --allow-releaseinfo-change")
          // The line the whole class exists for, spelled as the repository documents it, with the
          // frontend kept non-interactive because nobody is watching a terminal — the same reason every
          // verb in WarpManager carries --accept-tos.
          //
          // The three `-o` options are what the unstable links this install keeps meeting need. The
          // package is roughly 75 MB, and apt's own default — a 30 second wait per connection and no
          // retry at all — is shorter than the pause a CDN sitting behind a rate limiter leaves between
          // packets, so a healthy-but-slow download dies mid-transfer and the user is left with the
          // bare `apt-get failed (exit code 100)` this comment describes. `Retries` re-issues a fetch
          // that broke rather than giving up on it, and the two `Timeout` values raise how long each
          // attempt waits; http and https are separate keys because apt reads them separately, so
          // naming only one leaves the other on its default.
          //
          // The primary install sits inside `if !` for the reason the fallback needs it: `set -eu`
          // stops a top-level failing command before any later line could run, and this is the one
          // failure with a second attempt worth making. The exit code is what the whole operation
          // reports on, exactly as it did when the line stood alone.
          << QStringLiteral("if ! DEBIAN_FRONTEND=noninteractive apt-get "
                            "-o Acquire::Retries=3 -o Acquire::http::Timeout=60 "
                            "-o Acquire::https::Timeout=60 install -y \"$PACKAGE\"; then")
          << QStringLiteral("    echo \"WARP install: apt-get could not fetch the package; "
                            "retrying with a direct download\" >&2")
          // The fallback, entered only after the fetch failed. `apt-get download` names the file from
          // the index the update above just wrote, which is why this is not a `curl` of a literal URL:
          // Cloudflare publishes no stable path to the current .deb, and hardcoding one is the same
          // rot the key URL and the suite name are documented against higher up this file. The file
          // lands in a fresh temp directory (mktemp -d is 0700, and this shell is root) rather than the
          // caller's cwd, because `sh -s` inherits whatever directory Muffin happened to be started in.
          // dpkg is then allowed to fail — a package it cannot configure for want of dependencies is
          // not the end of the attempt — because the line after it resolves exactly those dependencies
          // and decides the exit. Its timeouts are looser than the primary's: this pass exists to
          // finish the fetch the first one lost.
          << QStringLiteral("    TMPD=\"$(mktemp -d)\"")
          << QStringLiteral("    cd \"$TMPD\"")
          << QStringLiteral("    apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=120 "
                            "-o Acquire::https::Timeout=120 download \"$PACKAGE\"")
          << QStringLiteral("    dpkg -i \"$PACKAGE\"*.deb || true")
          << QStringLiteral("    DEBIAN_FRONTEND=noninteractive apt-get "
                            "-o Acquire::Retries=3 -o Acquire::http::Timeout=60 "
                            "-o Acquire::https::Timeout=60 install -y -f")
          << QStringLiteral("fi")
          << QString();
    return lines.join(QLatin1Char('\n'));
}

QString WarpInstaller::linuxScriptBody(const QString &codename)
{
    return composeScript(codename, {});
}

QString WarpInstaller::linuxRepairScriptBody(const QString &codename)
{
    // The repair preamble, and the whole of what setRepair() changes on Linux.
    //
    // Two states of debris are worth telling apart from a clean absence, and both would otherwise
    // cost the install: a package unpacked but never configured, which an apt with a half-finished
    // transaction in its database will refuse to touch, and a package removed without --purge, whose
    // leftover config files the new install then reads back — including, quite possibly, the settings
    // that broke the last attempt. `dpkg --configure -a` finishes the first; --purge of an "rc" package
    // clears the second.
    //
    // Both are guarded rather than assumed. dpkg may not exist on a host whose apt got this far
    // through some other route, and `|| true` is deliberate on the two lines that only *improve* the
    // state: a repair that cannot finish somebody else's interrupted transaction should still get to
    // try its own install, which is the difference between a user with a working tunnel and a user
    // reading "apt failed (100)" with no idea that the fix was three directories away.
    static const QStringList preamble{
        QStringLiteral("# A previous attempt is on this machine. Clearing what it left behind, before"),
        QStringLiteral("# anything is asked of apt."),
        QStringLiteral("if command -v dpkg >/dev/null 2>&1; then"),
        QStringLiteral("    dpkg --configure -a || true"),
        QStringLiteral("    if dpkg -l \"$PACKAGE\" 2>/dev/null | grep -q '^rc'; then"),
        QStringLiteral("        echo \"WARP repair: purging the configuration files that the removed "
                       "$PACKAGE left behind\""),
        QStringLiteral("        dpkg --purge \"$PACKAGE\" || true"),
        QStringLiteral("    fi"),
        QStringLiteral("fi"),
        QString(),
    };
    return composeScript(codename, preamble);
}

QString WarpInstaller::parseCodename(const QString &osReleaseText)
{
    // /etc/os-release is KEY=value lines with optional quoting, and nothing here sources it: running an
    // interpreter over the file is the pattern polkit's own documentation warns about, and two field
    // lookups do the same job without one.
    //
    // VERSION_CODENAME first — that is the suite the repository ships. UBUNTU_CODENAME second, for a
    // derivative whose own VERSION_CODENAME names itself instead of the Ubuntu release its packages were
    // built against (a Linux Mint store reads exactly that way).
    QString fallback;
    const QStringList lines = osReleaseText.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const bool isVersion = line.startsWith(QLatin1String("VERSION_CODENAME="));
        const bool isUbuntu = line.startsWith(QLatin1String("UBUNTU_CODENAME="));
        if (!isVersion && !isUbuntu)
            continue;
        QString value = line.section(QLatin1Char('='), 1);
        // Strip one matching pair of quotes when present; anything else is the value as written.
        if (value.size() >= 2
            && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
            value = value.mid(1, value.size() - 2);
        if (!isUsableCodename(value))
            continue;
        if (isVersion)
            return value;
        if (fallback.isEmpty())
            fallback = value;
    }
    return fallback;
}

QString WarpInstaller::hostCodename()
{
    QFile file(QStringLiteral("/etc/os-release"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return parseCodename(QString::fromUtf8(file.readAll()));
}

QString WarpInstaller::repositorySuite(const QString &codename)
{
    // An upstream name passes straight through: "noble", "jammy" and "bookworm" are directories the
    // repository publishes, and rewriting one of them would be the same mistake pointed the other way.
    //
    // A derivative that names itself rather than its parent has no published directory to ask for, and
    // the answer is the upstream suite its packages were built against. The family test is a prefix
    // rather than a list of labels so a snapshot image ("kali-2025.1") lands on the same suite as the
    // rolling release it came from, without this function having to know the next snapshot's name.
    const QString family = QLatin1String(kDerivativeReleaseFamily);
    if (codename == family || codename.startsWith(family + QLatin1Char('-')))
        return QLatin1String(kDerivativeFallbackSuite);
    return codename;
}

QString WarpInstaller::hostRepositorySuite()
{
    // The suite to write for this machine: the release it calls itself, translated by the one mapping
    // that knows which of those names the repository has never published. Empty stays empty — a host
    // that names no release cannot be given a suite by imagination, and the caller refuses it.
    return repositorySuite(hostCodename());
}

bool WarpInstaller::composeLinuxInstall(const QString &pkexecPath, const QString &shellPath,
                                         bool alreadyRoot, QString *program, QStringList *args,
                                         QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!program || !args)
        return fail(QStringLiteral("WarpInstaller: composeLinuxInstall() needs somewhere to write"));
    program->clear();
    args->clear();

    // No shell means nobody to read the script, and that is a host this install cannot happen on rather
    // than a host missing a package. Refusing here says so; letting QProcess try would report a
    // FailedToStart naming a program nobody typed.
    if (shellPath.isEmpty())
        return fail(QStringLiteral("WarpInstaller: this host has no shell to read the install script"));
    if (!alreadyRoot && pkexecPath.isEmpty())
        return fail(QStringLiteral(
                        "WarpInstaller: installing the WARP client needs root, and pkexec is not "
                        "installed. Run Muffin as root, or install the package by hand: "
                        "apt-get install -y %1").arg(packageName()));

    if (alreadyRoot) {
        // The same gate MuffinInstallHelper::composePromote() and LinuxBackend apply: pkexec from a root
        // session is a prompt nobody is there to answer, not an extra permission.
        *program = shellPath;
        *args = { QStringLiteral("-s") };
        return true;
    }
    *program = pkexecPath;
    *args = { shellPath, QStringLiteral("-s") };
    return true;
}

bool WarpInstaller::canAskForRoot(bool alreadyRoot, bool pkexecPresent, bool polkitAgentReachable)
{
    // Already root: nothing to ask. Otherwise both halves are needed — a pkexec with no agent to answer
    // it cannot prompt, and would hang or fail about a display nobody is looking at. LinuxBackend
    // refuses on the same pair, for the same reason.
    return alreadyRoot || (pkexecPresent && polkitAgentReachable);
}

// ---- Windows: the installer, and the window it leaves behind ----

QString WarpInstaller::installerUrl() { return QLatin1String(kInstallerUrl); }
QString WarpInstaller::installerFileName() { return QLatin1String(kInstallerFileName); }

QStringList WarpInstaller::msiexecArgs(const QString &msiPath)
{
    // /i then the file, then /qn for a silent install, then /norestart. Qt composes the command line
    // and quotes msiPath itself (see WindowsArgs on what setArguments does and does not do), so the
    // path arrives as one argument and never as text somebody's shell re-reads. /norestart because an
    // installer that decides to reboot the machine mid-call is not this application's decision to make.
    return { QStringLiteral("/i"), msiPath, QStringLiteral("/qn"), QStringLiteral("/norestart") };
}

bool WarpInstaller::installSucceeded(int exitCode)
{
    return exitCode == kMsiSuccess || exitCode == kMsiRebootRequired;
}

QString WarpInstaller::windowsInstallDir(const QString &programFilesRoot)
{
    // The same three refusals ZapretPaths::systemInstallDirUnder() applies to a Program Files value read
    // out of the environment: this composes where a client was expected to land, and a value that could
    // name anywhere would make the answer meaningless.
    const QString root = QDir::fromNativeSeparators(programFilesRoot).trimmed();
    if (root.isEmpty() || !QDir::isAbsolutePath(root)
        || root.split(QLatin1Char('/'), Qt::SkipEmptyParts).contains(QLatin1String("..")))
        return QString();
    return root + QLatin1String(kInstallDirSuffix);
}

QStringList WarpInstaller::guiImages()
{
    // Two names, one program: the tray application has been Warp.exe and Cloudflare WARP.exe depending
    // on the build, and the current documentation names the longer one. Both are closed because the
    // promise "Muffin owns this window" cannot be allowed to depend on which version somebody
    // installed. The service — warp-svc.exe — is deliberately absent, and holds the tunnel.
    return { QLatin1String(kGuiImageLegacy), QLatin1String(kGuiImageCurrent) };
}

QStringList WarpInstaller::taskkillArgs(const QString &image)
{
    // /F to force it, /IM to name the process by image rather than by pid — the only handle available
    // on a GUI a different account started, which is the situation after a machine-wide MSI installs a
    // per-user tray application.
    return { QStringLiteral("/F"), QStringLiteral("/IM"), image };
}

bool WarpInstaller::platformHasWarpGui(Strategy::Platform platform)
{
    // Only the Windows package ships a window. Linux gets warp-svc and warp-cli and nothing to look at,
    // so a policy about that GUI is a policy nothing can keep — and a promise the settings page must not
    // make on a host where it means nothing. macOS is not a platform this application builds for.
    return platform == Strategy::Platform::Windows;
}

bool WarpInstaller::closesWarpGui(bool hideWarpGuiSetting, Strategy::Platform platform,
                                  bool installed)
{
    // Three conditions, each ruling out a host where the guard would do nothing useful: the user has to
    // have asked for it, the platform has to have a GUI to close, and a client has to exist at all — a
    // machine with no WARP has no window, and poking at the process table for one would be a startup
    // that costs a fork to discover something the toggle already says.
    return hideWarpGuiSetting && platformHasWarpGui(platform) && installed;
}

QStringList WarpInstaller::closeWindowsGui()
{
    QStringList killed;
#if defined(Q_OS_WIN)
    // taskkill by image name, one call per name. A blocking execute is right here and nowhere else in
    // this class: this is a sub-second lookup in the process table with no network and no package
    // manager in it, and the alternative — an async child per name — is two objects to own for a
    // command whose only interesting answer is "did it name a process?".
    for (const QString &image : guiImages()) {
        if (QProcess::execute(QStringLiteral("taskkill"), taskkillArgs(image)) == 0)
            killed << image;
        // Non-zero is taskkill saying "no such process", which is the normal case: a service running
        // with nobody looking at a tray icon is exactly the state this is meant to leave behind.
    }
#endif
    return killed;
}

bool WarpInstaller::installFoundFrom(const QString &resolvedProgram)
{
    return !resolvedProgram.isEmpty();
}

bool WarpInstaller::isNetworkFailure(const QString &output)
{
    // One lower-cased copy of the transcript, then the markers. apt prints the transport's own error
    // text with a capital that changes between versions ("Cannot initiate the connection" in one,
    // "Could not connect" in another, curl adds its own "(28 Timeout)" prefix), so case-sensitive
    // matching would miss whichever spelling this host's tool happened to choose and leave the user
    // without the one hint that could have helped them.
    const QString haystack = output.toLower();
    for (const char *const marker : kNetworkFailureMarkers) {
        if (haystack.contains(QLatin1String(marker)))
            return true;
    }
    return false;
}

// ---- the operation ----
//
// Everything above answers a question; everything below runs one. The shape is the same on both
// platforms and deliberately so: a stage change, one external command driven to completion, one check
// afterwards, and exactly one installationFinished(). Nothing here waits on a child by blocking,
// because the window reporting this progress has to keep painting while apt is still running.

bool WarpInstaller::start(QString *error)
{
    // Only the refusals a caller has to hear before it opens a progress window are synchronous: an
    // install already running (one operation at a time, like every other object in this core), a
    // platform with no client to install, and — on Linux — a host that cannot name an apt suite or
    // cannot be asked for root. Everything found after this point arrives through
    // installationFinished(), because by then there is a window that owns the answer.
    if (m_stage != Stage::Idle)
    {
        if (error)
            *error = QStringLiteral("WarpInstaller: an install is already running");
        return false;
    }
    if (m_platform != Strategy::Platform::Windows && m_platform != Strategy::Platform::Linux)
    {
        if (error)
            *error = QStringLiteral("WarpInstaller: this platform has no WARP client to install");
        return false;
    }
    if (m_platform == Strategy::Platform::Linux)
    {
        // Read before asking. A machine whose release this application cannot turn into a published apt
        // suite would otherwise pay for a password prompt only to be told, after the password, that there
        // was no suite to write.
        if (!isUsableCodename(hostRepositorySuite()))
        {
            if (error)
                *error = QStringLiteral("WarpInstaller: this host does not name an apt suite, so no "
                                        "repository entry can be written");
            return false;
        }
        if (!canAskForRoot(PrivilegeHelper::isElevated(),
                           !QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty(),
                           PrivilegeHelper::hasPolkitAgent()))
        {
            if (error)
                *error = QStringLiteral("WarpInstaller: installing the WARP client needs root, and "
                                        "there is nobody to ask. Run Muffin from a root session, or "
                                        "install %1 by hand.").arg(packageName());
            return false;
        }
    }

    setStage(Stage::Preparing);

    // Started a loop turn later rather than here: the caller has to get its stack back so it can show
    // the window that reports this, and a failure found inside beginLinux() would otherwise be emitted
    // before anybody had a progress dialog to receive it.
    QTimer::singleShot(0, this, [this]() {
        if (m_stage != Stage::Preparing)
            return;   // destroyed or refused between the call and this turn
        if (m_platform == Strategy::Platform::Windows)
            beginWindows();
        else
            beginLinux();
    });
    return true;
}

void WarpInstaller::beginLinux()
{
    // The host facts are read again here rather than carried over from start(): they are the same
    // answers, taken a loop turn later, and the second reading is the one the command is built from.
    // A pkexec that vanished in between is a fact about this machine that only the second reading can
    // prove, and composing from a stale one would exec a program the host no longer has.
    const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    // /bin/bash where it exists and /bin/sh where it does not: the generated script is POSIX, and a
    // stripped container image has the latter and not the former.
    const QString shellPath = QFileInfo::exists(QStringLiteral("/bin/bash"))
                                  ? QStringLiteral("/bin/bash")
                                  : QStringLiteral("/bin/sh");

    QString program;
    QStringList args;
    QString why;
    if (!composeLinuxInstall(pkexecPath, shellPath, PrivilegeHelper::isElevated(),
                             &program, &args, &why))
    {
        finishWith(false, why);
        return;
    }

    // The suite the repository is asked for, not the label the host chose for itself: repositorySuite()
    // maps a rolling derivative that publishes no repository of its own (Kali's "kali-rolling") onto the
    // upstream Debian suite its packages are built against. Writing the host's own name was the 404 that
    // made apt answer exit 100 here.
    const QString codename = hostRepositorySuite();
    if (!isUsableCodename(codename))
    {
        finishWith(false, QStringLiteral("WarpInstaller: this host stopped naming an apt suite before "
                                         "the install script could be written"));
        return;
    }

    setStage(Stage::Installing);
    emit installationProgress(QStringLiteral("Installing the Cloudflare WARP package. This asks for a "
                                             "password and runs apt, and cannot be cancelled once it "
                                             "starts."));
    // The script is the command's stdin, never a file on the authorised path — see composeLinuxInstall.
    // Which of the two bodies is chosen is the caller's decision, taken from what WarpManager saw on
    // this machine before asking for anything: a repair preamble for a machine with debris, plain
    // install for a machine with nothing.
    runCommand(program, args, QStringLiteral("apt-get"),
               (m_repair ? linuxRepairScriptBody(codename) : linuxScriptBody(codename)).toUtf8());
}

void WarpInstaller::beginWindows()
{
    // Staged in the temp directory, which is one this user always owns and which Windows empties on its
    // own schedule. Unlike ZapretDownloader there is no rename-into-place step here — msiexec copies the
    // package's payload where the package says it goes — so the staging directory does not have to share
    // a filesystem with the destination, and a throwaway path is the safer choice: the only thing that
    // has to be true of it is that msiexec can read it, and afterwards it is deleted.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty())
    {
        finishWith(false, QStringLiteral("WarpInstaller: this host has no temporary directory to stage "
                                         "the installer in"));
        return;
    }
    m_stagingDir = base + QLatin1String("/muffin-warp-")
                   + QString::number(QCoreApplication::applicationPid());
    if (!QDir().mkpath(m_stagingDir))
    {
        const QString failed = m_stagingDir;
        m_stagingDir.clear();
        finishWith(false, QStringLiteral("WarpInstaller: cannot create %1 to stage the installer in")
                              .arg(failed));
        return;
    }
    m_stagedInstaller = m_stagingDir + QLatin1Char('/') + installerFileName();

    setStage(Stage::Downloading);
    emit installationProgress(QStringLiteral("Downloading the Cloudflare WARP installer."));
    downloadInstaller();
}

void WarpInstaller::downloadInstaller()
{
    if (m_stagedInstaller.isEmpty())
    {
        finishWith(false, QStringLiteral("WarpInstaller: there is nowhere to write the installer"));
        return;
    }

    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    QNetworkRequest req{QUrl(installerUrl())};
    req.setTransferTimeout(kTransferTimeoutMs);
    req.setRawHeader("User-Agent", kUserAgent);
    // The documented link answers 302 to the current build's object; follow it, never downgrade it.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    auto *reply = m_network->get(req);
    m_reply = reply;

    // The file is owned by the reply: it exists only to receive these bytes, and closing it with the
    // transfer is one less thing the several ways this download can end each have to remember to do.
    auto *file = new QFile(m_stagedInstaller, reply);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        const QString err = file->errorString();
        m_reply = nullptr;
        reply->deleteLater();
        finishWith(false, QStringLiteral("WarpInstaller: cannot write %1 (%2)")
                              .arg(m_stagedInstaller, err));
        return;
    }

    // A write failure is recorded, not emitted from inside readyRead — exactly as ZapretDownloader does
    // it. Aborting the reply from its own readyRead handler would emit finished() re-entrantly and run
    // the state machine twice for one transfer.
    auto writeError = QSharedPointer<QString>::create();

    connect(reply, &QNetworkReply::readyRead, this, [reply, file, writeError]() {
        if (!writeError->isEmpty())
            return;   // a disk that refused once will refuse again; say it once
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        if (file->write(chunk) != chunk.size())
            *writeError = file->errorString();
    });

    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 done, qint64 total) {
        if (reply != m_reply)
            return;
        // The dialog shows a busy bar for this whole phase, so the only thing worth saying is roughly how
        // far the transfer has got — and only when the server said how large the file is.
        if (total > 0)
        {
            emit installationProgress(QStringLiteral("Downloading the Cloudflare WARP installer (%1% of "
                                                     "%2 MB).")
                                          .arg(done * 100 / total)
                                          .arg(total / (1024 * 1024)));
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, file, writeError]() {
        if (reply != m_reply)
        {
            reply->deleteLater();   // an abort() nothing here asked for; another path already reported
            return;
        }
        m_reply = nullptr;
        file->close();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorText = reply->errorString();
        reply->deleteLater();

        if (m_stage != Stage::Downloading)
            return;   // a deadline or a teardown ended this while the reply was in flight
        if (!writeError->isEmpty())
        {
            finishWith(false, QStringLiteral("WarpInstaller: cannot write the installer (%1)")
                                  .arg(*writeError));
            return;
        }
        if (error != QNetworkReply::NoError)
        {
            finishWith(false, QStringLiteral("WarpInstaller: the installer download failed (%1)")
                                  .arg(errorText));
            return;
        }

        // Size alone is the check. Cloudflare publishes no digest for this URL, so there is no expected
        // value to compare these bytes with; what stands between them and msiexec is the transport — an
        // https answer, on a policy that refuses a downgrade to anything less. A zero-length file is the
        // failure that actually happens: a redirect to an error page that saved nothing.
        const QFileInfo staged(m_stagedInstaller);
        if (!staged.isFile() || staged.size() == 0)
        {
            finishWith(false, QStringLiteral("WarpInstaller: the download produced no installer file"));
            return;
        }

        emit installationProgress(QStringLiteral("Running the Windows Installer package."));
        runMsiexec(m_stagedInstaller);
    });

    // The stall guard is the request's own, not a timer: setTransferTimeout resets on every byte, so a
    // slow link that is moving keeps going and a link that stopped moving is cut. This backstop fires
    // only if that deadline is never delivered at all, and it aborts rather than pretending: an open
    // reply holding a staged file is a directory that cannot be cleaned up.
    QTimer::singleShot(kTransferTimeoutMs + 5000, this, [this, reply]() {
        if (m_reply == reply)
            reply->abort();
    });
}

void WarpInstaller::runMsiexec(const QString &msiPath)
{
    // No pkexec-shaped refusal is possible on this path: packaging/windows/app.manifest asked Windows
    // for the elevated token before main() ran, so this process is already the one allowed to install,
    // and msiexec inherits that token rather than being asked for a second one.
    setStage(Stage::Installing);
    // The absolute path where the OS keeps it, not the bare name: a PATH lookup for "msiexec" can be
    // answered by anything the user's PATH points at, and this is the one command here that has to be
    // Windows Installer rather than something named like it. SystemRoot is the environment's own answer
    // for where Windows lives, and the file either is there or is not.
    QString program = QStringLiteral("msiexec.exe");
    const QString systemRoot = QProcessEnvironment::systemEnvironment()
                                   .value(QStringLiteral("SystemRoot"));
    if (!systemRoot.isEmpty())
    {
        const QString absolute = QDir(QDir::fromNativeSeparators(systemRoot))
                                     .filePath(QStringLiteral("System32/msiexec.exe"));
        if (QFileInfo::exists(absolute))
            program = absolute;
    }
    runCommand(program, msiexecArgs(msiPath), QStringLiteral("msiexec"));
}

void WarpInstaller::runCommand(const QString &program, const QStringList &args, const QString &what,
                               const QByteArray &script)
{
    m_output.clear();
    m_deadlineHit = false;

    auto *proc = new QProcess(this);
    m_process = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(program);
    proc->setArguments(args);

    connect(proc, &QProcess::readyRead, this, &WarpInstaller::relayOutput);

    connect(proc, &QProcess::finished, this, [this, proc, what](int code, QProcess::ExitStatus status) {
        if (m_process == proc)
            m_process = nullptr;
        relayOutput();      // whatever the last chunk left unread before the exit
        proc->deleteLater();

        if (m_stage != Stage::Installing)
            return;         // a deadline or a teardown already ended this run

        if (m_deadlineHit)
        {
            finishWith(false, QStringLiteral("WarpInstaller: %1 did not answer within %2 minutes and "
                                             "was stopped.%3")
                                  .arg(what, QString::number(kInstallTimeoutMs / 60000), outputTail()));
            return;
        }
        if (status != QProcess::NormalExit)
        {
            finishWith(false, QStringLiteral("WarpInstaller: %1 was killed instead of finishing.%2")
                                  .arg(what, outputTail()));
            return;
        }

        // Two success tests, because the two tools disagree about what a good exit means: apt answers 0
        // or nothing, and msiexec answers 3010 for a client that is installed and waiting for a reboot.
        const bool ok = (m_platform == Strategy::Platform::Windows)
                            ? installSucceeded(code)
                            : (code == 0);
        if (!ok)
        {
            QString message = QStringLiteral("WarpInstaller: %1 failed (exit code %2).%3")
                                  .arg(what, QString::number(code), outputTail());
            if (code == 126 || code == 127)
                message += QStringLiteral(" A 126 is what pkexec answers when the password was refused "
                                          "or the prompt was dismissed.");
            // The one failure with an action attached to it. A fetch that timed out is not apt refusing
            // the package and not a repository that lost the suite — it is this machine's route to
            // Cloudflare, and the application owns a tool that changes that route. Naming it here means
            // the sentence travels with the error into the journal and any caller, not only the window
            // that happens to be able to translate it; the UI says the same thing in the user's own
            // language, and this is what a log read over someone's shoulder still explains.
            if (isNetworkFailure(m_output))
                message += QStringLiteral(" The package could not be downloaded: the connection failed "
                                          "or timed out. If it keeps happening, start the Zapret bypass "
                                          "first and run the install again.");
            finishWith(false, message);
            return;
        }

        if (m_platform == Strategy::Platform::Windows)
            closeGuiAndVerify();
        else
            verifyAndFinish();
    });

    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        // FailedToStart is the only failure that never reaches finished(): here that means pkexec, the
        // shell or msiexec could not be execed at all, which is a fact about the host rather than about
        // the install — and Qt's own text says which.
        if (e != QProcess::FailedToStart)
            return;
        if (m_process == proc)
            m_process = nullptr;
        const QString text = proc->errorString();
        proc->deleteLater();
        if (m_stage == Stage::Installing)
            finishWith(false, QStringLiteral("WarpInstaller: the install command could not be started "
                                             "(%1)").arg(text));
    });

    // Killed rather than waited on, and reported as a timeout by the exit that follows: see
    // kInstallTimeoutMs for what this bounds and why the bound is generous.
    QTimer::singleShot(kInstallTimeoutMs, this, [this, proc]() {
        if (m_process != proc)
            return;
        m_deadlineHit = true;
        proc->kill();
    });

    proc->start();

    if (!script.isEmpty())
    {
        // The generated bash reaches the shell down the pipe and the pipe is then closed, which is what
        // makes `sh -s` read it, run it, and exit. Nothing was written to disk to be swapped.
        proc->write(script);
        proc->closeWriteChannel();
    }
}

void WarpInstaller::closeGuiAndVerify()
{
    setStage(Stage::ClosingGui);
    // The policy is asked, not assumed: a user who turned the single-window switch off wants the tray
    // application, and an install that reported success is the "installed" the decision needs — the
    // verification below is what proves it, and it would be backwards to close a window only after
    // proving the client exists, since the window is the thing in front of the user right now.
    if (closesWarpGui(m_hideWarpGui, m_platform, /*installed=*/true))
    {
        const QStringList killed = closeWindowsGui();
        if (!killed.isEmpty())
        {
            emit installationProgress(QStringLiteral("Closed the WARP window (%1); this application "
                                                     "drives the tunnel from here.")
                                          .arg(killed.join(QStringLiteral(", "))));
        }
    }
    verifyAndFinish();
}

void WarpInstaller::verifyAndFinish()
{
    setStage(Stage::Verifying);

    // Resolved the way WarpManager resolves it — PATH first, then the vendor's own directory — because
    // the answer has to be the same question the toggle asks a moment later. A check with a narrower
    // probe would report an install this application can drive as a failure: on Windows the MSI writes
    // warp-cli.exe into Program Files and never touches PATH, so a PATH-only probe calls a machine with
    // a working client empty.
    const QString resolved = WarpManager::resolveExecutable(
        QStandardPaths::findExecutable(WarpManager::warpCliProgram()),
        WarpManager::installCandidatePaths(WarpManager::programFilesRoot()));

    if (installFoundFrom(resolved))
    {
        finishWith(true, QString());
        return;
    }

    // The one failure that reports a successful installer as a failure, and the reason the check exists
    // at all: apt can finish while warp-svc never starts, and msiexec can answer 0 with the binary
    // written somewhere this process cannot see. Either way the interface must not offer the tunnel.
    finishWith(false, QStringLiteral("WarpInstaller: the installer reported success, but no warp-cli "
                                     "could be found afterwards, so this application still cannot drive "
                                     "the tunnel.%1").arg(outputTail()));
}

void WarpInstaller::finishWith(bool success, const QString &error)
{
    cleanupStaging();
    setStage(Stage::Idle);
    emit installationFinished(success, success ? QString() : error);
}

void WarpInstaller::relayOutput()
{
    if (!m_process)
        return;
    const QByteArray chunk = m_process->readAll();
    if (chunk.isEmpty())
        return;
    m_output += QString::fromLocal8Bit(chunk);

    // The newest complete line, which on a package manager is a real progress report ("Unpacking
    // cloudflare-warp (2024.6.3)...") and on a failure is the beginning of the reason. A line with no
    // newline after it yet is held back: half a sentence in a status bar reads as a glitch, and the rest
    // of it arrives in the next chunk or in the flush after the exit.
    const qsizetype last = m_output.lastIndexOf(QLatin1Char('\n'));
    if (last <= 0)
        return;
    const qsizetype prev = m_output.lastIndexOf(QLatin1Char('\n'), last - 1);
    const QString line = m_output.mid(prev + 1, last - prev - 1).trimmed();
    if (!line.isEmpty())
        emit installationProgress(line);
}

QString WarpInstaller::outputTail() const
{
    // apt and msiexec both put the explanation at the end, and both are chatty enough that the whole
    // transcript is noise in a status line. Empty when the command said nothing at all, so a failure
    // with no output does not end in a dangling colon.
    const QString trimmed = m_output.trimmed();
    if (trimmed.isEmpty())
        return QString();
    return QStringLiteral("\n%1").arg(trimmed.right(kOutputTailChars));
}

void WarpInstaller::cleanupStaging()
{
    if (m_stagingDir.isEmpty())
    {
        m_stagedInstaller.clear();
        return;
    }
    QFile::remove(m_stagedInstaller);
    QDir(m_stagingDir).removeRecursively();
    m_stagingDir.clear();
    m_stagedInstaller.clear();
}

void WarpInstaller::setStage(Stage stage)
{
    m_stage = stage;
}

} // namespace zapret


