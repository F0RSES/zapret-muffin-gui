#pragma once

#include <QWidget>

class QAction;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace zapret {

class Settings;

// Where zapret lives, what happens at startup, how the window behaves, which theme, how often the
// reachability probes run and how often a live profile is re-asked whether it still bypasses — as a
// page of the main window's own stack rather than a modal sheet.
//
// The move from dialog to page is the whole point: the rail already carries a settings glyph, and a
// glyph that opens a modal is two ways of navigating where the design wants one. It also changes what
// "commit" means. A dialog has a frame and an OK button; a page has neither, so the commit is a button
// of its own at the foot of the form, and leaving the page without pressing it is the equivalent of
// Cancel — except that it says so, with the note beside the button staying blank.
//
// Path validation stays live (on every keystroke) and cheap: ZapretPaths only stats marker files
// (bin/winws.exe, init.d/sysv/zapret, binaries/<arch>/nfqws). describePath()/pathLooksValid() are
// static so the verdict can be reasoned about without a widget hierarchy.
//
// The values are grouped into cards rather than hung on one long form, because the page had outgrown a
// single column: a path, four switches, a theme and two cadences read as one undifferentiated list of ten
// rows, and the path row read as the loudest thing on the screen because it carried three wide text
// buttons. Each card is a QFrame the sheet fills with the surface colour and strokes once; each is titled
// in the catalog's own words. The field column starts at the same pixel in all three — see kRowLabelPx —
// which is what makes three separate cards read as one page rather than as three unrelated dialogs.
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(Settings &settings, QWidget *parent = nullptr);
    ~SettingsPage() override;

    // Read the stored values into the widgets / write the widgets back into Settings.
    void loadFromSettings();
    void apply();

    QString zapretPath() const;
    void setZapretPath(const QString &path);

    // The warning shown when QSystemTrayIcon reports no tray host, or null. Exposed so a test can check
    // the page actually explains the degraded behaviour instead of leaving the box ticked in vain.
    QLabel *trayNote() const { return m_trayNote; }
    // The commit control and the acknowledgement it writes. Both exposed for the same reason: "Save
    // does nothing" and "Save never says it saved" are the two ways this page could quietly regress.
    QPushButton *saveButton() const { return m_save; }
    QLabel *savedNote() const { return m_savedNote; }
    // The two short fields. Exposed because their shape is the page's promise: a field that holds one
    // value stays beside its label instead of spanning the form, and the interval box carries no
    // stepper arrows. Both are things a later layout change can silently undo.
    QComboBox *themeField() const { return m_themeBox; }
    QSpinBox *pingIntervalField() const { return m_pingSpin; }
    // The watchdog's cadence, in the same shape as the ping interval above — and exposed for the same
    // reason: the range is a promise too, and it is the one this page has to keep with the model.
    QSpinBox *watchdogIntervalField() const { return m_watchdogSpin; }
    // The motion switch. Exposed because a checkbox that is drawn but never read, or read but never
    // written, looks exactly like a checkbox that works — and because unticking it is the only way to
    // reach the policy from outside, which is what the whole feature is for.
    QCheckBox *animationsField() const { return m_animationsCheck; }
    // The WARP window switch, and the reason it needs an accessor of its own: on a host whose WARP
    // package ships no window the row is not shown at all, and "hidden" is the promise a test has to be
    // able to check. See WarpInstaller::platformHasWarpGui().
    QCheckBox *hideWarpField() const { return m_hideWarpCheck; }

    // ---- the redesigned path row, the language field and the cards -------------------
    //
    // The field, the glyph inside it, and the two icons beside it. All four are exposed because the
    // redesign's promise is a shape rather than a behaviour: one text field, one in-field action, two
    // 32x32 icon buttons — and the three wide text buttons that used to fill that row are gone. A test
    // cannot check a shape it has no way to name, and a later "helpful" button beside the field would
    // undo it in silence.
    QLineEdit *pathField() const { return m_pathEdit; }
    QAction *browseAction() const { return m_browseAction; }
    QToolButton *autoDetectButton() const { return m_detectButton; }
    // The interface's language. The rail's flag button used to be the only way to change it, which left
    // this page claiming to hold the interface settings while the one setting that decides what language
    // the page itself is written in lived somewhere else entirely.
    QComboBox *languageField() const { return m_languageBox; }
    // The cards, in the order the page stacks them. Three accessors rather than a count, because which card
    // a field ended up in IS the grouping promise: "the bypass re-check is a network setting" is only
    // checkable if the network card is nameable. Rows are parented to their card, so a test can also walk
    // parentWidget() and ask the same question of a widget it has no accessor for.
    QFrame *generalCard() const { return m_generalCard; }
    QFrame *interfaceCard() const { return m_interfaceCard; }
    QFrame *networkCard() const { return m_networkCard; }

