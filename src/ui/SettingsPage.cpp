#include "SettingsPage.h"

#include "DownloadDialog.h"
#include "LanguageManager.h"
#include "Settings.h"
#include "StrategyParser.h"
#include "Theme.h"
#include "WarpInstaller.h"
#include "ZapretLocator.h"
#include "ZapretPaths.h"
#include "ZapretSource.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace zapret {

namespace {
// How long the acknowledgement next to the Save button holds. Long enough to read, short enough that it
// cannot be mistaken for the page's opinion of the values currently in the fields.
constexpr int kSavedNoteMs = 2000;

// The width of a field that holds one short value. A form column stretches whatever is put in it, and a
// theme picker that spans the page reads as a sentence rather than as a choice of three words — the same
// reason the interval box does not need a line long enough to type a novel into.
constexpr int kNarrowFieldPx = 200;

// The two icons beside the path field. 32 px is the hit area the window's own chrome buttons use, so the
// pair reads as controls of the same rank as everything else in the app rather than as decorations; 16 px
// is the glyph that fills it without touching the rim.
constexpr int kPathToolPx = 32;
constexpr int kPathGlyphPx = 16;

// The card's own padding. A card is a surface, not a border: the gap between the stroke and the first row
// is what makes it read as raised, and the gap between two cards is wider than the gap between two rows
// inside one — the page groups by distance as much as by frame.
//
// These four numbers are measured against the window, not chosen by taste: the settings page is 794x514
// inside the fixed 900x700 canvas, which leaves the scroll area's viewport about 455 px, and the three
// cards' own height is what decides whether the last row is on screen at the default font or one flick
// away. At 12/6/6/10 they come out at that height, so the page ends on screen here; the scroll area is why
// being a few pixels taller elsewhere — a larger font, a language whose sentences are longer than
// Russian's — is a scrollbar rather than a clipped setting.
constexpr int kCardHPadPx = 20;
constexpr int kCardVPadPx = 12;
// The heading sits exactly as close to its first row as one row sits to the next: a card has one rhythm,
// and its grouping cue is the 10 px between cards, not a second internal gap.
constexpr int kCardTitleGapPx = 6;
constexpr int kCardRowGapPx = 6;
constexpr int kCardGapPx = 10;
// The commit row to the last card. Not kCardGapPx: the Save button is not a fifth card.
constexpr int kCommitGapPx = 6;

// The glyph each of the three ways to fill the field is drawn by. Named here rather than spelled inline:
// the same strings are what MainWindow::applyTheme() re-bakes the icons from on a theme switch, since a
// baked pixmap cannot recolour itself.
const char kFolderGlyph[] = "folder";
const char kSearchGlyph[] = "search";
const char kDownloadGlyph[] = "download-cloud";
} // namespace

SettingsPage::SettingsPage(Settings &settings, QWidget *parent)
    : QWidget(parent)
    , m_settings(settings)
{
    build();
    loadFromSettings();

    // The application can move the path without anyone touching this form — a download finishing, the
    // startup auto-location, the placeholder's button. Re-read on every write that was not ours.
    connect(&m_settings, &Settings::changed, this, [this] {
        if (!m_writing)
            loadFromSettings();
    });

    // The language is the one setting on this page that rewrites the page. When the rail's flag button
    // moves it, MainWindow re-reads its whole tree, and a page that only re-read its values would go on
    // showing its own labels — and its composed placeholder, which no single catalog key backs — in the
    // language the user has just left.
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged, this,
            [this](const QString &) {
                refreshPathPlaceholder();
                retranslateTree(this);
            });
}

SettingsPage::~SettingsPage() = default;

