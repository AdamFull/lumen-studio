#include "ui/widgets/ColorWheel.h"

#include "ui/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <numbers>

namespace lumen::ui {

ColorWheel::ColorWheel(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(sizeHint());
}

void ColorWheel::setValue(double hue, double saturation)
{
    m_hue = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0);
    m_saturation = std::clamp(saturation, 0.0, 100.0);
    update();
}

void ColorWheel::set(double hue, double saturation, bool finished)
{
    const double h = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0);
    const double s = std::clamp(saturation, 0.0, 100.0);
    if (h != m_hue || s != m_saturation) {
        m_hue = h;
        m_saturation = s;
        update();
        emit valueChanged(m_hue, m_saturation);
    }
    if (finished)
        emit editingFinished();
}

QRectF ColorWheel::discRect() const
{
    const double side = std::min(width(), height()) - 8.0;
    return QRectF((width() - side) / 2, (height() - side) / 2, side, side);
}

void ColorWheel::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    m_disc = QImage();
}

void ColorWheel::paintEvent(QPaintEvent *)
{
    const QRectF disc = discRect();
    const qreal dpr = devicePixelRatioF();
    if (m_disc.isNull()) {
        // The colours at full saturation round the edge, fading to grey in the middle.
        const int side = int(std::ceil(disc.width() * dpr));
        m_disc = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        m_disc.fill(Qt::transparent);
        const double r = side / 2.0;
        for (int y = 0; y < side; ++y) {
            auto *line = reinterpret_cast<QRgb *>(m_disc.scanLine(y));
            for (int x = 0; x < side; ++x) {
                const double dx = x + 0.5 - r;
                const double dy = r - (y + 0.5);
                const double d = std::hypot(dx, dy) / r;
                if (d > 1.0)
                    continue;
                const double hue = std::fmod(std::atan2(dy, dx) * 180.0 / std::numbers::pi + 360.0, 360.0);
                const QColor c = QColor::fromHsvF(float(hue / 360.0), float(d), 0.8f);
                const double alpha = std::clamp((1.0 - d) * r, 0.0, 1.0); // a soft edge
                line[x] = qPremultiply(qRgba(c.red(), c.green(), c.blue(), int(alpha * 255)));
            }
        }
        m_disc.setDevicePixelRatio(dpr);
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.drawImage(disc.topLeft(), m_disc);
    // The point: where the tint is.
    const double angle = m_hue * std::numbers::pi / 180.0;
    const double radius = m_saturation / 100.0 * disc.width() / 2;
    const QPointF at = disc.center() + QPointF(std::cos(angle) * radius, -std::sin(angle) * radius);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 3));
    p.drawEllipse(at, 5, 5);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawEllipse(at, 5, 5);
    if (hasFocus()) {
        p.setPen(QPen(Theme::palette().accent, 1.5));
        p.drawEllipse(disc.adjusted(-2, -2, 2, 2));
    }
}

void ColorWheel::moveTo(const QPointF &pos)
{
    const QRectF disc = discRect();
    const QPointF d = pos - disc.center();
    const double hue = std::atan2(-d.y(), d.x()) * 180.0 / std::numbers::pi;
    const double saturation = std::hypot(d.x(), d.y()) / (disc.width() / 2) * 100.0;
    set(hue, saturation, false);
}

void ColorWheel::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    moveTo(e->position());
}

void ColorWheel::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging)
        moveTo(e->position());
}

void ColorWheel::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    emit editingFinished();
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent *)
{
    set(m_hue, 0, true);
}

void ColorWheel::keyPressEvent(QKeyEvent *e)
{
    const double step = e->modifiers() & Qt::ShiftModifier ? 1.0 : 5.0;
    switch (e->key()) {
    case Qt::Key_Left: set(m_hue + step, m_saturation, true); break;
    case Qt::Key_Right: set(m_hue - step, m_saturation, true); break;
    case Qt::Key_Up: set(m_hue, m_saturation + step, true); break;
    case Qt::Key_Down: set(m_hue, m_saturation - step, true); break;
    default: QWidget::keyPressEvent(e); break;
    }
}

} // namespace lumen::ui
