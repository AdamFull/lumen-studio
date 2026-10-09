#include "ui/parts/PuppetPreview.h"

#include "core/Async.h"
#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRandomGenerator>

#include <cmath>

namespace lumen::ui {

namespace {

bool isHair(const QString &tag)
{
    return tag.contains(QLatin1String("hair"));
}

bool isEyeBall(const QString &tag)
{
    return tag == QLatin1String("irides") || tag == QLatin1String("eyewhite");
}

double smooth(double current, double target, double dt, double speed)
{
    return current + (target - current) * std::min(1.0, dt * speed);
}

} // namespace

PuppetPreview::PuppetPreview(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(240, 240);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &PuppetPreview::tick);
    m_rescaleTimer.setSingleShot(true);
    m_rescaleTimer.setInterval(120);
    connect(&m_rescaleTimer, &QTimer::timeout, this, &PuppetPreview::rescale);
    m_clock.start();
}

void PuppetPreview::setLayers(const QList<PuppetLayer> &layers, const QSize &canvas)
{
    m_layers = layers;
    m_canvas = canvas;
    m_scaled.clear();
    m_scaledFor = 0.0;
    ++m_generation;
    // Pivots: the chin (head rotation) and the hips (body).
    QRect face, head, body;
    for (const PuppetLayer &l : layers) {
        if (l.tag == QLatin1String("face") || l.tag == QLatin1String("head"))
            face |= l.rect;
        if (l.group == QLatin1String("head") && !isHair(l.tag))
            head |= l.rect;
        if (l.group == QLatin1String("body"))
            body |= l.rect;
    }
    if (face.isEmpty())
        face = head;
    if (face.isEmpty())
        face = QRect(QPoint(canvas.width() / 3, 0), QSize(canvas.width() / 3, canvas.height() / 4));
    if (body.isEmpty())
        body = QRect(QPoint(0, 0), canvas);
    m_headPivot = QPointF(face.center().x(), face.bottom());
    m_bodyPivot = QPointF(body.center().x(), body.bottom());
    m_headSize = std::max(32, face.height());
    m_rescaleTimer.start(0);
    updateTimer();
    update();
}

void PuppetPreview::setParams(const Params &p)
{
    m_params = p;
    update();
}

void PuppetPreview::setAutoIdle(bool on)
{
    m_idle = on;
    updateTimer();
}

void PuppetPreview::setFollowMouse(bool on)
{
    m_follow = on;
    updateTimer();
}

void PuppetPreview::setTalking(bool on)
{
    m_talking = on;
    if (!on)
        m_params.mouthOpen = 0;
    updateTimer();
    update();
}

void PuppetPreview::updateTimer()
{
    const bool animate = (m_idle || m_follow || m_talking) && isVisible() && !m_layers.isEmpty();
    if (animate && !m_timer.isActive()) {
        m_lastT = m_clock.elapsed() / 1000.0;
        m_timer.start();
    } else if (!animate) {
        m_timer.stop();
    }
}

void PuppetPreview::showEvent(QShowEvent *)
{
    updateTimer();
}

void PuppetPreview::hideEvent(QHideEvent *)
{
    m_timer.stop();
}

void PuppetPreview::resizeEvent(QResizeEvent *)
{
    m_rescaleTimer.start();
}

void PuppetPreview::mouseMoveEvent(QMouseEvent *e)
{
    m_mouse = e->position();
}

void PuppetPreview::leaveEvent(QEvent *)
{
    m_mouse = QPointF(-1, -1);
}

double PuppetPreview::viewScale(const QSizeF &area) const
{
    if (m_canvas.isEmpty())
        return 1.0;
    return std::min(area.width() / m_canvas.width(), area.height() / m_canvas.height()) * 0.94;
}

void PuppetPreview::rescale()
{
    const double s = viewScale(size());
    if (m_layers.isEmpty() || s <= 0 || std::abs(s - m_scaledFor) < 1e-3)
        return;
    const QList<PuppetLayer> layers = m_layers;
    const quint64 generation = m_generation;
    QPointer<PuppetPreview> self(this);
    async::run([layers, s] {
        QList<QImage> out;
        for (const PuppetLayer &l : layers) {
            const QSize target(std::max(1, int(std::lround(l.rect.width() * s))), std::max(1, int(std::lround(l.rect.height() * s))));
            out.append(l.image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied));
        }
        return out;
    }).then(this, [self, generation, s](QList<QImage> scaled) {
        if (!self || generation != self->m_generation)
            return;
        self->m_scaled = scaled;
        self->m_scaledFor = s;
        self->update();
    });
}

