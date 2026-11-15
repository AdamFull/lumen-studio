#pragma once

#include <QImage>
#include <QWidget>

namespace lumen::ui {

// A colour wheel for a tint (plan M20 step 4): the angle is the hue (degrees, red at the right, as on
// an HSV wheel), the distance from the middle the saturation (%). Drag the point; double-click puts
// it back in the middle; the arrow keys move it (left and right the hue, up and down the
// saturation).
class ColorWheel : public QWidget
{
    Q_OBJECT
public:
    explicit ColorWheel(QWidget *parent = nullptr);

    double hue() const { return m_hue; }
    double saturation() const { return m_saturation; }
    void setValue(double hue, double saturation); // without emitting

    QSize sizeHint() const override { return {128, 128}; }
    QSize minimumSizeHint() const override { return {96, 96}; }

signals:
    void valueChanged(double hue, double saturation); // also while the point is dragged
    void editingFinished();                           // a drag ended, a key or a double-click moved it

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    QRectF discRect() const;
    void moveTo(const QPointF &pos);
    void set(double hue, double saturation, bool finished);

    double m_hue = 0;
    double m_saturation = 0;
    bool m_dragging = false;
    QImage m_disc;
};

} // namespace lumen::ui
