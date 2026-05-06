#ifndef CLICKABLESLIDER_H
#define CLICKABLESLIDER_H

#include <QSlider>

class ClickableSlider : public QSlider
{
    Q_OBJECT
public:
    explicit ClickableSlider(Qt::Orientation orientation, QWidget *parent = nullptr);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    int valueFromPos(const QPoint &pos) const;
};

#endif // CLICKABLESLIDER_H
