#include "PrivilegeHelper.h"
#include "WarpManager.h"

#include "ZapretSource.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

namespace zapret {

namespace {

// warp-cli's own timeouts are usually under a second, but a first "connect" has to talk to the
// daemon and register if the client was never enrolled — that round trip can take a while. 30 s
// is long enough for a slow first run and short enough that a wedged daemon shows up in the log
// rather than hanging the UI.
constexpr int kVerbTimeoutMs = 30000;

// The version question gets its own, much shorter, deadline. It asks nothing of the daemon and needs
// no network, so a client that has not answered in a couple of seconds is not slow, it is stuck — and
// the answer it is holding up decides whether a user gets an offer to install software.
constexpr int kVersionTimeoutMs = 3000;
// …and the same reasoning applies per package database, of which a round asks at most four.
constexpr int kProbeTimeoutMs = 2000;

// ---- the preparation's own clocks ----
//
// The protocol verb is a warp-cli talking to the daemon over a local socket: normally instant, and
// given the same budget as any other verb so a daemon that will not answer is caught rather than
// waited on.
constexpr int kProtocolTimeoutMs = kVerbTimeoutMs;
// The IPv6 read is a sysctl of one value. Two seconds is generous, and a host with no sysctl answers
// by not answering at all, which the step treats as "unknown" rather than as a finding.
constexpr int kIpv6TimeoutMs = 2000;
// The service restart is the one step a human may have to authorise, so its deadline has to be a
// polite one: pkexec sits waiting for a polkit agent while nobody is looking at the keyboard, and a
// prompt killed at thirty seconds reads as a program that has broken. A minute is long enough to
// type a password into and short enough that an unattended prompt still ends.
constexpr int kServiceTimeoutMs = 60000;
// How long the daemon is given to come back after a restart before the tunnel is asked for it.
// systemd reports the unit active before warp-svc has re-opened its control socket, and a connect
// issued in that window is answered by nothing at all — the same silent failure this whole sequence
// exists to remove, reproduced by our own fix. Timed, never slept: this fires on the event loop.
constexpr int kServiceSettleMs = 1500;
// How long a connect is allowed to stay "Connecting" before the journal says what that usually
// means. The daemon's own handshake on a working host is a couple of seconds; the case worth
// naming is the one where it never finishes because the provider is interfering with the transport.
constexpr int kConnectDeadlineMs = 15000;

// ---- the window suppression's own clocks ----
//
// Two of the three commands are a conversation with the caller's own systemd manager and normally
// answer in tens of milliseconds; the third is a process-table lookup. The deadline exists for the
// case none of them can produce on a healthy host — a session manager that has stopped answering —
// and it is generous enough that one wedged command costs a late line in the journal instead of a
// window nobody ever closed.
constexpr int kGuiCommandTimeoutMs = 10000;
// How much of a refused command is worth quoting. systemctl's own complaint is a sentence, and the
// journal already carries the rest of the run.
constexpr int kGuiDetailChars = 200;
// How much of a desktop entry is read before deciding what it is. An `Exec=` line belongs near the
// top of a file that never runs to a megabyte, and the whole point of reading it is to refuse to
// decide from a file name alone.
constexpr int kAutostartReadChars = 4096;

} // namespace

QString WarpManager::warpCliProgram()
{
    return QStringLiteral("warp-cli");
}

bool WarpManager::isAvailableFromPath(const QString &resolvedPath)
{
    return !resolvedPath.isEmpty();
}

QStringList WarpManager::versionArgs()
{
    return {QStringLiteral("--version")};
}

WarpManager::Probe WarpManager::classifyVersionAnswer(const QString &output, int exitCode,
                                                      bool crashed)
{
    // It died rather than exited. Our own timeout kill lands here as well, and the distinction does
    // not matter to the verdict: a client that cannot print a version line within the deadline is not
    // one a GUI can drive, because every verb it will be asked for needs the same process to come up.
    if (crashed)
        return Probe::Broken;

    // A clean exit is the answer we were hoping for. The text is not inspected: a version string's
    // exact shape has changed between releases, and "it ran and was satisfied" is the fact that
    // matters. That is also what keeps this probe from becoming a second version parser to maintain.
    if (exitCode == 0)
        return Probe::Usable;

    const QString t = output.toLower();

    // Damage has a small, stable vocabulary across platforms: a loader that cannot find one of the
    // package's own libraries, an executable the kernel refuses, a Windows side-by-side manifest that
    // does not resolve. Every one of these comes with a non-zero exit *and* names the failure, which
    // is the combination that licenses calling the install broken.
    static const char *const kDamageMarkers[] = {
        "error while loading shared libraries",
        "cannot open shared object file",
        "no such file or directory",
        "cannot execute binary file",
        "not a valid win32 application",
        "requested the runtime to terminate",
        "side-by-side configuration",
        "the specified module could not be found",
    };
    for (const char *marker : kDamageMarkers) {
        if (t.contains(QString::fromLatin1(marker)))
            return Probe::Broken;
    }

    // The other side of the same coin: a complaint about how it was asked proves a binary that runs.
    // Whether `--version` is spelled right has moved between builds; a working client must not be
    // taken away from a user because of it.
    static const char *const kRefusalMarkers[] = {
        "unrecognized option", "unknown option", "invalid option", "unimplemented option",
        "usage:",              "try '",                "bad option",
    };
    for (const char *marker : kRefusalMarkers) {
        if (t.contains(QString::fromLatin1(marker)))
            return Probe::Unreadable;
    }

    // Non-zero, and nothing the answer says names either verdict. Silence is the interesting case: a
    // binary that exits 3 and prints nothing at all *is* not answering, but so is one that has
    // decided --version is not for it. Guess damage here and a healthy tunnel disappears; guess
    // health and nothing changes from before this probe existed. Leave the verdict as it was found.
    return Probe::Unreadable;
}

QList<WarpManager::InstallationProbe> WarpManager::installationProbes(Strategy::Platform platform)
{
    // The databases are asked cheapest-first and in the order the host is most likely to own the first
    // one: the system package manager, then the two self-contained distributors, and the round stops
    // at the first yes. Markers are what a line has to contain to *be* the client — the package name
    // for the archives that print one, and the service's own name for the Windows queries.
    const QString package = QStringLiteral("cloudflare-warp");
    switch (platform) {
    case Strategy::Platform::Linux:
        return {
            {QStringLiteral("dpkg"), QStringLiteral("dpkg"),
             {QStringLiteral("-l"), package}, {package}},
            {QStringLiteral("rpm"), QStringLiteral("rpm"),
             {QStringLiteral("-q"), package}, {package}},
            {QStringLiteral("snap"), QStringLiteral("snap"),
             {QStringLiteral("list"), package}, {package}},
            {QStringLiteral("flatpak"), QStringLiteral("flatpak"),
             {QStringLiteral("list"), QStringLiteral("--app"),
              QStringLiteral("--columns=id,name")},
             {QStringLiteral("com.cloudflare.Warp"), QStringLiteral("cloudflare warp")}},
        };
    case Strategy::Platform::Windows:
        return {
            {QStringLiteral("service"), QStringLiteral("sc"),
             {QStringLiteral("query"), QStringLiteral("warp-svc")}, {QStringLiteral("warp-svc")}},
            {QStringLiteral("service"), QStringLiteral("sc"),
             {QStringLiteral("query"), QStringLiteral("Cloudflare WARP")},
             {QStringLiteral("Cloudflare WARP")}},
        };
    case Strategy::Platform::Unknown:
        break;
    }
    // A platform this file cannot name owns no database this function would dare to run. The round
    // ends immediately and answers "nothing found", which is the safe half of the design: an unknown
    // host is never told it has a client, and the offer it then gets is the one a human approves.
    return {};
}

bool WarpManager::probeAnswerNamesInstall(const InstallationProbe &probe, const QString &output,
                                          int exitCode, bool crashed)
{
    // A tool that died has no answer in it. This is not the same case as "the host has no such tool":
    // that one never reaches here, because a probe whose program does not resolve is skipped.
    if (crashed)
        return false;

    const QStringList lines = output.split(QLatin1Char('\n'));

    if (probe.source == QLatin1String("dpkg")) {
        // dpkg's first status character is what the user asked for, the second what is on the machine,
        // and only 'i' in that second column means installed. Matching the whole line instead would
        // read the error sentence "no packages found matching cloudflare-warp" as a yes — that line
        // names the package too, and it is precisely the line an absent machine prints.
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.startsWith(QLatin1String("ii")) && !trimmed.startsWith(QLatin1String("iI")))
                continue;
            for (const QString &marker : probe.markers) {
                if (trimmed.contains(marker, Qt::CaseInsensitive))
                    return true;
            }
        }
        return false;
    }

    if (probe.source == QLatin1String("service")) {
        // `sc` answers a service it cannot open with "[SC] OpenQuery failed 1060 …" and that number as
        // its exit code, so the code alone settles the common case. Requiring a state line as well is
        // what stops an answer that names the service only inside an error sentence from counting.
        if (exitCode != 0)
            return false;
        bool namesState = false;
        for (const QString &line : lines) {
            if (line.contains(QLatin1String("SERVICE_NAME:"), Qt::CaseInsensitive)
                || line.contains(QLatin1String("STATE:"), Qt::CaseInsensitive))
                namesState = true;
        }
        if (!namesState)
            return false;
        for (const QString &marker : probe.markers) {
            if (output.contains(marker, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }

    // The generic rule, which is all flatpak needs and enough for rpm and snap: these answer a package
    // they do not have with a sentence naming it *and* a non-zero exit, and with a table naming it and
    // exit zero when it is there. Requiring both halves is what keeps the error sentence out.
    if (exitCode != 0)
        return false;

    // …and the exit code is not trusted on its own either. Every one of these tools has been seen
    // print its absence sentence and exit zero anyway — a wrapper, a plugin, a `|| true` left in
    // somebody's script — and an absence stated out loud is a fact worth more than a code.
    static const char *const kAbsenceMarkers[] = {
        "is not installed", "not installed", "no packages found", "could not find",
        "no such package",  "no matches found", "error:", "usage:",
    };
    const QString lowered = output.toLower();
    for (const char *marker : kAbsenceMarkers) {
        if (lowered.contains(QString::fromLatin1(marker)))
            return false;
    }

    for (const QString &line : lines) {
        for (const QString &marker : probe.markers) {
            if (line.contains(marker, Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

bool WarpManager::probeAnswerNamesLeftovers(const InstallationProbe &probe, const QString &output,
                                             int exitCode, bool crashed)
{
    if (crashed || exitCode != 0)
        return false;

    // Only dpkg distinguishes "not here" from "the binaries are gone and the configuration is not".
    // "rc" is a package removed without --purge: exactly a previous attempt's debris, and the state
    // where reinstalling without cleaning first inherits the config file that may have broken it.
    // No other database here reports the difference, so no other database answers this question.
    if (probe.source != QLatin1String("dpkg"))
        return false;
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1String("rc")))
            continue;
        for (const QString &marker : probe.markers) {
            if (trimmed.contains(marker, Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}

QStringList WarpManager::registryKeys()
{
    // The vendor's own key, then the 32-bit mirror Windows keeps for a 32-bit installer. The uninstall
    // tree is where a version string reliably lives, but its subkeys are named by product GUID rather
    // than by product and QSettings cannot enumerate them, so it is not addressed here — the service
    // probe is what catches a client registered only under a GUID.
    return {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Cloudflare\\Cloudflare WARP"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Cloudflare\\Cloudflare WARP"),
    };
}

bool WarpManager::registryNamesInstall(const QString &displayVersion, bool keyHasValues)
{
    // A version string is the clean yes. Values without one still count: an MSI that wrote anything
    // under the vendor's key has already copied the client onto the disk, and an uninstall that had
    // got all the way through would have taken the key with it.
    return !displayVersion.trimmed().isEmpty() || keyHasValues;
}

bool WarpManager::registryReportsInstall()
{
#if defined(Q_OS_WIN)
    for (const QString &key : registryKeys()) {
        // NativeFormat on Windows *is* the registry, and the key path is the registry's own spelling.
        // Nothing here writes, and the hive read is the one this process was given — the same reason
        // programFilesRoot() reads this process's own environment.
        const QSettings settings(key, QSettings::NativeFormat);
        if (settings.status() != QSettings::NoError)
            continue;
        const QString version = settings.value(QStringLiteral("DisplayVersion")).toString();
        if (registryNamesInstall(version, !settings.allKeys().isEmpty()))
            return true;
    }
    return false;
#else
    // No registry, no key, no claim. The Linux half of the round is the probe list above.
    return false;
#endif
}

QStringList WarpManager::extraCandidatePaths(const QString &homePath)
{
    // Locations the vendor's own installer never writes, so installCandidatePaths() has no business
    // naming them and WarpInstaller's tests no business pinning them. Each is one way a machine can
    // have a client that PATH has never been told about, which is the failure this whole lookup
    // exists to close.
    QStringList paths{
        // The Linux package's own directory. It is on PATH for a login shell, and this is the list for
        // the processes that are not one: an app started by a session manager, a flatpak or an AppImage
        // with a minimal environment, and — the case that actually bit — a process whose PATH was read
        // before the package that put warp-cli there was installed. Without this entry a freshly
        // installed client stays invisible until the application is restarted.
        QStringLiteral("/usr/bin/warp-cli"),
        // A hand-unpacked tarball, or an install done by a different tool than this one.
        QStringLiteral("/usr/local/bin/warp-cli"),
        // snap keeps its commands under /snap/bin — on PATH for a login shell through
        // /etc/profile.d, and on nobody's PATH inside a service manager's environment.
        QStringLiteral("/snap/bin/warp-cli"),
        // flatpak exports per-installation, and the user's own export directory is not on PATH at all.
        QStringLiteral("/var/lib/flatpak/exports/bin/com.cloudflare.Warp"),
    };
    const QString home = QDir::fromNativeSeparators(homePath).trimmed();
    if (!home.isEmpty()) {
        paths << home + QStringLiteral("/.local/share/flatpak/exports/bin/com.cloudflare.Warp");
        // A per-user Windows install, for the host where WARP was installed for one account only.
        paths << home + QStringLiteral("/AppData/Local/Cloudflare/Cloudflare WARP/warp-cli.exe");
    }
    return paths;
}

QStringList WarpManager::statusArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("status")};
}

QStringList WarpManager::connectArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("connect")};
}

QStringList WarpManager::disconnectArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("disconnect")};
}

QStringList WarpManager::registrationShowArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("registration"),
            QStringLiteral("show")};
}

