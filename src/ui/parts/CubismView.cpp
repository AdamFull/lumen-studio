#include "ui/parts/CubismView.h"

#include "core/Async.h"
#include "live2d/CubismRenderer.h"
#include "ui/Theme.h"

#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRandomGenerator>
#include <QWheelEvent>

#include <cmath>

namespace lumen::ui {

namespace {

constexpr double kAnimatedResolution = 960.0;

double approach(double current, double target, double dt, double speed)
{
    return current + (target - current) * std::min(1.0, dt * speed);
}

} // namespace

CubismView::CubismView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(240, 240);
    m_timer.setInterval(33);
    connect(&m_timer, &QTimer::timeout, this, &CubismView::tick);
    m_clock.start();
}

CubismView::~CubismView() = default;

bool CubismView::loadModel(const QString &model3Path, QString *error)
{
    auto model = live2d::CubismModel::load(model3Path, error);
    if (!model)
        return false;
    m_model = std::move(model);
    m_idle.reset();
    const QString idle = m_model->settings().idleMotion();
    if (!idle.isEmpty()) {
        QString motionError;
        m_idle = live2d::Motion::load(idle, &motionError);
    }
    m_physics.reset();
    m_physicsError.clear();
    if (!m_model->settings().physics.isEmpty()) {
        m_physics = live2d::Physics::load(m_model->settings().physics, &m_physicsError);
        if (m_physics) {
            m_physics->bind(m_model->parameterIds());
            m_physics->setWind(m_wind);
            // Start at rest: settle the strands for the default pose.
            m_model->resetParameters();
            m_physics->stabilize(m_model->parameterValues(), m_model->parameterMinimums(), m_model->parameterMaximums(),
                                 m_model->parameterDefaults());
        }
    }
    m_name = QFileInfo(model3Path).completeBaseName().remove(QStringLiteral(".model3"));
    m_manual.clear();
    m_zoom = 1.0;
    m_pan = QPointF();
    m_image = QImage();
    ++m_generation;
    emit modelChanged();
    updateTimer();
    tick();
    return true;
}

void CubismView::clear()
{
    m_model.reset();
    m_idle.reset();
    m_physics.reset();
    m_image = QImage();
    m_manual.clear();
    ++m_generation;
    updateTimer();
    emit modelChanged();
    update();
}

void CubismView::setAutoIdle(bool on)
{
    m_autoIdle = on;
    updateTimer();
}

void CubismView::setFollowMouse(bool on)
{
    m_follow = on;
    updateTimer();
}

void CubismView::setTalking(bool on)
{
    m_talking = on;
    updateTimer();
}

void CubismView::setPlayMotion(bool on)
{
    m_playMotion = on;
    updateTimer();
}

void CubismView::setPhysicsEnabled(bool on)
{
    m_physicsEnabled = on;
    if (on && m_physics)
        m_physics->reset();
    updateTimer();
}

void CubismView::setWind(const QPointF &wind)
{
    m_wind = wind;
    if (m_physics)
        m_physics->setWind(wind);
}

void CubismView::setManual(int parameterIndex, float value)
{
    m_manual.insert(parameterIndex, value);
    if (!m_timer.isActive())
        tick();
}

void CubismView::clearManual()
{
    m_manual.clear();
    if (!m_timer.isActive())
        tick();
}

float CubismView::value(int parameterIndex) const
{
    return m_model ? m_model->parameter(parameterIndex) : 0.0f;
}

void CubismView::updateTimer()
{
    const bool animate = m_model && isVisible()
                         && (m_autoIdle || m_follow || m_talking || (m_playMotion && m_idle) || (m_physicsEnabled && m_physics));
    if (animate && !m_timer.isActive()) {
        m_lastT = m_clock.elapsed() / 1000.0;
        m_timer.start();
    } else if (!animate) {
        m_timer.stop();
        if (m_model)
            tick(); // show the current state once, at full resolution
    }
}

void CubismView::showEvent(QShowEvent *)
{
    updateTimer();
}

void CubismView::hideEvent(QHideEvent *)
{
    m_timer.stop();
}

void CubismView::resizeEvent(QResizeEvent *)
{
    if (m_model && !m_timer.isActive())
        requestRender();
}

