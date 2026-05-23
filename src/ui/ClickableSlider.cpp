#include "ClickableSlider.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QStyle>
#include <QStyleOptionSlider>

ClickableSlider::ClickableSlider(Qt::Orientation orientation, QWidget *parent)
    : QSlider(orientation, parent)
{
}

int ClickableSlider::valueFromPos(const QPoint &pos) const
{
    QStyleOptionSlider option;
    initStyleOption(&option);

    const QRect grooveRect = style()->subControlRect(QStyle::CC_Slider,
                                                     &option,
                                                     QStyle::SC_SliderGroove,
                                                     this);
    const QRect handleRect = style()->subControlRect(QStyle::CC_Slider,
                                                     &option,
                                                     QStyle::SC_SliderHandle,
                                                     this);

    int sliderMin = 0;
    int sliderMax = 0;
    int clickPosition = 0;

    if (orientation() == Qt::Horizontal) {
        const int handleLength = qMax(1, handleRect.width());
        sliderMin = grooveRect.left();
        sliderMax = grooveRect.right() - handleLength + 1;
        clickPosition = pos.x() - (handleLength / 2);
    } else {
        const int handleLength = qMax(1, handleRect.height());
        sliderMin = grooveRect.top();
        sliderMax = grooveRect.bottom() - handleLength + 1;
        clickPosition = pos.y() - (handleLength / 2);
    }

    const int sliderSpan = qMax(1, sliderMax - sliderMin);
    const int positionInSpan = qBound(0, clickPosition - sliderMin, sliderSpan);

    return QStyle::sliderValueFromPosition(minimum(),
                                           maximum(),
                                           positionInSpan,
                                           sliderSpan,
                                           option.upsideDown);
}

void ClickableSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int target = valueFromPos(event->pos());
        if (target != value())
            setValue(target);
        emit sliderPressed();
        event->accept();
    } else {
        QSlider::mousePressEvent(event);
    }
}

void ClickableSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        setValue(valueFromPos(event->pos()));
        event->accept();
    } else {
        QSlider::mouseMoveEvent(event);
    }
}

void ClickableSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit sliderReleased();
        event->accept();
    } else {
        QSlider::mouseReleaseEvent(event);
    }
}

void ClickableSlider::wheelEvent(QWheelEvent *event)
{
    int delta = event->angleDelta().y();
    if (delta == 0) delta = event->angleDelta().x();
    if (delta == 0) {
        event->ignore();
        return;
    }

    int steps = delta / 120;
    if (steps == 0) steps = (delta > 0) ? 1 : -1;

    // Use singleStep() for fine control via wheel.
    // For seekbar, we'll set this to something like 5s.
    int amount = steps * singleStep();

    emit sliderPressed();
    setValue(value() + amount);
    emit sliderReleased();

    event->accept();
}