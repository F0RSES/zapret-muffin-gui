#pragma once

#include "IZapretBackend.h"
#include "ModeToggle.h"    // ModeSelection is the pair of halves the window runs
#include "PingIndicator.h" // PingIndicator::PingStatus is stored per host for seeding new rows mid-cycle
#include "PowerButton.h"   // PowerButton::Mode is the return type of the state -> look mapping
#include "WarpManager.h"   // WarpManager::State is the tunnel's view of itself

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>

class QAction;
class QCloseEvent;
class QFrame;
class QHBoxLayout;
class QJsonObject;
class QLabel;
class QMenu;
class QMouseEvent;
class QProgressBar;
class QProgressDialog;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QSystemTrayIcon;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace zapret {

class BlockcheckEngine;
class HistoryPage;
class HistoryStore;
class LogPanel;
class ModeToggle;
class PingIndicator;
class PingService;
class ProfileCard;
class Settings;
class SettingsPage;
class UpdateChecker;
class WarpInstaller;
class WarpManager;
class ZapretManager;

// The application window: a narrow icon rail down the left, one full-width page at a time to its right,
// everything driven through ZapretManager + Settings.
//
// The rail is 70 px of glyphs only — brand at the top, the profiles, the journal and the history in the
// middle, the settings glyph and the version at the foot. The page beside it is the dashboard: a header
// naming the app and its connection state, one enormous power button that starts and stops the chosen
// profile, and a row of neon ping badges. Profile rows are the list you pick from — one row per strategy,
// each carrying the services it exists to unblock and a menu for the two things that act on the entry
// itself — and starting is the dashboard's job alone, so a row never carries a second power button.
//
// It owns no domain logic. Rows come from the manager's catalog (which is the parser's output), the
// start/stop path is the manager's, and persisted values are Settings'. What lives here is presentation
// state and cadence: which row is selected, what the title says, whether closing hides to the tray, which
// page the rail is showing, and the watchdog that re-asks a live profile every few minutes whether it
// still bypasses — the one thing here that acts without being asked to. The window is frameless, so the
// header's two chrome buttons and the drag that starts on the header are drawn and driven here too, and so
// is the window's shape: a rounded shell with a transparent band around it, since a frameless window with a
// background is still a rectangle.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    // The same window with its tunnel control handed in rather than built.
    //
    // Why a constructor and not a virtual factory: the window needs the control before its first
    // repaint — the pill's halves are clamped against whether warp-cli exists on this host, and the
    // chip is drawn from the tunnel's state — so the object has to be there inside the constructor
    // body. A virtual `createWarpManager()` would not answer there: a call made while constructing
    // a base class resolves to the base, and a suite would silently get the real warp-cli driver it
    // was trying to keep away from a test binary. Taking the dependency as an argument is the one
    // form that is honest about being decided before the window exists.
    //
    // `warp` is non-null only in a headless suite; production goes through the parent-only form
    // above and builds its own. The window adopts a supplied control exactly as it adopts one it
    // made — parented, and deleted with itself — so there is one owner on either path and a test
    // that passes a heap-allocated mock never has to wonder who frees it. A null `warp` is not an
    // absent tunnel: it means "build the real one", which is why this form takes `parent` with no
    // default — a bare `MainWindow(nullptr)` would be the one call that could mean either ctor.
    MainWindow(WarpManager *warp, QWidget *parent);
    ~MainWindow() override;

    // Text/colour of the toolbar status chip for a backend state. Static so the mapping is testable.
    static QString statusText(IZapretBackend::State state, const QString &strategyName);
    // The header's one-word chip for a state ("CONNECTED", "OFFLINE"). Uppercase and English on purpose:
    // it is a design element beside a 6 px dot, not a sentence, and the mock spells it that way. The
    // longer, strategy-naming form stays in statusText() for the tray tooltip and the log.
    static QString connectionText(IZapretBackend::State state);
    // QSS dynamic-property value for [statusKind="..."].
    static QString statusKind(IZapretBackend::State state);

    // The power button's look for a backend state: green to act on a live run, purple and breathing
    // while a transition is in flight, plain when there is nothing to stop. Static because two call
    // sites draw this button — the state-changed path, and the Stop press that answers ahead of the
    // backend (see showStoppingFeedback) — and one state must not be able to mean two looks.
    static PowerButton::Mode powerMode(IZapretBackend::State state);

    // ---- the WARP branch of the same three mappings ------------------------------
    //
    // WarpManager::State covers six cases, of which Unavailable and Error have no direct
    // equivalent in IZapretBackend's five — a backend that cannot be started is a startup
    // failure, whereas WARP whose daemon is not installed is a state the interface has to sit in
    // indefinitely. Kept next to their zapret counterparts so a maintainer adding a new backend
    // state sees immediately that the WARP table has to grow with it.
    static QString connectionText(WarpManager::State state);
    static QString statusKind(WarpManager::State state);
    static PowerButton::Mode powerMode(WarpManager::State state);
    // Whether the power button may act for a WARP state — the same reasoning as canStartFrom /
    // canStopFrom, applied to a tunnel that has to be brought up rather than launched.
    static bool canActFrom(WarpManager::State state);
    // The halves the pill may be left showing on a host that has no WARP client: the tunnel's half
    // goes dark and Zapret's stays exactly as it was. A stored pick that names a missing binary is
    // remembered by Settings (the intent survives) and clamped here (the widget must not light a
    // half nobody can press).
    static ModeSelection clampSelectionForAvailability(ModeSelection desired, bool warpAvailable);

    // Which question an offer asks: the ordinary "install it?" or the repair's "it is here and broken,
    // clear it and install again?". A key rather than a sentence because the sentence belongs to the
    // catalog, and because the choice between the two is the whole of the feature's promise — a
    // machine that already owns the client must never be told it does not. Static and pure so a test
    // can pin the pair without pressing anything.
    static QString installQuestionKey(bool repair);

    // ---- what a change of selection is allowed to do --------------------------------
    //
    // The pill says which subsystems the power button will control. Choosing is not starting: the
    // only press that brings a bypass UP is the button's own (see onPowerClicked), and a change of
    // selection may therefore only ever take a live one DOWN — and only after the user agreed to
    // that in words.
    //
    // There are two questions here, and both are pure so the whole rule is checkable without a
    // tunnel, a backend or a modal.
    //
    // Does this backend state hold the path? Starting, Running and Stopping all do: a launch in
    // flight is a run the user can still see being taken apart, and a teardown still has iptables
    // rules in the path until it finishes. Stopped and Error hold nothing.
    static bool isBypassHeld(IZapretBackend::State state);
    // Does this tunnel state hold the path? The same three cases on WARP's side, and Unavailable —
    // no warp-cli — is emphatically not one of them.
    static bool isTunnelHeld(WarpManager::State state);

    // What a switch still has to tear down. The type carries no "Start" member at all, which is the
    // point: a future edit cannot add a launch to the toggle's path without first adding an enum
    // value that every reader of this file will see was never there.
    enum class SwitchAction
    {
        None,        // nothing was running, or the user refused — move the pill, touch nothing else
        StopZapret,
        StopWarp,
        StopBoth     // a host can hold the path twice; the answer stops both
    };
    Q_ENUM(SwitchAction)
    static SwitchAction switchAction(bool zapretHeld, bool warpHeld, bool userAgreed);

