// uitest — headless widget + service verification for the zapret GUI layer.
//
// Runs under the offscreen QPA platform (QT_QPA_PLATFORM is also forced in-process, so the binary is
// runnable straight from a shell). Like coretest it is a plain console program: readable PASS/FAIL
// lines, non-zero exit on any failure, no QTest data-driven scaffolding.
//
// Deliberately NOT covered here: anything that would spawn pkexec or winws, anything that would spawn
// warp-cli (the WARP section hands each window an injected control that counts the verbs instead of
// running them), and real network reachability (the ping test only asserts that a verdict arrives).
// DownloadDialog is constructed but never started: startFetch() is the only thing in it that touches
// GitHub, and a test suite must not depend on that answer. UpdateChecker::check() is the release feed's
// one socket and is likewise never called — the badge section hands the window the answer it would have
// carried instead.
//
// A constructed MainWindow is covered in four places: the watchdog section, which needs the real catalog
// and journal to prove that a dead bypass turns into an offer; the enrolment section, which needs the
// window's own press to prove that a tunnel is asked for only after the device was settled; the
// release-badge section, which needs the header the badge lives in; and the rounded-shell section,
// which needs the window's own pixels. All are built without a window manager (offscreen) — the
// watchdog's three seams that would need a desktop, a network or a root prompt are overridden, and so
// are the enrolment section's launch, its consent question and its install offer — and all read the
// redirected test identity, skipping themselves when the harness has not redirected it. Neither the
// watchdog nor the badge section asks the release feed anything: the GitHub lookup is armed by
// main.cpp, which is not part of this binary, so a window here cannot reach the network however the
// suite drives it. Motion is the one thing in the suite that has to put a window on screen: a page
// behind another page has nothing to appear on, so a fade can only be watched on a visible window —
// and so can a badge that breathes, and so can the corners of the window itself, which the shell
// section reads out of a rendered grab.

#include "BlockcheckEngine.h"
#include "IZapretBackend.h"
#include "DownloadDialog.h"
#include "HistoryPage.h"
#include "HistoryStore.h"
#include "LanguageManager.h"
#include "LogPanel.h"
#include "MainWindow.h"
#include "ModeToggle.h"
#include "Motion.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "PowerButton.h"
#include "ProfileCard.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "Strategy.h"
#include "Theme.h"
#include "UpdateChecker.h"
#include "WarpInstaller.h"
#include "WarpManager.h"
#include "WarpSetupDialog.h"
#include "ZapretDownloader.h"
#include "ZapretPaths.h"
#include "ZapretSource.h"

#include <QApplication>
#include <QBrush>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QAbstractItemModel>
#include <QColor>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QLabel>
#include <QShortcut>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include <QtTest/QTest>

#include <cstdio>

using namespace zapret;

static int g_failures = 0;

// True when `w` sits in a QHBoxLayout that has a spacer after it — the shape the settings page uses to
// keep a short field beside its label instead of across the whole form. Walking the layouts is the only
// way to see it: a stretch is an item, not a widget, so nothing about the field itself records that it
// has one, and a maximum width alone would still leave the control floating in its column.
static bool fieldEndsInStretch(QWidget *w)
{
    if (!w || !w->parentWidget())
        return false;
    const auto layouts = w->parentWidget()->findChildren<QLayout *>();
    for (QLayout *l : layouts) {
        auto *row = qobject_cast<QHBoxLayout *>(l);
        if (!row)
            continue;
        const int at = row->indexOf(w);
        if (at < 0)
            continue;
        for (int i = at + 1; i < row->count(); ++i)
            if (row->itemAt(i) && row->itemAt(i)->spacerItem())
                return true;
    }
    return false;
}

static void check(bool cond, const QString &what)
{
    std::printf(cond ? "  PASS  %s\n" : "  FAIL  %s\n", qUtf8Printable(what));
    if (!cond)
        ++g_failures;
}

// The card a widget sits in, or null when it is outside every card (the commit row is, on purpose).
//
// The settings page parents each of its rows to the card that holds them, which is what makes "did this
// setting land in the right group" a question about the object tree rather than about coordinates: it
// survives any later change to the layout's geometry, and it fails the moment a row is moved to another
// card — which is the only way the grouping can silently rot.
static QWidget *settingsCardOf(QWidget *w)
{
    for (QWidget *p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
        if (p->objectName() == QLatin1String("settingsCard"))
            return p;
    return nullptr;
}

// Two colours the eye cannot tell apart are two colours that failed: antialiasing and the sheet's own
// gradient-free fills mean a rendered pixel is within a few units of the hex the sheet declared, and
// nothing tighter is provable on a machine whose font rendering this file does not control.
static bool rgbClose(const QColor &a, const QColor &b, int tol)
{
    return a.isValid() && b.isValid()
           && qAbs(a.red() - b.red()) <= tol && qAbs(a.green() - b.green()) <= tol
           && qAbs(a.blue() - b.blue()) <= tol;
}

// Is this colour dark enough to read as ink on a light surface? qGray is the same cheap brightness measure
// the ring-mark check below already uses; the bound is a sanity limit rather than a design value — the
// lightest grey the light sheet names for text (#6B7280) is 113 of 255, while the disabled/decorative
// tokens that are lighter are deliberately kept out of the list that tests this.
static bool isDarkInk(const QColor &c)
{
    return c.isValid() && qGray(c.rgb()) < 128;
}

// The sheet with every /* … */ block removed.
//
// Both files name selectors inside their comments to explain what a rule is for ("a plain
// `QMainWindow { … }` rule would match this window too", "the surfaces that reach a corner are
// QFrame#sidebar …"). A scan that looks for a selector's first match would then read that prose as the
// declaration and return the body of whatever rule happens to follow the sentence — so every structural
// read of a sheet goes through this first. Comment-free text is where a declaration can only ever live.
static QString sheetWithoutComments(const QString &sheet)
{
    QString code;
    code.reserve(sheet.size());
    bool inComment = false;
    for (int i = 0; i < sheet.size(); ++i) {
        if (!inComment && sheet.at(i) == QLatin1Char('/') && i + 1 < sheet.size()
            && sheet.at(i + 1) == QLatin1Char('*')) {
            inComment = true;
            ++i;
            continue;
        }
        if (inComment && sheet.at(i) == QLatin1Char('*') && i + 1 < sheet.size()
            && sheet.at(i + 1) == QLatin1Char('/')) {
            inComment = false;
            ++i;
            continue;
        }
        if (!inComment)
            code.append(sheet.at(i));
    }
    return code;
}

// True when some rule in `code` names QMainWindow as a bare TYPE (`QMainWindow {`, `QMainWindow, QDialog`).
// That is the shape of the bug the rounded shell dies of: a background on the window is an opaque
// rectangle over every pixel of a WA_TranslucentBackground surface, corners included, and the rounded card
// under it is a rounding nobody can see. Takes comment-free text, so a comment may name the type freely.
static bool hasUnqualifiedQMainWindowRule(const QString &code)
{
    static const QLatin1String type("QMainWindow");
    int at = code.indexOf(type);
    while (at >= 0) {
        const int after = at + type.size();
        const QChar next = after < code.size() ? code.at(after) : QLatin1Char(' ');
        if (!next.isLetterOrNumber() && next != QLatin1Char('_') && next != QLatin1Char('#'))
            return true;                       // not `QMainWindow#mainWindow` — the type itself
        at = code.indexOf(type, after);
    }
    return false;
}

// The colour one rule of one sheet declares, out of comment-free text (see sheetWithoutComments), or an
// invalid colour when nothing matches. Kept next to the helpers that read a sheet rather than a pixel
// because every check that wants to know what the SHEET says goes through it: hardcoding the same hexes
// here would be a test of this file, not of a theme.
static QColor sheetRuleColor(const QString &sheet, const QString &selector, const QString &prop)
{
    const QString code = sheetWithoutComments(sheet);
    const int at = code.indexOf(selector);
    const int open = at < 0 ? -1 : code.indexOf(QLatin1Char('{'), at);
    const int close = open < 0 ? -1 : code.indexOf(QLatin1Char('}'), open);
    if (close < 0)
        return QColor();
    const QStringList decls = code.mid(open + 1, close - open - 1).split(QLatin1Char(';'));
    for (const QString &d : decls) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0)
            return QColor(d.mid(colon + 1).trimmed());
    }
    return QColor();
}

// The first hex inside one declaration of one rule — the form a border takes ("1px solid #262A36"), where
// the colour is not the whole value and sheetRuleColor would hand back an unparseable string.
static QColor sheetRuleHex(const QString &sheet, const QString &selector, const QString &prop)
{
    const QString code = sheetWithoutComments(sheet);
    const int at = code.indexOf(selector);
    const int open = at < 0 ? -1 : code.indexOf(QLatin1Char('{'), at);
    const int close = open < 0 ? -1 : code.indexOf(QLatin1Char('}'), open);
    if (close < 0)
        return QColor();
    const QString body = code.mid(open + 1, close - open - 1);
    for (const QString &d : body.split(QLatin1Char(';'))) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon <= 0 || d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) != 0)
            continue;
        const QRegularExpression hex(QStringLiteral("#[0-9a-fA-F]{6}"));
        const QRegularExpressionMatch m = hex.match(d.mid(colon + 1));
        if (m.hasMatch())
            return QColor(m.captured(0));
    }
    return QColor();
}

// ---------------------------------------------------------------- Settings ----
static void testSettings()
{
    std::printf("\n[Settings]\n");

    QTemporaryDir dir;
    check(dir.isValid(), "temporary dir created");
    const QString file = dir.filePath(QStringLiteral("muffin-test.ini"));

    {
        Settings s(file);
        check(s.zapretPath().isEmpty(), "default zapretPath is empty");
        check(s.lastStrategyId().isEmpty(), "default lastStrategyId is empty");
        check(s.startLastOnLaunch() == false, "default startLastOnLaunch is false");
        check(s.minimizeToTray() == true, "default minimizeToTray is true");
        check(s.animationsEnabled() == true, "default animationsEnabled is true");
        check(s.useZapret() == true, "default useZapret is true — Zapret's half is on by default");
        check(s.useWarp() == false, "default useWarp is false — the tunnel is an explicit decision");
        check(s.pingIntervalSec() == 5, "default ping interval is 5s");
        check(s.watchdogIntervalMin() == 5, "default watchdog interval is 5 min");
        check(Settings::kWatchdogDefaultIntervalMin == 5,
              "and the default the window reads is the same number");
        check(s.watchdogIntervalMin() >= Settings::kWatchdogMinIntervalMin
                  && s.watchdogIntervalMin() <= Settings::kWatchdogMaxIntervalMin,
              "the default sits inside the range the page offers");
        check(s.theme() == QLatin1String("dark"), "default theme is dark");

        QSignalSpy changed(&s, &Settings::changed);
        s.setZapretPath(QStringLiteral("/opt/zapret/linux"));
        s.setLastStrategyId(QStringLiteral("general"));
        s.setStartLastOnLaunch(true);
        s.setMinimizeToTray(false);
        s.setAnimationsEnabled(false);
        s.setWatchdogIntervalMin(15);
        s.setTheme(QStringLiteral("system"));
        check(changed.count() == 7, "each setter emits changed()");

        // The spinbox range is also enforced in the model, so a bad value cannot be persisted.
        s.setPingIntervalSec(900);
        check(s.pingIntervalSec() == 60, "ping interval clamped to 60");
        s.setPingIntervalSec(1);
        check(s.pingIntervalSec() == 3, "ping interval clamped to 3");
        s.setPingIntervalSec(12);
        check(s.pingIntervalSec() == 12, "ping interval stored as given");
        // Same for the watchdog, against its own published bounds: a stored 0 would arm a timer that
        // fires continuously, and a stored 900 one that never fires in a session.
        s.setWatchdogIntervalMin(0);
        check(s.watchdogIntervalMin() == Settings::kWatchdogMinIntervalMin,
              "watchdog interval clamped to the floor");
        s.setWatchdogIntervalMin(900);
        check(s.watchdogIntervalMin() == Settings::kWatchdogMaxIntervalMin,
              "watchdog interval clamped to the ceiling");
        s.setWatchdogIntervalMin(15);
        check(s.watchdogIntervalMin() == 15, "watchdog interval stored as given");
    }

    {
        Settings reopened(file);
        check(reopened.zapretPath() == QLatin1String("/opt/zapret/linux"), "zapretPath round-trip");
        check(reopened.lastStrategyId() == QLatin1String("general"), "lastStrategyId round-trip");
        check(reopened.startLastOnLaunch() == true, "startLastOnLaunch round-trip");
        check(reopened.minimizeToTray() == false, "minimizeToTray round-trip");
        check(reopened.animationsEnabled() == false, "animationsEnabled round-trip");
        check(reopened.theme() == QLatin1String("system"), "theme round-trip");
        check(reopened.pingIntervalSec() == 12, "ping interval round-trip");
        check(reopened.watchdogIntervalMin() == 15, "watchdog interval round-trip");
    }
}

// Write `text` at `path`, creating parent directories on the way. The migration asserts read on-disk
// truth — the repair only fires when a folder really exists or really went away — so an empty promise
// of a path is not enough here.
static bool writeTextFile(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = text.toUtf8();
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

// ------------------------------------------------- Settings identity migration ----
//
// The settings half of the 0.1.1 organization rename. Everything here is the user-level store, so the
// section only runs while the harness redirects the config and data locations into the build tree
// (CMake sets XDG_CONFIG_HOME/XDG_DATA_HOME for uitest): otherwise the test would be reading and
// writing a real desktop's settings file, which is the one thing a migration test must never do.
static void clearTree(const QString &path)
{
    QDir dir(path);
    if (dir.exists())
        dir.removeRecursively();
}

static void testSettingsMigration()
{
    std::printf("\n[Settings: 0.1.1 identity migration]\n");

    const QString configBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString dataBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString xdgData = qEnvironmentVariable("XDG_DATA_HOME");
    const bool redirected =
        !xdgConfig.isEmpty() && !xdgData.isEmpty()
        && QDir::cleanPath(configBase) == QDir::cleanPath(xdgConfig)
        && QDir::cleanPath(dataBase) == QDir::cleanPath(xdgData);
    if (!redirected)
    {
        std::printf("  SKIP  the settings import needs XDG_CONFIG_HOME and XDG_DATA_HOME redirected "
                    "into the build tree (run it through ctest)\n");
        return;
    }

    const QString legacyOrg = ZapretPaths::legacyOrganizationName();
    const QString currentOrg = QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName();
    // The two stores are two directories under the redirected config root, named after the two
    // organizations. Asserting that up front is what proves this test is sandboxed, not the paths it
    // later rewrites.
    const QString legacyStoreDir = QDir(configBase).filePath(legacyOrg);
    const QString currentStoreDir = QDir(configBase).filePath(currentOrg);
    check(legacyOrg != currentOrg && !currentOrg.isEmpty(),
          "the test identity differs from the legacy organization");
    check(QDir::cleanPath(legacyStoreDir).startsWith(QDir::cleanPath(configBase) + QLatin1Char('/'))
              && QDir::cleanPath(currentStoreDir).startsWith(
                     QDir::cleanPath(configBase) + QLatin1Char('/')),
          "both stores live under the redirected config location");

    const QString legacyManaged = ZapretPaths::legacyManagedInstallDir();
    const QString managed = ZapretPaths::managedInstallDir();
    check(!legacyManaged.isEmpty() && !managed.isEmpty() && legacyManaged != managed,
          "the migration's two directory names are distinct");
    check(QDir::cleanPath(legacyManaged).startsWith(QDir::cleanPath(dataBase) + QLatin1Char('/'))
              && QDir::cleanPath(managed).startsWith(QDir::cleanPath(dataBase) + QLatin1Char('/')),
          "and both live under the redirected data location");

    clearTree(legacyStoreDir);
    clearTree(currentStoreDir);
    clearTree(legacyManaged);
    clearTree(managed);

    // ---- the state an upgraded user is in: 0.1.0's store filled, this build's not ----
    const QString legacyLinuxRoot = legacyManaged + QStringLiteral("/linux");
    const QString managedLinuxRoot = managed + QStringLiteral("/linux");
    // What a portable-mode user actually has in `strategy/last`: not a name, but the absolute path of
    // the binary the parser surfaced — inside the very folder the migration renames.
    const QString legacyPortableId =
        legacyManaged + QStringLiteral("/binaries/linux-x86_64/nfqws");
    const QString managedPortableId =
        managed + QStringLiteral("/binaries/linux-x86_64/nfqws");

    // The file 0.1.0's QSettings() resolved to: this build's scope and format, the organization it
    // replaced, the same application name. Everything the import reads comes from here.
    const QString legacyFile =
        QSettings(QSettings::defaultFormat(), QSettings::UserScope, legacyOrg, app).fileName();
    check(!legacyFile.isEmpty(), "the legacy store has a resolvable file name");
    check(QDir::cleanPath(legacyFile).startsWith(QDir::cleanPath(legacyStoreDir) + QLatin1Char('/')),
          "and it is the file inside the legacy organization folder");

    {
        // The tree moved first (that is ZapretPaths::migrateManagedInstallDir(), covered in coretest),
        // so what is on disk here is the post-move world: the new folder holds it, the old one is gone.
        check(writeTextFile(managedLinuxRoot + QStringLiteral("/nfq/nfqws"), "payload\n"),
              "planted the migrated tree at the new name");
        check(writeTextFile(managedPortableId, "nfqws\n"),
              "including the portable binary a stored strategy id can name");
        check(!QFileInfo::exists(legacyManaged), "the legacy folder is vacated");

        // Written through Settings itself, on the legacy store's own file name, so the key spellings
        // the import looks for are produced by the code that reads them and cannot drift.
        Settings legacyStore(legacyFile);
        legacyStore.setTheme(QStringLiteral("light"));
        legacyStore.setPingIntervalSec(7);
        legacyStore.setLastStrategyId(legacyPortableId);
        legacyStore.setStartLastOnLaunch(true);
        legacyStore.setMinimizeToTray(false);
        legacyStore.setZapretPath(legacyLinuxRoot);
        check(QFileInfo::exists(legacyFile), "the legacy store file was written");
    }

    // ---- the import ----
    importLegacySettings();

    {
        Settings migrated;   // this build's own store, reached by identity rather than by path
        check(QDir::cleanPath(migrated.filePath()) != QDir::cleanPath(legacyFile),
              "and it is a different file than the one the import read");
        check(migrated.theme() == QLatin1String("light"), "theme carried over");
        check(migrated.pingIntervalSec() == 7, "ping interval carried over");
        check(migrated.lastStrategyId() == QDir::cleanPath(managedPortableId),
              "a stored strategy path followed the tree to its new folder too");
        check(migrated.startLastOnLaunch() == true, "start-last-on-launch carried over");
        check(migrated.minimizeToTray() == false, "minimize-to-tray carried over");
        check(QDir::cleanPath(migrated.zapretPath()) == QDir::cleanPath(managedLinuxRoot),
              "the stored tree path followed the tree to its new folder");
    }

    // ---- never overwrite ----
    //
    // This guard is what makes running the import on every startup safe: once this identity holds any
    // value at all, the legacy store is inert. So edit the new store, contradict it from the old one,
    // and re-run — the user's newer choice must win.
    {
        Settings migrated;
        migrated.setTheme(QStringLiteral("system"));

        Settings legacyStore(legacyFile);
        legacyStore.setTheme(QStringLiteral("dark"));

        importLegacySettings();

        Settings again;
        check(again.theme() == QLatin1String("system"), "an imported store is never imported over");
        check(QDir::cleanPath(again.zapretPath()) == QDir::cleanPath(managedLinuxRoot),
              "and re-running the repair leaves the repaired path alone");
    }

    // ---- which stored paths the repair may touch, and which it must not ----
    //
    // Empty the current store, then read the import's answer for one stored value. The managed install
    // is the only folder this migration moves, so a path that names it — itself or a child the locator
    // resolved inside it — is rebased, while anything the user pointed elsewhere survives byte for byte.
    {
        auto importedPathFor = [&](const QString &storedPath) {
            clearTree(currentStoreDir);
            Settings legacyStore(legacyFile);
            legacyStore.setZapretPath(storedPath);
            importLegacySettings();
            Settings migrated;
            return migrated.zapretPath();
        };

        check(QDir::cleanPath(importedPathFor(legacyLinuxRoot)) == QDir::cleanPath(managedLinuxRoot),
              "a child of the moved folder keeps its child segment");
        check(QDir::cleanPath(importedPathFor(legacyManaged)) == QDir::cleanPath(managed),
              "the moved folder itself is rebased");
        const QString foreign = QStringLiteral("/opt/zapret/linux");
        check(importedPathFor(foreign) == foreign, "a path this migration never moved is left alone");
        check(importedPathFor(legacyLinuxRoot + QStringLiteral("/../linux"))
                  == QDir::cleanPath(managedLinuxRoot),
              "and an untidy spelling of a moved path is still recognised");
    }

    // A strategy id the parser could name is a bare word, and the stored root may be a folder the user
    // picked in the dialog: neither may be edited by a repair that is looking for one directory.
    {
        clearTree(currentStoreDir);
        Settings legacyStore(legacyFile);
        legacyStore.setLastStrategyId(QStringLiteral("general"));
        legacyStore.setZapretPath(QStringLiteral("/opt/zapret/linux"));
        importLegacySettings();

        Settings migrated;
        check(migrated.lastStrategyId() == QLatin1String("general"),
              "a bare strategy id is carried over as it was");
        check(migrated.zapretPath() == QLatin1String("/opt/zapret/linux"),
              "and a root outside the managed folder keeps its own spelling");
    }

    clearTree(legacyStoreDir);
    clearTree(currentStoreDir);
    clearTree(managed);
    clearTree(legacyManaged);
}

// ---------------------------------------------------------------- LogPanel ----
static QString firstForegroundColor(const QTextBlock &block)
{
    const QList<QTextLayout::FormatRange> ranges = block.textFormats();
    if (ranges.isEmpty())
        return QString();
    return ranges.first().format.foreground().color().name(QColor::HexRgb);
}

static void testLogPanel()
{
    std::printf("\n[LogPanel]\n");

    LogPanel panel;
    check(panel.storedLineCount() == 0, "starts empty");
    check(panel.filterLevel() == LogPanel::All, "starts unfiltered");

    panel.appendLine(QStringLiteral("service up"), LogPanel::Info);
    panel.appendLine(QStringLiteral("list missing"), LogPanel::Warn);
    panel.appendLine(QStringLiteral("winws exited"), LogPanel::Error);
    panel.appendLine(QStringLiteral("bare line"));   // default level = info

    check(panel.storedLineCount() == 4, "four lines stored");
    check(panel.view()->document()->blockCount() == 4, "four blocks in the document");

    const QString all = panel.text();
    check(all.contains(QStringLiteral("[INFO] service up")), "info line formatted");
    check(all.contains(QStringLiteral("[WARN] list missing")), "warn line formatted");
    check(all.contains(QStringLiteral("[ERR] winws exited")), "error line tagged ERR");
    check(all.count(QLatin1Char('\n')) == 3, "lines joined with newlines");
    check(QRegularExpression(R"(\[\d{2}:\d{2}:\d{2}\])").match(all).hasMatch(), "timestamp prefix present");

    QTextDocument *doc = panel.view()->document();
    check(firstForegroundColor(doc->findBlockByNumber(0)) == Theme::logColor(0).name(QColor::HexRgb),
          "info uses the theme text colour");
    check(firstForegroundColor(doc->findBlockByNumber(1)) == Theme::warning().name(QColor::HexRgb),
          "warn is amber");
    check(firstForegroundColor(doc->findBlockByNumber(2)) == Theme::error().name(QColor::HexRgb),
          "error is red");

    const qreal heightAll = doc->size().height();

    panel.setFilterLevel(LogPanel::Error);
    check(panel.visibleLineCount() == 1, "error filter leaves one visible line");
    check(panel.storedLineCount() == 4, "filtering does not drop stored lines");
    QString visibleText;
    for (QTextBlock b = doc->firstBlock(); b.isValid(); b = b.next())
        if (b.isVisible())
            visibleText += b.text();
    check(visibleText.contains(QStringLiteral("winws exited")), "the surviving line is the error");
    check(doc->size().height() < heightAll, "hidden blocks shrink the laid-out document");

    panel.setFilterLevel(LogPanel::Warn);
    check(panel.visibleLineCount() == 2, "warn filter keeps warn + error");

    panel.setFilterLevel(LogPanel::All);
    check(panel.visibleLineCount() == 4, "all filter restores every line");
    check(qFuzzyCompare(doc->size().height(), heightAll), "document height restored");

    panel.clear();
    check(panel.storedLineCount() == 0 && panel.view()->toPlainText().isEmpty(), "clear() empties both");

    panel.appendLine(QStringLiteral("persisted"), LogPanel::Warn);
    const QString out = QDir::tempPath() + QStringLiteral("/uitest-log.txt");
    check(panel.saveToFile(out), "saveToFile writes");
    QFile saved(out);
    check(saved.open(QIODevice::ReadOnly), "saved file readable");
    const QString savedText = QString::fromUtf8(saved.readAll());
    check(savedText.contains(QStringLiteral("[WARN] persisted")), "saved file holds the line");
    saved.close();
    QFile::remove(out);

    // The ring buffer cap must be honoured: the widget drops, and the cache follows it.
    LogPanel big;
    for (int i = 0; i < LogPanel::kMaxLines + 250; ++i)
        big.appendLine(QStringLiteral("line %1").arg(i), LogPanel::Info);
    check(big.storedLineCount() == LogPanel::kMaxLines, "cache capped at kMaxLines");
    check(big.view()->document()->blockCount() <= LogPanel::kMaxLines + 1, "view capped too");
    check(big.text().contains(QStringLiteral("line 250")), "oldest lines dropped first");
}

// ---------------------------------------------------------- PingIndicator ----
// True if `w`'s polished font is actually fixed-pitch — an 'i' advancing the same as a 'W'. This tests the
// rendering property (tabular figures that do not shift) rather than a family NAME, so it holds whatever
// QFontDatabase resolves the system's fixed face to, and it catches the real bug: a monospaced face silently
// replaced by the proportional UI family via the generic `QWidget { font-family }` sheet rule.
static bool resolvesMonospace(QWidget *w)
{
    if (!w)
        return false;
    w->ensurePolished();
    const QFontMetrics fm(w->font());
    return fm.horizontalAdvance(QLatin1Char('i')) == fm.horizontalAdvance(QLatin1Char('W'))
        && fm.horizontalAdvance(QLatin1Char('l')) == fm.horizontalAdvance(QLatin1Char('M'));
}

// A bare, freshly-polished QLabel carries whatever the app sheet + QApplication::font() resolve to on
// this host. Some runners (windows-2022 with aqt's Qt 6.7.3) substitute an unavailable UI family with a
// fixed-pitch face rather than a proportional one, which means the "is this widget monospace?" question
// has the same answer for every widget on the page — a pin cannot be shown to have made a difference
// because nothing else exists to compare against. `uiSampleIsProportional()` is that comparator's gate:
// when it returns false, the discriminating checks are uninformative, so the tests SKIP rather than
// report a false regression.
static bool uiSampleIsProportional()
{
    QLabel s;
    s.ensurePolished();
    const QFontMetrics fm(s.font());
    return fm.horizontalAdvance(QLatin1Char('i')) != fm.horizontalAdvance(QLatin1Char('W'));
}

static void testPingIndicator()
{
    std::printf("\n[PingIndicator]\n");

    PingIndicator chip(QStringLiteral("Youtube"));
    // The badge is a column now, not a status-bar chip: a fixed width that fits a name and a 50 px ring,
    // and no minimum that would push the dashboard's row apart.
    check(chip.width() >= 64, "badge has a width a 50px ring fits");
    check(chip.minimumWidth() < 120, "badge no longer reserves status-bar chip width");
    check(chip.status() == PingIndicator::PingStatus::Unknown, "starts Unknown");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Unknown, -1) == QStringLiteral("—"),
          "Unknown shows an em dash");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Ok, 12) == QStringLiteral("12 мс"),
          "Ok shows milliseconds");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Fail, -1) == QStringLiteral("таймаут"),
          "Fail shows timeout");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Waiting, -1) == QStringLiteral("…"),
          "Waiting shows ellipsis");

    // The figure above the ring is the same state without the unit: a bare number when there is one, and
    // something that is not a number when there is not.
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Ok, 24) == QStringLiteral("24"),
          "Ok shows the bare number");
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Fail, -1) == QStringLiteral("!"),
          "Fail shows a mark, not a zero that could read as a fast reply");
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Waiting, -1) == QStringLiteral("…"),
          "Waiting shows ellipsis");
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Unknown, -1) == QStringLiteral("—"),
          "Unknown shows an em dash");

    check(PingIndicator::statusFromServiceInt(0) == PingIndicator::PingStatus::Waiting, "0 -> Waiting");
    check(PingIndicator::statusFromServiceInt(1) == PingIndicator::PingStatus::Ok, "1 -> Ok");
    check(PingIndicator::statusFromServiceInt(2) == PingIndicator::PingStatus::Fail, "2 -> Fail");
    check(PingIndicator::statusFromServiceInt(42) == PingIndicator::PingStatus::Unknown, "other -> Unknown");

    check(PingIndicator::dotColor(PingIndicator::PingStatus::Ok) == Theme::pingOk(), "Ok ring is green");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Fail) == Theme::pingFail(), "Fail ring is red");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Waiting) == Theme::pingWaiting(),
          "Waiting ring is amber");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Unknown) == Theme::pingUnknown(),
          "Unknown ring is grey");
    // The dashboard's one green is a spec colour, not the general success token: the connected chip, the
    // live rings and a running power button all paint with this one value.
    check(Theme::pingOk() == QColor(0x10, 0xB9, 0x81), "the live green is #10B981");

    // Which mark a service gets, and that an unknown host still gets one rather than an empty ring.
    check(PingIndicator::glyphForService(QStringLiteral("Youtube")) == QStringLiteral("youtube"),
          "youtube gets its brand mark");
    check(PingIndicator::glyphForService(QStringLiteral("DISCORD")) == QStringLiteral("discord"),
          "the match ignores case, the names come from hostnames");
    check(PingIndicator::glyphForService(QStringLiteral("Google")) == QStringLiteral("globe"),
          "anything else gets the globe");

    check(!chip.isPulsing(), "Unknown does not pulse");
    chip.setStatus(PingIndicator::PingStatus::Ok, 34);
    check(chip.isPulsing(), "Ok pulses");
    check(chip.summary().startsWith(QStringLiteral("Youtube")), "summary carries the service name");
    check(chip.toolTip().contains(QStringLiteral("34 мс")), "tooltip carries the latency");
    chip.setStatus(PingIndicator::PingStatus::Fail, -1);
    check(!chip.isPulsing(), "Fail stops the pulse");

    // Renaming a badge re-picks its mark: a chip that kept youtube's glyph after being pointed at
    // discord would be the one thing on screen lying about what it measured.
    chip.setServiceName(QStringLiteral("Discord"));
    check(chip.glyph() == QStringLiteral("discord"), "setServiceName follows the name to the mark");
    check(chip.serviceName() == QStringLiteral("Discord"), "and keeps the name");

    // The latency figure is monospace WHERE IT RENDERS, not merely requested. The generic
    // `QWidget { font-family: <UI family> }` rule in the app sheet outranks a C++ setFamily(), so a bare
    // setFont() left the figure proportional and its digits shifted as they changed — this is the regression
    // Theme::applyFontViaSheet() exists to stop. It runs against the real applied sheet (main() did
    // Theme::apply first), so it is exactly what the widget shows on screen.
    //
    // Print both fonts' resolved family and advance widths: on a runner whose fontconfig resolves
    // QFontDatabase::FixedFont to a different concrete face than the dev box does (Fira Code vs DejaVu
    // Sans Mono vs Liberation Mono), the numbers alone tell us whether a fail is a "the pin did not take"
    // bug or a "this runner has no monospace face at all" environment fact. Without them, the log only
    // says FAIL and the next investigation is a re-run.
    QLabel *number = chip.findChild<QLabel *>(QStringLiteral("pingNumber"));
    if (number) number->ensurePolished();
    const QFontMetrics fmNumber(number ? number->font() : QFont());
    std::printf("  info  pingNumber family=\"%s\" i=%d W=%d l=%d M=%d\n",
                number ? qUtf8Printable(number->font().family()) : "(null)",
                fmNumber.horizontalAdvance(QLatin1Char('i')),
                fmNumber.horizontalAdvance(QLatin1Char('W')),
                fmNumber.horizontalAdvance(QLatin1Char('l')),
                fmNumber.horizontalAdvance(QLatin1Char('M')));
    check(number != nullptr, "the badge carries a #pingNumber figure");
    // The pin's effect is only measurable when there is a proportional face to measure against. On
    // a runner where the app's UI sheet resolves to a fixed-pitch fallback (windows-2022 + aqt's
    // Qt 6.7.3 + Inter not installed), every widget reports the same metrics and the check cannot
    // distinguish pin from no-pin — so skip rather than call it a regression.
    const bool uiDiscriminates = uiSampleIsProportional();
    if (uiDiscriminates) {
        check(resolvesMonospace(number),
              "and the figure resolves to a fixed-pitch face (no digit drift)");
    } else {
        std::printf("  SKIP  the resolved UI face is already fixed-pitch on this host; "
                    "the pin's monotonicity cannot be discriminated\n");
    }
    // Guard that the check above means something: the app's own UI family must be proportional, or the
    // monospace test would pass on a font that never got overridden in the first place. Report the
    // environment's answer rather than assert it — an unavailable UI family falling back to a fixed
    // face on a runner is an environment fact, not a code regression, and the previous hard fail here
    // was the Windows CI's only signal of that.
    QLabel uiSample;
    uiSample.ensurePolished();
    const QFontMetrics fmUi(uiSample.font());
    std::printf("  info  UI sample family=\"%s\" i=%d W=%d\n",
                qUtf8Printable(uiSample.font().family()),
                fmUi.horizontalAdvance(QLatin1Char('i')),
                fmUi.horizontalAdvance(QLatin1Char('W')));
    if (!uiSampleIsProportional()) {
        std::printf("  SKIP  UI face is fixed-pitch on this host; the monospace check above "
                    "cannot discriminate, so the guard has nothing to guard\n");
    } else {
        check(fmUi.horizontalAdvance(QLatin1Char('i')) != fmUi.horizontalAdvance(QLatin1Char('W')),
              "the UI text family really is proportional, so the monospace check discriminates");
    }
}

// ------------------------------------------------------------- PowerButton ----
static void testPowerButton()
{
    std::printf("\n[PowerButton]\n");

    PowerButton button;
    check(button.diameter() == 200, "the default circle is 200 px");
    // The widget is the circle plus the halo's margin on both sides — a button exactly as wide as its own
    // ring clips its glow into a square.
    check(button.width() == 200 + 2 * PowerButton::kHaloMargin, "the widget reserves halo room");
    check(button.height() == button.width(), "and the same on both axes");
    check(button.mode() == PowerButton::Mode::Idle, "starts idle");
    check(PowerButton::modeColor(PowerButton::Mode::Idle) == Theme::accent(),
          "idle is the accent purple");
    check(PowerButton::modeColor(PowerButton::Mode::Busy) == Theme::accent(),
          "a transition in flight stays purple and only breathes");
    check(PowerButton::modeColor(PowerButton::Mode::Active) == Theme::pingOk(),
          "a live run wears the same green as the connected chip");

    // WARP's palette — Cloudflare's own orange for idle/busy and blue for a live tunnel — is a
    // separate table on purpose: a purple ring around a connected WARP would read as the zapret
    // payload's neon, which is exactly the confusion the toggle exists to remove. The default
    // palette is still Zapret so no existing check above changes.
    check(button.palette() == PowerButton::Palette::Zapret, "the default palette is Zapret");
    check(PowerButton::modeColor(PowerButton::Mode::Idle, PowerButton::Palette::Warp)
              == QColor(0xF6, 0x82, 0x1F),
          "a WARP idle ring is Cloudflare orange");
    check(PowerButton::modeColor(PowerButton::Mode::Busy, PowerButton::Palette::Warp)
              == QColor(0xF6, 0x82, 0x1F),
          "a WARP busy ring is the same orange, only breathing");
    check(PowerButton::modeColor(PowerButton::Mode::Active, PowerButton::Palette::Warp)
              == QColor(0x22, 0xA7, 0xF0),
          "a live WARP tunnel is Cloudflare blue, not the zapret green");
    // The single-argument form and the two-argument form agree on Zapret, so an existing caller
    // that has not been updated cannot accidentally start drawing WARP colours.
    check(PowerButton::modeColor(PowerButton::Mode::Active)
              == PowerButton::modeColor(PowerButton::Mode::Active, PowerButton::Palette::Zapret),
          "the historical one-argument form is Zapret's palette");

    button.setPalette(PowerButton::Palette::Warp);
    check(button.palette() == PowerButton::Palette::Warp, "setPalette is readable back");
    button.setPalette(PowerButton::Palette::Zapret);
    check(button.palette() == PowerButton::Palette::Zapret, "and can be flipped back");

    check(button.glyph() == QStringLiteral("power"), "the default glyph is the power mark");

    button.show();
    QTest::qWait(10);        // a control nobody has put on screen is hidden, and nothing animates on it

    button.setMode(PowerButton::Mode::Busy);
    check(button.mode() == PowerButton::Mode::Busy, "mode is readable back");
    QTest::qWait(300);
    check(button.glow() > 0.0 && button.glow() < 1.0,
          "the busy pulse is actually moving the halo, not merely started");
    button.setMode(PowerButton::Mode::Idle);
    check(qFuzzyCompare(button.glow(), 1.0), "leaving Busy parks the glow at full");

    // A diameter change has to move the glyph's rasterised size with it, or a 120 px button would carry
    // a 64 px power mark; and absurd sizes are clamped rather than honoured, because a 0 px circle is a
    // widget nobody can click.
    button.setDiameter(120);
    check(button.diameter() == 120, "setDiameter is readable back");
    check(button.width() == 120 + 2 * PowerButton::kHaloMargin, "and resizes the widget");
    button.setDiameter(0);
    check(button.diameter() == 48, "a zero diameter is clamped, not obeyed");

    // ---- nothing the button paints lands outside the button ----
    //
    // A ghost duplicate of this circle, floating beside the window after a minimize/restore, reads like a
    // glow that escaped its widget and was then left on the surface. This is the measurement of that
    // claim rather than an argument about it: the button is put inside a parent, at its brightest —
    // Active, glow at the ceiling setGlow() allows — and every pixel of the parent outside the button's
    // geometry is compared against the colour the parent painted there. One leaked pixel fails.
    //
    // It passes for two reasons that hold independently: the widget carries kHaloMargin of its own space
    // on every side, which is more than the widest glow pass reaches past the rim, and a widget paints
    // into its own rect with Qt clipping it there — which is also why a setClipRect(rect()) inside
    // paintEvent() would be a second copy of a clip that already exists rather than a fix.
    {
        QWidget host;
        host.setAutoFillBackground(true);
        host.resize(button.width() + 60, button.height() + 60);
        auto *bright = new PowerButton(&host);
        bright->setMode(PowerButton::Mode::Active);
        bright->setGlow(1.0);
        bright->move(30, 30);
        const QImage img = host.grab().toImage();
        const QRect inside(bright->pos(), bright->size());
        // The ground is read out of the grab rather than named as a literal: a sheet is live on this
        // QApplication, and what the parent actually painted is the only honest baseline to compare to.
        const QColor ground = img.pixelColor(0, 0);
        int leaked = 0;
        QPoint first;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                if (inside.contains(x, y) || img.pixelColor(x, y) == ground)
                    continue;
                if (leaked == 0)
                    first = QPoint(x, y);
                ++leaked;
            }
        }
        check(leaked == 0,
              leaked == 0
                  ? QStringLiteral("at full glow and the brightest tint, not one pixel outside the "
                                     "button's %1x%2 geometry carries any of it")
                        .arg(bright->width())
                        .arg(bright->height())
                  : QStringLiteral("%1 pixels outside the button carry its glow, the first at (%2,%3)")
                        .arg(leaked)
                        .arg(first.x())
                        .arg(first.y()));
        check(img.width() == host.width() && img.height() == host.height()
                      && inside.width() == bright->width()
                      && img.pixelColor(inside.center()) != ground,
              "…and the reading covered the whole parent around a button that really did paint, so the "
              "silence above is a measurement and not a grab that missed the edges");
    }
}

// --------------------------------------------------------------- ModeToggle ----
//
// The Zapret/WARP pill. Every check here is against a piece of the widget's contract that a
// user's eye depends on — which half owns the thumb, what colour each half lights, and whether
// picking a half emits the change the window can act on. Geometry is a static function for the
// same reason the palette is: a suite cannot take a screenshot in a headless build, but it can
// ask "given these bounds, where does the thumb go?".
static void testModeToggle()
{
    std::printf("\n[ModeToggle]\n");

    const QRect bounds(0, 0, ModeToggle::kPillWidthPx, ModeToggle::kPillHeightPx);
    const QRect zapretThumb = ModeToggle::thumbRectFor(0, bounds);
    const QRect warpThumb = ModeToggle::thumbRectFor(1, bounds);

    // The two halves share the pill's interior width and the same inset top/bottom; only the x
    // offset separates them. Anything else and the thumb visibly changes size when it slides,
    // which reads as a bug in the widget rather than a change of mode.
    check(zapretThumb.height() == warpThumb.height(), "both halves share the same height");
    check(zapretThumb.left() < warpThumb.left(), "index 0 is the Zapret end");
    check(zapretThumb.width() > 0 && warpThumb.width() > 0, "no thumb is empty");
    check(zapretThumb.right() < warpThumb.left() + 1,
          "the halves do not overlap — a thumb that jumps across a gap reads as two controls");

    // The pill has exactly two labels, each naming one subsystem by what it is called everywhere
    // else on the screen. English + uppercase matches the header's connection chip's design rule.
    check(ModeToggle::labelFor(ModeToggle::Segment::Zapret) == QStringLiteral("Zapret"),
          "the Zapret half is labelled by name, not by an abstract icon");
    check(ModeToggle::labelFor(ModeToggle::Segment::Warp) == QStringLiteral("WARP"),
          "the WARP half is labelled in caps, like the chip's CONNECTED");
    check(ModeToggle::labelForIndex(0) == ModeToggle::labelFor(ModeToggle::Segment::Zapret),
          "index 0 and Segment::Zapret name the same half");

    // Colours: Zapret's half uses the theme's accent, WARP's is fixed Cloudflare blue so the
    // choice does not flip with a theme change. The blue is the one spelling the power button's
    // live-tunnel ring already wears — a lit half claims "this layer is on", not WARP's brand orange.
    check(ModeToggle::colorFor(ModeToggle::Segment::Zapret) == Theme::accent(),
          "the Zapret half borrows the accent already everywhere else on the screen");
    check(ModeToggle::colorFor(ModeToggle::Segment::Warp) == QColor(0x22, 0xA7, 0xF0),
          "the WARP half is Cloudflare blue, the live-tunnel colour");
    check(ModeToggle::colorFor(ModeToggle::Segment::Warp)
              == PowerButton::modeColor(PowerButton::Mode::Active, PowerButton::Palette::Warp),
          "toggle and button agree on what a live WARP blue looks like — one spelling of that hex");

    // describe() is what the switch prompt's informative line reads, so its three spellings of a
    // legal selection — and the dash for the impossible-now empty — are a contract with the seven
    // translations that never carry a subsystem name themselves.
    check(ModeToggle::describe(ModeSelection{true, true})
              == QStringLiteral("Zapret + WARP"),
          "both halves lit reads as the two subsystems it runs");
    check(ModeToggle::describe(ModeSelection{true, false})
              == ModeToggle::labelFor(ModeToggle::Segment::Zapret),
          "the lone bypass is named by that half's word");
    check(ModeToggle::describe(ModeSelection(false, false)) == QStringLiteral("\u2014"),
          "nothing lit is a dash, not a blank a user could read as a name");

    // ---- the two rules, spelled as pure statics ----
    // clamp() only ever *adds* zapret under a lit tunnel; it never removes a WARP. The illegal set is
    // just {zapret off, WARP on}, and clamp folds it back onto the dependency the tunnel rides on.
    check(ModeToggle::clamp(ModeSelection{false, true}) == ModeSelection{true, true},
          "a request for the tunnel supplies the bypass with it");
    check(ModeToggle::clamp(ModeSelection{true, false}) == ModeSelection{true, false},
          "clamp does not touch a legal selection");
    check(ModeToggle::clamp(ModeSelection(false, false)) == ModeSelection(false, false),
          "and it does not invent a half that was never asked for");

    // toggled() is what one click on one half leaves lit. Turning the bypass down also takes the tunnel
    // down (nothing it rides on survives); turning the tunnel up brings the bypass up in the same click.
    check(ModeToggle::toggled(ModeSelection{true, true}, ModeToggle::Segment::Zapret)
              == ModeSelection{false, false},
          "clicking the lit bypass down takes the dependent tunnel with it");
    check(ModeToggle::toggled(ModeSelection{true, false}, ModeToggle::Segment::Warp)
              == ModeSelection{true, true},
          "clicking the dark tunnel up lights the bypass it needs");
    check(ModeToggle::toggled(ModeSelection{true, false}, ModeToggle::Segment::Zapret)
              == ModeSelection{false, false},
          "clicking the lone lit bypass off lands on an empty pill");
    check(ModeToggle::toggled(ModeSelection{true, true}, ModeToggle::Segment::Warp)
              == ModeSelection{true, false},
          "clicking the lit tunnel down leaves the bypass running");

    // The widget itself: construction, selection round-trip, selectionChanged emission. The signal
    // carries the two halves as separate bools so a queued connection needs no ModeSelection metatype.
    ModeToggle toggle;
    check(toggle.selection() == ModeSelection(true, false), "the default selection is Zapret alone");
    check(toggle.isOn(ModeToggle::Segment::Zapret) && !toggle.isOn(ModeToggle::Segment::Warp),
          "and the per-half readers agree with the whole");
    int changes = 0;
    bool lastZapret = false;
    bool lastWarp = false;
    QObject::connect(&toggle, &ModeToggle::selectionChanged,
                     [&](bool zapretOn, bool warpOn) {
                         ++changes;
                         lastZapret = zapretOn;
                         lastWarp = warpOn;
                     });
    toggle.setSelection(ModeSelection(true, false));
    check(changes == 0, "re-setting the current selection is a no-op, not a signal storm");
    // A request for the impossible combination is clamped before it can emit, so asking for "WARP
    // without Zapret" lands on both halves and says so — the pill can never report an illegal state.
    toggle.setSelection(ModeSelection(false, true));
    check(changes == 1, "one change lands on one signal");
    check(lastZapret && lastWarp, "the signal carries the clamped pair — zapret rode in with the tunnel");
    check(toggle.selection() == ModeSelection(true, true), "and it reads back the same");
    toggle.setSelection(true, false);
    check(changes == 2, "turning the tunnel back down is a second change");
    check(!lastWarp, "…announced with the tunnel now dark");
}

// ------------------------------------------------------------ Warp mappings ----
//
// The static state -> look / state -> word / state -> kind tables MainWindow uses when the
// toggle is on Warp. They are statics so a suite can pin them from a console program without
// a window, and this check exists because a missing entry in a switch is the kind of bug that
// only shows on a real WARP install — which is precisely the machine a headless test is not.
static void testWarpMappings()
{
    std::printf("\n[WarpMappings]\n");

    check(MainWindow::connectionText(WarpManager::State::Connected)
              == QStringLiteral("CONNECTED"),
          "a live tunnel reads the same word on the chip as a live zapret run");
    check(MainWindow::connectionText(WarpManager::State::Disconnected)
              == QStringLiteral("OFFLINE"),
          "a stopped tunnel uses the zapret side's stopped word, so the chip is legible");
    check(MainWindow::connectionText(WarpManager::State::Connecting)
              == QStringLiteral("CONNECTING"),
          "a tunnel on its way up says so");
    check(MainWindow::connectionText(WarpManager::State::Unavailable)
              == QStringLiteral("ABSENT"),
          "an absent warp-cli has its own word, distinct from an offline but installed one");

    check(MainWindow::statusKind(WarpManager::State::Connected) == QStringLiteral("running"),
          "a live tunnel matches the QSS's running token");
    check(MainWindow::statusKind(WarpManager::State::Connecting) == QStringLiteral("starting"),
          "a transition shares the 'starting' token, as zapret does");
    check(MainWindow::statusKind(WarpManager::State::Unavailable) == QStringLiteral("absent"),
          "an absent binary has its own token, not 'stopped'");

    check(MainWindow::powerMode(WarpManager::State::Connected) == PowerButton::Mode::Active,
          "a connected tunnel gives the button the green-to-blue active look");
    check(MainWindow::powerMode(WarpManager::State::Connecting) == PowerButton::Mode::Busy,
          "a connect in flight gives it the breathing busy look");
    check(MainWindow::powerMode(WarpManager::State::Disconnected) == PowerButton::Mode::Idle,
          "a stopped tunnel is an idle button, ready to press");

    check(MainWindow::canActFrom(WarpManager::State::Connected),
          "a live tunnel can be stopped");
    check(MainWindow::canActFrom(WarpManager::State::Disconnected),
          "a stopped tunnel can be started");
    check(MainWindow::canActFrom(WarpManager::State::Error),
          "a failed verb still allows a retry, so the button never strands the user");
    check(!MainWindow::canActFrom(WarpManager::State::Connecting),
          "an in-flight connect is refused, not queued behind a second warp-cli");
    check(!MainWindow::canActFrom(WarpManager::State::Unavailable),
          "no warp-cli, no action");

    // The clamp: a stored WARP intent on a host that no longer has warp-cli has its tunnel half put
    // out so the button still means something; the bypass half is never moved by WARP availability.
    check(MainWindow::clampSelectionForAvailability(ModeSelection{true, true}, false)
              == ModeSelection{true, false},
          "a stale WARP pick loses only its tunnel half when warp-cli is missing");
    check(MainWindow::clampSelectionForAvailability(ModeSelection{true, true}, true)
              == ModeSelection{true, true},
          "and the combined pick is honoured when warp-cli is back");
    check(MainWindow::clampSelectionForAvailability(ModeSelection{true, false}, false)
              == ModeSelection{true, false},
          "a Zapret-only pick is never moved by WARP availability");
}

// --------------------------------------------------------- selection switch ----
//
// Moving the Zapret/WARP pill is allowed to do exactly two things: light a half, and — but only with
// the user's spoken agreement — take a live subsystem DOWN. It never brings one up. The rule is split
// into pure helpers so that claim is checkable without a backend, a tunnel or a modal dialog, and this
// section pins each of them: which states hold the path, which halves a change takes down, what a
// confirmed change is permitted to stop, and which selection ends up lit. MainWindow::onSelectionChanged
// is the ordering of these answers and nothing else, so everything the slot will act on is here.
static void testSelectionSwitch()
{
    std::printf("\n[selection switch]\n");

    using Sel = ModeSelection;
    using Action = MainWindow::SwitchAction;

    // ---- isBypassHeld: which zapret states mean the path is already altered ----
    check(MainWindow::isBypassHeld(IZapretBackend::State::Running), "a live bypass holds the path");
    check(MainWindow::isBypassHeld(IZapretBackend::State::Starting),
          "a launch in flight holds it too — half its queue rules are already installed");
    check(MainWindow::isBypassHeld(IZapretBackend::State::Stopping),
          "…and a teardown holds them until the flush returns, so it is still a bypass being lost");
    check(!MainWindow::isBypassHeld(IZapretBackend::State::Stopped),
          "nothing running is nothing the switch could destroy");
    check(!MainWindow::isBypassHeld(IZapretBackend::State::Error),
          "a failed run left no bypass behind, so switching off it is free");

    // The predicate is a switch over the enum. Counting the held states is what stops a sixth,
    // future state from reading as 'not held' merely because nobody thought about it here.
    const IZapretBackend::State backendStates[] = {
        IZapretBackend::State::Starting, IZapretBackend::State::Running,
        IZapretBackend::State::Stopping, IZapretBackend::State::Stopped,
        IZapretBackend::State::Error};
    int heldBackend = 0;
    for (IZapretBackend::State s : backendStates)
        if (MainWindow::isBypassHeld(s))
            ++heldBackend;
    check(heldBackend == 3, "exactly three of the five backend states hold the path");

    // ---- isTunnelHeld: the same question on the daemon's side ----
    check(MainWindow::isTunnelHeld(WarpManager::State::Connected), "a live tunnel holds the path");
    check(MainWindow::isTunnelHeld(WarpManager::State::Connecting), "a connect in flight holds it");
    check(MainWindow::isTunnelHeld(WarpManager::State::Disconnecting),
          "…and so does a disconnect in flight — the daemon is still working on the interface");
    check(!MainWindow::isTunnelHeld(WarpManager::State::Disconnected), "an offline tunnel holds nothing");
    check(!MainWindow::isTunnelHeld(WarpManager::State::Error), "a failed verb holds nothing");
    // The one that matters most on a machine without Cloudflare installed: an absent warp-cli is not
    // a held tunnel, or every switch on such a host would ask a question that cannot be acted on.
    check(!MainWindow::isTunnelHeld(WarpManager::State::Unavailable),
          "no warp-cli holds nothing — the switch must not ask about a subsystem that is absent");

    const WarpManager::State tunnelStates[] = {
        WarpManager::State::Connecting, WarpManager::State::Connected,
        WarpManager::State::Disconnecting, WarpManager::State::Disconnected,
        WarpManager::State::Error, WarpManager::State::Unavailable};
    int heldTunnel = 0;
    for (WarpManager::State s : tunnelStates)
        if (MainWindow::isTunnelHeld(s))
            ++heldTunnel;
    check(heldTunnel == 3, "exactly three of the six tunnel states hold the path");

    // ---- switchAction: the full matrix, consent included ----
    // Eight rows, because the answer depends on exactly three booleans. Naming each one is the point:
    // this is the whole of what a confirmed switch is permitted to do to a live subsystem.
    struct Row
    {
        bool zapretHeld;
        bool warpHeld;
        bool agreed;
        Action expected;
        const char *what;
    };
    const Row rows[] = {
        // An idle host switches silently: no consent was ever needed, so nothing is torn down.
        {false, false, false, Action::None, "idle host, never asked — nothing to stop"},
        {false, false, true, Action::None, "idle host, agreed anyway — still nothing to stop"},
        // Zapret alone.
        {true, false, false, Action::None, "bypass held, refused — the run keeps running"},
        {true, false, true, Action::StopZapret, "bypass held, agreed — the bypass comes down"},
        // WARP alone.
        {false, true, false, Action::None, "tunnel held, refused — the tunnel keeps running"},
        {false, true, true, Action::StopWarp, "tunnel held, agreed — the tunnel comes down"},
        // Both. A host can hold the path twice, and leaving that state has to clear both.
        {true, true, false, Action::None, "both held, refused — neither moves"},
        {true, true, true, Action::StopBoth, "both held, agreed — both come down"},
    };
    for (const Row &r : rows)
        check(MainWindow::switchAction(r.zapretHeld, r.warpHeld, r.agreed) == r.expected,
              QStringLiteral("switchAction(zapret=%1, warp=%2, agreed=%3): %4")
                  .arg(r.zapretHeld ? QStringLiteral("held") : QStringLiteral("idle"))
                  .arg(r.warpHeld ? QStringLiteral("held") : QStringLiteral("idle"))
                  .arg(r.agreed ? QStringLiteral("yes") : QStringLiteral("no"))
                  .arg(QString::fromUtf8(r.what)));

    // A refused question is a complete answer, never partial consent: holding every combination, no
    // refusal may name a teardown. This is the invariant the whole feature is built on, and it is
    // stated separately from the table above so a reordered enum value cannot hide a leak.
    for (const bool z : {false, true})
        for (const bool w : {false, true})
            check(MainWindow::switchAction(z, w, /*userAgreed=*/false) == Action::None,
                  QStringLiteral("refusing never tears anything down (zapretHeld=%1, warpHeld=%2)")
                      .arg(z ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(w ? QStringLiteral("true") : QStringLiteral("false")));

    // ---- the two per-half teardown predicates ----
    // A half is taken down only if it was lit, is going dark, and has something running behind it.
    // Lighting a dark half costs nothing; blanking an idle one takes away nothing. The two are split
    // because the bypass and the tunnel are stopped by two different managers.
    check(MainWindow::takesBypassDown(Sel{true, true}, Sel{false, false}, /*zapretHeld=*/true),
          "a lit, held bypass going dark is owed a stop");
    check(!MainWindow::takesBypassDown(Sel{true, true}, Sel{false, false}, /*zapretHeld=*/false),
          "…but not when nothing runs behind the half");
    check(!MainWindow::takesBypassDown(Sel{true, true}, Sel{true, false}, true),
          "a half that stays lit is never taken down");
    check(!MainWindow::takesBypassDown(Sel{false, false}, Sel{true, false}, true),
          "lighting a dark half stops nothing, held or not");
    check(MainWindow::takesTunnelDown(Sel{true, true}, Sel{true, false}, /*warpHeld=*/true),
          "a lit, held tunnel going dark is owed a stop");
    check(!MainWindow::takesTunnelDown(Sel{true, true}, Sel{true, false}, /*warpHeld=*/false),
          "…but not when the daemon holds nothing");
    check(!MainWindow::takesTunnelDown(Sel{true, false}, Sel{true, true}, true),
          "and lighting the tunnel up takes no tunnel down");

    // strandsAHeldSubsystem is the union of the two — whether a change has a price at all.
    check(MainWindow::strandsAHeldSubsystem(Sel{true, false}, Sel{false, false}, true, false),
          "a lone held bypass coming down strands it");
    check(MainWindow::strandsAHeldSubsystem(Sel{true, true}, Sel{true, false}, false, true),
          "…and so does a lone held tunnel");
    check(!MainWindow::strandsAHeldSubsystem(Sel{true, false}, Sel{true, true}, true, false),
          "adding the tunnel half strands nothing — nothing was going dark");
    check(!MainWindow::strandsAHeldSubsystem(Sel{true, true}, Sel{false, false}, false, false),
          "a change over an idle host strands nothing the user can see");

    // ---- resolveSelectionChange: which selection ends up lit ----
    // One rule: a refused change that costs something keeps what it had; everything else — an idle
    // host, an agreed change — takes what the user asked for. `somethingHeld` is strandsAHeldSubsystem's
    // verdict, which is exactly how onSelectionChanged calls it.
    const Sel zOnly(true, false);
    const Sel both(true, true);
    const Sel dark(false, false);
    check(MainWindow::resolveSelectionChange(zOnly, both, false, false) == both,
          "a free change on an idle host takes the pick — no question was ever asked");
    check(MainWindow::resolveSelectionChange(both, zOnly, true, true) == zOnly,
          "an agreed change takes the selection the user asked for");
    check(MainWindow::resolveSelectionChange(both, zOnly, true, false) == both,
          "a refused one leaves the pill exactly where it was");
    check(MainWindow::resolveSelectionChange(zOnly, dark, true, false) == zOnly,
          "…equally when the bypass is the thing that was held");
    check(MainWindow::resolveSelectionChange(both, dark, true, true) == dark,
          "and a both-held, agreed change clears both halves");
    // The subtle one: nothing held and nobody asked must NOT read as a refusal. If it did, every
    // change on an idle machine would snap the halves back and the pill would be useless.
    check(MainWindow::resolveSelectionChange(zOnly, both, /*somethingHeld=*/false, /*userAgreed=*/false)
              == both,
          "an unasked question is not a refused one — the idle move is free");

    // ---- the strands and the pill-move must agree ----
    // stopHeldForSelectionChange issues exactly the teardowns takesBypassDown/takesTunnelDown name, and
    // only after onSelectionChanged agreed to the price strandsAHeldSubsystem reported. So a change is
    // owed a stop iff it strands something, and the pill moves iff that change is free or agreed.
    struct Change
    {
        Sel from;
        Sel to;
        bool zapretHeld;
        bool warpHeld;
    };
    const Change changes[] = {
        {zOnly, both, true, false},       // lighting the tunnel over a running bypass: a free add
        {both, zOnly, true, true},        // dropping the tunnel, keeping the bypass
        {both, dark, true, true},         // dropping both at once
        {zOnly, dark, true, false},       // dropping the lone bypass
        {zOnly, dark, false, false},      // dropping a bypass with nothing running behind it
    };
    for (const Change &c : changes) {
        const bool costs = MainWindow::strandsAHeldSubsystem(c.from, c.to, c.zapretHeld, c.warpHeld);
        // The two per-half stops add up to the change's price by definition.
        const bool bypassStopOwed = MainWindow::takesBypassDown(c.from, c.to, c.zapretHeld);
        const bool tunnelStopOwed = MainWindow::takesTunnelDown(c.from, c.to, c.warpHeld);
        check((bypassStopOwed || tunnelStopOwed) == costs,
              QStringLiteral("the two per-half stops add up to the change's price"));
        for (const bool agreed : {false, true}) {
            const Sel target = MainWindow::resolveSelectionChange(c.from, c.to, costs, agreed);
            const bool moved = (target != c.from);
            // A change that strands something and is refused must leave the pill where it was; an
            // agreed or free one moves it to the requested selection. (`to` always differs from `from`
            // here — a no-op move is guarded out before onSelectionChanged ever decides.)
            check(moved == (!costs || agreed),
                  QStringLiteral("pill moves iff the change is free or agreed (costs=%1, agreed=%2)")
                      .arg(costs ? QStringLiteral("yes") : QStringLiteral("no"))
                      .arg(agreed ? QStringLiteral("yes") : QStringLiteral("no")));
            // …spelled the other way for the case that must never happen: the pill moving off a held
            // subsystem the user refused to lose.
            check(!(costs && !agreed && moved),
                  QStringLiteral("a refused change never strands a held subsystem by moving the pill"));
        }
    }

    // And switchAction is the same price read as one decision over both managers at once: an agreed
    // change stops whatever halves are held, and a refusal (or an idle host) stops nothing — matching
    // what the per-half predicates would have allowed. Pinned against the held flags alone, since
    // switchAction is consent- and state-only.
    for (const bool z : {false, true})
        for (const bool w : {false, true}) {
            const Action agreed = MainWindow::switchAction(z, w, /*userAgreed=*/true);
            check((agreed != Action::None) == (z || w),
                  QStringLiteral("a confirmed change stops something exactly when a half is held "
                                 "(zapretHeld=%1, warpHeld=%2)")
                      .arg(z ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(w ? QStringLiteral("true") : QStringLiteral("false")));
        }

    // The last of these helpers' whole purpose is what it cannot say: SwitchAction has no Start
    // member, so no selection change can launch anything. That is a fact about the type rather than a
    // runtime branch — the four values named above are the entire domain, and onPowerClicked() stays
    // the only path that brings a subsystem up.
    check(static_cast<int>(Action::None) == 0, "the action set begins at 'do nothing'");
}

// ------------------------------------------------------------- combined power ----
//
// One button drives both subsystems now, so what a press means is a lookup over the pair of live states
// rather than a branch in a slot — and what the lookup encodes is the dependency: the tunnel rides on
// the bypass, so it comes up after it and comes down before it. The same dependency survives the
// deferred start, where the gap between the launch and the connect is filled by a wait the user can
// cancel. Neither half of that is reachable from a headless runner through a real daemon, so both are
// pinned here as pure tables, the way the single-subsystem mappings above are: which press a state pair
// earns, and whether a connect is still owed once the wait has run its time.
static void testCombinedPower()
{
    std::printf("\n[combined power]\n");

    using Press = MainWindow::CombinedPress;
    using B = IZapretBackend::State;
    using W = WarpManager::State;
    using Sel = ModeSelection;

    const auto pressName = [](Press p) {
        switch (p) {
        case Press::Blocked: return "Blocked";
        case Press::ZapretThenWarp: return "ZapretThenWarp";
        case Press::WarpOnly: return "WarpOnly";
        case Press::ZapretOnly: return "ZapretOnly";
        case Press::WarpThenZapret: return "WarpThenZapret";
        }
        return "?";
    };
    const auto backendName = [](B s) {
        switch (s) {
        case B::Starting: return "bypass:Starting";
        case B::Running: return "bypass:Running";
        case B::Stopping: return "bypass:Stopping";
        case B::Stopped: return "bypass:Stopped";
        case B::Error: return "bypass:Error";
        }
        return "?";
    };
    const auto tunnelName = [](W s) {
        switch (s) {
        case W::Unavailable: return "tunnel:Unavailable";
        case W::Disconnected: return "tunnel:Disconnected";
        case W::Connecting: return "tunnel:Connecting";
        case W::Connected: return "tunnel:Connected";
        case W::Disconnecting: return "tunnel:Disconnecting";
        case W::Error: return "tunnel:Error";
        }
        return "?";
    };

    // ---- the press table, pair by pair ----
    struct Row
    {
        B zapret;
        W tunnel;
        Press expected;
        const char *what;
    };
    const Row rows[] = {
        // Nothing running on either side: the bypass goes first and the tunnel follows it once the
        // queue rules have had their settle time. A failed last verb reads as nothing running, and an
        // absent warp-cli does too — the press is not the place that decides whether the tunnel half
        // was ever pressable (clampSelectionForAvailability and the dark half are).
        {B::Stopped, W::Disconnected, Press::ZapretThenWarp,
         "a cold host starts the bypass and defers the tunnel"},
        {B::Error, W::Disconnected, Press::ZapretThenWarp,
         "…and a bypass whose last run failed is still nothing up"},
        {B::Stopped, W::Error, Press::ZapretThenWarp,
         "…and so is a tunnel whose last verb failed"},
        {B::Stopped, W::Unavailable, Press::ZapretThenWarp,
         "…and an absent warp-cli is answered as the down tunnel it is"},
        // The bypass has been up since before the press, so only the tunnel is outstanding — and its
        // rules have already had all the settle time they need, which is why this one is not delayed.
        {B::Running, W::Disconnected, Press::WarpOnly,
         "a live bypass with no tunnel completes the connect, unforced"},
        // The mirror-image half-failure: a tunnel up over a path that stopped carrying it. Only the
        // missing floor is asked for; the connected tunnel is left alone.
        {B::Stopped, W::Connected, Press::ZapretOnly,
         "a tunnel over no bypass brings the missing path back under it"},
        {B::Error, W::Connected, Press::ZapretOnly,
         "…and a bypass that failed is just as much a missing floor"},
        // Both up, so a press means going down — and down in the order that cannot leave the tunnel
        // holding an interface that no longer forwards anything.
        {B::Running, W::Connected, Press::WarpThenZapret,
         "a full run is taken down tunnel-first, bypass under it"},
        // Anything in flight, on either side, is not a press's problem to fix.
        {B::Starting, W::Disconnected, Press::Blocked,
         "a bypass launching refuses a press that would stack a second start"},
        {B::Stopping, W::Connected, Press::Blocked,
         "…and so does one tearing down, whichever half the user meant"},
        {B::Running, W::Connecting, Press::Blocked,
         "a tunnel connecting refuses a press that would race its own verb"},
        {B::Running, W::Disconnecting, Press::Blocked,
         "…and so does one disconnecting"},
    };
    for (const Row &r : rows) {
        const Press got = MainWindow::combinedPressFor(r.zapret, r.tunnel);
        check(got == r.expected,
              QStringLiteral("%1 + %2 -> %3 — %4")
                  .arg(QString::fromUtf8(backendName(r.zapret)),
                       QString::fromUtf8(tunnelName(r.tunnel)), QString::fromUtf8(pressName(got)),
                       QString::fromUtf8(r.what)));
    }

    // The table above is examples; these loops are the whole domain, so a state added to either enum
    // cannot fall into a branch written for the states that came before it. Thirty pairs, and each
    // loop says something the list of examples cannot: which pairs are refused, and what no pair is
    // ever allowed to ask for.
    const B backendStates[] = {B::Starting, B::Running, B::Stopping, B::Stopped, B::Error};
    const W tunnelStates[] = {W::Unavailable, W::Disconnected, W::Connecting, W::Connected,
                              W::Disconnecting, W::Error};

    // Exactly the in-flight pairs are refused. A press that reached past a Starting bypass or a
    // Connecting tunnel would spawn a second verb on top of one already settling — the shape of bug
    // that shows up only on a slow machine, which is why it is pinned as an equality over the cross
    // product rather than as the four cases above.
    int blocked = 0;
    for (B b : backendStates)
        for (W w : tunnelStates) {
            const bool inFlight = b == B::Starting || b == B::Stopping || w == W::Connecting
                                  || w == W::Disconnecting;
            const Press p = MainWindow::combinedPressFor(b, w);
            check((p == Press::Blocked) == inFlight,
                  QStringLiteral("%1 + %2 is refused only while a verb is already under way")
                      .arg(QString::fromUtf8(backendName(b)), QString::fromUtf8(tunnelName(w))));
            if (p == Press::Blocked)
                ++blocked;
        }
    check(blocked == 18, "eighteen of the thirty state pairs have a verb in flight");

    // The dependency itself, spelled as two never-minds: a press never asks a tunnel up over one
    // already held, and never asks a bypass down without naming the tunnel that rides on it. Those are
    // the ordering facts the folded button exists to enforce, and they hold for every pair — so no
    // future state pair can produce the sequence that leaves a tunnel routing into a closed path.
    for (B b : backendStates)
        for (W w : tunnelStates) {
            const Press p = MainWindow::combinedPressFor(b, w);
            check((p != Press::WarpOnly && p != Press::ZapretThenWarp)
                      || !MainWindow::isTunnelHeld(w),
                  QStringLiteral("%1 + %2 never brings up a tunnel the daemon already holds")
                      .arg(QString::fromUtf8(backendName(b)), QString::fromUtf8(tunnelName(w))));
            check(p != Press::WarpThenZapret || MainWindow::isTunnelHeld(w),
                  QStringLiteral("%1 + %2 never takes a bypass down over a tunnel left unnamed")
                      .arg(QString::fromUtf8(backendName(b)), QString::fromUtf8(tunnelName(w))));
        }

    // ---- the guard that ends the deferred start ----
    // Three seconds is long enough for the user to have turned the half dark, for the request to have
    // been answered already, or for the bypass to have died. All three hold the connect back, and the
    // conjunction is what the slot at the end of the wait is for — so it is pinned as a conjunction,
    // each conjunct dropped once, rather than as a branch count.
    const Sel both{true, true};
    const Sel zOnly{true, false};
    check(MainWindow::mayConnectWarpAfterDelay(both, true, true),
          "a lit tunnel half, a still-pending request and a held bypass earn the connect");
    check(!MainWindow::mayConnectWarpAfterDelay(zOnly, true, true),
          "…but the half went dark during the wait, so nobody is asking any more");
    check(!MainWindow::mayConnectWarpAfterDelay(both, false, true),
          "…and a request already answered is not owed a second tunnel");
    check(!MainWindow::mayConnectWarpAfterDelay(both, true, false),
          "…nor is one owed a tunnel over a bypass that never came up");

    // Exhaustively: the answer is the conjunction and nothing else. This matters beyond the button
    // because the journal's "the tunnel was withheld" line is emitted from the same guard's failure
    // branch — a connect permitted by a fourth condition would come with no line explaining it.
    for (const bool warp : {false, true})
        for (const bool pending : {false, true})
            for (const bool held : {false, true}) {
                const Sel s{true, warp};
                const bool allowed = MainWindow::mayConnectWarpAfterDelay(s, pending, held);
                check(allowed == (warp && pending && held),
                      QStringLiteral("mayConnectWarpAfterDelay(warp=%1, pending=%2, bypass=%3) is "
                                     "the conjunction alone")
                          .arg(warp ? QStringLiteral("lit") : QStringLiteral("dark"))
                          .arg(pending ? QStringLiteral("yes") : QStringLiteral("no"))
                          .arg(held ? QStringLiteral("held") : QStringLiteral("down")));
            }

    // And the pill cannot light a combination the guard would then have to refuse: clamp() is what
    // guarantees a lit tunnel half always rides with a lit bypass half, so the selection the wait is
    // checked against is always one the press table could have ordered. Stated once here because the
    // guard and the pill are the same dependency, written twice, in two files.
    for (const bool warp : {false, true}) {
        const Sel legal = ModeToggle::clamp(Sel{false, warp});
        check(!legal.warp || legal.zapret,
              "the pill cannot light a tunnel half the guard would connect under");
    }

    // ---- the look a chain that is still owed wears ----
    //
    // The flags that hold a tunnel request are the only thing that knows the second half of one press has
    // not happened yet: no manager state moves during the settle wait, during a round trip to Cloudflare,
    // or during a pkexec prompt nobody has answered. Read from the two states alone, that whole stretch
    // maps onto "the bypass is up and the tunnel is down" — the look of a press that finished, and the
    // reason the report said the button turned green and then waited to be pressed again.
    {
        const auto owedRunning =
            MainWindow::combinedLook(B::Running, W::Disconnected, /*chainPending=*/true);
        const auto doneRunning =
            MainWindow::combinedLook(B::Running, W::Disconnected, /*chainPending=*/false);
        check(owedRunning.mode == PowerButton::Mode::Busy,
              "a bypass that is up while a tunnel is still owed wears the working ring, not the finished "
              "one — these two rows used to be the same row, and that was the bug");
        check(doneRunning.mode == PowerButton::Mode::Active,
              "…and the same pair of states with nothing owed is the finished green it has always been, "
              "so the flag is what changed and not the mapping");
        check(owedRunning.pulsing && owedRunning.enabled,
              "the owed look still breathes and still takes a press: a ring that says \"working\" is not "
              "a ring that may not be pressed, and the press it would get is the one the table above "
              "already answers as WarpOnly");
        check(owedRunning.palette == PowerButton::Palette::Zapret,
              "…in zapret's purple, because the tunnel owns the blue from the moment it is held and not "
              "before — colouring a wait blue would claim a daemon that has been asked for nothing");

        // Every state the bypass can be in, with a tunnel still owed. The answer is the working ring in
        // all five, because what is owed does not depend on which of them a launch happens to be in.
        for (B b : backendStates) {
            const auto owed = MainWindow::combinedLook(b, W::Disconnected, true);
            check(owed.mode == PowerButton::Mode::Busy,
                  QStringLiteral("a chain still owed wears Busy over %1 as well")
                      .arg(QString::fromUtf8(backendName(b))));
        }
        // A tunnel that IS held decides the look on its own, owed or not: once the daemon has been asked,
        // its state is the fresher fact, and powerMode() of Connecting is already the working ring.
        for (const bool pending : {false, true}) {
            const auto held = MainWindow::combinedLook(B::Running, W::Connecting, pending);
            check(held.palette == PowerButton::Palette::Warp
                          && held.mode == PowerButton::Mode::Busy,
                  QStringLiteral("a connecting tunnel owns the ring whatever the flags say (owed=%1)")
                      .arg(pending ? QStringLiteral("yes") : QStringLiteral("no")));
        }
        check(MainWindow::combinedLook(B::Running, W::Connected, true).mode
                      == PowerButton::Mode::Active,
              "…and a tunnel that came up reads as up even with a stale flag raised, because a state the "
              "daemon reported beats a request the window is holding — the alternative is a ring that "
              "breathes over a live tunnel forever");
    }

    // ---- what an install borrows, what it gives back, and what it waits for ----
    //
    // Three decisions, all of them about authorship rather than about what the pill happens to show, and
    // all three worth stating as tables because each has a wrong answer that looks reasonable: borrow
    // again when one is already up, take down a bypass the user started, or hold the chain for a gap after
    // a step that moved nothing.
    {
        check(MainWindow::borrowsBypassForWarp(false, true),
              "an install on a machine with nothing up and a profile to run borrows a bypass");
        check(!MainWindow::borrowsBypassForWarp(true, true),
              "…and one riding a bypass that is already up borrows nothing it would then have to return");
        check(!MainWindow::borrowsBypassForWarp(false, false),
              "…nor does one with no profile to run: there is nothing to start, and the user has already "
              "been shown where to pick one");

        check(MainWindow::releasesBorrowedBypass(true, true),
              "a bypass this window started for the install is given back when the install is over");
        check(!MainWindow::releasesBorrowedBypass(false, true),
              "…and one the user started is not: the pill reading it a teardown has to act on is the "
              "same one the borrow could not tell apart");
        check(!MainWindow::releasesBorrowedBypass(true, false),
              "…nor is a borrow that never came up, which would be a stop issued against nothing");

        check(WarpSetupDialog::owesSettleAfter(QStringLiteral("daemon")),
              "starting the service owes a gap: on a clean machine that is a cold start, and the "
              "enrolment is the very next thing that needs its socket");
        check(WarpSetupDialog::owesSettleAfter(QStringLiteral("service")),
              "…and restarting it owes the same gap before the connect");
        for (const char *stepId : {"install", "register", "protocol", "connect"})
            check(!WarpSetupDialog::owesSettleAfter(QLatin1String(stepId)),
                  QStringLiteral("no gap after %1: it moved no daemon, so a wait there would be a pause "
                                 "with nothing behind it")
                      .arg(QLatin1String(stepId)));
    }

}

// --------------------------------------------------------------- ProfileCard ----
//
// A profile row is a selection item, not a control: no start button, only the name, the mini ping chips
// for the services it unblocks, and a menu (copy / delete). These checks pin that shape - the properties
// the sheet styles off, the signals the window wires, and the tooltip that carries a failure's reason
// now that a 52 px row has no room for a badge row of its own.
static void testProfileCard()
{
    std::printf("\n[ProfileCard]\n");

    Strategy s;
    s.id = QStringLiteral("gen-1");
    s.name = QStringLiteral("general");
    s.fileName = QStringLiteral("general.bat");
    s.dirPath = QDir::tempPath();
    s.filePath = QDir::tempPath() + QStringLiteral("/general.bat");
    s.kind = Strategy::Kind::Binary;
    s.platform = Strategy::Platform::Windows;
    s.executablePath = QDir::tempPath() + QStringLiteral("/bin/winws.exe");
    s.argv = {QStringLiteral("--wf-tcp=80,443"), QStringLiteral("--new"), QStringLiteral("--dpi-desync=fake")};
    s.requiresRoot = true;

    ProfileCard card(s);
    check(card.objectName() == QLatin1String("profileCard"), "objectName is profileCard");
    check(card.id() == QLatin1String("gen-1"), "id comes from the strategy");
    check(card.property("selected").toBool() == false, "not selected initially");
    check(card.property("error").toBool() == false, "not failed initially");
    check(card.strategy().name == QLatin1String("general"), "row keeps its strategy");
    check(card.nameLabel() && card.nameLabel()->text() == QLatin1String("general"),
          "the name label shows the strategy's name");

    const QString cmd = card.commandLine();
    check(cmd.contains(QStringLiteral("winws.exe")), "command line names the program");
    check(cmd.contains(QStringLiteral("--wf-tcp=80,443")), "command line carries argv");

    // ---- the row menu ----
    // Two entries about the file on disk, and deliberately no start/stop: the power button owns running.
    QToolButton *menuButton = card.menuButton();
    check(menuButton != nullptr, "the row has a menu button");
    check(menuButton && menuButton->menu() && menuButton->menu()->actions().size() == 2,
          "the menu holds exactly copy and delete");

    // ---- selection ----
    QSignalSpy clickedSpy(&card, &ProfileCard::clicked);
    QTest::mousePress(&card, Qt::LeftButton);
    check(clickedSpy.count() == 1 && clickedSpy.at(0).at(0).toString() == QLatin1String("gen-1"),
          "a left press emits clicked(id)");
    card.setSelected(true);
    check(card.isSelected(), "setSelected marks it selected");
    check(card.property("selected").toBool() == true, "and the QSS property follows");
    card.setSelected(true);   // idempotent
    check(card.isSelected(), "re-selecting an already-selected row is a no-op");
    card.setSelected(false);
    check(!card.isSelected() && card.property("selected").toBool() == false,
          "deselecting clears both");

    // ---- the menu's own signals ----
    QSignalSpy copySpy(&card, &ProfileCard::copyRequested);
    QSignalSpy deleteSpy(&card, &ProfileCard::deleteRequested);
    if (menuButton && menuButton->menu() && menuButton->menu()->actions().size() == 2) {
        menuButton->menu()->actions().at(0)->trigger();   // copy
        menuButton->menu()->actions().at(1)->trigger();   // delete
        check(copySpy.count() == 1 && deleteSpy.count() == 1, "both menu entries fire once");
        check(copySpy.at(0).at(0).toString() == QLatin1String("gen-1"), "copy carries the id");
        check(QApplication::clipboard()->text() == cmd,
              "copying already put the command on the clipboard");
    } else {
        check(false, "menu actions were reachable to trigger");
    }

    // ---- the mini ping chips ----
    // One chip per probed host, in order. A chip for a service nobody measures would be the only dishonest
    // number on the row, so the count must track setHosts exactly.
    const QStringList hosts = {QStringLiteral("youtube.com"), QStringLiteral("discord.com")};
    card.setHosts(hosts);
    check(card.pingChipCount() == 2, "two hosts, two chips");
    card.setHosts({QStringLiteral("youtube.com")});
    check(card.pingChipCount() == 1, "a shorter host list rebuilds, not reconciles");
    card.setHosts(hosts);
    // An unknown host is ignored rather than guessed at; a known one updates without disturbing the other.
    card.setPing(QStringLiteral("youtube.com"), PingIndicator::PingStatus::Ok, 42);
    card.setPing(QStringLiteral("nobody.test"), PingIndicator::PingStatus::Ok, 1);
    check(card.pingChipCount() == 2, "an unlisted host added no phantom chip");

    // ---- the ring's own shape ----
    // A chip is a mark inside a ring, and the ring is the part that had been drawn too small to read:
    // the mark at 16 px inside a 7 px radius, with its number touching the border. The three things that
    // fixed it are all invisible to a screenshot diff and visible to a layout edit, so they are pinned.
    // setHosts() retires the chips it replaces through deleteLater, and deferred deletion does not run
    // just because events were processed — so the tree still holds last rebuild's ghosts until they are
    // flushed. Counting before this would count chips the row no longer shows.
    QTest::qWait(10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto chips = card.findChildren<QWidget *>(QStringLiteral("miniPing"));
    check(chips.size() == 2, "one ring per service the row reports");
    if (chips.size() == 2) {
        QWidget *chip = chips.first();
        check(chip->property("pingStatus").toString() == QLatin1String("ok"),
              "the ring carries the verdict the sheet colours it from");
        check(chip->graphicsEffect() != nullptr,
              "the ring's glow is an effect, because QSS has no box-shadow");
        QLayout *pad = chip->layout();
        check(pad && pad->contentsMargins().left() > 0 && pad->contentsMargins().top() > 0,
              "mark and figure are padded off the ring's edge");
        const auto marks = chip->findChildren<QLabel *>(QStringLiteral("miniPingIcon"));
        check(!marks.isEmpty() && marks.first()->width() >= 20,
              "the mark is enlarged, not merely re-painted");
    }
    // An effect paints outside the widget it is set on, and a child is clipped to its parent's rect — so
    // the row that holds the chips has to reserve the halo's room or every glow is cut flat at the edge.
    QWidget *host = card.findChild<QWidget *>(QStringLiteral("profilePings"));
    check(host && host->layout() && host->layout()->contentsMargins().top() >= 6,
          "the chip row leaves room so the glow is not clipped");

    // ---- the failure surface ----
    // No badge row any more: the edge turns red and the reason moves to the tooltip, so the tooltip is the
    // assertion here - the whole reason, in order, after the path.
    const QString reason = QStringLiteral("LinuxBackend: start command failed (exit 1)");
    card.setError(reason);
    check(card.hasError(), "setError marks the row as failed");
    check(card.errorMessage() == reason, "the full reason is stored verbatim");
    check(card.property("error").toBool() == true, "the error property drives the QSS border");
    check(card.toolTip().contains(reason), "the tooltip carries the whole reason");
    check(card.toolTip().contains(QStringLiteral("/general.bat")),
          "and still names the file the row stands for");

    // Empty (and whitespace-only) clear it, and re-reporting is idempotent.
    card.setError(QStringLiteral("   "));
    check(!card.hasError(), "a whitespace-only message means no failure");
    check(card.property("error").toBool() == false, "and clears the error property");
    card.setError(reason);
    card.setError(reason);
    check(card.errorMessage() == reason, "re-reporting the same failure is idempotent");

    // A long backend message is never truncated in the model, and the tooltip can show all of it.
    const QString longReason = QStringLiteral(
        "WindowsBackend: not elevated - relaunch the application as administrator to load the "
        "WinDivert driver and then start this strategy again");
    card.setError(longReason);
    check(card.errorMessage() == longReason, "the model keeps the whole message");
    check(card.toolTip().contains(longReason), "the tooltip can show all of it");
    card.setError(QString());
    check(!card.hasError(), "an empty message clears the failure");

    // Retranslating and re-theming must not crash or drop state - both walk the chips and the menu.
    card.retranslateText();
    card.retheme();
    check(card.nameLabel()->text() == QLatin1String("general"), "retranslate kept the name");

    // ---- the chip text inherits the theme's proportional face ----
    // A chip can spell a word ("таймаут"), not only digits, so unlike the dashboard's number it is
    // deliberately NOT pinned to a fixed-pitch face: the generic `QWidget { font-family }` rule must
    // win here. The pinning machinery stays proven by the dashboard check above; this one proves the
    // chip opts out of it and inherits the proportional UI face.
    QLabel *chipVal = card.findChild<QLabel *>(QStringLiteral("miniPingValue"));
    if (chipVal) chipVal->ensurePolished();
    const QFontMetrics fmChip(chipVal ? chipVal->font() : QFont());
    std::printf("  info  miniPingValue family=\"%s\" i=%d W=%d l=%d M=%d\n",
                chipVal ? qUtf8Printable(chipVal->font().family()) : "(null)",
                fmChip.horizontalAdvance(QLatin1Char('i')),
                fmChip.horizontalAdvance(QLatin1Char('W')),
                fmChip.horizontalAdvance(QLatin1Char('l')),
                fmChip.horizontalAdvance(QLatin1Char('M')));
    check(chipVal != nullptr, "the chip carries a #miniPingValue figure");
    // Same discriminator gate as the pin's own check: a host whose UI fallback is a fixed face renders
    // the chip identically whether or not the sheet's generic `QWidget { font-family }` rule won, so
    // the "inherits proportional" claim has no way to be measured here. Skip, don't fail.
    if (uiSampleIsProportional()) {
        check(!resolvesMonospace(chipVal), "and the chip text inherits the proportional UI face");
    } else {
        std::printf("  SKIP  UI face is fixed-pitch on this host; chip inheritance is unmeasurable\n");
    }

    // ---- the row height FOLLOWS the font instead of clipping it ----
    // It used to be a literal 52 px. Now it is max(52 px floor, layout size hint, name line + padding),
    // so a larger theme/HiDPI text size grows the row rather than cutting the name's descenders or the
    // ring. The floor only sets the shipped look on a host whose resolved 11pt UI face is short enough
    // for line+padding to sit under it — on a runner whose sans-serif fallback is DejaVu Sans, 11pt
    // gives ~24 px line height, 24+32=56 already exceeds the floor, and the row's natural base is that
    // 56, not 52. Assert relative (base grows when the font grows, base returns when it is cleared)
    // rather than pinning a pixel the environment chooses; print the actual values so a future drift
    // in either direction is legible in the ctest --output-on-failure log without a re-run.
    const int baseRowHeight = card.height();
    const int baseNameSizeHint = card.nameLabel()->sizeHint().height();
    const int baseNameLineHeight = QFontMetrics(card.nameLabel()->font()).height();
    std::printf("  info  row base=%d, name sizeHint=%d, name line=%d, family=\"%s\"\n",
                baseRowHeight, baseNameSizeHint, baseNameLineHeight,
                qUtf8Printable(card.nameLabel()->font().family()));
    check(baseRowHeight >= 52, "the row keeps the design's 52 px floor at the base font");
    check(baseRowHeight >= baseNameSizeHint,
          "the row never clips the name's own height");

    card.nameLabel()->setStyleSheet(QStringLiteral("font-size: 42pt;"));   // a big-font stand-in
    card.nameLabel()->ensurePolished();
    card.setHosts(hosts);                                                   // re-runs the height maths
    const int grownRowHeight = card.height();
    const int grownNameSizeHint = card.nameLabel()->sizeHint().height();
    std::printf("  info  grown row=%d, name sizeHint=%d\n", grownRowHeight, grownNameSizeHint);
    check(grownRowHeight > baseRowHeight,
          "a larger name font grows the row past its base height");
    check(grownRowHeight >= grownNameSizeHint,
          "...and still leaves the larger name fully inside the row");

    card.nameLabel()->setStyleSheet(QString());                            // back to the sheet's 11pt
    card.nameLabel()->ensurePolished();
    card.setHosts(hosts);
    std::printf("  info  restored row=%d (expected %d)\n", card.height(), baseRowHeight);
    check(card.height() == baseRowHeight,
          "clearing the override returns the row to its base height");
}

// ------------------------------------------------------ tray + policy glue ----
static void testTrayPolicy()
{
    std::printf("\n[tray policy]\n");

    // Whether closing hides or quits is a pure predicate, so the whole truth table is checkable here
    // even though the window itself needs a real desktop to construct.
    check(MainWindow::hideOnClose(true, true), "minimize-to-tray with a tray host hides the window");
    check(!MainWindow::hideOnClose(true, false), "minimize-to-tray with no tray host quits instead");
    check(!MainWindow::hideOnClose(false, true), "an unticked setting never hides");
    check(!MainWindow::hideOnClose(false, false), "nor does it hide when both are off");

    // The predicate must follow the platform's own answer rather than a guessed list of desktops.
    check(MainWindow::traySupported() == QSystemTrayIcon::isSystemTrayAvailable(),
          "traySupported mirrors QSystemTrayIcon::isSystemTrayAvailable");

    // The settings page explains a tray this desktop cannot provide instead of leaving a ticked box that
    // silently does nothing — and, unlike the old modal, it commits from its own Save button and says so.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary settings store created");
    Settings settings(dir.filePath(QStringLiteral("muffin-test.ini")));
    settings.setMinimizeToTray(true);

    SettingsPage page(settings);
    QLabel *note = page.trayNote();
    check(note != nullptr, "the page carries a tray note");
    check(note && note->objectName() == QLatin1String("trayNote"), "the note is styled by objectName");
    // Hidden exactly when there is a tray to speak of — isHidden() is the widget's own flag, and the
    // page is not shown here, so it is the right thing to compare against.
    check(note && note->isHidden() == MainWindow::traySupported(),
          "the note is hidden exactly when this desktop has a tray host");
    check(note && !note->text().isEmpty(), "and the note says something when it is shown");
    check(!page.trayNote()->text().contains(QStringLiteral("%1")),
          "the note has no unsubstituted placeholder");

    // ---- the page is a commit, not a modal ----
    // A page has no frame to dismiss and no OK/Cancel pair, so the two ways it could quietly regress are
    // "Save does nothing" and "Save never says it saved". Both are pinned here, against the model the
    // page is meant to write through.
    QPushButton *save = page.saveButton();
    check(save != nullptr, "the page carries its own save button");
    check(save && save->objectName() == QLatin1String("primaryButton"), "and it is the primary action");

    const QString path = dir.filePath(QStringLiteral("zapret/linux"));
    page.setZapretPath(path);
    QSignalSpy savedSpy(&page, &SettingsPage::saved);
    save->click();
    check(savedSpy.count() == 1, "Save fires saved() once");
    check(settings.zapretPath() == path, "Save wrote the path through the model");
    // The tray box was loaded checked (the model said minimize-to-tray) and the commit wrote every field
    // back, so a round-trip through the page must leave the flag set — not silently drop it.
    check(settings.minimizeToTray(), "the page's commit preserved the tray setting it was loaded with");

    // The acknowledgement appears and is worded; the reload guard means committing did not drop it by
    // re-reading the form underneath the write.
    QLabel *ack = page.savedNote();
    check(ack != nullptr && !ack->text().isEmpty(), "Save put an acknowledgement beside the button");
    check(ack && ack->text() == LanguageManager::tr(QStringLiteral("settings.saved")),
          "and it is the catalog's 'saved' wording, not the key");

    // ---- the two short fields ----
    // A page that lets a form column stretch every field gives a three-word choice the same presence as
    // a path, which is how the settings sheet starts reading as a form to fill rather than as switches to
    // flip. The cap is one half of the fix; the row that ends in a stretch is the other, and both are
    // things a later layout edit can undo without a visible clue in the code.
    QComboBox *theme = page.themeField();
    QSpinBox *interval = page.pingIntervalField();
    check(theme && theme->maximumWidth() == 200, "the theme picker is one short value wide");
    check(interval && interval->maximumWidth() == 200, "and so is the ping interval");
    check(interval && interval->buttonSymbols() == QAbstractSpinBox::NoButtons,
          "the interval box shows no stepper arrows");
    check(theme && fieldEndsInStretch(theme), "the theme picker is held to the left of its row");
    check(interval && fieldEndsInStretch(interval), "and so is the interval box");

    // ---- the watchdog's cadence ----
    // The page is the only place the number is chosen, so the range it offers has to be the range the
    // model stores and the window arms its timer from — one spelling, read from Settings rather than
    // repeated as a second pair of literals here. Shaped like the box above on purpose.
    QSpinBox *watchdog = page.watchdogIntervalField();
    check(watchdog != nullptr, "the page carries a watchdog interval field");
    check(watchdog && watchdog->maximumWidth() == 200, "it is one short value wide like the others");
    check(watchdog && watchdog->buttonSymbols() == QAbstractSpinBox::NoButtons,
          "and it shows no stepper arrows either");
    check(watchdog && watchdog->minimum() == Settings::kWatchdogMinIntervalMin
                  && watchdog->maximum() == Settings::kWatchdogMaxIntervalMin,
          "its range is the model's range, 1 to 60 minutes");
    check(watchdog && fieldEndsInStretch(watchdog), "and it is held to the left of its row");
    check(watchdog && !watchdog->suffix().isEmpty(), "the unit is spelled on the field");
    check(watchdog && watchdog->property("i18nSuffix").toString()
                          == QLatin1String("settings.watchdog_suffix"),
          "tagged for retranslation, so the unit moves with the language");
    // Loaded from the model, not left on the widget's own default: the store said 5 minutes.
    check(watchdog && watchdog->value() == settings.watchdogIntervalMin(),
          "the field opens on the stored interval");
    // And committed back through Save, the only way this page writes anything.
    if (watchdog)
        watchdog->setValue(30);
    save->click();
    check(settings.watchdogIntervalMin() == 30, "Save wrote the new interval through the model");

    // A write that came from somewhere else (a download, the startup auto-location) re-reads the page
    // rather than leaving it showing a stale path.
    const QString other = dir.filePath(QStringLiteral("zapret/windows"));
    settings.setZapretPath(other);   // fires changed(); the page is not mid-commit, so it reloads
    check(page.zapretPath() == other, "an external change re-reads into the field");
}

// --------------------------------------------- settings page: cards, path row ----
//
// The redesign's promise is a shape, so that is what is checked: three titled cards, the settings grouped
// into them, one text field with a glyph inside it, two 32 px icons beside it, and no wide text buttons
// left. Behaviours (Save, the tray note, the ranges) belong to the section above; this one is about what
// the page looks like and where each row lives, because those are the two things a "make it prettier"
// commit can get wrong while every behaviour still passes.
static void testSettingsPageCards(QApplication &app)
{
    std::printf("\n[SettingsPage: cards and the path row]\n");

    QTemporaryDir dir;
    check(dir.isValid(), "temporary dir created");
    Settings settings(dir.filePath(QStringLiteral("muffin-cards.ini")));
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    QDir().mkpath(root + QStringLiteral("/init.d/sysv"));
    QFile marker(root + QStringLiteral("/init.d/sysv/zapret"));
    check(marker.open(QIODevice::WriteOnly), "planted a zapret marker so the path validates");
    marker.close();
    settings.setZapretPath(root);
    settings.setLanguage(QStringLiteral("ru"));

    // Layout is only computed for a widget that has been shown, and every assertion below about position
    // (which card, which column, which centre line) is a layout fact.
    Theme::apply(app, true);
    SettingsPage page(settings);
    page.resize(830, 560);
    page.show();
    QTest::qWait(30);

    // ---- the cards ----
    QFrame *general = page.generalCard();
    QFrame *ui = page.interfaceCard();
    QFrame *network = page.networkCard();
    check(general && ui && network, "the page is built as three cards");
    const auto cards = page.findChildren<QFrame *>(QStringLiteral("settingsCard"));
    check(cards.size() == 3, "…and exactly three frames answer to the card's object name");
    check(general && ui && network && cards.indexOf(general) == 0 && cards.indexOf(ui) == 1
                  && cards.indexOf(network) == 2,
          "…in the order the page stacks them: general, interface, network");

    const struct { QFrame *card; const char *key; } headings[] = {
        { general, "settings.group.general" },
        { ui, "settings.group.interface" },
        { network, "settings.group.network" },
    };
    for (const auto &h : headings) {
        QLabel *title = h.card ? h.card->findChild<QLabel *>(QStringLiteral("settingsCardTitle"))
                               : nullptr;
        check(title && title->text() == LanguageManager::tr(QString::fromLatin1(h.key)),
              QStringLiteral("each card carries its own heading (%1)")
                  .arg(QString::fromLatin1(h.key)));
    }

    // ---- the grouping ----
    check(settingsCardOf(page.pathField()) == general, "the zapret path is a general setting");
    check(settingsCardOf(page.trayNote()) == general,
          "…and so are the switches beside it, which are the program's own behaviour");
    check(settingsCardOf(page.themeField()) == ui, "the theme is an interface setting");
    check(settingsCardOf(page.languageField()) == ui, "…and so is the language");
    check(settingsCardOf(page.animationsField()) == ui, "…and so is the motion switch");
    // The single-window switch, and the promise it makes on THIS host. The row used to be hidden on Linux
    // on the belief that its package ships a daemon, a CLI and no window; it ships warp-taskbar, whose
    // window a connect maps, and masking that unit is what keeps the promise — so hiding the row there
    // was hiding the only control that owns the behaviour. Compared against the answer the core gives
    // rather than against a platform literal, so the page and the policy cannot drift apart.
    check(page.hideWarpField() != nullptr, "the page carries the single-window switch");
    check(page.hideWarpField()
                  && page.hideWarpField()->isHidden()
                         != WarpManager::platformHasWarpGui(ZapretSource::hostPlatform()),
          "…shown on a host that has a WARP window to keep away, and hidden only where there is none");
    check(settingsCardOf(page.pingIntervalField()) == network, "the ping interval is a network setting");
    check(settingsCardOf(page.watchdogIntervalField()) == network,
          "…and so is the interval a live profile is re-asked on");
    check(settingsCardOf(page.saveButton()) == nullptr,
          "the commit row belongs to no card — it closes the page, it is not one of its groups");

    // ---- one field column, three cards ----
    // The reason the row labels are pinned to one width: a QGridLayout takes its first column from the
    // widest cell in it, so three grids left to themselves would start their fields three pixels apart.
    const auto rowLabels = page.findChildren<QLabel *>(QStringLiteral("settingsRowLabel"));
    // Five labelled rows: the path, the theme, the language, and the two intervals. The switches carry
    // their own text and are not counted here — they are the reason the form has an empty label column.
    check(rowLabels.size() == 5, "every labelled row carries a label widget of its own");
    bool allPinned = !rowLabels.isEmpty();
    for (QLabel *label : rowLabels)
        if (label->minimumWidth() != SettingsPage::rowLabelWidth()
            || label->maximumWidth() != SettingsPage::rowLabelWidth())
            allPinned = false;
    check(allPinned, "every row label is pinned to one width, so the cards share a field edge");
    if (!rowLabels.isEmpty())
        check(rowLabels.first()->wordWrap(),
              "…and a long label wraps rather than breaking the column it is pinned to");

    // The vertical half of that promise is made by the LAYOUT, not only by the pixels measured further
    // down: every label is placed in the grid's first column with AlignLeft|AlignVCenter, and that pair is
    // what tells the layout to centre a short word on the line of a taller field. Read from each card's own
    // grid, so all three are covered rather than the one a screenshot happened to show.
    for (QFrame *card : {general, ui, network}) {
        const QString which = card == general ? QStringLiteral("general")
                              : card == ui    ? QStringLiteral("interface")
                                              : QStringLiteral("network");
        auto *grid = card ? card->findChild<QGridLayout *>() : nullptr;
        check(grid != nullptr,
              QStringLiteral("the %1 card keeps its rows in a QGridLayout").arg(which));
        check(grid && grid->columnCount() == 2 && grid->itemAtPosition(0, 1) != nullptr,
              QStringLiteral("…whose rows are the label/field pair, not a single stretched cell (%1)")
                  .arg(which));
        bool labelsCentred = grid != nullptr;
        int labelledRows = 0;
        if (grid) {
            for (int row = 0; row < grid->rowCount(); ++row) {
                QLayoutItem *item = grid->itemAtPosition(row, 0);
                if (!item || !qobject_cast<QLabel *>(item->widget()))
                    continue;
                ++labelledRows;
                if (!item->alignment().testFlag(Qt::AlignLeft)
                    || !item->alignment().testFlag(Qt::AlignVCenter))
                    labelsCentred = false;
            }
        }
        check(labelsCentred && labelledRows > 0,
              QStringLiteral("…and every label cell asks for left-aligned vertical centring (%1 row(s))")
                  .arg(labelledRows));
    }
    // And the label itself has to cooperate: it takes the row's full height and centres its own text
    // inside it. A label left at its sizeHint is exactly the bug this pair exists to prevent — the grid's
    // alignment only governs the cell, and a sizeHint-height label is still a line of text at the top of
    // whatever room the cell gives it.
    for (QLabel *label : rowLabels)
        check(label->alignment() == (Qt::AlignLeft | Qt::AlignVCenter)
                  && label->sizePolicy().verticalPolicy() == QSizePolicy::Expanding,
              QStringLiteral("row label \"%1\" fills its row and centres its text in it")
                  .arg(label->text()));

    const int fieldX = page.themeField()->mapTo(&page, QPoint(0, 0)).x();
    check(page.pingIntervalField()->mapTo(&page, QPoint(0, 0)).x() == fieldX,
          "the interface and the network cards start their fields at the same pixel");
    check(page.pathField()->mapTo(&page, QPoint(0, 0)).x() == fieldX,
          "…and so does the path field in the first one");

    // ---- one centre line per row ----
    //
    // The report was that "Тема" and "Язык" had slid. Horizontally the two columns already agree — that is
    // what the checks above measure — so what had moved is the vertical relation between a label and the
    // thing it names: a form-shaped layout sizes a row from its field and leaves a shorter label at the
    // row's top, which sits a 28 px word two pixels above a 32 px combo and four above a 35 px spin box.
    // Two pixels are invisible as a measurement and unmistakable as a page: the labels stop sharing a line
    // with the controls they belong to, and each row's offset is its own. The grid places both halves with
    // an explicit AlignVCenter; this reads the result as geometry, in the page's own coordinates.
    {
        auto labelFor = [&page](const QString &key) -> QLabel * {
            const QString text = LanguageManager::tr(key);
            const auto labels = page.findChildren<QLabel *>(QStringLiteral("settingsRowLabel"));
            for (QLabel *l : labels)
                if (l->text() == text)
                    return l;
            return nullptr;
        };
        const struct { const char *key; QWidget *field; } rows[] = {
            { "settings.theme", page.themeField() },
            { "settings.language", page.languageField() },
            { "settings.ping", page.pingIntervalField() },
            { "settings.watchdog", page.watchdogIntervalField() },
            { "settings.path", page.pathField() },
        };
        for (const auto &r : rows) {
            QLabel *label = labelFor(QLatin1String(r.key));
            check(label != nullptr && r.field != nullptr,
                  QStringLiteral("the %1 row has both halves to measure").arg(QLatin1String(r.key)));
            if (!label || !r.field)
                continue;
            const int labelCentre = label->mapTo(&page, QPoint(0, 0)).y() + label->height() / 2;
            const int fieldCentre = r.field->mapTo(&page, QPoint(0, 0)).y() + r.field->height() / 2;
            check(qAbs(labelCentre - fieldCentre) <= 1,
                  QStringLiteral("%1's label shares its field's centre line (%2 vs %3)")
                      .arg(QLatin1String(r.key))
                      .arg(labelCentre)
                      .arg(fieldCentre));
            check(label->mapTo(&page, QPoint(0, 0)).x()
                          == page.findChildren<QLabel *>(QStringLiteral("settingsRowLabel")).first()
                                 ->mapTo(&page, QPoint(0, 0)).x(),
                  QStringLiteral("…and starts at the same left edge as every other row label (%1)")
                      .arg(QLatin1String(r.key)));
        }
    }

    // ---- the path row ----
    QLineEdit *field = page.pathField();
    check(field != nullptr, "the path row is one text field");
    // Asked of the card, and of its direct children only: a QSpinBox owns a QLineEdit of its own, so a
    // page-wide search counts three edits on a page with one. The promise is about the row, not about Qt.
    const auto rowEdits = general ? general->findChildren<QLineEdit *>(QString(),
                                                                    Qt::FindDirectChildrenOnly)
                                  : QList<QLineEdit *>();
    check(rowEdits.size() == 1 && !rowEdits.isEmpty() && rowEdits.first() == field,
          "…and the only field the row holds — one box for one value");

    QAction *browse = page.browseAction();
    check(browse != nullptr, "the field carries a folder glyph of its own");
    check(field && browse && field->actions().contains(browse),
          "…as an action of the field itself, so it is drawn inside the box rather than beside it");
    // Which end of the box, asked of the widgets the field lays out for its actions — the position is the
    // field's business and not something the action carries.
    const auto inFieldButtons = field ? field->findChildren<QToolButton *>()
                                      : QList<QToolButton *>();
    bool glyphOnTheRight = false;
    for (QToolButton *b : inFieldButtons)
        if (!b->icon().isNull() && b->geometry().x() > field->width() / 2)
            glyphOnTheRight = true;
    check(glyphOnTheRight, "…and on the right, where the field puts the things that act on it");
    check(browse && !browse->icon().isNull(), "…as an icon, not as a word");
    check(browse && !browse->toolTip().isEmpty() && !browse->toolTip().contains(QLatin1String("%1")),
          "…explained by a tooltip with no unfilled placeholder in it");
    check(browse && !browse->text().isEmpty(), "…and named, for anything reading the screen");

    QToolButton *detect = page.autoDetectButton();
    check(detect && detect->size() == QSize(32, 32), "auto-detect is a 32x32 icon button");
    check(detect && detect->objectName() == QLatin1String("pathToolButton"),
          "…styled by the sheet's one rule for a field's icon actions");
    check(detect && !detect->icon().isNull() && detect->text().isEmpty(),
          "the row's one appendage is a glyph and nothing else");
    check(detect && !detect->toolTip().isEmpty() && !detect->accessibleName().isEmpty(),
          "a wordless button still says what it does, to the pointer and to the accessibility tree");

    // Fetching zapret is no longer one of this row's actions. A field about a path cannot answer the
    // question the icon existed for — "I have no zapret at all, how do I get it?" — which is asked one
    // page away, where an empty folder is visible as an empty list. Asserted by what the buttons are
    // NAMED, so a control that had merely moved within the page would still be caught here.
    const QString fetchName = LanguageManager::tr(QStringLiteral("action.download_zapret"));
    int fetchActions = 0;
    for (QToolButton *b : page.findChildren<QToolButton *>())
        if (b->accessibleName() == fetchName)
            ++fetchActions;
    check(fetchActions == 0,
          "nothing on the settings page offers to download zapret any more");
    check(detect && field && detect->parentWidget() == field->parentWidget(),
          "field and its one icon are still one row, not a row each");

    if (general && field && detect) {
        // The row's own geometry, in the card's coordinates — every widget in it is a direct child of the
        // card, so y() and x() compare without mapping. The numbers travel with the verdict, because a
        // check that only says "not centred" cannot be read from a CI log.
        const int fieldMid = field->y() + field->height() / 2;
        const int detectMid = detect->y() + detect->height() / 2;
        // One pixel of slack, and the reason is arithmetic rather than indulgence: an even control centred
        // in an odd row lands half a pixel out, and which way that rounds is the layout's business.
        check(qAbs(detectMid - fieldMid) <= 1,
              QStringLiteral("…centred on the field's line, so the row reads as one control with one "
                             "appendage (field y=%1 h=%2 mid=%3 | search mid=%4)")
                  .arg(field->y()).arg(field->height()).arg(fieldMid).arg(detectMid));
        check(detect->x() >= field->x() + field->width(),
              QStringLiteral("…and to the right of the field, where the field puts the things that act on "
                             "it (field ends %1, search at %2)")
                  .arg(field->x() + field->width()).arg(detect->x()));
    }

    // What the redesign removed. Three wide text buttons used to share this row's width with the field,
    // which is how one setting came to be the loudest line on the page.
    const auto textButtons = page.findChildren<QPushButton *>();
    check(textButtons.size() == 1 && !textButtons.isEmpty()
                  && textButtons.first() == page.saveButton(),
          "the only text button left on the page is Save");

    // ---- the language picker ----
    QComboBox *lang = page.languageField();
    check(lang != nullptr, "the interface card offers a language of its own");
    check(lang && lang->count() == LanguageManager::codes().size(),
          "…with one entry per catalog language");
    check(lang && lang->currentData().toString() == settings.language(),
          "…and it opens on the stored code, not on the widget's first row");
    check(lang && lang->itemText(0) == LanguageManager::nativeName(LanguageManager::codes().first()),
          "entries name themselves in their own script — a language is never translated");
    check(lang && lang->maximumWidth() == 200,
          "…and it is held to the same narrow shape as the theme picker");
    check(lang && lang->property("i18nItems").toStringList().isEmpty(),
          "…which is why it is the one combo on the page NOT tagged for retranslation");

    if (lang)
        lang->setCurrentIndex(lang->findData(QStringLiteral("de")));
    page.saveButton()->click();
    check(settings.language() == QLatin1String("de"), "Save stores the language that was picked");
    check(LanguageManager::instance().currentCode() == QLatin1String("de"),
          "…and moves the catalog with it, so the change is on screen and not only in the store");
    QLabel *generalTitle = general
        ? general->findChild<QLabel *>(QStringLiteral("settingsCardTitle")) : nullptr;
    check(generalTitle && generalTitle->text()
                              == LanguageManager::translate(QStringLiteral("de"),
                                                            QStringLiteral("settings.group.general")),
          "the card headings are re-read, so the new grouping does not speak the old language");
    check(browse && browse->toolTip()
                          == LanguageManager::translate(QStringLiteral("de"),
                                                        QStringLiteral("settings.browse_tip")),
          "…and so is the folder glyph's tooltip, which is all an icon-only control ever says");
    check(field && field->placeholderText().contains(
                       LanguageManager::translate(QStringLiteral("de"),
                                                 QStringLiteral("common.or"))),
          "…even the placeholder, which is composed of three texts rather than one catalog key");
    check(!page.zapretPath().isEmpty(), "a language change does not lose the value in the field");

    // The catalog is shared state: the rest of the binary, and the application itself, open in Russian.
    if (lang)
        lang->setCurrentIndex(lang->findData(QStringLiteral("ru")));
    page.saveButton()->click();
    check(LanguageManager::instance().currentCode() == QLatin1String("ru"),
          "and the section puts the language back the way it found it");

    // ---- the render ----
    // A card that is not styled is a rectangle of the page's own colour with a border nobody asked for,
    // and every assertion above would still pass on it. So the pixels are read too: the sheet's own
    // declaration for a card must be what the render paints inside the card, and something else in the
    // gap between two of them — which is the whole difference between a card and a line of text.
    const QString sheet = app.styleSheet();
    const QColor pageBg = sheetRuleColor(sheet, QStringLiteral("QWidget#settingsPage"),
                                         QStringLiteral("background-color"));
    const QColor cardBg = sheetRuleColor(sheet, QStringLiteral("QFrame#settingsCard"),
                                         QStringLiteral("background-color"));
    check(pageBg.isValid() && cardBg.isValid(), "the sheet says what the page and its cards paint");
    check(pageBg != cardBg, "a card is a different surface from the page it floats on");
    const QImage img = page.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    // Sampled from the card's OWN image, not from a coordinate inside the page: the page grab depends on
    // the scroll position, the page margins and a layout that has settled, none of which have anything to
    // do with the question "does a card paint its surface colour". The card's own pixmap asks exactly that.
    const QImage cardImg = general ? general->grab().toImage().convertToFormat(QImage::Format_ARGB32)
                                   : QImage();
    const QPoint padSpot(20, 6);   // inside the card's padding, above its first row
    check(cardImg.rect().contains(padSpot) && rgbClose(cardImg.pixelColor(padSpot), cardBg, 8),
          QStringLiteral("the card paints the sheet's surface where it is empty (%1 at (%2,%3), wanted %4)")
              .arg(cardImg.rect().contains(padSpot) ? cardImg.pixelColor(padSpot).name(QColor::HexRgb)
                                                   : QStringLiteral("<off-image>"))
              .arg(padSpot.x()).arg(padSpot.y())
              .arg(cardBg.isValid() ? cardBg.name(QColor::HexRgb) : QStringLiteral("<none>")));
    check(rgbClose(img.pixelColor(QPoint(4, 4)), pageBg, 8),
          "…and the page behind the cards paints its own, so the gap between cards is a gap");

    // The stroke is the other half of a card, and a stylesheet border is not decoration drawn OVER the
    // widget: it moves the contents in by its own width. So the leftmost pixel of the card's own column
    // must be the sheet's border colour, and a few pixels further in must be the card's own fill.
    const QColor stroke = sheetRuleHex(sheet, QStringLiteral("QFrame#settingsCard"),
                                       QStringLiteral("border"));
    const QPoint edge(0, cardImg.isNull() ? -1 : cardImg.height() / 2);
    check(stroke.isValid() && cardImg.rect().contains(edge)
                  && rgbClose(cardImg.pixelColor(edge), stroke, 24),
          QStringLiteral("…and the card is stroked: %1 at its left edge, wanted %2")
              .arg(cardImg.rect().contains(edge) ? cardImg.pixelColor(edge).name(QColor::HexRgb)
                                                 : QStringLiteral("<off-image>"))
              .arg(stroke.isValid() ? stroke.name(QColor::HexRgb) : QStringLiteral("<none>")));
    // Four pixels in rather than one: the sheet asks for a 1 px stroke, but a stroke is drawn in device
    // pixels and a scaled environment scales it, so sampling the pixel right next to the edge would be
    // reading the border and calling it a missing fill. Four is still far inside the card's padding and
    // far from its first row.
    const QPoint fillSpot(4, cardImg.isNull() ? -1 : cardImg.height() / 2);
    check(cardImg.rect().contains(fillSpot) && rgbClose(cardImg.pixelColor(fillSpot), cardBg, 8),
          "…with the card's fill taking over just inside that stroke");

    // Nothing may sit on top of anything else inside a card. Every row of a card is a direct child of it,
    // so their geometries are in one coordinate system and an overlap is a layout that stopped laying
    // out — which is exactly what a nested-layout mistake looks like on screen.
    int overlaps = 0;
    for (const QFrame *c : {general, ui, network}) {
        if (!c)
            continue;
        const auto pieces = c->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
        for (int i = 0; i < pieces.size(); ++i) {
            for (int j = i + 1; j < pieces.size(); ++j) {
                const QRect a = pieces.at(i)->geometry();
                const QRect b = pieces.at(j)->geometry();
                if (pieces.at(i)->isVisible() && pieces.at(j)->isVisible() && a.intersects(b))
                    ++overlaps;
            }
        }
    }
    check(overlaps == 0,
          QStringLiteral("no two widgets in a card occupy the same pixels (%1 collisions)").arg(overlaps));

    // And a card is only a card if it is taller than its own rows: the heading must be above the first
    // field, and the last row must end inside the frame rather than on its stroke.
    if (general && field) {
        const QRect titleOf = general->findChild<QLabel *>(QStringLiteral("settingsCardTitle"))
                                  ->geometry();
        check(titleOf.top() < field->geometry().top(), "the heading sits above the card's first field");
        check(field->geometry().bottom() < general->height() - 1,
              "…and every row finishes inside the card, not under its border");
    }

    // The three glyphs are baked pixmaps, so a theme switch cannot recolour them: the window re-bakes
    // every widget (and the field's action) that carries the name and the size it was drawn at. Without
    // these properties a light sheet would keep a dark-sheet glyph in its path field.
    check(browse && browse->property("glyph").toString() == QLatin1String("folder")
                  && browse->property("glyphPx").toInt() == 16,
          "the folder glyph declares the name and size applyTheme() must re-bake it at");
    check(detect && detect->property("glyph").toString() == QLatin1String("search"),
          "auto-detect declares its magnifier");
    // The cloud it used to be paired with is not this page's any more; its glyph declaration is checked
    // with the profiles page's own corner, where the button now lives.

    // ---- the light sheet ----
    // A card whose surface equals its page's is not a card, and the dark sheet is the only one where that
    // mistake is invisible — on white canvas a missing rule shows as a flat page with hairlines on it.
    Theme::apply(app, false);
    QTest::qWait(30);
    const QColor lightPage = sheetRuleColor(app.styleSheet(), QStringLiteral("QWidget#settingsPage"),
                                            QStringLiteral("background-color"));
    const QColor lightCard = sheetRuleColor(app.styleSheet(), QStringLiteral("QFrame#settingsCard"),
                                            QStringLiteral("background-color"));
    check(lightPage.isValid() && lightCard.isValid() && lightPage != lightCard,
          "the light sheet gives its cards a surface of their own too");
    const QImage lightCardImg = general ? general->grab().toImage().convertToFormat(QImage::Format_ARGB32)
                                        : QImage();
    check(lightCardImg.rect().contains(padSpot)
                  && rgbClose(lightCardImg.pixelColor(padSpot), lightCard, 8),
          QStringLiteral("…and paints it (%1 at (%2,%3), wanted %4)")
              .arg(lightCardImg.rect().contains(padSpot)
                       ? lightCardImg.pixelColor(padSpot).name(QColor::HexRgb)
                       : QStringLiteral("<off-image>"))
              .arg(padSpot.x()).arg(padSpot.y())
              .arg(lightCard.isValid() ? lightCard.name(QColor::HexRgb)
                                       : QStringLiteral("<none>")));
    check(rgbClose(page.grab().toImage().convertToFormat(QImage::Format_ARGB32).pixelColor(QPoint(4, 4)),
                   lightPage, 8),
          "…with the page still its own colour behind the cards");

    // Everything after this section — and the application itself — is drawn on the dark sheet.
    Theme::apply(app, true);
    QTest::qWait(30);
    page.hide();
}

// ---------------------------------------------------------- window policy ----
static void testWindowPolicy()
{
    std::printf("\n[window policy]\n");

    // The status chip's wording is a pure mapping, so the phases a portable run walks through — which
    // are the only ones a view can misread as "nothing is happening" — are checkable here.
    using S = IZapretBackend::State;
    check(MainWindow::statusText(S::Starting, QStringLiteral("nfqws (portable)"))
              .contains(QStringLiteral("nfqws (portable)")),
          "the firewall-up phase still names the strategy the user picked");
    check(MainWindow::statusText(S::Running, QStringLiteral("nfqws (portable)"))
              .contains(QStringLiteral("nfqws (portable)")),
          "and so does the live payload");
    check(!MainWindow::statusText(S::Stopping, QStringLiteral("nfqws (portable)")).isEmpty(),
          "the teardown phase has wording of its own");
    check(MainWindow::statusKind(S::Starting) == MainWindow::statusKind(S::Stopping),
          "in-flight phases share one chip colour: neither reads as idle");
    check(MainWindow::statusKind(S::Running) != MainWindow::statusKind(S::Stopped),
          "up and down are distinguishable");
    check(MainWindow::statusKind(S::Error) != MainWindow::statusKind(S::Stopped),
          "a failure is not shown as an ordinary stop");

    // The header's chip is the same mapping in one word. It has to distinguish all five states on its
    // own now: with the toolbar's pill gone it is the only place the state is written, and a chip that
    // said "OFFLINE" during a teardown would be the window lying about a live firewall.
    check(MainWindow::connectionText(S::Running) == QStringLiteral("CONNECTED"),
          "a live run is the connected state the mock draws");
    check(MainWindow::connectionText(S::Stopped) != MainWindow::connectionText(S::Stopping),
          "idle and tearing-down are different words");
    check(MainWindow::connectionText(S::Starting) != MainWindow::connectionText(S::Running),
          "coming up is not yet up");
    check(!MainWindow::connectionText(S::Error).isEmpty(), "a failure has wording");
    // The chip's word and its dot share one property value, so the pair cannot disagree.
    check(MainWindow::connectionText(S::Running) != MainWindow::connectionText(S::Error)
              && MainWindow::statusKind(S::Running) != MainWindow::statusKind(S::Error),
          "the two states that look most alike at 12 px differ in both word and colour");

    // ---- the action matrix ----
    // Every start/stop control in the window is derived from these two predicates, so the whole table IS
    // the feature. What was missing was one row: a Stopping state that left both the toolbar's
    // "Остановить" and every card's "Запустить" live for as long as the iptables teardown took, so the
    // second click ten seconds into a stop either re-issued terminate() or drew the backend's
    // "an operation is already in progress (stop first)" across a card that had done nothing wrong.
    check(MainWindow::canStartFrom(S::Stopped), "an idle backend accepts a start");
    check(MainWindow::canStartFrom(S::Error), "a failure can be retried");
    check(!MainWindow::canStartFrom(S::Starting), "a launch in flight accepts no other launch");
    check(!MainWindow::canStartFrom(S::Running), "one strategy at a time");
    check(!MainWindow::canStartFrom(S::Stopping),
          "a teardown accepts no start — the reported case");

    check(MainWindow::canStopFrom(S::Running), "a live run can be stopped");
    check(MainWindow::canStopFrom(S::Starting),
          "a stuck launch stays cancellable: the backend queues this stop instead of racing it");
    check(!MainWindow::canStopFrom(S::Stopping), "a stop already under way is not re-issued");
    check(!MainWindow::canStopFrom(S::Stopped), "nothing to stop when nothing ran");
    check(!MainWindow::canStopFrom(S::Error), "nor after a failure");

    for (const S s : {S::Stopped, S::Starting, S::Running, S::Stopping, S::Error}) {
        // A card holds a single button, so a state offering both actions could not be expressed at all.
        check(!MainWindow::canStartFrom(s) || !MainWindow::canStopFrom(s),
              "no state offers a start and a stop at the same time");
        // The sentence a gated control shows must exist exactly when the control is gated, or a greyed
        // button arrives with an empty tooltip and looks broken rather than busy.
        check(MainWindow::blockedReason(s).isEmpty() == MainWindow::canStartFrom(s),
              "a state that accepts a start has nothing to wait for, and one that refuses says why");
        // The chip is the only part of the window a user reads as "busy" — it must go amber over exactly
        // the states the buttons are gated by, or the wording and the controls disagree.
        check((MainWindow::statusKind(s) == QStringLiteral("starting"))
                  == (s == S::Starting || s == S::Stopping),
              "the busy chip covers precisely the two in-flight states");
    }

    check(MainWindow::statusText(S::Stopping, QStringLiteral("nfqws (portable)"))
              .contains(QStringLiteral("Остановка")),
          "the chip and the tray tooltip read Остановка… while the teardown runs");
    check(MainWindow::blockedReason(S::Stopping).contains(QStringLiteral("дождитесь")),
          "and the reason offered in its place tells the user to wait");
    check(MainWindow::blockedReason(S::Running).contains(QStringLiteral("остановите")),
          "while a run is up, the missing action is named as the stop");

    // ---- what a Stop press has to look like, before the backend can say so ----
    //
    // The teardown runs on the GUI thread and waits inside it, so the repaint stateChanged(Stopping)
    // would normally cause cannot land until the wait is over. The window therefore draws the stopping
    // look itself, ahead of the signal — and the look it draws has to be the same one the signal draws,
    // or a press shows a button the run then contradicts. These are the two halves of that: a look that
    // exists for a teardown, and one that differs from the live run it replaced.
    check(MainWindow::powerMode(S::Running) == PowerButton::Mode::Active,
          "a live run wears the green button whose press means stop");
    check(MainWindow::powerMode(S::Stopping) == PowerButton::Mode::Busy,
          "a teardown in flight is the busy look — the breathing ring the press can paint early");
    check(MainWindow::powerMode(S::Stopping) == MainWindow::powerMode(S::Starting),
          "both in-flight states draw one look, so the press and the signal cannot disagree");
    check(MainWindow::powerMode(S::Stopping) != MainWindow::powerMode(S::Running),
          "and pressing Stop visibly changes the button rather than leaving it as it was");
    for (const S s : {S::Stopped, S::Starting, S::Running, S::Stopping, S::Error}) {
        // One table for both controls: the chip goes busy over precisely the states the button does.
        check((MainWindow::powerMode(s) == PowerButton::Mode::Busy)
                  == (MainWindow::statusKind(s) == QStringLiteral("starting")),
              "the button's busy look and the chip's busy colour cover the same states");
    }
    check(MainWindow::powerMode(S::Stopped) == PowerButton::Mode::Idle
              && MainWindow::powerMode(S::Error) == PowerButton::Mode::Idle,
          "nothing running and nothing in flight is the plain idle button, failure included");

    // Which card a backend failure belongs to. A pending start owns the answer while the attempt is
    // unresolved; otherwise the live run does; otherwise the window's memory of the last live run —
    // which is exactly what a deferred portable error needs, because the manager has forgotten the
    // strategy by the time that error is announced (see the manager-level test in coretest).
    const QString pend = QStringLiteral("A");
    const QString act = QStringLiteral("B");
    const QString last = QStringLiteral("C");
    check(MainWindow::errorTargetId(pend, act, last) == pend,
          "the card that asked for this start owns its failure");
    check(MainWindow::errorTargetId(QString(), act, last) == act,
          "once the attempt resolved, the live run's card does");
    check(MainWindow::errorTargetId(QString(), QString(), last) == last,
          "with the run already forgotten, the remembered one is badged");
    check(MainWindow::errorTargetId(QString(), QString(), QString()).isEmpty(),
          "and a failure that belongs to no run badges nothing rather than something stale");
    check(MainWindow::errorTargetId(pend, QString(), QString()) == pend,
          "an empty fallback chain never outranks the pending start");

    // ---- which rows a ping verdict may reach ----
    // The rows are not a second copy of the dashboard: a cycle measures the path the running profile
    // opened, so an idle row must keep its last verdict rather than borrow somebody else's. The manual
    // round is the one press that measures every row at once, and the predicate is the whole rule.
    check(MainWindow::profilePingHosts().size() == 2,
          "a row carries a chip for the services it exists to unblock");
    check(MainWindow::profilePingHosts().contains(QLatin1String("youtube.com"))
              && MainWindow::profilePingHosts().contains(QLatin1String("discord.com")),
          "and those chips are the two the profile is used for");
    check(!MainWindow::profilePingHosts().contains(QLatin1String("google.com")),
          "the dashboard's baseline host is not a claim about a profile");
    check(MainWindow::profilePingHosts().size() < PingService::defaultHosts().size(),
          "the rows probe a subset of what the badges do, never more");

    check(!MainWindow::rowFollowsCycle(false, false),
          "an idle row does not follow the cycle of a run that is not its own");
    check(MainWindow::rowFollowsCycle(false, true),
          "the running profile's row follows every cycle");
    check(MainWindow::rowFollowsCycle(true, false),
          "a manual round reaches the rows that are not running");
    check(MainWindow::rowFollowsCycle(true, true),
          "and the one that is");

    // ---- the bypass check's verdict sheet ----
    //
    // The engine measures and the window words. What sits between them is this pure formatter, so the
    // two halves are run through each other here: a verdict goes out as the engine's own JSON (its real
    // resultToJson, not a hand-copy of the shape) and comes back as the line the user reads. That is
    // what catches a key renamed on one side only — a sheet that silently prints "FAIL (0 ms)" for
    // every host that actually passed.
    auto sheetFor = [](const QString &host, const HostCheckResult &r) {
        QJsonObject map;
        map.insert(host, BlockcheckEngine::resultToJson(r));
        return MainWindow::blockcheckResultText(map);
    };

    HostCheckResult pass;
    pass.success = true;
    pass.avgLatency = 45;
    pass.attemptsOk = 3;
    pass.attempts = 3;
    check(sheetFor(QStringLiteral("youtube.com"), pass)
              == QStringLiteral("Youtube: OK (45 ms)"),
          "a host that got through reads as OK with its latency");

    HostCheckResult dead;
    dead.success = false;
    dead.avgLatency = -1;
    dead.attempts = 3;
    dead.lastError = QStringLiteral("connect timed out after 3000 ms");
    check(sheetFor(QStringLiteral("discord.com"), dead) == QStringLiteral("Discord: FAIL"),
          "a host that did not is a bare FAIL with no latency invented for it");

    // The one line that means "intercepted", not "unreachable" — and the only case the sheet decorates.
    HostCheckResult intercepted = pass;
    intercepted.isCertificateValid = false;
    check(sheetFor(QStringLiteral("youtube.com"), intercepted)
              == QStringLiteral("Youtube: OK (45 ms) — %1")
                     .arg(LanguageManager::tr(QStringLiteral("blockcheck.cert_invalid"))),
          "an OK whose certificate was rejected says so on the same line");
    HostCheckResult deadUntrusted = dead;
    deadUntrusted.isCertificateValid = false;
    check(sheetFor(QStringLiteral("discord.com"), deadUntrusted)
              == QStringLiteral("Discord: FAIL"),
          "a host that never got through is not also blamed for its certificate");

    // An absent flag must read as the ordinary pass: the sheet is also handed maps written by older
    // builds and by muffin-cli, where the field simply is not there.
    QJsonObject legacy;
    legacy.insert(QStringLiteral("youtube.com"),
                  QJsonObject{{QStringLiteral("success"), true},
                              {QStringLiteral("latency"), 12}});
    check(MainWindow::blockcheckResultText(legacy) == QStringLiteral("Youtube: OK (12 ms)"),
          "a map without a certificate_valid key defaults to trusted, not to a warning");

    check(MainWindow::blockcheckResultText(QJsonObject()).isEmpty(),
          "no results is no lines — the empty map is the caller's no_results dialog instead");

    // Two hosts, inserted both ways round: QJsonObject owns the ordering, so the sheet a user reads
    // cannot depend on the order the probes happened to finish in.
    QJsonObject forward, backward;
    forward.insert(QStringLiteral("discord.com"), BlockcheckEngine::resultToJson(dead));
    forward.insert(QStringLiteral("youtube.com"), BlockcheckEngine::resultToJson(pass));
    backward.insert(QStringLiteral("youtube.com"), BlockcheckEngine::resultToJson(pass));
    backward.insert(QStringLiteral("discord.com"), BlockcheckEngine::resultToJson(dead));
    const QString two = MainWindow::blockcheckResultText(forward);
    check(two == MainWindow::blockcheckResultText(backward),
          "the sheet is the same text whatever order the results arrived in");
    check(two == QStringLiteral("Discord: FAIL\nYoutube: OK (45 ms)"),
          "one line per host, in the map's own sorted order");
}

// ------------------------------------------------------------ LanguageManager ----
//
// The catalog is compiled in rather than loaded, so these are the checks that keep it honest: that the
// seven languages exist in the order the picker shows them, that no row has a hole, that the fallback
// rules are the documented ones, that a language switch actually changes what new widgets say, and
// that the flag artwork the rail button is built from is really in the resources.
static void testLanguage()
{
    std::printf("\n[LanguageManager]\n");

    const QStringList codes = LanguageManager::codes();
    check(codes == QStringList({QStringLiteral("ru"), QStringLiteral("en"), QStringLiteral("es"),
                                QStringLiteral("de"), QStringLiteral("zh"), QStringLiteral("fr"),
                                QStringLiteral("hi")}),
          "seven languages, in menu order");
    check(LanguageManager::instance().currentCode() == QLatin1String("ru"),
          "the UI opens in Russian, the language every sheet was drawn in");

    const QStringList keys = LanguageManager::keys();
    check(keys.size() > 100, QStringLiteral("the catalog holds %1 keys").arg(keys.size()));
    check(keys.size() == QSet<QString>(keys.cbegin(), keys.cend()).size(),
          "no key is defined twice");

    int holes = 0;
    for (const QString &key : keys) {
        for (const QString &code : codes) {
            const QString text = LanguageManager::translate(code, key);
            if (text.isEmpty() || text == key)
                ++holes;
        }
    }
    check(holes == 0,
          QStringLiteral("every key has text in every language (%1 cells)").arg(keys.size() * codes.size()));

    // A hole is not the only way a translation can break: a format key that loses its %1 still
    // returns non-empty text, and the caller's .arg() then silently discards the word it was given.
    // The mode switch's journal line names the subsystem being stopped that way, so the placeholder
    // is pinned per language rather than trusted to the copy.
    for (const QString &code : codes)
        check(LanguageManager::translate(code, QStringLiteral("mode.switch_stopping"))
                  .contains(QStringLiteral("%1")),
              QStringLiteral("mode.switch_stopping keeps its placeholder in %1").arg(code));

    check(LanguageManager::translate(QStringLiteral("de"), QStringLiteral("nav.settings"))
              == QStringLiteral("Einstellungen"),
          "German is German");
    check(LanguageManager::translate(QStringLiteral("zh"), QStringLiteral("nav.logs"))
              == QStringLiteral("日志"), "Chinese is Chinese");
    check(LanguageManager::nativeName(QStringLiteral("hi")).contains(QStringLiteral("हि")),
          "a language names itself in its own script");

    // The documented fallback: an unknown key says the key, never nothing.
    check(LanguageManager::tr(QStringLiteral("no.such.key.at.all"))
              == QStringLiteral("no.such.key.at.all"),
          "a missing key falls back to its own name rather than to blank");
    // A format string with its argument in one call.
    check(LanguageManager::tr(QStringLiteral("status.running"), QStringLiteral("nfqws"))
              .contains(QStringLiteral("nfqws")),
          "tr(key, arg) fills %1");

    for (const QString &code : codes) {
        QIcon flag = LanguageManager::flagIcon(code);
        check(!flag.isNull(), QStringLiteral("flag icon present for %1").arg(code));
        check(!flag.pixmap(22, 16).isNull(), QStringLiteral("flag renders for %1").arg(code));
    }

    // Switching language: one signal, and nothing for a no-op or an unsupported code.
    QSignalSpy changed(&LanguageManager::instance(), &LanguageManager::languageChanged);
    LanguageManager::instance().setCode(QStringLiteral("qq"));
    check(LanguageManager::instance().currentCode() == QLatin1String("ru"),
          "an unknown code is refused, not applied");
    check(changed.count() == 0, "and refusing one emits nothing");
    LanguageManager::instance().setCode(QStringLiteral("en"));
    check(LanguageManager::instance().currentCode() == QLatin1String("en"), "en applied");
    check(changed.count() == 1, "one signal for one real change");
    LanguageManager::instance().setCode(QStringLiteral("en"));
    check(changed.count() == 1, "and none when nothing moved");

    // A widget built after the switch speaks the new language straight out of its keys, and the keys it
    // carries are the ones retranslateUi() walks — the property is the whole contract between the two.
    {
        LogPanel panel;
        check(panel.view() != nullptr, "the log panel still builds under English");
        const auto buttons = panel.findChildren<QPushButton *>();
        int tagged = 0;
        for (QPushButton *b : buttons) {
            const QString key = b->property("i18nKey").toString();
            if (key.isEmpty())
                continue;
            ++tagged;
            check(LanguageManager::keys().contains(key),
                  QStringLiteral("log button key '%1' is in the catalog").arg(key));
            check(b->text() == LanguageManager::tr(key),
                  QStringLiteral("log button '%1' says what its key says").arg(key));
        }
        check(tagged == 3, "copy, clear and save are all tagged for retranslation");
    }

    // The walk itself, on a widget that already exists: tag-and-rewrite is what makes a language switch
    // take effect without a restart, so it is checked on the log panel's own toolbar — buttons, the
    // filter combo's entries, and the tooltips — rather than on the window, whose tree is the watchdog
    // section's business and needs a store of its own.
    {
        LogPanel panel;
        LanguageManager::instance().setCode(QStringLiteral("de"));
        const int touched = retranslateTree(&panel);
        QPushButton *copy = nullptr;
        for (QPushButton *b : panel.findChildren<QPushButton *>())
            if (b->property("i18nKey").toString() == QLatin1String("log.copy"))
                copy = b;
        check(copy && copy->text() == QStringLiteral("Kopieren"),
              "retranslateTree re-says a button in the new language");
        check(copy && copy->toolTip() == QStringLiteral("Das gesamte Protokoll in die Zwischenablage kopieren"),
              "and its tooltip");
        QComboBox *filter = panel.findChild<QComboBox *>();
        check(filter && filter->itemText(0) == QStringLiteral("Alle")
                  && filter->itemText(3) == QStringLiteral("Fehler"),
              "combo entries follow, in index order");
        check(filter && filter->itemData(3).toInt() == LogPanel::Error,
              "and the value behind an entry is untouched by its label");
        check(touched >= 8, QStringLiteral("the walk rewrote %1 tagged strings").arg(touched));
        LanguageManager::instance().setCode(QStringLiteral("ru"));
        retranslateTree(&panel);
        check(copy->text() == QStringLiteral("Копировать"),
              "switching back re-says them again");
    }

    // The persisted choice: the default, and a value that survives the object that wrote it.
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("language-test.ini"));
    {
        Settings s(file);
        check(s.language() == QLatin1String("ru"), "an unset language is Russian");
        s.setLanguage(QStringLiteral("fr"));
    }
    {
        Settings s(file);
        check(s.language() == QLatin1String("fr"), "the chosen language is stored");
    }

    // Back to the language the rest of this suite reads the chrome in.
    LanguageManager::instance().setCode(QStringLiteral("ru"));
    check(LanguageManager::tr(QStringLiteral("nav.logs")) == QStringLiteral("Логи"),
          "restoring ru restores the Russian chrome");
}

// ------------------------------------------------------------------- Theme ----
static void testTheme(QApplication &app)
{
    std::printf("\n[Theme]\n");

    Theme::apply(app, true);
    check(Theme::isDark(), "dark mode is active");
    check(!app.styleSheet().isEmpty(), "dark stylesheet loaded from resources");
    check(app.styleSheet().contains(QStringLiteral("#C084FC")), "accent colour present in the sheet");
    check(app.styleSheet().contains(QStringLiteral("#0B0C10")), "dashboard background present in the sheet");
    check(app.styleSheet().contains(QStringLiteral("profileCard")), "profile row rules present");
    check(app.styleSheet().contains(QStringLiteral("settingsPage")), "settings page rules present");
    // The history page is sheet-styled apart from its row tints: the panel, the headline, the table and
    // the rate bar all need rules, or the page renders as loose labels on a black canvas.
    check(app.styleSheet().contains(QStringLiteral("historySummary")), "history summary rules present");
    check(app.styleSheet().contains(QStringLiteral("historyTable")), "history table rules present");
    // The rounded panel the table sits in, and the three count chips: both are widgets the page builds by
    // objectName, and an unstyled one is either a square table on a rounded card or plain loose text.
    check(app.styleSheet().contains(QStringLiteral("historyTableCard")),
          "history table panel rules present");
    check(app.styleSheet().contains(QStringLiteral("historyBadge")),
          "history count chip rules present");
    check(app.styleSheet().contains(QStringLiteral("historyRateBar")), "history rate bar rules present");
    check(app.styleSheet().contains(QStringLiteral("windowButton")), "window chrome rules present");
    // The rail and the page stack are the window's skeleton: if their rules go missing, the nav buttons
    // render as default raised buttons and the pages composite onto black.
    check(app.styleSheet().contains(QStringLiteral("navButton")), "rail button rules present");
    check(app.styleSheet().contains(QStringLiteral("pageStack")), "page stack rules present");
    // The dashboard redesign's own names. Each is a widget MainWindow paints or lays out by objectName,
    // and an unstyled one is a visible regression: #mainArea/#footerBar composite onto black, #appTitle
    // loses its 24 px weight, #statusDot loses the border-radius that makes it a dot at all.
    for (const QString &name : {QStringLiteral("mainArea"), QStringLiteral("appTitle"),
                                QStringLiteral("statusDot"), QStringLiteral("statusText"),
                                QStringLiteral("langButton"), QStringLiteral("footerBar"),
                                QStringLiteral("pingNumber"), QStringLiteral("pingLabel")}) {
        check(app.styleSheet().contains(name),
              QStringLiteral("dashboard rule present: %1").arg(name));
    }
    // The two neon values the C++ painters use must also be in the sheet, or the stylesheet and the
    // hand-painted circles drift apart into two greens and two greys.
    check(app.styleSheet().contains(QStringLiteral("#10B981")), "the sheet's live green is the painted one");
    check(app.styleSheet().contains(QStringLiteral("#9CA3AF")), "the sheet's badge grey is the painted one");
    check(app.palette().color(QPalette::Window) == Theme::windowBackground(), "palette matches tokens");

    Theme::apply(app, false);
    check(!Theme::isDark(), "light mode is active");
    check(app.styleSheet().contains(QStringLiteral("#F3F4F6")), "light background in the sheet");

    // Read one declaration out of one selector's rule. Needed because "does the sheet contain this hex"
    // is a much weaker question than it looks: #1F2937 is the light sheet's general text colour, so a
    // whole-sheet search stays green after the log rule alone stops matching the token the code paints
    // with. Scoping to the selector is what makes the check a check — and stripping the comments first is
    // what makes the scope mean the DECLARATION, since these files name selectors in prose to explain them
    // (see sheetWithoutComments).
    auto ruleProperty = [](const QString &sheetIn, const QString &selector, const QString &prop) {
        const QString sheet = sheetWithoutComments(sheetIn);
        const int at = sheet.indexOf(selector);
        const int open = at < 0 ? -1 : sheet.indexOf(QLatin1Char('{'), at);
        const int close = open < 0 ? -1 : sheet.indexOf(QLatin1Char('}'), open);
        if (close < 0)
            return QString();
        const QStringList decls = sheet.mid(open + 1, close - open - 1).split(QLatin1Char(';'));
        for (const QString &d : decls) {
            const int colon = d.indexOf(QLatin1Char(':'));
            if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0)
                return d.mid(colon + 1).trimmed();
        }
        return QString();
    };

    // Both sheets must carry the rules the dynamic properties switch on, or a badge/notice would render
    // as unstyled default text.
    for (bool dark : {true, false}) {
        Theme::apply(app, dark);
        const QString sheet = app.styleSheet();
        check(sheet.contains(QStringLiteral("[error=\"true\"]")),
              QStringLiteral("%1 sheet styles a failed profile row").arg(dark ? QStringLiteral("dark")
                                                                              : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("selected=\"true\"")),
              QStringLiteral("%1 sheet styles the selected row").arg(dark ? QStringLiteral("dark")
                                                                         : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("miniPing")),
              QStringLiteral("%1 sheet styles the row's ping chips").arg(dark ? QStringLiteral("dark")
                                                                              : QStringLiteral("light")));
        // The ring's colour is the verdict, and the verdict only reaches QSS through the token the row
        // stamps — a sheet without these rules draws every ring the same grey, whatever the service said.
        check(sheet.contains(QStringLiteral("miniPing[pingStatus=\"ok\"]")),
              QStringLiteral("%1 sheet colours a live ring").arg(dark ? QStringLiteral("dark")
                                                                     : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("miniPing[pingStatus=\"fail\"]")),
              QStringLiteral("%1 sheet colours a failed ring").arg(dark ? QStringLiteral("dark")
                                                                       : QStringLiteral("light")));
        // The row's menu opens on press, so the style's own arrow is a second affordance for one action.
        check(sheet.contains(QStringLiteral("#cardMenu::menu-indicator")),
              QStringLiteral("%1 sheet hides the row menu's arrow").arg(dark ? QStringLiteral("dark")
                                                                            : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("savedNote")),
              QStringLiteral("%1 sheet colours the save acknowledgement").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("trayNote")),
              QStringLiteral("%1 sheet colours the tray notice").arg(dark ? QStringLiteral("dark")
                                                                         : QStringLiteral("light")));
        // The settings page's cards. Each name is a widget the page builds by objectName, and an
        // unstyled one is visible: a card without its rule is a rectangle of the page's own colour with a
        // border nobody asked for, and the pair of icons beside the path field would take the generic
        // QToolButton padding that no longer fits them at 32 px.
        for (const QString &name : {QStringLiteral("settingsCard"),
                                    QStringLiteral("settingsCardTitle"),
                                    QStringLiteral("pathToolButton"),
                                    // The setup wizard's own five names. An unstyled one is visible
                                    // rather than merely plain: the status line's colour IS the state,
                                    // and the log pane without its rule is a proportional-font box in a
                                    // dialog full of monospaced output.
                                    QStringLiteral("setupStatus"),
                                    QStringLiteral("setupStatusFailed"),
                                    QStringLiteral("setupStatusDone"),
                                    QStringLiteral("setupProgress"),
                                    QStringLiteral("setupLog")})
            check(sheet.contains(name),
                  QStringLiteral("%1 sheet styles the settings card and its parts: %2")
                      .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), name));
        // The card must be a different surface from the page it floats on — that is the whole idea of a
        // card — and the theme's own surface token is what makes it so in both sheets.
        const QColor pageBg = sheetRuleColor(sheet, QStringLiteral("QWidget#settingsPage"),
                                             QStringLiteral("background-color"));
        const QColor cardBg = sheetRuleColor(sheet, QStringLiteral("QFrame#settingsCard"),
                                             QStringLiteral("background-color"));
        check(pageBg.isValid() && cardBg.isValid() && pageBg != cardBg,
              QStringLiteral("%1 sheet paints its cards on a surface distinct from the page (%2 vs %3)")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"),
                       pageBg.isValid() ? pageBg.name(QColor::HexRgb) : QStringLiteral("<none>"),
                       cardBg.isValid() ? cardBg.name(QColor::HexRgb) : QStringLiteral("<none>")));
        // The in-field glyph must be flattened: the generic tool-button padding would push it out of the
        // field it is supposed to sit inside.
        check(sheet.contains(QStringLiteral("QLineEdit QToolButton")),
              QStringLiteral("%1 sheet flattens the glyph drawn inside a line edit")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("navButton")),
              QStringLiteral("%1 sheet styles the rail").arg(dark ? QStringLiteral("dark")
                                                                 : QStringLiteral("light")));
        // The history page's rate bar is coloured by a dynamic property the code stamps and the sheet
        // reads; a band missing from one theme leaves that theme's summary a colourless thread.
        check(sheet.contains(QStringLiteral("historyRateBar[rateLevel=\"good\"]")),
              QStringLiteral("%1 sheet colours a healthy bypass").arg(dark ? QStringLiteral("dark")
                                                                          : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("historyRateBar[rateLevel=\"poor\"]")),
              QStringLiteral("%1 sheet colours a failing one").arg(dark ? QStringLiteral("dark")
                                                                       : QStringLiteral("light")));
        // The row tints are model data, and only the absence of an item background rule lets them
        // through: this is the sheet promising not to override them.
        check(!sheet.contains(QStringLiteral("historyTable::item { background")),
              QStringLiteral("%1 sheet leaves the row colours to the model").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The rounded box belongs to the panel the table sits in, not to the table: a QTableView's radius
        // rounds only its FRAME, while the header band and the rows are children that keep square corners
        // and paint over the arc — the bug. So the panel declares the border and the radius, the table
        // declares no fill and no border of its own, and HistoryPage keeps the table 4 px clear of the
        // panel's edge (checked where the corners are read, below).
        check(ruleProperty(sheet, QStringLiteral("QFrame#historyTableCard"),
                           QStringLiteral("border-radius"))
                  == QStringLiteral("12px"),
              QStringLiteral("%1 sheet rounds the table's panel").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The table takes the panel's own fill and no border of its own: the panel is the only thing in
        // there that draws a box. (The fill cannot be `transparent` either — a scroll viewport that paints
        // nothing composites onto black, which is why this sheet's last block exists.)
        check(ruleProperty(sheet, QStringLiteral("QTableView#historyTable"),
                           QStringLiteral("background-color"))
                  == ruleProperty(sheet, QStringLiteral("QFrame#historyTableCard"),
                                  QStringLiteral("background-color")),
              QStringLiteral("%1 sheet makes the table and its panel one surface").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(ruleProperty(sheet, QStringLiteral("QTableView#historyTable"),
                           QStringLiteral("border"))
                  == QStringLiteral("none"),
              QStringLiteral("%1 sheet gives the panel the only border in there").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The chips' two shouting bands are the sheet's own state colours, read off the property the code
        // stamps. A band missing from one theme leaves that theme's failures grey.
        check(sheet.contains(QStringLiteral("historyBadge[badgeKind=\"danger\"]")),
              QStringLiteral("%1 sheet colours a chip that counted failures").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("historyBadge[badgeKind=\"warning\"]")),
              QStringLiteral("%1 sheet colours a chip that counted substitutions").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // ---- the window's own shell ----
        //
        // The rounded window is a shape, and a shape is only ever visible as pixels — those are read off a
        // shown window in testWindowFrame(). What belongs here, in the sheet, is the OTHER half: that both
        // themes declare the shape at all, in the numbers MainWindow names, and that nothing in either
        // theme paints the rectangle that would hide it.
        const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
        const QString code = sheetWithoutComments(sheet);
        // The bug this file's whole cascade exists around, in one line: a `QMainWindow { … }` type rule puts
        // an opaque square over every pixel of a translucent surface, corners included, and the rounded
        // card below it becomes a rounding nobody can see. Structural, because "the sheet contains #0B0C10"
        // is true whether or not the window paints it.
        check(!hasUnqualifiedQMainWindowRule(code),
              QStringLiteral("%1 sheet gives the main window no rectangle of its own").arg(theme));
        check(ruleProperty(sheet, QStringLiteral("QMainWindow#mainWindow"),
                           QStringLiteral("background-color"))
                  == QStringLiteral("transparent"),
              QStringLiteral("%1 sheet's window rule is the transparent one").arg(theme));
        // The canvas between the window and the card is the transparent band, and is named here because a
        // reader who "completed" the window-surfaces block with it would undo the corners.
        check(ruleProperty(sheet, QStringLiteral("QWidget#rootCanvas"),
                           QStringLiteral("background-color"))
                  == QStringLiteral("transparent"),
              QStringLiteral("%1 sheet leaves the band around the shell unpainted").arg(theme));
        // The card itself: the arc, the stroke, the fill — all three read out of the constants the C++
        // side and the pixel test both use, so the sheets, the code and the pixels are one design.
        check(ruleProperty(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                           QStringLiteral("border-radius"))
                  == QStringLiteral("%1px").arg(MainWindow::kWindowFrameRadiusPx),
              QStringLiteral("%1 sheet rounds the window shell by %2px")
                  .arg(theme).arg(MainWindow::kWindowFrameRadiusPx));
        check(sheetRuleHex(sheet, QStringLiteral("QFrame#mainWindowFrame"), QStringLiteral("border"))
                  == Theme::border(),
              QStringLiteral("%1 sheet strokes the shell in the sheet's own border token")
                  .arg(theme));
        check(sheetRuleColor(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                             QStringLiteral("background-color"))
                  == Theme::windowBackground(),
              QStringLiteral("%1 sheet fills the shell with the window background").arg(theme));
        check(sheetRuleColor(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                             QStringLiteral("background-color"))
                  != sheetRuleColor(sheet, QStringLiteral("QFrame#sidebar"),
                                    QStringLiteral("background-color"))
                      || dark,
              QStringLiteral("%1 sheet's shell and rail are distinguishable, so a corner that leaks the "
                             "rail is a pixel that disagrees").arg(theme));
        // Qt does not clip a child to its parent's radius, so every surface that reaches a window corner
        // carries the arc's INNER edge — one pixel less than the radius, because the pixel it would
        // otherwise cover is the stroke. An unrounded one is a square of the app outside the silhouette.
        const QString inner = QStringLiteral("%1px").arg(MainWindow::kWindowFrameRadiusPx
                                                         - MainWindow::kWindowFrameBorderPx);
        struct CornerRule {
            const char *selector;
            const char *property;
        };
        const CornerRule kCornerRules[] = {
            {"QFrame#sidebar", "border-top-left-radius"},
            {"QFrame#sidebar", "border-bottom-left-radius"},
            {"QFrame#mainArea", "border-top-right-radius"},
            {"QFrame#mainArea", "border-bottom-right-radius"},
            {"QWidget#headerBar", "border-top-right-radius"},
            {"QFrame#footerBar", "border-bottom-right-radius"},
        };
        for (const CornerRule &c : kCornerRules) {
            check(ruleProperty(sheet, QLatin1String(c.selector), QLatin1String(c.property))
                      == inner,
                  QStringLiteral("%1 sheet rounds %2's %3 to the shell's inner arc (%4)")
                      .arg(theme, QLatin1String(c.selector), QLatin1String(c.property), inner));
        }

        // The dashboard's painted circles read their colours from C++ tokens, but everything framing them
        // is sheet styling — and both sheets must carry it, or the light theme shows an unstyled rail.
        for (const QString &name : {QStringLiteral("mainArea"), QStringLiteral("statusDot"),
                                    QStringLiteral("langButton"), QStringLiteral("pingLabel")}) {
            check(sheet.contains(name),
                  QStringLiteral("%1 sheet styles %2")
                      .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), name));
        }
        // The unread-journal alert is a dynamic property like [error], and its colour has the same two
        // sources as the accent: the rail label reads it off the sheet, while the badge glyph on a failed
        // card is tinted with Theme::error() in C++. One red, or the tab and the card disagree.
        check(sheet.contains(QStringLiteral("[alert=\"true\"]")),
              QStringLiteral("%1 sheet styles the unread-journal alert").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(Theme::error().name(QColor::HexRgb).toUpper()),
              QStringLiteral("%1 sheet's alert red is the red the code paints with").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The accent is painted by C++ code (icons, the window icon, QPalette::Highlight) *and* by the
        // sheet (buttons, borders, the checked tab). Those are one design, so the sheet must contain
        // the very hex the accessor returns — otherwise a code-drawn glyph drifts from the button
        // beside it, which is the quietest possible way for a theme to break.
        check(sheet.contains(Theme::accent().name(QColor::HexRgb).toUpper()),
              QStringLiteral("%1 sheet carries the accent the code paints with").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The journal's text colour has the accent's two sources too: the sheet paints the
        // QPlainTextEdit, and every line the log appends is formatted with Theme::logColor(0). A sheet
        // left on an older hex gives the frame one colour and its contents another, which reads as a
        // rendering glitch rather than a broken token. Scoped to the rule, not searched sheet-wide —
        // see ruleProperty above.
        const QString logHex =
            ruleProperty(sheet, QStringLiteral("QPlainTextEdit#logView"), QStringLiteral("color"));
        check(logHex.compare(Theme::logColor(0).name(QColor::HexRgb), Qt::CaseInsensitive) == 0,
              QStringLiteral("%1 sheet's logView colour (%2) is the colour the code writes log lines "
                             "in (%3)")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"),
                       logHex.isEmpty() ? QStringLiteral("<none>") : logHex,
                       Theme::logColor(0).name(QColor::HexRgb)));
        // The journal's other half, and the half the reported bug was about: ink and well have to be
        // readable TOGETHER. A rule with dark ink over a dark well passes any check that looks at one
        // colour alone, so the distance between the two is measured as well as each value.
        const QColor logInk = sheetRuleColor(sheet, QStringLiteral("QPlainTextEdit#logView"),
                                             QStringLiteral("color"));
        const QColor logWell = sheetRuleColor(sheet, QStringLiteral("QPlainTextEdit#logView"),
                                              QStringLiteral("background-color"));
        const int logSpan = logInk.isValid() && logWell.isValid()
                                ? qAbs(qGray(logInk.rgb()) - qGray(logWell.rgb()))
                                : -1;
        check(logSpan > 140,
              QStringLiteral("%1 sheet's journal is dark ink on a well that stands off it (%2 on %3, %4 "
                             "gray levels apart)")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                  .arg(logInk.isValid() ? logInk.name(QColor::HexRgb) : QStringLiteral("<none>"))
                  .arg(logWell.isValid() ? logWell.name(QColor::HexRgb) : QStringLiteral("<none>"))
                  .arg(logSpan));
        // The setup wizard's pane is the journal's sibling — monospace output in a recessed well — so it
        // speaks with the same ink. Two mono consoles in one app and two different greys is a palette
        // that drifted rather than a design.
        const QColor setupInk = sheetRuleColor(sheet, QStringLiteral("QPlainTextEdit#setupLog"),
                                               QStringLiteral("color"));
        check(setupInk.isValid() && setupInk == logInk,
              QStringLiteral("%1 sheet's setup console speaks with the journal's ink (%2 vs %3)")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                  .arg(setupInk.isValid() ? setupInk.name(QColor::HexRgb) : QStringLiteral("<none>"))
                  .arg(logInk.isValid() ? logInk.name(QColor::HexRgb) : QStringLiteral("<none>")));

        if (!dark) {
            // The light theme's failure report, stated as three properties rather than one hex: the well
            // is a light fill, the ink is dark, and the well is the recessed token rather than the card's
            // white. Any later palette nudge stays free while those three remain true.
            check(qGray(logWell.rgb()) > 200,
                  QStringLiteral("the light journal well is a light fill (%1)")
                      .arg(logWell.name(QColor::HexRgb)));
            check(isDarkInk(logInk),
                  QStringLiteral("…and its ink is dark, not the white the dark sheet left behind (%1)")
                      .arg(logInk.name(QColor::HexRgb)));
            check(logWell != Theme::surface(),
                  QStringLiteral("…and the well is the recessed fill, not the card's surface (%1 vs %2)")
                      .arg(logWell.name(QColor::HexRgb), Theme::surface().name(QColor::HexRgb)));
            // The two canvas tokens the redesign was asked for, read from the rules rather than from this
            // file's own literals: a soft grey-blue page, and a white card behind a grey hairline.
            check(pageBg == QColor(0xF3, 0xF4, 0xF6),
                  QStringLiteral("the light canvas is the soft grey-blue #F3F4F6 (%1)")
                      .arg(pageBg.name(QColor::HexRgb)));
            const QColor cardEdge = sheetRuleHex(sheet, QStringLiteral("QFrame#settingsCard"),
                                                 QStringLiteral("border"));
            check(cardBg == Theme::surface() && cardEdge == QColor(0xD1, 0xD5, 0xDB),
                  QStringLiteral("…and a card is white behind the grey hairline (%1 over %2)")
                      .arg(cardBg.name(QColor::HexRgb),
                           cardEdge.isValid() ? cardEdge.name(QColor::HexRgb)
                                              : QStringLiteral("<none>")));
            // Body copy across the light sheet, not only in the journal. The failure mode was inherited
            // white text, which is invisible on every surface this theme has; the rules below carry the
            // words a user actually reads. Accent and state colours are deliberately absent — they are
            // supposed to be coloured.
            const struct { const char *selector; const char *what; } inks[] = {
                { "QWidget", "the base text" },
                { "QLineEdit, QSpinBox, QComboBox", "the input text" },
                { "QCheckBox", "the switch labels" },
                { "QLabel#pageTitle", "page headings" },
                { "QLabel#footerHint", "the footer hint" },
                { "QLabel#muted", "secondary text" },
                { "QLabel#placeholder", "empty-state copy" },
            };
            for (const auto &inkRule : inks) {
                const QColor ink = sheetRuleColor(sheet, QLatin1String(inkRule.selector),
                                                  QStringLiteral("color"));
                check(isDarkInk(ink),
                      QStringLiteral("the light sheet keeps %1 dark (%2 from %3)")
                          .arg(QLatin1String(inkRule.what),
                               ink.isValid() ? ink.name(QColor::HexRgb)
                                             : QStringLiteral("<none>"),
                               QLatin1String(inkRule.selector)));
            }
        }
        // The dashboard's service mark is baked into a pixmap by PingIndicator::refreshGlyph(), so no
        // sheet rule can rescue it once the theme changes — it has to be the right colour at bake time.
        // The failure this guards is not "slightly low contrast" but the mark vanishing: white on the
        // light sheet's #FFFFFF page is nothing at all. Distance from the surface it lands on, rather
        // than mere inequality, is what a glyph actually needs to be seen.
        const QColor mark = Theme::markOnRing();
        const int contrast = qAbs(qGray(mark.rgb()) - qGray(Theme::surface().rgb()));
        check(contrast > 100,
              QStringLiteral("%1 sheet's ring mark stands off its page by %2 gray levels")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                  .arg(contrast));
        check(dark ? mark == QColor(Qt::white) : mark.lightness() < 128,
              QStringLiteral("%1 sheet's ring mark is the white-on-dark / dark-on-white the page "
                             "behind it requires").arg(dark ? QStringLiteral("dark")
                                                            : QStringLiteral("light")));
    }

    check(Theme::modeFromName(QStringLiteral("light")) == Theme::Mode::Light, "name -> Light");
    check(Theme::modeFromName(QStringLiteral("System")) == Theme::Mode::System, "name -> System");
    check(Theme::modeFromName(QStringLiteral("nonsense")) == Theme::Mode::Dark, "unknown name -> Dark");
    check(Theme::nameForMode(Theme::Mode::System) == QLatin1String("system"), "mode -> name");

    const QIcon play = Theme::icon(QStringLiteral("play"), QColor(Qt::red), 16);
    check(!play.isNull(), "play glyph loads from the qrc");
    check(!play.pixmap(16, 16).isNull(), "play glyph rasterises at 16px");
    const QPixmap pm = play.pixmap(16, 16);
    bool sawInk = false;
    if (!pm.isNull()) {
        const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height() && !sawInk; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = img.pixel(x, y);
                if (qAlpha(px) > 200 && qRed(px) > 120 && qGreen(px) < 90 && qBlue(px) < 90) {
                    sawInk = true;
                    break;
                }
            }
        }
    }
    check(sawInk, "currentColor substitution tinted the glyph red");

    // Every shipped icon must resolve, or the QSS/toolbars would silently show holes.
    const QStringList glyphs = {QStringLiteral("app"), QStringLiteral("play"), QStringLiteral("stop"),
                                QStringLiteral("refresh"), QStringLiteral("settings"),
                                QStringLiteral("tray"), QStringLiteral("clear"), QStringLiteral("copy"),
                                QStringLiteral("ping-ok"), QStringLiteral("ping-fail"),
                                QStringLiteral("ping-wait"), QStringLiteral("error"),
                                // The rail's own set: brand, the page glyphs and the foot action.
                                QStringLiteral("home"), QStringLiteral("logs"),
                                QStringLiteral("chart"),
                                QStringLiteral("power"),
                                // The dashboard's set: a ping badge's clock and the marks inside its ring.
                                QStringLiteral("clock"), QStringLiteral("globe"),
                                QStringLiteral("youtube"), QStringLiteral("discord"),
                                // The profile row's set: its menu handle, and the row menu's two glyphs.
                                QStringLiteral("more"), QStringLiteral("cards"),
                                // The frameless window's own chrome.
                                QStringLiteral("window-close"), QStringLiteral("window-minimize"),
                                QStringLiteral("ping-test"),
                                // The settings page's path row: the glyph inside the field and the two
                                // beside it. Each is looked up by name from C++ and baked into a pixmap at
                                // load time, so a missing file is an invisible control rather than an
                                // error — the one failure mode an icon-only button can have quietly.
                                QStringLiteral("folder"), QStringLiteral("search"),
                                QStringLiteral("download-cloud")};
    for (const QString &g : glyphs)
        check(QFile::exists(QStringLiteral(":/icons/%1.svg").arg(g)),
              QStringLiteral("icon file exists: %1").arg(g));

    // The three path-row glyphs are the app's only wordless buttons, so an icon that resolves to an empty
    // pixmap would leave a user staring at two 32 px plates with no clue what either is. Existence is not
    // enough — they have to carry ink at the size they are drawn at.
    for (const QString &g : {QStringLiteral("folder"), QStringLiteral("search"),
                             QStringLiteral("download-cloud")}) {
        const QPixmap pm = Theme::icon(g, Theme::iconColor(), 16).pixmap(16, 16);
        check(!pm.isNull(), QStringLiteral("path-row glyph rasterises: %1").arg(g));
        bool inked = false;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height() && !inked; ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 40) { inked = true; break; }
        }
        check(inked, QStringLiteral("path-row glyph has ink: %1").arg(g));
    }

    // The chrome and menu glyphs are thin strokes drawn for 16 px; a glyph that only "exists" but fails to
    // rasterise (a malformed path, a size that never got its currentColor) shows up as a hole in the header
    // or the row, so they are loaded through Theme::icon() exactly as the window draws them.
    Theme::apply(app, true);
    for (const QString &g : {QStringLiteral("more"), QStringLiteral("window-close"),
                             QStringLiteral("window-minimize")}) {
        const QPixmap pm = Theme::icon(g, Theme::iconColor(), 16).pixmap(16, 16);
        check(!pm.isNull(), QStringLiteral("chrome glyph rasterises: %1").arg(g));
        bool inked = false;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height() && !inked; ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 40) { inked = true; break; }
        }
        check(inked, QStringLiteral("chrome glyph has ink: %1").arg(g));
    }

    // The rail draws each glyph at 20 px through Theme::icon(), which rewrites the SVG's declared
    // width/height and its currentColor. A glyph that breaks that contract — a hardcoded fill, a
    // missing size — still "exists" as a file and shows up as a grey hole in the sidebar, so existence
    // alone is not enough for the ones the redesign added.
    Theme::apply(app, true);
    for (const QString &g : {QStringLiteral("home"), QStringLiteral("logs"), QStringLiteral("power"),
                             QStringLiteral("app"), QStringLiteral("clock"), QStringLiteral("globe"),
                             QStringLiteral("youtube"), QStringLiteral("discord"),
                             QStringLiteral("chart")}) {
        const QPixmap pm = Theme::icon(g, Theme::accent(), 20).pixmap(20, 20);
        check(!pm.isNull(), QStringLiteral("rail glyph rasterises: %1").arg(g));
        bool inked = false;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height() && !inked; ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 40) { inked = true; break; }
        }
        check(inked, QStringLiteral("rail glyph has ink: %1").arg(g));
    }

    // The brand marks are filled paths whose play triangle / eyes are cut out by fill-rule, so a ring's
    // 22 px white render must have holes in it as well as ink — a solid blob is the same size as the
    // mark and reads as an unrelated circle at a glance.
    {
        const QPixmap pm = Theme::icon(QStringLiteral("youtube"), QColor(Qt::white), 22).pixmap(22, 22);
        check(!pm.isNull(), "the youtube mark rasterises at ring size");
        int inked = 0, empty = 0;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 40)
                        ++inked;
                    else
                        ++empty;
                }
            }
        }
        check(inked > 0 && empty > 0, "the mark is drawn, and its play triangle is cut out");
    }

    // The badge glyph must actually rasterise in both themes, or a failed start shows a red hole.
    check(!Theme::icon(QStringLiteral("error"), Theme::error(), 16).pixmap(16, 16).isNull(),
          "the error glyph renders in the dark theme tint");
}

// -------------------------------------------------------------- PingService ----
static void testPingService()
{
    std::printf("\n[PingService]\n");

    check(PingService::defaultHosts().size() == 3, "three default hosts");
    check(PingService::defaultHosts().contains(QLatin1String("youtube.com")), "youtube is a default host");

    {
        PingService ping;
        // 127.0.0.1:443 answers instantly one way or the other on any host — we assert that a verdict
        // arrives, not which verdict it is.
        ping.setHosts({QStringLiteral("127.0.0.1")});
        ping.setIntervalMs(200);
        check(ping.hosts().size() == 1, "setHosts stored");
        check(ping.intervalMs() == 200, "setIntervalMs stored");

        QSignalSpy spy(&ping, &PingService::pingUpdated);
        ping.start();
        check(spy.wait(20000) || spy.count() > 0, "pingUpdated emitted");

        // A cycle emits Waiting first and the verdict afterwards: keep pumping events until the
        // verdict lands (bounded), rather than asserting on the first signal only.
        int waited = 0;
        while (waited < 20000) {
            bool verdictSeen = false;
            for (int i = 0; i < spy.count(); ++i) {
                const int status = spy.at(i).at(1).toInt();
                if (status == int(PingService::Ok) || status == int(PingService::Fail))
                    verdictSeen = true;
            }
            if (verdictSeen)
                break;
            QTest::qWait(50);
            waited += 50;
        }

        bool sawWaiting = false;
        bool sawVerdict = false;
        for (int i = 0; i < spy.count() && !(sawWaiting && sawVerdict); ++i) {
            const QList<QVariant> args = spy.at(i);
            if (args.size() != 3)
                continue;
            const int status = args.at(1).toInt();
            sawWaiting = sawWaiting || (status == int(PingService::Waiting));
            sawVerdict = sawVerdict || (status == int(PingService::Ok) || status == int(PingService::Fail));
            }
        for (int i = 0; i < spy.count(); ++i)
            check(spy.at(i).at(0).toString() == QLatin1String("127.0.0.1"), "signal names the host");
        check(sawWaiting, "a Waiting state was reported before the verdict");
        check(sawVerdict, "a terminal Ok/Fail was reported");

        ping.stop();
        // stop() posts into the worker thread, so the flag clears asynchronously.
        int cleared = 0;
        while (ping.isRunning() && cleared < 5000) {
            QTest::qWait(25);
            cleared += 25;
        }
        check(!ping.isRunning(), "stop() clears the running flag");
    }
    // Leaving the scope joins the worker thread (~PingService). If that hangs, the test times out
    // rather than leaking a thread.
    check(true, "PingService thread joined cleanly");
}

// ------------------------------------------------------------- DownloadDialog ----
//
// Nothing here talks to GitHub. The dialog's constructor only builds widgets and the request is
// startFetch(), which these tests never call — that separation is precisely what makes a download UI
// testable, and it is what the assertions below are written against.
static void testDownloadDialog()
{
    std::printf("\n[DownloadDialog]\n");

    DownloadDialog dialog;
    check(!dialog.isBusy(), "a fresh dialog has nothing in flight");
    check(!dialog.isDownloadEnabled(),
          "the download action is disabled until a release has actually been picked");
    check(dialog.installedPath().isEmpty(), "nothing is reported installed before anything ran");
    check(!dialog.statusText().isEmpty(), "and the status line is never left blank");
    check(dialog.downloader() != nullptr, "the dialog owns its downloader");

    // The default target is the directory the GUI manages — the same one ZapretLocator lists first, so
    // what is installed here is still found after a restart.
    const QString root = ZapretDownloader::installRoot();
    check(!root.isEmpty() && QDir::fromNativeSeparators(root).endsWith(QLatin1String("/zapret")),
          "the managed install directory ends in zapret");
    check(dialog.targetDir() == root, "and the dialog points at it without being told to");

    QTemporaryDir tmp;
    check(tmp.isValid(), "temporary directory for the target override");
    const QString overrideDir = tmp.filePath(QStringLiteral("zapret-target"));
    dialog.setTargetDir(overrideDir);
    check(dialog.targetDir() == overrideDir, "an override is reported back verbatim");
    check(!QDir(overrideDir).exists(),
          "naming a target does not create it: only a finished download may write there");

    // Wording the info line is built from, checked without a network answer.
    check(DownloadDialog::humanSize(0) == QStringLiteral("0 Б"), "zero bytes has wording of its own");
    check(DownloadDialog::humanSize(1536).contains(QStringLiteral("КиБ")), "kilobytes are spelled in КиБ");
    check(DownloadDialog::humanSize(7844856).contains(QStringLiteral("МиБ")), "and megabytes in МиБ");
    check(DownloadDialog::humanSize(-1) == QStringLiteral("неизвестно"),
          "an unannounced size is called unknown, not zero");

    // ---- which completion message the finished path earns ----
    //
    // onDownloadFinished() ends in a modal QMessageBox, so a headless suite cannot call it: nothing is
    // there to click OK, and the test would hang rather than fail. The decision it used to make inline —
    // promoted or not, and therefore what the user is told about future password prompts — now lives in
    // two statics, which is what makes it checkable at all. What is asserted here is the promise the GUI
    // makes to the user, not the promotion mechanics (coretest owns those).
    const QString systemTree = ZapretPaths::systemInstallDir();
    const QString managedTree = ZapretPaths::managedInstallDir();

    // The one path that is a promoted install, and everything that merely resembles one.
    check(DownloadDialog::isPromotedPath(systemTree) == !systemTree.isEmpty(),
          "the system tree is promoted, and nothing is when this host has no system tree");
    check(!DownloadDialog::isPromotedPath(QString()), "an unset path is not a promoted install");
    check(!DownloadDialog::isPromotedPath(managedTree),
          "the managed folder is never announced as the system-wide install");
    // The sibling names a promotion builds beside the tree. Both hold a not-yet-root-owned payload, and
    // the polkit rule deliberately refuses them, so promising passwordless starts for either is a lie
    // about a tree the rule will still challenge. Exact match, not prefix match, is the property.
    check(!DownloadDialog::isPromotedPath(systemTree + QStringLiteral(".new")),
          "the staging directory is not a promoted install");
    check(!DownloadDialog::isPromotedPath(systemTree + QStringLiteral(".old")),
          "nor is the backup of the incumbent");
    check(!DownloadDialog::isPromotedPath(QStringLiteral("/opt/zapret")),
          "upstream's own default tree is not ours");
    check(!DownloadDialog::isPromotedPath(QLatin1Char('/') + systemTree),
          "and a doubled separator does not sneak past the equality test");

    const QString rootNote = LanguageManager::tr(QStringLiteral("dl.root_note"));
    const QString failureNote = QStringLiteral("the root helper is not installed");

    if (systemTree.isEmpty())
    {
        // Windows: there is no destination a download can be promoted into, so no finished path — not
        // even the empty string, which is what the dialog would otherwise compare against — may be
        // reported as a passwordless install. Every install this platform makes is the managed one.
        const QString text = DownloadDialog::installedMessageText(managedTree, failureNote);
        check(!text.contains(rootNote),
              "windows never promises passwordless starts, because it can never promote");
        check(!DownloadDialog::installedMessageText(QString(), QString()).contains(rootNote),
              "and an empty path there reads as no install, not as a promoted one");
    }
    else
    {
        // A promoted install promises passwordless starts, and must not also carry a stale failure line:
        // a promotion that landed after an earlier one failed should not still report that failure.
        const QString promotedText = DownloadDialog::installedMessageText(systemTree, failureNote);
        check(promotedText.contains(rootNote),
              "a promoted install says starts will not ask for a password");
        check(!promotedText.contains(failureNote),
              "and it does not repeat a failure the successful promotion just contradicted");
        check(promotedText.contains(QDir::toNativeSeparators(systemTree)),
              "naming the tree the payload really sits in");
    }

    // The managed folder works identically but asks each time, so the reason the promotion fell through
    // is the whole value of the message — that is what the user would otherwise have to go find.
    const QString managedText = DownloadDialog::installedMessageText(managedTree, failureNote);
    check(!managedText.contains(rootNote),
          "a managed install does not promise passwordless starts it cannot deliver");
    check(managedText.contains(failureNote), "and carries the reason why");
    check(managedText.contains(QDir::toNativeSeparators(managedTree)), "naming where it actually landed");

    // No promotion attempted (the default, and the whole of Windows) and no promotion note to show: the
    // message is the bare location, with no dangling blank lines from an empty note.
    const QString plainText = DownloadDialog::installedMessageText(managedTree, QString());
    check(!plainText.contains(rootNote) && !plainText.contains(failureNote),
          "with nothing to report about promotion the message says nothing about it");
    check(!plainText.endsWith(QStringLiteral("\n")), "and an empty note leaves no trailing blank line");
}

// ---------------------------------------------------------------- History page ----
//
// The page is the record's face: a summary of everything the store holds, and the rows themselves with
// their verdict painted on. Half of that is arithmetic and mapping — what colour is an intercepted row,
// what percent is two of three — which is checked on the class without a widget at all; the other half
// is "does the table follow the store", checked on a live page over a real temporary file.

namespace {

// A history page that answers its own confirmation. The default one opens a modal, and a suite has no
// pointer to click and nobody to press Escape; what is worth checking is the decision on either side of
// the answer, so the answer is data here.
class ClearProbe : public HistoryPage
{
public:
    using HistoryPage::HistoryPage;

    bool answerYes = true;
    int asked = 0;
    int lastTotal = -1;

protected:
    bool confirmClear(int totalRows) override
    {
        ++asked;
        lastTotal = totalRows;
        return answerYes;
    }
};

// The colour one sheet declares for one selector property. The corner checks below compare pixels
// with colours the SHEET owns - the panel fill and the header band - so reading them out of the
// sheet is what keeps those assertions true in either theme; hardcoding them here would test this
// file, not a theme.
QColor sheetColor(const QString &sheet, const QString &selector, const QString &prop)
{
    const int at = sheet.indexOf(selector);
    const int open = at < 0 ? -1 : sheet.indexOf(QLatin1Char('{'), at);
    const int close = open < 0 ? -1 : sheet.indexOf(QLatin1Char('}'), open);
    if (close < 0)
        return QColor();
    const QStringList decls = sheet.mid(open + 1, close - open - 1).split(QLatin1Char(';'));
    for (const QString &d : decls) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0)
            return QColor(d.mid(colon + 1).trimmed());
    }
    return QColor();
}

// What a rendered row strip is mostly made of: its background. Averaged over the strip rather than
// sampled, because the text in it is a few hundred pixels against twenty thousand of tint.
QColor rowAverage(const QImage &shot, int row, int rowHeight)
{
    const int top = row * rowHeight + 4;
    const int bottom = qMin(shot.height(), top + qMax(1, rowHeight - 10));
    qint64 r = 0, g = 0, b = 0, n = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = 4; x + 4 < shot.width(); ++x) {
            const QColor c = shot.pixelColor(x, y);
            r += c.red(); g += c.green(); b += c.blue(); ++n;
        }
    }
    if (n == 0)
        return QColor();
    return QColor(int(r / n), int(g / n), int(b / n));
}

} // namespace

static void testHistoryPage()
{
    std::printf("\n[HistoryPage]\n");

    // ---- the three verdicts, from the two booleans a row carries ----
    HistoryEntry passed;   passed.host = QStringLiteral("a.example");   passed.success = true;
    HistoryEntry intercepted; intercepted.host = QStringLiteral("b.example");
    intercepted.success = true; intercepted.certValid = false;
    HistoryEntry failed;   failed.host = QStringLiteral("c.example");   failed.success = false;
    HistoryEntry failedWithBadCert = failed;
    failedWithBadCert.certValid = false;

    check(HistoryPage::verdictFor(passed) == HistoryVerdict::Passed,
          "a clean pass is the boring green");
    check(HistoryPage::verdictFor(intercepted) == HistoryVerdict::Intercepted,
          "a pass whose certificate was rejected is the amber one — that is why the field exists");
    check(HistoryPage::verdictFor(failed) == HistoryVerdict::Failed, "no connection is a failure");
    check(HistoryPage::verdictFor(failedWithBadCert) == HistoryVerdict::Failed,
          "…and stays one when the TLS half failed too: a check that never connected has no "
          "interception to accuse anybody of");

    check(HistoryPage::verdictColor(HistoryVerdict::Passed) == Theme::success(),
          "the passed word is the theme's green, not a second green");
    check(HistoryPage::verdictColor(HistoryVerdict::Failed) == Theme::error(),
          "the failed word is the theme's red");
    check(HistoryPage::verdictColor(HistoryVerdict::Intercepted) == Theme::warning(),
          "the intercepted word is the theme's amber");

    const QColor tint = HistoryPage::verdictTint(HistoryVerdict::Intercepted);
    check(tint.red() == Theme::warning().red() && tint.green() == Theme::warning().green()
              && tint.blue() == Theme::warning().blue(),
          "a row's background is its verdict's colour, not an unrelated one");
    check(tint.alpha() > 0 && tint.alpha() < 90,
          QStringLiteral("…and kept a wash rather than a fill (alpha %1)").arg(tint.alpha()));

    // The words themselves are catalog entries; a missing row falls back to the key, which is exactly
    // what a status column must never show.
    for (const QString &key : {QStringLiteral("history.verdict.passed"),
                               QStringLiteral("history.verdict.failed"),
                               QStringLiteral("history.verdict.intercepted")})
        check(LanguageManager::tr(key) != key,
              QStringLiteral("the status column's word '%1' is translated").arg(key));
    check(HistoryPage::verdictText(HistoryVerdict::Intercepted)
              == QStringLiteral("Подмена сертификата"),
          "the amber row says what happened to the certificate, in Russian");

    // ---- the arithmetic behind the headline ----
    check(HistoryPage::Tally().successRate() == -1,
          "an empty record has no percentage, and 0% is a different claim");

    QVector<HistoryEntry> mixed;
    mixed << passed << passed << intercepted << failed;
    const HistoryPage::Tally t = HistoryPage::tallyOf(mixed);
    check(t.total == 4 && t.passed == 2 && t.intercepted == 1 && t.failed == 1,
          "one pass over the rows counts each verdict once");
    check(t.successRate() == 50, "two of four is 50%");
    check(HistoryPage::tallyOf(QVector<HistoryEntry>{failed}).successRate() == 0,
          "none of one is 0%, and that is not the same sentence as -1");

    QVector<HistoryEntry> twoOfThree;
    twoOfThree << passed << passed << failed;
    check(HistoryPage::tallyOf(twoOfThree).successRate() == 67,
          "two of three rounds to 67% — a truncated 66% undersells the record it summarises");
    // ---- the live page over a real store ----
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the history file");
    HistoryStore store(dir.filePath(QStringLiteral("history.json")));
    store.setSaveDelayMs(0);

    ClearProbe page(&store);
    check(page.rowCount() == 0, "a fresh record shows an empty table");
    check(page.showsEmptyNotice(), "…and says so, instead of drawing a bare grid of headings");
    check(page.successRate() == -1, "with no number to show");
    check(!page.clearButton()->isEnabled(), "and nothing to clear, so the button says so");

    // ---- the empty state, measured rather than eyeballed ----
    //
    // The report was that the sentence rides over the clock icon or gets cut. Both are geometry, so both
    // are read as geometry: the two labels' rects in the empty page's own coordinates, and the height the
    // word-wrapped label asks for at the width it was given. A stack page that is shorter than its own
    // content is the usual cause of the second one, and it is invisible to every other check here.
    {
        QWidget *empty = page.findChild<QWidget *>(QStringLiteral("historyEmpty"));
        QLabel *glyph = empty ? empty->findChild<QLabel *>(QStringLiteral("historyEmptyGlyph")) : nullptr;
        QLabel *text = empty ? empty->findChild<QLabel *>(QStringLiteral("placeholder")) : nullptr;
        check(empty && glyph && text,
              "the empty state is an icon and a sentence, both reachable by name");
        if (empty && glyph && text) {
            page.resize(880, 620);
            page.show();
            QTest::qWait(40);
            const QRect g(QRect(glyph->mapTo(empty, QPoint(0, 0)), glyph->size()));
            const QRect t(text->mapTo(empty, QPoint(0, 0)), text->size());
            check(t.top() >= g.bottom(),
                  QStringLiteral("the sentence starts below the icon, not across it (icon ends %1, text "
                                 "starts %2)").arg(g.bottom()).arg(t.top()));
            const int wanted = text->heightForWidth(t.width());
            check(t.height() >= wanted,
                  QStringLiteral("…and the label is given the height its own wrapping asks for "
                                 "(%1 px wanted, %2 given)").arg(wanted).arg(t.height()));
            check(!text->text().isEmpty() && text->isVisible(),
                  "…and there is a visible sentence to measure, so the two readings above are not a "
                  "comparison between two empty rectangles");

            // ---- the sentence is centred on its own middle, not on the pair's ----
            //
            // The report was that the sentence had slid down, and the arithmetic explains why it read
            // that way while the block was technically centred: the block is an icon above a sentence, and
            // a pair centred as one unit leaves its *text* half an icon-plus-gap below the middle of the
            // panel — twenty-four pixels on this page, which is exactly "somewhere in the lower half".
            // What the eye is looking for is the sentence, so the sentence is what gets measured against
            // the panel's centre line.
            {
                const int panelCentre = empty->rect().center().y();
                check(qAbs(t.center().y() - panelCentre) <= 3,
                      QStringLiteral("the empty-state sentence sits on the panel's centre line "
                                     "(sentence %1, panel %2)")
                          .arg(t.center().y())
                          .arg(panelCentre));
            }
        }
    }

    // Timestamps spelled out rather than left to the store's "stamp it now": the ordering checks below
    // are about which row the page picks first, and two appends inside the same millisecond would make
    // that a race instead of a fact.
    const QDateTime t0(QDate(2026, 10, 22), QTime(12, 0, 0));
    store.appendResult(QStringLiteral("nfqws (portable)"), QStringLiteral("first.example"),
                       true, 42, true, t0);
    check(page.rowCount() == 1,
          "the store's changed() moves the table by itself — nobody had to tell the page");
    check(!page.showsEmptyNotice(), "and the notice steps aside for it");
    check(page.clearButton()->isEnabled(), "…while clearing becomes a thing you can do");

    store.appendResult(QStringLiteral("nfqws (portable)"), QStringLiteral("second.example"),
                       false, -1, true, t0.addSecs(60));
    store.appendResult(QStringLiteral("str192-domain"), QStringLiteral("third.example"),
                       true, 10, false, t0.addSecs(120));
    check(page.rowCount() == 3, "one refresh per recorded check");

    QAbstractItemModel *model = page.table()->model();
    const int colHost = HistoryTableModel::ColumnHost;
    const int colVerdict = HistoryTableModel::ColumnVerdict;
    const int colLatency = HistoryTableModel::ColumnLatency;
    const int colTime = HistoryTableModel::ColumnTime;
    check(model->columnCount(QModelIndex()) == HistoryTableModel::ColumnCount,
          "the table has the five columns it was drawn with");
    check(model->data(model->index(0, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("third.example"),
          "newest first: the row the user just watched happen is at the top");
    check(model->data(model->index(2, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("first.example"), "…and the oldest is at the bottom");

    check(model->data(model->index(0, colVerdict), Qt::DisplayRole).toString()
              == HistoryPage::verdictText(HistoryVerdict::Intercepted),
          "the newest row is the amber one, and says so in words as well as in colour");
    check(model->data(model->index(1, colVerdict), Qt::DisplayRole).toString()
              == HistoryPage::verdictText(HistoryVerdict::Failed), "the middle row is the failure");
    check(model->data(model->index(2, colVerdict), Qt::DisplayRole).toString()
              == HistoryPage::verdictText(HistoryVerdict::Passed), "the oldest row passed");

    check(model->data(model->index(1, colLatency), Qt::DisplayRole).toString()
              == QStringLiteral("—"),
          "a check that never connected shows a dash, not the 0 it was measured as");
    check(model->data(model->index(2, colLatency), Qt::DisplayRole).toString()
              == QStringLiteral("42"), "a measured latency shows as whole milliseconds");
    check(model->data(model->index(2, colTime), Qt::DisplayRole).toString()
              .contains(QStringLiteral("22.10.2026")), "the time column carries the date too");

    // The colour coding, as the view is told it: every cell of the row wears the verdict, and the status
    // word wears it at full strength.
    check(model->data(model->index(0, colHost), Qt::BackgroundRole).value<QBrush>().color()
              == HistoryPage::verdictTint(HistoryVerdict::Intercepted),
          "an intercepted row's background is the amber wash");
    check(model->data(model->index(1, colTime), Qt::BackgroundRole).value<QBrush>().color()
              == HistoryPage::verdictTint(HistoryVerdict::Failed),
          "…a failed row's is the red one, on every column of the row");
    check(model->data(model->index(0, colVerdict), Qt::ForegroundRole).value<QBrush>().color()
              == HistoryPage::verdictColor(HistoryVerdict::Intercepted),
          "and the status word is its verdict's colour at full strength");
    check(!model->data(model->index(0, colHost), Qt::ForegroundRole).isValid(),
          "…which the rest of the row deliberately does not take, or the page would be a palette");

    // ---- the summary ----
    check(page.successRate() == 33, "one clean check in three is 33%");
    check(page.headline()->text() == LanguageManager::tr(QStringLiteral("history.headline"),
                                                        QStringLiteral("33")),
          "and the headline says exactly that");
    // ---- the three count chips ----
    // One sentence cannot be coloured per number, so the summary is three chips and the colour is the
    // decision worth testing: red and amber are claims that something happened, which means a zero must
    // not wear one and a seven must not wear grey.
    check(page.summaryBadgesVisible(), "a record with rows in it shows its chips");
    check(page.totalBadge()->text()
              == LanguageManager::tr(QStringLiteral("history.badge.total")).arg(3),
          "the first chip names what the percentage was computed from");
    check(page.failedBadge()->text()
              == LanguageManager::tr(QStringLiteral("history.badge.failed")).arg(1),
          "…the second the checks that never connected…");
    check(page.certBadge()->text()
              == LanguageManager::tr(QStringLiteral("history.badge.cert")).arg(1),
          "…and the third the certificates that came back somebody else's");
    check(page.badgeKind(page.totalBadge()) == QStringLiteral("neutral"),
          "a total is a number, never an alarm");
    check(page.badgeKind(page.failedBadge()) == QStringLiteral("danger"),
          "one failure does wear the sheet's red");
    check(page.badgeKind(page.certBadge()) == QStringLiteral("warning"),
          "…and one substitution its amber");
    // …and the band reaches the screen. Each chip carries its figure in an inline stylesheet (the mono
    // font needs one to beat the sheet's generic `QWidget` rule), which is exactly the situation where a
    // widget's own colours could silently stop coming from the app sheet — so a chip is rendered and the
    // colour it claimed is counted inside it.
    auto pixelsOf = [](QLabel *badge, const QColor &ink) {
        const QImage img = badge->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        int hits = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (qAbs(c.red() - ink.red()) < 24 && qAbs(c.green() - ink.green()) < 24
                    && qAbs(c.blue() - ink.blue()) < 24)
                    ++hits;
            }
        return hits;
    };
    check(pixelsOf(page.failedBadge(), Theme::error()) > 40,
          QStringLiteral("the failure chip is rendered in the sheet's red (%1 px of %2x%3)")
              .arg(pixelsOf(page.failedBadge(), Theme::error()))
              .arg(page.failedBadge()->width()).arg(page.failedBadge()->height()));
    check(pixelsOf(page.certBadge(), Theme::warning()) > 40,
          QStringLiteral("…and the substitution chip in its amber (%1 px of %2x%3)")
              .arg(pixelsOf(page.certBadge(), Theme::warning()))
              .arg(page.certBadge()->width()).arg(page.certBadge()->height()));
    // Read across as well as straight: each chip wears one colour, not either. Without these the two
    // counts above would be satisfied by a chip that painted the wrong state colour, and the two state
    // colours are close enough for a loose tolerance to let that through.
    check(pixelsOf(page.failedBadge(), Theme::warning()) == 0,
          "…and the failure chip is not the amber one");
    check(pixelsOf(page.certBadge(), Theme::error()) == 0, "…and the substitution chip is not the red one");
    check(page.certBadge()->text() != page.failedBadge()->text(),
          "the two chips are different labels, not one widget checked twice");
    check(pixelsOf(page.totalBadge(), Theme::error()) == 0,
          "…while the total chip wears no alarm colour at all");

    check(page.table()->model()->rowCount() == page.rowCount(),
          "the hook the suite reads is the table's own row count, not a second number");

    // ---- do the row colours survive the stylesheet? ----
    //
    // Everything above is the model's side of the promise. The sheet is the other half, and it is the
    // half that breaks silently: the tint is a brush on the model, and a styled item view only puts it on
    // screen because the page installs a delegate that fills it (see VerdictDelegate). Drop that delegate,
    // or let the sheet declare a background of its own for ::item, and the table keeps its words, its
    // columns and its order while losing the one thing that makes a failed night findable at a glance.
    // So the table is rendered, and its pixels are read.
    page.resize(840, 460);
    if (QLayout *l = page.layout())
        l->activate();
    const QImage shot = page.table()->viewport()->grab().toImage()
                           .convertToFormat(QImage::Format_ARGB32);
    check(shot.width() > 300 && shot.height() > 80,
          QStringLiteral("the table renders at %1x%2").arg(shot.width()).arg(shot.height()));
    if (shot.width() > 300 && shot.height() > 80) {
        // Redness is where the red and the amber rows part company from the green one, greenness is
        // where the amber parts from the red: amber is a green that ran out of blue, and red is not.
        auto redness = [&](int row) {
            const QColor c = rowAverage(shot, row, 30);
            return c.red() - c.green();
        };
        auto greenness = [&](int row) {
            const QColor c = rowAverage(shot, row, 30);
            return c.green() - c.blue();
        };
        check(redness(2) < redness(0) - 4 && redness(0) < redness(1) - 4,
              QStringLiteral("rendered: the passed row (%1) is less red than the intercepted (%2), "
                             "which is less red than the failed one (%3)")
                  .arg(redness(2)).arg(redness(0)).arg(redness(1)));
        check(greenness(1) < greenness(2) - 4 && greenness(2) < greenness(0) - 4,
              QStringLiteral("rendered: the failed row (%1) is less green than the passed (%2), "
                             "which is less green than the amber one (%3)")
                  .arg(greenness(1)).arg(greenness(2)).arg(greenness(0)));

        // The word, not only the row. An inherited `color` from the sheet would paint every status word
        // in the page's text colour and leave the verdict in the background alone — a page that reads as
        // three grey words on three coloured stripes is one edit away from reading as nothing.
        auto inkCount = [&](int row, const QColor &want) {
            int n = 0;
            const int top = row * 30 + 4;
            const int bottom = qMin(shot.height(), top + 20);
            for (int y = top; y < bottom; ++y)
                for (int x = 4; x + 4 < shot.width(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    const int d = qAbs(c.red() - want.red()) + qAbs(c.green() - want.green())
                                  + qAbs(c.blue() - want.blue());
                    if (d < 90)
                        ++n;
                }
            return n;
        };
        check(inkCount(0, Theme::warning()) > 20, "the amber row's status word is amber");
        check(inkCount(1, Theme::error()) > 20, "the failed row's status word is red");
        check(inkCount(2, Theme::success()) > 20, "the passed row's status word is green");
    }

    // The three chips have to fit beside the headline rather than push it out: the window is a fixed
    // 900 px, and a summary that overflowed would clip the one number the page exists to show.
    if (QLayout *fit = page.layout())
        fit->activate();
    const QRect chips(page.totalBadge()->mapTo(&page, QPoint(0, 0)),
                      page.certBadge()->mapTo(&page, QPoint(page.certBadge()->width(), 0)));
    const QRect head(page.headline()->mapTo(&page, QPoint(0, 0)),
                     page.headline()->mapTo(&page, QPoint(page.headline()->width(), 0)));
    check(chips.right() < page.width() && chips.left() > head.right()
              && page.totalBadge()->height() > 8 && page.certBadge()->width() > 20,
          QStringLiteral("the chips fit on the summary line, after the headline (head ends %1, "
                         "chips run %2..%3 of %4)")
              .arg(head.right()).arg(chips.left()).arg(chips.right()).arg(page.width()));

    // ---- does the panel's shape actually hold? ----
    //
    // Everything above is the model's side of the page. Its shape is the other half, and it is the half
    // that broke silently: the radius used to sit on the QTableView, which rounds its own frame while the
    // header band paints straight over the arc, square-cornered and outside the panel. No assertion on the
    // rows and no contains("border-radius") on the sheet can see that, so the pixels are read - against the
    // colours the sheet itself declares, which is what keeps each of them true in either theme.
    {
        QWidget *card = page.tableCard();
        QHeaderView *hdr = page.table()->horizontalHeader();
        check(card != nullptr && hdr != nullptr && card->width() > 120 && card->height() > 60,
              QStringLiteral("the table's panel is on screen at %1x%2")
                  .arg(card ? card->width() : -1).arg(card ? card->height() : -1));
        if (card && hdr && card->width() > 120 && card->height() > 60) {
            auto within6 = [](const QColor &a, const QColor &b) {
                return qAbs(a.red() - b.red()) <= 6 && qAbs(a.green() - b.green()) <= 6
                       && qAbs(a.blue() - b.blue()) <= 6;
            };
            // Run it in BOTH themes, and each theme proves something the other cannot. The three
            // greys in play are: light page #F3F4F6, card #FFFFFF, band #F3F4F6; dark page #0B0C10,
            // card #12131A, band #15161C — and within6() is the yardstick.
            //
            //  * A corner left square because the table sits flush to the panel reads the CARD FILL
            //    there: 12 levels off the page grey in light, 14 in dark. Both runs catch it.
            //  * A corner covered by the HEADER BAND reads the band colour: 14 off the page in dark,
            //    so dark catches it — but #F3F4F6 in light is the page's own grey, so a band leak in
            //    light is invisible to this particular sample. It is dark's job, not light's.
            //  * Which rule owns the band is light's job. Dark declares that grey twice — #15161C on
            //    QHeaderView#historyHeader::section and the same #15161C on the generic
            //    QHeaderView::section fallback at the foot of the sheet — so a band painted by either
            //    rule looks identical there. Light declares #F3F4F6 against that fallback's #E5E7EB,
            //    fourteen apart: only the light run can see the ID-qualified rule beating the
            //    fallback, which is what the inset relies on (a band the fallback re-tinted would
            //    change colour under the arc without changing its shape).
            //
            // Reading the wanted colours out of the live sheet instead of hardcoding them is what
            // lets one loop cover both.
            for (const bool dark : {true, false}) {
                const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
                Theme::apply(*static_cast<QApplication *>(qApp), dark);
                if (QLayout *ls = page.layout())
                    ls->activate();
                const QImage chrome = page.grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QColor pageInk = Theme::windowBackground();
                const QColor bandInk = sheetColor(static_cast<QApplication *>(qApp)->styleSheet(),
                                                  QStringLiteral("QHeaderView#historyHeader::section"),
                                                  QStringLiteral("background-color"));
                check(bandInk.isValid(),
                      QStringLiteral("the %1 sheet declares the header band's colour").arg(theme));
                // One pixel inside the panel's frame, at the corner. Outside a 12 px arc there is
                // nothing but the page behind the panel, and a band that leaked covers this point.
                const QPoint tl = card->mapTo(&page, QPoint(1, 1));
                const QPoint tr = card->mapTo(&page, QPoint(card->width() - 2, 1));
                // And the band itself, read in the strip where its text does not paint: proof the clean
                // corners are a shape rather than a header that forgot to draw at all. In the left
                // padding of the first section, vertically centred — band colour there, and no glyph,
                // whichever way the header's own padding resolves.
                const QPoint band = hdr->mapTo(&page, QPoint(3, hdr->height() / 2));
                check(chrome.rect().contains(tl) && within6(chrome.pixelColor(tl), pageInk),
                      QStringLiteral("%1: the panel's top-left corner shows the page behind it, not "
                                     "the header band (%2,%3,%4)")
                          .arg(theme).arg(chrome.pixelColor(tl).red())
                          .arg(chrome.pixelColor(tl).green()).arg(chrome.pixelColor(tl).blue()));
                check(chrome.rect().contains(tr) && within6(chrome.pixelColor(tr), pageInk),
                      QStringLiteral("…%1 and so does its top-right one (%2,%3,%4)")
                          .arg(theme).arg(chrome.pixelColor(tr).red())
                          .arg(chrome.pixelColor(tr).green()).arg(chrome.pixelColor(tr).blue()));
                check(chrome.rect().contains(band) && within6(chrome.pixelColor(band), bandInk),
                      QStringLiteral("%1: the header band is painted where the sheet puts it "
                                     "(%2,%3,%4), so the clean corners are a shape and not a blank")
                          .arg(theme).arg(chrome.pixelColor(band).red())
                          .arg(chrome.pixelColor(band).green())
                          .arg(chrome.pixelColor(band).blue()));
                chrome.save(dark ? QStringLiteral("/tmp/stats-dark.png")
                                 : QStringLiteral("/tmp/stats-light.png"));
            }
            // Leave the page on dark, the default the rest of the binary and the app itself expect.
            Theme::apply(*static_cast<QApplication *>(qApp), true);
            if (QLayout *ls = page.layout())
                ls->activate();
        }
    }

    // ---- sorting ----
    // The unmeasured row has to land at the far end of an ascending latency sort, not at the front: a
    // reader clicking the latency header is asking what is slowest to answer, and "it never answered" is
    // not the fastest possible answer.
    page.table()->sortByColumn(HistoryTableModel::ColumnLatency, Qt::AscendingOrder);
    check(model->data(model->index(0, colLatency), Qt::DisplayRole).toString()
              == QStringLiteral("10"),
          "sorting by latency puts the quickest answer first");
    check(model->data(model->index(2, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("second.example"),
          "…and the check that never connected last, not first");
    page.table()->sortByColumn(colTime, Qt::DescendingOrder);
    check(model->data(model->index(0, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("third.example"),
          "the newest-first reading order comes back on the click that asks for it");

    // ---- the clear, both answers ----
    QSignalSpy clearedSpy(&page, &HistoryPage::cleared);
    page.answerYes = false;
    page.clearButton()->click();
    check(page.asked == 1, "the press asks before it destroys");
    check(page.lastTotal == 3, "and says how much is at stake while asking");
    check(store.count() == 3 && page.rowCount() == 3,
          "a no leaves every row where it was, in the file and on the screen alike");
    check(clearedSpy.count() == 0, "…and is not reported as a clearing");

    page.answerYes = true;
    page.clearButton()->click();
    check(page.asked == 2, "a second press asks again rather than remembering the last answer");
    check(store.count() == 0, "a yes empties the record");
    check(page.rowCount() == 0, "…and the table follows the record, not a copy of it");
    check(page.showsEmptyNotice(), "the notice comes back into its own");
    check(page.successRate() == -1, "with the percentage taken away rather than set to zero");
    check(!page.clearButton()->isEnabled(), "…and nothing left to clear");
    check(clearedSpy.count() == 1 && clearedSpy.at(0).at(0).toInt() == 3,
          "the page says how many rows went, so the window can put the number in the journal");
    check(page.headline()->text() == LanguageManager::tr(QStringLiteral("history.headline.empty")),
          "the headline switches to the sentence an empty record deserves");
    check(!page.summaryBadgesVisible(),
          "…and the three chips go quiet rather than reciting three zeroes at it");
    // The bar's band is a dynamic property the sheet colours. "none" is the point: an empty record must
    // not be painted as a verdict, and a grey thread is the only honest thing to show for it.
    check(page.rateLevel() == QStringLiteral("none"),
          "…and the indicator shows no verdict at all rather than a zero percent one");

    // ---- language ----
    // Headers and verdict words are derived from the catalog rather than tagged on a widget, so they are
    // the part a language switch can leave behind. The page is asked to re-read them, and does.
    LanguageManager::instance().setCode(QStringLiteral("en"));
    page.retranslateUi();
    check(model->headerData(colVerdict, Qt::Horizontal, Qt::DisplayRole).toString()
              == QStringLiteral("Status"),
          "the header row follows the language");
    store.appendResult(QStringLiteral("nfqws"), QStringLiteral("en.example"), true, 5, true, t0);
    check(model->data(model->index(0, colVerdict), Qt::DisplayRole).toString()
              == QStringLiteral("Passed"),
          "…and so does a row's verdict word, on a row the page was not rebuilt for");
    check(!page.showsEmptyNotice(), "the one new row is enough to put the table back");
    check(page.summaryBadgesVisible(), "…and the chips came back with it");
    check(page.badgeKind(page.failedBadge()) == QStringLiteral("neutral"),
          "a record with nothing failed takes the red off the failure chip");
    check(page.badgeKind(page.certBadge()) == QStringLiteral("neutral"),
          "…and the amber off the certificate one");
    LanguageManager::instance().setCode(QStringLiteral("ru"));
    page.retranslateUi();
    check(model->headerData(colVerdict, Qt::Horizontal, Qt::DisplayRole).toString()
              == QStringLiteral("Статус"), "switching back re-says them again");
    check(page.headline()->text() == LanguageManager::tr(QStringLiteral("history.headline"),
                                                        QStringLiteral("100")),
          "the summary re-says itself in the new language too");

    // Every text-bearing control on the page carries a catalog key — the contract the window's
    // retranslate walk reads. A button with a hardcoded sentence on it is invisible to a language switch
    // and to this check.
    const auto buttons = page.findChildren<QPushButton *>();
    int tagged = 0;
    for (QPushButton *b : buttons) {
        const QString key = b->property("i18nKey").toString();
        if (key.isEmpty())
            continue;
        ++tagged;
        check(LanguageManager::keys().contains(key),
              QStringLiteral("history page button key '%1' is in the catalog").arg(key));
        check(b->text() == LanguageManager::tr(key),
              QStringLiteral("history page button '%1' says what its key says").arg(key));
    }
    check(tagged >= 1, "the page's own controls are tagged for retranslation");

    // ---- the indicator's bands ----
    // Where the headline's colour turns is a policy of its own, and it is the policy the whole summary
    // exists to state: a bypass that passes one check in two is not working, and painting it green
    // because the bar moved at all is the page lying about the one number it was built to show.
    check(page.rateLevel() == QStringLiteral("good"), "one clean check in one is the healthy band");
    store.appendResult(QStringLiteral("nfqws"), QStringLiteral("bad.example"), false, -1, true,
                       t0.addSecs(600));
    check(page.rateLevel() == QStringLiteral("poor"),
          "…and it falls to the failing band at half, not at zero");
    store.appendResult(QStringLiteral("nfqws"), QStringLiteral("ok.example"), true, 7, true,
                       t0.addSecs(660));
    check(page.rateLevel() == QStringLiteral("fair"),
          "two of three is its own amber band, because it is neither working nor broken");
}

// --------------------------------------------------------------------- Motion ----
//
// The three kinds of movement and the one switch over them, each checked the same way round: the motion
// is running when it should be, it has STOPPED and cleaned up when it should not, and the widget is left
// where its layout wanted it. That last part is the difference between an animation and an animation
// that broke the screen, and it is invisible to any check that only asks whether one started.
//
// The window's own paths — setCurrentPage, populateCards, renderStatus — are exercised in the watchdog
// section below, which is the one place a real window is built.
static void testMotion()
{
    std::printf("\n[motion]\n");

    const bool wasEnabled = Motion::enabled();
    Motion::setEnabled(true);

    check(Motion::enabled(), "motion is on by default: the design was drawn moving");
    Motion::setEnabled(false);
    check(!Motion::enabled(), "and the switch is readable back");
    Motion::setEnabled(true);

    // ---- a page appearing ----
    {
        QWidget page;
        page.resize(320, 200);
        page.show();
        QTest::qWait(10);
        check(page.graphicsEffect() == nullptr, "a widget nobody touched carries no effect");

        Motion::fadeIn(&page, 120);
        check(Motion::isMoving(&page), "fadeIn starts a run on the widget it was given");
        auto *fx = qobject_cast<QGraphicsOpacityEffect *>(page.graphicsEffect());
        check(fx != nullptr,
              "…driven by an opacity effect: QWidget::windowOpacity is a window property and would fade "
              "the whole application rather than the page inside it");
        QTest::qWait(60);
        check(fx && fx->opacity() > 0.0 && fx->opacity() <= 1.0,
              QStringLiteral("…whose opacity is actually moving (%1)").arg(fx ? fx->opacity() : -1.0));
        QTest::qWait(150);
        check(!Motion::isMoving(&page), "…which ends by itself");
        check(page.graphicsEffect() == nullptr,
              "…and detaches: an effect left attached would tax every repaint the page goes on doing");
        check(!Motion::isMoving(nullptr), "a null widget is simply not moving");
        page.hide();
    }

    // ---- a row rising into place ----
    {
        QWidget host;
        auto *lay = new QVBoxLayout(&host);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        auto *one = new QWidget(&host);
        auto *two = new QWidget(&host);
        one->setFixedSize(300, 52);
        two->setFixedSize(300, 52);
        lay->addWidget(one);
        lay->addWidget(two);
        host.show();
        QTest::qWait(10);
        const QPoint oneRest = one->pos();
        const QPoint twoRest = two->pos();
        check(twoRest.y() > oneRest.y(), "the fixture layout stacked its two rows");

        Motion::riseIn(one);
        Motion::riseIn(two, Motion::kCardStepMs);
        check(one->pos() == oneRest + QPoint(0, Motion::kCardLiftPx),
              "a rising row starts below the slot the layout gave it");
        check(Motion::isMoving(one) && Motion::isMoving(two),
              "…and a staggered row is staged from the same moment rather than starting late");
        QTest::qWait(Motion::kCardRiseMs + Motion::kCardStepMs + 150);
        check(one->pos() == oneRest && two->pos() == twoRest,
              "…and both land exactly where the layout said, not 14 px short of it");
        check(one->graphicsEffect() == nullptr && two->graphicsEffect() == nullptr,
              "…with no effect left behind on either row");
        host.hide();
    }

    // ---- off means nothing moves, and nothing is installed in order not to ----
    {
        QWidget page;
        auto *lay = new QVBoxLayout(&page);
        auto *row = new QWidget(&page);
        row->setFixedSize(200, 40);
        lay->addWidget(row);
        page.show();
        QTest::qWait(10);
        const QPoint rest = row->pos();

        Motion::setEnabled(false);
        Motion::fadeIn(&page);
        Motion::riseIn(row);
        check(page.graphicsEffect() == nullptr && row->graphicsEffect() == nullptr,
              "with motion off, neither entrance installs an effect at all");
        check(!Motion::isMoving(&page) && !Motion::isMoving(row), "…and neither is left running");
        check(row->pos() == rest, "…and the row was never moved, so there is nothing to put back");

        // The mid-flight case: unchecking the box lands what was already in the air.
        Motion::setEnabled(true);
        Motion::riseIn(row, 0, 600);
        check(Motion::isMoving(row), "a long rise is in flight");
        Motion::setEnabled(false);
        check(!Motion::isMoving(row), "unchecking motion stops it where it stands");
        check(row->pos() == rest && row->graphicsEffect() == nullptr,
              "…and puts the row back with the effect off, rather than parking it mid-slide");
        Motion::setEnabled(true);
        page.hide();
    }

    // ---- the power button's breath ----
    {
        PowerButton button;
        button.show();
        QTest::qWait(10);     // the same rule as above: hidden means still, so show it first
        check(!button.isPulsing(), "an idle button does not breathe");
        check(button.graphicsEffect() == nullptr, "…and carries no effect at all");

        button.setPulsing(true);
        check(button.isPulsing(), "a live run breathes the whole control");
        check(qobject_cast<QGraphicsOpacityEffect *>(button.graphicsEffect()) != nullptr,
              "…as an opacity effect, the only thing that dims ring, halo and glyph by one factor");
        QTest::qWait(300);
        const qreal level = Motion::breathLevel(&button);
        check(level > 0.0 && level <= 1.0, QStringLiteral("the breath stays in range (%1)").arg(level));
        check(level < 1.0, "…and is actually below full, which is what makes it a pulse rather than a poke");

        button.setPulsing(false);
        check(!button.isPulsing(), "stopping the run stops the breath");
        check(button.graphicsEffect() == nullptr,
              "…taking the effect off, so the ring goes back through the fast paint path");
        check(qFuzzyCompare(button.glow(), 1.0), "…and leaves no layer parked at a dimmed value");

        // One rhythm at a time: while the button breathes, the halo holds still.
        button.setPulsing(true);
        check(qFuzzyCompare(button.glow(), 1.0),
              "a breathing button's halo does not breathe alongside it on a second period");
        Motion::setEnabled(false);
        button.setPulsing(true);        // what the next renderStatus() does under the new policy
        check(!button.isPulsing(), "with motion off, the same state leaves the button still");
        check(button.graphicsEffect() == nullptr, "…and no effect was installed in order to be skipped");
        Motion::setEnabled(true);
        button.setPulsing(true);
        check(button.isPulsing(), "…and switching motion back on brings the pulse back");
        button.setPulsing(false);
    }

    // ---- the halo breath belongs to a transition, and to the same switch ----
    {
        PowerButton button;
        Motion::setEnabled(true);
        button.show();
        QTest::qWait(10);     // the same rule as above: a widget never put on screen stays still
        button.setMode(PowerButton::Mode::Busy);
        QTest::qWait(300);
        check(button.glow() < 1.0 && button.glow() > 0.0,
              "a transition in flight breathes the halo");
        Motion::setEnabled(false);
        button.setPulsing(false);       // the window reconciles on every settings change
        check(qFuzzyCompare(button.glow(), 1.0),
              "…and the motion switch parks it at full rather than leaving it half-dimmed");
        Motion::setEnabled(true);
        button.setMode(PowerButton::Mode::Idle);
        check(qFuzzyCompare(button.glow(), 1.0), "an idle button is a still one");
        button.hide();
    }

    // ---- the settings page owns the switch ----
    QTemporaryDir dir;
    check(dir.isValid(), "temporary store for the motion checkbox");
    Settings store(dir.filePath(QStringLiteral("muffin-motion.ini")));
    check(store.animationsEnabled(), "an unset store means motion on");
    {
        SettingsPage settingsPage(store);
        QCheckBox *box = settingsPage.animationsField();
        check(box != nullptr, "the settings page carries the motion checkbox");
        check(box && box->isChecked(), "…ticked, because the model says so");
        check(box && box->text() == LanguageManager::tr(QStringLiteral("settings.animations")),
              "…labelled from the catalog rather than from a literal written into the page");
        check(box && box->toolTip() == LanguageManager::tr(QStringLiteral("settings.animations_tip")),
              "…and its tooltip came from the catalog too");
        if (box)
            box->setChecked(false);
        settingsPage.saveButton()->click();
        check(!store.animationsEnabled(), "Save writes an untick through the model");
        check(box && !box->isChecked(), "…and the commit did not re-read the form over the tick");
        store.setAnimationsEnabled(true);        // a write from anywhere else
        check(box && box->isChecked(), "an external write re-reads into the box");
    }

    Motion::setEnabled(wasEnabled);
}

// ------------------------------------------------------------------- Watchdog ----
//
// The quiet bypass check: a timer asks one host whether the live profile still gets through, and a FAIL
// turns into a red journal line, a balloon and an offer to switch. The deciding half is pure and is
// checked as such; the acting half is checked on a real window, because what it does depends on the
// catalog, on the row the power button points at, and on the journal that has to say it out loud.
//
// Three things in that path need something a test suite must not have — a network (the engine's probe),
// a desktop (the modal), a root prompt (the backend's stop/start) — and all three are MainWindow's
// virtual seams, overridden below. Everything between them is the production code.

namespace {

// A verdict map in the engine's own JSON shape, so the watchdog is handed exactly what testFinished
// would hand it: one entry per host, `success` the only field the decision reads.
QJsonObject watchdogVerdicts(const QStringList &hosts, const QStringList &through)
{
    QJsonObject out;
    for (const QString &host : hosts) {
        HostCheckResult r;
        r.success = through.contains(host);
        r.attempts = 3;
        r.attemptsOk = r.success ? 3 : 0;
        r.avgLatency = r.success ? 40 : -1;
        r.ttfbMs = r.success ? 12 : -1;
        r.lastError = r.success ? QString() : QStringLiteral("connection timed out");
        out.insert(host, BlockcheckEngine::resultToJson(r));
    }
    return out;
}

// The row id the window uses for a strategy: the parser's name when it has one, the file when it does
// not. The same expression populateCards() and watchdogNextStrategyId() use.
QString watchdogRowId(const Strategy &s)
{
    return s.id.isEmpty() ? s.filePath : s.id;
}

// A linux tree the parser names two profiles in: the sysv/openrc init script — which survives the scan
// only because <root>/config exists, since an unconfigured tree loses its InitScript card — and the
// portable nfqws payload synthesised from binaries/<arch>/nfqws. Two rows is the point: with one profile
// in the list there is nothing to rotate to, and the whole prompt path would be unreachable.
bool makeWatchdogTree(const QString &root)
{
    const QString initBody = QStringLiteral(
        "#!/bin/sh\ncase \"$1\" in\n  start) echo starting ;;\n  stop) echo stopping ;;\nesac\n");
    bool ok = true;
    ok &= writeTextFile(root + QLatin1String("/init.d/sysv/zapret"), initBody);
    ok &= writeTextFile(root + QLatin1String("/init.d/openrc/zapret"), initBody);
    ok &= writeTextFile(root + QLatin1String("/config.default"),
                        QStringLiteral("#!/bin/sh\n# options\n"));
    ok &= writeTextFile(root + QLatin1String("/config"), QStringLiteral("MODE_FILTER=none\n"));
    const QString arch = ZapretPaths::nativeArchDirName();
    ok &= writeTextFile(root + QLatin1String("/binaries/")
                            + (arch.isEmpty() ? QStringLiteral("linux-x86_64") : arch)
                            + QLatin1String("/nfqws"),
                        QStringLiteral("fixture payload\n"));
    return ok;
}

// The window under test: the same class, with its three answers replaced.
class WatchdogProbe : public MainWindow
{
public:
    using MainWindow::MainWindow;

    int probes = 0;
    int confirms = 0;
    bool answerYes = true;
    QStringList confirmPairs;   // "from|next", one per question asked
    QStringList performed;      // "from|next", one per rotation actually carried out

protected:
    void startWatchdogProbe(const Strategy &strategy) override
    {
        ++probes;
        // Not the base: the base drives the shared engine, which would bring a profile up through
        // pkexec and probe the real internet from a test binary. The verdict is fed to
        // onWatchdogResult() by hand instead, which is where the base's cycle would have landed.
    }

    bool confirmWatchdogRotation(const QString &fromName, const QString &nextName) override
    {
        ++confirms;
        confirmPairs << (fromName + QLatin1Char('|') + nextName);
        return answerYes;   // no modal anywhere in this suite — nothing out here could answer one
    }

    void performWatchdogRotation(const QString &fromId, const QString &nextId) override
    {
        performed << (fromId + QLatin1Char('|') + nextId);
        // Again not the base: the real act stops a live payload and starts another, both through the
        // backend. The intent it replaces is what watchdogRotationRequested has already recorded.
    }
};

} // namespace

// True when the harness has redirected the config location into the build tree. A window builds its own
// Settings from the process identity and writes the path it locates into that store, so a section that
// constructs one runs only where the redirect took effect — requiring the path to sit under the binary's
// own directory is what makes that safe rather than merely likely: a developer running the suite from a
// shell where XDG_CONFIG_HOME is just ~/.config has that variable set, and a check against it alone would
// let the test write into their real config home. Anywhere else the section skips.
//
// Spelled once because two sections need it: the watchdog's acting half and the release badge.
static bool configRedirectedIntoBuildTree()
{
    const QString buildTree = QCoreApplication::applicationDirPath();
    const QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString configBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir().mkpath(xdgConfig);   // the store's directory is created on its first write anyway
    const QString canonicalXdg = QDir(xdgConfig).canonicalPath();
    const QString canonicalBuild = QDir(buildTree).canonicalPath();
    return !xdgConfig.isEmpty() && !canonicalXdg.isEmpty()
           && QDir::cleanPath(configBase) == QDir::cleanPath(xdgConfig)
           && canonicalXdg.startsWith(canonicalBuild + QLatin1Char('/'));
}

static void testWatchdog()
{
    std::printf("\n[Watchdog]\n");

    using S = IZapretBackend::State;

    // ---- the one host, and the cadence ----
    const QString quick = MainWindow::watchdogQuickHost();
    check(quick == QLatin1String("discord.com"), "the quiet check probes one quick host");
    check(MainWindow::profilePingHosts().contains(quick),
          "and it is a service the profile rows promise to unblock");
    check(quick != QLatin1String("google.com"),
          "…not the baseline, which answers happily with no bypass at all");

    check(MainWindow::watchdogIntervalMs(5) == 300000, "the default five minutes is five minutes");
    check(MainWindow::watchdogIntervalMs(Settings::kWatchdogDefaultIntervalMin) == 300000,
          "and the window's default is the model's default, spelled once");
    check(MainWindow::watchdogIntervalMs(1) == 60000, "the floor is a minute");
    check(MainWindow::watchdogIntervalMs(60) == 3600000, "the ceiling is an hour");
    check(MainWindow::watchdogIntervalMs(0) == 60000, "a stored zero is not a timer that never rests");
    check(MainWindow::watchdogIntervalMs(-9) == 60000, "nor is a negative one");
    check(MainWindow::watchdogIntervalMs(100000) == 3600000,
          "and a hand-edited huge value is an hour, not a check that never happens");

    // ---- when a tick may measure ----
    check(MainWindow::watchdogShouldProbe(S::Running, false, false), "a live run is re-checked");
    check(!MainWindow::watchdogShouldProbe(S::Running, true, false),
          "a tick does not stack on the quiet cycle it started");
    check(!MainWindow::watchdogShouldProbe(S::Running, false, true),
          "…nor on the round the user pressed");
    check(!MainWindow::watchdogShouldProbe(S::Stopped, false, false),
          "a stopped profile has nothing for the answer to describe");
    check(!MainWindow::watchdogShouldProbe(S::Starting, false, false),
          "a launch in flight has opened nothing yet");
    check(!MainWindow::watchdogShouldProbe(S::Stopping, false, false),
          "and a teardown is closing the path it would measure");
    check(!MainWindow::watchdogShouldProbe(S::Error, false, false),
          "a failed run is not a live one");

    // ---- what counts as dead ----
    const QStringList one{quick};
    check(MainWindow::watchdogCycleFailed(watchdogVerdicts(one, {})),
          "no attempt through on any host is the FAIL the watchdog exists for");
    check(!MainWindow::watchdogCycleFailed(watchdogVerdicts(one, one)),
          "the same host getting through is not");
    check(!MainWindow::watchdogCycleFailed(QJsonObject()),
          "an empty map is a broken measurement, not a dead bypass");
    const QStringList two{quick, QStringLiteral("youtube.com")};
    check(!MainWindow::watchdogCycleFailed(watchdogVerdicts(two, {QStringLiteral("youtube.com")})),
          "one living host out of two is the bypass working");
    QJsonObject bare;
    bare.insert(quick, QJsonObject{{QStringLiteral("latency"), 30}});
    check(MainWindow::watchdogCycleFailed(bare), "a verdict with no success key is not a pass");

    // ---- which profile comes next ----
    QList<Strategy> list;
    for (const char *name : {"A", "B", "C"}) {
        Strategy s;
        s.id = QLatin1String(name);
        s.name = QLatin1String(name);
        list.append(s);
    }
    Strategy unnamed;
    unnamed.filePath = QStringLiteral("/opt/zapret/linux/no-name.sh");
    QList<Strategy> withUnnamed = list;
    withUnnamed << unnamed;

    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("A")) == QLatin1String("B"),
          "the next row in the list is the one offered");
    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("B")) == QLatin1String("C"),
          "…and the one after that, the time after");
    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("C")) == QLatin1String("A"),
          "the list wraps instead of running out");
    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("Z")) == QLatin1String("A"),
          "a run that names nothing on the list gets the first row");
    check(MainWindow::watchdogNextStrategyId(list, QString()) == QLatin1String("A"),
          "…and so does a run with no name at all");
    check(MainWindow::watchdogNextStrategyId(withUnnamed, QStringLiteral("C")) == unnamed.filePath,
          "a row the parser could not name is addressed by its path");
    check(MainWindow::watchdogNextStrategyId(QList<Strategy>{list.first()}, QStringLiteral("A"))
              .isEmpty(),
          "one profile in the list is not a rotation");
    check(MainWindow::watchdogNextStrategyId(QList<Strategy>{}, QStringLiteral("A")).isEmpty(),
          "nor is an empty list");

    // ---- the acting half, on a real window ----
    //
    // See configRedirectedIntoBuildTree() for why this section, and any other that constructs a
    // MainWindow, refuses to run outside a redirected config location.
    if (!configRedirectedIntoBuildTree()) {
        std::printf("  SKIP  the window section needs XDG_CONFIG_HOME redirected into the build tree "
                    "(run it through ctest)\n");
        return;
    }

    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;   // the same store a MainWindow constructed here resolves to
    const QString savedPath = store.zapretPath();
    const QString savedLast = store.lastStrategyId();
    const bool savedStart = store.startLastOnLaunch();
    const int savedInterval = store.watchdogIntervalMin();

    store.setStartLastOnLaunch(false);   // nothing may start itself while the window is being built
    store.setLastStrategyId(QString());
    store.setWatchdogIntervalMin(7);     // a cadence only this suite sees, so the timer is checkable
    store.setZapretPath(root);

    {
        WatchdogProbe window;
        const QList<Strategy> catalog = window.manager()->strategies();
        check(catalog.size() >= 2,
              QStringLiteral("the fixture tree gives the window %1 profiles").arg(catalog.size()));
        check(window.cardCount() >= 2, "and draws a row for each of them");

        check(window.watchdogTimerActive(), "the watchdog is armed the moment the window exists");
        check(window.watchdogTimerInterval() == 7 * 60 * 1000,
              "and it beats on the stored cadence, not on a literal in the window");

        // ---- the history page, on the real window ----
        // The page follows its store by itself, so what the window has to get right is narrower and
        // worth pinning: that the page exists in the stack, that it was handed the store the window
        // records into rather than a second one of its own, and that its rail glyph reaches it.
        check(window.historyPage() != nullptr, "the window builds a history page");
        check(window.historyPage()->store() == window.historyStore(),
              "…and shows the record the window writes, not a copy of it");
        const int rowsBefore = window.historyPage()->rowCount();
        window.historyStore()->appendResult(QStringLiteral("fixture"),
                                            QStringLiteral("watch.example"), true, 11, true,
                                            QDateTime::currentDateTime());
        check(window.historyPage()->rowCount() == rowsBefore + 1,
              "a verdict recorded by the window reaches the page without anybody refreshing it");
        window.setCurrentPage(MainWindow::PageHistory);
        check(window.currentPage() == MainWindow::PageHistory,
              "the rail's history glyph has a page to switch to");
        window.showDashboard();
        check(window.currentPage() == MainWindow::PageDashboard,
              "…and the way back is the one every other full-screen page uses");

        // The other half of startWatchdog()'s rule: Settings::changed() answers every write, and the
        // one a start makes must not push the next check further out. Written through the window's own
        // model — which is the same object SettingsPage holds a reference to — because that is the only
        // way the application ever changes a setting.
        window.settingsModel()->setLastStrategyId(QStringLiteral("an-unrelated-write"));
        check(window.watchdogTimerInterval() == 7 * 60 * 1000,
              "a write that did not move the interval leaves the countdown alone");
        window.settingsModel()->setWatchdogIntervalMin(20);
        check(window.watchdogTimerInterval() == 20 * 60 * 1000,
              "a new interval re-arms the timer with the new one");

        // A tick on a stopped window must not reach the engine at all.
        window.onWatchdogTick();
        check(window.probes == 0, "a stopped window asks the engine for nothing");

        // ---- the dead bypass, answered yes ----
        QSignalSpy rotation(&window, &MainWindow::watchdogRotationRequested);
        const QString fromId = watchdogRowId(catalog.first());   // what the window points at idle
        const QString nextId = MainWindow::watchdogNextStrategyId(catalog, fromId);
        check(!nextId.isEmpty(), "the fixture list has something to rotate to");
        QString nextName;
        for (const Strategy &s : catalog)
            if (watchdogRowId(s) == nextId)
                nextName = s.name;

        const int before = window.logPanel()->storedLineCount();
        window.onWatchdogResult(watchdogVerdicts(one, {}));
        const QString alert = LanguageManager::tr(QStringLiteral("watchdog.alert"));
        const QString rotating = LanguageManager::tr(QStringLiteral("watchdog.rotating")).arg(nextName);

        check(window.logPanel()->storedLineCount() == before + 2,
              "one dead check costs the journal two lines — the alert, and the switch");
        check(!alert.contains(QStringLiteral("%1")), "the alert sentence needs no arguments");
        QTextBlock hit, moved;
        QTextDocument *doc = window.logPanel()->view()->document();
        for (QTextBlock b = doc->firstBlock(); b.isValid(); b = b.next()) {
            if (b.text().contains(alert))
                hit = b;
            if (b.text().contains(rotating))
                moved = b;
        }
        check(hit.isValid(), "the alert is in the journal, in the current language");
        check(hit.isValid() && firstForegroundColor(hit) == Theme::error().name(QColor::HexRgb),
              "and it is the red line, not an info row");
        check(moved.isValid(), "and the switch names the profile it is making");

        check(window.confirms == 1, "the user is asked once, not twice for one verdict");
        check(window.confirmPairs.size() == 1
                  && window.confirmPairs.first()
                         == catalog.first().name + QLatin1Char('|') + nextName,
              "the question names the profile that died and the one being offered");
        check(rotation.count() == 1, "a yes is recorded as the intent to switch profiles");
        if (rotation.count() == 1) {
            check(rotation.at(0).size() == 2, "…carried as the pair of ids");
            check(rotation.at(0).at(0).toString() == fromId, "from the profile whose check failed");
            check(rotation.at(0).at(1).toString() == nextId, "to the next row in the list");
        }
        check(window.performed.size() == 1 && window.performed.first()
                                        == fromId + QLatin1Char('|') + nextId,
              "and the act is asked for exactly the pair the intent named");

        // ---- the same verdict, answered no ----
        window.answerYes = false;
        const int askedBefore = window.logPanel()->storedLineCount();
        window.onWatchdogResult(watchdogVerdicts(one, {}));
        check(window.confirms == 2, "a second dead check asks again");
        check(rotation.count() == 1, "a no is not a switch: nothing more was requested");
        check(window.performed.size() == 1, "…and nothing was performed");
        check(window.logPanel()->storedLineCount() == askedBefore + 1,
              "the alert is still given when the answer is no");

        // ---- a pass, and a cycle that produced nothing: silence ----
        const int silentBefore = window.logPanel()->storedLineCount();
        window.onWatchdogResult(watchdogVerdicts(one, one));
        window.onWatchdogResult(QJsonObject());
        check(window.logPanel()->storedLineCount() == silentBefore,
              "a passing check and a broken one leave the journal untouched");
        check(window.confirms == 2, "neither asks the user anything");
        check(rotation.count() == 1, "…and neither switches a profile");

        // ---- motion, on a window that is actually on screen ----
        //
        // Everything above drives a window nobody ever showed, which is the only honest way to test a
        // journal and a watchdog without a desktop. Motion refuses a hidden widget — a page behind
        // another page has nothing to appear on — so the fade, the cascade and the pulse can only be
        // watched on a window that is on screen, and under the offscreen platform that is a call rather
        // than a wish. This is also the only place the window's own three paths (setCurrentPage,
        // populateCards, renderStatus) are checked rather than the primitives they call.
        const bool motionWasOn = Motion::enabled();
        Motion::setEnabled(true);
        window.show();
        QTest::qWait(40);

        // The rows' resting places, taken while nothing animates them, so the entrance is measured
        // against the layout's own answer rather than against a position it had not decided yet.
        Motion::setEnabled(false);
        window.setCurrentPage(MainWindow::PageDashboard);
        window.setCurrentPage(MainWindow::PageProfiles);
        QTest::qWait(40);
        const QList<ProfileCard *> rows = window.cardRows();
        QVector<QPoint> resting;
        for (ProfileCard *card : rows)
            resting.append(card->pos());
        check(rows.size() >= 2,
              QStringLiteral("the fixture list gives the entrance %1 rows to move").arg(rows.size()));
        check(resting.size() >= 2 && resting.at(1).y() > resting.at(0).y(),
              "the list laid its rows out one under the other before anything was asked to move");
        bool installed = false;
        for (ProfileCard *card : rows)
            installed = installed || card->graphicsEffect() != nullptr;
        check(!installed && !Motion::isMoving(window.currentPageWidget()),
              "opening a page with motion off installs nothing and leaves nothing running");

        Motion::setEnabled(true);
        window.setCurrentPage(MainWindow::PageDashboard);
        QWidget *shown = window.currentPageWidget();
        const QPoint risen = shown->pos();
        check(Motion::isMoving(shown),
              "opening a page animates the widget the stack is now showing, not the index it was given");
        QTest::qWait(Motion::kPageMoveMs + 120);
        // The rise, measured rather than assumed: a page that only faded would be at its resting place at
        // both samples, and one that had risen and not come back would not be at it at the second.
        check(risen.y() == shown->pos().y() + Motion::kPageLiftPx
                  && risen.x() == shown->pos().x(),
              QStringLiteral("…and it arrives from %1 px below, travelling only on the vertical axis "
                             "(started at %2, ended at %3)")
                  .arg(Motion::kPageLiftPx).arg(risen.x()).arg(risen.y())
                  .arg(shown->pos().x()).arg(shown->pos().y()));
        check(!Motion::isMoving(shown) && shown->graphicsEffect() == nullptr,
              "…which ends by itself and takes the effect off, so a page that appeared once stops "
              "paying for it on every later repaint");

        window.setCurrentPage(MainWindow::PageProfiles);
        int rising = 0;
        for (ProfileCard *card : std::as_const(rows))
            rising += Motion::isMoving(card) ? 1 : 0;
        check(rising == qMin(rows.size(), Motion::kCardRiseRows),
              QStringLiteral("%1 of the list's %2 rows rise into place (the cap is %3)")
                  .arg(rising)
                  .arg(rows.size())
                  .arg(Motion::kCardRiseRows));
        QTest::qWait(Motion::kCardRiseMs + Motion::kCardStepMs * Motion::kCardRiseRows + 250);
        int landed = 0;
        for (int i = 0; i < rows.size(); ++i) {
            if (!Motion::isMoving(rows.at(i)) && rows.at(i)->pos() == resting.value(i)
                    && rows.at(i)->graphicsEffect() == nullptr)
                ++landed;
        }
        check(landed == rows.size(),
              "every row finished exactly where the layout had put it with motion off, effect gone — "
              "an entrance that parked a row 14 px low would break the list long after it looked right");

        // The pulse is the window's claim about a backend state, so it is checked through the window.
        window.setCurrentPage(MainWindow::PageDashboard);
        QTest::qWait(Motion::kPageMoveMs + 60);
        PowerButton *power = window.powerButton();
        check(power != nullptr, "the window exposes the button the pulse belongs to");
        window.renderStatusFor(IZapretBackend::State::Running);
        check(power->mode() == PowerButton::Mode::Active && power->isPulsing(),
              "a run that is up breathes the whole control");
        window.renderStatusFor(IZapretBackend::State::Stopping);
        check(power->mode() == PowerButton::Mode::Busy && power->isPulsing(),
              "a teardown breathes too — it is a run coming down, not an absence of one");
        window.renderStatusFor(IZapretBackend::State::Starting);
        check(power->mode() == PowerButton::Mode::Busy && !power->isPulsing(),
              "a launch in flight has no run to be alive yet, so the ring breathes instead of the button");
        QTest::qWait(300);
        check(power->glow() < 1.0, "…and that ring is measurably mid-breath, not merely started");
        window.renderStatusFor(IZapretBackend::State::Stopped);
        check(!power->isPulsing() && power->graphicsEffect() == nullptr
                  && qFuzzyCompare(power->glow(), 1.0),
              "an idle machine is a still button at full brightness: the pulse must not contradict the "
              "word the chip is showing");
        window.hide();
        Motion::setEnabled(motionWasOn);

        store.setLastStrategyId(QString());   // do not leave the fixture's route to a later run
    }

    store.setZapretPath(savedPath);
    store.setLastStrategyId(savedLast);
    store.setStartLastOnLaunch(savedStart);
    store.setWatchdogIntervalMin(savedInterval);
}

// ------------------------------------------------ WARP enrolment wiring ----
//
// testCombinedPower() pins what a combined press means; it cannot pin that the window performs it.
// The claim worth checking is the one a state machine gets wrong in silence: the tunnel is asked to
// connect only after this machine's Cloudflare enrolment has been settled, never instead of it —
// because warp-cli accepts a connect from a client it has no registration for, never comes up, and
// reports the same "not registered" line on every poll, so the button reads idle instead of stuck.
//
// Everything below therefore drives a real MainWindow whose tunnel control answers rather than acts.
// What is observed is the window's own sequence of calls, in the order it made them: the virtual
// instance surface of WarpManager is what makes those calls countable, and MainWindow's
// two-argument constructor is what keeps the control — rather than a warp-cli driver reaching for a
// daemon — inside a test binary.

namespace {

// One ordered record, shared by a window and its control.
//
// An ordering claim needs the two objects' acts interleaved. Two counters that happen to agree would
// also have agreed about a connect issued before the enrolment was asked for, which is the one case
// worth failing, so both objects write into the same list as they are called.
struct ActLog
{
    QStringList acts;
    void record(const QString &act) { acts << act; }
};

// The tunnel control under test: the real class, with every verb replaced by a count and every
// answer handed out by the scenario that needs it. Nothing here spawns warp-cli, and nothing here
// needs a daemon, a network, or a machine that has Cloudflare's client installed at all.
class CountingWarpManager : public WarpManager
{
public:
    explicit CountingWarpManager(QObject *parent = nullptr) : WarpManager(parent) {}

    ActLog *log = nullptr;
    // What the window is told about this host and this tunnel. One flag under two names, as the
    // control it replaces insists on: a mock that disagreed between "is it drivable" and "is it
    // there" would be a toggle offering to install a client it says it can already drive.
    bool available = true;
    State reported = State::Disconnected;
    // Whether the enrolment check reports that it started. False is the answer of a host with no
    // warp-cli: the caller is told why in the error and owes the check nothing further.
    bool enrolmentStarts = true;
    // The enrolment reading the window is told, which is what decides whether lighting the tunnel half
    // means "ask the daemon" or "the machine is not set up yet, walk the wizard". Unknown by default,
    // because that is what a manager that has never been asked reports — and a mock that claimed
    // NotRegistered out of the box would send every existing scenario down the wizard path.
    Registration registration = Registration::Unknown;

    int ensureCalls = 0;
    int prepareCalls = 0;
    int rescans = 0;
    // The vendor's window, counted rather than run — and overriding this one is not a convenience but a
    // safety requirement: the base asks a real systemd manager to mask a unit and a real pkill to kill a
    // process, which are the two things a suite must never do to the machine running it. A scenario that
    // wants the real chain runs it through the statics in coretest instead, where nothing is spawned.
    int suppressCalls = 0;
    // The reverse direction, counted for the same reason: the base asks a real systemctl to unmask and
    // re-enable the taskbar's unit, which is a change to the developer's session made from a test.
    int restoreCalls = 0;
    // What a rescan "finds". Empty by default, which is the honest answer for a control that has never
    // resolved anything; a scenario that watches the sentence the wizard prints sets it.
    QString programPath;
    int startCalls = 0;
    int stopCalls = 0;
    int refreshCalls = 0;
    bool polling = false;
    // Whether the preparation can be begun at all, and whether it answers on the spot. The second flag
    // defaults to yes because the ordering scenarios here are about *which acts ran in which order*,
    // and a preparation that completes inside its own call keeps every one of those assertions about
    // the press intact. A scenario that wants to watch the gap between the two answers turns it off.
    bool preparationStarts = true;
    bool preparationAnswersImmediately = true;

    // Overridden without default arguments on purpose: the base's defaults belong to the base, and a
    // caller reaching this object through a WarpManager* keeps taking them from there.
    State state() const override { return reported; }
    bool isAvailable() const override { return available; }
    bool isInstalled() const override { return available; }
    bool isPollingEnabled() const override { return polling; }
    Registration registrationState() const override { return registration; }
    QString resolvedProgram() const override { return programPath; }

    // Recorded like the verbs, because what the wizard's second step depends on is not that a rescan
    // happened but that it happened BEFORE the enrolment was asked for.
    void rescanAvailability() override
    {
        ++rescans;
        if (log)
            log->record(QStringLiteral("rescanAvailability"));
    }

    bool ensureRegistered(QString *error) override
    {
        ++ensureCalls;
        if (log)
            log->record(QStringLiteral("ensureRegistered"));
        if (!enrolmentStarts) {
            if (error)
                *error = QStringLiteral("warp-cli is not installed on this host");
            return false;
        }
        return true;
    }

    // The preparation between the enrolment and the connect. Counted like the two verbs either side of
    // it, because what the scenarios assert is that a tunnel is asked for after the machine was made
    // ready — so the default answer is "ready, immediately", and the one scenario that wants the gap
    // turns that off and hands out preparationFinished() itself.
    bool prepareEnvironment(QString *error) override
    {
        ++prepareCalls;
        if (log)
            log->record(QStringLiteral("prepareEnvironment"));
        if (!preparationStarts) {
            if (error)
                *error = QStringLiteral("warp-cli is not installed on this host");
            return false;
        }
        if (preparationAnswersImmediately)
            emit preparationFinished(true);
        return true;
    }

    bool start(QString *error) override
    {
        Q_UNUSED(error);
        ++startCalls;
        if (log)
            log->record(QStringLiteral("start"));
        // The state a real connect would move to, so the repaint it triggers redraws from the answer
        // a caller would have got rather than from one nobody asked for.
        reported = State::Connecting;
        emit stateChanged(reported);
        return true;
    }

    bool stop(QString *error) override
    {
        Q_UNUSED(error);
        ++stopCalls;
        if (log)
            log->record(QStringLiteral("stop"));
        return true;
    }

    bool refresh(QString *error) override
    {
        Q_UNUSED(error);
        ++refreshCalls;
        return true;
    }

    // Recorded, never run — see suppressCalls above for what running it would do to this machine.
    void suppressWarpGui() override
    {
        ++suppressCalls;
        if (log)
            log->record(QStringLiteral("suppressWarpGui"));
    }

    void restoreWarpGui() override
    {
        ++restoreCalls;
        if (log)
            log->record(QStringLiteral("restoreWarpGui"));
    }

    // Takes the flag and nothing else. The base's implementation arms a status timer, and a control
    // that let it would have a poll firing out from under the scenario that was counting calls.
    void setPollingEnabled(bool on) override { polling = on; }

    // The two answers the real object gives from the asynchronous show -> delete -> new sequence,
    // handed out by hand. A scenario decides when the enrolment has settled; nothing here waits.
    void answerReading(Registration reading) { emit registrationChanged(reading); }
    void answerFinished(bool ok) { emit registrationFinished(ok); }
    // The second of the two answers a connect waits on, for the scenario that turned
    // preparationAnswersImmediately off.
    void answerPreparation(bool ok) { emit preparationFinished(ok); }
    // The stage announcements, handed out the same way the two verdicts are.
    void answerStage(Stage stage) { emit stageChanged(stage); }
    // The journal channel, for the scenarios where the reason behind a verdict is the thing under test:
    // registrationFinished() carries a bool and nothing else, so a failure's own words arrive here.
    void answerLog(const QString &line, int level) { emit logMessage(line, level); }
    // The daemon's own answer, for a scenario that needs the window to repaint from a state rather than
    // to cause one. Emitted here because a signal of another object cannot be raised from outside it.
    void emitState(State s) { reported = s; emit stateChanged(s); }
};

// The window under test: the same class, with the acts that would need a payload, a root prompt or a
// modal replaced by a record, and its tunnel control handed in rather than built.
class EnrolmentProbe : public MainWindow
{
public:
    EnrolmentProbe(WarpManager *warp, QWidget *parent = nullptr) : MainWindow(warp, parent) {}

    ActLog *log = nullptr;
    int bypassStarts = 0;
    // Counted rather than recorded in the act log, because the log's exact sequences are other
    // scenarios' claims and connectWarp() is the act those sequences already spell as the enrolment it
    // delegates to. What this counter adds is the name the report used: one press, both halves.
    int connectCalls = 0;
    int installsOffered = 0;
    int setupsOpened = 0;
    // Whether the bypass is up for the wait's guard. A press records the request; this is what the
    // delay exists to wait for, and the guard is what makes the delay a condition rather than a pause.
    bool bypassHeld = true;
    // Counted, not run: the base of this one reaches a real backend's teardown, and what the install
    // scenarios claim is that the window decided to give the bypass back — the stop itself is the
    // button's own path and is measured where it is tested.
    int bypassStops = 0;

protected:
    void takeDownBorrowedBypass() override { ++bypassStops; }

    bool startBypass() override
    {
        ++bypassStarts;
        if (log)
            log->record(QStringLiteral("startBypass"));
        // Not the base: that brings a profile up through the backend, which is a pkexec prompt and a
        // payload. The press only needs to know the launch was accepted for the wait to be armed.
        return true;
    }

    bool bypassIsUpOrRising() const override { return bypassHeld; }

    void connectWarp() override
    {
        ++connectCalls;
        MainWindow::connectWarp();   // the base is the thing under test; only the ask is counted here
    }

    void offerWarpInstall() override
    {
        ++installsOffered;
        // Not the base: that installs software as root.
    }

    // Not the base either, and for a sharper reason: the base opens a modal dialog that runs a package
    // manager and waits for a password, which in a headless suite is a test that never returns.
    void openWarpSetup() override
    {
        ++setupsOpened;
        if (log)
            log->record(QStringLiteral("openWarpSetup"));
    }
};

// A window and its counting control, built fresh for one scenario and taken down with it.
//
// Fresh, because everything worth checking here lives in a per-request flag: a scenario that
// inherited another's pending enrolment would be testing a sequence that had already run. The control
// is allocated rather than a member because the window adopts it and deletes it — one owner, exactly
// as with the one it builds itself — and the window must outlive the control it owns.
//
// The store is armed on the way in for the same reason the fixture is rebuilt: the window reads its
// two halves from the store before anything can be said to it, and a scenario that ran after one
// which turned the tunnel half dark would otherwise come up with the half it had just watched go.
struct ArmBothHalves
{
    explicit ArmBothHalves(Settings &store)
    {
        store.setStartLastOnLaunch(false);   // nothing may start itself while the window is built
        store.setUseZapret(true);
        store.setUseWarp(true);
    }
};

struct EnrolmentFixture
{
    // Declared ahead of the control and the window because that is the order they are built in, and
    // the store write has to precede the window's own read of it.
    ArmBothHalves armed;
    ActLog log;
    CountingWarpManager *warp;
    EnrolmentProbe *window;

    explicit EnrolmentFixture(Settings &store)
        : armed(store), warp(new CountingWarpManager), window(new EnrolmentProbe(warp, nullptr))
    {
        warp->log = &log;
        window->log = &log;
        // The wait a combined press arms is the gap being survived, not the thing being observed:
        // three real seconds would be a test of the clock rather than of the ordering.
        window->setWarpStartDelayMs(0);
    }

    ~EnrolmentFixture() { delete window; }   // takes the control with it

    EnrolmentFixture(const EnrolmentFixture &) = delete;
    EnrolmentFixture &operator=(const EnrolmentFixture &) = delete;

    // The press, through the button rather than a call into a slot. A click is what the user gives,
    // and whether the control was even enabled for this state is part of what is being claimed. The
    // wait is for the zero-millisecond settle timer a combined press arms, which has to reach the
    // event loop to fire.
    void press()
    {
        if (PowerButton *power = window->powerButton())
            power->click();
        QTest::qWait(20);
    }
};

} // namespace

// The enrolment half of a combined start, checked through the window that has to perform it.
static void testWarpEnrolmentWiring()
{
    std::printf("\n[WARP enrolment wiring]\n");

    if (!configRedirectedIntoBuildTree()) {
        std::printf("  SKIP  the wiring belongs to a window, and constructing one needs XDG_CONFIG_HOME "
                    "redirected into the build tree (run it through ctest)\n");
        return;
    }

    // A two-profile tree for the same reason the watchdog section wants one: the press's first act is
    // asking for the selected profile, and a window with no rows to select would say so instead of
    // reaching the ordering this section is here to watch. (The act itself is overridden — nothing
    // here may reach pkexec — but the press has to have a reason to reach it.)
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;   // the same store a MainWindow constructed here resolves to
    const QString savedPath = store.zapretPath();
    const bool savedZapret = store.useZapret();
    const bool savedWarp = store.useWarp();
    const bool savedStart = store.startLastOnLaunch();

    store.setZapretPath(root);
    // The two halves each fixture comes up with is armed per fixture (see ArmBothHalves), not once
    // here: a scenario below watches the user turn the tunnel half dark, and that move is persisted
    // the moment it is agreed — the store is the window's own, not a copy the test holds apart.

    // ---- the press asks the enrolment, and asks nothing else yet ----
    {
        EnrolmentFixture f(store);
        check(f.window->warpManager() == f.warp,
              "the window drives the control it was handed, not a second one it built for itself");
        check(f.window->selection() == ModeSelection{true, true},
              "both halves came up lit, which is the only selection a combined press reads");
        check(f.warp->polling, "…and the tunnel's polling was turned on in that control");
        check(f.window->warpStartDelayMs() == 0, "…and the press's wait is set to nothing");

        f.press();
        check(f.warp->ensureCalls == 1,
              "a press with the tunnel half lit asks whether this device is enrolled");
        check(f.warp->startCalls == 0,
              "…and asks nothing of the tunnel while that question is still owed an answer");
        check(f.log.acts == (QStringList{QStringLiteral("startBypass"),
                                         QStringLiteral("ensureRegistered")}),
              "bypass first, then the enrolment check — the order the press table encodes, read off "
              "the acts rather than asserted of the table");
        // The claim the report asked about, in the words it used: one click, both halves. Nothing here
        // waits for a second press, and the second half is not merely scheduled — it has already been
        // asked for by the time the click's own settle timer has fired.
        check(f.window->bypassStarts == 1 && f.window->connectCalls == 1,
              "ONE press reached both halves of the chain: the bypass launch and the tunnel's connect "
              "path, with no second click asked of the user");
        check(f.window->warpChainPending(),
              "…and the window still knows a tunnel is owed while the enrolment is outstanding");
        check(f.window->powerButton()->mode() == PowerButton::Mode::Busy,
              "which is what the ring says: a press that owes two subsystems looks like work in flight "
              "for as long as either of them is, rather than turning finished the moment the bypass is up");
    }

    // ---- an answer that says no earns no connect ----
    {
        EnrolmentFixture f(store);
        f.press();
        check(f.window->powerButton()->mode() == PowerButton::Mode::Busy,
              "while the enrolment is outstanding the ring says a chain is in flight — the only thing on "
              "screen that does, since no manager state moves for the length of that round trip");
        // Counted from after the press, because the press writes a journal line of its own when the
        // wait lands: charging that to the answer would be measuring two claims as though they were one.
        const int linesBefore = f.window->logPanel()->storedLineCount();
        f.warp->answerFinished(false);
        check(f.warp->startCalls == 0,
              "a failed enrolment issues no connect — the tunnel this protects is the one that "
              "would have hung silently");
        check(f.window->logPanel()->storedLineCount() == linesBefore + 1
                  && f.window->logPanel()->view()->toPlainText()
                         .contains(LanguageManager::tr(QStringLiteral("warp.enroll_failed"))),
              "…and the journal says what the failure cost, which is the half a raw CLI error never "
              "carries");
        check(!f.window->warpChainPending(),
              "the answer also closes the request, so nothing is owed any more");
        check(f.window->powerButton()->mode() != PowerButton::Mode::Busy,
              "…and the ring stops saying otherwise: a button left breathing over a chain that ended is "
              "the same lie told in the other direction. The look it returns to is zapret's own — the "
              "half that did come up keeps saying so, and the table pins that as Active over a live "
              "bypass rather than as a second shade of failure");

        // The request is closed out by the answer that refused it, so nothing further is owed.
        f.warp->answerFinished(true);
        check(f.warp->startCalls == 0,
              "and a later yes does not revive a request the failed one already answered");
    }

    // ---- an answer that says yes earns exactly one ----
    {
        EnrolmentFixture f(store);
        f.press();
        f.warp->answerFinished(true);
        check(f.warp->startCalls == 1,
              "a settled enrolment is answered with the connect it was waiting for");
        check(f.log.acts == (QStringList{QStringLiteral("startBypass"),
                                         QStringLiteral("ensureRegistered"),
                                         QStringLiteral("prepareEnvironment"),
                                         QStringLiteral("start")}),
              "…and the connect came last, after the machine was made ready: the ordering is the whole "
              "claim of connectWarp(), and two counters that agreed would also have agreed about it "
              "being issued first");
        f.warp->answerFinished(true);
        check(f.warp->startCalls == 1,
              "one request earns one tunnel, not one connect per answer that happens to arrive");
    }

    // ---- the preparation is a step the user can see ----
    //
    // The scenario this fix introduces: between "this device is enrolled" and "bring the tunnel up"
    // there is now a stretch of seconds in which the program is waiting on a root prompt. Nothing on
    // the dashboard moves during it, so what is checked here is not the ordering — the scenario above
    // covers that — but whether the window has anything at all to show while the machine is being made
    // ready. A user given no answer to "what is it doing" has one sane move left: kill it.
    {
        EnrolmentFixture f(store);
        f.warp->preparationAnswersImmediately = false;   // hold the answer open, as a restart does
        f.press();
        f.warp->answerFinished(true);
        check(f.warp->prepareCalls == 1,
              "a settled enrolment is followed by a request to prepare the machine");
        check(f.warp->startCalls == 0,
              "…and no tunnel is asked for while that is still outstanding — the restart is a step "
              "rather than a hope precisely because a connect issued first is never answered");

        QLabel *footer = f.window->findChild<QLabel *>(QStringLiteral("footerHint"));
        check(footer != nullptr, "the footer is reachable by name to be read");
        const int linesBefore = f.window->logPanel()->storedLineCount();
        f.warp->answerStage(WarpManager::Stage::Service);
        const QString restarting = LanguageManager::tr(QStringLiteral("warp.prepare.service"));
        check(f.window->logPanel()->storedLineCount() == linesBefore + 1
                      && f.window->logPanel()->view()->toPlainText().contains(restarting),
              "a restart in progress is named in the journal, not left as a still button");
        check(footer && footer->text() == restarting,
              "…and on the footer, which is the part of the window the user is actually looking at");

        f.warp->answerStage(WarpManager::Stage::Idle);
        check(f.window->logPanel()->storedLineCount() == linesBefore + 1,
              "the closing Idle announces nothing — a finished preparation has no business overwriting "
              "the tunnel's own status with a fourth sentence");

        f.warp->answerPreparation(true);
        check(f.warp->startCalls == 1,
              "and the connect is issued the moment the machine is said to be ready");
    }

    // ---- a machine that could not be prepared is still asked to connect ----
    //
    // The preparation is the fix for a tunnel that will not come up, not a precondition of asking: a
    // host with no pkexec, or a user who declined the prompt, may still be a host whose daemon is
    // perfectly stuck-free. Refusing there would turn an unavailable optimisation into a tunnel that
    // cannot be started at all — and would say so only in a journal nobody had opened yet.
    {
        EnrolmentFixture f(store);
        f.warp->preparationAnswersImmediately = false;
        f.press();
        f.warp->answerFinished(true);
        const int linesBefore = f.window->logPanel()->storedLineCount();
        f.warp->answerPreparation(false);
        check(f.warp->startCalls == 1, "a preparation that could not finish still issues the connect");
        check(f.window->logPanel()->storedLineCount() == linesBefore + 1
                      && f.window->logPanel()->view()->toPlainText().contains(
                             LanguageManager::tr(QStringLiteral("warp.prepare_failed"))),
              "…and says that it is trying without having prepared, so the attempt and the skipped fix "
              "stay distinguishable after the fact");
    }

    // ---- an enrolled device goes straight to the tunnel ----
    //
    // The report this answers: turning WARP on is slow every time, because the chain restarted warp-svc
    // behind a polkit prompt and then waited for the daemon to re-open its socket. That is a
    // FIRST-INSTALL cost. On a device whose own `registration show` says Registered, a press is one
    // short-lived warp-cli verb — no prepareEnvironment(), no restart, no settle — and this is the
    // scenario that pins it.
    {
        EnrolmentFixture f(store);
        f.warp->registration = WarpManager::Registration::Registered;
        f.press();
        check(f.warp->ensureCalls == 1, "the enrolment is still read — that check is one short verb");
        f.warp->answerFinished(true);
        check(f.warp->prepareCalls == 0,
              "a registered device is never prepared: the restart belongs to the first install, not to "
              "every connect");
        check(f.warp->startCalls == 1,
              "…and the connect is issued inside the same answer, which is what makes the toggle feel "
              "instant rather than patient");
        check(f.log.acts == (QStringList{QStringLiteral("startBypass"),
                                         QStringLiteral("ensureRegistered"),
                                         QStringLiteral("start")}),
              "three acts, in that order, with no restart and no settle between the enrolment and the "
              "tunnel");
    }

    // ---- a device that is not enrolled still pays the whole chain ----
    //
    // The other side of the same policy, and the reason the gate is not simply "never prepare": a first
    // install is exactly the case the protocol switch and the daemon restart exist for, so a reading that
    // is not Registered must still go through them before the tunnel is asked for.
    {
        EnrolmentFixture f(store);
        f.warp->registration = WarpManager::Registration::NotRegistered;
        f.warp->preparationAnswersImmediately = false;
        f.press();
        f.warp->answerFinished(true);
        check(f.warp->prepareCalls == 1,
              "an unenrolled device is prepared before the tunnel is asked for — the first-install path "
              "is not lost, it is only kept off the everyday one");
        check(f.warp->startCalls == 0, "…and the connect waits for it, as it always has");
        f.warp->answerPreparation(true);
        check(f.warp->startCalls == 1, "…and follows the preparation once it is done");
    }

    // ---- the header speaks for the tunnel while the tunnel half is lit ----
    //
    // The reported bug was "the WARP button is blue but nothing connects, and the terminal says
    // warp=off". With the tunnel down the chip borrowed the bypass's word, and for a running zapret
    // that word is CONNECTED — so the one sentence on screen described the wrong subsystem while the
    // half the user had lit was the one that had failed.
    //
    // Which of these assertions carries that weight, and which cannot: a suite here cannot bring a real
    // payload up (pkexec, a live queue), so the bypass stays Stopped and a Stopped zapret's own word is
    // OFFLINE — the same word the tunnel wants when it is down. The Disconnected pair below is therefore
    // stated for completeness, and the assertion that actually bites is the Error one: it is the case
    // where the borrowed word and the true one cannot agree. Verified by reversing the fix — with the
    // old mapping this section fails on the Error check and nothing else.
    {
        EnrolmentFixture f(store);
        QLabel *chip = f.window->findChild<QLabel *>(QStringLiteral("statusText"));
        check(chip != nullptr, "the header chip is reachable by name to be read");
        if (chip) {
            f.warp->emitState(WarpManager::State::Connected);
            const QString up = chip->text();
            f.warp->emitState(WarpManager::State::Disconnected);
            check(up == QStringLiteral("CONNECTED"),
                  "a tunnel that is up is allowed the word it earns");
            check(chip->text() != up && chip->text() == QStringLiteral("OFFLINE"),
                  QStringLiteral("…and one that is not up says that instead, whatever the bypass is "
                                 "doing (the chip now reads %1)").arg(chip->text()));
            check(chip->property("statusKind").toString() == QStringLiteral("stopped"),
                  "…with the dot agreeing with the word, so the pair cannot describe two states at once");
            f.warp->emitState(WarpManager::State::Error);
            check(chip->text() == QStringLiteral("ERROR")
                          && chip->property("statusKind").toString() == QStringLiteral("error"),
                  "a failed verb is named as the failure it is, not inherited from the other layer");
        }
    }

    // ---- an answer to a request this window never made ----
    {
        EnrolmentFixture f(store);
        // Nothing was pressed, so nothing is pending: this is the guard that keeps a registration
        // finished() from some other caller's check from bringing a tunnel up here.
        f.warp->answerReading(WarpManager::Registration::Registered);
        f.warp->answerFinished(true);
        check(f.warp->ensureCalls == 0, "an idle window asks the daemon for nothing");
        check(f.warp->startCalls == 0,
              "…and an enrolment answer nobody is waiting for brings no tunnel up");

        // A reading worth mentioning is still said, even with no press behind it: the journal line
        // exists because enrolling takes seconds and a silent button reads as a stuck one.
        const int linesBefore = f.window->logPanel()->storedLineCount();
        f.warp->answerReading(WarpManager::Registration::NotRegistered);
        check(f.window->logPanel()->storedLineCount() == linesBefore + 1,
              "a device that is about to be enrolled is announced before it is");
        check(f.window->logPanel()->view()->toPlainText()
                  .contains(LanguageManager::tr(QStringLiteral("warp.enrolling"))),
              "…in the words the journal keeps for it, not in a placeholder");
        f.warp->answerReading(WarpManager::Registration::Registered);
        check(f.window->logPanel()->storedLineCount() == linesBefore + 1,
              "a device that already has an enrolment promises nothing about making one");
    }

    // ---- the user changes their mind while the check runs ----
    {
        EnrolmentFixture f(store);
        // Said here because this is the scenario that ends with the tunnel half written dark, and the
        // store is the window's own: what it persists is the next window's starting point, which is
        // why every fixture arms itself rather than trusting the last one.
        check(f.window->selection() == ModeSelection{true, true},
              "this scenario's window came up with both halves lit");
        f.press();
        check(f.warp->ensureCalls == 1, "the check is running");
        // The pill is the user's own gesture, so it goes through the widget rather than through the
        // window's private writer — a programmatic move would be the window answering itself.
        f.window->modeToggle()->setSelection(ModeSelection{true, false});
        check(f.window->selection() == ModeSelection{true, false}, "the tunnel half went dark");
        check(!store.useWarp(),
              "…the change was applied, not asked about: there is no modal between a press on the pill "
              "and the selection the store is being written");
        f.warp->answerFinished(true);
        check(f.warp->startCalls == 0,
              "a successful enrolment is no licence to connect a tunnel the user just turned off");
        check(f.warp->stopCalls == 0,
              "…and nothing that was never held is taken down on the way out");
    }

    // ---- a second press while the check runs ----
    {
        EnrolmentFixture f(store);
        f.press();
        f.press();
        check(f.warp->ensureCalls == 1,
              "one request per press, and no second enrolment stacked on the first — a device enrolled "
              "twice is a device that asked Cloudflare for two keys");
        check(f.warp->startCalls == 0, "…and still no connect while the answer is outstanding");
    }

    // ---- the bypass the wait was waiting for never came up ----
    {
        EnrolmentFixture f(store);
        f.window->bypassHeld = false;
        f.press();
        check(f.window->bypassStarts == 1, "the press did reach the launch it had to survive");
        check(f.warp->ensureCalls == 0,
              "and a wait that ends over a bypass that never held asks the daemon for nothing");
        check(f.warp->startCalls == 0, "…and certainly connects no tunnel on top of it");
    }

    // ---- a host with no client at all ----
    {
        EnrolmentFixture f(store);
        f.warp->enrolmentStarts = false;
        f.press();
        check(f.warp->ensureCalls == 1, "the press still asks");
        check(f.warp->startCalls == 0,
              "a check that could not even begin is answered with no connect and no promise of one");
        f.warp->answerFinished(true);
        check(f.warp->startCalls == 0, "…and a request that never began owes nothing to a late answer");
    }

    // ---- a tunnel this machine cannot serve ----
    {
        // Availability is read while the window is built, so this needs the control configured before
        // the ctor runs rather than after — which is also why it is built by hand rather than through
        // the fixture, whose control arrives already available.
        ArmBothHalves armed(store);   // both halves asked for; the host is what refuses one
        ActLog log;
        auto *warp = new CountingWarpManager;
        warp->available = false;
        warp->log = &log;
        EnrolmentProbe window(warp, nullptr);
        window.log = &log;
        window.setWarpStartDelayMs(0);
        check(window.selection() == ModeSelection{true, false},
              "a stored WARP half the host cannot serve comes up dark rather than unpressable");
        window.powerButton()->click();
        QTest::qWait(20);
        check(warp->ensureCalls == 0,
              "and pressing on a dark tunnel half asks no enrolment of a client that is not here");
        window.modeToggle()->setSelection(ModeSelection{true, true});
        check(window.setupsOpened == 1,
              "…while lighting that half is answered with the wizard, because on a machine with no "
              "client an install is the first of six steps and not the last");
        check(window.installsOffered == 0,
              "…and not with the install-only offer, which would do that first step and then leave the "
              "device unenrolled, unswitched and unconnected with nothing on screen to say so");
        check(warp->ensureCalls == 0, "…and not with an enrolment against nothing");
    }

    // ---- a client that is there but was never enrolled ----
    {
        EnrolmentFixture f(store);
        // The reading a manager gives before anything has asked it is Unknown, and Unknown is not
        // "not enrolled" — it is "not known yet", which the press path answers by asking. Only a reading
        // that says the device has no enrolment sends the user to the wizard.
        f.warp->registration = WarpManager::Registration::NotRegistered;
        f.window->modeToggle()->setSelection(ModeSelection{true, false});
        f.window->modeToggle()->setSelection(ModeSelection{true, true});
        check(f.window->setupsOpened == 1,
              "a client that was never enrolled gets the wizard: the missing step is the enrolment, and "
              "a connect issued without it is refused by the daemon");
    }

    // ---- a client that is there and whose enrolment has not been read ----
    {
        EnrolmentFixture f(store);
        f.window->modeToggle()->setSelection(ModeSelection{true, false});
        f.window->modeToggle()->setSelection(ModeSelection{true, true});
        check(f.window->setupsOpened == 0,
              "an unread enrolment is not an absent one — the press asks the daemon and waits for the "
              "answer instead of walking the user through a wizard it may not need");
        check(f.window->installsOffered == 0, "…and offers to install nothing over a working client");
    }

    // ---- the bypass a WARP install borrows, and gives back ----
    //
    // Cloudflare's own repository is one of the endpoints a blocked network refuses, so an install on a
    // machine that is not running a bypass needs one lent to it — and needs it handed back afterwards, or
    // Muffin has left queue rules rewriting traffic for a tunnel the user may never finish setting up.
    // Both halves are the claim; the middle one is the case where nothing is borrowed and nothing may be
    // taken down, because the user brought the path up themselves.
    {
        EnrolmentFixture f(store);
        f.warp->log = nullptr;            // this scenario reads the journal, not the act log
        f.window->bypassHeld = false;     // nothing is up to borrow
        check(!f.window->zapretAutoStarted(), "the window starts owing no bypass");

        f.window->borrowBypassForWarpInstall();
        check(f.window->bypassStarts == 1 && f.window->zapretAutoStarted(),
              "a WARP install on a machine with no bypass running starts one, and remembers that it did");
        check(f.window->logPanel()->view()->toPlainText()
                      .contains(LanguageManager::tr(QStringLiteral("warp.zapret_borrowed"))),
              "…says so in the journal, because the launch that follows costs a root prompt and the line "
              "is the only thing telling the user what that password is for");

        f.window->borrowBypassForWarpInstall();
        check(f.window->bypassStarts == 1,
              "asking twice borrows once — a second launch would be a second pkexec behind the first");

        f.window->bypassHeld = true;      // the borrowed bypass came up
        f.window->releaseBorrowedBypass();
        check(f.window->bypassStops == 1 && !f.window->zapretAutoStarted(),
              "and when the install ends the borrowed path is given back: the machine is left as it was "
              "found rather than running a bypass nobody asked to keep");
        check(f.window->logPanel()->view()->toPlainText()
                      .contains(LanguageManager::tr(QStringLiteral("warp.zapret_released"))),
              "…in the journal, which is where somebody reading an hour later looks");

        f.window->releaseBorrowedBypass();
        check(f.window->bypassStops == 1,
              "…once. A second release at the end of the wizard, after the chain already returned it at "
              "the connect, must not spend a second teardown on a bypass that is coming down");
    }
    {
        EnrolmentFixture f(store);
        f.warp->log = nullptr;
        f.window->bypassHeld = true;      // the user's own Zapret + WARP run is already up
        f.window->borrowBypassForWarpInstall();
        check(f.window->bypassStarts == 0 && !f.window->zapretAutoStarted(),
              "nothing is borrowed from a machine whose bypass is already up, and nothing is claimed as "
              "borrowed that was not");
        f.window->releaseBorrowedBypass();
        check(f.window->bypassStops == 0,
              "…and at the end of the install nothing is taken down either: a path the user brought up is "
              "not this install's collateral");
        check(f.window->logPanel()->view()->toPlainText()
                      .contains(LanguageManager::tr(QStringLiteral("warp.zapret_kept"))),
              "the keeping is said, because a journal that passes over a bypass still running after an "
              "install leaves the user to work out whether Muffin means to stop it");
    }

    // ---- the single-window policy reaches both hands that act on it ----
    //
    // The switch on the settings page is one value with two executors: WarpInstaller closes the window at
    // the end of a package run, WarpManager keeps it closed at every connect. The store owns the answer,
    // so the window has to carry it to both — once at construction, and again whenever the user moves it.
    // A hand left holding the value from the last launch is a switch that works sometimes, and the
    // "sometimes" is whichever subsystem the user happens to reach for first.
    {
        const bool savedHide = store.hideWarpGui();
        store.setHideWarpGui(false);
        EnrolmentFixture f(store);
        check(f.window->warpManager() && f.window->warpInstaller(),
              "both hands exist by the time the window is up");
        check(!f.window->warpManager()->hideWarpGui(),
              "a policy read at construction reaches the tunnel's manager");
        check(!f.window->warpInstaller()->hideWarpGui(),
              "…and the installer's copy of the same answer, which is the one an install would act on");
        check(f.warp->restoreCalls == 1,
              "a policy going from on to off hands the tray application back — the mask Muffin put on the "
              "unit is not left in place behind the user's own decision, which is what makes the switch "
              "off mean something");

        // Move the setting the way the settings page moves it — through the window's own store, because
        // `Settings::changed` is a signal of one object: a second instance pointed at the same file
        // writes the value without ever telling the window that it did.
        f.window->settingsModel()->setHideWarpGui(true);
        check(f.window->warpManager()->hideWarpGui(),
              "moving the switch re-hands it to the manager, so the next connect uses the answer the "
              "user just gave rather than the one from the last launch");
        check(f.window->warpInstaller()->hideWarpGui(),
              "…and to the installer for the same reason: an install started after this move is asked "
              "under a policy that has already changed");
        check(f.warp->restoreCalls == 1,
              "going back on asks for no restore of its own — the connect that follows is what suppresses");
        f.window->settingsModel()->setHideWarpGui(false);
        check(f.warp->restoreCalls == 2,
              "and off again is the other direction again: one transition, one undo, so a machine that "
              "was masked twice and unmasked once is not left masked");
        f.window->settingsModel()->setHideWarpGui(true);
        f.warp->log = nullptr;
        store.setHideWarpGui(savedHide);
    }

    // ---- a switch that takes a HELD half down pays the cost without asking ----
    //
    // The case the modal used to interrupt: the tunnel is up, the user turns its half dark. The pill
    // moves, the held subsystem is taken down through the window's own teardown, and nothing waits for
    // an answer — which is what the check reads, because the seam the question had is gone.
    {
        EnrolmentFixture f(store);
        f.warp->emitState(WarpManager::State::Connected);
        const int stopsBefore = f.warp->stopCalls;
        f.window->modeToggle()->setSelection(ModeSelection{true, false});
        check(f.window->selection() == ModeSelection{true, false},
              "the pill lands where the user put it, in one gesture");
        check(f.warp->stopCalls == stopsBefore + 1,
              "…and the held tunnel is brought down by that same gesture: asking is not what makes the "
              "stop safe, ordering is");
        f.warp->emitState(WarpManager::State::Disconnected);
    }

    // ---- where fetching zapret is offered now ----
    //
    // Two forms of one affordance, and the pair is the point: the loud one is the placeholder a user with
    // no zapret at all is looking at, and the quiet one is the corner of a page that already lists
    // profiles and might be out of date. Both are on the page that shows the result of the fetch, which is
    // the reason they left the path field on the settings page.
    {
        EnrolmentFixture f(store);
        QToolButton *fetch = nullptr;
        const auto tools = f.window->findChildren<QToolButton *>();
        for (QToolButton *t : tools) {
            if (t->property("role").toString() == QLatin1String("download"))
                fetch = t;
        }
        check(fetch != nullptr, "the profiles page carries the fetch icon");
        check(fetch && fetch->property("glyph").toString() == QLatin1String("download-cloud")
                      && fetch->property("glyphPx").toInt() == 16,
              "…declaring the glyph and size applyTheme() re-bakes it at, like the icons it replaced");
        check(fetch && fetch->objectName() == QLatin1String("pathToolButton")
                      && fetch->size() == QSize(32, 32)
                      && fetch->focusPolicy() == Qt::TabFocus,
              "…and drawn, sized and reachable as the settings page's path icons are, so one sheet rule "
              "and one Tab order cover all of them");
        check(fetch && !fetch->toolTip().isEmpty() && !fetch->accessibleName().isEmpty(),
              "…and wordless is not the same as unlabelled");
        // The corner gained an icon in a row that already carries a back button, a title and two text
        // buttons, in a window of a fixed width. Overflow there clips one of them with no warning to
        // anybody, so it is measured: both corner icons have to be inside the page they are laid into.
        QWidget *cornerHost = fetch ? fetch->parentWidget() : nullptr;
        QPushButton *refreshBtn = nullptr;
        if (cornerHost) {
            const auto buttons = cornerHost->findChildren<QPushButton *>();
            for (QPushButton *b : buttons) {
                if (b->property("glyph").toString() == QLatin1String("refresh"))
                    refreshBtn = b;
            }
        }
        // Positions only mean anything after the layout has run, and the layout only runs for a widget
        // that has been shown — measuring a hidden page would have been a check of (0,0).
        f.window->setCurrentPage(MainWindow::PageProfiles);
        f.window->show();
        QTest::qWait(80);
        check(fetch && cornerHost && refreshBtn
                      && fetch->x() + fetch->width() <= cornerHost->width()
                      && refreshBtn->x() + refreshBtn->width() <= cornerHost->width(),
              QStringLiteral("…and both corner controls fit the row, so nothing was clipped to make room "
                             "for the new one (fetch ends %1, refresh ends %2, page is %3)")
                  .arg(fetch ? fetch->x() + fetch->width() : -1)
                  .arg(refreshBtn ? refreshBtn->x() + refreshBtn->width() : -1)
                  .arg(cornerHost ? cornerHost->width() : -1));

        QPushButton *bigFetch = nullptr;
        if (QWidget *placeholder = f.window->findChild<QWidget *>(QStringLiteral("placeholderPage"))) {
            const auto buttons = placeholder->findChildren<QPushButton *>();
            for (QPushButton *b : buttons) {
                if (b->text() == LanguageManager::tr(QStringLiteral("action.download_zapret")))
                    bigFetch = b;
            }
        }
        check(bigFetch != nullptr,
              "…and the empty list itself offers the download as a primary button, which is where the "
              "affordance belongs: nothing to show is the moment a user needs a way to get something");
        check(bigFetch && bigFetch->objectName() == QLatin1String("primaryButton"),
              "…styled as the page's primary action, not as one of its quiet ones");
    }

    // ---- the header names the section the stack is on ----
    {
        EnrolmentFixture f(store);
        MainWindow *w = f.window;
        check(w->appTitleText() == QStringLiteral("Muffin"),
              "the dashboard is the window itself, so the header carries the brand and nothing else");
        const struct { int page; const char *key; } sections[] = {
            {MainWindow::PageProfiles, "nav.profiles"},
            {MainWindow::PageLogs, "nav.logs"},
            {MainWindow::PageHistory, "nav.history"},
            {MainWindow::PageSettings, "nav.settings"},
        };
        for (const auto &s : sections) {
            w->setCurrentPage(s.page);
            const QString want = LanguageManager::tr(QStringLiteral("header.title"))
                                     .arg(LanguageManager::tr(QLatin1String(s.key)));
            check(w->appTitleText() == want,
                  QStringLiteral("the header reads '%1' on the %2 page — the section word is the rail's "
                                 "own key, so the two cannot name it differently")
                      .arg(want, QLatin1String(s.key)));
        }
        w->setCurrentPage(MainWindow::PageDashboard);
        check(w->appTitleText() == QStringLiteral("Muffin"),
              "…and goes back to the bare brand on the way home");
    }

    // ---- one heading per page: the header band is it ----
    //
    // Each full-screen page used to open with a "← Muffin" button and its own large name, one band below
    // a header that already says "Muffin • Профили". Three of the four have since dropped both, and what
    // this pins is the shape that is left: the header is the only place a page is named, the rows the
    // headings shared keep their controls, and the way out is the one every page has always had.
    {
        EnrolmentFixture f(store);
        MainWindow *w = f.window;
        const int pages[] = {MainWindow::PageProfiles, MainWindow::PageLogs,
                             MainWindow::PageHistory, MainWindow::PageSettings};
        for (int page : pages) {
            w->setCurrentPage(page);
            QWidget *current = w->currentPageWidget();
            check(current
                      && current->findChildren<QLabel *>(QStringLiteral("pageTitle"),
                                                        Qt::FindChildrenRecursively)
                             .isEmpty(),
                  QStringLiteral("page %1 carries no heading of its own under the window's header")
                      .arg(page));
        }
        w->setCurrentPage(MainWindow::PageDashboard);

        // By glyph rather than by text: the words were in seven languages and the arrow was the one thing
        // all seven shared, so a leftover button is catchable whatever the language under the suite.
        int arrows = 0;
        const auto buttons = w->findChildren<QPushButton *>();
        for (QPushButton *b : buttons)
            if (b->text().startsWith(QString::fromUtf8("\xe2\x86\x90")))
                ++arrows;
        check(arrows == 0,
              "nothing in the window offers a '←' way back any more: the rail and Esc are the way out");

        // …and Esc is that way out, still registered on the window. Checked as a registration rather than
        // as a key event, because an offscreen window is not the active window and a posted Escape would
        // prove the platform rather than the application.
        bool escapeArmed = false;
        const auto shortcuts = w->findChildren<QShortcut *>();
        for (QShortcut *s : shortcuts)
            if (s->key() == QKeySequence(Qt::Key_Escape))
                escapeArmed = true;
        check(escapeArmed, "Esc is registered as the way back, which is what the visible buttons could go");
        check(w->currentPage() == MainWindow::PageDashboard, "…and the page it returns to is the dashboard");

        // The actions those rows used to carry are still on their pages: clearing the record, measuring
        // the services, checking the bypass, and the journal's own filter and tools.
        w->setCurrentPage(MainWindow::PageProfiles);
        int profilesActions = 0;
        const auto profilesButtons = w->currentPageWidget()->findChildren<QPushButton *>(
            QStringLiteral("ghostButton"));
        for (QPushButton *b : profilesButtons)
            if (!b->text().isEmpty())
                ++profilesActions;
        check(profilesActions >= 3,
              "the profiles row still carries its measurements — dropping the headings dropped nothing");
        w->setCurrentPage(MainWindow::PageHistory);
        check(w->historyPage() && w->historyPage()->clearButton()
                  && w->historyPage()->clearButton()->objectName() == QLatin1String("dangerButton"),
              "…and the history page still carries the one destructive thing it can do");
        w->setCurrentPage(MainWindow::PageLogs);
        check(w->logPanel() && w->logPanel()->view() != nullptr,
              "…and the journal starts where its text starts");
        w->setCurrentPage(MainWindow::PageSettings);
        QPushButton *save = nullptr;
        const auto settingsButtons = w->currentPageWidget()->findChildren<QPushButton *>(
            QStringLiteral("primaryButton"));
        for (QPushButton *b : settingsButtons)
            if (b->isVisibleTo(w))
                save = b;
        check(save != nullptr,
              "…and the form still closes with the Save button that commits it");
        w->setCurrentPage(MainWindow::PageDashboard);
    }

    // The store is left exactly as the section found it: a run that ended with the WARP half lit would
    // be a next launch that asks for a tunnel.
    store.setZapretPath(savedPath);
    store.setUseZapret(savedZapret);
    store.setUseWarp(savedWarp);
    store.setStartLastOnLaunch(savedStart);
}

// ------------------------------------------------- the WARP setup wizard ----
//
// The wizard with its three seams replaced: no package manager, no daemon, no root prompt. Each step is
// recorded and answered by the scenario, which is what makes the chain's order, its retry, its cancel and
// the words on its status line checkable on a machine that has none of the software the real chain
// drives — and, more to the point, without changing anything on the machine running the tests.
class ProbeSetupDialog : public WarpSetupDialog
{
public:
    using WarpSetupDialog::WarpSetupDialog;
    // The two ways a step reports back are protected on the base, and a scenario drives the dialog from
    // outside its own class — so they are re-exposed here. That is the point of the seam: a test answers
    // a step exactly where the real process would have.
    using WarpSetupDialog::stepAnswered;
    using WarpSetupDialog::commandAnswered;

    QStringList commands;      // one entry per launched command, as "program arg arg"
    int installBegun = 0;
    int registerBegun = 0;
    // Shared with the control, so one log carries the order the chain actually ran in — which is the
    // whole claim when the question is "was the host re-asked before the enrolment".
    ActLog *acts = nullptr;
    // How long this scenario's gap after a step that moved the daemon is. Zero by default: the wait is
    // 1.5 s of real time per chain scenario, and a suite that spent it in every scenario would be timing
    // a clock rather than ordering acts. The two scenarios that watch the gap set it, and one of them
    // also checks that the base answer is the core's number rather than a second one kept here.
    int settleMs = 0;
    // Whether the enrolment step is the real one rather than a record. Off by default, because a
    // scenario driving the chain from outside wants a step it answers rather than a pair of signals it
    // has to arrange; on for the scenario watching what the manager's own channels do to the sentence a
    // failure is reported with, which cannot be observed through a stub.
    bool delegateRegistration = false;
    // Whether the wizard's own command launcher runs rather than a record. Off by default for the same
    // reason the other seams are, and on for the one claim that cannot be made through a stub: that a
    // command's own stderr survives into the sentence about its failure.
    bool realCommands = false;
    // The base's answer to the seam below, for the scenario that pins the wizard's gap to the core's
    // number rather than to a second constant kept here.
    int baseSettleMs() const { return WarpSetupDialog::serviceSettleMs(); }

protected:
    int serviceSettleMs() const override { return settleMs; }

    bool launchCommand(const WarpManager::SetupCommand &command, QString *error) override
    {
        if (realCommands)
            return WarpSetupDialog::launchCommand(command, error);
        Q_UNUSED(error);
        commands << command.program + QLatin1Char(' ') + command.args.join(QLatin1Char(' '));
        return true;
    }
    bool beginInstallStep(QString *error) override
    {
        Q_UNUSED(error);
        ++installBegun;
        if (acts)
            acts->record(QStringLiteral("install"));
        return true;
    }
    bool beginRegistrationStep(QString *error) override
    {
        if (delegateRegistration)
            return WarpSetupDialog::beginRegistrationStep(error);
        ++registerBegun;
        if (acts)
            acts->record(QStringLiteral("register"));
        return true;
    }
};

static void testWarpSetupDialog()
{
    std::printf("\n[WARP setup wizard]\n");

    // Does this widget actually paint in `want`? The status line is transparent-backed, so the question
    // is whether any of its pixels carry the colour — which is precisely what a stylesheet rule that was
    // never re-read after an objectName change fails to do. Reading the wanted colour out of the sheet
    // rather than out of a literal is what ties the render to the declaration.
    auto paintsColor = [](QWidget *w, const QColor &want) {
        if (!w || !want.isValid())
            return false;
        const QImage img = w->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                if (qAlpha(img.pixel(x, y)) > 128 && rgbClose(img.pixelColor(x, y), want, 24))
                    return true;
            }
        }
        return false;
    };
    const QString sheet = qApp->styleSheet();
    const QColor runningColour = sheetRuleColor(sheet, QStringLiteral("QLabel#setupStatus"),
                                                QStringLiteral("color"));
    const QColor failedColour = sheetRuleColor(sheet, QStringLiteral("QLabel#setupStatusFailed"),
                                               QStringLiteral("color"));
    const QColor doneColour = sheetRuleColor(sheet, QStringLiteral("QLabel#setupStatusDone"),
                                             QStringLiteral("color"));
    check(runningColour.isValid() && failedColour.isValid() && doneColour.isValid(),
          "the sheet gives the status line a colour per state, which is the whole way it reports one");
    check(runningColour != failedColour && failedColour != doneColour,
          "…and the three are distinguishable, so the last line cannot be mistaken for a middle one");

    // A control that reports a machine with no client and no enrolment reading, which is the case the
    // wizard exists for and the one whose plan is the full six steps.
    CountingWarpManager manager;
    manager.available = false;
    manager.registration = WarpManager::Registration::Unknown;

    // ---- the plan the dialog was handed ----
    {
        ProbeSetupDialog dialog(manager);
        const auto steps = dialog.steps();
        QStringList ids;
        for (const WarpManager::SetupStep &s : steps)
            ids << s.id;
        check(ids == WarpManager::setupStepIds(Strategy::Platform::Linux),
              "the dialog walks the same plan the core spells, rather than a second copy of it");
        check(dialog.stepCount() == 6, "…which on a bare Linux host is six steps");
        bool allTitled = !steps.isEmpty();
        for (const WarpManager::SetupStep &s : steps)
            if (WarpSetupDialog::stepTitle(s.id).isEmpty())
                allTitled = false;
        check(allTitled,
              "every step id has a sentence in the catalog — a step renamed in the core and forgotten "
              "here would otherwise show up as a blank status line");
        const QString sample = WarpSetupDialog::stepStatusText(2, 5, QStringLiteral("X"));
        check(sample.contains(QStringLiteral("2")) && sample.contains(QStringLiteral("X")),
              "the status line carries the step's position and its own name");
    }

    // ---- the shape before anything runs ----
    {
        ProbeSetupDialog dialog(manager);
        check(dialog.startButton() && dialog.startButton()->isVisibleTo(&dialog),
              "the dialog opens on its question, with the way to answer it");
        check(dialog.closeButton() && !dialog.closeButton()->isVisibleTo(&dialog),
              "…and no Close before there is a result to close");
        check(!dialog.logVisible() && dialog.progressBar()
                      && !dialog.progressBar()->isVisibleTo(&dialog),
              "the bar and the log stay off screen until there is something to show in them");
        check(dialog.currentStepIndex() == -1, "no step is running before the user agrees");
    }
    // ---- the whole chain, step by step ----
    {
        ProbeSetupDialog dialog(manager);
        dialog.show();
        QSignalSpy finished(&dialog, &WarpSetupDialog::setupFinished);
        // The half of the borrow that lives in the chain rather than in the window: the wizard is the only
        // thing that knows when it has stopped needing the bypass, and the connect is that line. Watched
        // as a signal because the wiring on the other end of it opens a modal the suite cannot drive.
        QSignalSpy released(&dialog, &WarpSetupDialog::bypassReleaseRequested);

        dialog.startSetup();
        check(dialog.startButton() && !dialog.startButton()->isVisibleTo(&dialog),
              "pressing Start takes the question away — the answer is now the run itself");
        check(dialog.logVisible() && dialog.progressBar()->isVisibleTo(&dialog)
                      && dialog.statusLabel()->isVisibleTo(&dialog),
              "…and brings up the bar, the status line and the log");
        check(dialog.installBegun == 1 && dialog.currentStepIndex() == 0,
              "the first step is the install, and it is the installer's rather than a second apt here");
        check(dialog.statusLabel()->text()
                      == WarpSetupDialog::stepStatusText(
                             1, dialog.stepCount(),
                             WarpSetupDialog::stepTitle(QStringLiteral("install"))),
              "the status names the step and its position");
        check(!dialog.cancelButton()->isEnabled(),
              "and Cancel is withheld for exactly as long as a package manager is mid-transaction");
        check(paintsColor(dialog.statusLabel(), runningColour),
              "…and the status line is drawn in the running colour, not merely named after it");

        dialog.stepAnswered(true, QString());
        check(dialog.currentStepIndex() == 1 && dialog.commands.size() == 1
                      && dialog.commands.at(0)
                             .contains(QStringLiteral("enable --now warp-svc")),
              "then the daemon is asked to come up, and before anything is asked OF it: a warp-cli verb "
              "is a client talking to warp-svc over a socket, and a socket with nobody behind it refuses "
              "the enrolment with a sentence that reads like a verdict about the device");
        check(dialog.statusLabel()->text()
                      == WarpSetupDialog::stepStatusText(
                             2, dialog.stepCount(),
                             WarpSetupDialog::stepTitle(QStringLiteral("daemon"))),
              "…announced as a step of its own, so the root prompt it is about to raise has a sentence "
              "attached to it rather than arriving over a line about registration");

        dialog.commandAnswered(0, QStringLiteral("Created symlink /etc/systemd/system/multi-user.target"
                                                 ".wants/warp-svc.service"), false);
        check(dialog.registerBegun == 1 && dialog.currentStepIndex() == 2,
              "the enrolment is next, and it is the manager's — a bare `registration new` here would be "
              "a second key for a device that may already hold one");

        dialog.stepAnswered(true, QString());
        check(dialog.currentStepIndex() == 3 && dialog.commands.size() == 2
                      && dialog.commands.at(1).contains(QStringLiteral("tunnel protocol set MASQUE")),
              "then the transport is switched, with the argv the core spelled rather than a second copy");

        dialog.commandAnswered(0, QStringLiteral("Success"), false);
        check(dialog.currentStepIndex() == 4 && dialog.commands.size() == 3
                      && dialog.commands.at(2).contains(QStringLiteral("systemctl restart warp-svc")),
              "then the service is restarted — the step that clears the daemon a connect used to hang "
              "against, and one that comes after the enrolment because a daemon restarted mid "
              "`registration new` loses the key that call is obtaining");
        check(dialog.logView()->toPlainText().contains(QStringLiteral("systemctl restart warp-svc")),
              "…and the pane shows the command that was run, not only the plan's idea of it");

        check(released.isEmpty(),
              "and nothing has been given back yet: the enrolment is still talking to Cloudflare through "
              "the path the install borrowed");
        dialog.commandAnswered(0, QString(), false);
        check(dialog.currentStepIndex() == 5 && dialog.commands.size() == 4
                      && dialog.commands.at(3).contains(QStringLiteral("connect")),
              "and only then is the tunnel asked for");
        check(released.count() == 1,
              "…with the release announced at the connect step and not before it, which is where the "
              "borrow stops being needed: everything after this rides the tunnel, not the bypass");
        check(dialog.progressBar()->value() == 5,
              "the bar counts finished steps, so it is not full while one is still running");

        dialog.commandAnswered(0, QString(), false);
        check(dialog.succeeded(), "a chain whose every step answered is a chain that succeeded");
        check(finished.count() == 1 && finished.at(0).at(0).toBool(),
              "…announced once, so a caller cannot read two outcomes out of one run");
        check(dialog.progressBar()->value() == dialog.progressBar()->maximum(),
              "the bar is full when the work is");
        check(dialog.statusLabel()->text()
                      == LanguageManager::tr(QStringLiteral("warp.setup.done")),
              "and the last line says so in the catalog's own words");
        check(paintsColor(dialog.statusLabel(), doneColour),
              "…in the colour the sheet reserves for success, so the ending reads differently from the "
              "middle without being read at all");
        check(dialog.closeButton() && dialog.closeButton()->isVisibleTo(&dialog)
                      && !dialog.startButton()->isVisibleTo(&dialog),
              "with a Close to press and no Start left to press again");
        dialog.hide();
    }
    // ---- the host is asked again between the install and everything that depends on it ----
    //
    // The reported failure, reproduced as an ordering claim: the install step succeeds, and the manager
    // still holds the "no client here" it cached when it was constructed — before apt ran. The enrolment
    // asked of that cache is refused with "warp-cli is not installed on this machine", directly under a
    // step that says the install worked. What fixes it is not a cleverer cache but the rescan happening
    // at the one moment the answer is guaranteed to have changed, and the order below is that claim.
    {
        ActLog acts;
        manager.log = &acts;
        manager.programPath = QStringLiteral("/usr/bin/warp-cli");
        // This control is shared with every scenario above, and the one that walked a whole successful
        // chain has already taken it through an install step — so the claim here is about the rescans
        // THIS scenario causes, read against the count it started at rather than against a zero that
        // belonged to a different dialog.
        const int rescansBefore = manager.rescans;
        ProbeSetupDialog dialog(manager);
        dialog.acts = &acts;
        dialog.show();

        dialog.startSetup();
        check(manager.rescans == rescansBefore,
              "starting the chain re-asks the host nothing yet — the install has not happened");
        dialog.stepAnswered(true, QString());   // the install step reports success

        check(manager.rescans == rescansBefore + 1,
              "a successful install is followed by a rescan of the host, because everything after this "
              "step is a verb against a client that did not exist a moment ago");
        check(dialog.commands.size() == 1
                      && dialog.commands.at(0).contains(QStringLiteral("enable --now warp-svc")),
              "…and what that rescan unblocks is the daemon step, the next thing the plan runs and the "
              "first thing that needs the client the package manager just put down");
        dialog.commandAnswered(0, QString(), false);
        check(acts.acts == (QStringList{QStringLiteral("install"),
                                        QStringLiteral("rescanAvailability"),
                                        QStringLiteral("register")}),
              "…and the rescan lands between the install and the enrolment, which is the only place it "
              "does any good");
        const QString pane = dialog.logView()->toPlainText();
        check(pane.contains(LanguageManager::tr(QStringLiteral("warp.setup.rescan"))),
              "the pane says the check happened, so a refusal on the next step cannot read as a "
              "contradiction of the one before it");
        check(pane.contains(QStringLiteral("/usr/bin/warp-cli"))
                      && pane.contains(LanguageManager::tr(QStringLiteral("warp.setup.rescan_found"))
                                           .arg(QStringLiteral("/usr/bin/warp-cli"))),
              "…and names what it found, in the sentence the catalog spells for it");

        // The other half of the same rescan: a package that reported success and left nothing drivable
        // is a diagnosis the next step's own error cannot give, so it is said here, in red, first.
        manager.programPath.clear();
        ProbeSetupDialog empty(manager);
        empty.show();
        empty.startSetup();
        empty.stepAnswered(true, QString());
        check(empty.logView()->toPlainText()
                      .contains(LanguageManager::tr(QStringLiteral("warp.setup.rescan_missing"))),
              "and when the rescan finds nothing, the pane says that the package lied rather than "
              "letting the next step fail for reasons of its own");
        empty.hide();
        manager.programPath = QStringLiteral("/usr/bin/warp-cli");
        manager.log = nullptr;
        dialog.hide();
    }

    // ---- a step that fails, and the retry of it ----
    {
        ProbeSetupDialog dialog(manager);
        dialog.show();
        QSignalSpy finished(&dialog, &WarpSetupDialog::setupFinished);
        dialog.startSetup();
        dialog.stepAnswered(true, QString());                 // install
        dialog.commandAnswered(0, QString(), false);          // daemon
        dialog.stepAnswered(true, QString());                 // enrolment
        const int commandsBefore = dialog.commands.size();
        dialog.commandAnswered(1, QStringLiteral("warp-cli: unrecognized option"), false);

        check(dialog.currentStepIndex() == 3, "the chain stops at the step that failed");
        check(dialog.statusLabel()->objectName() == QLatin1String("setupStatusFailed"),
              "…and the status line is styled as the failure it is, not left in its running colour");
        check(paintsColor(dialog.statusLabel(), failedColour),
              "…which means repainted, not just renamed: a stylesheet never re-reads an objectName on "
              "its own, and a red rule on a purple line is a failure the user cannot see");
        check(dialog.statusLabel()->text().contains(QStringLiteral("warp-cli: unrecognized option")),
              "the sentence quotes what the command said, which is the half a bare exit code never "
              "carries");
        check(dialog.logView()->toPlainText()
                      .contains(QStringLiteral("warp-cli: unrecognized option")),
              "…and so does the log, in the red the journal fails in");
        check(dialog.startButton() && dialog.startButton()->isVisibleTo(&dialog)
                      && dialog.startButton()->text()
                             == LanguageManager::tr(QStringLiteral("warp.setup.retry")),
              "the primary button comes back as a retry, because after a failure the two options are "
              "\"try that again\" and \"stop\"");
        check(dialog.cancelButton()->isEnabled(), "…and stopping is the other one");
        check(!dialog.succeeded() && finished.count() == 1 && !finished.at(0).at(0).toBool(),
              "a failed chain is announced as failed, once");
        check(!dialog.logView()->toPlainText()
                      .contains(LanguageManager::tr(QStringLiteral("warp.setup.daemon_down"))
                                    .arg(WarpManager::serviceStartHint(
                                        ZapretSource::hostPlatform()))),
              "…and a client that merely disliked the question is not blamed on the service: the "
              "manual-start sentence belongs to a refusal that names the daemon, and printing it for "
              "every failure would teach the user to restart a service that was never the problem");

        dialog.startSetup();
        check(dialog.currentStepIndex() == 3 && dialog.commands.size() == commandsBefore + 1,
              "and the retry repeats the step that failed rather than installing and enrolling again — "
              "a second enrolment would be a second key with the first one orphaned");
        dialog.hide();
    }

    // ---- a daemon step that is refused does not end the chain ----
    //
    // Its commands are the only non-fatal ones in the plan, and this is why: three different machines all
    // answer it with a non-zero exit — a host with no systemd at all, a pkexec prompt the user declined,
    // and a Windows service that was already running, which `sc start` reports as the error 1056. Only
    // the enrolment that follows can tell those apart, so a refusal here is written down and walked past
    // rather than reported as the failure of the whole setup.
    {
        ProbeSetupDialog dialog(manager);
        dialog.show();
        dialog.startSetup();
        dialog.stepAnswered(true, QString());        // the install reports success
        check(dialog.currentStepIndex() == 1, "the daemon is the step the install hands over to");
        dialog.commandAnswered(1, QStringLiteral("pkexec: Unexpected response"), false);
        check(dialog.currentStepIndex() == 2 && dialog.registerBegun == 1,
              "…and a refusal of it is survived: the enrolment is still asked for, because a machine "
              "whose daemon somebody else started has nothing to gain from the wizard stopping here");
        check(dialog.logView()->toPlainText().contains(QStringLiteral("pkexec: Unexpected response")),
              "the refusal is in the pane rather than swallowed on the way past — a chain that quietly "
              "skipped a step would be a chain nobody could explain afterwards");
        check(!dialog.logView()->toPlainText()
                      .contains(LanguageManager::tr(QStringLiteral("warp.setup.daemon_down"))
                                    .arg(WarpManager::serviceStartHint(
                                        ZapretSource::hostPlatform()))),
              "…and it is not dressed as the failure it is not: the manual-start sentence belongs to a "
              "chain that stopped, and printing it over a step that went on to work would be a red line "
              "the user learns to ignore");
        dialog.hide();
    }

    // ---- the wizard takes the vendor's window down before the connect that summons it ----
    //
    // The claim is an order, and it is the order measured on the machine that reported the bug: the
    // window appears *because of* the connect (the daemon's status traffic activates the taskbar's bus
    // name, whose unit answers by mapping a window), so asking afterwards is chasing it and asking
    // beforehand closes the door. The automatic path is covered by WarpManager::start(); this chain runs
    // `warp-cli connect` as its own command, so it owes the same ask separately — and a wizard that
    // skipped it would leave every fresh install with a second window on screen.
    {
        CountingWarpManager bare;
        bare.available = false;      // a host with nothing: the full six-step plan
        ProbeSetupDialog dialog(bare);
        dialog.show();
        dialog.startSetup();
        dialog.stepAnswered(true, QString());            // install reports success
        dialog.commandAnswered(0, QString(), false);     // daemon: systemctl enable --now
        dialog.stepAnswered(true, QString());            // enrolment settled
        dialog.commandAnswered(0, QString(), false);     // protocol: MASQUE — which launches the restart
        check(dialog.commands.size() == 3
                  && dialog.commands.last().contains(QStringLiteral("restart")),
              "the service step's restart is the command standing in front of the connect");
        check(bare.suppressCalls == 0,
              "no window policy is asked of a chain that has not reached the verb that opens one");
        dialog.commandAnswered(0, QString(), false);     // service answered — the chain walks to connect
        check(dialog.commands.size() == 4
                  && dialog.commands.last().contains(QStringLiteral("connect")),
              "…and the wizard has reached its last step, the connect itself");
        check(bare.suppressCalls == 1,
              "the window is taken down as the chain reaches the connect, not after it and not never");
        dialog.hide();
    }

    // ---- the gap a step that moved the daemon owes the verb after it ----
    //
    // The race the field report caught: systemd says warp-svc is active before warp-svc has re-opened
    // its control socket, and the wizard runs its next command the instant the previous one exits. Without
    // a gap, the connect that follows a restart is aimed at a path with nobody behind it, and the answer
    // it gets — "Unable to connect to the CloudflareWARP daemon: No such file or directory" — reads as a
    // broken tunnel rather than as a daemon still arriving.
    {
        ProbeSetupDialog dialog(manager);
        dialog.settleMs = 60;
        dialog.show();
        check(dialog.baseSettleMs() == WarpManager::serviceSettleMs(),
              "the wizard's gap is the core's number read through, not a second constant kept beside it: "
              "two numbers for one socket are two opinions about how long it takes to come back");
        dialog.startSetup();
        dialog.stepAnswered(true, QString());          // install reports success
        check(dialog.commands.size() == 1
                      && dialog.commands.at(0).contains(QStringLiteral("enable --now warp-svc")),
              "the daemon step is launched");
        dialog.commandAnswered(0, QStringLiteral("active"), false);
        check(dialog.isSettling(),
              "and when it exits the chain stops rather than continuing: the unit is active, the daemon "
              "is not yet answerable");
        check(dialog.commands.size() == 1 && dialog.registerBegun == 0,
              "…and nothing is asked of the daemon inside the gap, which is the whole of the race");
        check(dialog.statusLabel()->text().contains(
                      LanguageManager::tr(QStringLiteral("warp.setup.settle")).arg(0.06, 0, 'g', 2)),
              "…and the pause says what it is waiting for, so a wizard that has stopped moving for a "
              "moment is not one that has stuck");
        QTest::qWait(200);
        check(!dialog.isSettling() && dialog.currentStepIndex() == 2 && dialog.registerBegun == 1,
              "when the gap ends the chain carries on by itself, into the enrolment");
        dialog.hide();
    }

    // ---- a command refused by a daemon that is still arriving is asked again ----
    //
    // The other half of the same wait-out, on the path the settle does not cover: a command that exits
    // non-zero because the unit is active but its socket is not. The wording is the one the clean-install
    // report carried, and it is the classifier's newest marker — a phrase the older list would have read
    // as an ordinary failure and reported as one.
    {
        ProbeSetupDialog dialog(manager);
        dialog.settleMs = 0;
        dialog.show();
        dialog.startSetup();
        dialog.stepAnswered(true, QString());          // install
        dialog.commandAnswered(0, QStringLiteral("active"), false);   // daemon
        dialog.stepAnswered(true, QString());          // enrolment (stubbed)
        check(dialog.commands.size() == 2
                      && dialog.commands.at(1).contains(QStringLiteral("protocol set MASQUE")),
              "the protocol verb is the command now in flight");
        dialog.commandAnswered(1, QStringLiteral("Unable to connect to the CloudflareWARP daemon: "
                                                "No such file or directory"), false);
        check(dialog.daemonRetries() == 1 && dialog.currentStepIndex() == 3 && !dialog.succeeded(),
              "a refusal that says the daemon is not there is waited out on the same command, not failed "
              "and not advanced past");
        QTest::qWait(30);
        check(dialog.commands.size() == 3 && dialog.commands.at(2).contains(QStringLiteral("MASQUE")),
              "…and the ask is repeated rather than replayed from a cache, which is the only way a "
              "daemon that arrives in the meantime is ever reached");
        dialog.commandAnswered(0, QStringLiteral("Success"), false);
        check(dialog.currentStepIndex() == 4 && dialog.daemonRetries() == 0,
              "…and once it answers, the step moves on with a full budget again: attempts spent on a "
              "socket that was arriving are not charged to the next one");
        dialog.hide();
    }

    // ---- a refusal that names the daemon is asked again, and only then told on ----
    //
    // The report the daemon step was added for, driven through the manager's real channels rather than
    // through the stub, because what is under test is what the dialog does with what the manager says:
    // registrationFinished() carries a bool and nothing else, and the reason arrives beside it on the
    // journal channel — which the dialog used to relay and then forget. The retry is the newer half of the
    // same story: a cold-starting warp-svc answers the first verb after its start with this exact
    // sentence, and a wizard that reported it as a failure was reporting the machine as broken while it
    // was still arriving.
    {
        const QString daemonRefusal = QStringLiteral(
            "WarpManager: Missing registration. Try running: 'warp-cli registration new'\n"
            "Maybe the daemon is not running?");
        ProbeSetupDialog dialog(manager);
        dialog.delegateRegistration = true;
        dialog.show();
        dialog.startSetup();
        dialog.stepAnswered(true, QString());          // install
        dialog.commandAnswered(0, QString(), false);   // daemon
        const int enrollBefore = manager.ensureCalls;

        for (int attempt = 1; attempt <= WarpSetupDialog::kDaemonRetryAttempts; ++attempt) {
            manager.answerLog(daemonRefusal, 2);
            manager.answerFinished(false);
            QTest::qWait(30);   // the retry's gap is armed on the event loop and never slept in the caller
            check(dialog.currentStepIndex() == 2 && !dialog.succeeded(),
                  QStringLiteral("attempt %1 is waited out on the same step, not failed").arg(attempt));
            check(dialog.daemonRetries() == attempt,
                  QStringLiteral("…and counted, which is the only thing that bounds it to %1")
                      .arg(WarpSetupDialog::kDaemonRetryAttempts));
            check(dialog.logView()->toPlainText().contains(
                          LanguageManager::tr(QStringLiteral("warp.setup.retry_daemon"))
                              .arg(attempt)
                              .arg(WarpSetupDialog::kDaemonRetryAttempts)),
                  QStringLiteral("…said in the pane, so a chain that pauses is not a chain that has stuck"));
        }
        check(manager.ensureCalls == enrollBefore + WarpSetupDialog::kDaemonRetryAttempts,
              "the retry re-asks the manager rather than replaying a cached answer — and it is the same "
              "bounded number of asks, not one per turn of the event loop");
        check(dialog.statusLabel()->objectName() == QLatin1String("setupStatus"),
              "…while it is retrying the window is still in its running state, not its failed one");

        // The budget spent. Now, and only now, this is a failure the user has to be handed.
        manager.answerLog(daemonRefusal, 2);
        manager.answerFinished(false);
        QTest::qWait(30);

        const QString pane = dialog.logView()->toPlainText();
        check(pane.contains(QStringLiteral("Maybe the daemon is not running?")),
              "the daemon's own sentence reaches the pane…");
        check(pane.contains(LanguageManager::tr(QStringLiteral("warp.setup.daemon_down"))
                                .arg(WarpManager::serviceStartHint(ZapretSource::hostPlatform()))),
              "…with the one command that would fix it underneath, once the second chances are spent: the "
              "difference between a refusal the user can act on and one they can only read");
        check(dialog.currentStepIndex() == 2,
              "…and only now is the step failed, and at the step it belongs to");
        check(dialog.statusLabel()->text().contains(QStringLiteral("Missing registration")),
              "the status line quotes the reason instead of the step's title, because the words for what "
              "went wrong are about the service and the title says \"registration\"");
        check(dialog.statusLabel()->objectName() == QLatin1String("setupStatusFailed"),
              "…styled as the failure it is, so what comes next cannot read as a step still running");
        check(dialog.startButton() && dialog.startButton()->isVisibleTo(&dialog)
                      && dialog.startButton()->text()
                             == LanguageManager::tr(QStringLiteral("warp.setup.retry")),
              "and the way to try again is on screen: starting the service by hand and pressing it is the "
              "whole recovery, so a dialog offering neither is one that has to be killed");
        dialog.hide();
    }

#if defined(Q_OS_UNIX)
    // ---- what a failed command said is what its failure sentence says ----
    //
    // The pane reported "exit 1 with no output" for commands that had explained themselves in full. The
    // cause was in this file and not in the child: the handler that streams output into the pane drains the
    // channel as it goes, and the failure sentence was then built from a second read of that same, now
    // empty, buffer — with the channels merged, so stderr had been drained by that very read too. Asserted
    // end to end against a real process writing to a real error channel, because every other scenario
    // here replaces the launcher rather than running it.
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temporary directory for the stand-in client");
        const QString fake = dir.path() + QStringLiteral("/warp-cli");
        const QString body = QStringLiteral("#!/bin/sh\n"
                                            "echo 'the socket is not there yet' >&2\n"
                                            "exit 3\n");
        bool written = false;
        QString how;
        {
            QFile script(fake);
            const bool opened = script.open(QIODevice::WriteOnly | QIODevice::Truncate);
            const qint64 sent = opened ? script.write(body.toUtf8()) : -1;
            script.close();
            const bool perms = QFile::setPermissions(
                fake, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                      | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
                      | QFileDevice::ExeOther);
            how = QStringLiteral("(open %1, wrote %2 of %3, chmod %4, exists %5)")
                      .arg(opened ? QStringLiteral("ok") : QStringLiteral("failed"))
                      .arg(sent)
                      .arg(body.toUtf8().size())
                      .arg(perms ? QStringLiteral("ok") : QStringLiteral("failed"))
                      .arg(QFileInfo::exists(fake) ? QStringLiteral("yes") : QStringLiteral("no"));
            written = opened && sent == body.toUtf8().size() && perms;
        }
        check(written, QStringLiteral("a stand-in warp-cli that answers on stderr and exits 3 was "
                                       "written and made runnable %1")
                          .arg(how));

        const bool wasAvailable = manager.available;
        const WarpManager::Registration wasRegistration = manager.registration;
        const QString wasPath = manager.programPath;
        manager.available = true;                        // isInstalled() reads this one flag
        manager.registration = WarpManager::Registration::Registered;
        manager.programPath = fake;                      // and this is the binary the chain will run
        {
            ProbeSetupDialog dialog(manager);
            dialog.realCommands = true;
            dialog.settleMs = 0;
            dialog.show();
            check(dialog.stepCount() == 3,
                  "a machine with an enrolled client is left with protocol, service and connect — so the "
                  "first thing the chain runs is a command this test can let be real");
            dialog.startSetup();
            QTest::qWait(900);
            const QString pane = dialog.logView()->toPlainText();
            check(dialog.statusLabel()->text().contains(QStringLiteral("the socket is not there yet")),
                  "the failure sentence quotes what the command said, including the line it wrote to its "
                  "error channel");
            check(!pane.contains(QStringLiteral("with no output"))
                          && !dialog.statusLabel()->text().contains(QStringLiteral("with no output")),
                  "…and neither the pane nor the sentence claims a command said nothing");
            check(!dialog.succeeded(),
                  "…and the step is failed on its merits rather than swallowed by a wait for output that "
                  "had already been read");
            dialog.hide();
        }
        manager.available = wasAvailable;
        manager.registration = wasRegistration;
        manager.programPath = wasPath;
    }
#endif

    // ---- walking away ----
    //
    // Driven through the button rather than by calling the slot, so the wiring is part of what is
    // checked: a disabled Cancel that could still be reached by some other route would be a package
    // manager interrupted halfway.
    {
        ProbeSetupDialog dialog(manager);
        dialog.show();
        dialog.startSetup();
        dialog.cancelButton()->click();
        check(!dialog.cancelled(),
              "the install step cannot be walked away from: an interrupted package manager leaves a "
              "database to repair");
        dialog.stepAnswered(true, QString());
        dialog.cancelButton()->click();
        check(dialog.cancelled() && dialog.result() == QDialog::Rejected,
              "a step that can be interrupted is, and the dialog closes as declined — a window closed by "
              "hand is not a machine that was set up");
        dialog.hide();
    }
}


// A release answer has to become something a person can see, and the seeing is the window's claim
// rather than the checker's: UpdateChecker decides whether a build is ahead, MainWindow decides what an
// offer looks like. This section hands the window an answer by hand — the same way the watchdog section
// hands it a verdict — because the one method that would produce an answer on its own opens a socket to
// GitHub, and no suite can afford to be waiting on that.
static void testUpdateNotice()
{
    std::printf("\n[Update notice]\n");

    // The delay is a number the feature was specified with, and one a test can hold the code to.
    check(MainWindow::kUpdateCheckDelayMs == 5000,
          "the first lookup waits the five seconds the startup was specified for");

    if (!configRedirectedIntoBuildTree()) {
        std::printf("  SKIP  the notice belongs to a window, and constructing one needs XDG_CONFIG_HOME "
                    "redirected into the build tree (run it through ctest)\n");
        return;
    }

    // A two-profile tree, for the same reason the watchdog section wants one: pointed at nothing, the
    // window opens its settings page instead of settling into the chrome this section is checking.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;
    const QString savedPath = store.zapretPath();
    store.setZapretPath(root);

    {
        // Never shown at first, and never asking: scheduleUpdateCheck() is main.cpp's call, which is
        // exactly why a window constructed here cannot reach the network.
        MainWindow window;
        QToolButton *badge = window.updateButton();
        check(badge != nullptr, "the header carries the release badge");
        if (!badge) {
            store.setZapretPath(savedPath);
            return;
        }

        check(badge->objectName() == QLatin1String("windowButton"),
              "…as a control of the window chrome, which is where the design puts it");
        check(badge->property("role").toString() == QLatin1String("update"),
              "…told apart from minimise and close by a role, so both sheets can style it alone");
        check(badge->property("glyph").toString() == QLatin1String("update-available"),
              "…and carrying the glyph property applyTheme() re-bakes, so a theme change keeps it lit");

        // ---- before any answer, nothing is offered ----
        check(badge->isHidden(), "a window nobody has answered yet shows nothing");
        check(!window.updateNoticeShown(), "…and says so through the window's own predicate");
        check(window.updateVersion().isEmpty(), "…with no version on offer to name");

        // ---- the answer arrives ----
        const int lines = window.logPanel()->storedLineCount();
        window.onUpdateAvailable(QStringLiteral("v0.5.0"),
                                 QStringLiteral("https://example.invalid/releases/tag/v0.5.0"));
        check(window.updateNoticeShown(),
              "an answer naming a newer build raises the badge — the visible half of the feature");
        check(!badge->isHidden(), "…which is on its way to being seen, window on screen or not");
        check(window.updateVersion() == QLatin1String("v0.5.0"),
              "…quoting the tag the feed published rather than a re-spelling of it");
        check(window.logPanel()->storedLineCount() == lines + 1,
              "…and recorded in the journal as well as shown, so the offer outlives the glance");

        // The words the press will ask with, read out of the catalog rather than written here.
        const QString question = LanguageManager::tr(QStringLiteral("update.question"),
                                                     QStringLiteral("v0.5.0"));
        check(question.contains(QStringLiteral("v0.5.0")),
              "the question names the version it is offering");
        check(!LanguageManager::tr(QStringLiteral("update.title")).isEmpty(),
              "…and the dialog has a title to ask it under");
        check(!LanguageManager::tr(QStringLiteral("update.badge_tip")).isEmpty(),
              "…and the icon-only control has an accessible name, which is what a glyph with no label "
              "leaves a screen reader to read");

        // A second answer replaces the offer rather than stacking a second badge on it or losing the
        // first: the feed can only be ahead by one release, and pressing it must open the newest page.
        window.onUpdateAvailable(QStringLiteral("v0.6.0"),
                                 QStringLiteral("https://example.invalid/releases/tag/v0.6.0"));
        check(window.updateVersion() == QLatin1String("v0.6.0"),
              "a newer answer replaces the version the badge is offering");
        check(window.updateNoticeShown(), "…without taking the offer away");

        // ---- the pulse, on a badge that can be seen ----
        // Breathing is a claim about a widget on screen, so it is checked the way the motion section
        // checks every other one: with the window actually shown, under the offscreen platform.
        const bool motionWasOn = Motion::enabled();
        Motion::setEnabled(true);
        window.show();
        QTest::qWait(40);
        check(badge->isVisible(), "the badge is on screen once its window is");
        check(Motion::isBreathing(badge),
              "…and breathing, which is what tells a person an icon has just arrived rather than always "
              "lived in that corner");
        Motion::setEnabled(false);
        window.onUpdateAvailable(QStringLiteral("v0.7.0"), QStringLiteral("https://example.invalid/r"));
        check(!Motion::isBreathing(badge) && window.updateNoticeShown(),
              "with motion off the offer still arrives: the switch governs the movement, never the news");
        Motion::setEnabled(motionWasOn);
        window.hide();
    }

    store.setZapretPath(savedPath);
}

// ------------------------------------------------------------- rounded shell ----
//
// The window's own corners. Everything above asks what the window contains; this asks what the window IS —
// a rounded rectangle with no system frame and, since the shell, no background of its own. A shape cannot
// be checked by reading a property, so the window is rendered and the rendering is read: the shell paints
// an arc, and what lies outside it is not a colour at all. That last part is the whole trick —
// QWidget::grab() fills its target with Qt::transparent before rendering, so an alpha of 0 in the image is
// the desktop showing through, which is exactly the fact this section turns on.
//
// Four ways it can be wrong, each with its own reading:
//   * no band — a card flush with the surface edge has its 1 px stroke cut in half by that edge, so the
//     window's own outline is a smudge instead of a line;
//   * a painted window or canvas — an opaque rectangle again, corners and all, with the rounding hidden
//     behind it: the black squares this feature exists to remove, in whatever colour the theme uses;
//   * a square corner inside the arc — the rail, the main column, the header band and the footer are
//     children, and Qt does not clip a child to its parent's radius, so an unrounded one paints a nub of
//     the app outside the silhouette and over the stroke;
//   * the hierarchy — the shell sits between the window and everything the suite finds by name, so a page,
//     a rail glyph or a chrome control that stopped being reachable, or a header press that stopped
//     reaching the window that drags on it, is the same feature failing quietly;
//   * the shadow — the band is the shadow's room, and the two readings that matter are that it fades instead
//     of stepping, and that by the time it reaches the surface's own edge only a tail is left there. A band
//     narrowed for any reason puts the body of the shadow against the window border instead: a hard line, and
//     the cheapest of these to miss.
//
// The shape and the shadow cannot be read out of one render, because a drop shadow paints into the very band
// the corner tests measure: "nothing is painted here" is only a claim about a shadow-free window. So the
// window is drawn twice, once with the effect switched off and once with it on, and every reading below is
// made from the picture it can be trusted in.
// As with the stats panel, the light theme proves what the dark one cannot: there the rail is #E5E7EB
// against a #F3F4F6 shell, so a leaked corner is a grey pixel outside the arc, while the dark sheet paints
// both #0B0C10 and hides the mistake in a matching colour. Every reading below therefore runs in both
// themes, against the colours each sheet declares rather than colours written here.
// The window with its surface refresh counted. The refresh itself is left to the base, deliberately: a
// probe that only counted would let the real implementation rot, and what the scenario has to prove is
// that the event asks for the work — not that a test can watch itself ask.
class SurfaceProbe : public MainWindow
{
public:
    using MainWindow::MainWindow;
    int refreshes = 0;

protected:
    void refreshWindowSurface() override
    {
        ++refreshes;
        MainWindow::refreshWindowSurface();
    }
};

static void testWindowFrame()
{
    std::printf("\n[Rounded shell]\n");

    QApplication &app = *static_cast<QApplication *>(qApp);
    const int r = MainWindow::kWindowFrameRadiusPx;
    const int m = MainWindow::kWindowFrameMarginPx;
    const int b = MainWindow::kWindowFrameBorderPx;

    // The numbers the shell and its shadow are made of, spelled once more here so a change to one of them
    // has to pass two places that would otherwise disagree quietly.
    check(r == 12 && b == 1, "the shell is 12 px of arc over a 1 px stroke");
    // The band is not a free design value any more: it is the shadow's reach, and the shadow is the reason
    // the surface's own edge no longer falls inside it. QGraphicsDropShadowEffect blurs with a radius of half
    // its blurRadius and the offset pushes the whole curve down, so the band is that reach plus a pixel. The
    // tail does keep going past the band — as a tail; the ring reading below is what says so.
    const int reach = MainWindow::kWindowShadowBlurPx / 2;
    check(m == reach + MainWindow::kWindowShadowOffsetYpx + 1,
          QStringLiteral("…inside a band of exactly the reach the shadow needs (%1 px of blur radius, %2 px "
                         "of offset, %3 px over the end of it = %4 px)")
              .arg(reach).arg(MainWindow::kWindowShadowOffsetYpx).arg(1).arg(m));
    check(m > reach, QStringLiteral("…which does mean the band is wider than the stroke it protects (%1 px "
                                    "of it is only there for the shadow)").arg(m));

    if (!configRedirectedIntoBuildTree()) {
        std::printf("  SKIP  the shell belongs to a window, and constructing one needs XDG_CONFIG_HOME "
                    "redirected into the build tree (run it through ctest)\n");
        return;
    }

    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;
    const QString savedPath = store.zapretPath();
    const QString savedLast = store.lastStrategyId();
    const bool savedStart = store.startLastOnLaunch();
    store.setStartLastOnLaunch(false);   // nothing may start itself while its corners are being read
    store.setLastStrategyId(QString());
    store.setZapretPath(root);

    // ---- a window-state change owes the surface a full refresh ----
    //
    // The window is frameless and translucent. It used to carry a QGraphicsDropShadowEffect on its canvas,
    // which meant the whole surface — the power button included — was being drawn out of a pixmap the
    // compositor kept between paints; that cache is the shape of a stale frame after a minimize/restore,
    // and the reported ghost was a second, unclickable power button standing beside the real one. The shadow
    // is painted from geometry now, so there is no such cache for the window's own content. What the refresh
    // still has to reach is the effects that remain — Motion's breath on a Busy control, a chip's glow —
    // and what it must not do is disturb the shadow, which is no longer an object it could invalidate.
    {
        SurfaceProbe probe;
        probe.show();
        QTest::qWait(60);
        QWidget *canvas = probe.findChild<QWidget *>(QStringLiteral("rootCanvas"));
        check(canvas != nullptr && canvas->graphicsEffect() == nullptr,
              "the canvas owns no graphics effect at all — the shadow is painted by the window, so there is "
              "no cached copy of the window's content for a restore to leave on the desktop");

        // Read against the count the probe arrived at rather than against zero: showing a window can hand
        // it a state change of its own, and the claim is about the events this scenario sends.
        const int before = probe.refreshes;
        QEvent stateChange(QEvent::WindowStateChange);
        QCoreApplication::sendEvent(&probe, &stateChange);
        check(probe.refreshes == before + 1,
              "a window-state change still asks for the whole surface to be refreshed, because the effects "
              "that remain are still caches, and a restore is still when one can be stale");

        QEvent activation(QEvent::ActivationChange);
        QCoreApplication::sendEvent(&probe, &activation);
        QEvent fontChange(QEvent::FontChange);
        QCoreApplication::sendEvent(&probe, &fontChange);
        check(probe.refreshes == before + 1,
              "…and nothing else asks: an activation and a font change are not a surface whose geometry "
              "just moved, and repainting the whole window for either would be a full refresh per event");

        // The shadow, read as pixels rather than as an object. A refresh that repainted the surface without
        // it would look identical in every assertion above and would take the window's whole silhouette
        // claim away on the user's next minimize.
        const QImage after = probe.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const int m = MainWindow::kWindowFrameMarginPx;
        const QPoint justOutside(m - 1, after.height() / 2);      // one pixel out from the card's stroke
        const QColor ink = after.pixelColor(justOutside);
        check(qAlpha(after.pixel(justOutside)) >= 40
                      && ink.red() <= 8 && ink.green() <= 8 && ink.blue() <= 8,
              QStringLiteral("…and after the refresh the band one pixel off the stroke still carries the "
                             "shadow the curve names for that distance (%1 of black)")
                  .arg(qAlpha(after.pixel(justOutside))));
        check(probe.centralWidget() == canvas,
              "…and the canvas it repaints is still the window's own, not a replacement");
        probe.hide();
    }

    {
        MainWindow window;
        // ---- the hierarchy the rest of the suite reads through ----
        QWidget *canvas = window.findChild<QWidget *>(QStringLiteral("rootCanvas"));
        QFrame *shell = window.windowFrame();
        check(canvas != nullptr && canvas->parentWidget() == &window,
              "the central widget is still the window's own canvas");
        check(shell != nullptr, "the window builds a rounded shell");
        if (!canvas || !shell) {
            store.setZapretPath(savedPath);
            store.setLastStrategyId(savedLast);
            store.setStartLastOnLaunch(savedStart);
            return;
        }
        check(shell->objectName() == QLatin1String("mainWindowFrame"),
              "…under the name both sheets style it by");
        check(shell->parentWidget() == canvas,
              "…between that canvas and everything else, which is the one thing the new layer changes");

        // ---- the shadow, as a declaration ----
        //
        // There is no object left to read these numbers off, and that absence is the fix: a
        // QGraphicsDropShadowEffect can only blur what it has first rendered, so while one was attached to
        // the canvas — the widget whose rect is the whole surface — the window's own content was parked in a
        // pixmap that later paints were drawn from. The parameters therefore live in the curve the painter
        // walks, and this is where the file's own claim about them is checked: that it is the shadow the
        // constants name, and that it ends where the band says it must.
        check(canvas->graphicsEffect() == nullptr,
              "the canvas carries no effect: its rect is the whole surface, and a shadow rendered from that "
              "would cache the window's content along with its own edges");
        check(shell->graphicsEffect() == nullptr && window.graphicsEffect() == nullptr,
              "…and neither does the card nor the window, so nothing in the layer chain is holding a "
              "pixmap of anything");
        check(MainWindow::windowShadowReachPx()
                      == MainWindow::kWindowShadowBlurPx / 2.0 + MainWindow::kWindowShadowOffsetYpx,
              "the reach is the room the band was widened for, so the shadow cannot end past the surface");
        check(MainWindow::windowShadowAlphaAt(0.0) == MainWindow::kWindowShadowAlpha
                      && MainWindow::windowShadowAlphaAt(-1.0) == MainWindow::kWindowShadowAlpha,
              "the peak is the alpha the effect was given, and held across the strip the offset drops past "
              "the card's own edge");
        check(MainWindow::windowShadowAlphaAt(MainWindow::windowShadowReachPx()) == 0,
              "…and the curve is spent by the reach the band guarantees, rather than being cut there");
        // "Never increases" is the shape; "strictly falls while anything is left to see" is the point. A
        // cubic run out to integers goes flat at 1 and 0 long before the reach, and a check that demanded
        // a strict fall there would be a check about rounding rather than about the shadow.
        bool monotone = true;
        bool fallsWhileVisible = true;
        for (qreal d = 0.5; d < MainWindow::windowShadowReachPx(); d += 1.0) {
            const int here = MainWindow::windowShadowAlphaAt(d);
            const int next = MainWindow::windowShadowAlphaAt(d + 1.0);
            if (next > here)
                monotone = false;
            if (here >= 2 && next >= here)
                fallsWhileVisible = false;
        }
        check(monotone && fallsWhileVisible,
              "the fall-off falls at every step of it while anything is left to see: a shadow that steps "
              "instead of fading is the artefact the band exists to make invisible");
        // The peak sits below the threshold every shape test below reads band pixels at, which is what lets
        // one render carry both claims: a shadow cannot be mistaken for an app pixel by a >128 count.
        check(MainWindow::kWindowShadowAlpha <= 128,
              "the shadow's strongest pixel is dimmer than the opacity a corner test counts as painted, so "
              "the silhouette and the shadow can be read from the same picture");
        check(MainWindow::windowShadowPassCount() >= int(MainWindow::windowShadowReachPx()),
              "one pass per pixel of the reach, which is what makes the painted band the declared one");
        // How much of the window is still composited by an effect, and which layers. The claim the ghost
        // turns on is about the layers whose rect IS the surface — window, canvas, card — because only one
        // of those could be holding a pixmap of the whole window, and of the button inside it. What remains
        // are glows on small widgets inside the card, which can at worst cache a row of the list; counted
        // and said, so a report can be read against a number rather than against a promise.
        int attached = 0;
        const QList<QWidget *> tree = window.findChildren<QWidget *>() << &window;
        for (QWidget *w : tree) {
            if (w->graphicsEffect() != nullptr)
                ++attached;
        }
        check(window.graphicsEffect() == nullptr && canvas->graphicsEffect() == nullptr
                      && shell->graphicsEffect() == nullptr,
              QStringLiteral("no effect is attached to any layer that spans the surface, so nothing in this "
                             "process is holding a cached picture of the window a restore could leave "
                             "behind (%1 smaller effects remain, none of them surface-sized)")
                  .arg(attached));
        // The claim Task 3 makes is that findChild() still reaches everything, because it walks the tree
        // rather than naming a parent. Four kinds of thing the suite leans on: the page stack, a rail
        // glyph, the footer at the bottom of the column, and a page built several levels below the shell.
        check(window.findChild<QStackedWidget *>(QStringLiteral("pageStack")) != nullptr,
              "the page stack is reachable through the shell");
        check(window.findChild<QPushButton *>(QStringLiteral("navButton")) != nullptr,
              "…and so is a rail glyph");
        check(window.findChild<QFrame *>(QStringLiteral("footerBar")) != nullptr,
              "…and the footer at the bottom of the column");
        check(window.findChild<QWidget *>(QStringLiteral("historyPage")) != nullptr,
              "…and a page several levels below the shell");

        // A claim about a shape needs the window on screen and rendered. The offscreen platform is enough:
        // grab() draws the widget tree into its own image and asks no compositor for anything. Showing it
        // is also what puts the layout to work, so every geometry read below comes after it.
        window.show();
        QTest::qWait(60);

        // ---- the band, as geometry ----
        check(shell->pos() == QPoint(m, m),
              QStringLiteral("the shell starts %1 px in from the surface on both axes (%2,%3)")
                  .arg(m).arg(shell->pos().x()).arg(shell->pos().y()));
        check(shell->width() == window.width() - 2 * m
                  && shell->height() == window.height() - 2 * m,
              QStringLiteral("…and stops %1 px before it again (%2x%3 inside a %4x%5 window)")
                  .arg(m).arg(shell->width()).arg(shell->height())
                  .arg(window.width()).arg(window.height()));

        // ---- the chrome controls, where the design puts them ----
        QToolButton *closeBtn = nullptr;
        QToolButton *minBtn = nullptr;
        const auto chromeButtons = window.findChildren<QToolButton *>();
        for (QToolButton *btn : chromeButtons) {
            const QString role = btn->property("role").toString();
            if (role == QLatin1String("close"))
                closeBtn = btn;
            else if (role == QLatin1String("min"))
                minBtn = btn;
        }
        check(closeBtn != nullptr && minBtn != nullptr,
              "the minimise and close controls are still built into the header");
        // Inside the card, clear of its stroke: a control that overlapped the 1 px border would be the
        // design's top-right corner cut away by its own button.
        const QRect shellInner = QRect(shell->pos(), shell->size())
                                     .adjusted(b, b, -b - 1, -b - 1);
        for (QToolButton *btn : {closeBtn, minBtn}) {
            if (!btn)
                continue;
            const QRect g(btn->mapTo(&window, QPoint(0, 0)), btn->size());
            check(shellInner.contains(g),
                  QStringLiteral("the %1 control sits inside the shell, clear of its stroke")
                      .arg(btn->property("role").toString()));
            check(g.center().x() > window.width() / 2 && g.center().y() < window.height() / 4,
                  QStringLiteral("…and still in the top-right corner, where window furniture goes (%1,%2)")
                      .arg(g.x()).arg(g.y()));
        }
        const int w = window.width();
        const int h = window.height();
        // One mirrored coordinate for all four corners: (lx, ly) counts away from the corner in question,
        // which puts every corner's arc centre at (m + r, m + r) and lets one arithmetic read all of them.
        struct Corner {
            const char *name;
            int x0;   // the window x of the corner's own pixel
            int dx;   // …and the direction that goes into the window
            int y0;
            int dy;
            const char *surface;   // the selector whose colour must be found inside the arc
        };
        const Corner corners[] = {
            {"top-left",      0, 1,      0,  1, "QFrame#sidebar"},
            {"top-right",  w - 1, -1,     0,  1, "QFrame#mainArea"},
            {"bottom-left",   0, 1,   h - 1, -1, "QFrame#sidebar"},
            {"bottom-right", w - 1, -1, h - 1, -1, "QFrame#mainArea"},
        };
        auto at = [](const Corner &c, int lx, int ly) {
            return QPoint(c.x0 + c.dx * lx, c.y0 + c.dy * ly);
        };
        // A pixel of the corner box is painted by the shell exactly when its centre falls inside the outer
        // arc. A square corner paints every one of them, which is what the count below tells apart.
        auto arcPaints = [&](int lx, int ly) {
            const double dx = lx + 0.5 - (m + r);
            const double dy = ly + 0.5 - (m + r);
            return dx * dx + dy * dy <= double(r * r);
        };
        // How far in from the corner the 45° diagonal crosses the arc, in that mirrored coordinate: the
        // pixel that must be nothing if the corner is round, and something if any child left it square.
        const int diagOut = m + r - int((r + 1) / 1.4142135623730951) - 1;
        auto rgbWithin = [](const QColor &x, const QColor &y, int tol) {
            return qAbs(x.red() - y.red()) <= tol && qAbs(x.green() - y.green()) <= tol
                   && qAbs(x.blue() - y.blue()) <= tol;
        };
        // One picture, not two. The band used to be readable as emptiness because the shadow could be
        // switched off, and there is nothing left to switch off — that absence is the change. So the
        // silhouette and its shadow are read from the render the user sees, and the claim that keeps its
        // teeth is told apart by colour instead: a hand-painted shadow is black, and a child corner that
        // forgot to round carries the app's own surface. The counts below that decide "arc or square" stay
        // sound on the shared picture for the reason the numbers above pin — the shadow's peak is dimmer
        // than the alpha those counts call painted.
        auto appInk = [](const QImage &img, const QPoint &p) {
            if (!img.rect().contains(p) || qAlpha(img.pixel(p)) == 0)
                return false;                       // nothing there, or nothing but the desktop
            const QColor c = img.pixelColor(p);
            return c.red() > 8 || c.green() > 8 || c.blue() > 8;   // black under any alpha is a shadow
        };

        for (const bool dark : {true, false}) {
            const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
            Theme::apply(app, dark);
            QTest::qWait(30);
            const QImage view = window.grab().toImage().convertToFormat(QImage::Format_ARGB32);
            check(view.hasAlphaChannel() && view.width() == w && view.height() == h,
                  QStringLiteral("%1: the window renders %2x%3 with an alpha channel for corners to hide in")
                      .arg(theme).arg(view.width()).arg(view.height()));
            if (!view.hasAlphaChannel() || view.width() != w || view.height() != h)
                continue;

            const QString sheet = app.styleSheet();
            const QColor stroke = sheetRuleHex(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                                               QStringLiteral("border"));
            check(stroke.isValid(),
                  QStringLiteral("%1: the sheet declares the shell's stroke colour").arg(theme));

            // ---- the four corners ----
            for (const Corner &c : corners) {
                const QColor wantFill = sheetRuleColor(sheet, QLatin1String(c.surface),
                                                       QStringLiteral("background-color"));
                check(wantFill.isValid(),
                      QStringLiteral("%1: the sheet says what %2 paints at a corner")
                          .arg(theme, QLatin1String(c.surface)));

                // Nothing of the app may be at the window's very corner pixel…
                check(qAlpha(view.pixel(at(c, 0, 0))) == 0,
                      QStringLiteral("%1: the window's %2 corner pixel is empty, not %3")
                          .arg(theme, QLatin1String(c.name))
                          .arg(view.pixelColor(at(c, 0, 0)).name(QColor::HexRgb)));
                // …nor one step further in along the diagonal, which is exactly where a square child
                // corner — an unrounded rail, band or footer — would still be reaching. The shadow is in the
                // band here too now that one render carries both claims, so what is tested is the thing that
                // still separates them: the child would put the app's own colour there, and a shadow is black.
                const QPoint out = at(c, diagOut, diagOut);
                check(!appInk(view, out),
                      QStringLiteral("%1: nothing but the shadow lies at (%3,%4) of the %2 corner, so the "
                                     "corner is an arc and not a child that forgot to round")
                          .arg(theme, QLatin1String(c.name)).arg(out.x()).arg(out.y()));
                // And the corner is filled with its own surface, not a hole in the shell: proof the clean
                // diagonal is a shape rather than a window that stopped drawing. Four pixels short of the
                // arc's own centre along each axis puts the sample well inside the arc, clear of the stroke
                // and of the antialiased edge alike — counted from the corner of the CARD, since the band
                // between the card and the surface is a window's worth of pixels that must not be measured.
                const QPoint inside = at(c, m + r - 4, m + r - 4);
                check(rgbWithin(view.pixelColor(inside), wantFill, 12),
                      QStringLiteral("%1: the %2 corner is painted by %3 (%4 at (%5,%6), wanted %7)")
                          .arg(theme, QLatin1String(c.name), QLatin1String(c.surface))
                          .arg(view.pixelColor(inside).name(QColor::HexRgb))
                          .arg(inside.x()).arg(inside.y())
                          .arg(wantFill.name(QColor::HexRgb)));

                int painted = 0;
                int expected = 0;
                // The r x r box between the arc's centre and the corner it rounds — the one box whose
                // painted count separates an arc (about three quarters of it) from a square child (all of
                // it). It starts at the band rather than at the surface, so widening the band moves the box
                // with it instead of leaving it counting an empty margin twice over.
                for (int ly = m; ly < m + r; ++ly) {
                    for (int lx = m; lx < m + r; ++lx) {
                        if (qAlpha(view.pixel(at(c, lx, ly))) > 128)
                            ++painted;
                        if (arcPaints(lx, ly))
                            ++expected;
                    }
                }
                check(qAbs(painted - expected) <= 14,
                      QStringLiteral("%1: the %2 corner is the arc, not a square — %3 of the %4x%4 box "
                                     "painted, the arc says %5 and a square would be %6")
                          .arg(theme, QLatin1String(c.name)).arg(painted).arg(r)
                          .arg(expected).arg(r * r));
            }
            // ---- the four straight edges: the band, as pixels ----
            // Nothing antialiased to argue with here, and this is the reading that proves the margin: the
            // first pixel the window paints on an edge has to be the shell's stroke, one band in from the
            // surface's own edge. Flush against that edge, the same stroke is half a pixel of smudge.
            struct Edge {
                const char *name;
                int dx;   // which axis is the surface normal, and in which direction
                int dy;
            };
            const Edge edges[] = {{"top", 0, -1}, {"bottom", 0, 1}, {"left", -1, 0}, {"right", 1, 0}};
            for (const Edge &e : edges) {
                // The stroke pixel, and the one before it on the way out of the window.
                const QPoint on(e.dx < 0 ? m : (e.dx > 0 ? w - 1 - m : w / 2),
                                e.dy < 0 ? m : (e.dy > 0 ? h - 1 - m : h / 2));
                const QPoint out(on.x() + e.dx, on.y() + e.dy);
                // The band one pixel out from the stroke: shadow yes, app no — the same separation the
                // corners are read by, and the reading that still proves the inset, because the stroke
                // itself is the next pixel in and is measured at `on` below.
                check(!appInk(view, out),
                      QStringLiteral("%1: the %2 edge's %3 px band carries nothing but the shadow, so the "
                                     "window really is inset from its own surface")
                          .arg(theme, QLatin1String(e.name)).arg(m));
                check(qAlpha(view.pixel(on)) > 200 && rgbWithin(view.pixelColor(on), stroke, 16),
                      QStringLiteral("%1: the %2 edge draws the shell's stroke at (%3,%4) — %5, wanted %6")
                          .arg(theme, QLatin1String(e.name)).arg(on.x()).arg(on.y())
                          .arg(view.pixelColor(on).name(QColor::HexRgb))
                          .arg(stroke.isValid() ? stroke.name(QColor::HexRgb)
                                                : QStringLiteral("<none>")));
            }

            // ---- the shadow in that band ----
            //
            // Three readings, in the order anyone notices them: that there is shadow at all, that it falls
            // off instead of ending in a step, and that it is spent before it reaches the surface's own edge.
            // The last is what the band's width exists for, and the first to fail if the curve grows or the
            // margin shrinks without the other being asked.
            auto bandAlpha = [&](const QPoint &p) { return qAlpha(view.pixel(p)); };
            int maxNear = 0;
            for (const Edge &e : edges) {
                // The same mid-edge column the band is measured on, walked outward from the stroke. Half a
                // blur radius out is the far sample: still inside the fade on every edge, because the offset
                // only ever lengthens the shadow's reach downward and never shortens it below the radius.
                const QPoint on(e.dx < 0 ? m : (e.dx > 0 ? w - 1 - m : w / 2),
                                e.dy < 0 ? m : (e.dy > 0 ? h - 1 - m : h / 2));
                const QPoint near(on.x() + e.dx, on.y() + e.dy);
                const QPoint far(on.x() + e.dx * (reach / 2), on.y() + e.dy * (reach / 2));
                maxNear = qMax(maxNear, bandAlpha(near));
                check(bandAlpha(near) >= 16,
                      QStringLiteral("%1: the %2 edge casts into the band — %3 of alpha one pixel outside "
                                     "the stroke").arg(theme, QLatin1String(e.name)).arg(bandAlpha(near)));
                // …and on the two edges the offset runs along, read against the declared curve rather than
                // against a threshold. The pixel one out from the stroke is painted by the pass whose centre
                // sits half a pixel out, so a gap here means the painter and the curve have drifted apart —
                // the one way this feature can go wrong while every individual number still looks right.
                if (e.dx != 0) {
                    const int want = MainWindow::windowShadowAlphaAt(0.5);
                    check(qAbs(bandAlpha(near) - want) <= 12,
                          QStringLiteral("%1: the %2 band is the declared curve, not an impression of one — "
                                         "%3 measured where the curve says %4")
                              .arg(theme, QLatin1String(e.name)).arg(bandAlpha(near)).arg(want));
                }
                check(bandAlpha(near) <= MainWindow::kWindowShadowAlpha,
                      QStringLiteral("%1: …and never more opaque than the %2 of black the shadow was given "
                                     "(%3 here on the %4 edge)")
                          .arg(theme).arg(MainWindow::kWindowShadowAlpha).arg(bandAlpha(near))
                          .arg(QLatin1String(e.name)));
                check(bandAlpha(near) > bandAlpha(far),
                      QStringLiteral("%1: the %2 shadow fades rather than steps (%3 at the stroke, %4 half a "
                                     "radius out)")
                          .arg(theme, QLatin1String(e.name)).arg(bandAlpha(near)).arg(bandAlpha(far)));
                const QColor ink = view.pixelColor(near);
                check(ink.red() <= 8 && ink.green() <= 8 && ink.blue() <= 8,
                      QStringLiteral("%1: the %2 shadow is black, not a colour of the app's own (%3)")
                          .arg(theme, QLatin1String(e.name)).arg(ink.name(QColor::HexArgb)));
            }

            // Every pixel of the surface's own outer ring, no exceptions, and no claim of exact zero: a
            // Gaussian tail is still falling when it arrives, so what the band has to guarantee is that the
            // shadow is *nothing there is a chance of seeing* by then — under a tenth of the black it was made
            // of, and under an eighth of its own strength one pixel off the stroke. Measured, the band ends at
            // four of alpha here and seventy-five at the stroke. Narrow it to six pixels and the ring stops
            // being a tail and starts being the shadow's body — thirty-seven is a step you can see on a light
            // desktop, and that is exactly the cut this reading is here to hear about.
            int ringMax = 0;
            for (int x = 0; x < w; ++x) {
                ringMax = qMax(ringMax, bandAlpha(QPoint(x, 0)));
                ringMax = qMax(ringMax, bandAlpha(QPoint(x, h - 1)));
            }
            for (int y = 0; y < h; ++y) {
                ringMax = qMax(ringMax, bandAlpha(QPoint(0, y)));
                ringMax = qMax(ringMax, bandAlpha(QPoint(w - 1, y)));
            }
            check(ringMax <= MainWindow::kWindowShadowAlpha / 10,
                  QStringLiteral("%1: the shadow is spent at the surface edge — its strongest ring pixel is "
                                 "%2 of the %3 it was given, and a cut would read as a step")
                      .arg(theme).arg(ringMax).arg(MainWindow::kWindowShadowAlpha));
            check(ringMax * 8 <= maxNear,
                  QStringLiteral("%1: …and that ring pixel is a tail, not the shadow itself (%2 against the "
                                 "%3 it carries one pixel off the stroke)")
                      .arg(theme).arg(ringMax).arg(maxNear));

            // The four corners of the cast window in particular: a shadow that pooled in the surface's corner
            // would square the window again in the one place the whole shell exists to round it.
            int cornerMax = 0;
            for (const QPoint &p : {QPoint(0, 0), QPoint(w - 1, 0), QPoint(0, h - 1), QPoint(w - 1, h - 1)})
                cornerMax = qMax(cornerMax, bandAlpha(p));
            check(cornerMax <= MainWindow::kWindowShadowAlpha / 20,
                  QStringLiteral("%1: the shadow leaves the window's corners empty, so the silhouette is "
                                 "still the arc (%2 at the strongest)")
                      .arg(theme).arg(cornerMax));

            // And the shadow falls outside the card and nowhere else. A drop shadow that also laid over the
            // app's own pixels would read as a dirty window, and it would be the cheapest way for this
            // feature to get the theme's colours wrong while every corner reading above still passed.
            QFrame *rail = window.findChild<QFrame *>(QStringLiteral("sidebar"));
            if (rail) {
                const QPoint railCentre =
                    rail->mapTo(&window, QPoint(rail->width() / 2, rail->height() / 2));
                check(view.rect().contains(railCentre) && qAlpha(view.pixel(railCentre)) == 255,
                      QStringLiteral("%1: the middle of the rail is still fully opaque under the shadow")
                          .arg(theme));
                // …and the colour of it, against the sheet rather than against a second render: the pair
                // of pictures this used to compare no longer exists, and what the claim was always about
                // is whether the shadow laid itself over the app's own pixels.
                const QColor railWant = sheetRuleColor(sheet, QStringLiteral("QFrame#sidebar"),
                                                       QStringLiteral("background-color"));
                check(railWant.isValid() && rgbWithin(view.pixelColor(railCentre), railWant, 3),
                      QStringLiteral("%1: …and the shadow has not tinted it (%2 painted, the sheet says %3)")
                          .arg(theme).arg(view.pixelColor(railCentre).name(QColor::HexRgb))
                          .arg(railWant.name(QColor::HexRgb)));
            }

            // ---- the chrome controls, inside the silhouette ----
            // Geometry proved they are clear of the stroke; this proves it in the same pixels the corners
            // are judged by: every corner of every control lands on a painted pixel, so none of them is
            // sticking out past the arc the window is made of.
            for (QToolButton *btn : {closeBtn, minBtn}) {
                if (!btn)
                    continue;
                const QRect g(btn->mapTo(&window, QPoint(0, 0)), btn->size());
                bool allPainted = true;
                for (const QPoint &p : {g.topLeft(), g.topRight(), g.bottomLeft(), g.bottomRight()}) {
                    if (!view.rect().contains(p) || qAlpha(view.pixel(p)) == 0)
                        allPainted = false;
                }
                check(allPainted,
                      QStringLiteral("%1: every corner of the %2 control lands on painted pixels, so it is "
                                     "inside the rounded area")
                          .arg(theme, btn->property("role").toString()));
            }

            // The shell as the user sees it, kept for the eye to check the arc it cannot measure. There is
            // no second picture beside it any more: the shadow is part of this one, which is the change.
            view.save(dark ? QStringLiteral("/tmp/shell-dark.png")
                           : QStringLiteral("/tmp/shell-light.png"));
        }

        // ---- the drag, across the new layer ----
        //
        // A press on bare header space is not a press on the window: it lands on the band, is ignored
        // there, and travels up through the shell to the window that starts a drag from it. The shell put
        // a layer into that journey, and a frameless window that cannot be dragged is a window that cannot
        // be moved at all — a much worse failure than a wrong colour. So the press is delivered to a child
        // and the window's own position is what is measured.
        Theme::apply(app, true);
        QTest::qWait(30);
        QWidget *band = window.findChild<QWidget *>(QStringLiteral("headerBar"));
        check(band != nullptr, "the header band is reachable by name for the drag to start on");
        if (band) {
            check(band->width() > 200 && band->height() > 24,
                  QStringLiteral("…and is big enough to grab by (%1x%2)").arg(band->width())
                      .arg(band->height()));
            const QPoint grabPoint(band->width() / 2, 20);   // between the title and the status chip
            const QRect closeInBand = closeBtn
                                          ? QRect(closeBtn->mapTo(band, QPoint(0, 0)), closeBtn->size())
                                          : QRect();
            check(!closeInBand.contains(grabPoint),
                  "…the grab point is bare header space, not one of the controls sitting on it");

            const QPoint before = window.pos();
            const QPoint p1 = band->mapTo(&window, grabPoint);
            QTest::mousePress(band, Qt::LeftButton, Qt::NoModifier, grabPoint);
            const QPoint p2 = p1 + QPoint(27, 7);
            QTest::mouseMove(&window, p2);
            const QPoint moved = window.pos();
            check(moved != before,
                  QStringLiteral("a press on the header and a drag move the frameless window ((%1,%2) -> "
                                 "(%3,%4))").arg(before.x()).arg(before.y())
                      .arg(moved.x()).arg(moved.y()));
            check(moved - before == p2 - p1,
                  QStringLiteral("…by exactly as far as the pointer went, so the extra layer did not shift "
                                 "the grab point (window moved (%1,%2), pointer (%3,%4))")
                      .arg(moved.x() - before.x()).arg(moved.y() - before.y())
                      .arg(p2.x() - p1.x()).arg(p2.y() - p1.y()));
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, p2);

            // A second drag, measured from a second press. If the release had left the window mid-drag, or
            // the offset had carried over from the first press point, this jump would not be the
            // pointer's own.
            const QPoint againFrom = window.pos();
            const QPoint q1 = band->mapTo(&window, grabPoint);
            QTest::mousePress(band, Qt::LeftButton, Qt::NoModifier, grabPoint);
            QTest::mouseMove(&window, q1 + QPoint(-19, 11));
            const QPoint againTo = window.pos();
            check(againTo - againFrom == QPoint(-19, 11),
                  QStringLiteral("…and the next drag is its own, not a carry-over from the last (moved "
                                 "(%1,%2), expected (-19,11))")
                      .arg(againTo.x() - againFrom.x()).arg(againTo.y() - againFrom.y()));
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, q1 + QPoint(-19, 11));

            // And below the band a press belongs to the page rather than to the window: the same journey
            // up ends with the drag handler refusing it, so clicking anything never drags the app about.
            const QPoint still = window.pos();
            QWidget *page = window.findChild<QWidget *>(QStringLiteral("dashboardPage"));
            if (page && page->height() > 60) {
                const QPoint onPage(page->width() / 2, page->height() - 30);
                const QPoint onWindow = page->mapTo(&window, onPage);
                check(onWindow.y() > 64 + m + b,
                      QStringLiteral("…that press is below the drag band (%1 px into a %2 px window)")
                          .arg(onWindow.y()).arg(window.height()));
                QTest::mousePress(page, Qt::LeftButton, Qt::NoModifier, onPage);
                QTest::mouseMove(&window, onWindow + QPoint(30, 30));
                check(window.pos() == still,
                      "…and dragging from the page leaves the window where it was");
                QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                                    onWindow + QPoint(30, 30));
            }
        }

        // ---- the drag, from the band the shadow falls through ----
        //
        // The band is a window's worth of pixels wide now, because that is what the shadow needs, and every
        // one of them is a place the pointer can land without touching a widget. They have to stay grabbable:
        // a shadow is paint, and paint does not take the mouse. So the press goes to the canvas itself, in the
        // middle of the top band and on nothing but fall-off, and the window's own position is what proves
        // the drag reached the handler that moves it.
        {
            const QPoint inBand(w / 2, m / 2);
            check(inBand.y() > 0 && inBand.y() < m,
                  QStringLiteral("…the press point is in the band and not on the card (%1 px of %2)")
                      .arg(inBand.y()).arg(m));
            const QPoint before = window.pos();
            QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, inBand);
            const QPoint after = inBand + QPoint(15, -9);
            QTest::mouseMove(&window, after);
            const QPoint moved = window.pos();
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, after);
            check(moved - before == QPoint(15, -9),
                  QStringLiteral("a press on the shadowed band still drags the frameless window (moved "
                                 "(%1,%2), expected (15,-9))").arg(moved.x() - before.x())
                      .arg(moved.y() - before.y()));
        }

        // ---- the in-field glyph, across a theme switch ----
        //
        // The folder icon inside the settings page's path field is the app's only glyph that is neither a
        // QPushButton nor a QToolButton — a QLineEdit draws a QAction through a button of its own — so the
        // window's re-bake pass has to be told about actions as well. A colour baked into a pixmap does not
        // follow the sheet by itself, and the failure would be invisible to every assertion that looks only
        // at properties: the glyph would simply stay the colour of the theme the page was built in, which is
        // dark-on-light the moment the user picks the light theme.
        {
            SettingsPage *windowPage = window.findChild<SettingsPage *>();
            QAction *fieldAction = windowPage ? windowPage->browseAction() : nullptr;
            check(fieldAction != nullptr, "the window's settings page carries the in-field glyph");

            // A fingerprint of a glyph's colour, cheap and stable: the weighted sum of the pixels that
            // carry ink. Two tints of one shape differ here, and a missing icon lands on zero.
            auto inkOf = [](QAction *a) {
                const QImage img = a->icon().pixmap(16, 16).toImage()
                                       .convertToFormat(QImage::Format_ARGB32);
                quint64 ink = 0;
                for (int y = 0; y < img.height(); ++y) {
                    for (int x = 0; x < img.width(); ++x) {
                        if (qAlpha(img.pixel(x, y)) > 40) {
                            ink += quint64(qRed(img.pixel(x, y))) * 3
                                   + quint64(qGreen(img.pixel(x, y))) * 5
                                   + quint64(qBlue(img.pixel(x, y))) * 7;
                        }
                    }
                }
                return ink;
            };

            const QString savedTheme = window.settingsModel()->theme();
            window.settingsModel()->setTheme(QStringLiteral("dark"));
            QTest::qWait(30);
            const quint64 darkInk = fieldAction ? inkOf(fieldAction) : 0;
            window.settingsModel()->setTheme(QStringLiteral("light"));
            QTest::qWait(30);
            const quint64 lightInk = fieldAction ? inkOf(fieldAction) : 0;
            check(fieldAction && darkInk != lightInk && darkInk != 0 && lightInk != 0,
                  QStringLiteral("switching theme on the model re-bakes the glyph inside the field "
                                 "(%1 -> %2 of ink)").arg(darkInk).arg(lightInk));
            // And not merely differently: the new pixels are the ones the light sheet's own icon colour
            // paints, so the in-field glyph cannot drift from the icons beside it.
            const QImage wanted = Theme::themedIcon(QStringLiteral("folder"), 16)
                                      .pixmap(16, 16).toImage().convertToFormat(QImage::Format_ARGB32);
            quint64 wantedInk = 0;
            for (int y = 0; y < wanted.height(); ++y) {
                for (int x = 0; x < wanted.width(); ++x) {
                    if (qAlpha(wanted.pixel(x, y)) > 40) {
                        wantedInk += quint64(qRed(wanted.pixel(x, y))) * 3
                                     + quint64(qGreen(wanted.pixel(x, y))) * 5
                                     + quint64(qBlue(wanted.pixel(x, y))) * 7;
                    }
                }
            }
            check(fieldAction && lightInk == wantedInk,
                  "…to the light theme's own icon colour, not to some third tint");
            window.settingsModel()->setTheme(savedTheme);
            QTest::qWait(30);
        }

        // Back on the theme the rest of the binary — and the application itself — expects.
        Theme::apply(app, true);
        window.hide();
    }

    store.setZapretPath(savedPath);
    store.setLastStrategyId(savedLast);
    store.setStartLastOnLaunch(savedStart);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MuffinTest"));
    QApplication::setOrganizationName(QStringLiteral("MuffinTest"));

    std::printf("=== zapret GUI verification (offscreen) ===\n");
    std::printf("platform = %s\n", qPrintable(app.platformName()));

    Theme::apply(app, true);

    testSettings();
    testSettingsMigration();
    testLogPanel();
    testPingIndicator();
    testPowerButton();
    testModeToggle();
    testWarpMappings();
    testSelectionSwitch();
    testCombinedPower();
    testProfileCard();
    testTrayPolicy();
    testSettingsPageCards(app);
    testWindowPolicy();
    testLanguage();
    testTheme(app);
    testPingService();
    testDownloadDialog();
    testHistoryPage();
    testMotion();
    testWatchdog();
    testWarpEnrolmentWiring();
    testWarpSetupDialog();
    testUpdateNotice();
    testWindowFrame();

    std::printf("\n=== %s (%d failures) ===\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
