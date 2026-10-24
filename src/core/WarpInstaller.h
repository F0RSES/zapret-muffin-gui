#pragma once

#include "Strategy.h"

#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace zapret {

// Puts the Cloudflare WARP client on a machine that does not have it.
//
// Why installing WARP is not like installing zapret: zapret is a tree of files this application
// downloads, unpacks and execs itself, so its install is a filesystem transaction. WARP is a system
// service that owns a network interface — on Linux an apt package that runs warp-svc under systemd, on
// Windows an MSI that registers a service and a tunnel provider. Neither is something an unprivileged
// process may do, and neither is reversible by deleting a folder. So this class does the smallest
// possible privileged thing: one command a human approves with a password (pkexec) or one that runs
// inside an already-elevated process (msiexec, because packaging/windows/app.manifest asked Windows for
// that token before main() ran) — and nothing after it.
//
// What it deliberately does NOT do:
//
//   * Register the client. `warp-cli registration new` enrols this machine with Cloudflare and chooses
//     a key; that is a decision about somebody's network identity, and warp-cli does it on the first
//     connect anyway. WarpManager owns connect, so registration stays there.
//   * Offer a Cancel. A package manager interrupted halfway leaves a database to repair — dpkg
//     --configure -a, or an MSI in a state only Windows Installer can unwind — so the progress window
//     shows a busy bar and no button. If that ever changes, the cancel has to run the repair too.
//
// The commands are static functions returning data, not methods that act: msiexec is unreachable from
// a Linux runner and apt-get from a Windows one, and the only way to check the spelling of the half you
// cannot run is to ask for it as a value. Same reason WindowsArgs and
// ZapretPaths::systemInstallDirUnder() exist.
//
// Threading: belongs to the GUI thread's event loop, like WarpManager. One operation at a time.
class WarpInstaller : public QObject
{
    Q_OBJECT

public:
    // Which phase the install is in, and what its progress line describes.
    enum class Stage
    {
        Idle,
        Preparing,    // reading the host: package manager, shell, somebody to ask for root
        Downloading,  // Windows only: the MSI on its way to a temporary file
        Installing,   // the privileged command running
        ClosingGui,   // Windows only: the installer opens a window; this app owns the tunnel
        Verifying     // asking whether a warp-cli exists now that it did not before
    };
    Q_ENUM(Stage)

    explicit WarpInstaller(QObject *parent = nullptr);
    ~WarpInstaller() override;

    // Which platform's install this object would run. Defaults to the host, and exists to be set so a
    // test can drive the Windows half — composition and the staged file — on a Linux runner. What is
    // always the host's own decision is the spawn itself, which is guarded by Q_OS_WIN where it runs.
    void setPlatform(Strategy::Platform platform);
    Strategy::Platform platform() const;

    // The single-window policy, handed in rather than read by this class: Settings belongs to the UI and
    // this object is in the core, which has no business opening a QSettings — the same seam WarpManager
    // uses for the same reason. It is only asked at the end of an install, through closesWarpGui(), and
    // only where that function says a window could exist at all.
    void setHideWarpGui(bool on);
    bool hideWarpGui() const { return m_hideWarpGui; }

    bool isBusy() const { return m_stage != Stage::Idle; }
    Stage stage() const { return m_stage; }

    // The whole install, asynchronously. False with an explanation only when nothing could even be
    // started (an install already running; no root and nobody to ask for one). Every later outcome — a
    // refused password, an MSI that returned 1603, a client that did not appear afterwards — arrives
    // through installationFinished().
    bool start(QString *error = nullptr);

    // ---- Linux: the repository, exactly as Cloudflare publishes it ----
    //
    // These five values are the ones https://pkg.cloudflareclient.com/ asks an apt host to write (the
    // page the WARP client docs link for Linux), verified against it: the key URL, the dearmoured
    // keyring it produces, the sources file, the URL the deb line names, and the package. Any one of
    // them drifting turns the install into "apt could not find cloudflare-warp" — a failure with no
    // clue in it — so they are named here and asserted by name in coretest, not lost inside a string.
    static QString publicKeyUrl();      // https://pkg.cloudflareclient.com/pubkey.gpg
    static QString repositoryUrl();     // https://pkg.cloudflareclient.com/
    static QString keyringPath();       // /usr/share/keyrings/cloudflare-warp-archive-keyring.gpg
    static QString sourcesListPath();   // /etc/apt/sources.list.d/cloudflare-client.list
    static QString packageName();       // cloudflare-warp

    // The one line written into sourcesListPath(): signed-by first, then the URL, then the suite, then
    // the component — the shape the repository documents.
    static QString repositoryLine(const QString &codename);

    // The generated script. Its only substitution is the release name, and isUsableCodename() is what
    // keeps that name from carrying anything a shell would read as something other than a word. apt's
    // own output is the progress, so this is one command rather than four: an interruption in the middle
    // of `apt-get install` leaves a dpkg to repair, and four prompts costing four passwords would be
    // the worse outcome anyway.
    static QString linuxScriptBody(const QString &codename);