void SettingsPage::build()
{
    setObjectName(QStringLiteral("settingsPage"));
    setAttribute(Qt::WA_StyledBackground, true);

    // The cards are stacked inside a scroll area. Three titled surfaces with their own padding come close
    // to the height the fixed 900x700 window has left below its header, its footer and its own page
    // heading — and at a large font setting they exceed it, so a page that silently clipped its last card
    // would be one row shorter than the user's settings. The journal and the profile list already scroll
    // for the same reason.
    //
    // The commit row stays OUTSIDE the scroll: Save is the one control on this page that must never be
    // reachable only by scrolling to it, because a user who has just changed three values and cannot see
    // the button has no reason to know the write is still owed.
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *stack = new QWidget(scroll);
    auto *cards = new QVBoxLayout(stack);
    cards->setContentsMargins(0, 0, 0, 0);
    // The gap between two cards (10 px) is wider than the gap between two rows inside one (6 px) — the
    // page groups by distance as well as by frame, and a group the eye has to find the border of is a
    // group that failed.
    cards->setSpacing(kCardGapPx);
    scroll->setWidget(stack);

    const Card general = makeCard(stack, QStringLiteral("settings.group.general"));
    const Card uiCard = makeCard(stack, QStringLiteral("settings.group.interface"));
    const Card network = makeCard(stack, QStringLiteral("settings.group.network"));
    m_generalCard = general.frame;
    m_interfaceCard = uiCard.frame;
    m_networkCard = network.frame;

    // ---- the path card -------------------------------------------------------
    // Where zapret lives, and the only field on the page that has to hold a whole path.
    m_pathEdit = new QLineEdit(general.frame);
    // The field is at least as tall as the icons beside it. Without this the row's height is whatever the
    // font happens to need, and a machine whose fallback face is shorter than 32 px gets a squat field
    // with two buttons hanging above and below its centre — the same line, two different heights, which is
    // what "one control with two appendages" promises not to look like. It still grows past this when a
    // bigger font asks for more, and the icons stay centred on it (see the alignment below).
    m_pathEdit->setMinimumHeight(kPathToolPx);
    refreshPathPlaceholder();
    m_pathEdit->setClearButtonEnabled(true);

    // Three ways to fill one field, because three situations call for them: the tree is on this disk and
    // the user knows where, the tree is on this disk and nobody knows where, or there is no tree yet.
    //
    // All three used to be wide text buttons, which made this one row louder and taller than every other
    // row on the page and gave the field a quarter of the width left to it. Now the action that fills the
    // field with a folder is drawn IN it — a QLineEdit places and clips it, so it costs the text nothing —
    // and the two that are about the disk rather than about a folder stand beside it as 32 px icons, each
    // still saying what it does in words for the pointer and for the accessibility tree.
    m_browseAction = new QAction(general.frame);
    m_browseAction->setObjectName(QStringLiteral("browseAction"));
    m_browseAction->setProperty("glyph", QLatin1String(kFolderGlyph));
    m_browseAction->setProperty("glyphPx", kPathGlyphPx);
    m_browseAction->setIcon(Theme::themedIcon(QLatin1String(kFolderGlyph), kPathGlyphPx));
    i18nText(m_browseAction, QStringLiteral("settings.browse"));
    i18nTip(m_browseAction, QStringLiteral("settings.browse_tip"));
    m_pathEdit->addAction(m_browseAction, QLineEdit::TrailingPosition);

    m_detectButton = makePathTool(general.frame, QLatin1String(kSearchGlyph),
                                  QStringLiteral("settings.autodetect"),
                                  QStringLiteral("settings.autodetect_tip"));
    m_downloadButton = makePathTool(general.frame, QLatin1String(kDownloadGlyph),
                                    QStringLiteral("action.download_zapret"),
                                    QStringLiteral("action.download_zapret_tip"));

    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(8);
    pathRow->addWidget(m_pathEdit, 1);
    // The alignment is spelled out rather than left to the layout. A fixed-size widget in a taller row is
    // centred by Qt today, but that is QWidgetItem's arithmetic and not a promise: a row whose height is
    // set by a font this file cannot see would otherwise place the icons by whichever default the platform
    // picks, and a field with two buttons dangling off its vertical centre stops reading as one control.
    pathRow->addWidget(m_detectButton, 0, Qt::AlignVCenter);
    pathRow->addWidget(m_downloadButton, 0, Qt::AlignVCenter);

    general.form->addRow(rowLabel(general.frame, QStringLiteral("settings.path")), pathRow);

    // The verdict on the path, under the field rather than beside it: it is a sentence, and a sentence
    // squeezed into the remaining 200 px of a row wraps into unreadable confetti.
    m_pathState = new QLabel(general.frame);
    m_pathState->setWordWrap(true);
    general.form->addRow(QString(), m_pathState);

    // ---- what the application does on its own ---------------------------------
    // Still the general card: these are the settings about the program's own life — what it starts with,
    // what it hides into, which window it closes — and separating them from the folder they all act on
    // would have bought a fourth heading at the price of the last row fitting on screen.
    i18nText(m_startLastCheck = new QCheckBox(general.frame),
             QStringLiteral("settings.start_last"));
    i18nText(m_trayCheck = new QCheckBox(general.frame), QStringLiteral("settings.tray"));

    // A ticked box that cannot be honoured is worse than an explanation. Sessions without a StatusNotifier
    // host (bare GNOME, a plain Wayland compositor) report no tray at all, so spell out what closing the
    // window will actually do — MainWindow still retries, so the tray may appear later by itself.
    m_trayNote = new QLabel(general.frame);
    m_trayNote->setObjectName(QStringLiteral("trayNote"));
    i18nText(m_trayNote, QStringLiteral("settings.tray_note"));
    m_trayNote->setWordWrap(true);
    m_trayNote->setVisible(!QSystemTrayIcon::isSystemTrayAvailable());

    // The WARP window switch, built like the two above and hidden on a host with nothing to hide. The
    // Linux package installs a daemon and a CLI and no window at all, and a settings row promising to
    // close a window that cannot exist is a lie the page would tell on every machine that is not
    // Windows — hence platformHasWarpGui(), which is the same function the install path asks.
    i18nText(m_hideWarpCheck = new QCheckBox(general.frame), QStringLiteral("settings.hide_warp"));
    i18nTip(m_hideWarpCheck, QStringLiteral("settings.hide_warp_tip"));
    m_hideWarpCheck->setVisible(
        WarpInstaller::platformHasWarpGui(ZapretSource::hostPlatform()));

    // A switch names itself, so it is a label as much as a field — and a label belongs at the card's left
    // edge, under its heading, not indented into the column where a value it has no label for would go.
    general.body->addWidget(m_startLastCheck);
    general.body->addWidget(m_trayCheck);
    general.body->addWidget(m_trayNote);
    general.body->addWidget(m_hideWarpCheck);

    // ---- the interface card ---------------------------------------------------
    m_themeBox = new QComboBox(uiCard.frame);
    m_themeBox->setMaximumWidth(kNarrowFieldPx);
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.dark")), Theme::nameForMode(Theme::Mode::Dark));
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.light")), Theme::nameForMode(Theme::Mode::Light));
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.system")), Theme::nameForMode(Theme::Mode::System));
    i18nItems(m_themeBox, { QStringLiteral("theme.dark"), QStringLiteral("theme.light"),
                            QStringLiteral("theme.system") });

    // The language, in the same shape as the theme above it because it is the same kind of answer: one
    // choice out of a short list. The entries are deliberately NOT tagged with i18nItems — a language is
    // named in its own script in every language, which is the one thing about this picker that must not
    // be translated: the person who cannot read the current UI is exactly the person who has to recognise
    // their own. See LanguageManager::nativeName().
    m_languageBox = new QComboBox(uiCard.frame);
    m_languageBox->setObjectName(QStringLiteral("languageBox"));
    m_languageBox->setMaximumWidth(kNarrowFieldPx);
    for (const QString &code : LanguageManager::codes())
        m_languageBox->addItem(LanguageManager::nativeName(code), code);
    i18nTip(m_languageBox, QStringLiteral("settings.language_tip"));

    // Motion joins the two pickers rather than the switches above them because the card is about how the
    // interface behaves toward the eye: what it looks like, what it speaks, and whether it moves at all.
    i18nText(m_animationsCheck = new QCheckBox(uiCard.frame),
             QStringLiteral("settings.animations"));
    i18nTip(m_animationsCheck, QStringLiteral("settings.animations_tip"));

    // The short fields, held to the left. A QFormLayout gives a field the whole width of its column, which
    // is right for a path and wrong for a three-word choice: the card then reads as two long controls and
    // a lot of empty form. The stretch beside each one is what keeps it beside its label rather than
    // across the screen — and it is the same promise the row made when the page was one column.
    auto *themeField = new QHBoxLayout;
    themeField->setContentsMargins(0, 0, 0, 0);
    themeField->addWidget(m_themeBox);
    themeField->addStretch(1);

    auto *languageField = new QHBoxLayout;
    languageField->setContentsMargins(0, 0, 0, 0);
    languageField->addWidget(m_languageBox);
    languageField->addStretch(1);

    uiCard.form->addRow(rowLabel(uiCard.frame, QStringLiteral("settings.theme")), themeField);
    uiCard.form->addRow(rowLabel(uiCard.frame, QStringLiteral("settings.language")),
                         languageField);
    uiCard.body->addWidget(m_animationsCheck);

    // ---- the network card -----------------------------------------------------
    // The two cadences. Both are answers to a question about the same services, asked on two different
    // clocks, so they live in one card rather than at opposite ends of the page.
    m_pingSpin = new QSpinBox(network.frame);
    m_pingSpin->setMaximumWidth(kNarrowFieldPx);
    // No stepper arrows. They are the platform's own control, drawn by the style rather than by the
    // sheet, and the value they set is a number the user types or nudges once a year — the arrows cost
    // more attention than the field they belong to. Typing and the wheel still work.
    m_pingSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_pingSpin->setRange(3, 60);
    m_pingSpin->setSuffix(LanguageManager::tr(QStringLiteral("settings.ping_suffix")));
    i18nSuffix(m_pingSpin, QStringLiteral("settings.ping_suffix"));
    m_pingSpin->setSingleStep(1);

    // The watchdog's cadence, built like the box above because it is the same kind of answer: one short
    // number with a unit. Its range comes from Settings, not from a second pair of literals here — the
    // model clamps what it stores and the window arms a QTimer from that value, so three places would
    // otherwise have to agree on "1..60" by coincidence.
    m_watchdogSpin = new QSpinBox(network.frame);
    m_watchdogSpin->setMaximumWidth(kNarrowFieldPx);
    m_watchdogSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_watchdogSpin->setRange(Settings::kWatchdogMinIntervalMin, Settings::kWatchdogMaxIntervalMin);
    m_watchdogSpin->setSuffix(LanguageManager::tr(QStringLiteral("settings.watchdog_suffix")));
    i18nSuffix(m_watchdogSpin, QStringLiteral("settings.watchdog_suffix"));
    m_watchdogSpin->setSingleStep(1);
    i18nTip(m_watchdogSpin, QStringLiteral("settings.watchdog_tip"));

    // The short fields, held to the left. A QFormLayout gives a field the whole width of its column, which
    // is right for a path and wrong for a two-word measurement: the card then reads as two long controls
    // and a lot of empty form. The stretch beside each one is what keeps it beside its label rather than
    // across the screen.
    auto *pingField = new QHBoxLayout;
    pingField->setContentsMargins(0, 0, 0, 0);
    pingField->addWidget(m_pingSpin);
    pingField->addStretch(1);

    auto *watchdogField = new QHBoxLayout;
    watchdogField->setContentsMargins(0, 0, 0, 0);
    watchdogField->addWidget(m_watchdogSpin);
    watchdogField->addStretch(1);

    network.form->addRow(rowLabel(network.frame, QStringLiteral("settings.ping")), pingField);
    // Right under the ping interval, because that is what it is: a second cadence, measured in minutes
    // rather than seconds, for the same question about the same services.
    network.form->addRow(rowLabel(network.frame, QStringLiteral("settings.watchdog")), watchdogField);

    cards->addWidget(m_generalCard);
    cards->addWidget(m_interfaceCard);
    cards->addWidget(m_networkCard);
    cards->addStretch(1);

    // The commit row. The dialog's OK/Cancel pair is gone: a page has no frame to dismiss, so leaving it
    // is the cancel, and the only thing that has to be explicit is the write. The acknowledgement sits to
    // the left of the button it belongs to rather than in a footer, because a note the button did not
    // cause would be indistinguishable from a note about the values.
    m_savedNote = new QLabel(this);
    m_savedNote->setObjectName(QStringLiteral("savedNote"));
    m_savedNote->setTextFormat(Qt::PlainText);

    m_save = new QPushButton(this);
    m_save->setObjectName(QStringLiteral("primaryButton"));
    m_save->setCursor(Qt::PointingHandCursor);
    m_save->setAutoDefault(false);
    i18nText(m_save, QStringLiteral("settings.save"));

    auto *commit = new QHBoxLayout;
    commit->setContentsMargins(0, 0, 0, 0);
    commit->setSpacing(12);
    commit->addWidget(m_savedNote);
    commit->addStretch(1);
    commit->addWidget(m_save);

    m_savedTimer = new QTimer(this);
    m_savedTimer->setSingleShot(true);
    connect(m_savedTimer, &QTimer::timeout, this, [this] { m_savedNote->clear(); });

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(28, 10, 28, 10);
    // The commit row sits closer to the cards than the cards sit to each other, because it is not another
    // card — it is what closes them. The number also buys the last few pixels the three cards need to
    // finish above the fold at the default font.
    column->setSpacing(kCommitGapPx);
    column->addWidget(scroll, 1);
    column->addLayout(commit);

    connect(m_save, &QPushButton::clicked, this, &SettingsPage::apply);
    // The folder glyph is an action, so it is pressed through the action rather than through a button —
    // and the field also fires it on Enter-ish synthetic triggers, which is the same handler and the same
    // dialog, so there is one path to a folder and not two.
    connect(m_browseAction, &QAction::triggered, this, &SettingsPage::browse);
    connect(m_detectButton, &QToolButton::clicked, this, &SettingsPage::autoDetect);
    connect(m_downloadButton, &QToolButton::clicked, this, &SettingsPage::downloadZapret);
    connect(m_pathEdit, &QLineEdit::textChanged, this, &SettingsPage::revalidatePath);
}

