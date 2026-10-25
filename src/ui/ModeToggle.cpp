#include "ModeToggle.h"

#include "Motion.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QVariantAnimation>

namespace zapret {

namespace {

// How long a half takes to come up or go dark. Matches Motion's kCardRiseMs — the same family of
// motion as a row rising into place — and is deliberately not the 220 ms the old single thumb spent
// crossing the pill: a cross-pill slide needed that long to be followable, a fade only needs to be
// seen. Anything faster and a click looks like it did nothing; anything slower and the pill reads
// as lagging behind the mouse.
constexpr int kFadeMs = 180;

// The alpha a fully lit half's wash carries. The pure tone at full strength under white text pushes
// the label out of legibility, and the pill's job is to say which half is on, not to compete with
// the power ring for attention.
constexpr qreal kWashAlpha = 0.28;

// The rounded corner radius the pill is drawn with. Half the height is the fully round case; a
// 40 px pill at 20 px corner is a stadium, which is the shape the design asked for.
constexpr int kPillRadiusPx = ModeToggle::kPillHeightPx / 2;

qreal lerp(qreal a, qreal b, qreal t) { return a + (b - a) * t; }

QColor mixQ(const QColor &a, const QColor &b, qreal t)
{
    QColor out;
    out.setRedF(lerp(a.redF(), b.redF(), t));
    out.setGreenF(lerp(a.greenF(), b.greenF(), t));
    out.setBlueF(lerp(a.blueF(), b.blueF(), t));
    out.setAlphaF(lerp(a.alphaF(), b.alphaF(), t));
    return out;
}

} // namespace

QRect ModeToggle::thumbRectFor(int index, const QRect &bounds)
{
    // The two halves inside the pill's own border. Subtract kThumbPaddingPx on every side of the
    // interior so a lit half reads as an inset element rather than a full-width highlight.
    const QRect interior = bounds.adjusted(kThumbPaddingPx, kThumbPaddingPx,
                                           -kThumbPaddingPx, -kThumbPaddingPx);
    if (interior.width() <= 0 || interior.height() <= 0)
        return QRect();
    const int halfWidth = interior.width() / 2;
    const int x = (index == 0) ? interior.left() : interior.left() + halfWidth;
    const int right = interior.right();
    const int w = (index == 0) ? halfWidth : (right - (interior.left() + halfWidth) + 1);
    return QRect(x, interior.top(), w, interior.height());
}

ModeSelection ModeToggle::clamp(ModeSelection wanted)
{
    // WARP cannot work without the bypass it rides on, so a request for the tunnel is a request for
    // both — and the second branch is the same fact read from the other side: with zapret dark there
    // is nothing for a lit WARP half to mean. clamp() only ever *adds* zapret, never removes WARP:
    // the combination {zapret off, WARP on} is not nonsense, it is the tunnel asking for its
    // dependency, and the dependency is the cheaper of the two to honour.
    if (wanted.warp)
        wanted.zapret = true;
    return wanted;
}

ModeSelection ModeToggle::toggled(ModeSelection current, Segment segment)
{
    ModeSelection next = current;
    if (segment == Segment::Zapret) {
        next.zapret = !current.zapret;
        // Turning the bypass off in a click that never mentioned the tunnel still takes the tunnel
        // down: it cannot stay lit over the rules it depends on, and a half that cannot work is a
        // half that must not pretend otherwise.
        if (!next.zapret)
            next.warp = false;
    } else {
        next.warp = !current.warp;
        // And the reverse: the user who asked for the tunnel gets the bypass it needs in the same
        // click rather than in a second one they were never told about.
        if (next.warp)
            next.zapret = true;
    }
    return clamp(next);
}

QColor ModeToggle::colorFor(Segment segment)
{
    // Zapret stays on the dashboard's own accent (purple in the dark sheet) so the mode the app was
    // designed around is the mode whose colour is already everywhere else on the screen. The tunnel
    // half takes the same Cloudflare blue the power ring wears for a live WARP connection — one
    // colour, one spelling, two widgets — because the orange the single-choice pill used is
    // WARP's *brand* colour, while what a lit half claims is "this layer is on".
    if (segment == Segment::Warp)
        return QColor(0x22, 0xA7, 0xF0);   // Cloudflare blue
    return Theme::accent();
}

QColor ModeToggle::colorForIndex(int index)
{
    return colorFor(segmentForIndex(index));
}

QString ModeToggle::labelFor(Segment segment)
{
    return (segment == Segment::Warp) ? QStringLiteral("WARP") : QStringLiteral("Zapret");
}

QString ModeToggle::labelForIndex(int index)
{
    return labelFor(segmentForIndex(index));
}

QString ModeToggle::describe(ModeSelection selection)
{
    if (selection.zapret && selection.warp)
        return labelFor(Segment::Zapret) + QStringLiteral(" + ") + labelFor(Segment::Warp);
    if (selection.zapret)
        return labelFor(Segment::Zapret);
    if (selection.warp)
        return labelFor(Segment::Warp);
    return QStringLiteral("\u2014");
}

ModeToggle::ModeToggle(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kPillWidthPx, kPillHeightPx);