    // Does moving from `from` to `to` take down something the user can see running? Only a half that
    // was lit, is going dark, and is actually held: lighting a dark half costs nothing, and turning
    // off a half with nothing behind it takes away nothing.
    static bool strandsAHeldSubsystem(ModeSelection from, ModeSelection to,
                                      bool zapretHeld, bool warpHeld);
    // …split by half, because the two teardowns a change can owe are issued by two different
    // managers and only this says which ones are owed at all.
    static bool takesBypassDown(ModeSelection from, ModeSelection to, bool zapretHeld);
    static bool takesTunnelDown(ModeSelection from, ModeSelection to, bool warpHeld);

    // Which halves end up lit. A refused question leaves the pill exactly where it was; everything
    // else — an idle host, an agreed change — takes the selection the user asked for. Keeping the
    // declined case in here rather than in the slot is what lets a test pin "no means no" without a
    // window.
    static ModeSelection resolveSelectionChange(ModeSelection current, ModeSelection requested,
                                                bool somethingHeld, bool userAgreed);

    // ---- the one button with two subsystems under it --------------------------------
    //
    // With both halves lit a press means one operation over two steps, and the two directions are
    // not mirror images. Coming up: zapret first, because the tunnel's packets only reach Cloudflare
    // through the queue rules the bypass installs — and then the tunnel, after those rules have had
    // time to settle. Going down: the tunnel first, because taking the floor out from under a live
    // tunnel leaves it holding an interface that no longer forwards anything.
    //
    // Which of those a press is, is a pure function of the two states, named so a test can pin the
    // whole table without a process, a daemon, or a wait.
    enum class CombinedPress
    {
        Blocked,        // a transition is in flight somewhere; a second press cannot help it
        ZapretThenWarp, // nothing is up: the bypass, then the tunnel kWarpStartDelayMs later
        WarpOnly,       // the bypass is up and the tunnel is not: finish what the pick asked for
        ZapretOnly,     // the tunnel is up and the bypass is not: bring the missing floor back
        WarpThenZapret  // both are up: the tunnel down first, then the bypass under it
    };
    Q_ENUM(CombinedPress)
    static CombinedPress combinedPressFor(IZapretBackend::State zapret, WarpManager::State tunnel);

    // How long the rules get to settle before the tunnel is asked to connect. Issued the instant a
    // start is accepted, warp-cli's connect can race the iptables commands that create the queue it
    // hands packets to; the symptom is a tunnel reporting Connected while nothing passes. One
    // number, spelled once, and read through warpStartDelayMs() so a test can shorten the wait
    // without shortening the reasoning.
    static constexpr int kWarpStartDelayMs = 3000;

    // Whether the delay may fire. The tunnel is only ever asked to connect while the half that
    // asked for it is still lit and the bypass it rides on is still holding the path — a run that
    // died in the meantime, a half the user turned back off, or a teardown that got there first all
    // mean the connect must not be issued. Pure, so the guard is checkable without a timer.
    static bool mayConnectWarpAfterDelay(ModeSelection selection, bool startStillPending,
                                         bool bypassHeld);

    // The look the chip and the ring wear while both halves are lit. Grouped in one struct because
    // the five things a state decides — the button's mode, its palette, the chip's word, the chip's
    // QSS token, and whether a press would be honoured — must come from one table: two renderers
    // that disagree about one state is how a header starts lying about a run.
    struct CombinedLook
    {
        PowerButton::Mode mode = PowerButton::Mode::Idle;
        PowerButton::Palette palette = PowerButton::Palette::Zapret;
        QString chipText;
        QString chipKind;
        bool pulsing = false;
        bool enabled = false;
    };
    // Which layer is live decides the ring's colour: the tunnel's blue while the tunnel is up or on
    // its way up, zapret's purple otherwise — so the ring says which of the two halves is actually
    // doing something, rather than claiming both are because both were picked.
    static CombinedLook combinedLook(IZapretBackend::State zapret, WarpManager::State tunnel);

    // ---- action gating: pure predicates again, so the whole matrix is checkable without a process ----
    //
    // A backend owns one operation at a time. LinuxBackend::start() refuses outright while anything is up
    // or coming up ("an operation is already in progress (stop first)"), and a portable stop takes as long
    // as the iptables teardown under pkexec needs — ten seconds in the log that exposed this. For all of
    // that time the window used to keep both the toolbar's "Остановить" and every card's "Запустить"
    // clickable, so the only honest answer was an error row and a failure badge on a card that had done
    // nothing wrong. Greyed out and worded is the fix; the guards in the slots below are the backstop.
    static bool canStartFrom(IZapretBackend::State state);
    static bool canStopFrom(IZapretBackend::State state);
    // Why a start cannot be issued in this state, empty when it can. Doubles as the tooltip of every
    // control the gating disables, so a greyed button says what it is waiting for.
    static QString blockedReason(IZapretBackend::State state);

    // ---- tray policy, expressed as pure predicates so it is testable without a desktop ----
    // Whether the platform reports a tray host at all (GNOME without an AppIndicator extension, a bare
    // Wayland compositor and a headless session all answer false).
    static bool traySupported();
    // Closing the window may only hide it when there is a tray to hide it into; otherwise the same
    // click has to quit, or the user loses their only way back to the window.
    static bool hideOnClose(bool minimizeToTraySetting, bool trayAvailable);

    // Which card a backend failure should be pinned to, from the three ids the window can name a run
    // by, in precedence order:
    //
    //   pendingStartId — the start this window asked for and that has not resolved yet;
    //   activeId       — the strategy the manager currently reports as running;
    //   lastRunId      — the last run the window saw as active, kept because a manager that has
    //                    already forgotten the run answers with "" (see m_lastRunId).
    //
    // The third case is not hypothetical: portable mode deliberately defers its error report until the
    // nfqueue teardown finished, and by then stateChanged(Error) has already made the manager drop the
    // active strategy, so the first two are both empty and the failure would otherwise badge nothing.
    static QString errorTargetId(const QString &pendingStartId, const QString &activeId,
                                 const QString &lastRunId);

    // ---- the ping cadence each row is allowed to follow ----------------------------------------
    //
    // The dashboard's badges and the rows' chips answer to different claims, so they are driven
    // differently. The badges say "this is how the path answers right now" — that is true on every
    // cycle, so they follow all of them. A row says "this is how the services this profile exists to
    // unblock are answering", which is only true while that profile is the one running: an idle entry
    // has nothing in the path to measure. An idle row therefore keeps the last verdict it was given
    // instead of quietly re-using the running profile's numbers, and the one moment every row may move
    // at once is the manual round the profiles page asks for.
    //
    // Both halves are pure so the whole rule is checkable without a backend, a network or a window.
    //
    // The hosts a row carries a chip for: the services zapret is used to unblock. The dashboard probes
    // one more — a baseline that answers with or without any strategy — and that number belongs to the
    // baseline reading, not to a claim about this profile.
    static QStringList profilePingHosts();
    // Whether a verdict from the service may repaint this row.
    static bool rowFollowsCycle(bool manualRound, bool rowIsActive);