// ---- the card furniture ------------------------------------------------------
//
// Three small builders rather than three long inline blocks, because a card's shape is decided once and
// then repeated three times: the same stroke, the same padding, the same heading, the same two columns.
// Written out three times it would be three chances to drift, and drifting is exactly how a page of
// "cards" ends up looking like three boxes somebody drew in a hurry.

SettingsPage::Card SettingsPage::makeCard(QWidget *host, const QString &titleKey)
{
    Card card;
    card.frame = new QFrame(host);
    card.frame->setObjectName(QStringLiteral("settingsCard"));

    auto *box = new QVBoxLayout(card.frame);
    box->setContentsMargins(kCardHPadPx, kCardVPadPx, kCardHPadPx, kCardVPadPx);
    // One rhythm inside the card: heading to first row is the same distance as row to row. What separates
    // a card from its neighbour is the gap outside it, not a second internal spacing.
    box->setSpacing(kCardTitleGapPx);
    card.body = box;

    auto *title = new QLabel(card.frame);
    title->setObjectName(QStringLiteral("settingsCardTitle"));
    i18nText(title, titleKey);
    box->addWidget(title);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(kCardRowGapPx);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    box->addLayout(form);
    card.form = form;

    return card;
}

QLabel *SettingsPage::rowLabel(QFrame *card, const QString &key)
{
    auto *label = new QLabel(card);
    label->setObjectName(QStringLiteral("settingsRowLabel"));
    label->setWordWrap(true);
    // Pinned rather than floored, and the difference is the whole point: a QFormLayout takes as much label
    // column as its widest label asks for, so three forms left to themselves would start the field column
    // at three different pixels — "Путь к zapret" short, "Интервал проверки обхода" long, and the page
    // ragged. A fixed width makes every card share one left edge for its fields.
    label->setMinimumWidth(kRowLabelPx);
    label->setMaximumWidth(kRowLabelPx);
    return i18nText(label, key);
}

