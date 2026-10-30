#pragma once

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QPainterPath>
#include <QPolygon>
#include <QRectF>
#include <QRegion>
#include <QTimer>
#include <QWidget>

namespace zapret {

// A QComboBox whose popup is a frameless, translucent top-level clipped to the sheet's rounded rectangle.
//
// Qt hosts the list in its own QComboBoxPrivateContainer (a QFrame window) and paints that frame's square
// background behind the QAbstractItemView. A QSS border-radius on the view is therefore hidden under four
// square corners: measured in the offscreen snapshot as opaque #9f9f9f at (0,0). The container does not
// exist when the combo is constructed, so the flags cannot be set from the constructor; showPopup() is the
// one moment it exists and has not been shown yet. A mask is applied after the show as well, because a
// transparent window alone depends on the compositor — the mask makes the silhouette exact on any host.
class ThemedComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void showPopup() override
    {
        if (QWidget *popup = view()->window(); popup && popup != window()) {
            popup->setAttribute(Qt::WA_TranslucentBackground, true);
            popup->setAttribute(Qt::WA_NoSystemBackground, true);
            popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        }
        if (QAbstractItemView *list = view()) {
            list->setFrameShape(QFrame::NoFrame);
            // Deliberately NOT translucent: the sheet gives the list an opaque background and the
            // viewport is what paints it. Marking the view translucent (as an earlier version did) let
            // the window show through the dropdown — the reported "transparent background".
        }
        QComboBox::showPopup();
        // After show, and only then: the container has its final geometry. The radius is the sheet's 10 px
        // on QComboBox QAbstractItemView; keeping the two numbers equal is what makes the mask invisible.
        if (QWidget *popup = view()->window(); popup && popup != window()) {
            QTimer::singleShot(0, popup, [popup] {
                QPainterPath rounded;
                rounded.addRoundedRect(QRectF(popup->rect()), 10, 10);
                popup->setMask(QRegion(rounded.toFillPolygon().toPolygon()));
            });
        }
    }
};

} // namespace zapret