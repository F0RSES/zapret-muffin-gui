#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;
class QTimer;

namespace zapret {

// Drives the Cloudflare WARP client through its own `warp-cli` binary.
//
// WARP is deliberately NOT an IZapretBackend. A backend owns one child process from launch to
// teardown and reports its exit; WARP's tunnel lives in a system daemon the user installed
// separately, and `warp-cli` is a short-lived control client that exits seconds after every verb.
// Reusing the backend contract here would either have to lie about "the process died" every time
// warp-cli exits normally, or keep a phantom process object alive for a run that is really a
// daemon's — both are the sort of thing that quietly breaks the power button's state machine.
//
// The manager therefore owns only:
//   * availability — is warp-cli on PATH?
//   * the current state, learned from `warp-cli --accept-tos status`
//   * one in-flight verb (connect / disconnect / status), issued asynchronously
//
// Everything on the CLI itself is delegated through QProcess, and the whole class is designed to
// be testable without the daemon: the argv the verbs run, the way the status answer is parsed,
// and the fallback when warp-cli is absent are all static functions a headless suite can call
// directly.
//
// The instance surface is virtual as well, and that is for the half of testing the statics cannot
// reach. What the statics prove is what one verb does with one answer; what they cannot prove is
// that a *caller* asks at the right moment — that a window which means to bring the tunnel up
// settles the enrolment first, and does not connect behind an answer that said no. Proving that
// needs a control the suite can hand answers out of, and no daemon to hand them from. So the verbs,
// the state and the availability are virtual, a suite subclasses this class and counts the calls,
// and MainWindow takes its control from outside — see its constructor's injected form.
//
// Virtual only on the instance methods, never on the statics: a parser a test calls directly is
// already a seam, and a virtual static would be a second way to be wrong about which answer the
// running object means.
//
// Threading: like every other QObject here, WarpManager belongs to the GUI thread's event loop
// and is not internally locked.
class WarpManager : public QObject
{
    Q_OBJECT

public:
    // Coarse lifecycle state, chosen to map 1:1 onto the three looks the power button wears plus
    // one "there is nothing to press" case a backend never needs.
    enum class State
    {
        Unavailable,   // warp-cli is not installed on this host — the toggle cannot be used
        Disconnected,  // warp-cli answers, and the tunnel is down
        Connecting,    // a connect() is in flight, or the daemon says it is coming up
        Connected,     // the tunnel is up
        Disconnecting, // a disconnect() is in flight
        Error          // the last verb failed; errorOccurred carries the reason
    };
    Q_ENUM(State)

    // How far this machine has got with Cloudflare, read from `warp-cli registration show`.
    //
    // This is a different axis from State, not a finer grade of it. A client can be enrolled with
    // its tunnel down — the normal idle look, Registered beside Disconnected — and it can be
    // *unenrolled*, which is the one case State cannot express: the daemon accepts a connect, tries,
    // and never comes up, and then says the same "not registered" line on every poll that it said
    // the first time. Folding that into State would mean either a sixth button colour for something
    // the power button cannot act on, or reading an unenrolled daemon as Disconnected and letting
    // connect() fail forever. So the enrolment question keeps its own name and its own answer.
    //
    // Unknown is the default and a real member, not a placeholder for "not done yet". Enrolling a
    // device asks Cloudflare for a key that identifies this machine, and the Conflict case goes
    // further — it deletes the key the user already has. Neither is something to do on a guess, so a
    // reading the manager cannot parse leaves the device exactly as it found it. A crashed verb, a
    // failed `registration show`, and output that names nothing recognisable all land here.
    enum class Registration
    {
        Unknown,        // the answer was not readable — leave the enrolment alone
        Registered,     // there is an enrolment; a connect can be asked for as it stands
        NotRegistered,  // never enrolled, or the enrolment was deleted — enrol it
        Conflict        // an enrolment the daemon will not use — delete it, then enrol
    };
    Q_ENUM(Registration)

