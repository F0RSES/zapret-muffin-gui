#include "WarpManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
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

} // namespace

QString WarpManager::warpCliProgram()
{
    return QStringLiteral("warp-cli");
}

bool WarpManager::isAvailableFromPath(const QString &resolvedPath)
{
    return !resolvedPath.isEmpty();
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
    // Kill any warp-cli still running: a status probe that outlives us would be a stray
    // subprocess with no owner. QProcess does the right thing when its parent is destroyed, but
    // the explicit kill makes the intent readable at a glance.
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void WarpManager::rescanAvailability()
{
    // PATH first, then the directory Cloudflare's own package writes to. The second half is not
    // decoration: the Windows MSI never edits PATH, so a machine that has the client installed
    // answers a PATH-only probe with nothing — and the interface would offer to install software
    // that is already there.
    const QString resolved = resolveExecutable(QStandardPaths::findExecutable(warpCliProgram()),
                                               installCandidatePaths(programFilesRoot()));
    const bool now = isAvailableFromPath(resolved);
    m_program = resolved;
    if (now == m_available)
        return;
    m_available = now;
    emit availabilityChanged(now);
    // The state a moment ago ("Unavailable") is not true any more, so say so. If WARP became
    // available the next refresh will replace this with the daemon's real answer; if it went
    // away, nothing can.
    setState(now ? State::Disconnected : State::Unavailable);
    if (now) {
        QString err;
        refresh(&err);
    }
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

