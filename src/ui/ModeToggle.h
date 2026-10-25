#pragma once

#include <QColor>
#include <QRect>
#include <QString>
#include <QWidget>

class QVariantAnimation;

namespace zapret {

// Which halves of the dashboard's pill are lit.
//
// Two independent switches, not one choice between two values. WARP rides on zapret: the tunnel's
// packets only reach Cloudflare through the queue rules zapret puts in the path, so "WARP without
// Zapret" is not a state this application can be in, and a control that offered it would be offering
// a broken one. The legal set is therefore {nothing}, {zapret}, {zapret + WARP} — three of the four
// combinations, and clamp() is the rule that keeps every reader inside those three.
struct ModeSelection
{
    bool zapret = true;
    bool warp = false;

    bool operator==(const ModeSelection &other) const
    {
        return zapret == other.zapret && warp == other.warp;
    }
    bool operator!=(const ModeSelection &other) const { return !(*this == other); }
    // Neither half lit: the power button controls nothing until one is picked.
    bool empty() const { return !zapret && !warp; }
};

// The dashboard's Zapret/WARP segmented pill, now two switches rather than one selector.
//
// Each half lights on its own: clicking a dark half turns it on, clicking a lit half turns it off,
// and the two rules above decide what the other half does in the same click. There is no thumb
// travelling between the halves any more — that shape said "one of these two, never both", which is
// precisely the claim the multi-select model retired. What is left of it is the geometry: each
// half's highlight still occupies exactly one of the two rectangles thumbRectFor() names, so the
// pill reads as one control with two states rather than as two buttons that happen to touch.
//
// The whole control is painted in paintEvent() for the same reason PowerButton is — a
// QGraphicsDropShadowEffect cannot be clipped to a rounded pill, and each half's wash has to breathe
// with its own fade rather than have the pill redraw around it.
//
// Design rules the tests hold this to:
//   * two halves, and only two — the "index" a test can reach for is either 0 (Zapret) or 1 (WARP),
//     and the geometry helper takes exactly that;
//   * the rules are pure statics, so the whole of "what a click can leave lit" is checkable without
//     a widget, a window or a Qt platform;
//   * colour is per-half, not per-state, so a theme flip cannot make Zapret purple on one run and
//     orange on the next.
class ModeToggle : public QWidget
{
    Q_OBJECT

public:
    // Which half of the pill a caller means. Retained as an enum because the half's label, colour
    // and rectangle are all per-half facts, and "which half" is not the same question as "which
    // halves are lit" — that one is a ModeSelection.
    enum class Segment
    {
        Zapret,
        Warp
    };
    Q_ENUM(Segment)

    explicit ModeToggle(QWidget *parent = nullptr);
    ~ModeToggle() override;

    // ---- the two rules, spelled as pure functions ------------------------------------
    //
    // WARP never rides alone. A wanted selection that lights WARP supplies zapret with it. Turning
    // zapret down is not clamp()'s business — it answers "can this combination exist", and the
    // honest answer for {zapret off, WARP on} is "yes, and it means both of them".
    static ModeSelection clamp(ModeSelection wanted);

    // What a click on one half leaves lit. A lit half goes dark and a dark half comes up; turning
    // zapret off takes WARP down in the same click (nothing it rides on is left), and turning WARP
    // up brings zapret up with it. Passed through clamp() on the way out, so no click can land on
    // an impossible combination.
    static ModeSelection toggled(ModeSelection current, Segment segment);

    ModeSelection selection() const { return m_selection; }
    bool isOn(Segment segment) const
    {
        return segment == Segment::Zapret ? m_selection.zapret : m_selection.warp;
    }

    // Move both halves at once — the startup seed, an availability clamp, a snap-back after a
    // refused question. Rules are applied first, and a selection the pill already shows emits
    // nothing.
    void setSelection(ModeSelection wanted);
    void setSelection(bool zapretOn, bool warpOn) { setSelection(ModeSelection{zapretOn, warpOn}); }

    // The pill's preferred size — wide enough for two legible halves — exposed so a layout does not
    // have to hardcode the width it wants. Height is fixed to kPillHeightPx.
    static constexpr int kPillWidthPx = 240;
    static constexpr int kPillHeightPx = 40;
    static constexpr int kThumbPaddingPx = 4;

    // The rectangle one half's highlight should occupy inside `bounds`. Public and static because
    // the geometry is still the whole promise the widget makes: a test can check that a lit half
    // lands where it says without a window, a paint or a Qt platform.
    static QRect thumbRectFor(int index, const QRect &bounds);
    static int indexFor(Segment segment) { return segment == Segment::Zapret ? 0 : 1; }
    static Segment segmentForIndex(int index) { return index == 0 ? Segment::Zapret : Segment::Warp; }

    // The colour each half wears when it is lit. Theme-independent neon so a half's meaning
    // survives a dark/light flip: zapret on the dashboard's own accent (purple), the tunnel on the
    // same Cloudflare blue the power button's live-tunnel ring uses.
    static QColor colorFor(Segment segment);
    static QColor colorForIndex(int index);
    // Half labels; language-neutral English to match the design of the power button's chip
    // ("CONNECTED"). A translated form would need every test to reach into a catalog.
    static QString labelFor(Segment segment);
    static QString labelForIndex(int index);
    // What a whole selection adds up to, in the halves' own words — "Zapret + WARP", "Zapret", or
    // the dash for nothing at all. The window puts this in the informative line of the one question
    // the pill is allowed to ask, so the seven translations of that question never carry a name.
    static QString describe(ModeSelection selection);

    // The keyboard's cursor: Left/Right move it between the halves, Space/Enter toggles the half it
    // is on. Readable because a test that cannot take a screenshot can still ask where the focus is.
    Segment focusedSegment() const { return m_focus; }
    void setFocusedSegment(Segment segment);

    // Re-rasterise the pieces whose colour comes from the theme (the pill's border and a dark
    // half's text). MainWindow calls this on a theme switch, as it does for every baked-pixmap
    // widget.
    void retheme();

signals:
    // The two halves as the user left them — already clamped, so a listener never has to re-apply
    // the rules. Two booleans rather than one ModeSelection because a queued connection then needs
    // no metatype registration, and every consumer wants both halves anyway.
    void selectionChanged(bool zapretOn, bool warpOn);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    QSize sizeHint() const override;

private:
    int indexFromX(int x) const;
    // Bring one half's highlight up or take it down, animating its fade (or snapping it when motion is
    // off). Called by applySelection for whichever halves changed, never on its own.
    void setLit(Segment segment, bool on);
    // The one writer of m_selection: apply the rules, start the fades the change calls for, repaint,
    // and tell whoever is listening what the two halves are now.
    void applySelection(ModeSelection next);

    ModeSelection m_selection{true, false};
    Segment m_focus = Segment::Zapret;
    // How strongly each half's highlight is painted right now, 0.0-1.0, animated by its own fade.
    // Indexed by indexFor(segment); the lit state itself lives in m_selection, these are only what
    // the pixels are doing on their way to it.
    qreal m_level[2] = {1.0, 0.0};
    QVariantAnimation *m_fade[2] = {nullptr, nullptr};
};

} // namespace zapret