QToolButton *SettingsPage::makePathTool(QFrame *card, const QString &glyph, const QString &nameKey,
                                        const QString &tipKey)
{
    auto *b = new QToolButton(card);
    b->setObjectName(QStringLiteral("pathToolButton"));
    // The two properties every other baked glyph in the window carries. applyTheme() re-bakes the icon
    // from them after a theme switch, because the colour is painted into the pixmap at load time and a
    // stylesheet rule cannot reach a pixmap the sheet never owned.
    b->setProperty("glyph", glyph);
    b->setProperty("glyphPx", kPathGlyphPx);
    b->setIcon(Theme::themedIcon(glyph, kPathGlyphPx));
    b->setIconSize(QSize(kPathGlyphPx, kPathGlyphPx));
    b->setFixedSize(kPathToolPx, kPathToolPx);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::TabFocus);
    // A control with no words on it still has to say what it does — twice, because the two audiences are
    // different: the tooltip answers a pointer that hovers, and the accessible name is the only thing a
    // screen reader has for a button whose entire face is a glyph.
    i18nAcc(b, nameKey);
    i18nTip(b, tipKey);
    return b;
}

void SettingsPage::refreshPathPlaceholder()
{
    // Two examples and the word between them, because the field accepts a tree for either platform and a
    // user on Windows reading only the first half would go looking for a linux/ folder. The word is the
    // only translated piece, which is why this is composed here rather than being one catalog key.
    m_pathEdit->setPlaceholderText(QStringLiteral("/home/…/zapret/linux ")
                                   + LanguageManager::tr(QStringLiteral("common.or"))
                                   + QStringLiteral(" C:\\...\\zapret\\windows"));
}