QStringList WarpManager::registrationNewArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("registration"),
            QStringLiteral("new")};
}

QStringList WarpManager::registrationDeleteArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("registration"),
            QStringLiteral("delete")};
}

QByteArray WarpManager::termsAnswer()
{
    return QByteArrayLiteral("y\n");
}

// ---- the vendor's own window ----

QString WarpManager::warpGuiProcessName()
{
    // The image the Linux package's own unit execs (/bin/warp-taskbar, a symlink into
    // /usr/lib/warp/warp-taskbar). Not "cloudflare", not "warp-gui": the first matches nothing and the
    // second has never existed, and both are the sort of name invented from the window's *title*
    // ("Cloudflare One Client") rather than from its process table.
    return QStringLiteral("warp-taskbar");
}

QString WarpManager::warpGuiUserUnit()
{
    // The unit file the package installs under /usr/lib/systemd/user, wanted by
    // graphical-session.target. Spelled with its suffix because that is how systemctl names it, and a
    // verb aimed at "warp-taskbar" alone would be answered about something else.
    return QStringLiteral("warp-taskbar.service");
}

QString WarpManager::warpGuiBusName()
{
    // The bus name the unit is declared against (Type=dbus / BusName=) and the one its activation file
    // publishes. Named here because it is the mechanism: a client that asks this name for nothing still
    // gets the unit started, which is why a stop alone does not hold.
    return QStringLiteral("com.cloudflare.WarpTaskbar");
}

QStringList WarpManager::systemctlUserArgs(const QStringList &verbs)
{
    QStringList args{ QStringLiteral("--user") };
    args += verbs;
    return args;
}

QStringList WarpManager::userUnitDisableArgs()
{
    // Untangling the unit from graphical-session.target, so the window does not come back at the next
    // login for a mask that has since been undone — by an update, or by the user.
    return systemctlUserArgs({ QStringLiteral("disable"), warpGuiUserUnit() });
}

QStringList WarpManager::userUnitMaskArgs()
{
    // `mask --now`, both halves needed and measured rather than assumed: `mask` alone redirects the unit
    // to /dev/null but leaves a running instance running, and `stop` alone is a job the next activation
    // request can cancel. Together they take the window down and close the door behind it.
    return systemctlUserArgs({ QStringLiteral("mask"), QStringLiteral("--now"), warpGuiUserUnit() });
}

QStringList WarpManager::userUnitUnmaskArgs()
{
    // The door opened again, and nothing more: unmasking does not start the unit, which is what lets the
    // enable below decide whether it should be running.
    return systemctlUserArgs({ QStringLiteral("unmask"), warpGuiUserUnit() });
}

QStringList WarpManager::userUnitEnableArgs()
{
    // `enable`, deliberately without `--now`. A user who turned the single-window policy off has asked to
    // have the tray application back, and the honest reading of that is "start it with my session again" —
    // not "Muffin launches somebody else's window at me, now, in the middle of a settings change". The
    // mask is what was in the way; taking it off and putting the unit back on graphical-session.target
    // returns the machine to the state the user's own package manager left it in.
    return systemctlUserArgs({ QStringLiteral("enable"), warpGuiUserUnit() });
}

QList<WarpManager::GuiSuppressionCommand> WarpManager::guiRestorationCommands(
    Strategy::Platform platform)
{
    if (platform != Strategy::Platform::Linux)
        return {};
    // Unmask first, then put the unit back: the reverse order asks a masked unit to be enabled and is
    // answered with an error about a unit that cannot be run, which is a refusal about the order rather
    // than about the machine. No kill in either list-position here, and no autostart sweep — see the
    // declaration for why a change of mind is not a licence to rename files back.
    return {
        { QStringLiteral("unmask"), QStringLiteral("systemctl"), userUnitUnmaskArgs() },
        { QStringLiteral("reautoenable"), QStringLiteral("systemctl"), userUnitEnableArgs() },
    };
}

QStringList WarpManager::pkillArgs(const QString &processName)
{
    // -x, never -f: see the declaration. A name is matched against the image, so warp-svc and a
    // half-finished warp-cli verb cannot be reached by this command.
    return { QStringLiteral("-x"), processName };
}

QList<WarpManager::GuiSuppressionCommand> WarpManager::guiSuppressionCommands(
    Strategy::Platform platform)
{
    if (platform != Strategy::Platform::Linux)
        return {};

    // The order is the measurement above. disable first, because it is the only command here that touches
    // the *next* session; mask --now, because it is the one that holds against this session's activation
    // traffic; pkill last, because by then the service manager has been told and what is left is an
    // instance it does not own — started by hand, or by a session that beat us to the unit.
    return {
        { QStringLiteral("unautoenable"), QStringLiteral("systemctl"), userUnitDisableArgs() },
        { QStringLiteral("mask"), QStringLiteral("systemctl"), userUnitMaskArgs() },
        { QStringLiteral("kill"), QStringLiteral("pkill"), pkillArgs(warpGuiProcessName()) },
    };
}

bool WarpManager::platformHasWarpGui(Strategy::Platform platform)
{
    // Both of the platforms this application ships for have a window to keep away — Windows by its own
    // tray application, Linux by warp-taskbar. That is a correction rather than a preference: this
    // function used to answer "only Windows" on the belief that the Linux package installs a daemon, a
    // CLI and nothing to look at. It does not. It installs a Flutter desktop application, enables a user
    // unit for it, and maps that unit's window in answer to a tunnel transition.
    //
    // Unknown is the one honest no: a host this file cannot name has no client to own a window, and a yes
    // there would have the settings page promise to close something on a machine nobody installed WARP on.
    return platform == Strategy::Platform::Windows || platform == Strategy::Platform::Linux;
}

QStringList WarpManager::autostartDirectories(const QString &homePath, const QString &xdgConfigDirs)
{
    // The user's own first: it is the only one this process may write, and an entry a user dropped there
    // is theirs to have moved aside without a password. Then one per $XDG_CONFIG_DIRS entry, with the
    // spec's own fallback of /etc/xdg when the variable is unset or empty — a list that *is* set replaces
    // the default rather than adding to it, which is why the fallback lives here and not in the split.
    // Every path is suffixed autostart/, the directory entries are looked in: /etc/xdg alone is the
    // config root, not the entry directory, and a sweep of it would walk a thousand unrelated files.
    QStringList dirs;
    const QString home = QDir::fromNativeSeparators(homePath).trimmed();
    if (!home.isEmpty())
        dirs << home + QStringLiteral("/.config/autostart");

    const QString configured = xdgConfigDirs.trimmed();
    const QStringList roots = configured.isEmpty()
        ? QStringList{ QStringLiteral("/etc/xdg") }
        : configured.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &root : roots) {
        const QString one = QDir::fromNativeSeparators(root).trimmed();
        if (!one.isEmpty())
            dirs << one + QStringLiteral("/autostart");
    }
    dirs.removeAll(QString());
    dirs.removeDuplicates();
    return dirs;
}

bool WarpManager::autostartNamesWarpGui(const QString &fileName, const QString &contents)
{
    // Already moved aside. Saying no here is what makes the sweep idempotent: a second pass over a
    // directory it has already cleared of WARP entries must not rename `x.desktop.disabled` into
    // `x.desktop.disabled.disabled`, which is a file no session reads and no user recognises.
    if (fileName.endsWith(QStringLiteral(".disabled"), Qt::CaseInsensitive))
        return false;

    const QString text = contents.toLower();
    // An entry a session is already told to skip is not the window coming back. Renaming it would bury the
    // fact that somebody else disabled it, and hidden entries cannot autostart in the first place.
    if (text.contains(QStringLiteral("hidden=true")))
        return false;

    // The line that cannot drift with a vendor's file-naming habits: what would actually run. Read line by
    // line so a `Comment=` mentioning the taskbar cannot qualify an entry that is about something else.
    const QString guiName = warpGuiProcessName().toLower();
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QStringLiteral("exec=")))
            continue;
        const QString exec = trimmed.mid(QStringLiteral("exec=").size());
        if (exec.contains(guiName) || exec.contains(QStringLiteral("warp_taskbar")))
            return true;
    }

    // The file name alone, accepted only where it is unmistakably the window's own. Deliberately narrower
    // than "mentions cloudflare": the same package ships com.cloudflare.warp.desktop, whose Exec is
    // `warp-cli registration token %u` — the deep-link handler for an enrolment link, not a tray program.
    // Moving *that* aside would break browser enrolment over a window that was never in the way, which is
    // exactly the over-eager sweep this function exists to refuse.
    const QString lowered = fileName.toLower();
    return lowered.contains(guiName)
           || lowered.contains(QStringLiteral("warptaskbar"))
           || lowered.contains(QStringLiteral("cloudflare-one-client"));
}

