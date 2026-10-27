#pragma once

#include "WarpManager.h"

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;

namespace zapret {

class WarpInstaller;

// The setup wizard: one window that walks a machine from "no WARP at all" to "tunnel up", saying which
// step it is on and what each step printed.
//
// Why this exists when the automatic path already does the same things. That path (WarpInstaller,
// then WarpManager::ensureRegistered(), then prepareEnvironment(), then connect) is correct and
// invisible, which is exactly the problem: when it stops halfway — warp-cli not on PATH yet after an
// install, a daemon that was never started, a polkit prompt nobody noticed behind this window, a
// service that needed a restart — the user is left with a lit toggle, a power ring belonging to the
// other subsystem, and no account of what happened. A wizard trades that invisibility for a list: six
// named steps, a bar, and the commands' own output.
//
// What it deliberately reuses rather than restates:
//
//   * the ORDER and the argv come from WarpManager::setupSteps() — one plan, pinned by coretest, which
//     is why a second spelling of "MASQUE" or "warp-svc" cannot appear here;
//   * the install is WarpInstaller's, because it owns the repository, the key, the repair, the staging
//     of an MSI and the classification of a failure into network / dpkg-lock / refused-password.
//     Running apt from this dialog instead would lose all five;
//   * the enrolment is WarpManager::ensureRegistered(), because a bare `registration new` on a device
//     that is already enrolled — or that holds a conflicting enrolment — is a second key and an
//     orphaned first. The wizard asks; it does not guess which of the four readings applies.
//
// Every command runs through QProcess asynchronously and nothing here calls waitForFinished(). That is a
// correctness requirement rather than a style choice: the privileged step is `pkexec`, whose password
// prompt belongs to a polkit agent in another window, and a modal dialog that blocks its own event loop
// is a dialog that cannot be answered — the prompt would sit there with this window frozen behind it.
//
// Testability: the three seams below are virtual, so a suite can drive the whole state machine — step
// order, the retry after a failure, the bar, the words on the label — without installing anything,
// enrolling a device or restarting a service on the machine running the tests.
class WarpSetupDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WarpSetupDialog(WarpManager &manager, QWidget *parent = nullptr);
    ~WarpSetupDialog() override;

    // The plan this dialog walks, for one machine: a host that already has a client skips the install,
    // and a device already enrolled skips the enrolment. Read once at construction and not again, so the
    // numbering the user watches cannot change underneath them mid-run.
    QList<WarpManager::SetupStep> steps() const { return m_steps; }
    int stepCount() const { return m_steps.size(); }
    // Which step is in flight: -1 before the first and after the last.
    int currentStepIndex() const { return m_index; }
    // The status line as the user reads it — "Шаг 3 из 6: Регистрация устройства…". Pure and static so a
    // test can pin the wording and the numbering without building a dialog.
    static QString stepStatusText(int oneBasedIndex, int count, const QString &stepTitle);
    // The sentence for one step id, from the catalog. Empty for an id this build does not know, which is
    // the case a test wants to see rather than have papered over with a fallback.
    static QString stepTitle(const QString &stepId);
    bool succeeded() const { return m_succeeded; }
    bool cancelled() const { return m_cancelled; }

    // The single-window policy, handed in by the window that owns it rather than read here: Settings
    // belongs to the UI and this dialog is told what the user chose, exactly as WarpInstaller is. Without
    // it the wizard would install a client and then leave its window open beside this one, which is the
    // thing the setting exists to prevent.
    void setHideWarpGui(bool hide) { m_hideWarpGui = hide; }

    // The widgets the design promises, exposed because a wizard's shape is a claim of its own: an intro,
    // one bar, one status line, one log, and a single primary button whose word changes with the state.
    QLabel *statusLabel() const { return m_status; }
    QProgressBar *progressBar() const { return m_progress; }
    QPlainTextEdit *logView() const { return m_log; }
    QPushButton *startButton() const { return m_start; }
    QPushButton *cancelButton() const { return m_cancel; }
    QPushButton *closeButton() const { return m_close; }
    // Whether the log pane is on screen. Hidden until the run starts, because an empty box with a border
    // around nothing is furniture rather than information.
    bool logVisible() const;

