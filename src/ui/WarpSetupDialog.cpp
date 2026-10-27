#include "WarpSetupDialog.h"

#include "LanguageManager.h"
#include "PrivilegeHelper.h"
#include "Theme.h"
#include "WarpInstaller.h"
#include "ZapretSource.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace zapret {

namespace {
// The wizard's own width: wide enough for a command line and the tail of a package manager's output to be
// readable without wrapping every two words, narrow enough not to cover the window that opened it — the
// user should still be able to see the dashboard behind a dialog about the dashboard.
constexpr int kDialogWidthPx = 560;
// The log pane, in lines of the sheet's own monospace. Four or five is what a step actually prints; more
// pushes the buttons off a small screen, and the journal keeps the full text afterwards anyway.
constexpr int kLogLines = 6;
// A command whose plan named no deadline still gets one, because a wizard that can be stranded by a
// wedged child is a window that has to be killed. Generous: this is the fallback, not the policy.
constexpr int kFallbackCommandTimeoutMs = 120000;
// How much of a command's own output is worth keeping. A package manager prints two hundred lines to say
// one thing went wrong, and the pane is a summary — the tail is where the reason is.
constexpr int kOutputTailChars = 400;
constexpr int kErrorDetailChars = 300;
} // namespace

WarpSetupDialog::WarpSetupDialog(WarpManager &manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    // The plan is read once, from what this machine says about itself right now. Re-reading it mid-run
    // would let the numbering move under the user — "step 3 of 5" becoming "step 3 of 4" because an
    // install just finished is the kind of thing that reads as the program losing track of itself.
    //
    // isInstalled() is deliberately the conservative input for "does this machine need an install": it
    // answers yes while the installation round is still asking package databases, so a wizard opened in
    // that first second skips the install step. The alternative — treating an unfinished round as
    // "nothing here" — would run a package manager over a client the machine may well have. The cost of
    // being conservative is that the later steps fail with "warp-cli was not found on this host" and the
    // user presses the retry that is on screen; the cost of being eager is an install nobody needed.
    m_steps = WarpManager::setupSteps(ZapretSource::hostPlatform(), manager.isInstalled(),
                                      manager.registrationState());
    build();
}

WarpSetupDialog::~WarpSetupDialog()
{
    // A command still running when the window goes away is a child process nobody will read. Killing it
    // is the honest ending for every step but the package manager's — and that one cannot be cancelled by
    // the user in the first place, so the only way to reach this line mid-install is the application
    // itself being torn down.
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(200);
        m_process = nullptr;
    }
}

void WarpSetupDialog::build()
{
    setObjectName(QStringLiteral("warpSetupDialog"));
    setWindowTitle(LanguageManager::tr(QStringLiteral("warp.setup.title")));
    setModal(true);
    setMinimumWidth(kDialogWidthPx);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(24, 20, 24, 20);
    column->setSpacing(12);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    i18nText(m_title, QStringLiteral("warp.setup.title"));
    column->addWidget(m_title);

    m_intro = new QLabel(this);
    m_intro->setObjectName(QStringLiteral("muted"));
    m_intro->setWordWrap(true);
    i18nText(m_intro, QStringLiteral("warp.setup.intro"));
    column->addWidget(m_intro);

    // The running half, hidden until something is running: a bar at zero and a status line saying nothing
    // are two widgets asking to be explained before the user has agreed to anything.
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("setupStatus"));
    m_status->setWordWrap(true);
    m_status->hide();

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("setupProgress"));
    m_progress->setRange(0, qMax(1, m_steps.size()));
    m_progress->setValue(0);
    // No percentage inside the bar: five steps make every value a multiple of twenty, and a number that
    // can only be 0, 20, 40, 60, 80 or 100 is a step counter wearing a disguise. The status line already
    // says which step this is.
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName(QStringLiteral("setupLog"));
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    // Bounded, because a package manager can print a great deal and this pane is a summary rather than
    // the record — the journal keeps everything.
    m_log->setMaximumBlockCount(400);
    m_log->setFixedHeight(kLogLines * 18 + 16);
    m_log->hide();

    column->addWidget(m_status);
    column->addWidget(m_progress);
    column->addWidget(m_log);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 4, 0, 0);
    buttons->setSpacing(10);

    m_cancel = new QPushButton(this);
    m_cancel->setObjectName(QStringLiteral("ghostButton"));
    m_cancel->setCursor(Qt::PointingHandCursor);
    i18nText(m_cancel, QStringLiteral("common.cancel"));

    m_start = new QPushButton(this);
    m_start->setObjectName(QStringLiteral("primaryButton"));
    m_start->setCursor(Qt::PointingHandCursor);
    m_start->setDefault(true);
    i18nText(m_start, QStringLiteral("warp.setup.start"));

    m_close = new QPushButton(this);
    m_close->setObjectName(QStringLiteral("primaryButton"));
    m_close->setCursor(Qt::PointingHandCursor);
    i18nText(m_close, QStringLiteral("warp.setup.close"));
    m_close->hide();

    buttons->addStretch(1);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_start);
    buttons->addWidget(m_close);
    column->addLayout(buttons);

    connect(m_start, &QPushButton::clicked, this, &WarpSetupDialog::startSetup);
    connect(m_cancel, &QPushButton::clicked, this, &WarpSetupDialog::onCancel);
    connect(m_close, &QPushButton::clicked, this, [this] {
        // Accepted only when the chain completed: the caller reads this code to decide whether to trust
        // the tunnel, and a window closed by hand after a failure is not a machine that was set up.
        done(m_succeeded ? QDialog::Accepted : QDialog::Rejected);
    });
}