    // The verdict sheet for one finished bypass check: a line per host in QJsonObject's own sorted
    // key order (so the sheet cannot reorder between two presses), "Discord: OK (45 ms)" or
    // "Youtube: FAIL", with the catalog's certificate warning appended to an OK whose TLS
    // certificate was rejected — the one line that says "intercepted" rather than "unreachable".
    // Static because the formatting is a policy of its own and checkable without a window.
    static QString blockcheckResultText(const QJsonObject &results);

    // ---- the watchdog: is the bypass still bypassing? ------------------------------------------
    //
    // A profile that worked at breakfast can stop working at tea time, and the only way anybody finds
    // out is to open the thing they were bypassing for. So the window re-asks one cheap question on a
    // timer, through the same BlockcheckEngine the profiles page's button drives, and when the answer
    // turns from OK to FAIL it says so in the journal, reaches the desktop through the tray, and offers
    // the next profile in the list.
    //
    // Everything that decides — whether to measure, what counts as a failure, which profile comes next —
    // is static and pure below, in the same spirit as canStartFrom() and errorTargetId(): the whole
    // policy is checkable without a backend, a network, a desktop or a window.
    //
    // The one host a quiet cycle probes. Deliberately one, and deliberately a service this app exists to
    // unblock: a background check must cost a second or two of traffic, not a round of three, and
    // google.com is the baseline that answers happily with no bypass at all — measuring it would let a
    // dead profile pass.
    static QString watchdogQuickHost();
    // minutes -> QTimer interval, clamped through the model's own range so a stored 0, a hand-edited
    // 900 or a future default cannot arm a timer that fires every tick or once a fortnight.
    static int watchdogIntervalMs(int minutes);
    // Whether a tick may measure now. Only a live run has a path for the answer to describe: probing
    // while stopped would measure the provider's default behaviour and call it the profile's. And one
    // engine serves one cycle at a time, whatever asked for it.
    static bool watchdogShouldProbe(IZapretBackend::State state, bool quietRunInFlight,
                                    bool manualCycleInFlight);
    // The FAIL that starts all of this: at least one verdict in the map, and not one host through. An
    // empty map is a cycle that broke (no manager, a refused start, a preset that never came up) — a
    // broken measurement is not evidence about the bypass, and must not wake anybody with a question.
    static bool watchdogCycleFailed(const QJsonObject &results);
    // The profile to rotate to: the entry after `currentId` in the window's own row order, wrapping at
    // the end. An empty answer means there is nothing else to try — the same profile brought back up is
    // not a rotation, it is a retry, and the watchdog's promise is a different profile.
    static QString watchdogNextStrategyId(const QList<Strategy> &strategies,
                                          const QString &currentId);

    // Accessors used by tests and by nothing else in the app.
    ZapretManager *manager() const { return m_manager; }
    LogPanel *logPanel() const { return m_log; }
    Settings *settingsModel() const { return m_settings; }
    int cardCount() const { return static_cast<int>(m_cards.size()); }
    // True when a tray icon was actually created (a DE without a tray host leaves this false).
    bool hasTray() const { return m_tray != nullptr; }
    // The record and the page that shows it. Hooks for the same reason as cardCount(): the promise of the
    // history page is that it follows the store, and a suite cannot prove that about two objects it has
    // no way to name.
    HistoryStore *historyStore() const { return m_history; }
    HistoryPage *historyPage() const { return m_historyPage; }
    // The card a failure was pinned to ("" when none). Test hook for the inline error badge.
    QString errorCardId() const;
    // The power button. A hook for the same reason as cardCount(): the pulse is the window's claim about
    // a backend state, and a suite cannot check a claim it has no way to look at.
    PowerButton *powerButton() const { return m_power; }
    // The dashboard's Zapret/WARP pill. Same reasoning as powerButton(): a suite cannot assert
    // which subsystems the window is pointed at unless the widget that decides it is reachable.
    ModeToggle *modeToggle() const { return m_modeToggle; }
    // Which halves the window is honouring — its own copy, not a read-through to the pill, which can
    // be mid-fade or mid-snap-back. The value the power button's meaning is decided from.
    ModeSelection selection() const { return m_selection; }
    // How long a combined press lets the bypass's rules settle before it asks the tunnel to connect,
    // and whether such a wait is running. Hooks for the same reason the power button is exposed: a
    // timer the suite cannot look at is a timer it cannot prove fired in the right order, and a
    // three-second wait is a long time for a headless test to spend proving one line of ordering.
    int warpStartDelayMs() const { return m_warpStartDelayMs; }
    void setWarpStartDelayMs(int ms);
    bool warpStartPending() const { return m_warpStartPending; }

    // The WARP manager. Exposed for the same reason the power button is: its state — available,
    // disconnected, connected — is a claim the window makes on screen, and reading it off the
    // widget alone would leave a test unable to say whether the toggle's colour matches the
    // tunnel it is supposed to represent.
    WarpManager *warpManager() const { return m_warp; }
    // Draw the chip and the button for one state, without a manager ever having been in it. The same
    // entry point showStoppingFeedback() uses — and for the same reason: a teardown is a state the
    // backend will not sit still for, so the only way to check what the window draws for it is to ask
    // for that drawing directly.
    void renderStatusFor(IZapretBackend::State state) { renderStatus(state); }
    // The watchdog's live cadence, and whether it is armed. Hooks for the same reason as cardCount():
    // a timer the suite cannot look at is a timer it cannot prove was re-armed when the setting moved.
    int watchdogTimerInterval() const;
    bool watchdogTimerActive() const;
    // The profile rows in the order the list draws them, which is not the order the hash holds them in.
    // The entrance cascade needs that order — the third row has to rise third — and a test that wants to
    // prove a row finished where the layout put it needs the same list to walk.
    QList<ProfileCard *> cardRows() const;

    // ---- the rounded shell --------------------------------------------------------------
    //
    // The window carries no system frame and, since the shell below, no background of its own either:
    // #mainWindowFrame in both sheets IS the silhouette the user sees. These three numbers are that
    // silhouette's geometry, and both sheets read them the way they read the palette — the card declares
    // kWindowFrameRadiusPx of arc and kWindowFrameBorderPx of stroke, and every surface that reaches a
    // window corner carries the arc's INNER edge (radius minus border) itself, because Qt does not clip a
    // child to its parent's radius: a square child corner paints over the stroke and out past the
    // silhouette, which under a translucent window is a nub on the desktop rather than a wrong colour
    // inside the app.
    static constexpr int kWindowFrameRadiusPx = 12;
    static constexpr int kWindowFrameBorderPx = 1;

    // The shadow the card casts. A rounded card on a translucent surface can sit so close to the desktop
    // that it reads as part of it: the 1 px stroke is the only thing between the app and whatever is behind
    // it, and a stroke is a thin claim to make on its own. The shadow is what says "this is a window, and it
    // is above your desktop".
    //
    // One value for both themes rather than a per-sheet one, and that is not an oversight: the shadow lands
    // on the desktop, whose colour this app neither knows nor styles. The palette has nothing to reconcile it
    // with, so it is declared here, in the code that owns the window's shape.
    static constexpr int kWindowShadowBlurPx = 24;
    static constexpr int kWindowShadowOffsetYpx = 4;
    static constexpr int kWindowShadowAlpha = 100;