void SettingsPage::commitLanguage(const QString &code)
{
    // An unsupported code is refused by the catalog too, so storing it would be writing down a language
    // the interface cannot display — and the rail's flag menu would come back showing the old one.
    if (code.isEmpty() || !LanguageManager::isSupported(code))
        return;

    if (code != m_settings.language())
        m_settings.setLanguage(code);

    // The store is not the screen. main.cpp installs the catalog before the first widget exists and the
    // rail's flag button moves it on press; nothing re-reads the key on its own. A page that only wrote
    // it would therefore change nothing until the next launch — including the labels of the very card the
    // user had just used to change it.
    if (LanguageManager::instance().currentCode() != code)
        LanguageManager::instance().setCode(code);
}

void SettingsPage::loadFromSettings()
{
    const QString path = m_settings.zapretPath();
    m_pathEdit->setText(path);
    m_startLastCheck->setChecked(m_settings.startLastOnLaunch());
    m_trayCheck->setChecked(m_settings.minimizeToTray());
    m_animationsCheck->setChecked(m_settings.animationsEnabled());
    m_hideWarpCheck->setChecked(m_settings.hideWarpGui());
    m_pingSpin->setValue(m_settings.pingIntervalSec());
    m_watchdogSpin->setValue(m_settings.watchdogIntervalMin());
    m_trayNote->setVisible(!QSystemTrayIcon::isSystemTrayAvailable());

    const QString theme = m_settings.theme();
    const int index = m_themeBox->findData(theme);
    m_themeBox->setCurrentIndex(index >= 0 ? index : 0);

    // The same shape as the theme above it, and the same fallback: a store holding a code this build has
    // no catalog for (an ini edited by hand, or a value written by a future build that added a language)
    // resolves to the first entry rather than to an empty selection — the picker must always be showing
    // the language the interface is actually written in.
    const int languageIndex = m_languageBox->findData(m_settings.language());
    m_languageBox->setCurrentIndex(languageIndex >= 0 ? languageIndex : 0);

    revalidatePath();
}