    // What the primary button presses. Public so a suite can drive the chain the way a user does, and so
    // a caller can start it without faking a click.
    void startSetup();

signals:
    // Emitted once, when the chain ends: true if every step succeeded. The dialog does not close itself —
    // the user reads the last line and presses Close, which is the difference between a result and a
    // window that vanished while they were looking at it.
    void setupFinished(bool ok);

protected:
    // ---- the three seams ----
    // Each returns false when the work could not even be started, with the reason in `error`. A subclass
    // that overrides them drives the wizard without a package manager, a daemon or a root prompt; the
    // base implementations are the real ones.
    virtual bool launchCommand(const WarpManager::SetupCommand &command, QString *error);
    virtual bool beginInstallStep(QString *error);
    virtual bool beginRegistrationStep(QString *error);

    // How a step's work reports back. Protected rather than private because it is the other half of the
    // seam: a suite that overrides the launchers has to be able to answer them.
    void commandAnswered(int exitCode, const QString &output, bool crashed);
    void stepAnswered(bool ok, const QString &detail);

private slots:
    void onCancel();

private:
    // QDialog's escape route, sent through onCancel() so Esc obeys the same rule the Cancel button does:
    // a package manager mid-step is not a step the user may interrupt.
    void reject() override;
    void build();
    void runStep();
    // The current step's next command, or the step's own completion when it has run out of them. Split
    // from runStep() because a step can need two commands and only the step knows when it is finished.
    bool runStepCommand(const WarpManager::SetupStep &step);
    void advance();
    // Ask the host again what it has, immediately after the step that changed the answer, and say what
    // came back. Its own method because the two halves — the rescan and the sentence about it — belong
    // together, and because a caller that rescans silently is how the next step ends up refused for a
    // reason nobody can see.
    void rescanAfterInstall();
    void failStep(const QString &detail);
    void finish(bool ok);
    // The log's two voices. A command's own output is quoted as it arrived; a failure is coloured, which
    // is the one place this dialog uses colour as information rather than as decoration.
    void appendLog(const QString &text);
    void appendError(const QString &text);
    void paintStatus();
    // Rename the status line and make the sheet re-read it. Split out because the two halves are one
    // operation: a name the stylesheet never re-polishes is a name that changed nothing on screen.
    void setStatusCode(const QString &objectName);
    // Resolve one planned command into the program and argv this host can actually run: warp-cli by the
    // path the manager resolved, anything else through PATH, and a privileged one through the same gate
    // every other privileged command in this application goes through.
    bool resolveCommand(const WarpManager::SetupCommand &command, QString *program,
                        QStringList *args, QString *error);

    WarpManager &m_manager;
    WarpInstaller *m_installer = nullptr;
    QProcess *m_process = nullptr;    // the command in flight, non-null only while it runs
    QList<WarpManager::SetupStep> m_steps;
    int m_index = -1;
    // Which command of the current step is running: a step can need two, because a service control
    // manager has no restart verb and no shell is allowed to invent one.
    int m_commandIndex = 0;
    bool m_running = false;
    bool m_succeeded = false;
    bool m_cancelled = false;
    bool m_hideWarpGui = true;   // the setting's own default, until the window hands over the real one
    // The enrolment step is the manager's, so its answer and its journal lines arrive through signals
    // that outlive the step. Both connections are held and broken before being remade, because a wizard
    // opened twice must not answer the first run's signal into the second run's log.
    QMetaObject::Connection m_registrationWatcher;
    QMetaObject::Connection m_managerLogWatcher;
    // The reason behind the manager's last error-level journal line, kept only for the length of an
    // enrolment step. registrationFinished() carries a bool and nothing else, so without this the wizard
    // reports a failure whose cause it was told about and then threw away — and a refusal that names the
    // daemon never reaches the classifier that would have turned it into a command the user can run.
    QString m_lastManagerError;

    QLabel *m_title = nullptr;
    QLabel *m_intro = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_close = nullptr;
};

} // namespace zapret
