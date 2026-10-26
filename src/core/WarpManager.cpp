#include "WarpManager.h"

#include "ZapretSource.h"

#include <QCoreApplication>
#include <QDir>
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
        // A poll is not a user request — if a verb is already in flight, drop this tick rather
        // than queueing a second warp-cli behind it and having the pair disagree about the state.
        if (m_process)
            return;
        QString err;
        refresh(&err);
    });

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

    // PATH first, then the directories the client's own packages write to. The second half is not
    // decoration: the Windows MSI never edits PATH, a snap's command lives under /snap/bin, and a
    // user-scoped flatpak export is on nobody's PATH at all — so a machine that has the client
    // installed answers a PATH-only probe with nothing, and the interface offers to install software
    // that is already there.
    QStringList candidates = installCandidatePaths(programFilesRoot());
    candidates += extraCandidatePaths(QDir::homePath());
    const QString resolved = resolveExecutable(QStandardPaths::findExecutable(warpCliProgram()),
                                              candidates);
    m_program = resolved;
    setAvailable(isAvailableFromPath(resolved));

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

bool WarpManager::start(QString *error)
{
    return startVerb(connectArgs(), State::Connecting, true, error);
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
    if (!m_available) {
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
}

void WarpManager::runQueuedRegistration()
{
    if (!m_regQueued || m_regRunning || m_process)
        return;
    m_regQueued = false;
    runRegistrationShow();
}

bool WarpManager::startVerb(const QStringList &args, State onAccepted, bool wasConnectAttempt,
                            QString *error, const QByteArray &stdinData, RawHandler raw)
{
    if (!m_available) {
        if (error)
            *error = QStringLiteral("WarpManager: warp-cli is not installed on this machine.");
        return false;
    }
    if (m_process) {
        if (error)
            *error = QStringLiteral("WarpManager: another warp-cli request is still running.");
        return false;
    }

    m_lastVerbWasConnect = wasConnectAttempt;
    m_pendingState = onAccepted;
    // Which of the two completion paths this verb's answer takes: a handler means the enrolment
    // sequence wants the raw output and exit code, no handler means the state machine reads it as a
    // status. Set before the process exists so the failure paths below can find it too.
    m_rawHandler = std::move(raw);

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    // The resolved path, not the bare name: on a Windows host that got WARP from the MSI, warp-cli
    // exists only in Program Files and PATH has never heard of it. rescanAvailability() put either
    // a PATH hit or that absolute path in m_program before anything could reach this line.
    proc->setProgram(m_program.isEmpty() ? warpCliProgram() : m_program);
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
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
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
            runQueuedRegistration();
            return;
        }
        emit logMessage(QStringLiteral("WarpManager: warp-cli could not be started (%1)")
                            .arg(text), 2);
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
    QTimer::singleShot(kVerbTimeoutMs, proc, [this, proc]() {
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
        runQueuedRegistration();
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
    // The verb that made an enrolment wait its turn has landed. If the request that queued itself is
    // still owed — nobody cancelled it, since the only thing that clears the flag is the sequence
    // running to its end — this is where it gets its turn.
    runQueuedRegistration();
}

void WarpManager::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged(s);
}

} // namespace zapret