QString WarpManager::disabledAutostartName(const QString &fileName)
{
    return fileName + QStringLiteral(".disabled");
}

// ---- the preparation, as data ----

QString WarpManager::tunnelProtocol()
{
    // One spelling for the transport that gets WARP through where its default does not. Named by a
    // function rather than written inline because the argv, the journal line and the test all have to
    // say the same word.
    return QStringLiteral("MASQUE");
}

QStringList WarpManager::protocolArgs()
{
    return { QStringLiteral("--accept-tos"), QStringLiteral("tunnel"),
             QStringLiteral("protocol"), QStringLiteral("set"), tunnelProtocol() };
}

QString WarpManager::serviceUnit()
{
    // The systemd unit the client's own package installs and names after itself.
    return QStringLiteral("warp-svc");
}

QStringList WarpManager::serviceRestartArgs()
{
    return { QStringLiteral("restart"), serviceUnit() };
}

int WarpManager::serviceSettleMs()
{
    // One number, and two readers of it: this class's own preparation, and the setup wizard's chain,
    // which restarts the same unit and then runs `warp-cli connect` as its next command. The gap is a
    // fact about warp-svc rather than about either caller — systemd reports the unit active before the
    // daemon has re-opened its control socket, and a verb issued in that window is answered with "Unable
    // to connect to the CloudflareWARP daemon: No such file or directory" — so a second constant in the
    // dialog would be a second opinion about how long the daemon takes, and the two would drift.
    return kServiceSettleMs;
}

QStringList WarpManager::systemdEnableStartArgs()
{
    // `enable --now` rather than a bare `start`: the second brings the daemon up until the next boot
    // and the first keeps it there, and a tunnel that quietly stops existing after a reboot is a
    // support request nobody can connect back to the wizard that set it up. Both halves are idempotent,
    // so a host whose unit is already enabled and running answers this with exit 0 and nothing to do —
    // which is what lets the step run unconditionally instead of after a state check of its own.
    return { QStringLiteral("enable"), QStringLiteral("--now"), serviceUnit() };
}

QStringList WarpManager::windowsServiceQueryArgs()
{
    return { QStringLiteral("query"), serviceUnit() };
}

QStringList WarpManager::windowsServiceStopArgs()
{
    return { QStringLiteral("stop"), serviceUnit() };
}

QStringList WarpManager::windowsServiceStartArgs()
{
    return { QStringLiteral("start"), serviceUnit() };
}

bool WarpManager::daemonLooksDown(const QString &output)
{
    const QString t = output.toLower();
    // Silence is not a diagnosis. An empty answer leaves the caller's own sentence as the failure,
    // because guessing "the daemon is down" out of nothing at all is how a network refusal ends up
    // telling the user to restart a service that was never the problem.
    if (t.trimmed().isEmpty())
        return false;

    // The client's own vocabulary for "the socket had nobody behind it". Deliberately narrow: every
    // marker names the daemon or the unit, so a refusal about the network, about an enrolment, or about
    // a flag this build does not know is left to the sentence that quoted it.
    static const char *const kMarkers[] = {
        "daemon is not running",
        "daemon isn't running",
        "daemon not running",
        "maybe the daemon",
        "is the daemon running",
        "connect to the daemon",     // "failed to connect to the daemon", "couldn't connect to…"
        "connection to the daemon",
        // The spelling the clean-install report arrived in, verbatim:
        //   Unable to connect to the CloudflareWARP daemon: No such file or directory
        // Worth its own marker because the word between "connect to the" and "daemon" is the vendor's,
        // so neither of the two phrasings above matches it — and this is the one answer that means the
        // restart has not finished re-opening its socket yet, i.e. exactly the refusal worth waiting out
        // rather than reporting.
        "warp daemon",
        "daemon is unavailable",
        "warp-svc is not running",
        "warp-svc failed",
    };
    for (const char *marker : kMarkers) {
        if (t.contains(QString::fromLatin1(marker)))
            return true;
    }
    return false;
}

QString WarpManager::serviceStartHint(Strategy::Platform platform)
{
    if (platform == Strategy::Platform::Windows)
        return QStringLiteral("sc start %1").arg(serviceUnit());
    return QStringLiteral("sudo systemctl start %1").arg(serviceUnit());
}

QStringList WarpManager::ipv6CheckArgs()
{
    // `-n` prints the value alone, which is what parseIpv6State() reads. Asking rather than setting:
    // see prepareEnvironment() for why this one is a check and not a fix.
    return { QStringLiteral("-n"), QStringLiteral("net.ipv6.conf.all.disable_ipv6") };
}

WarpManager::Ipv6 WarpManager::parseIpv6State(const QString &sysctlOutput)
{
    // The kernel's own spelling is "<key> = 0" when sysctl printed the key anyway, and a bare "0" or
    // "1" under `-n`. Both are read, because a host that printed the key is not a host whose IPv6
    // state we should then claim not to know. Anything else — an empty answer, an error sentence, a
    // value that is neither digit — is Unknown, which the journal reports as "could not tell" rather
    // than as a finding about the machine.
    const QString t = sysctlOutput.trimmed();
    if (t.isEmpty())
        return Ipv6::Unknown;
    const QString value = t.contains(QLatin1Char('=')) ? t.section(QLatin1Char('='), 1).trimmed() : t;
    if (value == QLatin1String("1"))
        return Ipv6::Disabled;      // disable_ipv6=1 means this host switched IPv6 off
    if (value == QLatin1String("0"))
        return Ipv6::Enabled;
    return Ipv6::Unknown;
}

QList<WarpManager::PreparationStep> WarpManager::preparationSteps(Strategy::Platform platform)
{
    // Linux carries the whole sequence: the transport that survives a hostile provider, the read that
    // explains why a MASQUE tunnel may still not come up, and the restart that clears the daemon's
    // stale state.
    //
    // The order is the finding, not a preference. Protocol first because it is free, unprivileged and
    // warp-cli's own verb — if the daemon is reachable at all, this lands before anything heavier
    // runs. IPv6 second because it is a read, and reading before a restart means the journal's line
    // about the host describes the host as it was, not as the restart left it. Service last because it
    // is the step that can prompt a human for a password, and nothing that need not wait behind it
    // should be made to.
    QList<PreparationStep> steps;
    if (platform == Strategy::Platform::Linux || platform == Strategy::Platform::Windows) {
        steps.append({ QStringLiteral("masque"), warpCliProgram(), protocolArgs(),
                       /*privileged=*/false, /*fatal=*/false, kProtocolTimeoutMs });
    }
    if (platform != Strategy::Platform::Linux) {
        // Windows gets the protocol step and nothing else: the stuck-daemon state is systemd's, and
        // the equivalent here would be `sc stop`/`sc start` on the service — a UAC prompt on every
        // tunnel start, which is a cost nobody agreed to pay for a problem not observed on this
        // platform. Unknown gets neither step, because there is no client to reconfigure.
        return steps;
    }
    steps.append({ QStringLiteral("ipv6"), QStringLiteral("sysctl"), ipv6CheckArgs(),
                   /*privileged=*/false, /*fatal=*/false, kIpv6TimeoutMs });
    steps.append({ QStringLiteral("service"), QStringLiteral("systemctl"), serviceRestartArgs(),
                   /*privileged=*/true, /*fatal=*/false, kServiceTimeoutMs });
    return steps;
}

bool WarpManager::composeServiceRestart(const QString &systemctlPath, const QString &pkexecPath,
                                        bool alreadyRoot, QString *program, QStringList *args,
                                        QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!program || !args)
        return fail(QStringLiteral("WarpManager: composeServiceRestart() needs somewhere to write"));
    program->clear();
    args->clear();

    // No systemctl means no systemd: this is a host the restart cannot be performed on, said plainly
    // rather than worked around — the alternative is guessing at `service restart` for a unit that may
    // not exist there either.
    if (systemctlPath.isEmpty())
        return fail(QStringLiteral("WarpManager: this host has no systemctl, so %1 cannot be "
                                   "restarted before the tunnel is asked for").arg(serviceUnit()));
    if (!alreadyRoot && pkexecPath.isEmpty())
        return fail(QStringLiteral("WarpManager: restarting %1 needs root, and pkexec is not "
                                   "installed. Run Muffin as root, or restart the service by hand: "
                                   "systemctl restart %1").arg(serviceUnit()));

    // The gate itself belongs to composePrivilegedCommand(), and is deliberately not restated here:
    // two copies of "pkexec from a root session is a prompt nobody is there to answer" is two chances
    // for one of them to be edited without the other.
    return composePrivilegedCommand(Strategy::Platform::Linux, systemctlPath, serviceRestartArgs(),
                                    pkexecPath, alreadyRoot, program, args, error);
}

bool WarpManager::composePrivilegedCommand(Strategy::Platform platform, const QString &programPath,
                                           const QStringList &args, const QString &pkexecPath,
                                           bool alreadyRoot, QString *outProgram,
                                           QStringList *outArgs, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!outProgram || !outArgs)
        return fail(QStringLiteral("WarpManager: composePrivilegedCommand() needs somewhere to write"));
    outProgram->clear();
    outArgs->clear();

    if (programPath.isEmpty())
        return fail(QStringLiteral("WarpManager: a privileged command named no program to run"));

    // Windows needs no prefix: packaging/windows/app.manifest asks for requireAdministrator, so this
    // process already holds the token, and a second prompt for something it may already do is a UAC
    // dialog with nothing behind it. Linux already root is the same case for a different reason — the
    // gate WarpInstaller::composeLinuxInstall(), MuffinInstallHelper::composePromote() and LinuxBackend
    // all apply.
    if (platform != Strategy::Platform::Linux || alreadyRoot) {
        *outProgram = programPath;
        *outArgs = args;
        return true;
    }
    if (pkexecPath.isEmpty())
        return fail(QStringLiteral("WarpManager: this needs root, and pkexec is not installed. Run "
                                   "Muffin as root, or run it by hand: %1 %2")
                        .arg(programPath, args.join(QLatin1Char(' '))));

    *outProgram = pkexecPath;
    *outArgs = QStringList{ programPath } + args;
    return true;
}

// ---- the setup wizard's chain ----

QStringList WarpManager::setupStepIds(Strategy::Platform platform)
{
    // The full chain, spelled by asking for the plan of a machine that has nothing and knows nothing:
    // one source of truth for the order, rather than a second list a test compares the first one to.
    QStringList ids;
    const QList<SetupStep> all = setupSteps(platform, /*installed=*/false, Registration::Unknown);
    for (const SetupStep &s : all)
        ids << s.id;
    return ids;
}