    explicit WarpManager(QObject *parent = nullptr);
    ~WarpManager() override;

    // ---- availability ---------------------------------------------------------
    // The name of the control program on this host. Kept as one word ("warp-cli") rather than an
    // absolute path: warp-cli lives under Program Files on Windows and /usr/bin elsewhere, and
    // QStandardPaths::findExecutable resolves the same name on both.
    static QString warpCliProgram();

    // Whether warp-cli resolves on this host right now. Testable form: pass the PATH-resolved
    // path — non-empty means available. The instance method caches the last answer so an
    // unavailable state does not re-probe PATH on every repaint.
    //
    // Virtual because availability is the answer a window branches on — a client that is not there
    // is an offer to install one, a client that is there is a tunnel to drive — and a suite that
    // wants to watch which branch it took cannot manufacture a warp-cli on a machine that has none.
    virtual bool isAvailable() const { return m_available; }
    static bool isAvailableFromPath(const QString &resolvedPath);

    // Whether the WARP client is on this machine — the question a caller asks before offering to
    // install it, and the same fact availability answers. Two names for one flag on purpose, not two
    // probes: "can I drive the tunnel?" and "is there a tunnel to drive?" have exactly one source of
    // truth (a warp-cli we can find), and a second lookup that could disagree with the first would
    // be a toggle that offers to install software the machine already has.
    //
    // A mock that overrides these two must give them the same answer, for the same reason: they are
    // one flag read under two names, and a control that says "there is a client" to one caller and
    // "there is none" to another is the disagreement the two names exist to avoid.
    virtual bool isInstalled() const { return m_available; }

    // The Program Files root this process was started with, or empty on every other platform. Read
    // from the environment rather than hardcoded, exactly as ZapretPaths::systemInstallDir() does:
    // the value is the one the OS wrote for this process, and a machine can keep Windows somewhere
    // other than C:\. Public because WarpInstaller asks the same question to check whether the
    // install it just ran put a client on this disk.
    static QString programFilesRoot();

    // The one directory Cloudflare's own Windows package writes to, given the Program Files root:
    // "<root>/Cloudflare/Cloudflare WARP/warp-cli.exe". Empty root means no candidates, which is
    // what every non-Windows host passes — there the package manager installs into /usr/bin, and a
    // PATH lookup already names it. Pure composition so a Linux runner can assert the Windows
    // spelling, the way WindowsArgs and ZapretPaths::systemInstallDirUnder() do.
    //
    // This exists because the MSI never touches PATH: a Windows machine with WARP installed answers
    // QStandardPaths::findExecutable("warp-cli") with nothing, and a PATH-only probe would call that
    // machine empty and offer to install the client it already has.
    static QStringList installCandidatePaths(const QString &programFilesRoot);

    // PATH's answer first (already resolved by the caller), then the candidates that are real files.
    // The first hit is the program every verb runs, so an off-PATH install is still drivable.
    static QString resolveExecutable(const QString &pathHit, const QStringList &candidates);

    // ---- the argv each verb runs ---------------------------------------------
    //
    // Every one carries `--accept-tos` because warp-cli refuses to do anything at all without it
    // (and, in the words of its own man page, "prompts interactively" — which is exactly the
    // wrong behaviour for a GUI with nobody watching a hidden console). Split out so a test can
    // assert "the connect call is what we think it is" without needing warp-cli at all.
    static QStringList statusArgs();
    static QStringList connectArgs();
    static QStringList disconnectArgs();

    // The enrolment verbs, spelled the way warp-cli names them: the subcommand is `registration`,
    // not `register`, and `show`/`new`/`delete` are its three forms on both platforms. `--accept-tos`
    // rides first here for the same reason as everywhere else — without the flag the CLI will not
    // act at all — and the enrolment pair additionally gets an answer on stdin, because some builds
    // ask for the terms of service on a console nobody is watching; see termsAnswer().
    static QStringList registrationShowArgs();
    static QStringList registrationNewArgs();
    static QStringList registrationDeleteArgs();