    // The transparent band between the card and the surface's own edge. It has two jobs and the second one
    // is the bigger: it keeps the 1 px stroke off the window's last pixel (a stroke that begins on the last
    // pixel is a stroke cut in half — the frameless equivalent of the square corners
    // WA_TranslucentBackground exists to remove), and it is the only room the shadow above has to fall in.
    // QGraphicsDropShadowEffect blurs with a radius of half its blurRadius, and a Gaussian has no last
    // pixel: what the band buys is not a shadow that ends before the surface, but one that is down to a few
    // units of alpha by the time it gets there — invisible, where the same curve six pixels in is a step you
    // can see on a light desktop. Reach plus the offset is where that fade is spent for any purpose a user
    // has; narrow the band below it and testWindowFrame() starts failing on the ring.
    static constexpr int kWindowFrameMarginPx = kWindowShadowBlurPx / 2 + kWindowShadowOffsetYpx + 1;

    // The shell itself. A hook for the same reason powerButton() is one: the rounded silhouette is the
    // window's claim about its own shape, and a suite cannot read the pixels of a widget it cannot name.
    QFrame *windowFrame() const { return m_windowFrame; }

    // ---- the release check --------------------------------------------------------------
    //
    // Whether the application itself is the newest thing its own repository has released. Zapret is
    // downloaded and replaced by the download path; nothing until now ever asked about Muffin, so a
    // user running 0.4.1 had no way to learn that 0.4.9 existed unless they went looking.
    //
    // The lookup is not started by the constructor, and that is deliberate rather than an oversight:
    // the same rule DownloadDialog lives by is that the one method touching GitHub is only ever called
    // by hand, so a suite that constructs a window can never be depending on an answer, a network or a
    // rate limit. main.cpp arms the delay after the window is on screen; the delay exists because a
    // lookup issued while the first paint is still happening costs the startup the seconds it was
    // supposed to save.
    //
    // How long the window waits before asking the first time. Five seconds is after the interface has
    // settled and before anybody has finished reading the log's opening lines.
    static constexpr int kUpdateCheckDelayMs = 5000;
    // Arm that wait. Called once, from main.cpp, after the window is shown; calling it again asks the
    // feed again, which is the whole point of keeping it a method rather than a line in the constructor.
    void scheduleUpdateCheck();
    // The badge, for the same reason powerButton() is exposed: whether the offer is on screen is the
    // window's claim, and a suite cannot check a claim it has no way to look at.
    QToolButton *updateButton() const { return m_updateButton; }
    // True when the window has decided to show the badge. Deliberately NOT the widget's isVisible():
    // a badge on a window that is hidden in the tray is not "on screen" in Qt's sense even though the
    // offer is live, and a test of the decision has to be independent of whether a desktop exists.
    bool updateNoticeShown() const;
    // The version on offer, "" until an answer names one. Also the window's hook for asserting that the
    // number the badge was raised for is the number the dialog will name.
    QString updateVersion() const { return m_updateVersion; }

    // ---- rail navigation ------------------------------------------------------------
    // Pages are addressed by index so a test — and the unread-log alert below — can name the visible
    // one without walking the widget tree.
    //
    // PageProfiles is not reachable from the rail as its own glyph: it opens from the dashboard's route
    // pill, which is where the choice belongs — you pick a profile in order to run it. PageSettings, by
    // contrast, IS a rail glyph: the settings sheet became a page in this same stack so that the rail's
    // settings icon navigates rather than popping a modal, which is the whole argument of the rail.
    // PageHistory is a glyph for the same reason: the record of past checks is a place a user goes to
    // answer a question, not a dialog to dismiss.
    enum Page : int
    {
        PageDashboard  = 0,
        PageProfiles   = 1,
        PageLogs       = 2,
        PageHistory    = 3,
        PageSettings   = 4
    };
    int currentPage() const;
    void setCurrentPage(int page);
    // The page the stack is showing, as a widget. currentPage() names which one; this is the handle the
    // motion checks need, because a fade is a fact about a widget and not about an index. Defined out of
    // line for the same reason currentPage() is: the stack itself is only forward-declared here.
    QWidget *currentPageWidget() const;
    // The page to fall back to when a full-screen view is closed: always the dashboard, never whichever
    // page happened to be up before.
    Q_INVOKABLE void showDashboard();
    // Set while the journal is hidden and a warning or error arrives, cleared by opening it: the page
    // switch costs the user the passive view of the log, so something has to say "there is news here".
    bool logAlertActive() const;

    void refreshStrategies();
    // The rail's settings glyph and Ctrl+S both route here. Since SettingsDialog became SettingsPage the
    // method only switches the stack to the settings page; it no longer pops a modal. Kept as a named
    // slot (rather than a bare setCurrentPage call at each site) so the language-neutral entry point stays
    // one place and the startup "no strategies yet" prompt has something to defer to.
    void openSettings();
    // Fetch the newest release for this platform, install it into the managed directory, and point the
    // application at it. Reached from the toolbar, from SettingsPage and from the "no strategies"
    // placeholder — three entrances, one implementation, because a download that behaves differently
    // depending on which button opened it is the kind of bug nobody notices until an install fails.
    void downloadZapret();

    // Unhide and focus the window: show + raise + activate, in that order. Three call sites need it —
    // the tray's "Show" menu entry, a click on the tray icon itself, and main.cpp's single-instance
    // channel when a second launch reaches the running instance — and hand-copying the sequence into a
    // lambda at each of them is exactly how they start to disagree (the classic symptom being a tray
    // click that reveals the window but leaves another application focused). Public because it is the
    // entry point main.cpp wires into; nothing else in the class needs to reach it.
    void bringToFront();

public slots:
    // The release feed named a build ahead of this one. Raises the badge and puts it breathing; nothing
    // else, because an offer is not an event and a dialog that arrives on its own is the difference
    // between an updater and a nag. Public for the same reason onWatchdogResult is: the answer belongs
    // to a network that a suite must not depend on, so the suite hands the verdict in by hand.
    void onUpdateAvailable(const QString &version, const QString &downloadUrl);
    // The watchdog timer's answer. Guards first (watchdogShouldProbe), then asks the shared engine for
    // one quiet verdict on watchdogQuickHost() — quiet meaning the journal, the profiles-page meter and
    // the verdict sheet all stay out of it, because this cycle was not asked for and the window is most
    // likely hidden. Public (rather than a private slot) because a five-minute timer is not something a
    // test can wait for: the suite drives a tick by hand.
    void onWatchdogTick();
    // What a quiet cycle came back with — the engine's testFinished on the normal path, or a map handed
    // straight in by a test. Silent on a pass, on a cycle that produced no verdict at all, and on one
    // whose run came down while it was being measured; on a FAIL it says so in red, reaches the desktop,
    // and asks whether to try the next profile.
    void onWatchdogResult(const QJsonObject &results);

signals:
    // A quiet check said the live profile is dead and the user answered "yes" to switching. Emitted
    // before anything is touched — this is the intent, performWatchdogRotation() is the act, and the
    // difference is what lets a test watch the decision without a backend being asked to move.
    void watchdogRotationRequested(const QString &fromId, const QString &nextId);

protected:
    void closeEvent(QCloseEvent *event) override;
    // The window is frameless (see the constructor), so its own header is the only thing to grab: a press
    // there drags the whole window. A press anywhere else falls through to the usual widget behaviour.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    // ---- the watchdog's three seams ---------------------------------------------------
    //
    // The alert has exactly one part that needs a desktop and one part that needs a backend, and both
    // are answers rather than behaviour: what a test wants to check is the decision in between. Each is
    // virtual so a suite can answer it by hand instead of blocking on a modal, spawning pkexec, or
    // waiting on a network that may not be there.
    //
    // The measurement itself — the quiet cycle a tick asks for. The real one drives the engine the
    // power button shares; the verdict lands in onWatchdogResult() either way.
    virtual void startWatchdogProbe(const Strategy &strategy);
    // The question the user answers, naming the profile that died and the one being offered. The two
    // are spelled out because "the next profile" is a position in a list the user has to look at to
    // know what they are agreeing to.
    virtual bool confirmWatchdogRotation(const QString &fromName, const QString &nextName);
    // Bring this one down, bring that one up when it is down. One method rather than two calls at the
    // call site, because the pair is the unit: a rotation that only stopped the old profile is a
    // rotation that left the user with nothing running.
    virtual void performWatchdogRotation(const QString &fromId, const QString &nextId);