// Esc is a cancel, and Cancel is disabled for exactly as long as a package manager holds the step — so
// the key has to inherit that rule rather than be a second way to interrupt an apt that must not be
// interrupted. Routing QDialog's own escape through onCancel() is what makes the two agree.
void WarpSetupDialog::reject()
{
    onCancel();
}

QString WarpSetupDialog::stepTitle(const QString &stepId)
{
    // One key per step id, and the ids come from the plan in WarpManager rather than from a list kept
    // here. A step the catalog has no sentence for gets an empty title, which a test can see: falling back
    // to the id itself would hide exactly the mistake worth catching — a step renamed in the core and
    // forgotten in the catalog.
    if (stepId == QLatin1String("install"))
        return LanguageManager::tr(QStringLiteral("warp.setup.step.install"));
    if (stepId == QLatin1String("register"))
        return LanguageManager::tr(QStringLiteral("warp.setup.step.register"));
    if (stepId == QLatin1String("protocol"))
        return LanguageManager::tr(QStringLiteral("warp.setup.step.protocol"));
    if (stepId == QLatin1String("service"))
        return LanguageManager::tr(QStringLiteral("warp.setup.step.service"));
    if (stepId == QLatin1String("connect"))
        return LanguageManager::tr(QStringLiteral("warp.setup.step.connect"));
    return QString();
}

QString WarpSetupDialog::stepStatusText(int oneBasedIndex, int count, const QString &title)
{
    return LanguageManager::tr(QStringLiteral("warp.setup.step"))
        .arg(oneBasedIndex)
        .arg(count)
        .arg(title);
}

bool WarpSetupDialog::logVisible() const
{
    return m_log && m_log->isVisible();
}

// ---- the chain ----

void WarpSetupDialog::startSetup()
{
    if (m_running)
        return;   // the button is hidden while a step runs; this is the belt to that brace

    // A retry continues from the step that failed instead of starting the chain over: re-running an
    // install that succeeded is a package manager over a working client, and re-enrolling a device that
    // enrolled a moment ago is a second key with the first one orphaned. m_index is left where the
    // failure put it, which is what makes "Повторить" mean what it says.
    if (m_index < 0)
        m_index = 0;
    m_commandIndex = 0;
    m_running = true;
    m_cancelled = false;

    m_start->hide();
    m_close->hide();
    m_status->show();
    m_progress->show();
    m_log->show();
    // The intro has done its job — the user agreed — and leaving it above a running log is two paragraphs
    // competing for the same attention.
    m_intro->hide();

    runStep();
}

void WarpSetupDialog::runStep()
{
    if (m_index >= m_steps.size()) {
        finish(true);
        return;
    }
    const WarpManager::SetupStep step = m_steps.at(m_index);
    paintStatus();

    // Cancel is offered only where it can be honoured: a step whose work is a package manager cannot be
    // interrupted without leaving a database to repair, and a button that appears to offer that is worse
    // than no button at all.
    m_cancel->setEnabled(step.cancellable);

    QString err;
    bool started = false;
    switch (step.kind) {
    case WarpManager::SetupKind::Install:
        started = beginInstallStep(&err);
        break;
    case WarpManager::SetupKind::Registration:
        started = beginRegistrationStep(&err);
        break;
    case WarpManager::SetupKind::Command:
        started = runStepCommand(step);
        break;
    }
    if (!started)
        failStep(err);
}