    // True when `dir` is a zapret root, or a base that contains a windows/ or linux/ root.
    static bool pathLooksValid(const QString &dir);
    // Human verdict shown next to the field (platform + how many entries were found).
    static QString describePath(const QString &dir);
    // The width every row label is pinned to, and so the x the field column starts at in all three cards.
    // Published because it is a promise rather than a private number: a QFormLayout sizes its own label
    // column from its widest label, so three forms left to themselves would put the fields at three
    // different pixels — and a page whose columns do not line up is the "колхозно" this redesign is here
    // to remove.
    static int rowLabelWidth() { return kRowLabelPx; }

signals:
    void saved();

private slots:
    void browse();
    void autoDetect();
    void revalidatePath();

private:
    void build();
    void setPathState(bool ok, const QString &message);
    // A card of the page: the framed surface, the layout rows that span it go into, and the two-column
    // form the labelled rows go into. All three are handed back because a card has two kinds of row and
    // they do not start at the same x: a labelled setting indents its value to the field column, while a
    // switch — which is its own label — belongs at the card's left edge, under the heading.
    struct Card
    {
        QFrame *frame = nullptr;
        QVBoxLayout *body = nullptr;
        QFormLayout *form = nullptr;
    };

    // A card: the framed surface, its heading, and the form its rows go in. The heading carries the
    // catalog key it came from like every other label here, so a language switch re-reads it from the
    // catalog rather than from a hand-maintained list of widgets somewhere else. `host` is the widget the
    // card is parented to and must be the one its layout is installed on — a layout positions its widgets
    // in ITS parent's coordinates, so a card parented to the page and laid out inside the scroll's content
    // widget would be drawn at the wrong offset.
    Card makeCard(QWidget *host, const QString &titleKey);
    // A row label pinned to kRowLabelPx, returned already carrying its key.
    QLabel *rowLabel(QFrame *card, const QString &key);
    // One of the two icon buttons beside the path field. They differ in glyph and in words, not in shape,
    // so the shape is spelled once — including the promise that an icon-only control still says what it
    // does: a tooltip for the pointer and an accessible name for a screen reader.
    QToolButton *makePathTool(QFrame *card, const QString &glyph, const QString &nameKey,
                              const QString &tipKey);
    // The field's placeholder, rebuilt from the catalog. A method rather than an inline string because it
    // is composed of three texts (a unix example, the word "or", a windows example) and so cannot ride on
    // the single-key i18nPlaceholder property the rest of the page's strings use.
    void refreshPathPlaceholder();
    // The language switch, applied the way the rail's flag button applies it: store the code AND move the
    // catalog, because nothing re-reads the store on its own and a page that only wrote it would change
    // nothing on screen until the next launch.
    void commitLanguage(const QString &code);

    Settings &m_settings;
    // Guards the Settings::changed() reload: apply() writes through the model, and the model answers by
    // telling every view to re-read itself — including this one, mid-commit.
    bool m_writing = false;

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_pathState = nullptr;
    // The folder glyph inside the field. A QAction rather than a button: QLineEdit owns the widget that
    // draws it, positions it, and hides it behind the text when the field is too narrow — which is exactly
    // why the row no longer needs a wide "Обзор…" beside it.
    QAction *m_browseAction = nullptr;
    // The icon beside it, 32x32 and wordless. The row is about a path, and a row of which the path is
    // the subject should not spend more width on verbs than on the field they fill. Fetching a release is
    // not one of them: that belongs where the empty result is visible — the profiles page, whose
    // placeholder says so when there are no strategies to list.
    QToolButton *m_detectButton = nullptr;

    QCheckBox *m_startLastCheck = nullptr;
    QCheckBox *m_trayCheck = nullptr;
    QCheckBox *m_animationsCheck = nullptr;   // the motion switch, written through Motion
    QCheckBox *m_hideWarpCheck = nullptr;     // the single-window policy; hidden where there is no GUI
    QLabel *m_trayNote = nullptr;    // shown only when this desktop has no tray host at all
    QComboBox *m_themeBox = nullptr;
    // The interface's language, in the same narrow shape as the theme box: one choice of seven words.
    QComboBox *m_languageBox = nullptr;
    QSpinBox *m_pingSpin = nullptr;
    QSpinBox *m_watchdogSpin = nullptr;

    // The three cards, in the order they are stacked. Each one owns its rows — the widgets inside a card are
    // parented to it — which is what lets a test ask which card a field belongs to instead of guessing
    // that from coordinates.
    QFrame *m_generalCard = nullptr;
    QFrame *m_interfaceCard = nullptr;
    QFrame *m_networkCard = nullptr;

    QPushButton *m_save = nullptr;
    QLabel *m_savedNote = nullptr;
    QTimer *m_savedTimer = nullptr;  // for how long "Сохранено ✓" stays up

    // The label column every card shares. A QFormLayout takes its label width from the widest label in it,
    // so three independent forms would start their fields at three different pixels; pinning each label to
    // this width pins the field column to one x across the whole page. Word wrap is what keeps a long
    // German or Hindi row from breaking the alignment instead of forcing it.
    static constexpr int kRowLabelPx = 168;
};

} // namespace zapret