void SettingsPage::apply()
{
    // One guard around nine writes: Settings answers every write with changed(), and the reload slot
    // connected in the constructor would otherwise re-read the form underneath the commit — harmless
    // for these values, but it would drop the note this call is about to put up.
    m_writing = true;
    m_settings.setZapretPath(m_pathEdit->text().trimmed());
    m_settings.setStartLastOnLaunch(m_startLastCheck->isChecked());
    m_settings.setMinimizeToTray(m_trayCheck->isChecked());
    // One write, and Motion reads it in answer to Settings::changed() — the page does not touch the
    // policy itself, so a Save from anywhere (the tray, a script editing the store) moves it the same way.
    m_settings.setAnimationsEnabled(m_animationsCheck->isChecked());
    // Written whether or not the row was shown: the value means nothing on a host with no WARP GUI, and
    // a hidden box that stopped writing would turn a Windows setting into a Linux one by way of a
    // shared store. The default is the model's, not the widget's.
    m_settings.setHideWarpGui(m_hideWarpCheck->isChecked());
    m_settings.setPingIntervalSec(m_pingSpin->value());
    m_settings.setWatchdogIntervalMin(m_watchdogSpin->value());
    m_settings.setTheme(m_themeBox->currentData().toString());
    // Inside the guard because it writes the store, and outside the reload it would trigger: the language
    // is the one value here that rewrites the page's own labels, and re-reading the form in the middle of
    // that would be the form answering itself.
    commitLanguage(m_languageBox->currentData().toString());
    m_writing = false;

    // Set after the language has moved, so the acknowledgement is said in the language the user just
    // asked for rather than the one they were leaving.
    m_savedNote->setText(LanguageManager::tr(QStringLiteral("settings.saved")));
    m_savedTimer->start(kSavedNoteMs);
    emit saved();
}