    // One fade per half. They run independently because the halves are independent switches: a
    // click that takes the tunnel down while leaving the bypass lit should darken one side only,
    // and a shared animation could not say that without moving the side nobody touched.
    for (int index = 0; index < 2; ++index) {
        m_fade[index] = new QVariantAnimation(this);
        m_fade[index]->setDuration(kFadeMs);
        m_fade[index]->setEasingCurve(QEasingCurve::InOutSine);
        connect(m_fade[index], &QVariantAnimation::valueChanged, this,
                [this, index](const QVariant &v) {
                    const qreal next = v.toReal();
                    if (qFuzzyCompare(next, m_level[index]))
                        return;
                    m_level[index] = next;
                    update();
                });
    }
}

ModeToggle::~ModeToggle()
{
    for (QVariantAnimation *fade : m_fade) {
        if (fade)
            fade->stop();
    }
}

QSize ModeToggle::sizeHint() const
{
    return QSize(kPillWidthPx, kPillHeightPx);
}

void ModeToggle::setSelection(ModeSelection wanted)
{
    applySelection(wanted);
}

void ModeToggle::applySelection(ModeSelection next)
{
    next = clamp(next);
    if (next == m_selection)
        return;   // a no-op move emits nothing, so a snap-back cannot reach the window as a request

    const ModeSelection previous = m_selection;
    m_selection = next;
    if (next.zapret != previous.zapret)
        setLit(Segment::Zapret, next.zapret);
    if (next.warp != previous.warp)
        setLit(Segment::Warp, next.warp);
    update();
    emit selectionChanged(next.zapret, next.warp);
}

void ModeToggle::setLit(Segment segment, bool on)
{
    const int index = indexFor(segment);
    const qreal target = on ? 1.0 : 0.0;
    if (!Motion::enabled()) {
        m_level[index] = target;
        return;
    }
    QVariantAnimation *fade = m_fade[index];
    if (fade->state() == QAbstractAnimation::Running)
        fade->stop();
    fade->setStartValue(m_level[index]);
    fade->setEndValue(target);
    fade->start();
}

void ModeToggle::setFocusedSegment(Segment segment)
{
    if (m_focus == segment)
        return;
    m_focus = segment;
    update();
}

void ModeToggle::retheme()
{
    update();
}

int ModeToggle::indexFromX(int x) const
{
    const QRect interior = rect().adjusted(kThumbPaddingPx, kThumbPaddingPx,
                                           -kThumbPaddingPx, -kThumbPaddingPx);
    if (interior.width() <= 0)
        return 0;
    return (x < interior.left() + interior.width() / 2) ? 0 : 1;
}