    // ---- the selection change's seams -------------------------------------------
    //
    // Moving the pill has one possible cost: a live subsystem has to come down first. Split the same
    // way the watchdog's rotation is — the question here, the acts in the slots — so a suite can
    // answer it by hand instead of blocking on a modal nobody out there can click.
    //
    // `from` and `to` are whole selections. The box shows what each of them adds up to
    // (ModeToggle::describe) in its informative line, so the seven translations of the question never
    // carry the name of a half.
    virtual bool confirmStopAndSwitch(ModeSelection from, ModeSelection to);

    // ---- the acts the one button performs ----------------------------------------
    //
    // A combined press is an ordering, and an ordering is only checkable if the things it orders can
    // be watched rather than run. Each seam below is the whole of one step: the base drives the real
    // subsystem, an override records that it was asked to. Split for the same reason the watchdog's
    // three seams are — proving which comes first must not require a payload, a daemon, or a root
    // prompt, none of which a headless suite has.
    virtual bool startBypass();       // the power button's own start path for the selected profile
    // Bring the tunnel up. Split into two because the half that makes it work is asynchronous: an
    // unenrolled client accepts `connect` and then never comes up, so this asks WarpManager to check
    // the enrolment first, and the tunnel is issued from the answer. Named as the request it is — a
    // true here means the check started, not that WARP is connecting.
    virtual void connectWarp();
    virtual void disconnectWarp();    // warp-cli disconnect, plus the disconnecting look
    // The connect half of connectWarp(), split out so the enrolment answer can issue it without every
    // caller having to know the two are separate round trips. This is the seam that actually reaches
    // warp-cli, so a suite that wants to prove the ordering can record this instead of running it.
    virtual void startWarpTunnel();
    // The step between "this device is enrolled" and "bring the tunnel up": put the machine into a
    // state where a connect can actually come up. Virtual for the same reason its two neighbours are —
    // the ordering across three asynchronous answers is the claim, and it is only provable by a suite
    // that can watch the acts instead of running them.
    virtual void prepareWarpEnvironment();
    // Whether the bypass is up, or on its way up and not being taken down. The wait's guard reads this
    // rather than the manager directly, so the delay can be proved to withhold a connect over a run
    // that never came up — the one failure the three seconds exist for.
    virtual bool bypassIsUpOrRising() const;


private slots:
    void onStopRequested();
    // The tray's "Stop" and Ctrl+Shift+X: everything the selection is holding, the tunnel first and
    // the bypass under it. Split from onStopRequested() because the two mean different things to
    // different callers — a watchdog rotation switching profiles has no business touching a tunnel
    // that is passing traffic happily — and one slot serving both is how a rotation quietly drops a
    // user's VPN.
    void onStopAllRequested();

    // The badge was pressed. Asks the one question and, on yes, hands the release page to the desktop's
    // browser — the extent of what this feature does without root, and the reason the answer is a
    // question rather than an action.
    void onUpdateBadgeClicked();
    // The power button: stop what the selection is running, otherwise start what it asks for. One
    // slot for both because the button is one control whose meaning the state decides — and, with
    // both halves lit, whose meaning is an order of operations rather than one operation.
    void onPowerClicked();
    void onStateChanged(IZapretBackend::State state);
    // The press when the tunnel is part of the selection. Reached from onPowerClicked; split out
    // because the combined path reads two managers and the zapret-only path reads one, and folding
    // them together is how a state check ends up on every line of both.
    void onCombinedPowerClicked();
    // The three seconds the bypass's rules were given are up. Guarded, so a run that never came up
    // or a half the user turned back off in the meantime does not get a connect issued for it.
    void onWarpStartDelayElapsed();
    // The user clicked one half of the Zapret/WARP pill. Persist the pair, retint the button, and
    // redraw the chip so the header describes what the button is about to control, not what it used
    // to control.
    void onSelectionChanged(bool zapretOn, bool warpOn);
    // The tunnel moved. Same redraw path as a backend state change, driven from the WARP side.
    void onWarpStateChanged(WarpManager::State state);
    // warp-cli appeared or disappeared while the window was open. Re-clamp the pill so a
    // mid-session uninstall does not leave the button pointed at a subsystem with no binary.
    void onWarpAvailabilityChanged(bool available);
    // The host finished answering whether it owns a WARP client this process cannot run. A yes is said
    // in the journal instead of being acted on; a no is where an offer that a press had to withhold
    // finally gets made, because the press that asked for it has long since returned.
    void onWarpInstallationChanged(bool installed, const QString &via);
    // The host has no WARP client and the user just asked for the tunnel. Offer to put one there:
    // install is the only way the WARP half of the pill can be honoured on this machine, and this is
    // the moment the absence is felt rather than a state the window sits in silently. Virtual for the
    // same reason the acts the button performs are: the question it asks is a modal, and a suite has
    // nobody to click it.
    virtual void offerWarpInstall();
    // The setup wizard: install, enrol, switch the transport, restart the daemon and connect, in that
    // order and in the open. Offered instead of an install when the machine has no client at all or has
    // one that was never enrolled, because in those two cases an install is only the first of four
    // things that have to happen, and doing the other three silently is how the user ends up with a lit
    // toggle and a daemon that says `warp=off`.
    //
    // Virtual for the reason offerWarpInstall() is: the real one is a modal dialog that runs a package
    // manager and asks for root, and a suite has neither a screen nor a password to type.
    virtual void openWarpSetup();