    // A suite name apt will accept: non-empty, lowercase alphanumerics with dot, dash and underscore,
    // and no newline or shell metacharacter — the value goes into a script a root shell will read.
    static bool isUsableCodename(const QString &codename);

    // The release name of this machine, from /etc/os-release: VERSION_CODENAME, falling back to
    // UBUNTU_CODENAME (a Debian-derived release names its parent's suite, and that is the suite the
    // repository ships). Parsed rather than shelled out to lsb_release, which is not installed on every
    // host the .deb reaches. Empty means this machine cannot be asked to add an apt suite at all.
    static QString hostCodename();
    // The same reading, from text handed in: /etc/os-release is KEY=value lines, so this is a lookup
    // with the two keys tried in order. Split out because the /etc/os-release of the machine running the
    // tests is not a sample of every release this installer will ever be pointed at.
    static QString parseCodename(const QString &osReleaseText);

    // The suite to ask Cloudflare's repository for, given the name a host calls its own release.
    //
    // The repository publishes upstream Debian and Ubuntu suites only, and a rolling derivative that
    // builds from Debian rather than shipping a release of its own names itself in /etc/os-release —
    // Kali writes VERSION_CODENAME=kali-rolling, which is not a directory on the server. Asking for it
    // anyway is a 404 during `apt-get update`, which `set -eu` turns into the failed install the user
    // reports as "apt-get failed (exit code 100)". So a name in such a family is answered with the
    // published suite its packages are built against (kDerivativeFallbackSuite), and every other name —
    // noble, jammy, bookworm, trixie — is returned untouched.
    //
    // Public and pure rather than folded into the generator, for the reason the whole Linux half is:
    // the mapping decides what a root shell writes, and coretest has to be able to hand it the os-release
    // of a machine no runner here has.
    static QString repositorySuite(const QString &codename);
    // This machine's answer: hostCodename() through repositorySuite(). This is the value the install
    // script is composed with. Empty when the host names no release at all — a missing name is not a
    // family this function could map, and the caller refuses it before a password is asked for.
    static QString hostRepositorySuite();

    // The program and argv that ASK for the install: `pkexec <shell> -s`, or `<shell> -s` when this
    // process is already root — the gate MuffinInstallHelper::composePromote() applies, for the same
    // reason (pkexec from a root session is a prompt nobody is there to answer). `-s` says "read the
    // script from standard input", and that is the design: the generated bytes go down a pipe this
    // process holds, so no file anybody could rewrite ever sits on the authorised path. polkit's own
    // warning about `pkexec <shell> <script>` is that the interpreter runs whatever that file says and
    // the file belongs to the caller; here there is no file to swap between approval and exec.
    // `pkexecPath` and `shellPath` are parameters so a test can present a host without either;
    // `shellPath` is /bin/bash where it exists and /bin/sh where it does not, because the script is
    // POSIX and a stripped container has the latter and not the former.
    static bool composeLinuxInstall(const QString &pkexecPath, const QString &shellPath,
                                    bool alreadyRoot, QString *program, QStringList *args,
                                    QString *error);

    // Whether root can be asked for at all: already root, or a pkexec with an agent reachable to
    // answer it. Every input is passed in, because "this machine has no polkit agent" is a fact a test
    // has to be able to present rather than one it has to hope the runner does not have.
    static bool canAskForRoot(bool alreadyRoot, bool pkexecPresent, bool polkitAgentReachable);

    // ---- Windows: the installer, and the window it leaves behind ----
    //
    // The documented link for the current Windows build, taken from the WARP client docs' own download
    // button. It answers 302 -> .../windows/version/<build> -> the MSI, with a Content-Disposition
    // naming the file, so this is the URL that does not go stale between releases; the redirect is
    // followed under the same policy ZapretDownloader asks Qt for ("follow it, never downgrade it").
    static QString installerUrl();

    // What the downloaded MSI is called in the staging directory. A fixed name and not the one the
    // server offered: a name that arrives over a network is a name that can hold anything, and this is
    // the path handed to msiexec.
    static QString installerFileName();

    // msiexec's argv for a silent install: /i to install the file, /qn for no interface at all — a
    // progress dialog belonging to somebody else's installer, sitting on the user's screen while this
    // window claims to be the one controlling WARP — and /norestart, because an installer that decides
    // to reboot the machine mid-call is not this application's decision to make.
    static QStringList msiexecArgs(const QString &msiPath);

    // The msiexec exit codes that mean the client is installed now: 0, and 3010 for "installed, but
    // reboot before it works". Calling the second a failure would have the interface offer to install
    // the thing it just installed.
    static bool installSucceeded(int exitCode);

    // Where the MSI puts its files, given a Program Files root: the directory whose warp-cli.exe a
    // verification stats, and the one WarpManager::installCandidatePaths() composes verbs from. An
    // empty root means no answer, which is also what every non-Windows host passes.
    static QString windowsInstallDir(const QString &programFilesRoot);