bool WarpSetupDialog::runStepCommand(const WarpManager::SetupStep &step)
{
    if (m_commandIndex >= step.commands.size()) {
        advance();   // every command this step needed has answered
        return true;
    }
    const WarpManager::SetupCommand command = step.commands.at(m_commandIndex);
    // Written here rather than inside launchCommand(), so the pane says what is about to run no matter
    // who runs it: the launcher is a seam a test replaces, and a log that only the real one writes is a
    // log that goes quiet exactly when somebody is watching it. The elevation prefix is the host's
    // business and is not part of the sentence — the plan's own spelling is what the user agreed to.
    appendLog(QStringLiteral("$ %1 %2").arg(command.program, command.args.join(QLatin1Char(' '))));

    QString err;
    if (!launchCommand(command, &err)) {
        failStep(err);
        return false;
    }
    return true;
}

bool WarpSetupDialog::resolveCommand(const WarpManager::SetupCommand &command, QString *program,
                                     QStringList *args, QString *error)
{
    // warp-cli by the path the manager already resolved — an MSI install lives off PATH, and this dialog
    // has no business doing that lookup a second time and getting a different answer. Everything else
    // through PATH, because a plan that hardcodes /usr/bin/systemctl is a plan that breaks on the
    // distribution which put it somewhere else.
    QString resolved = command.program == WarpManager::warpCliProgram()
        ? m_manager.resolvedProgram()
        : QStandardPaths::findExecutable(command.program);
    if (resolved.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1 was not found on this host").arg(command.program);
        return false;
    }
    if (!command.privileged) {
        *program = resolved;
        *args = command.args;
        return true;
    }
    // The same gate every privileged command in this application goes through, so "already root means no
    // pkexec" and "no pkexec means say so in words" are decided in one place and tested once.
    return WarpManager::composePrivilegedCommand(ZapretSource::hostPlatform(), resolved, command.args,
                                                 QStandardPaths::findExecutable(
                                                     QStringLiteral("pkexec")),
                                                 PrivilegeHelper::isElevated(), program, args, error);
}

bool WarpSetupDialog::launchCommand(const WarpManager::SetupCommand &command, QString *error)
{
    if (m_process) {
        if (error)
            *error = QStringLiteral("a previous command is still running");
        return false;
    }
    QString program;
    QStringList args;
    if (!resolveCommand(command, &program, &args, error))
        return false;

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(program);
    proc->setArguments(args);
    // Nothing is written to this child's stdin and the write channel is never closed here. The password
    // prompt of a privileged step belongs to a polkit agent in another window, not to this pipe, and
    // closing the channel is how a caller "helpfully" turns that prompt into an immediate EOF — which is
    // one of the two ways the automatic path was reported to stall.
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        const QString chunk = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        if (!chunk.isEmpty())
            appendLog(chunk);
    });
    connect(proc, &QProcess::finished, this, [this, proc](int exitCode, QProcess::ExitStatus status) {
        if (m_process != proc)
            return;
        m_process = nullptr;
        const QString tail = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        proc->deleteLater();
        commandAnswered(exitCode, tail, status == QProcess::CrashExit);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        // FailedToStart is the only error that never produces finished(); without this branch a command
        // that could not be exec'd would leave the wizard waiting on an answer that cannot arrive.
        if (e != QProcess::FailedToStart || m_process != proc)
            return;
        m_process = nullptr;
        const QString text = proc->errorString();
        proc->deleteLater();
        commandAnswered(-1, text, true);
    });

    m_process = proc;
    proc->start();
    // One deadline per command, generous for the step a human has to authorise: pkexec sits waiting for a
    // polkit agent while nobody is at the keyboard, and killing that after thirty seconds reads as a
    // program that broke rather than as a prompt that was missed.
    const int timeout = command.timeoutMs > 0 ? command.timeoutMs : kFallbackCommandTimeoutMs;
    QTimer::singleShot(timeout, proc, [this, proc, timeout] {
        if (m_process != proc)
            return;   // it answered on its own
        appendError(QStringLiteral("timed out after %1 s — killed").arg(timeout / 1000));
        proc->kill();
    });
    return true;
}