    void onWarpInstallationProgress(const QString &status);
    void onWarpInstallationFinished(bool success, const QString &error);
    // The device's enrolment was read. Said while the check is still running, and used for exactly one
    // thing: putting a line in the journal at the moment WARP is about to be set up for the first time,
    // because the enrolment round trip takes seconds and a button that appears to do nothing for that
    // long reads as a button that is stuck. Not a reading the window stores or redraws from — the
    // tunnel's look comes from stateChanged, and an enrolment has no colour of its own.
    void onWarpRegistrationChanged(WarpManager::Registration registration);
    // The enrolment check answered. True means the device is enrolled and the tunnel can be asked for;
    // false means it is not and no connect is issued behind it — see WarpManager::ensureRegistered().
    void onWarpRegistrationFinished(bool ok);
    // The machine is being made ready for a tunnel: the MASQUE switch, the IPv6 read, the service
    // restart. Every stage is said in the journal and in the footer, because this is the first step of
    // the start sequence that can spend a minute visibly doing nothing — a root prompt on screen with a
    // button that has stopped moving is the exact thing that makes a user kill a working program.
    void onWarpStage(WarpManager::Stage stage);
    // The preparation answered. A tunnel is asked for either way — a machine that could not be fully
    // prepared may still carry a working daemon, and the sentence that says what was skipped is more
    // useful than a start refused on principle — but never for a request the user has withdrawn.
    void onWarpPreparationFinished(bool ok);
    void onStrategyStarted(const QString &id, const QString &name);
    void onStrategyStopped();
    void onLogMessage(const QString &line, int level);
    void onErrorOccurred(const QString &message);
    void onPingUpdated(const QString &host, int status, int latencyMs);
    // The profiles page's "Тест пинга". One round of the service's own cycle, handed to every row
    // rather than only to the running one — the answer to "the numbers on the rows I am not running are
    // the last thing they said, and I want to know what they say now".
    void onPingTestRequested();
    void onSettingsChanged();
    void onStrategiesLoaded(int count);
    // A download finished and validated: persist the path and rescan.
    void zapretInstalled(const QString &path);
    // A profile row was pressed. The window owns what selection means: it becomes the profile the power
    // button will run, exactly as choosing it from the old per-card start button used to.
    void onProfileSelected(const QString &id);
    // The row's menu said "Скопировать". ProfileCard already put the command on the clipboard; this is
    // the window's cue to say so in the footer, naming the profile that was copied.
    void onProfileCopied(const QString &id);
    // The row's menu said "Удалить": confirm, remove the file on disk, and rescan. A profile is a real
    // file in the zapret folder, so this is destructive and asks first.
    void onProfileDeleteRequested(const QString &id);

private:
    void buildCentral();
    // The rail: brand at the head, the journal in the middle, settings and the version at the foot.
    QWidget *buildSidebar();
    // The column to the rail's right: header, page stack, footer.
    QWidget *buildMainArea();
    QWidget *buildHeader();
    QWidget *buildFooter();
    QWidget *buildDashboardPage();
    QWidget *buildProfilesPage();
    QWidget *buildLogsPage();
    // The history and statistics page. Like the journal it is a whole-screen view of a record the window
    // already keeps; unlike the journal it is derived, so it needs the store handed to it and re-read
    // whenever the store says it moved.
    QWidget *buildHistoryPage();
    // The settings page, added to the same stack as the others. It owns its own widgets and reads/writes
    // Settings directly, so this only constructs it once and remembers the pointer for retranslate/theme.
    QWidget *buildSettingsPage();
    // One rail glyph. The name goes in a property so applyTheme() can re-tint it: Theme::icon() bakes the
    // colour into a pixmap, so a theme switch cannot repaint an already-loaded glyph.
    QPushButton *addRailButton(QVBoxLayout *box, const QString &glyph, const QString &textKey);
    // A frameless-window control: 30x30, icon-only, tinted like a rail glyph so applyTheme() can re-bake
    // it. `role` is "true" for the close button, which the sheet colours as the destructive one.
    QToolButton *addWindowButton(QHBoxLayout *row, const QString &glyph, const QString &textKey,
                                 const char *role);
    // The language picker's menu: one checkable action per catalog language, each tagged with its code.
    QMenu *buildLanguageMenu();
    // Turns the journal's tab red while it holds a warning or error the user has not opened. The
    // property is what the QSS [alert="true"] rule keys off, so it needs the same unpolish/polish
    // dance ProfileCard uses for [selected] and [error].
    void setLogAlert(bool on);
    // Bake the release badge's glyph in the current theme's action colour. One method because two
    // places need it — the header builds the badge, and a theme change re-bakes every glyph in the
    // window — and a colour spelled twice is a colour that drifts out of agreement between the first
    // frame and the tenth.
    void tintUpdateBadge();
    // The one line the header's chip is allowed to be built from: text plus colour from the state.
    void setConnectionChip(const QString &text, const QString &statusKind);
    // Draw the chip and the power button while both halves are lit, from the two states the
    // selection is about. Kept apart from renderStatus() rather than overloaded because the two
    // enums do not overlap: an IZapretBackend::State does not know Unavailable is a thing, and
    // folding both into one switch is how a case ends up falling through on the wrong one.
    void renderCombinedStatus();
    // …the same drawing with the states handed in, which is what lets a teardown be painted before
    // either manager has reported it (see showStoppingFeedback) and what keeps the two renderers
    // reading one table.
    void renderCombinedLook(IZapretBackend::State zapret, WarpManager::State tunnel);

    // Create the tray icon if the platform has a tray host. Idempotent; returns whether one exists.
    bool ensureTray();
    void startTrayRetry();
    void buildShortcuts();
    void wire();

    void populateCards();
    // The entrance cascade for the profile rows: the first kCardRiseRows rows rise into their layout
    // slots one step behind the last, the rest simply appear. Called when the page is opened and when
    // the list is rebuilt while it is the page being looked at — the two moments a row arrives in front
    // of somebody — and a no-op when motion is off.
    void animateCardEntrance();
    // Open and close the manual ping round: the button's own wording is the only place the state is
    // visible, so both halves go through here and the round cannot be left running by an early return.
    // The watchdog is not pessimism: the service bails out of a cycle without a word when it is
    // stopped, and a button stuck on "Проверка…" would be the window lying about a measurement that
    // already ended.
    void beginPingTest();
    void endPingTest();

    // ---- the real-bypass check ----------------------------------------------------------
    // The profiles page's second measuring press. Where the ping test asks "are the services
    // reachable", this one asks the harder question through a preset the engine itself brings up
    // and takes back down (BlockcheckEngine): connect, TLS handshake, certificate verdict, three
    // tries per host. The window owns no part of the measurement — it presses, drives the bar and
    // the button's wording, and opens the verdict sheet when testFinished lands.
    void onBlockcheckRequested();
    void beginBlockcheck();
    void endBlockcheck();
    void onBlockcheckFinished(const QJsonObject &results);