QList<WarpManager::SetupStep> WarpManager::setupSteps(Strategy::Platform platform, bool installed,
                                                      Registration registration)
{
    // The order is the finding, and every step has a reason for being where it is:
    //
    //   install  — nothing else can be asked of a client that is not on the disk. Skipped when one is
    //              here, because a package manager run over a working install is how a tunnel gets
    //              removed rather than repaired.
    //   daemon   — the socket every later verb is addressed to. A package that was just unpacked does
    //              not promise warp-svc was started, and a daemon that is down answers an enrolment
    //              with "Missing registration … Maybe the daemon is not running?", which is a sentence
    //              about the service and reads like one about the device. Before the enrolment, and
    //              only where the plan enrols: an already-enrolled machine is brought up by the restart
    //              below just as surely, and a second root prompt in one run is a cost with nothing
    //              behind it.
    //   register — a connect against an unenrolled device is refused by the daemon. Delegated to
    //              ensureRegistered() rather than spelled as `registration new` here: that function
    //              reads first and deletes a conflicting enrolment before making a new one, while a
    //              second `registration new` on an enrolled machine is a second key and an orphaned
    //              first. Skipped when the reading already says Registered.
    //   protocol — the transport that survives a provider interfering with the default one.
    //   service  — the daemon's state goes stale when it came up while a zapret queue was already
    //              rewriting the routing, and only a restart clears it. After the protocol, because the
    //              restart is what makes that setting belong to the daemon now running rather than to
    //              the next one somebody starts by hand — and after the enrolment, because a daemon
    //              restarted mid `registration new` loses the key that call is obtaining.
    //   connect  — last, and only after all of the above. It is the verb that used to be issued first
    //              and then never answered, which is the whole reason this chain exists.
    QList<SetupStep> steps;
    const bool knownPlatform = platform == Strategy::Platform::Linux
                               || platform == Strategy::Platform::Windows;

    if (!installed) {
        SetupStep install;
        install.id = QStringLiteral("install");
        install.kind = SetupKind::Install;
        // Not cancellable, and the wizard disables its own Cancel for the length of the step rather
        // than offering a choice it cannot honour: an apt or msiexec interrupted halfway leaves a
        // database to repair. WarpInstaller owns the timeout, the staging and the classification of
        // what went wrong.
        install.cancellable = false;
        steps.append(install);
    }

    if (registration != Registration::Registered) {
        if (knownPlatform) {
            // Every command here is non-fatal, and that is a decision rather than an oversight. What
            // this step wants is a daemon that answers, and the enrolment that follows is the only
            // judge of whether it got one: a refused pkexec, a host with no systemd at all (a container,
            // where the daemon may still be running under something else) and a Windows service that
            // was already up — which `sc start` reports as the error 1056 — all land here, and only the
            // last of those is a success. Stopping the wizard on a refusal it cannot interpret would
            // strand a machine the next step could have enrolled; continuing costs one warp-cli call
            // and ends in the enrolment's own words, which is the sentence the hint is written for.
            SetupStep daemon;
            daemon.id = QStringLiteral("daemon");
            if (platform == Strategy::Platform::Linux) {
                daemon.commands.append({ QStringLiteral("systemctl"), systemdEnableStartArgs(),
                                         /*privileged=*/true, kServiceTimeoutMs, /*fatal=*/false });
            } else {
                // Query, then start. The query is what puts the service's state in the pane, so "it was
                // already running" is something the user reads rather than infers from a refusal, and a
                // service that is not there at all is named before anything tries to start it.
                daemon.commands.append({ QStringLiteral("sc"), windowsServiceQueryArgs(),
                                         /*privileged=*/false, kProtocolTimeoutMs, /*fatal=*/false });
                daemon.commands.append({ QStringLiteral("sc"), windowsServiceStartArgs(),
                                         /*privileged=*/true, kProtocolTimeoutMs, /*fatal=*/false });
            }
            steps.append(daemon);
        }

        SetupStep reg;
        reg.id = QStringLiteral("register");
        reg.kind = SetupKind::Registration;
        steps.append(reg);
    }

    if (knownPlatform) {
        SetupStep protocol;
        protocol.id = QStringLiteral("protocol");
        protocol.commands.append({ warpCliProgram(), protocolArgs(), /*privileged=*/false,
                                   kProtocolTimeoutMs });
        steps.append(protocol);
    }

    if (platform == Strategy::Platform::Linux) {
        SetupStep service;
        service.id = QStringLiteral("service");
        service.commands.append({ QStringLiteral("systemctl"), serviceRestartArgs(),
                                  /*privileged=*/true, kServiceTimeoutMs });
        steps.append(service);
    } else if (platform == Strategy::Platform::Windows) {
        // Two commands and not one, because a service control manager has no restart verb and no shell
        // is allowed to invent one. Both privileged, both short, and the step fails if either does: a
        // service left stopped by a wizard that reported success is worse than a wizard that said so.
        SetupStep service;
        service.id = QStringLiteral("service");
        service.commands.append({ QStringLiteral("sc"), windowsServiceStopArgs(),
                                  /*privileged=*/true, kProtocolTimeoutMs });
        service.commands.append({ QStringLiteral("sc"), windowsServiceStartArgs(),
                                  /*privileged=*/true, kProtocolTimeoutMs });
        steps.append(service);
    }

    if (knownPlatform) {
        SetupStep connectStep;
        connectStep.id = QStringLiteral("connect");
        connectStep.commands.append({ warpCliProgram(), connectArgs(), /*privileged=*/false,
                                      kVerbTimeoutMs });
        steps.append(connectStep);
    }
    return steps;
}

WarpManager::State WarpManager::parseStatus(const QString &raw, bool wasConnectAttempt)
{
    // warp-cli's status answer is a single line ending in a state word, wrapped in an ISO
    // timestamp and the program's own log tag. Windows writes a shorter "Status: X" prefix;
    // macOS/Linux write "<date> warp-cli [INFO] X". Both spell the state the same way, so we
    // look for the words in order of confidence.
    //
    // Case-insensitive because the version on Windows capitalises "Connected" while the version
    // on Linux writes it the same way but sometimes prefixes "Already ". Any of these being a
    // substring of the whole answer is enough to name the state; the order below is what makes
    // "Disconnected" not match "Connected" first, since a "not connected" phrasing would
    // otherwise be read as connected.
    const QString t = raw.toLower();

    // Check the negatives first. "Disconnected" contains "connected", so if we scanned for
    // "connected" first every disconnected client would report as connected.
    if (t.contains(QStringLiteral("disconnected")))
        return State::Disconnected;
    if (t.contains(QStringLiteral("disconnecting")))
        return State::Disconnecting;

    // The daemon's "connecting" phase is often reported as the string "Connect pending" or
    // "Connecting" (Linux) or "Status: Connecting" (Windows).
    if (t.contains(QStringLiteral("connecting")) || t.contains(QStringLiteral("connect pending")))
        return State::Connecting;

    if (t.contains(QStringLiteral("connected")))
        return State::Connected;

    // A daemon that has never been enrolled says "Not registered" — from warp-cli's own
    // vocabulary. From the user's point of view that means "WARP is not set up on this
    // machine", and the honest answer depends on whether we just asked it to connect.
    if (t.contains(QStringLiteral("not registered")) || t.contains(QStringLiteral("unregistered")))
        return wasConnectAttempt ? State::Connecting : State::Disconnected;

    // No recognisable marker. The safest thing to say is "the tunnel is not up", because the
    // alternative — reporting Connected on an unparseable answer — is a claim that would keep
    // the button green when the daemon has actually gone away.
    return State::Disconnected;
}

WarpManager::Registration WarpManager::parseRegistration(const QString &raw, int exitCode)
{
    // Same reason parseStatus() lowercases first: the text comes from different vendors' build trees,
    // and the casing of "Registration"/"registration" has not been stable between them.
    const QString t = raw.toLower();

    // Negatives, and ahead of every positive, because they are substrings of the positives they would
    // otherwise be read as. "not enrolled" contains "enrolled"; "not registered" contains "registered";
    // "Could not get configuration: Client registration does not exist!" contains "registration", which
    // is the single most tempting word in this output and the one most likely to be present when the
    // answer is no.
    if (t.contains(QStringLiteral("does not exist"))
        || t.contains(QStringLiteral("not enrolled"))
        || t.contains(QStringLiteral("not registered"))
        || t.contains(QStringLiteral("no registration"))
        // The spelling the field report arrived in: `Error: Missing registration. Try running:
        // "warp-cli registration new"`, exit 1. A client that names the verb which would fix it is
        // giving the clearest negative it has, and answering Unknown to it is the one reading that
        // makes the wizard useless — needsEnrollment() excludes Unknown on purpose, so the enrolment is
        // never attempted and every retry fails on the same sentence with the daemon perfectly healthy.
        || t.contains(QStringLiteral("missing registration"))
        || t.contains(QStringLiteral("client is not enrolled"))
        || t.contains(QStringLiteral("unregistered"))) {
        return Registration::NotRegistered;
    }

    // A conflict: an enrolment exists that the daemon will not use, so it has to go before a new one
    // can come. The HTTP status, the daemon's own word, and the two account-limit phrasings that are
    // the same problem wearing different clothes all land here, because all three are answered the
    // same way — delete, then enrol. Tested after the negatives because a build that says "not
    // registered: conflict" means the first more usefully than the second.
    if (t.contains(QStringLiteral("conflict"))
        || t.contains(QStringLiteral("409"))
        || t.contains(QStringLiteral("already registered"))
        || t.contains(QStringLiteral("already enrolled"))
        || t.contains(QStringLiteral("too many devices"))
        || t.contains(QStringLiteral("maximum number of device"))) {
        return Registration::Conflict;
    }

    // Positives. Which fields a registered client prints has moved between versions — `Type:` is
    // spelled `enrollment` on some builds and `warp` on others, and a limited account omits it — so
    // any single one of these names an enrolment, but one of them has to actually be there, and the
    // run has to have succeeded for it to count. An empty output with exit 0 is not "registered", it
    // is a daemon with nothing to say, which is the case this branch exists to refuse to guess about.
    if (exitCode == 0
        && (t.contains(QStringLiteral("registration id"))
            || t.contains(QStringLiteral("account type"))
            || t.contains(QStringLiteral("type: enrollment"))
            || t.contains(QStringLiteral("type: limited"))
            || t.contains(QStringLiteral("type: warp"))
            || t.contains(QStringLiteral("key:")))) {
        return Registration::Registered;
    }

    // Nothing readable, whatever the exit code said. This is the branch that decides the device is
    // left alone, so it is worth being explicit about what it covers: a `registration show` that
    // failed for a reason we have never seen, output in a language whose words for these states are
    // not the ones above, and a daemon that answered with an error string of its own invention.
    return Registration::Unknown;
}

bool WarpManager::needsEnrollment(Registration r)
{
    switch (r) {
    case Registration::NotRegistered:
    case Registration::Conflict:
        return true;
    case Registration::Registered:
    case Registration::Unknown:
        break;
    }
    // Registered needs nothing. Unknown needs something we are not entitled to do on a guess.
    return false;
}

bool WarpManager::needsDeleteFirst(Registration r)
{
    return r == Registration::Conflict;
}