void PuppetPreview::tick()
{
    const double t = m_clock.elapsed() / 1000.0;
    const double dt = std::clamp(t - m_lastT, 0.0, 0.1);
    m_lastT = t;
    Params p = m_params;
    const double prevX = p.angleX;
    if (m_follow && m_mouse.x() >= 0) {
        const double nx = std::clamp((m_mouse.x() / std::max(1, width())) * 2.0 - 1.0, -1.0, 1.0);
        const double ny = std::clamp((m_mouse.y() / std::max(1, height())) * 2.0 - 1.0, -1.0, 1.0);
        p.angleX = smooth(p.angleX, nx * 30.0, dt, 6.0);
        p.angleY = smooth(p.angleY, -ny * 30.0, dt, 6.0);
        p.angleZ = smooth(p.angleZ, nx * ny * -10.0, dt, 4.0);
        p.bodyX = smooth(p.bodyX, nx * 6.0, dt, 3.0);
    } else if (m_idle) {
        p.angleX = smooth(p.angleX, 12.0 * std::sin(t * 0.55) + 4.0 * std::sin(t * 1.6 + 0.4), dt, 4.0);
        p.angleY = smooth(p.angleY, 7.0 * std::sin(t * 0.42 + 1.0), dt, 4.0);
        p.angleZ = smooth(p.angleZ, 5.0 * std::sin(t * 0.5 + 2.0), dt, 4.0);
        p.bodyX = smooth(p.bodyX, 4.0 * std::sin(t * 0.37), dt, 3.0);
    }
    if (m_idle || m_follow) {
        p.breath = (std::sin(t * 1.25) + 1.0) / 2.0;
        // Hair lags behind head turns plus a gentle idle sway.
        p.hair = std::clamp(smooth(p.hair, 0.45 * std::sin(t * 1.3) - (p.angleX - prevX) * 0.6, dt, 5.0), -1.0, 1.0);
        if (t >= m_nextBlink) {
            m_blinkStart = t;
            m_nextBlink = t + 2.2 + QRandomGenerator::global()->bounded(3.0);
        }
        const double d = t - m_blinkStart;
        if (m_blinkStart >= 0 && d < 0.08)
            p.eyeOpen = 1.0 - d / 0.08;
        else if (m_blinkStart >= 0 && d < 0.22)
            p.eyeOpen = (d - 0.08) / 0.14;
        else
            p.eyeOpen = 1.0;
    }
    if (m_talking)
        p.mouthOpen = std::clamp(0.5 + 0.5 * std::sin(t * 11.0) * std::sin(t * 3.1 + 1.0), 0.0, 1.0);
    m_params = p;
    emit paramsChanged();
    update();
}

void PuppetPreview::render(QPainter &p, const QRectF &target)
{
    if (m_layers.isEmpty() || m_canvas.isEmpty())
        return;
    const double s = viewScale(target.size());
    const QPointF origin(target.center().x() - m_canvas.width() * s / 2.0, target.center().y() - m_canvas.height() * s / 2.0);
    const Params &a = m_params;
    const bool cached = m_scaled.size() == m_layers.size();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_layers.size(); ++i) {
        const PuppetLayer &l = m_layers[i];
        if (!l.visible || l.image.isNull())
            continue;
        const QImage &img = cached ? m_scaled[i] : l.image;
        p.save();
        p.translate(origin);
        p.scale(s, s);
        const bool head = l.group == QLatin1String("head");
        // Body: lean and parallax.
        p.translate(a.bodyX / 10.0 * (l.depth - 0.4) * m_canvas.width() * 0.03, 0);
        p.translate(m_bodyPivot);
        p.rotate(a.bodyX * 0.25);
        if (!head && (l.group == QLatin1String("body") || l.group == QLatin1String("extra")))
            p.scale(1.0, 1.0 + a.breath * 0.012);
        p.translate(-m_bodyPivot);
        if (head) {
            // Head: rotation around the chin plus depth-dependent parallax.
            const double nx = a.angleX / 30.0, ny = a.angleY / 30.0;
            const double rel = l.depth - 0.6;
            p.translate(nx * m_headSize * (0.06 + rel * 0.3), -ny * m_headSize * (0.04 + rel * 0.2));
            p.translate(m_headPivot);
            p.rotate(a.angleZ * 0.7 + a.bodyX * 0.3);
            p.translate(-m_headPivot);
            if (isEyeBall(l.tag))
                p.translate(nx * l.rect.width() * 0.05, -ny * l.rect.height() * 0.08);
        }
        const QPointF c = QRectF(l.rect).center();
        if (isEyeBall(l.tag)) {
            p.translate(c);
            p.scale(1.0, std::max(0.06, a.eyeOpen));
            p.translate(-c);
        } else if (l.tag == QLatin1String("eyelash")) {
            p.translate(0, (1.0 - a.eyeOpen) * l.rect.height() * 0.35);
        } else if (l.tag == QLatin1String("mouth")) {
            p.translate(QPointF(c.x(), l.rect.top()));
            p.scale(1.0, 1.0 + a.mouthOpen * 0.9);
            p.translate(-QPointF(c.x(), l.rect.top()));
        }
        if (isHair(l.tag) && std::abs(a.hair) > 1e-3) {
            // Strip deformation anchored at the top of the layer.
            const int strips = 24;
            const double amp = l.rect.height() * 0.035 * a.hair * (l.tag == QLatin1String("back hair") ? 1.3 : 1.0);
            for (int k = 0; k < strips; ++k) {
                const double f0 = double(k) / strips, f1 = double(k + 1) / strips;
                const double fm = (f0 + f1) / 2.0;
                const QRectF dst(l.rect.left() + amp * fm * fm, l.rect.top() + f0 * l.rect.height(), l.rect.width(),
                                 (f1 - f0) * l.rect.height() + 0.6);
                const QRectF src(0, f0 * img.height(), img.width(), (f1 - f0) * img.height());
                p.drawImage(dst, img, src);
            }
        } else {
            p.drawImage(QRectF(l.rect), img);
        }
        p.restore();
    }
}

void PuppetPreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const Palette &pal = Theme::palette();
    p.fillRect(rect(), pal.canvas);
    if (m_layers.isEmpty()) {
        p.setPen(pal.textMuted);
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Generate or cut out parts to preview the character as a 2.5D puppet."));
        return;
    }
    if (std::abs(viewScale(size()) - m_scaledFor) > 1e-3 && !m_rescaleTimer.isActive())
        m_rescaleTimer.start();
    render(p, rect());
}

QImage PuppetPreview::renderFrame(const QSize &size)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Theme::palette().canvas);
    QPainter p(&out);
    const QList<QImage> scaled = m_scaled;
    m_scaled.clear(); // render from the full-resolution layers
    render(p, QRectF(QPointF(0, 0), QSizeF(size)));
    m_scaled = scaled;
    p.end();
    return out;
}

} // namespace lumen::ui