    // ---- the watchdog's own half --------------------------------------------------------
    // (Re)arm the timer from Settings, and only when the interval really moved: Settings answers every
    // write with changed() — including the last-strategy id a start writes — and restarting a five-
    // minute cadence on each of those is how a check silently never happens.
    void startWatchdog();
    // The profile a quiet verdict belongs to, in the window's own precedence order (see errorTargetId),
    // falling back to the row the power button is pointed at when no run is named any more.
    QString watchdogRunId() const;
    // The display name of a catalog entry, or the id itself when the row is gone — a message that names
    // a profile has to name it with whatever the user recognises.
    QString watchdogProfileName(const QString &id) const;
    // The deferred half of a rotation: the start that waits for the stop to finish. Idempotent — it
    // does nothing while no rotation is waiting, and the id it consumes it consumes once.
    void continueWatchdogRotation();
    void updateStatusUi();
    // Record a selection as the current one: the window's own pair, the pill, both Settings keys, the
    // tunnel's polling and one repaint. This is the only writer of m_selection, and it never starts or
    // stops anything — a programmatic move (the startup seed, an availability clamp, snapping the halves
    // back after a refused question) must not be able to reach the path, and must not re-enter the slot
    // that asks. m_settingMode is what makes the pill's own signal ignorable while this runs.
    void setCurrentSelection(ModeSelection selection);
    // Take down whatever the change is leaving behind — the bypass, the tunnel, or both — because the
    // user agreed to a selection that costs it. Named for the reason rather than reusing the power
    // button's slots: onPowerClicked() is the only place anything is ever brought UP, and this file has
    // to keep that fact visibly apart from every place something is put down.
    void stopHeldForSelectionChange(ModeSelection from, ModeSelection to);
    // Arm the wait that precedes a tunnel connect, and whether one is running. Only the press paths
    // call the first; the guard in the slot the timer fires is mayConnectWarpAfterDelay().
    void scheduleWarpStartAfterBypass();

    // Draw the chip and the power button for one state, with no reference to what the manager says it
    // is in. updateStatusUi() calls it with the real state; the Stop press calls the feedback below,
    // which uses the state the teardown is about to enter. Kept apart from the tray so a look can be
    // painted before a signal exists.
    void renderStatus(IZapretBackend::State state);
    // Paint the stopping look at once, synchronously, before the stop that blocks this thread starts —
    // a queued repaint would not land until the teardown was over, which is the delay this answers.
    void showStoppingFeedback();
    void updateTrayIcon();
    void applyTheme();
    void requestQuit();
    void startStrategy(const QString &id, bool interactive);
    // The footer's temporary line — what statusBar()->showMessage used to be. The redesign retired the
    // status bar, and a window that creates one just to flash a sentence in it for three seconds is how
    // the old chrome creeps back.
    void showTransientMessage(const QString &text, int timeoutMs);

    // Pin a backend failure onto the card that caused it (an empty message clears it), and tell the
    // user once — repeated identical failures are logged and badged, but not re-dialoged.
    void attachErrorBadge(const QString &id, const QString &message);
    void notifyError(const QString &message, bool interactive);

    // Rewrite every translatable string in this window's whole object tree.
    //
    // Widgets that carry text declare it twice: once when they are built (setText(tr("key"))) and once
    // as a dynamic property naming that key ("i18nKey", plus "i18nTip" for the tooltip, "i18nAcc" for
    // the accessible name, "i18nItems" for a combo's entries, "i18nSuffix" for a spin box). This walk
    // re-reads the properties and re-applies the text in the language LanguageManager now holds, so
    // switching language is one call and one repaint — no restart, and no per-widget retranslate list
    // that a new button could silently be missing from. Because the walk starts here, it also reaches
    // the dialogs built with this window as parent (DownloadDialog) and the profile rows, which are
    // descendants too.
    void retranslateUi();
    // The rail's language glyph is the current language's flag, and the menu ticks it: both follow the
    // code LanguageManager holds, whether it changed from the menu or from a settings write.
    void updateLanguageButton();

    Settings *m_settings = nullptr;
    ZapretManager *m_manager = nullptr;
    PingService *m_ping = nullptr;

    // The real-bypass check's engine. A child of the window — it lives on this thread, and only its
    // internal probe worker does not — wired to the same manager the power button drives.
    BlockcheckEngine *m_blockcheck = nullptr;
    // Every verdict the engine returns is recorded here, so "when did this last work" has an answer
    // after the window is gone. A child too: it lives on this thread and only moves its file write off
    // it. See core/HistoryStore.h for the cap and the format.
    HistoryStore *m_history = nullptr;
    // Which profile the cycle in flight is measuring, kept because hostChecked names the host and not
    // the preset, and a history row without its preset is half a record.
    QString m_blockcheckPreset;
    LogPanel *m_log = nullptr;

    // The rounded shell: everything else in the window lives inside it, and it is the thing the window's
    // shape is drawn by — neither the window nor the canvas holding it paints a background (see
    // buildCentral(), and QMainWindow#mainWindow / QWidget#rootCanvas in both sheets).
    QFrame *m_windowFrame = nullptr;
    QWidget *m_sidebar = nullptr;          // the rail: brand, the page glyphs, settings and version
    QFrame *m_mainArea = nullptr;          // header, page stack, footer
    QStackedWidget *m_pages = nullptr;     // dashboard <-> profiles <-> logs <-> history <-> settings
    QPushButton *m_navLogs = nullptr;      // carries the [alert="true"] property
    QPushButton *m_navStrategies = nullptr;
    QPushButton *m_navHistory = nullptr;   // the record of past checks, beside the journal's lines
    QPushButton *m_navSettings = nullptr;  // the settings glyph; checked while PageSettings is up
    QToolButton *m_minButton = nullptr;    // frameless-window chrome
    QToolButton *m_closeButton = nullptr;
    // The release badge: a third windowButton that is hidden until an answer names a newer build. It is
    // chrome-shaped for the same reason the two beside it are — it lives in the header's control row and
    // has to read as part of it — but unlike them its press opens a question rather than the window.
    QToolButton *m_updateButton = nullptr;
    UpdateChecker *m_update = nullptr;      // the release feed, a child of the window
    QString m_updateVersion;                // the tag on offer, "" until one arrives
    QString m_updateUrl;                    // the page its press hands to the browser
    QToolButton *m_langButton = nullptr;   // the current language's flag, opening the language menu
    PowerButton *m_power = nullptr;        // the dashboard's one control
    ModeToggle *m_modeToggle = nullptr;    // the Zapret/WARP pill under the button
    // Which subsystems the power button controls. The window's own copy, not a read-through to the
    // pill: the pill can be mid-fade, mid-clamp or mid-snap-back, and what the button means has to be
    // one pair the selection path writes and nothing else touches. Seeded from Settings and kept in
    // step by setCurrentSelection().
    ModeSelection m_selection{true, false};
    // True while setCurrentSelection() is moving the pill, so the selectionChanged the widget emits on
    // the way back is recognised as a programmatic echo and not as a second request from the user.
    bool m_settingMode = false;
    WarpManager *m_warp = nullptr;         // the tunnel control (a child of the window)
    // The second half of a combined start: the wait between the bypass coming up and the tunnel being
    // asked to connect, and whether that wait is owed. Pending is set by the press that asked for both
    // and cleared by the timer, by a teardown, or by a run that never arrived — the three ways a
    // connect can be correctly withheld.
    QTimer *m_warpStartTimer = nullptr;
    bool m_warpStartPending = false;
    // Set between asking WarpManager for the enrolment check and that check answering — the window is
    // lit for WARP, but no tunnel has been issued yet. Two things depend on it: an answer must not be
    // honoured twice if the user pressed again while the check ran, and a half the user turned back off
    // during it is a connect owed to nobody.
    bool m_warpEnrollPending = false;
    // The same flag for the preparation that follows the enrolment. Kept apart rather than folded into
    // one "start is on its way" bool because the two answers arrive through different signals, and a
    // single flag could not tell a late enrolment answer from a late preparation answer — which is how
    // a withdrawn request gets a tunnel issued behind it anyway.
    bool m_warpPrepPending = false;
    int m_warpStartDelayMs = kWarpStartDelayMs;