QStringList WarpManager::installCandidatePaths(const QString &programFilesRoot)
{
    // An empty root is not a directory to look in. Every non-Windows caller passes exactly that,
    // because there the package manager owns /usr/bin and PATH already names the client; the value
    // only arrives from a host that read ProgramFiles out of its own environment.
    const QString root = QDir::fromNativeSeparators(programFilesRoot).trimmed();
    if (root.isEmpty())
        return {};
    return { root + QStringLiteral("/Cloudflare/Cloudflare WARP/warp-cli.exe") };
}

QString WarpManager::resolveExecutable(const QString &pathHit, const QStringList &candidates)
{
    // PATH wins when it answered: whatever the user's shell would find is the program the user
    // means, and a copy in Program Files is only a fallback for the host where PATH was never told.
    if (!pathHit.isEmpty())
        return pathHit;
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

QString WarpManager::programFilesRoot()
{
#if defined(Q_OS_WIN)
    // Read from this process's own environment block, which is the elevated one: a value a
    // non-admin could edit would point a driver install somewhere else. ZapretPaths reads the same
    // variable for the same reason (see systemInstallDir()).
    return QProcessEnvironment::systemEnvironment().value(QStringLiteral("ProgramFiles"));
#else
    return QString();
#endif
}

WarpManager::WarpManager(QObject *parent)
    : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    m_pollTimer->setTimerType(Qt::CoarseTimer);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        // A poll is not a user request — if a verb is already in flight, drop this tick rather than
        // queueing a second warp-cli behind it and having the pair disagree about the state.
        if (m_process)
            return;
        QString err;
        refresh(&err);
    });

    // The preparation's two clocks. Single-shot both: the settle gap is owed once per restart, and a
    // connect gets one deadline per request rather than a repeating reminder every fifteen seconds.
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    connect(m_settleTimer, &QTimer::timeout, this, [this] {
        finishPreparation(true, QString());
    });

    m_connectDeadline = new QTimer(this);
    m_connectDeadline->setSingleShot(true);
    m_connectDeadline->setTimerType(Qt::CoarseTimer);
    connect(m_connectDeadline, &QTimer::timeout, this, &WarpManager::onConnectDeadline);

    rescanAvailability();
}

WarpManager::~WarpManager()
{
    // Kill any warp-cli still running: a status probe that outlives us would be a stray subprocess
    // with no owner. QProcess does the right thing when its parent is destroyed, but the explicit
    // kill makes the intent readable at a glance — and the two detection processes have to be
    // unwired as well, because their lambdas answer to `this` and would run after it is gone.
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    if (m_versionProcess) {
        m_versionProcess->disconnect(this);
        m_versionProcess->kill();
        m_versionProcess->waitForFinished(200);
    }
    if (m_probeProcess) {
        m_probeProcess->disconnect(this);
        m_probeProcess->kill();
        m_probeProcess->waitForFinished(200);
    }
    if (m_guiProcess) {
        // The same unwiring as the other two side chains, and for the same reason: a systemctl still
        // running when this object goes away would answer into a deleted `this`. The command itself is
        // idempotent, so killing it mid-run leaves the machine no worse than it found it.
        m_guiProcess->disconnect(this);
        m_guiProcess->kill();
        m_guiProcess->waitForFinished(200);
    }
}

bool WarpManager::refreshProgramPath()
{
    // PATH first, then the directories the client's own packages write to. The second half is not
    // decoration: the Windows MSI never edits PATH, a distro package lands in /usr/bin that a service
    // manager's environment may never have named, a snap's command lives under /snap/bin, and a
    // user-scoped flatpak export is on nobody's PATH at all — so a machine that has the client installed
    // answers a PATH-only probe with nothing, and the interface offers to install software that is
    // already there.
    QStringList candidates = installCandidatePaths(programFilesRoot());
    candidates += extraCandidatePaths(QDir::homePath());
    const QString resolved = resolveExecutable(QStandardPaths::findExecutable(warpCliProgram()),
                                              candidates);
    const bool was = m_available;
    m_program = resolved;
    setAvailable(isAvailableFromPath(resolved));
    if (!was && m_available) {
        // The one transition worth a journal line of its own: an answer this object had already given
        // turned out to be stale, and everything that follows — a verb refused or allowed, an install
        // offered or not — is read against it.
        emit logMessage(QStringLiteral("WarpManager: warp-cli was not there a moment ago and is now — "
                                        "found at %1").arg(resolved), 0);
    }
    return m_available;
}

void WarpManager::rescanAvailability()
{
    // A rescan makes any earlier answer stale, and a round still running would answer about the
    // machine as it was before whatever the caller just did. Stop it before deciding anything new.
    stopInstallationProbe();
    m_broken = false;
    m_brokenDetail.clear();
    m_packaged = false;
    m_packagedVia.clear();

    refreshProgramPath();

    if (!m_available) {
        // Nothing runnable. That is not yet the same as nothing installed, so ask the host before any
        // caller is allowed to conclude it is empty.
        startInstallationProbe();
        return;
    }
    // Found. Ask it one question before the machine is trusted to be drivable: a half-removed
    // package, an update interrupted mid-way and a binary whose libraries are gone all leave an
    // executable behind for PATH to find, and every verb issued to one of those fails with an error
    // the user can do nothing about.
    verifyCandidate();
}

void WarpManager::setAvailable(bool available)
{
    if (available == m_available)
        return;
    m_available = available;
    emit availabilityChanged(available);
    // The state a moment ago ("Unavailable") is not true any more, so say so. If WARP became
    // available the next refresh will replace this with the daemon's real answer; if it went
    // away, nothing can.
    setState(available ? State::Disconnected : State::Unavailable);
    if (available) {
        QString err;
        refresh(&err);
    }
}

void WarpManager::verifyCandidate()
{
    if (m_versionProcess)
        return;     // one verdict at a time; the round already running is the answer being sought

    auto *proc = new QProcess(this);
    proc->setProgram(m_program);
    proc->setArguments(versionArgs());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::finished, this,
            [this, proc](int exitCode, QProcess::ExitStatus status) {
                if (m_versionProcess != proc)
                    return;
                m_versionProcess = nullptr;
                const QString out = QString::fromUtf8(proc->readAll()).trimmed();
                proc->deleteLater();
                onVersionAnswered(out, exitCode, status == QProcess::CrashExit);
            });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError) {
        if (m_versionProcess != proc)
            return;
        // FailedToStart is the loudest possible Broken: the operating system refused the file. The
        // guard above keeps this from double-answering a child that also manages to emit finished().
        m_versionProcess = nullptr;
        const QString out = QString::fromUtf8(proc->readAll()).trimmed();
        proc->deleteLater();
        onVersionAnswered(out.isEmpty() ? QStringLiteral("failed to start") : out, -1, true);
    });

    m_versionProcess = proc;
    proc->start();
    // The one deadline this class cannot read off a live client: a warp-cli waiting on a daemon that
    // is not answering would otherwise hold the verdict — and so the offer it gates — open forever.
    // Being killed classifies as Broken, which is the honest reading of a client that cannot say what
    // version it is.
    QTimer::singleShot(kVersionTimeoutMs, proc, [this, proc]() {
        if (m_versionProcess != proc)
            return;
        proc->kill();
    });
}

void WarpManager::onVersionAnswered(const QString &out, int exitCode, bool crashed)
{
    const Probe verdict = classifyVersionAnswer(out, exitCode, crashed);
    if (verdict == Probe::Usable) {
        // The client is what it claims to be. Nothing is emitted: availability was announced
        // optimistically when the program was found, and a second identical signal would be noise.
        return;
    }
    if (verdict == Probe::Unreadable) {
        // Say it rather than swallow it. The verdict is "no news", but the reason is a fact a user
        // reading the journal after a connect that went nowhere will want to have seen.
        emit logMessage(QStringLiteral("WarpManager: the client's version answer (%1) names neither a "
                                        "working install nor a damaged one — leaving it as it is")
                            .arg(out.isEmpty()
                                     ? QStringLiteral("exit %1 with no output").arg(exitCode)
                                     : out),
                        1);
        return;
    }

    // Broken: an executable is on this disk and it cannot be driven. Take the half of the interface
    // that presumes otherwise away, and remember why, so the offer that follows is the repair rather
    // than a fresh install of something that is already there.
    m_broken = true;
    m_brokenDetail = out;
    emit logMessage(QStringLiteral("WarpManager: a WARP client was found but does not answer as one "
                                   "— it needs repairing (%1)")
                        .arg(out.isEmpty() ? QStringLiteral("exit %1").arg(exitCode) : out),
                    2);
    setAvailable(false);
    startInstallationProbe();
}

void WarpManager::setPollingEnabled(bool on)
{
    m_pollEnabled = on;
    if (!m_pollTimer)
        return;
    if (on && m_available)
        m_pollTimer->start();
    else
        m_pollTimer->stop();
}

void WarpManager::startInstallationProbe()
{
    if (m_probing)
        stopInstallationProbe();      // an answer about the machine as it was is not this one's

    // The registry reads before anything is forked: it costs no process, and on Windows it is the
    // cheapest way to learn that an MSI lives here. An empty version off-Windows is exactly the
    // nothing-to-report answer.
    if (registryReportsInstall()) {
        finishProbe(true, QStringLiteral("registry"));
        return;
    }

    m_probeQueue = installationProbes(ZapretSource::hostPlatform());
    if (m_probeQueue.isEmpty()) {
        // Nothing to ask — an unknown platform, or a host with none of these tools. "Nothing found"
        // is the honest summary, and the caller gets to decide whether to ask a human.
        finishProbe(false, QString());
        return;
    }
    m_probing = true;
    runNextProbe();
}

void WarpManager::runNextProbe()
{
    while (!m_probeQueue.isEmpty()) {
        const InstallationProbe probe = m_probeQueue.takeFirst();
        const QString program = QStandardPaths::findExecutable(probe.program);
        if (program.isEmpty())
            continue;     // this host has no such database to ask, which is not evidence of absence
        m_currentProbe = probe;

        auto *proc = new QProcess(this);
        proc->setProgram(program);
        proc->setArguments(probe.args);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(proc, &QProcess::finished, this,
                [this, proc](int exitCode, QProcess::ExitStatus status) {
                    if (m_probeProcess != proc)
                        return;
                    m_probeProcess = nullptr;
                    const QString out = QString::fromUtf8(proc->readAll()).trimmed();
                    proc->deleteLater();
                    onProbeFinished(out, exitCode, status == QProcess::CrashExit);
                });
        m_probeProcess = proc;
        proc->start();
        // One deadline per question, and a short one: a whole round of these gates an offer, and a
        // package database that has been left hanging would leave a user unable to install software
        // they have every right to install. A killed probe answers "no", which is what an unanswered
        // one is.
        QTimer::singleShot(kProbeTimeoutMs, proc, [this, proc]() {
            if (m_probeProcess != proc)
                return;
            proc->kill();
        });
        return;
    }
    finishProbe(false, QString());
}