void ModeToggle::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Segment segment = segmentForIndex(indexFromX(event->pos().x()));
    // The click moves the keyboard's cursor with it: a user who has just pressed the WARP half and
    // then reaches for the keyboard expects the next Space to mean the same half.
    setFocusedSegment(segment);
    applySelection(toggled(m_selection, segment));
}

void ModeToggle::keyPressEvent(QKeyEvent *event)
{
    // Left/Right no longer mean "pick that half" — with two independent switches they can only mean
    // "put the cursor there" — and Space is the press. The old one-key flip between halves is gone
    // because a flip cannot say which half a user is looking at.
    if (event->key() == Qt::Key_Left) {
        setFocusedSegment(Segment::Zapret);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        setFocusedSegment(Segment::Warp);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        applySelection(toggled(m_selection, m_focus));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ModeToggle::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = rect();
    // The pill's track: a bordered stadium slightly darker than the surface so a lit half has
    // something to sit on. Reuses the sheet's hover token as the recessed fill for the same reason
    // the log well's dark band matches the code it holds — a control that reads as inset is one that
    // shares its colour with what the surface itself becomes on press.
    const QColor track = Theme::hover();
    const QColor border = Theme::border();
    p.setPen(QPen(border, 1));
    p.setBrush(track);
    p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), kPillRadiusPx, kPillRadiusPx);

    QFont f = p.font();
    f.setBold(true);
    p.setFont(f);

    const int halfInterior = (r.width() - 2 * kThumbPaddingPx) / 2;
    const QRect textRect[2] = {
        QRect(r.left() + kThumbPaddingPx, r.top(), halfInterior, r.height()),
        QRect(r.left() + kThumbPaddingPx + halfInterior, r.top(),
              r.width() - kThumbPaddingPx - halfInterior, r.height()),
    };

    for (int index = 0; index < 2; ++index) {
        const qreal level = m_level[index];
        const QColor color = colorForIndex(index);
        const QRectF half = QRectF(thumbRectFor(index, r));
        const qreal halfRadius = qreal(half.height()) / 2.0;

        // The half's own wash, faded in and out on its own. A dark half paints nothing: with two
        // independent switches the absence of a highlight IS the reading of "off", and a grey thumb
        // parked there would say "selected, but not".
        if (level > 0.001) {
            QColor wash = color;
            wash.setAlphaF(kWashAlpha * level);
            p.setPen(Qt::NoPen);
            p.setBrush(wash);
            p.drawRoundedRect(half, halfRadius, halfRadius);
        }

        // The label carries the same state at the same strength: a lit half is drawn in the half's
        // full colour, a dark one in muted text, and the fade interpolates between the two so one
        // click visibly changes the word rather than only the background behind it.
        p.setPen(mixQ(Theme::mutedText(), color, level));
        p.drawText(textRect[index], Qt::AlignCenter, labelForIndex(index));

        // Which half the keyboard is on. Only ever drawn while the pill actually has focus — an
        // outlined half on an unfocused pill would read as a second lit state.
        if (hasFocus() && index == indexFor(m_focus)) {
            QColor rim = color;
            rim.setAlphaF(0.75);
            p.setPen(QPen(rim, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(half.adjusted(0.5, 0.5, -0.5, -0.5), halfRadius, halfRadius);
        }
    }

    // Both halves lit: the two washes meet in the middle and become one band, which is what the
    // combined mode should look like — one control doing two things. The hairline between them is
    // what stops that band from reading as a single selection: two halves, two subsystems, and the
    // eye should still be able to find the seam.
    if (m_level[0] > 0.5 && m_level[1] > 0.5) {
        QColor seam = border;
        seam.setAlphaF(qMin(m_level[0], m_level[1]));
        p.setPen(QPen(seam, 1));
        const int mid = r.left() + kThumbPaddingPx + halfInterior;
        p.drawLine(QPointF(mid, r.top() + kThumbPaddingPx + 2.0),
                   QPointF(mid, r.bottom() - kThumbPaddingPx - 1.0));
    }
}

} // namespace zapret