void CubismView::applyAnimation(double t, double dt)
{
    live2d::CubismModel &m = *m_model;
    m.resetParameters();
    if (m_playMotion && m_idle) {
        m_idle->evaluate(t, [&m](live2d::MotionCurve::Target target, const QString &id, double v) {
            if (target == live2d::MotionCurve::Target::Parameter)
                m.setParameter(id, float(v));
            else if (target == live2d::MotionCurve::Target::PartOpacity)
                m.setPartOpacity(id, float(v));
        });
    }
    // Procedural motion is added on top of the motion values.
    double tx = 0, ty = 0, tz = 0, tb = 0, ex = 0, ey = 0;
    if (m_follow && m_mouse.x() >= 0) {
        const double nx = std::clamp(m_mouse.x() / std::max(1, width()) * 2.0 - 1.0, -1.0, 1.0);
        const double ny = std::clamp(m_mouse.y() / std::max(1, height()) * 2.0 - 1.0, -1.0, 1.0);
        tx = nx * 30.0;
        ty = -ny * 30.0;
        tz = nx * ny * -10.0;
        tb = nx * 10.0;
        ex = nx;
        ey = -ny;
    } else if (m_autoIdle && !(m_playMotion && m_idle)) {
        tx = 12.0 * std::sin(t * 0.55) + 4.0 * std::sin(t * 1.6 + 0.4);
        ty = 7.0 * std::sin(t * 0.42 + 1.0);
        tz = 5.0 * std::sin(t * 0.5 + 2.0);
        tb = 4.0 * std::sin(t * 0.37);
    }
    m_angleX = approach(m_angleX, tx, dt, 6.0);
    m_angleY = approach(m_angleY, ty, dt, 6.0);
    m_angleZ = approach(m_angleZ, tz, dt, 4.0);
    m_bodyX = approach(m_bodyX, tb, dt, 3.0);
    m_eyeX = approach(m_eyeX, ex, dt, 10.0);
    m_eyeY = approach(m_eyeY, ey, dt, 10.0);
    auto add = [&m](const QString &id, double delta) {
        const int i = m.parameterIndex(id);
        if (i >= 0)
            m.setParameter(i, m.parameter(i) + float(delta));
    };
    add(QStringLiteral("ParamAngleX"), m_angleX);
    add(QStringLiteral("ParamAngleY"), m_angleY);
    add(QStringLiteral("ParamAngleZ"), m_angleZ);
    add(QStringLiteral("ParamBodyAngleX"), m_bodyX);
    add(QStringLiteral("ParamEyeBallX"), m_eyeX);
    add(QStringLiteral("ParamEyeBallY"), m_eyeY);
    if (m_autoIdle || m_follow) {
        m.setParameter(QStringLiteral("ParamBreath"), float((std::sin(t * 1.25) + 1.0) / 2.0));
        // Blinking scales the eye-open parameters (EyeBlink group, or the standard ids).
        if (t >= m_nextBlink) {
            m_blinkStart = t;
            m_nextBlink = t + 2.2 + QRandomGenerator::global()->bounded(3.0);
        }
        const double d = t - m_blinkStart;
        double open = 1.0;
        if (m_blinkStart >= 0 && d < 0.08)
            open = 1.0 - d / 0.08;
        else if (m_blinkStart >= 0 && d < 0.22)
            open = (d - 0.08) / 0.14;
        QStringList eyes = m.settings().eyeBlinkIds;
        if (eyes.isEmpty())
            eyes = {QStringLiteral("ParamEyeLOpen"), QStringLiteral("ParamEyeROpen")};
        for (const QString &id : std::as_const(eyes)) {
            const int i = m.parameterIndex(id);
            if (i >= 0)
                m.setParameter(i, m.parameter(i) * float(open));
        }
    }
    if (m_talking) {
        const float mouth = float(std::clamp(0.5 + 0.5 * std::sin(t * 11.0) * std::sin(t * 3.1 + 1.0), 0.0, 1.0));
        QStringList lips = m.settings().lipSyncIds;
        if (lips.isEmpty())
            lips = {QStringLiteral("ParamMouthOpenY")};
        for (const QString &id : std::as_const(lips))
            m.setParameter(id, mouth);
    }
    // Manual inputs drive the physics too (dragging an angle slider swings the hair).
    for (auto it = m_manual.cbegin(); it != m_manual.cend(); ++it)
        m.setParameter(it.key(), it.value());
    if (m_physicsEnabled && m_physics)
        m_physics->evaluate(m.parameterValues(), m.parameterMinimums(), m.parameterMaximums(), m.parameterDefaults(), dt);
    // A manually set physics output stays where the user put it.
    for (auto it = m_manual.cbegin(); it != m_manual.cend(); ++it)
        m.setParameter(it.key(), it.value());
}