bool WarpSetupDialog::beginInstallStep(QString *error)
{
    if (!m_installer) {
        m_installer = new WarpInstaller(this);
        // The single-window policy is the window's decision, handed over before exec(); the installer
        // reads it when the package has landed and the vendor's own GUI could come up beside ours.
        m_installer->setHideWarpGui(m_hideWarpGui);
        // A client that is present and broken is what makes this a repair, and the installer's own repair
        // script is the thing that clears a dpkg transaction apt will otherwise refuse to touch.
        m_installer->setRepair(m_manager.installLooksBroken());
        // Every line it writes goes to the pane as well as to the journal: this is the step whose output
        // the user is most likely to need, because it is the one that talks to a package manager.
        connect(m_installer, &WarpInstaller::installationProgress, this,
                [this](const QString &line) { appendLog(line); });
        connect(m_installer, &WarpInstaller::installationFinished, this,
                [this](bool ok, const QString &err) { stepAnswered(ok, err); });
    }
    // A refusal here happens before anything ran — no root to ask for, no shell, no suite name — and its
    // message says which, so the user gets that sentence rather than a bare "install failed".
    return m_installer->start(error);
}

bool WarpSetupDialog::beginRegistrationStep(QString *error)
{
    // Delegated rather than spelled out as `registration new`, and the reason is the four readings:
    // ensureRegistered() reads the device first, deletes a conflicting enrolment, and only then enrols.
    // Doing it by hand here would mean a second key for a device that already has one.
    disconnect(m_registrationWatcher);
    m_registrationWatcher = connect(&m_manager, &WarpManager::registrationFinished, this,
                                    [this](bool ok) { stepAnswered(ok, QString()); });
    disconnect(m_managerLogWatcher);
    m_managerLogWatcher = connect(&m_manager, &WarpManager::logMessage, this,
                                  [this](const QString &line, int) { appendLog(line); });
    if (!m_manager.ensureRegistered(error)) {
        disconnect(m_registrationWatcher);
        disconnect(m_managerLogWatcher);
        return false;
    }
    return true;
}

// ---- the answers ----

void WarpSetupDialog::commandAnswered(int exitCode, const QString &output, bool crashed)
{
    if (!m_running || m_cancelled)
        return;   // a command that outlived the wizard's interest in it

    const WarpManager::SetupStep step = m_index >= 0 && m_index < m_steps.size()
                                            ? m_steps.at(m_index)
                                            : WarpManager::SetupStep();
    if (crashed || exitCode != 0) {
        // The command's own words are the useful half of a failure, quoted as they arrived and only the
        // tail of them: the reason is at the end, and the pane is not the place for two hundred lines.
        const QString detail = output.isEmpty()
            ? (crashed ? QStringLiteral("the command was killed or crashed")
                       : QStringLiteral("exit %1 with no output").arg(exitCode))
            : output.right(kErrorDetailChars);
        stepAnswered(false, detail);
        return;
    }
    if (!output.isEmpty())
        appendLog(output.right(kOutputTailChars));

    // One command of a step answered. A step with more than one — Windows' service stop and start — runs
    // its next; a step with one is finished and the chain moves on.
    ++m_commandIndex;
    runStepCommand(step);
}

void WarpSetupDialog::stepAnswered(bool ok, const QString &detail)
{
    if (!m_running || m_cancelled)
        return;
    disconnect(m_registrationWatcher);
    disconnect(m_managerLogWatcher);
    if (!ok) {
        failStep(detail);
        return;
    }
    // The step that just finished may have changed what this machine has, and the manager learned
    // everything it knows about the host when it was constructed — before that happened.
    if (m_index >= 0 && m_index < m_steps.size()
        && m_steps.at(m_index).kind == WarpManager::SetupKind::Install)
        rescanAfterInstall();
    advance();
}

void WarpSetupDialog::rescanAfterInstall()
{
    // Asked here, at the one moment in the chain where the answer is guaranteed to have changed, and
    // said out loud either way. Without this the next step is refused with "warp-cli is not installed on
    // this machine" — a true sentence about the machine as it was before the user's own package manager
    // ran, and the last thing anybody wants to read directly under a step that says the install worked.
    appendLog(LanguageManager::tr(QStringLiteral("warp.setup.rescan")));
    m_manager.rescanAvailability();
    const QString found = m_manager.resolvedProgram();
    if (found.isEmpty()) {
        // Red, and before the step that is about to fail rather than instead of it: the package reported
        // success and nothing drivable came of it, which is a diagnosis the next step's own error cannot
        // give. The chain still runs — a client that only a package database can see is not this
        // dialog's call to make — but the user is not left to infer why.
        appendError(LanguageManager::tr(QStringLiteral("warp.setup.rescan_missing")));
        return;
    }
    appendLog(LanguageManager::tr(QStringLiteral("warp.setup.rescan_found")).arg(found));
}