QString SettingsPage::zapretPath() const
{
    return m_pathEdit->text().trimmed();
}

void SettingsPage::setZapretPath(const QString &path)
{
    m_pathEdit->setText(path);
}


bool SettingsPage::pathLooksValid(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return false;
    if (ZapretPaths::detectPlatform(dir) != Strategy::Platform::Unknown)
        return true;
    return !ZapretLocator::locate({dir}).isEmpty();
}

QString SettingsPage::describePath(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return LanguageManager::tr(QStringLiteral("path.none"));

    const Strategy::Platform direct = ZapretPaths::detectPlatform(dir);
    if (direct != Strategy::Platform::Unknown) {
        const QList<Strategy> found = StrategyParser::parseTree(dir, direct, 2);
        return LanguageManager::tr(QStringLiteral("path.found"))
            .arg(direct == Strategy::Platform::Windows ? QStringLiteral("windows")
                                                       : QStringLiteral("linux"),
                 QString::number(found.size()));
    }

    const QList<ZapretLocator::Found> roots = ZapretLocator::locate({dir});
    if (!roots.isEmpty()) {
        QStringList names;
        for (const ZapretLocator::Found &f : roots)
            names << (f.platform == Strategy::Platform::Windows ? QStringLiteral("windows")
                                                                : QStringLiteral("linux"));
        return LanguageManager::tr(QStringLiteral("path.base")).arg(names.join(QStringLiteral(", ")));
    }

    return LanguageManager::tr(QStringLiteral("path.not_zapret"));
}

void SettingsPage::revalidatePath()
{
    const QString dir = m_pathEdit->text().trimmed();
    const bool ok = pathLooksValid(dir);
    setPathState(ok, describePath(dir));
}

void SettingsPage::setPathState(bool ok, const QString &message)
{
    m_pathState->setText(ok ? QStringLiteral("✓ %1").arg(message)
                            : QStringLiteral("✕ %1").arg(message));
    m_pathState->setObjectName(ok ? QStringLiteral("validOk") : QStringLiteral("validBad"));
    m_pathState->style()->unpolish(m_pathState);
    m_pathState->style()->polish(m_pathState);
    m_pathState->update();

    m_pathEdit->setObjectName(ok ? QStringLiteral("pathOk") : QStringLiteral("pathBad"));
    m_pathEdit->style()->unpolish(m_pathEdit);
    m_pathEdit->style()->polish(m_pathEdit);
    m_pathEdit->update();
}

void SettingsPage::browse()
{
    const QString start = m_pathEdit->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(
        this, LanguageManager::tr(QStringLiteral("settings.folder_title")),
        start.isEmpty() ? QDir::homePath() : start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_pathEdit->setText(QDir::toNativeSeparators(dir));
}

void SettingsPage::autoDetect()
{
    const QList<ZapretLocator::Found> found = ZapretLocator::locate();
    if (found.isEmpty()) {
        setPathState(false, LanguageManager::tr(QStringLiteral("path.not_found_manual")));
        return;
    }
    m_pathEdit->setText(QDir::toNativeSeparators(found.first().path));
}

void SettingsPage::downloadZapret()
{
    DownloadDialog dialog(this);
    connect(&dialog, &DownloadDialog::installed, this, [this](const QString &path) {
        m_pathEdit->setText(QDir::toNativeSeparators(path));
        // Said out loud rather than left to the textChanged handler: setText is a no-op when the field
        // already reads exactly this path, and the fresh tree still has to be described.
        revalidatePath();
    });
    dialog.startFetch();
    dialog.exec();
    // Nothing reaches Settings until Save is pressed: a download that lands in the field and a path the
    // application will actually use are two decisions, and this page owns the second.
}

} // namespace zapret