void WarpManager::onProbeFinished(const QString &out, int exitCode, bool crashed)
{
    if (probeAnswerNamesInstall(m_currentProbe, out, exitCode, crashed)) {
        // The first yes ends the round: a machine with both dpkg and snap knows which one owns the
        // client, and asking the rest adds nothing but delay.
        finishProbe(true, m_currentProbe.source);
        return;
    }
    if (probeAnswerNamesLeftovers(m_currentProbe, out, exitCode, crashed)) {
        // Debris rather than an install. Not packaged — there is no client to use — but broken enough
        // that the next offer has to say it will clean up.
        m_broken = true;
        m_brokenDetail = QStringLiteral("%1: %2").arg(m_currentProbe.source, out);
        emit logMessage(QStringLiteral("WarpManager: %1 reports a removed cloudflare-warp package "
                                        "whose configuration files are still on the machine")
                            .arg(m_currentProbe.source),
                        1);
    }
    runNextProbe();
}

void WarpManager::finishProbe(bool packaged, QString via)
{
    m_probing = false;
    m_probeQueue.clear();
    m_currentProbe = InstallationProbe();
    if (m_probeProcess) {
        QProcess *proc = m_probeProcess;
        m_probeProcess = nullptr;
        proc->kill();
        proc->deleteLater();
    }
    m_packaged = packaged;
    m_packagedVia = packaged ? via : QString();
    if (packaged) {
        emit logMessage(QStringLiteral("WarpManager: an existing WARP client was found via %1 — it is "
                                        "left alone rather than installed over")
                            .arg(via),
                        1);
    }
    // Emitted whether or not anything moved. A caller holding a deferred offer is waiting on this
    // signal rather than on a diff, and `installed` is re-read rather than passed in so the answer
    // cannot disagree with what isInstalled() says a moment later.
    emit installationChanged(isInstalled(), m_packagedVia);
}

void WarpManager::stopInstallationProbe()
{
    m_probing = false;
    m_probeQueue.clear();
    m_currentProbe = InstallationProbe();
    if (!m_probeProcess)
        return;
    QProcess *proc = m_probeProcess;
    m_probeProcess = nullptr;
    proc->kill();
    proc->deleteLater();
}

// ---- the window suppression ----

void WarpManager::suppressWarpGui()
{
    // The switch is the user's, and it is the only difference between this ask and the reverse one below:
    // a host where they asked for the tray application is a host whose window nobody may close.
    if (!m_hideWarpGui)
        return;
    beginGuiPolicy(/*restoring=*/false);
}

void WarpManager::restoreWarpGui()
{
    beginGuiPolicy(/*restoring=*/true);
}

void WarpManager::beginGuiPolicy(bool restoring)
{
    const Strategy::Platform platform = ZapretSource::hostPlatform();

    // Two gates, cheapest first, and each rules out a host where the run below would be a fork with
    // nothing behind it: a platform with no WARP window has no process for these commands to name, and a
    // machine with no client at all is a machine this class must not go poking the process table or the
    // session manager of. isInstalled() rather than isAvailable(), because a packaged-but-unrunnable
    // client still owns a tray program and a user unit.
    if (!platformHasWarpGui(platform) || !isInstalled())
        return;
    // Already on its way. A second mask — or a second unmask — of one unit is the same command asked
    // twice, and the run in flight will reach it anyway. Two runs in parallel would also be two writers
    // to one queue, which is how a restore ends up executing a suppression's argument list.
    if (m_guiRunning)
        return;

    m_guiRestoring = restoring;
    m_guiQueue = restoring ? guiRestorationCommands(platform) : guiSuppressionCommands(platform);
    m_guiChanged = false;
    m_guiRunning = true;
    if (m_guiQueue.isEmpty()) {
        // Nothing to command, but still worth ending properly: the sweep belongs to a run whether or not
        // the platform had commands, and m_guiRunning has to come back down through one path.
        finishGuiSuppression(QString());
        return;
    }
    runNextGuiSuppression();
}

void WarpManager::runNextGuiSuppression()
{
    while (!m_guiQueue.isEmpty()) {
        const GuiSuppressionCommand command = m_guiQueue.takeFirst();
        // Resolved now rather than stored: where systemctl lives is a fact about the host at the moment
        // the run asks, not about its platform. A tool that is not there is named and skipped — unlike a
        // missing installation probe, whose absence settles the question, the next command here may still
        // land, and stopping at the first missing binary would leave the window up on a host whose pkill
        // somebody moved.
        const QString program = QStandardPaths::findExecutable(command.program);
        if (program.isEmpty()) {
            emit logMessage(QStringLiteral("WarpManager: %1 is not available on this host — skipping "
                                            "the %2 step for the WARP window")
                                .arg(command.program, command.id), 1);
            continue;
        }

        auto *proc = new QProcess(this);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        proc->setProgram(program);
        proc->setArguments(command.args);
        m_currentGuiCommand = command;

        connect(proc, &QProcess::finished, this,
                [this, proc, command](int exitCode, QProcess::ExitStatus status) {
            if (m_guiProcess != proc)
                return;
            m_guiProcess = nullptr;
            const QString out = QString::fromUtf8(proc->readAll()).trimmed();
            proc->deleteLater();
            onGuiSuppressionAnswered(command, exitCode, status == QProcess::CrashExit, out);
        });
        connect(proc, &QProcess::errorOccurred, this, [this, proc, command](QProcess::ProcessError e) {
            // FailedToStart is the only error that never produces finished(); left alone it would strand
            // the run with an owner nothing is waiting for.
            if (e != QProcess::FailedToStart || m_guiProcess != proc)
                return;
            m_guiProcess = nullptr;
            const QString text = proc->errorString();
            proc->deleteLater();
            // Not exec'd is the same shape as refused: the chain moves on, because a machine whose
            // systemctl will not start still has a pkill that can.
            onGuiSuppressionAnswered(command, -1, true, text);
        });

        m_guiProcess = proc;
        proc->start();
        // Bounded like every other child in this file. A session manager that has stopped answering must
        // not hold the window policy open forever; the kill lands as CrashExit, and the chain continues.
        QTimer::singleShot(kGuiCommandTimeoutMs, proc, [this, proc]() {
            if (m_guiProcess != proc)
                return;   // it answered on its own
            proc->kill();
        });
        return;   // the answer above carries the chain forward
    }
    finishGuiSuppression(QString());
}

void WarpManager::onGuiSuppressionAnswered(const GuiSuppressionCommand &command, int exitCode,
                                           bool crashed, const QString &output)
{
    // What an answer means is not one rule here, and the difference is the whole of what the journal may
    // say about it. pkill has a vocabulary of its own: 0 means it matched something, 1 means nothing was
    // running — the outcome wanted, not a failure — and only codes above that are a complaint about the
    // command. Reporting the middle one as a problem would have the log claim a window is still up on a
    // machine where no window was ever started.
    if (command.id == QLatin1String("kill") && !crashed && exitCode == 1) {
        runNextGuiSuppression();
        return;
    }
    if (crashed || exitCode != 0) {
        // Warn, never error, and never fatal to the run. A window that survives is a cosmetic failure on
        // a machine whose tunnel still works, and the next command may still land: a host whose
        // systemctl is not the session manager the app guessed still has a process table to sweep.
        emit logMessage(QStringLiteral("WarpManager: the %1 step for the WARP window did not succeed (%2)")
                            .arg(command.id,
                                 output.isEmpty()
                                     ? QStringLiteral("exit %1").arg(exitCode)
                                     : output.left(kGuiDetailChars)), 1);
        runNextGuiSuppression();
        return;
    }

    // Success — and only a success that *moved* anything earns a sentence. systemctl prints "Created
    // symlink …" when it masks and "Removed …" when it untangles a unit, and nothing at all when the unit
    // was already where it belongs, so the output is the evidence that this connect changed the machine
    // rather than re-applied a policy that was already in force. pkill's own evidence is a zero exit,
    // which is the one code that says something had been running.
    if (command.id == QLatin1String("kill") || !output.trimmed().isEmpty())
        m_guiChanged = true;

    runNextGuiSuppression();
}

void WarpManager::finishGuiSuppression(const QString &why)
{
    if (!m_guiRunning)
        return;   // already closed: one end per started run, exactly as the two sequences' finish* keep
    m_guiRunning = false;
    m_guiQueue.clear();
    m_currentGuiCommand = GuiSuppressionCommand();
    if (m_guiProcess) {
        QProcess *proc = m_guiProcess;
        m_guiProcess = nullptr;
        proc->disconnect(this);
        proc->kill();
        proc->deleteLater();
    }

    // The sweep once per object, on the platform whose sessions read those directories, and only on the
    // way down: a restore is a change of mind about a policy, not a licence to rename back files that
    // some other tool — or the user — may have edited since. Done at the end rather than the start
    // because it is the one part of this that touches files rather than the service manager, and a run
    // still holding a child has no business starting a directory walk on top of it.
    if (!m_autostartSwept && !m_guiRestoring
        && ZapretSource::hostPlatform() == Strategy::Platform::Linux) {
        m_autostartSwept = true;
        sweepWarpAutostart();
    }

    if (m_guiChanged && why.isEmpty()) {
        // One sentence per direction, because the two runs leave the machine in opposite states and a
        // single wording would misreport one of them. Both are quiet when nothing moved: systemctl says
        // "Created symlink …" / "Removed …" only when it changed something, which is the evidence
        // m_guiChanged is built from.
        emit logMessage(m_guiRestoring
                            ? QStringLiteral("WarpManager: the WARP window's session unit was unmasked and "
                                             "enabled again — the tray application returns with your next "
                                             "sign-in")
                            : QStringLiteral("WarpManager: the WARP window was closed and its session unit "
                                             "masked — Muffin drives the tunnel from here"),
                        0);
    }
    m_guiChanged = false;
    m_guiRestoring = false;
}

int WarpManager::sweepWarpAutostart()
{
    int moved = 0;
    const QString configDirs =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("XDG_CONFIG_DIRS"));
    const QStringList dirs = autostartDirectories(QDir::homePath(), configDirs);

    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists())
            continue;
        const QStringList entries =
            dir.entryList(QStringList{ QStringLiteral("*.desktop") }, QDir::Files);
        for (const QString &entry : entries) {
            const QString filePath = dir.filePath(entry);
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly))
                continue;   // unreadable is not a match: deciding from the name alone is how the
                            // enrolment deep-link handler gets disabled instead of the tray program
            const QString contents = QString::fromUtf8(file.read(kAutostartReadChars));
            file.close();
            if (!autostartNamesWarpGui(entry, contents))
                continue;
            if (QFile::rename(filePath, dir.filePath(disabledAutostartName(entry)))) {
                ++moved;
                emit logMessage(QStringLiteral("WarpManager: %1 was moved aside, so the WARP window does "
                                                "not start with the session")
                                    .arg(filePath), 0);
                continue;
            }
            // The expected half of this on a normal machine: a system entry belongs to root and its
            // package owns it. Saying which file refused is the difference between "Muffin tried" and
            // "Muffin did nothing" — and no password is asked for a boot-time nicety, because the
            // per-session mask above already holds today, without root.
            emit logMessage(QStringLiteral("WarpManager: %1 autostarts the WARP window and is owned by "
                                           "the system, so it was left alone; the session mask keeps the "
                                           "window away for this session")
                                .arg(filePath), 1);
        }
    }
    return moved;
}