void WarpSetupDialog::advance()
{
    // The bar counts completed steps, so it moves when a step is done rather than when the next one
    // begins: a bar that filled on arrival would claim the work was finished before it started.
    m_progress->setValue(qMin(m_index + 1, m_progress->maximum()));
    ++m_index;
    m_commandIndex = 0;
    runStep();
}

void WarpSetupDialog::failStep(const QString &detail)
{
    m_running = false;
    const QString title = m_index >= 0 && m_index < m_steps.size()
                              ? stepTitle(m_steps.at(m_index).id)
                              : QString();
    const QString sentence = LanguageManager::tr(QStringLiteral("warp.setup.error"))
                                 .arg(m_index + 1)
                                 .arg(detail.isEmpty() ? title : detail);
    appendError(sentence);
    m_status->setText(sentence);
    setStatusCode(QStringLiteral("setupStatusFailed"));
    // The primary button comes back as a retry of THIS step, and Cancel is offered again: after a failure
    // the user's options are "try that again" and "stop", and a dialog offering neither is one that has
    // to be killed from a terminal.
    i18nText(m_start, QStringLiteral("warp.setup.retry"));
    m_start->show();
    m_cancel->setEnabled(true);
    emit setupFinished(false);
}

void WarpSetupDialog::finish(bool ok)
{
    m_running = false;
    m_succeeded = ok;
    disconnect(m_registrationWatcher);
    disconnect(m_managerLogWatcher);
    m_progress->setValue(m_progress->maximum());
    m_status->setText(LanguageManager::tr(QStringLiteral("warp.setup.done")));
    setStatusCode(QStringLiteral("setupStatusDone"));
    appendLog(LanguageManager::tr(QStringLiteral("warp.setup.done")));
    m_cancel->hide();
    m_start->hide();
    m_close->show();
    emit setupFinished(true);
}

void WarpSetupDialog::onCancel()
{
    if (m_succeeded) {
        done(QDialog::Accepted);   // nothing left to cancel; the window's work is finished
        return;
    }
    if (m_running && m_index >= 0 && m_index < m_steps.size()
        && !m_steps.at(m_index).cancellable) {
        // A package manager mid-transaction. The button is disabled for exactly this case, but Esc and a
        // window-manager close both arrive here, and the answer has to be a sentence rather than a
        // half-finished apt and a database to repair.
        appendError(QStringLiteral("cannot interrupt the package manager — it would leave a database "
                                   "to repair"));
        return;
    }

    m_cancelled = true;
    m_running = false;
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    disconnect(m_registrationWatcher);
    disconnect(m_managerLogWatcher);
    const QString where = m_index >= 0 && m_index < m_steps.size()
                              ? stepTitle(m_steps.at(m_index).id)
                              : QString();
    appendLog(LanguageManager::tr(QStringLiteral("warp.setup.cancelled")).arg(where));
    emit setupFinished(false);
    done(QDialog::Rejected);
}

// ---- what the user reads ----

void WarpSetupDialog::setStatusCode(const QString &objectName)
{
    // Setting the name is not enough and this is the mistake the rest of the codebase documents: QSS
    // never re-reads a widget on its own, so a status line renamed from "running" to "failed" keeps the
    // colour it was polished with until something else happens to repaint it. The unpolish/polish pair is
    // what SettingsPage::setPathState() and ProfileCard's [selected] do for the same reason.
    m_status->setObjectName(objectName);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->update();
}

void WarpSetupDialog::paintStatus()
{
    if (m_index < 0 || m_index >= m_steps.size())
        return;
    const WarpManager::SetupStep step = m_steps.at(m_index);
    m_status->setText(stepStatusText(m_index + 1, m_steps.size(), stepTitle(step.id)));
    setStatusCode(QStringLiteral("setupStatus"));
    m_log->moveCursor(QTextCursor::End);
}

void WarpSetupDialog::appendLog(const QString &text)
{
    if (!m_log || text.isEmpty())
        return;
    m_log->appendPlainText(text);
    m_log->moveCursor(QTextCursor::End);
}

void WarpSetupDialog::appendError(const QString &text)
{
    if (!m_log || text.isEmpty())
        return;
    // Colour as information, and the only place this dialog uses it: a failure has to be findable by
    // scanning the pane rather than by reading every line a package manager printed. The colour is the
    // theme's own, so the wizard's red is the red the journal and the profile rows fail in.
    m_log->appendHtml(QStringLiteral("<span style=\"color:%1;\">%2</span>")
                          .arg(Theme::error().name(QColor::HexRgb), text.toHtmlEscaped()));
    m_log->moveCursor(QTextCursor::End);
}

} // namespace zapret