    // Puts the Cloudflare WARP client on a host that has none, when the user picks the WARP half of the
    // pill and there is nothing behind it. Owns one operation at a time and never offers a cancel — a
    // package manager interrupted halfway leaves a database to repair, so the progress window it opens
    // has a busy bar and no button (see the class comment in WarpInstaller.h).
    WarpInstaller *m_warpInstaller = nullptr;
    QProgressDialog *m_warpProgress = nullptr;   // that window, non-null only while an install runs
    // Whether the setup wizard is on screen. One at a time, because two wizards would race for the same
    // daemon, the same package database and the same root prompt.
    bool m_warpSetupOpen = false;
    QWidget *m_statusDot = nullptr;        // the 6 px chip beside the connection text
    QLabel *m_statusLabel = nullptr;       // "CONNECTED" and friends
    QLabel *m_footerLabel = nullptr;       // the ping cadence, or a transient message
    QTimer *m_footerTimer = nullptr;       // for how long a transient message holds the footer
    QString m_footerIdleText;              // what the footer says when nothing is being announced
    QWidget *m_dashboardPage = nullptr;
    QWidget *m_profilesPage = nullptr;
    QWidget *m_logsPage = nullptr;
    HistoryPage *m_historyPage = nullptr;   // PageHistory's content, without the way back around it
    SettingsPage *m_settingsPage = nullptr;
    QScrollArea *m_scroll = nullptr;
    QStackedWidget *m_stack = nullptr;      // profiles list <-> "no strategies found" placeholder
    QWidget *m_cardHost = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    QWidget *m_placeholder = nullptr;      // "no strategies found" page: the text plus its two ways out

    QSystemTrayIcon *m_tray = nullptr;
    QTimer *m_trayRetry = nullptr;           // the tray host can appear after we start
    int m_trayAttempts = 0;

    QHash<QString, ProfileCard *> m_cards;                 // strategy id -> row
    QHash<QString, PingIndicator *> m_indicators;          // host -> the dashboard's ring
    // The last verdict per host, kept so a profile row built after the cycle started can show a real
    // number instead of grey. Doubles as the source for the dashboard rings on a re-theme.
    struct PingMemo { PingIndicator::PingStatus status = PingIndicator::PingStatus::Unknown; int latencyMs = -1; };
    QHash<QString, PingMemo> m_lastPing;
    // ---- the manual ping round ---------------------------------------------------------------
    // A press of the profiles page's "Тест пинга" asks the service for one cycle and lets every row
    // take it, active or not. The round is over when every host the service probes has answered —
    // seeded from that list rather than from the two a row shows, because the dashboard's baseline
    // badge is measured by the same cycle and finishing early would leave it mid-probe.
    bool m_pingTestActive = false;
    QSet<QString> m_pingTestPending;
    QTimer *m_pingTestWatchdog = nullptr;   // ends the round if the service never answers at all
    QPushButton *m_pingTestButton = nullptr;
    // ---- the real-bypass check round ---------------------------------------------------------
    // The button, the slim bar under the page head, and the one flag that says a cycle owns them.
    // No watchdog lives here: every phase of the engine is bounded by its own deadlines, and
    // testFinished is contractually the last word of every run — including a failed one — so the
    // window never has to guess whether an answer is still coming.
    bool m_blockcheckActive = false;
    QPushButton *m_blockcheckButton = nullptr;
    QProgressBar *m_blockcheckProgress = nullptr;
    // ---- the watchdog -------------------------------------------------------------------
    // Fires every Settings::watchdogIntervalMin() minutes from the moment the window exists. There is
    // no "waiting for the first answer" state to show and no watchdog-of-the-watchdog: a tick either
    // starts a cycle or does nothing, and every cycle ends in onWatchdogResult() by the engine's own
    // contract (testFinished is the last word of every run, refused ones included).
    QTimer *m_watchdogTimer = nullptr;
    // A quiet cycle owns the shared engine while this is set: the manual press is refused, the engine's
    // log/meter channels stay muted, and its testFinished goes to the watchdog instead of to the
    // verdict sheet. Also the flag that keeps two ticks from measuring at once.
    bool m_watchdogRun = false;
    // Set when the run a quiet cycle is measuring comes down mid-flight — the user pressed Stop, or the
    // payload died. The engine's answer still has to arrive (it owns the engine until testFinished), but
    // that verdict describes a path being torn down rather than a provider that moved, so it is dropped
    // instead of turned into an offer to start something the user has just stopped.
    bool m_watchdogVerdictVoid = false;
    // The profile an accepted rotation starts once the current one is down. The backend takes one
    // operation at a time and a portable teardown costs seconds, so the start cannot happen in the same
    // call as the stop — see performWatchdogRotation().
    QString m_watchdogRotationId;
    // The profile the power button will run — read at start, written on every row press, and persisted
    // through Settings under lastStrategyId so it survives a restart. Empty means "nothing chosen yet",
    // which the power button answers by opening the profiles page.
    QString m_selectedId;

    // Frameless-window drag: while the pointer is down on the header this holds the offset from the
    // window's top-left to the press point, so moving the pointer moves the window without a jump. A
    // null offset means "not dragging"; the window is fixed-size, so nothing else moves it.
    QPoint m_dragOffset;
    bool m_dragging = false;

    bool m_quitting = false;          // explicit quit (tray menu / Ctrl+Q), bypasses hide-to-tray
    bool m_applyingSettings = false;  // re-entrancy guard for onSettingsChanged
    bool m_needsSettingsPrompt = false;
    bool m_trayWarned = false;        // "no tray here" has already been said once

    // Which strategy the current start attempt belongs to, and which card carries a failure badge.
    QString m_pendingStartId;
    QString m_errorCardId;
    // The most recent run the manager still named as active. ZapretManager clears that name inside the
    // same stateChanged(Error) that precedes a backend's errorOccurred, so a deferred failure — every
    // portable-mode failure that had to tear the queue down first — arrives with nothing to attribute
    // it to unless the window remembered. Cleared when a run comes down cleanly, so a much later,
    // unrelated refusal cannot badge a card whose run ended without incident.
    QString m_lastRunId;
    QString m_lastErrorText;          // for dialog coalescing of repeated identical failures
    QElapsedTimer m_errorClock;       // running from construction
    qint64 m_lastErrorAt = 0;
};

} // namespace zapret