bool WarpManager::start(QString *error)
{
    if (!startVerb(connectArgs(), State::Connecting, true, error))
        return false;
    // The request was accepted, which is not the same as the tunnel being up — and the difference is
    // the exact shape of the failure this whole half was written for. Give it a deadline to say so.
    armConnectDeadline();
    // And take the vendor's window away for the same reason. This is the verb that produces it: the
    // daemon's status traffic activates com.cloudflare.WarpTaskbar, whose unit answers by mapping a
    // window titled "Cloudflare One Client" a second or two after the CLI has already said Success.
    // Asked here rather than after a poll reads Connecting, because by then the window has been on screen
    // for five seconds — a masked unit cannot be activated, so preventing it beats chasing it.
    suppressWarpGui();
    return true;
}

bool WarpManager::stop(QString *error)
{
    return startVerb(disconnectArgs(), State::Disconnecting, false, error);
}

bool WarpManager::refresh(QString *error)
{
    return startVerb(statusArgs(), m_state, false, error);
}

bool WarpManager::ensureRegistered(QString *error)
{
    // availableNow() rather than m_available: a cached "no client here" is the one answer this class
    // must not keep, because the thing that makes it wrong is an install this class does not run. See
    // availableNow() in the header.
    if (!availableNow()) {
        if (error)
            *error = QStringLiteral("WarpManager: warp-cli is not installed on this machine.");
        return false;
    }
    // Already on its way. The running sequence's registrationFinished() answers everyone who joined
    // it, and starting a second one would be a second `registration new` for one device — a second
    // key, and the first one orphaned.
    if (m_regRunning)
        return true;
    if (m_process) {
        // A poll or a connect holds the single verb slot. Enrolment is the one thing that cannot be
        // dropped when the slot is busy: skipping it means issuing a connect the daemon will never
        // complete, so this asks to run as soon as the verb in flight lands instead.
        m_regQueued = true;
        return true;
    }
    runRegistrationShow();
    return true;
}

void WarpManager::runRegistrationShow()
{
    m_regRunning = true;
    // The reading is what decides everything after it, so the sequence starts by asking rather than by
    // assuming: on a machine that is already enrolled — nearly every machine that gets here — this is
    // the only warp-cli call the whole enrolment step costs.
    QString err;
    if (!startVerb(registrationShowArgs(), m_state, false, &err, QByteArray(),
                   [this](const QString &out, int exitCode, bool crashed) {
                       onRegistrationShown(out, exitCode, crashed);
                   })) {
        finishRegistration(false, err);
    }
}

void WarpManager::onRegistrationShown(const QString &out, int exitCode, bool crashed)
{
    // A verb that died rather than exited has no answer in it, whatever its buffer managed to print
    // on the way down; that is Unknown, and Unknown is the reading the device is left alone for.
    const Registration r = crashed ? Registration::Unknown : parseRegistration(out, exitCode);
    setRegistration(r);

    if (r == Registration::Unknown) {
        finishRegistration(false,
                           out.isEmpty()
                               ? QStringLiteral("warp-cli registration show did not answer")
                               : out);
        return;
    }
    if (!needsEnrollment(r)) {
        // The ordinary case, and the one the check exists to protect: an enrolled device is left
        // exactly as it is. No delete, no new, no new key — the tunnel can be asked for.
        finishRegistration(true, QString());
        return;
    }

    if (needsDeleteFirst(r)) {
        // Loud on purpose: this is the only branch that throws something away, and what it throws
        // away is the machine's existing WARP identity. A user reading the journal later can see that
        // the old enrolment was replaced rather than newly created.
        emit logMessage(QStringLiteral(
                            "WarpManager: existing registration conflicts — replacing it with a new one"),
                        1);
        QString err;
        if (!startVerb(registrationDeleteArgs(), m_state, false, &err, termsAnswer(),
                       [this](const QString &o, int c, bool k) {
                           onRegistrationDeleted(o, c, k);
                       })) {
            finishRegistration(false, err);
        }
        return;
    }

    QString err;
    if (!startVerb(registrationNewArgs(), m_state, false, &err, termsAnswer(),
                   [this](const QString &o, int c, bool k) { onRegistrationNewed(o, c, k); })) {
        finishRegistration(false, err);
    }
}

void WarpManager::onRegistrationDeleted(const QString &out, int exitCode, bool crashed)
{
    if (crashed || exitCode != 0) {
        // The conflict is still in place because we did not get it out, so enrolling now would earn
        // the same conflict straight back. Stop with the device as it was found rather than half-way
        // through: the one state worse than a conflict is a delete that worked followed by a new that
        // did not, which leaves the machine with no enrolment at all.
        finishRegistration(false,
                           out.isEmpty()
                               ? QStringLiteral("warp-cli registration delete failed")
                               : out);
        return;
    }
    // Not a fresh reading — this is the result of our own delete — so the value goes in without a
    // registrationChanged() burst: the window has already said "setting WARP up" once for this
    // sequence, and saying it again for a transition we caused ourselves would read like a new problem.
    m_registration = Registration::NotRegistered;
    QString err;
    if (!startVerb(registrationNewArgs(), m_state, false, &err, termsAnswer(),
                   [this](const QString &o, int c, bool k) { onRegistrationNewed(o, c, k); })) {
        finishRegistration(false, err);
    }
}

void WarpManager::onRegistrationNewed(const QString &out, int exitCode, bool crashed)
{
    if (crashed || exitCode != 0) {
        // The enrolment did not happen. Nothing else is attempted and nothing else is claimed: the
        // device may or may not have a registration now, and the next check will read that rather
        // than assume this call's failure carried over.
        finishRegistration(false,
                           out.isEmpty()
                               ? QStringLiteral("warp-cli registration new failed")
                               : out);
        return;
    }
    // A zero exit from `registration new` is warp-cli saying it enrolled this machine, which is the
    // reading the tunnel is about to rely on. Set through the setter so the window hears the device
    // move out of the state it logged about: the sequence's registrationChanged() and its
    // registrationFinished() then arrive in the same order whether the device came in enrolled or was
    // enrolled by us.
    setRegistration(Registration::Registered);
    finishRegistration(true, QString());
}

void WarpManager::setRegistration(Registration r)
{
    if (r == m_registration)
        return;
    m_registration = r;
    emit registrationChanged(r);
}

void WarpManager::finishRegistration(bool ok, const QString &why)
{
    m_regRunning = false;
    m_regQueued = false;
    if (!ok)
        emit logMessage(QStringLiteral("WarpManager: %1").arg(why), 2);
    emit registrationFinished(ok);
    // The caller's slot may already have started the preparation from inside that signal. This is for
    // the other case — a preparation that queued itself before the enrolment ever ran — which would
    // otherwise be stranded by the sequence it was waiting for having finished without a pump.
    runQueuedPreparation();
}

void WarpManager::runQueuedRegistration()
{
    if (!m_regQueued || m_regRunning || m_process)
        return;
    m_regQueued = false;
    runRegistrationShow();
}

void WarpManager::runQueuedPreparation()
{
    if (!m_prepQueued || m_prepRunning || m_process)
        return;
    // Enrolment first, always: restarting the daemon in the middle of a `registration new` would cost
    // the key that call is in the process of obtaining. Staying queued is the answer, and
    // finishRegistration() pumps this again when the sequence ahead of it has answered.
    if (m_regRunning || m_regQueued)
        return;
    m_prepQueued = false;
    m_prepIndex = 0;
    runPreparationStep();
}

void WarpManager::runQueuedSequences()
{
    // One pump for both deferred sequences. Two callers each pumping only their own is how a
    // preparation queued behind an enrolment is forgotten the moment the enrolment finishes — which
    // leaves a connect issued without the MASQUE switch and the restart, the exact hang this half
    // exists to prevent.
    runQueuedRegistration();
    runQueuedPreparation();
}

// ---- the preparation sequence ----

bool WarpManager::prepareEnvironment(QString *error)
{
    m_prepSteps = preparationSteps(ZapretSource::hostPlatform());
    m_prepIndex = 0;
    m_prepRestarted = false;
    if (m_prepSteps.isEmpty()) {
        // Nothing to do is not a failure to do it. A host with no client to reconfigure answers
        // immediately and lets the caller on to its connect; the journal stays quiet, because a
        // sentence about a no-op at every tunnel start is noise that teaches nobody to read it.
        if (error)
            error->clear();
        emit preparationFinished(true);
        return true;
    }
    if (!availableNow()) {
        if (error)
            *error = QStringLiteral("WarpManager: warp-cli is not installed on this machine.");
        return false;
    }
    // Already on its way: joining the run is the right answer, and a second restart queued behind the
    // first would restart a service that has not had the chance to come back up yet.
    if (m_prepRunning)
        return true;
    if (m_process) {
        // A poll, a verb or the enrolment holds the slot. Deferring is not dropping.
        m_prepQueued = true;
        return true;
    }
    return runPreparationStep();
}

bool WarpManager::runPreparationStep()
{
    if (m_prepIndex >= m_prepSteps.size()) {
        startSettleWait();
        return true;
    }
    const PreparationStep step = m_prepSteps.at(m_prepIndex);
    m_prepRunning = true;

    QString err;
    QString program;
    QStringList args;
    if (step.privileged) {
        // Resolved here rather than stored in the plan, because "where is pkexec" is a fact about the
        // host at the moment the step runs, not about its platform.
        const QString target = QStandardPaths::findExecutable(step.program);
        const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        QString stepError;
        if (!composeServiceRestart(target, pkexec, PrivilegeHelper::isElevated(), &program, &args,
                                   &stepError)) {
            // Non-fatal by design: the protocol step has already run, and a host whose daemon is not
            // stuck connects anyway. Saying so once and continuing beats refusing a tunnel because one
            // of two fixes could not be applied — and beats a second prompt behind a refused first.
            emit logMessage(stepError, 1);
            ++m_prepIndex;
            return runPreparationStep();
        }
    } else {
        // warp-cli by the resolved path (an MSI install is off PATH), everything else by PATH — and a
        // program that is not there is skipped with a word, never guessed at another name for.
        program = step.program == warpCliProgram()
            ? (m_program.isEmpty() ? warpCliProgram() : m_program)
            : QStandardPaths::findExecutable(step.program);
        args = step.args;
        if (program.isEmpty()) {
            emit logMessage(QStringLiteral("WarpManager: %1 is not available on this host — skipping "
                                            "the %2 step").arg(step.program, step.id), 1);
            ++m_prepIndex;
            return runPreparationStep();
        }
    }

    setStage(step.id == QLatin1String("masque") ? Stage::Protocol
                                                : Stage::Service);
    const PreparationStep captured = step;
    if (!startStep(program, args, step.timeoutMs, m_state, /*wasConnectAttempt=*/false, &err,
                   QByteArray(),
                   [this, captured](const QString &out, int code, bool crashed) {
                       onPreparationStepAnswered(captured, out, code, crashed);
                   })) {
        finishPreparation(false, err);
        return false;
    }
    return true;
}