    // ---- the parser -----------------------------------------------------------
    //
    // warp-cli's status output varies by version and platform: a Linux 2023.09 answer reads
    // "2024-... warp-cli [INFO] Connected", a Windows one writes "Status: Connected", and an
    // unregistered client says either "Disconnected" or "Not registered". What every flavour
    // agrees on is that the state word appears in the text, so this scans for the recognised
    // markers in a fixed order and falls back to Disconnected when nothing matches — a status
    // the manager cannot read is a status it should not claim.
    //
    // `wasConnectAttempt` is what makes "WARP is not registered, or is otherwise unavailable"
    // read as Connecting rather than Error when it appears during a verb we ourselves just ran;
    // the same text on a bare refresh means the daemon has nothing to say, which is closer to
    // "Disconnected" than to a failure of ours.
    static State parseStatus(const QString &raw, bool wasConnectAttempt = false);

    // ---- the enrolment parser --------------------------------------------------
    //
    // `registration show` has no machine-readable output format, and what it prints depends on the
    // answer: an enrolled client prints a block of `Key:` / `Account type:` / `Mode:` lines, an
    // unenrolled one prints an error — "Client registration does not exist!" on current Linux builds,
    // "Device is not enrolled" on older ones and on Windows — and a conflicting one prints a 409 in
    // wording that has moved twice. So this looks for the markers that have survived, in the order
    // the collisions force: "not enrolled" contains "enrolled" and "not registered" contains
    // "registered", so every negative has to be tested before any positive, for exactly the reason
    // parseStatus() tests Disconnected before Connected.
    //
    // `exitCode` is evidence, not a veto. The unenrolled answer *comes with* a non-zero exit — that
    // is how warp-cli reports it — so a readable negative is a real answer whatever the code says.
    // Only the positive side requires a clean exit, because the fields a successful `show` prints are
    // the sole thing that proves the daemon actually handed a registration back rather than echoing a
    // failed lookup. Zero exit and nothing recognisable is still Unknown: saying "registered" about
    // text we did not read is how a device gets re-enrolled over the user's objection.
    static Registration parseRegistration(const QString &raw, int exitCode);

    // Whether a reading means `registration new` has to run. Registered needs nothing and Unknown is
    // deliberately excluded — the honest answer to a device you cannot read is to do nothing to it,
    // because the alternative is asking Cloudflare for a second key or throwing away the first.
    static bool needsEnrollment(Registration r);

    // Whether the enrolment has to be taken out before a new one can go in. Only Conflict does:
    // Cloudflare will not issue a second key to a device whose first it still considers live, and
    // `registration new` over a conflict answers with the same conflict again.
    static bool needsDeleteFirst(Registration r);

    // The bytes written to an enrolment verb's stdin before its write channel is closed.
    //
    // `--accept-tos` covers the builds that read the flag, and not every build does: the Windows MSI
    // and older Linux ones ask "Do you accept the terms of service?" on their console. From a GUI
    // nobody sees that console, and a warp-cli waiting on it sits until our own timeout kills it —
    // mid-registration, with a key half-issued, which is the worst moment in the whole sequence to be
    // interrupted. Answering `y` covers the prompt where it appears and is ignored where it does not,
    // so both platforms take one code path. Lowercase `y` plus a newline is what the prompt accepts.
    static QByteArray termsAnswer();

    // ---- current view ---------------------------------------------------------
    // Virtual for the same reason the verbs below are: the state is what a caller reads to decide
    // what a press means, and a suite that wants to watch that decision has to be able to hand out a
    // state without owning a daemon that is in it.
    virtual State state() const { return m_state; }

    // The last enrolment reading, which is Unknown until a check has answered. A caller may read it
    // to decide whether to mention enrolment at all; it is not a substitute for ensureRegistered(),
    // which re-reads the daemon rather than trusting what was seen last time.
    virtual Registration registrationState() const { return m_registration; }