void CubismView::tick()
{
    if (!m_model)
        return;
    const double t = m_clock.elapsed() / 1000.0;
    const double dt = std::clamp(t - m_lastT, 0.0, 0.1);
    m_lastT = t;
    applyAnimation(t, dt);
    requestRender();
}

void CubismView::requestRender()
{
    if (!m_model)
        return;
    if (m_rendering) {
        m_dirty = true; // render again once the current frame is done (drops intermediate frames)
        return;
    }
    m_rendering = true;
    m_dirty = false;
    // While animating, cap the resolution (software rendering); a still frame renders at full size.
    double scale = devicePixelRatioF();
    const int longest = std::max(width(), height());
    if (m_timer.isActive() && longest * scale > kAnimatedResolution)
        scale = kAnimatedResolution / double(std::max(1, longest));
    live2d::RenderOptions options;
    options.size = (QSizeF(size()) * scale).toSize();
    options.zoom = m_zoom;
    options.pan = m_pan * scale;
    const live2d::ModelFrame frame = m_model->update();
    const QList<QImage> textures = m_model->textures();
    const quint64 generation = m_generation;
    QPointer<CubismView> self(this);
    async::run([frame, textures, options] { return live2d::renderFrame(frame, textures, options); })
        .then(this, [self, generation, scale](QImage image) {
            if (!self)
                return;
            self->m_rendering = false;
            if (generation != self->m_generation)
                return;
            image.setDevicePixelRatio(scale);
            self->m_image = image;
            self->update();
            emit self->framePresented();
            if (self->m_dirty)
                self->requestRender();
        });
}

QImage CubismView::renderNow(const QSize &size)
{
    if (!m_model)
        return {};
    live2d::RenderOptions options;
    options.size = size;
    options.zoom = m_zoom;
    return live2d::renderFrame(m_model->update(), m_model->textures(), options);
}

void CubismView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const Palette &pal = Theme::palette();
    p.fillRect(rect(), pal.canvas);
    if (!m_model) {
        p.setPen(pal.textMuted);
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Open a Cubism model (.model3.json) exported from Cubism Editor to preview the rigged result."));
        return;
    }
    if (!m_image.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(rect()), m_image);
    }
    p.setPen(pal.textMuted);
    p.drawText(rect().adjusted(10, 6, -10, -6), Qt::AlignLeft | Qt::AlignBottom,
               tr("Wheel: zoom · drag: pan · double-click: reset"));
}

void CubismView::mouseMoveEvent(QMouseEvent *e)
{
    m_mouse = e->position();
    if (m_dragging) {
        m_pan += e->position() - m_dragStart;
        m_dragStart = e->position();
        if (!m_timer.isActive())
            requestRender();
    }
}

void CubismView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
        m_dragging = true;
        m_dragStart = e->position();
        setCursor(Qt::ClosedHandCursor);
    }
}

void CubismView::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    unsetCursor();
}

void CubismView::mouseDoubleClickEvent(QMouseEvent *)
{
    m_zoom = 1.0;
    m_pan = QPointF();
    if (!m_timer.isActive())
        requestRender();
}

void CubismView::wheelEvent(QWheelEvent *e)
{
    const double factor = std::pow(1.0015, e->angleDelta().y());
    m_zoom = std::clamp(m_zoom * factor, 0.2, 8.0);
    if (!m_timer.isActive())
        requestRender();
}

void CubismView::leaveEvent(QEvent *)
{
    m_mouse = QPointF(-1, -1);
}

} // namespace lumen::ui
