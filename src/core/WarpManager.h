#pragma once

#include "Strategy.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <QList>

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
//   * availability — is there a warp-cli this process can run?
//   * installation — is the client on this machine even when nothing runnable was found: its own
//     version answer, the package databases (dpkg, rpm, snap, flatpak), the Windows service and the
//     Windows registry. This is the question that has to be answered *before* an install is offered,
//     because an install over a client the machine already has is at best a waste and at worst the
//     removal of somebody's tunnel.
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

    // Which part of the preparation the tunnel is currently waiting on, announced as it starts.
    //
    // This is a third axis, beside State and Registration, and it exists for one reason: the steps
    // take seconds, one of them asks the user for root, and a tunnel half that is lit while nothing
    // visibly happens is read as a frozen program. The stage is what lets the window say "restarting
    // the WARP service" instead of saying nothing.
    //
    // It carries no verdict. Idle is where a host that needed nothing leaves it and where a finished
    // run lands; the answer to "did it work" is preparationFinished(), and the answer to "is the
    // tunnel up" is state().
    enum class Stage
    {
        Idle,      // nothing is being prepared — before the first step and after the last
        Protocol,  // switching the tunnel to MASQUE
        Service,   // restarting warp-svc, which is the step that can ask for root
        Waiting    // the service is back and the daemon needs a moment before it will answer
    };
    Q_ENUM(Stage)

    // One command of prepareEnvironment(), spelled as data for the same reason installationProbes()
    // is: the whole policy of what a tunnel needs before it can be asked to connect can then be read
    // at once, pinned by a test on a host that has none of these binaries, and extended without
    // touching the runner.
    struct PreparationStep
    {
        QString id;          // "masque" | "ipv6" | "service" — names the step in the journal and in the stage
        QString program;     // "warp-cli" | "sysctl" | "systemctl" — resolved by the runner, never guessed
        QStringList args;    // the question already split; no shell composes any of this
        bool privileged = false;  // needs root, so the program is prefixed with pkexec (unless we have it)
        bool fatal = false;       // a failure of this one ends the preparation instead of continuing
        int timeoutMs = 0;        // a step that never answers must not hold the tunnel hostage
    };

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

    // The path availability resolved, or the bare program name when it came from PATH. Published because
    // a caller that runs warp-cli itself has to run the SAME one: on a Windows host that got the client
    // from the MSI it exists only in Program Files, and a second lookup by bare name would find nothing
    // and report a client missing that this manager is successfully driving.
    //
    // Virtual with the rest of the instance surface, and for the same reason: a suite that hands a window
    // a control cannot manufacture a warp-cli on a machine that has none, and "which path did the rescan
    // find" is exactly the answer a caller puts in front of a user.
    virtual QString resolvedProgram() const { return m_program; }

    // What a client we managed to run said about itself. The three answers are the whole point of
    // running it: a binary that exits cleanly is here and usable, one that dies or reports it cannot
    // load is a broken install, and anything else is a reading that does not license either verdict.
    //
    // Unreadable exists because a non-zero exit is not automatically damage. warp-cli's own wording
    // for a flag it does not know ("unrecognized option") comes with exit 1 from a perfectly healthy
    // binary, and calling that broken would take a working tunnel away from a user on the strength of
    // an argument-spelling difference. Unknown readings therefore leave the machine as found — the
    // same rule Registration::Unknown applies to an unreadable enrolment.
    enum class Probe
    {
        Usable,       // it ran and answered: this is a client to drive
        Broken,       // it died, or said it could not load: an install to repair, not to drive
        Unreadable    // an answer that names neither: leave the verdict as it stood
    };
    Q_ENUM(Probe)

    // One package database's question about the client: which tool to ask, with which argv, and what
    // a line naming the client has to contain. Data rather than code so the whole list can be read at
    // once, pinned by a test, and answered without a runner that happens to own apt.
    struct InstallationProbe
    {
        QString source;       // how the answer gets named in the journal: "dpkg", "snap", "service"
        QString program;      // the tool to run; a tool this host does not have is skipped, not guessed
        QStringList args;     // the question, already split — no shell composes any of this
        QStringList markers;  // a line that names the client contains one of these, case-insensitive
    };

    // Whether the WARP client is on this machine, which is the question a caller has to ask before
    // offering to install it — and, deliberately, a broader question than "can I drive the tunnel?".
    // Three things answer yes:
    //
    //   * a warp-cli this process can run (which is also isAvailable()),
    //   * a package database that says the client is installed — snap, flatpak, an rpm box whose PATH
    //     the vendor never touched, a Windows machine whose registry and service both know a client
    //     no candidate path named,
    //   * an answer that has not arrived yet. A machine the manager is still asking is a machine it
    //     must not call empty: the premature offer is the exact failure this lookup exists to prevent.
    //
    // The relation between the two names is therefore a subset, not an identity: drivable implies
    // installed, and installed does not imply drivable — the packaged-but-unrunnable machine is the
    // reason the second question exists at all. A mock that overrides both keeps that subset, or it
    // is a toggle offering to install a client it claims it can already drive.
    virtual bool isInstalled() const { return m_available || m_packaged || m_probing; }

    // Whether something is here that is not working: a client that ran and reported damage, or a
    // package database that names the client as removed-but-config-files-present. The difference this
    // exists to make visible is between "nothing to install yet" and "a previous attempt left debris",
    // because only the second one deserves an offer that says it will clean up first.
    virtual bool installLooksBroken() const { return m_broken; }

    // Which source answered yes to isInstalled() — "dpkg", "snap", "registry", … — or empty when the
    // answer came from a runnable warp-cli or there is no client here. A caller writing "WARP is
    // already installed, using it" needs to say where "it" is, or the line reads as a guess.
    virtual QString installedVia() const { return m_packagedVia; }

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

    // The same question for a client the vendored installer did not put there. A snap has its own
    // bin directory that no PATH-editing package touches, a flatpak exports its commands elsewhere
    // again, and a per-user Windows install lives under the profile rather than under Program Files.
    // Split from installCandidatePaths() because that one is pinned against WarpInstaller's own
    // directory by a test — the two lists are owned by different code and must not drift together.
    static QStringList extraCandidatePaths(const QString &homePath);

    // PATH's answer first (already resolved by the caller), then the candidates that are real files.
    // The first hit is the program every verb runs, so an off-PATH install is still drivable.
    static QString resolveExecutable(const QString &pathHit, const QStringList &candidates);

    // The one verb that is not a request about the tunnel: what the client says its own version is.
    // Spelled without `--accept-tos`, unlike every verb below, because printing a version needs no
    // daemon and accepts no terms — and because a flag this tool might not know in some build is the
    // reason classifyVersionAnswer() distinguishes "damaged" from "did not like being asked".
    static QStringList versionArgs();

    // Read a client's own answer about itself. `crashed` means it died rather than exited — including
    // death by our own timeout, which is the same fact to a GUI: nothing is going to come of it.
    static Probe classifyVersionAnswer(const QString &output, int exitCode, bool crashed);

    // Every database this platform keeps its packages in, in the order they are asked. The list is
    // the whole policy: an unknown platform asks nothing, a tool missing from the host is skipped when
    // the round runs, and the first database that says "installed" ends the round.
    static QList<InstallationProbe> installationProbes(Strategy::Platform platform);

    // Whether one database's answer says the client is installed. Exit code is evidence and not a
    // veto, exactly as in parseRegistration(): `rpm -q` and `snap list` both answer a package they do
    // not have with a sentence *and* a non-zero code, and dpkg prints a table headed by two status
    // characters whose first column is the only thing that separates "installed" from "removed but
    // still holding its config files".
    static bool probeAnswerNamesInstall(const InstallationProbe &probe, const QString &output,
                                        int exitCode, bool crashed);

    // Whether the same answer describes debris instead: a package removed with its configuration left
    // behind. Only dpkg reports that state distinctly ("rc" in the status column), and it is the
    // Linux half of what installLooksBroken() is for.
    static bool probeAnswerNamesLeftovers(const InstallationProbe &probe, const QString &output,
                                          int exitCode, bool crashed);

    // The Windows registry keys that name the client, in the order they are tried: the vendor's own
    // key first, its 32-bit mirror next (the MSI is 32-bit and a 32-bit view is where its values
    // land), and the uninstall entries Windows Installer writes for any product last. Paths rather
    // than a yes/no so the spelling is checkable from a Linux runner.
    static QStringList registryKeys();

    // Whether a value read out of those keys proves an install. A version string is the clean answer;
    // the bare existence of values under the vendor's key counts too, because an MSI that got as far
    // as writing any of them has copied the client onto the disk.
    static bool registryNamesInstall(const QString &displayVersion, bool keyHasValues);

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

    // ---- the preparation argv, spelled once ----------------------------------
    //
    // What a WARP tunnel needs before `connect` can be asked for on a host whose provider interferes
    // with the default transport. Both halves were learned from a machine that could not otherwise
    // come up, and both are cheap enough to run unconditionally rather than to guess about:
    //
    //   * `tunnel protocol set MASQUE` — the protocol that survives where the WireGuard handshake
    //     does not. warp-cli accepts the command on a host already set to MASQUE and says so; setting
    //     it again is not a change, so nothing here has to read the current value first.
    //   * `systemctl restart warp-svc` — the daemon keeps a stale state when it came up while a zapret
    //     queue was already rewriting the routing, and then answers every connect without ever
    //     connecting. Only a restart clears it, which is why this runs before every tunnel start
    //     rather than after a failed one.

    // The protocol verb, on warp-cli's own slot: `tunnel protocol set <name>`, carrying --accept-tos
    // like every other verb here. MASQUE is named by one function so the runner, the journal and the
    // test cannot each spell it slightly differently.
    static QStringList protocolArgs();
    static QString tunnelProtocol();

    // The service restart, without the privilege prefix: what `systemctl` is asked to do.
    static QStringList serviceRestartArgs();
    static QString serviceUnit();

    // How long the daemon is given to come back after that restart before anything asks it for
    // something. Public because the setup wizard restarts the same unit and then runs `warp-cli connect`
    // as its next command: the gap is a fact about warp-svc, not about either caller, and a second
    // constant on the other side of it is a second opinion that will drift. Timed, never slept.
    static int serviceSettleMs();

    // ---- the daemon's own start, spelled once per service manager ----
    //
    // Every warp-cli verb is a client talking to warp-svc over a local socket, so a daemon that is not
    // running refuses all of them — including the enrolment the setup wizard has to settle before it can
    // do anything else. A package manager putting the unit on the disk is not a promise that it was
    // started, and a machine that had it stopped by hand does not start it again by itself.
    //
    // systemd's answer is one idempotent verb: `enable --now` starts a unit that is stopped, leaves a
    // unit that is running alone, and keeps it up across a boot besides — a client that is started but
    // not enabled comes back down at the next login and takes the tunnel with it, which is the same bug
    // reported a week later with no wizard anywhere in it.
    static QStringList systemdEnableStartArgs();
    // A service control manager has no `--now` and no idempotent start: `sc start` on a service the MSI
    // already started refuses with 1056. Windows' half is therefore a query the journal can quote the
    // state of, plus a start the plan marks non-fatal — see SetupCommand::fatal.
    static QStringList windowsServiceQueryArgs();
    static QStringList windowsServiceStopArgs();
    static QStringList windowsServiceStartArgs();

    // Whether one of warp-cli's answers says the daemon was not there to be asked. Worth reading the
    // client's own wording literally, because this is the one refusal whose cause a user can fix with a
    // single command: "Missing registration. Try running: 'warp-cli registration new'" followed by
    // "Maybe the daemon is not running?" is what a healthy client with a dead daemon prints, and taking
    // the first of those two lines at its word would have the interface offer an enrolment that cannot
    // be issued. Pure and static for the reason every parser here is: the text arrives from a vendor's
    // build tree, so a runner has to be able to hand over the transcript of a machine whose daemon is
    // down without having to reproduce one.
    static bool daemonLooksDown(const QString &output);

    // The command a user is told to run by hand when it was not. The sentence belongs to the catalog and
    // the command to the core, so the two cannot drift into naming different service managers: sudo on
    // Linux because the reader is sitting at a terminal, bare `sc` on Windows because this process
    // already holds the administrator token its manifest asked for and an elevated console is where that
    // verb works. A platform this file cannot name gets the systemd spelling, which is the one the two
    // distributions this application ships for use.
    static QString serviceStartHint(Strategy::Platform platform);

    // The IPv6 question, asked rather than answered: reading one sysctl value needs no root and
    // changes nothing, and an IPv6-less host is worth naming in the journal because MASQUE is much
    // less likely to come up there. Deliberately not `sysctl -w` — see prepareEnvironment().
    static QStringList ipv6CheckArgs();
    // Whether that answer says IPv6 is switched off on this host. Unknown when the text said nothing
    // either way, which is the case the journal mentions without drawing a conclusion from.
    enum class Ipv6 { Unknown, Enabled, Disabled };
    Q_ENUM(Ipv6)
    static Ipv6 parseIpv6State(const QString &sysctlOutput);

    // The whole preparation as data: which command, with what argv, needing root or not, and whether
    // its failing stops the rest. Linux gets all three steps; Windows the protocol one only, because
    // the stuck-daemon state this exists for is systemd's, and a UAC prompt on every tunnel start is a
    // trade the user did not agree to (see prepareEnvironment()'s note on the elevation). A platform
    // with no package manager, no systemd and no warp-cli to reconfigure prepares nothing at all.
    static QList<PreparationStep> preparationSteps(Strategy::Platform platform);

    // ---- the setup wizard's chain, as data ----
    //
    // What "set WARP up from nothing" means, step by step, so the wizard that walks it and the test
    // that pins it read the same list. Data rather than code for the reason installationProbes() and
    // preparationSteps() are: the sequence is the finding (a client that is installed but never
    // enrolled cannot connect; one enrolled over the default transport may not come up on a hostile
    // link), and a sequence written as calls inside a dialog is a sequence no runner can check without
    // running a package manager.
    //
    // One command a step runs. Most steps have one; the two Windows steps have two each, because there
    // is no `restart` verb for a service control manager and no shell is allowed to invent one.
    struct SetupCommand
    {
        QString program;         // resolved by the runner, never guessed
        QStringList args;        // already split — nothing here is a command line to be parsed
        bool privileged = false; // needs root/admin; see composePrivilegedCommand()
        int timeoutMs = 0;       // a step that never answers must not hold a modal window open forever
        // Whether this command's refusal ends the step. True — the default, and what every command but
        // the daemon step's carries — for a command whose non-zero exit is a fact about the machine.
        // False where a refusal is the answer we were hoping for: `sc start` on a service the MSI
        // already started exits 1056, and a wizard that stopped there would be a wizard that fails on
        // every healthy Windows host. The runner still puts the output in the pane, because "the service
        // said it was already running" is worth more than silence; it just does not stop the chain.
        // Mirrors PreparationStep::fatal, which is the same question in the automatic path.
        bool fatal = true;
    };

    // Which of the three ways a step gets done. Kept explicit rather than inferred from an empty
    // program, because the three have different owners and different failure modes: the install is
    // WarpInstaller's (repository, key, repair, and a network-vs-dpkg-lock classification of its own),
    // the enrolment is WarpManager's (read, then delete-then-new on a conflict — a second
    // `registration new` on an enrolled device is a second key and an orphaned first), and the rest are
    // plain commands this dialog runs and reads the output of.
    enum class SetupKind
    {
        Install,
        Registration,
        Command
    };
    Q_ENUM(SetupKind)

    struct SetupStep
    {
        QString id;              // "install" | "daemon" | "register" | "protocol" | "service" | "connect"
        SetupKind kind = SetupKind::Command;
        QList<SetupCommand> commands;
        // Whether the user may walk away mid-step. False for the package manager and nothing else: an
        // apt or msiexec interrupted halfway leaves a database to repair, which is the same reason
        // WarpInstaller offers no Cancel — so the wizard disables its own button for the length of that
        // one step instead of pretending the choice exists.
        bool cancellable = true;
    };

    // The chain for one machine, pure: a host that already has a client skips the install, and a device
    // that is already enrolled skips the enrolment — the two steps that are expensive or destructive
    // when they are not needed. `installed` and `registration` are handed in rather than read, so every
    // branch is checkable on a runner that has neither.
    static QList<SetupStep> setupSteps(Strategy::Platform platform, bool installed,
                                       Registration registration);
    // The step ids in the order the wizard walks them, for a host that has nothing and knows nothing —
    // the full chain, spelled once so a test can pin the sequence without building a plan.
    static QStringList setupStepIds(Strategy::Platform platform);

    // How a privileged command is run on this host. One gate, one spelling, and every caller goes
    // through it: pkexec-prefixed on Linux for a session that is not already root, bare everywhere else,
    // because Windows' own manifest asked for the administrator token before main() ran and a second
    // UAC prompt for something the process may already do is a prompt with nothing behind it.
    static bool composePrivilegedCommand(Strategy::Platform platform, const QString &programPath,
                                         const QStringList &args, const QString &pkexecPath,
                                         bool alreadyRoot, QString *outProgram, QStringList *outArgs,
                                         QString *error);

    // The program and argv a privileged step actually runs — pkexec-prefixed, or bare for a session
    // that is already root. Split out and passed its resolved paths exactly like
    // WarpInstaller::composeLinuxInstall(): the gate that matters, pkexec from a root session being a
    // prompt nobody is there to answer, must be checkable without being root, and a host with no
    // pkexec has to be told so in words rather than by a FailedToStart naming a program nobody typed.
    static bool composeServiceRestart(const QString &systemctlPath, const QString &pkexecPath,
                                      bool alreadyRoot, QString *program, QStringList *args,
                                      QString *error);

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

    // ---- the vendor's own window --------------------------------------------
    //
    // What the Linux client puts on screen, and how to keep it off.
    //
    // This half exists because the premise it replaces was wrong. The Linux package ships a desktop
    // application after all — /usr/lib/warp/warp-taskbar, a Flutter tray program whose window is titled
    // "Cloudflare One Client" — and it brings that window up in answer to a tunnel transition: on a host
    // where the taskbar was running, `warp-cli connect` mapped two windows within seconds of the verb's
    // own "Success". A second window talking about the same tunnel is the one thing this application's
    // single-window promise cannot survive, so whatever moves the tunnel has to take that window down.
    //
    // Killing it is not enough, and the measurements are why the API is shaped as it is. The program runs
    // under a *user* systemd unit (/usr/lib/systemd/user/warp-taskbar.service) declared `Type=dbus`,
    // `BusName=com.cloudflare.WarpTaskbar`, `Restart=always`, `RestartSec=5`, and the same bus name is
    // D-Bus-activatable (/usr/share/dbus-1/services/com.cloudflare.WarpTaskbar.service names that very
    // unit). On the machine this was reported from:
    //
    //   * `pkill -x warp-taskbar` — a new instance was running eight seconds later;
    //   * `systemctl --user stop warp-taskbar.service` — the stop job was cancelled and the unit was
    //     active again, the activation arriving from the daemon's side of the socket;
    //   * `systemctl --user mask warp-taskbar.service` — stayed dead across a subsequent
    //     `warp-cli connect`, because a masked unit can be neither started nor activated.
    //
    // So this is a policy the service manager has to be told about, not a signal to send — and it needs
    // no root, because the unit belongs to the user's own manager and masking it is one symlink under
    // ~/.config/systemd/user. The kill stays last for what no unit covers: a taskbar started by hand, or
    // one a session manager started before we got here.
    //
    // The names are data rather than code for the reason every argv in this file is: a Linux runner can
    // assert them without a client, and a Windows runner without a systemd.
    static QString warpGuiProcessName();   // "warp-taskbar" — the image the unit execs
    static QString warpGuiUserUnit();      // "warp-taskbar.service" — what keeps it up
    static QString warpGuiBusName();       // "com.cloudflare.WarpTaskbar" — what reactivates it

    // One command of the suppression. The same shape as PreparationStep and InstallationProbe, and for
    // the same reason: the whole policy of what keeping the window away means is then readable at once,
    // pinnable by a test on a host that owns none of these binaries, and extendable without touching the
    // runner.
    struct GuiSuppressionCommand
    {
        QString id;        // "unautoenable" | "mask" | "kill" — names itself in the journal
        QString program;   // "systemctl" | "pkill" — resolved by the runner, never guessed
        QStringList args;  // already split; no shell composes any of this
    };

    // The suppression for one platform, in the order it has to run: the unit is told not to be wanted by
    // the next session, then masked *and stopped* (the order is a finding, not a preference — masking an
    // active unit does not stop it, and a bare stop job can be cancelled by the activation that follows
    // it), then the process itself, which catches a taskbar no unit owns.
    //
    // Windows answers with an empty list, and that is deliberate rather than unfinished: the Windows half
    // of the single-window promise already has an owner — WarpInstaller::closeWindowsGui(), which knows
    // the two image names the vendor has used for its tray program — and a second list of the same
    // processes here would be a second opinion about what to kill, drifting from the first. Unknown gets
    // nothing, as everywhere else in this file.
    static QList<GuiSuppressionCommand> guiSuppressionCommands(Strategy::Platform platform);

    // The inverse list, for the case the switch is turned off: the unit is unmasked and then enabled and
    // started, which hands the tray application back to the session that was kept away from it. There is
    // deliberately no kill in it, and no sweep of autostart files either — a restore is a change of mind
    // about a policy, not a licence to rename back files some other tool or the user may have edited since.
    static QList<GuiSuppressionCommand> guiRestorationCommands(Strategy::Platform platform);
    static QStringList userUnitUnmaskArgs();
    static QStringList userUnitEnableArgs();

    // `systemctl` addressed to the caller's own session manager: `--user` first, then the verbs. Split
    // out because that one flag is the whole difference between taking a tray program away and stopping
    // the daemon that holds the tunnel.
    static QStringList systemctlUserArgs(const QStringList &verbs);
    static QStringList userUnitDisableArgs();
    static QStringList userUnitMaskArgs();

    // `pkill -x <name>` — exact image name, and never `-f`. A `-f` sweep matches the *command line*, so
    // `-f warp` names warp-svc (the daemon holding the tunnel) and every in-flight `warp-cli`, and
    // `-f cloudflare` reaches the package's crashpad handler and anything else that merely mentions the
    // vendor; LinuxBackend's own kill list documents the same rule for the same reason. An exact name can
    // only ever hit the window this is about.
    static QStringList pkillArgs(const QString &processName);

    // Whether the WARP package on this platform puts a window on screen. Both of the ones this
    // application ships for do; macOS is not a platform this application builds for, and an Unknown host
    // has no client to have a window. Asked by the settings page (which must not offer a switch that can
    // mean nothing) and by the installer's single-window policy, so it is stated once — WarpInstaller
    // delegates here rather than keeping a second answer that would drift.
    static bool platformHasWarpGui(Strategy::Platform platform);

    // The directories a desktop session reads for autostart entries, in the order the XDG spec asks them
    // to be read: the user's own first (writable by this process), then one per `$XDG_CONFIG_DIRS` entry
    // (root-owned on a normal machine, where a refusal is expected and costs only a journal line). Built
    // from the two values handed in rather than from the environment, so a runner can pin the spelling of
    // a directory it does not have — the same reason every path composition in this file takes its input.
    static QStringList autostartDirectories(const QString &homePath, const QString &xdgConfigDirs);

    // Whether one autostart file *is* the WARP window. Both halves are evidence: the name it ships under
    // has moved between builds (`com.cloudflare.WarpTaskbar`, `io.github.…`, `warp-taskbar`), and the
    // `Exec=` line is the one thing that cannot lie about what would start. A file that only mentions
    // cloudflare in a comment is not an entry for its window, and an entry already marked `Hidden` or
    // `NoDisplay` cannot start anything — so neither half is trusted alone where the other is missing.
    static bool autostartNamesWarpGui(const QString &fileName, const QString &contents);

    // The name an autostart entry is moved to. `.disabled` is the suffix the desktop-entry spec reserves
    // for exactly this — "the file is here, and it is not to be started" — which is reversible by hand and
    // a smaller lie than deleting a file somebody's package manager owns.
    static QString disabledAutostartName(const QString &fileName);

    // Take this machine's WARP window away and keep it away: the commands above in order, each on its own
    // short deadline, then the autostart sweep.
    //
    // Asked *before* a tunnel is moved rather than after, because a window that never appears beats a
    // window that is chased — the activation the daemon sends is what maps those two windows, and a
    // masked unit cannot answer it. It is asked again after a service restart (the same activation, sent
    // by a warp-svc that has just come back up) and whenever the tunnel is reported Connected, so a host
    // somebody set up by hand ends in the same state as one that was not.
    //
    // Idempotent and cheap: `systemctl` answering about a unit it has already masked is exit 0 and no
    // work. It never blocks — the chain runs on its own QProcess, off the verb slot, exactly as the
    // installation probes do, because a window kill holding that slot would strand a status poll behind
    // it. A second call while one runs joins the run in flight. A refusal (no systemctl, no pkill, a
    // session manager that will not answer) is one journal line at warn level and never an error: a
    // tunnel that works behind a window nobody closed is worth more than a failure the user can do
    // nothing about.
    //
    // Virtual because whether a caller asks for this at the right moment is the claim worth testing, and
    // no static parser can see it — the same reason ensureRegistered() and prepareEnvironment() are.
    virtual void suppressWarpGui();

    // The other direction, because a switch that works one way is not a switch.
    //
    // Turning the policy off has to give back what turning it on took away: the mask is durable precisely
    // so that a connect cannot be interrupted by a window, and the same durability means the tray
    // application stays gone across logins until something unmasks it. So the ask is the pair of commands
    // that undoes it — unmask, then enable — and it is never a kill and never a start: Muffin takes the
    // policy off the unit and leaves the launching of Cloudflare's own window to the user's session.
    //
    // Asked on the transition from on to off rather than on every read of an off setting: a startup that
    // found the switch off should leave a machine that was never touched exactly as it is, and a machine
    // that was masked by an earlier session is corrected the moment the window learns the user's wish.
    virtual void restoreWarpGui();

    // The single-window switch, handed in rather than read: this class is in the core and Settings
    // belongs to the UI, which is the same seam WarpInstaller::setHideWarpGui() exists for. Its default
    // is the store's own default rather than false, because the policy is on unless the user turned it
    // off — and a manager that never got told still keeps the promise the interface says it makes.
    //
    // The one setter in this class that acts: going from on to off hands the tray application back, under
    // the same gates the suppression runs under.
    void setHideWarpGui(bool on)
    {
        if (m_hideWarpGui == on)
            return;   // every Settings::changed() arrives here, and an unchanged value asks for nothing
        const bool wasOn = m_hideWarpGui;
        m_hideWarpGui = on;
        if (wasOn)
            restoreWarpGui();
    }
    bool hideWarpGui() const { return m_hideWarpGui; }

    // Whether a suppression run is in flight. Exposed for the reason isPollingEnabled() is: a suite that
    // hands out answers has to be able to see what it is answering about.
    bool isSuppressingWarpGui() const { return m_guiRunning; }

    // Whether the run in flight (or the last one) changed anything on this machine. The journal keys off
    // it, because a policy that has already been applied answers "nothing happened" and a per-connect
    // sentence about a no-op is noise that teaches nobody to read the log.
    bool guiSuppressionChangedAnything() const { return m_guiChanged; }

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

    // Put this machine into a state where a connect can actually come up, without touching anything
    // that does not need touching.
    //
    // Why it exists, in the words of the machine it was found on: on a host whose provider interferes
    // with WARP's default transport, `warp-cli connect` is accepted and never completes — the UI's
    // tunnel half lights, the power ring stays on zapret, and the daemon says `warp=off` forever. Two
    // things fix that, and neither is a guess at a diagnosis: switch the tunnel to MASQUE, and
    // restart warp-svc, whose state goes stale when it came up while a zapret queue was already
    // rewriting the routing. Both are in preparationSteps(); the runner works through that list rather
    // than through ifs.
    //
    // Ordering is the whole point, and it is why this is a step rather than three calls at the caller:
    // the restart has to happen *after* the enrolment (a daemon restart mid-`registration new` loses
    // it) and *before* the connect, and the connect has to wait for the service to come back — hence
    // the settle at the end, on a timer, never a sleep on the GUI thread.
    //
    // On root: exactly one step needs it, and it is asked for through pkexec each time the tunnel is
    // brought up — which is a prompt the user sees, so it is announced through stageChanged() first.
    // Two deliberate refusals follow from that. The first is IPv6: the fix some guides give is
    // `sysctl -w net.ipv6.conf.all.disable_ipv6=0`, which flips a system-wide setting beyond this
    // app's remit and would need a second prompt to do it; this reads the value instead and says in
    // the journal what it found and what would change it. The second is Windows, where the service
    // step is absent — the stuck state is systemd's, and a UAC prompt per tunnel start was not
    // agreed to. A refused or unavailable elevation is reported and does not stop the connect: the
    // other step is still worth what it cost, and a host where the daemon is not stuck works anyway.
    //
    // Answers through preparationFinished(ok) — true when every fatal step said yes — and reports each
    // step as it begins through stageChanged(stage). Like ensureRegistered(), it never blocks, joins
    // its own run rather than starting a second one, queues itself behind a busy verb slot, and
    // returns false only when nothing could be begun at all.
    virtual bool prepareEnvironment(QString *error = nullptr);

    // Re-probe availability: find the program, then ask it whether it is able to answer, and — only if
    // nothing runnable was found — ask this machine's package databases whether the client is there
    // anyway. Called on construction; a caller who just installed WARP while the window was open can
    // ask for the same effect without a full restart. Nothing in it blocks: the two follow-ups are
    // asynchronous, so a constructor that runs this on a machine with no client forks no more than
    // one short command per database the host actually has.
    virtual void rescanAvailability();

    // The cheap half of that rescan: resolve the program again and update availability, with no
    // package-database round and no version probe behind it. Split out because there are callers that
    // need the answer and must not pay for the round — a guard on the way into a verb, which runs on
    // every refused call — and because a rescan that clears the installation half is the wrong tool for
    // "did warp-cli appear while we were not looking". Returns whether a client is drivable now.
    bool refreshProgramPath();

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
    // Whether this machine has a WARP client at all, and where the answer came from. Emitted when a
    // round of package probes ends — with `installed` true and `via` naming the database that said
    // yes, or false and empty. `via` is empty for a drivable client too: that is the case nobody has
    // to be told about, because the tunnel starts working.
    //
    // Separate from availabilityChanged() on purpose. That one answers "can the half be pressed", and
    // a client that exists but cannot be run changes only this one; a caller holding a deferred offer
    // needs the second edge, not a re-run of the first.
    void installationChanged(bool installed, QString via);
    // Which step of prepareEnvironment() is running, announced as it begins — Idle when the last one
    // is over. The window turns these into the sentence the user reads while a root prompt or a
    // service restart is doing its work; see Stage.
    void stageChanged(WarpManager::Stage stage);
    // Whether the machine was left ready for a connect: true when every fatal step answered yes
    // (including the case where the host had no steps to run), false when one refused or failed.
    // Exactly one of these closes every sequence prepareEnvironment() was told it started, in the
    // same contract registrationFinished() keeps.
    void preparationFinished(bool ok);
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
    // The guard every verb uses on the way in.
    //
    // A cached YES is kept: a client that was drivable a moment ago still is, and re-resolving on every
    // poll buys nothing. A cached NO is re-asked, because "not installed" is exactly the answer that
    // goes stale — the thing that makes it stale is an install, and an install is something this class
    // neither runs nor can see. Without the re-ask the first verb after a successful install is refused
    // with "warp-cli is not installed on this machine", a sentence about the machine as it was before
    // the user's own package manager ran, which is the failure the setup wizard was built to walk
    // through and then hit on its own second step.
    bool availableNow() { return m_available || refreshProgramPath(); }


    // The steps of ensureRegistered(), each one started from the previous one's answer.
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
    // The preparation's own version of the same courtesy, and the reason both exist: a sequence that
    // found the slot busy is not cancelled by that, it is deferred — a connect issued without the
    // MASQUE switch and the service restart is the hang this whole half was written for.
    void runQueuedPreparation();
    // The one pump every freed slot calls, so the two deferred sequences cannot each forget the other.
    void runQueuedSequences();

    // ---- the preparation sequence ----
    // The same shape as the enrolment chain — one step per answer, one signal at the end — and one
    // deliberate difference: a step's program is not always warp-cli, so it goes through startStep()
    // with a resolved path rather than through startVerb().
    bool runPreparationStep();
    void onPreparationStepAnswered(const PreparationStep &step, const QString &out, int exitCode,
                                   bool crashed);
    void setStage(Stage stage);
    // Ends the sequence with the same once-only guarantee finishRegistration() gives its own.
    void finishPreparation(bool ok, const QString &why);
    // The gap between the service coming back and the daemon being worth asking. Started by the last
    // step's answer, and the only thing between that answer and preparationFinished(true).
    void startSettleWait();
    // The wait a connect that never completed is owed: one journal line naming the one cause this
    // whole sequence exists for, so a timeout is not indistinguishable from a dead client.
    void armConnectDeadline();
    void onConnectDeadline();

    // Every verb this class runs goes through these two. startVerb() is the warp-cli spelling —
    // program resolved, state machine told what to expect — and startStep() is the same machinery for
    // an arbitrary program, which is what lets `pkexec systemctl restart warp-svc` ride the existing
    // timeout, the same in-flight slot, and the same FailedToStart cleanup instead of growing a second
    // copy of all three.
    bool startStep(const QString &program, const QStringList &args, int timeoutMs, State onAccepted,
                   bool wasConnectAttempt, QString *error,
                   const QByteArray &stdinData = QByteArray(), RawHandler raw = nullptr);

    // ---- the two follow-ups rescanAvailability() starts ----
    //
    // Both run on their own QProcess, never on the verb slot: a status poll that arrived while the
    // host was being asked its package databases would otherwise be refused by a probe nobody asked
    // for, and a poll dropped on that account is a tunnel that looks unchanged forever.
    void verifyCandidate();
    void onVersionAnswered(const QString &out, int exitCode, bool crashed);
    void startInstallationProbe();
    void runNextProbe();
    void onProbeFinished(const QString &out, int exitCode, bool crashed);
    // Ends a round with the answer the databases gave. Idempotent: a round already over is not ended
    // twice, and a killed probe still lands here, because "no answer" is the answer "absent" to.
    // Ending the round. `via` is taken BY VALUE on purpose: the only caller that has a name to pass hands
    // in m_currentProbe.source, and this function clears m_currentProbe before it reads it. A const
    // reference here therefore names an already-reset, empty string — the verdict keeps, the name of the
    // database that earned it disappears, and the journal says "found via " with nothing after it.
    void finishProbe(bool packaged, QString via);
    void stopInstallationProbe();
    void setAvailable(bool available);

    // ---- the window suppression ----
    //
    // The same shape as the installation-probe chain it sits beside, and for the same reason: a command
    // per answer, one owned QProcess that is not the verb slot, and a program that does not resolve is
    // skipped with a word rather than guessed at another name for. The last difference matters: a probe
    // whose tool is missing is a machine that has no client, while a suppression whose systemctl is
    // missing is a machine whose window will not be closed by *this* command — the next one in the list
    // may still land, so the run continues instead of ending.
    void runNextGuiSuppression();
    // Which of the two directions the run in flight is working through — false is "keep the window away",
    // true is "give it back" — decided once, by the caller that built the queue, and read by the runner
    // when it picks the sentence to log. Two flags on two paths would be two ways to disagree about what
    // the machine was just told to do.
    void beginGuiPolicy(bool restoring);
    // The answer carries the command's own output, because what a refusal *means* differs per command:
    // systemctl's complaint is a sentence worth quoting, and pkill's exit 1 is the outcome wanted.
    void onGuiSuppressionAnswered(const GuiSuppressionCommand &command, int exitCode, bool crashed,
                                  const QString &output);
    // Ends the run: clears the flags, and says once what the run did. Idempotent, so a killed command
    // and a completed one both land here exactly once.
    void finishGuiSuppression(const QString &why);
    // The autostart sweep, run once per object rather than once per command: renaming a file that is
    // already renamed is a no-op, and a second pass over the same directories would be a second list of
    // rejections to read. Returns how many entries were moved aside.
    int sweepWarpAutostart();
    // The registry half of a round: read in-process rather than forked out, because QSettings opens a
    // registry key without a helper program and every non-Windows host has no such key to read. False
    // off Windows, so the callers there do not need to know which platform compiled them.
    static bool registryReportsInstall();

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

    // ---- the preparation half ----
    //
    // The same running/queued pair the enrolment keeps, for the same two reasons: a second request
    // joins the run in flight rather than queueing a second restart of one service, and a request that
    // found the slot busy cannot be dropped — a connect issued without the preparation is the silent
    // hang this sequence exists to prevent.
    bool m_prepRunning = false;
    bool m_prepQueued = false;
    // The list being worked through, snapshotted when the run starts so a step's answer can be read
    // against the step that asked it, and m_prepIndex walks it.
    QList<PreparationStep> m_prepSteps;
    int m_prepIndex = 0;
    // The last stage announced.
    Stage m_stage = Stage::Idle;
    // Whether this run actually restarted the service — which is the only case that owes the settle
    // wait. A host that skipped the step (no pkexec, an older warp-cli) or whose restart was refused
    // never moved the daemon, and making it wait 1.5 s for a service that did not move would be the
    // app charging the user for a fix it did not apply.
    bool m_prepRestarted = false;
    // The gap between the service coming back and the daemon being worth asking, and the deadline a
    // connect is given to prove itself. Single-shot both, owned by this object, and neither ever
    // sleeps the GUI thread.
    QTimer *m_settleTimer = nullptr;
    QTimer *m_connectDeadline = nullptr;

    // ---- the installation half ----
    //
    // Whether a package database, the Windows registry or a service said the client is on this
    // machine even though nothing runnable was found. Cached for the same reason m_available is: an
    // offer to install is decided in a click handler, and a click handler cannot wait for apt.
    bool m_packaged = false;
    // Which source answered yes — "dpkg", "snap", "registry" — empty when nothing did. Carried beside
    // the bool because "already installed" without "by what" is not a sentence anybody can act on.
    QString m_packagedVia;
    // True from the moment a round starts until it answers. This is what makes isInstalled() answer
    // yes while the host is being asked, which is the difference between offering an install and
    // offering an install over the top of one.
    bool m_probing = false;
    // A client that was found and then reported damage, or the config-file debris of a removed one.
    // Distinct from !m_available: the two want different offers, one for software that is not there
    // and one for software that is there and does not work.
    bool m_broken = false;
    // The version answer's own words, kept for the log line the demotion writes; the classification is
    // a judgement, the sentence the user reads should quote the tool.
    QString m_brokenDetail;

    QList<InstallationProbe> m_probeQueue;    // the databases not asked yet in this round
    InstallationProbe m_currentProbe;         // the one whose answer is in flight
    QProcess *m_probeProcess = nullptr;       // non-null only while one probe command runs
    // The version question's own process, kept apart from the verb slot for the reason in the comment
    // on verifyCandidate(): the verdict it produces is about the machine, and a poll must not be able
    // to answer it or be refused by it.
    QProcess *m_versionProcess = nullptr;

    // ---- the window-suppression half ----
    //
    // The single-window policy, owned by the caller (Settings) and handed in: see setHideWarpGui().
    bool m_hideWarpGui = true;
    // True for the whole command list, so a second request joins the run in flight instead of starting a
    // second mask of a unit that is already masked.
    bool m_guiRunning = false;
    // Which direction the run in flight is working: true when it is handing the tray application back
    // rather than keeping it away. See beginGuiPolicy().
    bool m_guiRestoring = false;
    // The list being worked through, snapshotted when the run starts so a command's answer is read
    // against the command that asked it.
    QList<GuiSuppressionCommand> m_guiQueue;
    GuiSuppressionCommand m_currentGuiCommand;
    // The chain's own process — never m_process, which is the verb slot a status poll needs.
    QProcess *m_guiProcess = nullptr;
    // Whether the autostart sweep has been done on this object. One pass is enough for a session: the
    // files it renames do not come back by themselves, and a per-connect sweep would be a per-connect
    // directory walk of nobody's home.
    bool m_autostartSwept = false;
    // Whether the run in flight moved something. systemctl prints "Created symlink …" / "Removed …" when
    // it changes a unit and nothing when the unit was already where it belongs, so the answer is the
    // evidence for one journal line — and pkill's exit 0 is the same evidence for the window itself.
    bool m_guiChanged = false;
};

} // namespace zapret