    // The GUI executables a WARP install may leave on screen. Both spellings, because Cloudflare
    // renamed the tray application: current documentation names "Cloudflare WARP.exe" while the builds
    // people are still running are "Warp.exe". The service — warp-svc.exe, which is what actually holds
    // the tunnel — is on no list here and must never be added to one.
    static QStringList guiImages();

    // taskkill's argv for one image name: /F to force it, /IM to name the process by image rather than
    // by pid, which is the only handle available on a GUI another account started.
    static QStringList taskkillArgs(const QString &image);

    // Whether this platform has a WARP GUI to close at all. Linux's package installs a daemon and a CLI
    // and nothing else, so a promise to keep that window hidden is a promise nothing can keep; macOS is
    // not a platform this application builds for.
    static bool platformHasWarpGui(Strategy::Platform platform);

    // The single-window policy's own decision, pure: it can mean something only where there is a GUI to
    // close, and only when there is a client at all — a machine with no WARP has no window, and the
    // guard's job is to take over a running client, not to poke at processes on a host that has nothing
    // installed and never will.
    static bool closesWarpGui(bool hideWarpGuiSetting, Strategy::Platform platform, bool installed);

    // Best effort: close whatever WARP GUI is open, and report the image names that were actually
    // running. A taskkill answering "no such process" is the normal case, not an error — that is what
    // "nothing is in the way" sounds like. Two callers need exactly this and share it: the last step of
    // an install, and MainWindow's single-window policy on startup.
    static QStringList closeWindowsGui();

    // ---- the check every install ends with ----
    //
    // An installer's exit code says a package ran, not that this application can now drive a tunnel,
    // and the difference is the whole reason the check exists: apt can succeed while the service fails
    // to start, and msiexec can return 0 with warp-cli nowhere on disk. Resolved the way WarpManager
    // resolves it — PATH first, then the vendor's own directory — because the answer has to be the same
    // question the toggle asks a moment later.
    static bool installFoundFrom(const QString &resolvedProgram);

signals:
    // One human-readable line per phase, including the tail of the package manager's own output.
    // English, like every other message the core emits (see WarpManager::logMessage): it reports what a
    // tool did, and it lands in the journal beside lines that cannot be translated either.
    void installationProgress(QString status);
    // Exactly one of these ends every start(). `error` is empty when success is true.
    void installationFinished(bool success, QString error);

private:
    void setStage(Stage stage);
    void beginLinux();                  // compose the script, then ask for root
    void beginWindows();                // fetch the MSI, then install it
    void downloadInstaller();
    // One external command, run to completion (asynchronously). `script` is written to its stdin and
    // the channel closed, which is how the generated bash reaches the shell — see composeLinuxInstall.
    void runCommand(const QString &program, const QStringList &args, const QString &what,
                    const QByteArray &script = QByteArray());
    void runMsiexec(const QString &msiPath);
    void closeGuiAndVerify();
    void verifyAndFinish();
    void finishWith(bool success, const QString &error);
    void relayOutput();                 // the running command's newest line, as progress
    // The tail of what the running command printed, formatted for the end of a status line or an error.
    // Empty when the command said nothing. See kOutputTailChars for how much "tail" means.
    QString outputTail() const;
    // Remove the staged MSI and the directory holding it. Called on every way an install can end — the
    // success, each failure, and this object's destruction — because a 40 MB installer dropped in
    // QDir::tempPath() and left there is a file the user did not ask for and cannot find.
    void cleanupStaging();

    // How long the privileged command may take before it is killed. Generous on purpose: a slow link
    // pulling a package index is normal, and a cancelled package manager leaves a database to repair.
    // What this bounds is a wedged pkexec with no agent to answer it, which would otherwise hold the
    // modal progress window — and so the whole interface — open forever.
    static constexpr int kInstallTimeoutMs = 15 * 60 * 1000;
    // The download's own stall limit, per transfer, in the shape ZapretDownloader already uses.
    static constexpr int kTransferTimeoutMs = 90 * 1000;

    QProcess *m_process = nullptr;              // the privileged command, non-null only while it runs
    QNetworkAccessManager *m_network = nullptr; // created on first use, child of this
    QNetworkReply *m_reply = nullptr;           // the MSI download, non-null only while it runs
    QString m_stagingDir;                       // the throwaway directory holding the MSI, "" when none
    QString m_stagedInstaller;                  // the temporary MSI, "" when there is none
    QString m_output;                           // everything the running command has said so far
    // Set when the deadline below fired, so the exit that follows can be reported as a timeout rather
    // than as a package manager that chose to fail. Without it a killed apt reads as "apt-get said no".
    bool m_deadlineHit = false;
    Stage m_stage = Stage::Idle;
    Strategy::Platform m_platform = Strategy::Platform::Unknown;
    // The single-window policy, as the window handed it over. Defaults to on, matching Settings, so an
    // object nobody configured behaves like the application that ships it.
    bool m_hideWarpGui = true;
};

} // namespace zapret