void WarpManager::onPreparationStepAnswered(const PreparationStep &step, const QString &out,
                                            int exitCode, bool crashed)
{
    const bool failed = crashed || exitCode != 0;

    if (step.id == QLatin1String("ipv6")) {
        // A read rather than a change, so its answer is a sentence and not a verdict. Reported
        // whatever it says: "could not tell" and "IPv6 is switched off here" are the two things a user
        // staring at a tunnel that will not come up can actually act on.
        const Ipv6 state = parseIpv6State(out);
        if (state == Ipv6::Disabled) {
            emit logMessage(QStringLiteral("WarpManager: this host has IPv6 switched off "
                                           "(net.ipv6.conf.all.disable_ipv6=1), which a MASQUE tunnel "
                                           "often needs. Muffin does not change a system-wide setting "
                                           "on your behalf; to try it: sudo sysctl -w "
                                           "net.ipv6.conf.all.disable_ipv6=0"), 1);
        } else if (state == Ipv6::Enabled) {
            emit logMessage(QStringLiteral("WarpManager: IPv6 is enabled on this host"), 0);
        } else {
            emit logMessage(QStringLiteral("WarpManager: could not tell this host's IPv6 state (%1)")
                                .arg(out.isEmpty() ? QStringLiteral("no output") : out), 1);
        }
    } else if (failed) {
        emit logMessage(QStringLiteral("WarpManager: the %1 step failed (%2, exit %3)")
                            .arg(step.id,
                                 out.isEmpty()
                                     ? (crashed ? QStringLiteral("killed or crashed")
                                                : QStringLiteral("no output"))
                                     : out)
                            .arg(exitCode),
                        step.fatal ? 2 : 1);
        if (step.fatal) {
            finishPreparation(false, QStringLiteral("the %1 step failed").arg(step.id));
            return;
        }
    } else if (step.id == QLatin1String("masque")) {
        emit logMessage(QStringLiteral("WarpManager: the tunnel protocol is set to %1")
                            .arg(tunnelProtocol()), 0);
    } else if (step.id == QLatin1String("service")) {
        // The one answer that changes what the end of the run owes: the daemon is back and has to be
        // given the moment it takes to re-open its control socket.
        m_prepRestarted = true;
        emit logMessage(QStringLiteral("WarpManager: %1 was restarted").arg(serviceUnit()), 0);
        // And the one answer that also brings the vendor's window back with it: a warp-svc that has just
        // come up re-activates the taskbar over the same bus name a connect does, so the mask has to be
        // re-asserted here rather than left to the connect that follows — which, on the fast path, may
        // never come at all.
        suppressWarpGui();
    }

    ++m_prepIndex;
    if (m_prepIndex >= m_prepSteps.size()) {
        startSettleWait();
        return;
    }
    runPreparationStep();
}

void WarpManager::setStage(Stage stage)
{
    if (stage == m_stage)
        return;
    m_stage = stage;
    emit stageChanged(stage);
}

void WarpManager::startSettleWait()
{
    // Only a run that actually restarted the service owes the wait: systemd reports a unit active
    // before warp-svc has re-opened its control socket, and a connect issued in that gap is answered by
    // nothing at all — the same silent failure this sequence exists to remove, reproduced by our own
    // fix. A host that skipped the step or had it refused is not waiting for a daemon that never moved,
    // so it is not held for 1.5 s either.
    if (!m_prepRestarted || !m_settleTimer) {
        finishPreparation(true, QString());
        return;
    }
    setStage(Stage::Waiting);
    m_settleTimer->start(serviceSettleMs());
}

void WarpManager::finishPreparation(bool ok, const QString &why)
{
    if (!m_prepRunning)
        return;   // already closed: preparationFinished() is promised exactly once per started run
    m_prepRunning = false;
    m_prepQueued = false;
    m_prepIndex = m_prepSteps.size();
    setStage(Stage::Idle);
    if (!ok && !why.isEmpty())
        emit logMessage(QStringLiteral("WarpManager: %1").arg(why), 2);
    emit preparationFinished(ok);
    // A sequence this one was holding ahead of, or that it made wait for the slot.
    runQueuedRegistration();
}

void WarpManager::armConnectDeadline()
{
    if (!m_connectDeadline)
        return;
    m_connectDeadline->start(kConnectDeadlineMs);
}

void WarpManager::onConnectDeadline()
{
    // The one case worth a sentence is the one the user cannot see: a connect that was accepted and
    // has not come up. Anything the daemon has already answered — connected, disconnected, error —
    // says itself, and repeating it here would be the app disagreeing with its own status line.
    if (m_state != State::Connecting)
        return;
    emit logMessage(QStringLiteral("WarpManager: the tunnel did not come up within %1 s of being "
                                   "asked to. The protocol was set to %2 and %3 was restarted, so "
                                   "what is left is that the provider is blocking WARP itself — "
                                   "check whether it reaches Cloudflare on this connection at all.")
                        .arg(kConnectDeadlineMs / 1000)
                        .arg(tunnelProtocol(), serviceUnit()),
                    2);
}

bool WarpManager::startVerb(const QStringList &args, State onAccepted, bool wasConnectAttempt,
                            QString *error, const QByteArray &stdinData, RawHandler raw)
{
    if (!availableNow()) {
        if (error)
            *error = QStringLiteral("WarpManager: warp-cli is not installed on this machine.");
        return false;
    }
    // The resolved path, not the bare name: on a Windows host that got WARP from the MSI, warp-cli
    // exists only in Program Files and PATH has never heard of it. rescanAvailability() put either
    // a PATH hit or that absolute path in m_program before anything could reach this line.
    return startStep(m_program.isEmpty() ? warpCliProgram() : m_program, args, kVerbTimeoutMs,
                     onAccepted, wasConnectAttempt, error, stdinData, std::move(raw));
}

bool WarpManager::startStep(const QString &program, const QStringList &args, int timeoutMs,
                            State onAccepted, bool wasConnectAttempt, QString *error,
                            const QByteArray &stdinData, RawHandler raw)
{
    if (program.isEmpty()) {
        if (error)
            *error = QStringLiteral("WarpManager: a preparation step named no program to run.");
        return false;
    }
    if (m_process) {
        if (error)
            *error = QStringLiteral("WarpManager: another warp-cli request is still running.");
        return false;
    }

    m_lastVerbWasConnect = wasConnectAttempt;
    m_pendingState = onAccepted;
    // Which of the two completion paths this verb's answer takes: a handler means the sequence owns
    // the raw output and exit code, no handler means the state machine reads it as a status. Set
    // before the process exists so the failure paths below can find it too.
    m_rawHandler = std::move(raw);

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(program);
    proc->setArguments(args);

    if (!stdinData.isEmpty()) {
        // Answer the terms-of-service prompt as soon as the child is up, then close the write
        // channel. Closing it is the half that matters: a warp-cli reading a console that stays open
        // waits for more input, and the only thing that would ever end that wait is our own timeout
        // killing the verb — mid-registration. Two bytes to a pipe either lands at once or means the
        // child is not reading stdin at all, which is the case this write is politely ignored by.
        connect(proc, &QProcess::started, this, [proc, stdinData]() {
            proc->write(stdinData);
            proc->waitForBytesWritten(1000);
            proc->closeWriteChannel();
        });
    }

    connect(proc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        onVerbFinished();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, program](QProcess::ProcessError e) {
        // FailedToStart is the only case that never produces finished(); every other error is
        // reported after the exit signal arrives. If we do not clear the pointer here a
        // wedged QProcess keeps every subsequent verb from starting at all.
        if (e != QProcess::FailedToStart)
            return;
        if (m_process == proc)
            m_process = nullptr;
        const QString text = proc->errorString();
        // Taken out before it runs, exactly as in onVerbFinished(): a sequence that cannot start its
        // verb has to be ended from here, or its caller would wait forever for a
        // registrationFinished() that no process is left to emit. There is no exit code for a program
        // that was never exec'd, so it gets the crashed answer and its own error text as the output.
        RawHandler failed;
        failed.swap(m_rawHandler);
        proc->deleteLater();
        if (failed) {
            m_lastVerbWasConnect = false;
            failed(text, -1, true);
            runQueuedSequences();
            return;
        }
        emit logMessage(QStringLiteral("WarpManager: %1 could not be started (%2)")
                            .arg(program, text), 2);
        emit errorOccurred(text);
        setState(State::Error);
    });

    m_process = proc;
    proc->start();
    // Report the intent immediately: warp-cli's connect verb returns in a fraction of a second
    // but the tunnel does not actually come up until the daemon finishes the handshake. Without
    // this transition the button would look like nothing had happened for the whole round trip.
    if (m_state != onAccepted)
        setState(onAccepted);

    // Bound the wait even though warp-cli is normally quick. A daemon that has hung would
    // otherwise strand us in Connecting forever.
    QTimer::singleShot(timeoutMs, proc, [this, proc]() {
        if (m_process != proc)
            return;   // the finished signal already beat us here
        proc->kill();
    });
    return true;
}

void WarpManager::onVerbFinished()
{
    if (!m_process)
        return;
    QProcess *proc = m_process;
    m_process = nullptr;

    const QString out = QString::fromUtf8(proc->readAll()).trimmed();
    const int exitCode = proc->exitCode();
    const bool crashed = proc->exitStatus() == QProcess::CrashExit;
    proc->deleteLater();

    // An enrolment step's answer belongs to its sequence, not to the state machine. Swapped out
    // rather than read, because the handler starts the next step's verb from inside here and that
    // needs the slot this function freed two lines above; and dispatched ahead of the exit-code test
    // below, because a `registration show` that exits non-zero is *evidence* — it is how warp-cli
    // reports an unenrolled device — whereas for a status probe a non-zero exit is a failure.
    RawHandler raw;
    raw.swap(m_rawHandler);
    if (raw) {
        m_lastVerbWasConnect = false;
        raw(out, exitCode, crashed);
        return;
    }

    if (crashed || exitCode != 0) {
        const QString reason = out.isEmpty()
            ? QStringLiteral("warp-cli exited with code %1").arg(exitCode)
            : out;
        emit logMessage(QStringLiteral("WarpManager: %1").arg(reason), 2);
        emit errorOccurred(reason);
        setState(State::Error);
        runQueuedSequences();
        return;
    }

    // A status probe's stdout is the only place the daemon's truth lives; connect/disconnect
    // produce a short acknowledgement that does not itself carry the state.
    const State parsed = parseStatus(out, m_lastVerbWasConnect);
    // If the parse said "Disconnected" but we just asked for a connect, that usually means the
    // daemon acknowledged the request and is still completing the handshake. Keep the pending
    // state so the button does not flip back to Idle for a run that is on its way up; the next
    // poll (five seconds later) reads the real answer.
    if (m_lastVerbWasConnect && parsed == State::Disconnected && m_pendingState == State::Connecting) {
        setState(State::Connecting);
    } else {
        setState(parsed);
    }
    m_lastVerbWasConnect = false;
    // The verb that made a deferred sequence wait its turn has landed. If either request is still
    // owed — nobody cancelled it, since the only thing that clears a flag is its sequence running to
    // its end — this is where it gets its turn.
    runQueuedSequences();
}

void WarpManager::setState(State s)
{
    if (s == m_state)
        return;
    // The deadline exists to name a connect that was accepted and then said nothing. Any answer that
    // arrives cancels it: Connected because the tunnel came up, Error and Disconnecting because the
    // daemon has said something more specific than silence, and the journal should carry that instead.
    if (m_connectDeadline
        && (s == State::Connected || s == State::Error || s == State::Disconnecting))
        m_connectDeadline->stop();
    m_state = s;
    emit stateChanged(s);
}

} // namespace zapret