    // ---- verbs ----------------------------------------------------------------
    // Each returns false when the operation could not even be started (warp-cli absent, or a
    // verb already in flight) and fills *error then; a successful return only means the request
    // was accepted, not that the tunnel moved. Completion lands through stateChanged.
    //
    // Named start/stop rather than connect/disconnect for two reasons: `connect` is a QObject
    // member, so a subclass that spells it that way shadows the base and every unqualified
    // `connect(...)` inside this file resolves to the wrong overload; and the rest of the
    // application already speaks start/stop about a tunnel — the power button, ZapretManager,
    // the tray menu — so a second vocabulary here would be a translation table waiting to
    // drift.
    //
    // Virtual so a caller's ordering is checkable. The claim worth testing is not that `connect`
    // reaches warp-cli — the statics above already pin the argv — but that a window asks for it
    // *after* the enrolment answered and not before; that is only observable by counting the asks.
    virtual bool start(QString *error = nullptr);
    virtual bool stop(QString *error = nullptr);
    virtual bool refresh(QString *error = nullptr);

    // Make sure this machine has a WARP enrolment, without disturbing one it already has.
    //
    // Why it exists: warp-cli will happily accept `connect` from a client that was never enrolled.
    // The daemon takes the request, fails to find a registration, and reports the same "not
    // registered" line on every poll — so the tunnel never comes up and the button never says why.
    // Bringing the tunnel up therefore has to be preceded by making sure there is something to bring
    // up. This is the background half of that: no dialog, no console for the user to watch.
    //
    // What it runs: `registration show`, then — only if that reading asks for it — `registration
    // delete` (Conflict) and `registration new`. Each step is a short-lived warp-cli on this
    // object's single in-flight verb slot, so a status poll that fires mid-sequence is dropped
    // exactly as it is during a connect, and the steps never overlap. Nothing here is synchronous:
    // `registration new` talks to Cloudflare and can take seconds, and the entire point of doing it
    // in the background is that the GUI thread is not sitting in waitForFinished() while it does.
    //
    // How it answers: registrationChanged(reading) as soon as the device has been read, then
    // registrationFinished(ok) once at the end. `ok` is true when the device is enrolled — either it
    // already was, or this sequence just enrolled it — and false when it could not be, including the
    // Unknown case: refusing to act on a device we cannot read is a failure to prepare, not a silent
    // success, and the caller must not bring a tunnel up behind it.
    //
    // The return value is only about starting. False means the sequence could not be begun at all
    // (warp-cli is absent) and *error then says so; true means it was started, or was already running
    // and this call joined it — either way registrationFinished will answer. Asking twice is safe: a
    // second `registration new` would be a second key for one device.
    //
    // The one method here that a window has to be proved to call, and the reason the instance surface
    // is virtual at all: a connect issued while this was still owed an answer is the silent hang the
    // method exists to prevent, and no static parser can see that mistake.
    virtual bool ensureRegistered(QString *error = nullptr);

    // Re-probe availability. Called on construction; a caller who just installed WARP while the
    // window was open can ask for the same effect without a full restart.
    virtual void rescanAvailability();

    // Cadence-driven polling. Off by default; MainWindow turns it on only while a mode is
    // active, because a status probe on a machine with no warp-cli is a wasted fork every tick.
    // Virtual because it arms a timer, and a mock that let the base arm one would have a poll
    // firing out from under the test that was counting calls.
    virtual void setPollingEnabled(bool on);
    virtual bool isPollingEnabled() const { return m_pollEnabled; }
    // How often a poll fires while enabled. Exposed so a test can pin the cadence without
    // reaching into the private timer.
    static constexpr int kPollIntervalMs = 5000;

signals:
    void stateChanged(State s);
    void availabilityChanged(bool available);
    // The enrolment reading, each time a check has read the device. Always emitted before the
    // registrationFinished() that follows it, and specifically so a window can say "setting WARP up"
    // at the moment enrolling becomes necessary rather than after it has already happened.
    void registrationChanged(Registration r);
    // Whether the device is enrolled now. See ensureRegistered() for what each answer means and for
    // the guarantee that exactly one of these closes every sequence the caller was told it started.
    void registrationFinished(bool ok);
    // Mirror of IZapretBackend's channel — the log line format the window already understands.
    // level: 0=info, 1=warn, 2=error
    void logMessage(const QString &line, int level);
    void errorOccurred(const QString &message);

private:
    // What an in-flight verb hands back when it belongs to an enrolment sequence instead of to the
    // state machine: the merged output, the exit code, and whether the child died rather than
    // exited. A verb that never started at all reports an empty output, -1 and crashed, because
    // there is no code for a program that was never exec'd and the sequence still has to be ended.
    using RawHandler = std::function<void(const QString &output, int exitCode, bool crashed)>;

    bool startVerb(const QStringList &args, State onAccepted, bool wasConnectAttempt,
                   QString *error, const QByteArray &stdinData = QByteArray(),
                   RawHandler raw = nullptr);
    void onVerbFinished();
    void setState(State s);

    // The three steps of ensureRegistered(), each one started from the previous one's answer.
    void runRegistrationShow();
    void onRegistrationShown(const QString &out, int exitCode, bool crashed);
    void onRegistrationDeleted(const QString &out, int exitCode, bool crashed);
    void onRegistrationNewed(const QString &out, int exitCode, bool crashed);
    void setRegistration(Registration r);
    // Ends a sequence: clears the running flag and emits registrationFinished exactly once, logging
    // `why` when the answer was no. Idempotent enough to be called from a step that failed to start.
    void finishRegistration(bool ok, const QString &why);
    // Starts an enrolment that was asked for while some other verb held the slot, if one is waiting.
    void runQueuedRegistration();

    State m_state = State::Unavailable;
    bool m_available = false;
    // The program every verb runs: warp-cli as PATH names it, or the absolute path of the copy in the
    // vendor's own install directory. Empty when there is no client here at all — which is the same
    // fact m_available caches, kept as the path rather than the bool because a verb has to exec
    // something, and a bool cannot be handed to QProcess.
    QString m_program;
    bool m_pollEnabled = false;
    QProcess *m_process = nullptr;   // the in-flight verb, non-null only while it runs
    QTimer *m_pollTimer = nullptr;
    // Whether the verb currently in flight is the one that could legitimately read an unknown
    // status as Connecting rather than Disconnected; see parseStatus().
    bool m_lastVerbWasConnect = false;
    // The state we expect the running verb to leave the daemon in, so a parse that returns
    // "Disconnected" while a connect() is still settling can be reported as Connecting.
    State m_pendingState = State::Disconnected;

    // The last enrolment reading, Unknown until a check answers. Cached so a caller can ask what the
    // device looks like without spawning warp-cli again; deliberately not used to skip the check,
    // because a registration the manager only *thinks* it saw is a registration it will not re-read.
    Registration m_registration = Registration::Unknown;
    // Set while the in-flight verb belongs to an enrolment step rather than to the state machine.
    // Taken out (swapped, not merely read) before it runs, because the handler starts the next verb
    // and needs the slot the finished signal has just freed.
    RawHandler m_rawHandler;
    // True for the whole show -> delete -> new sequence, so a second ensureRegistered() joins the
    // run in flight instead of queueing a duplicate enrolment behind it.
    bool m_regRunning = false;
    // Set when enrolment was asked for while a poll or a connect held the verb slot. The sequence
    // starts as soon as that verb lands, because enrolment is the one thing that must not simply be
    // dropped: a connect issued without it is a connect that never comes up.
    bool m_regQueued = false;
};

} // namespace zapret
