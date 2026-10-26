#include "ui/canvas/CanvasView.h"

#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/Settings.h"
#include "pose/PoseRenderer.h"
#include "ui/Theme.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/Tool.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPointer>
#include <QTabletEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace lumen::ui {

namespace {

constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 64.0;

class GLSurface : public QOpenGLWidget
{
public:
    GLSurface(CanvasView *view, QWidget *parent)
        : QOpenGLWidget(parent)
        , m_view(view)
    {
    }

protected:
    void paintGL() override
    {
        QPainter p(this);
        m_view->render(p, rect());
    }

private:
    CanvasView *m_view;
};

class RasterSurface : public QWidget
{
public:
    RasterSurface(CanvasView *view, QWidget *parent)
        : QWidget(parent)
        , m_view(view)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        m_view->render(p, rect());
    }

private:
    CanvasView *m_view;
};

bool useOpenGL()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))
        return false;
    return Settings::instance().value(QStringLiteral("perf/gpuCanvas"), true).toBool();
}

// Marching squares outline of a mask (document coordinates). Runs on a worker thread.
QPainterPath maskContour(const QImage &mask)
{
    QPainterPath path;
    if (mask.isNull())
        return path;
    const int maxSide = 1400;
    const double s = std::max(1.0, double(std::max(mask.width(), mask.height())) / maxSide);
    const QImage m = s > 1.0 ? img::thresholdMask(img::scaled(mask, QSize(int(mask.width() / s), int(mask.height() / s)), true), 128)
                             : img::thresholdMask(mask, 128);
    const int w = m.width(), h = m.height();
    auto at = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;
        return m.constScanLine(y)[x] != 0;
    };
    auto seg = [&](double x1, double y1, double x2, double y2) {
        path.moveTo(x1 * s, y1 * s);
        path.lineTo(x2 * s, y2 * s);
    };
    for (int y = -1; y < h; ++y) {
        for (int x = -1; x < w; ++x) {
            const int c = (at(x, y) ? 8 : 0) | (at(x + 1, y) ? 4 : 0) | (at(x + 1, y + 1) ? 2 : 0) | (at(x, y + 1) ? 1 : 0);
            if (c == 0 || c == 15)
                continue;
            // Cell corners at pixel centres (x+0.5, y+0.5) .. (x+1.5, y+1.5); edges midpoints:
            const double cx = x + 1.0, cy = y + 1.0;
            const QPointF top(cx, cy - 0.5), right(cx + 0.5, cy), bottom(cx, cy + 0.5), left(cx - 0.5, cy);
            auto line = [&](const QPointF &a, const QPointF &b) { seg(a.x(), a.y(), b.x(), b.y()); };
            switch (c) {
            case 1: case 14: line(left, bottom); break;
            case 2: case 13: line(bottom, right); break;
            case 3: case 12: line(left, right); break;
            case 4: case 11: line(top, right); break;
            case 5: line(left, top); line(bottom, right); break;
            case 6: case 9: line(top, bottom); break;
            case 7: case 8: line(left, top); break;
            case 10: line(top, right); line(left, bottom); break;
            default: break;
            }
        }
    }
    return path;
}

} // namespace

CanvasView::CanvasView(EditorState *state, QWidget *parent)
    : QWidget(parent)
    , m_state(state)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (useOpenGL())
        m_surface = new GLSurface(this, this);
    else
        m_surface = new RasterSurface(this, this);
    layout->addWidget(m_surface);
    m_surface->setMouseTracking(true);
    m_surface->setFocusPolicy(Qt::StrongFocus);
    m_surface->setAttribute(Qt::WA_TabletTracking);
    m_surface->installEventFilter(this);
    setFocusProxy(m_surface);

    m_contourTimer.setSingleShot(true);
    m_contourTimer.setInterval(90);
    connect(&m_contourTimer, &QTimer::timeout, this, &CanvasView::scheduleContour);
    m_antsTimer.setInterval(140);
    connect(&m_antsTimer, &QTimer::timeout, this, [this] {
        m_antsOffset = (m_antsOffset + 1) % 8;
        m_surface->update();
    });
    connect(state, &EditorState::changed, this, [this] { m_surface->update(); });
}

CanvasView::~CanvasView() = default;

void CanvasView::setDocument(doc::Document *doc)
{
    if (m_doc == doc)
        return;
    if (m_doc)
        m_doc->disconnect(this);
    m_doc = doc;
    m_contour = QPainterPath();
    m_antsTimer.stop();
    // A preview belongs to the document it was made of.
    const bool hadFilter = !m_filterPreview.isNull(), hadResult = !m_resultPreview.isNull();
    m_filterPreview = QImage();
    m_resultPreview = QImage();
    m_previewScaled = QImage();
    m_fitPending = true;
    if (doc) {
        m_overlay.reset(doc->size());
        connect(doc, &doc::Document::compositeChanged, this, [this](const QRect &r) { updateDocRect(r); });
        connect(doc, &doc::Document::selectionChanged, this, [this](const QRect &r) { rebuildSelectionOverlay(r); });
        connect(doc, &doc::Document::sizeChanged, this, [this](const QSize &s) {
            m_overlay.reset(s);
            fitToView();
        });
        connect(doc, &doc::Document::controlPoseChanged, this, &CanvasView::updateAll);
        rebuildSelectionOverlay(doc->bounds());
        QTimer::singleShot(0, this, &CanvasView::fitToView);
    }
    if (hadFilter)
        emit previewChanged(Preview::Filter);
    if (hadResult)
        emit previewChanged(Preview::Result);
    emit documentChanged(doc);
    updateAll();
}

void CanvasView::setTool(Tool *tool)
{
    if (m_tool == tool)
        return;
    if (m_tool) {
        m_tool->deactivated(this);
        m_tool->disconnect(this);
    }
    m_tool = tool;
    if (m_toolActive) {
        m_toolActive = false;
        emit interactionChanged(false);
    }
    if (m_tool) {
        m_tool->activated(this);
        connect(m_tool, &Tool::overlayChanged, this, &CanvasView::updateAll);
        m_surface->setCursor(m_tool->brushRadius() > 0 ? QCursor(Qt::BlankCursor) : m_tool->cursor());
    }
    updateAll();
}

void CanvasView::setCursorOverride(const QCursor &cursor)
{
    m_surface->setCursor(cursor);
}

void CanvasView::setPreview(Preview kind, const QImage &image)
{
    QImage &slot = kind == Preview::Result ? m_resultPreview : m_filterPreview;
    if (image.isNull() && slot.isNull())
        return;
    slot = image;
    m_previewScaled = QImage();
    emit previewChanged(kind);
    updateAll();
}

// ---- view transform -----------------------------------------------------------------------------

QPointF CanvasView::mapToDoc(const QPointF &widgetPos) const
{
    return (widgetPos - m_pan) / m_zoom;
}

QPointF CanvasView::mapFromDoc(const QPointF &docPos) const
{
    return docPos * m_zoom + m_pan;
}

QRectF CanvasView::mapFromDoc(const QRectF &docRect) const
{
    return QRectF(mapFromDoc(docRect.topLeft()), docRect.size() * m_zoom);
}

QRectF CanvasView::visibleDocRect() const
{
    return QRectF(mapToDoc(QPointF(0, 0)), QSizeF(m_surface->size()) / m_zoom);
}

void CanvasView::setZoom(double zoom, const QPointF &anchor)
{
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const QPointF docAnchor = mapToDoc(anchor);
    m_zoom = zoom;
    m_pan = anchor - docAnchor * m_zoom;
    m_fitted = false;
    clampPan();
    emit zoomChanged(m_zoom);
    emit viewChanged();
    updateAll();
}

void CanvasView::zoomIn()
{
    setZoom(m_zoom * 1.25, QPointF(m_surface->width() / 2.0, m_surface->height() / 2.0));
}

void CanvasView::zoomOut()
{
    setZoom(m_zoom / 1.25, QPointF(m_surface->width() / 2.0, m_surface->height() / 2.0));
}

void CanvasView::fitToView()
{
    // Fitting needs the final widget size: postpone until the view is shown and laid out.
    if (!m_doc || !m_surface->isVisible() || m_surface->width() < 64 || m_surface->height() < 64) {
        m_fitPending = true;
        return;
    }
    m_fitPending = false;
    m_fitted = true;
    const QSizeF avail = QSizeF(m_surface->size()) * 0.92;
    const QSize s = m_doc->size();
    m_zoom = std::clamp(std::min(avail.width() / s.width(), avail.height() / s.height()), kMinZoom, kMaxZoom);
    m_pan = QPointF((m_surface->width() - s.width() * m_zoom) / 2.0, (m_surface->height() - s.height() * m_zoom) / 2.0);
    emit zoomChanged(m_zoom);
    emit viewChanged();
    updateAll();
}

void CanvasView::actualPixels()
{
    setZoom(1.0 / devicePixelRatioF(), QPointF(m_surface->width() / 2.0, m_surface->height() / 2.0));
}

void CanvasView::panBy(const QPointF &delta)
{
    m_pan += delta;
    m_fitted = false;
    clampPan();
    emit viewChanged();
    updateAll();
}

void CanvasView::clampPan()
{
    if (!m_doc)
        return;
    // Keep at least 64 px of the document visible.
    const QSizeF docSize = QSizeF(m_doc->size()) * m_zoom;
    const double margin = 64.0;
    m_pan.setX(std::clamp(m_pan.x(), margin - docSize.width(), m_surface->width() - margin));
    m_pan.setY(std::clamp(m_pan.y(), margin - docSize.height(), m_surface->height() - margin));
}

void CanvasView::updateDocRect(const QRectF &docRect)
{
    m_surface->update(mapFromDoc(docRect).toAlignedRect().adjusted(-2, -2, 2, 2));
}

void CanvasView::updateAll()
{
    m_surface->update();
}

void CanvasView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    // A fitted picture stays whole (e.g. when the results strip takes some of the height).
    if (m_fitPending || m_fitted)
        QTimer::singleShot(0, this, &CanvasView::fitToView);
    emit viewChanged();
}

void CanvasView::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_fitPending)
        QTimer::singleShot(0, this, &CanvasView::fitToView);
}

// ---- selection overlay ------------------------------------------------------------------------------

void CanvasView::rebuildSelectionOverlay(const QRect &rect)
{
    m_overlay.invalidate(rect);
    m_contourTimer.start();
    updateDocRect(rect);
}

void CanvasView::scheduleContour()
{
    if (!m_doc || !m_doc->hasSelection()) {
        m_contour = QPainterPath();
        m_antsTimer.stop();
        updateAll();
        return;
    }
    const quint64 generation = ++m_contourGeneration;
    const QImage mask = m_doc->selection();
    QPointer<CanvasView> self(this);
    async::run([mask] { return maskContour(mask); }).then(this, [self, generation](QPainterPath path) {
        if (!self || generation != self->m_contourGeneration)
            return;
        self->m_contour = path;
        if (!path.isEmpty())
            self->m_antsTimer.start();
        else
            self->m_antsTimer.stop();
        self->updateAll();
    });
}

// ---- rendering --------------------------------------------------------------------------------------

void CanvasView::drawCheckerboard(QPainter &p, const QRectF &rect)
{
    if (m_checker.isNull()) {
        const int c = 8;
        m_checker = QPixmap(c * 2, c * 2);
        const bool dark = Theme::instance().isDark();
        m_checker.fill(dark ? QColor(0x3a3d44) : QColor(0xffffff));
        QPainter cp(&m_checker);
        const QColor alt = dark ? QColor(0x2c2f35) : QColor(0xd9dbe0);
        cp.fillRect(0, 0, c, c, alt);
        cp.fillRect(c, c, c, c, alt);
    }
    p.fillRect(rect, QBrush(m_checker));
}

void CanvasView::render(QPainter &p, const QRect &widgetRect)
{
    const Palette &pal = Theme::palette();
    p.fillRect(widgetRect, pal.canvas);
    if (!m_doc) {
        p.setPen(pal.textMuted);
        p.drawText(widgetRect, Qt::AlignCenter, tr("Open an image or create a new document\n(File → New, or drop an image here)"));
        return;
    }
    const QRectF docWidget = mapFromDoc(QRectF(m_doc->bounds()));
    // Soft drop shadow.
    for (int i = 1; i <= 6; ++i) {
        QColor shadow(0, 0, 0, 14 - i * 2);
        p.fillRect(docWidget.adjusted(-i, -i + 2, i, i + 2), shadow);
    }
    drawCheckerboard(p, docWidget);

    const QRectF visible = visibleDocRect().intersected(QRectF(m_doc->bounds()));
    if (visible.isEmpty())
        return;
    const QRect visibleDoc = visible.toAlignedRect();
    const int level = doc::TileGrid::levelForZoom(m_zoom * devicePixelRatioF());
    m_doc->updateComposite(visibleDoc, level);
    const bool smooth = m_zoom < 2.0;
    p.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    doc::TileGrid &tiles = m_doc->compositeTiles();
    const int ts = doc::TileGrid::kTileSize;
    const int c0 = visibleDoc.left() / ts, c1 = visibleDoc.right() / ts;
    const int r0 = visibleDoc.top() / ts, r1 = visibleDoc.bottom() / ts;
    auto snapped = [this](const QRect &docRect) {
        // Snap tile edges to whole device pixels so adjacent tiles never leave seams.
        const QPointF tl = mapFromDoc(QPointF(docRect.topLeft()));
        const QPointF br = mapFromDoc(QPointF(docRect.right() + 1, docRect.bottom() + 1));
        return QRectF(QPointF(std::round(tl.x()), std::round(tl.y())), QPointF(std::round(br.x()), std::round(br.y())));
    };
    // A result to choose from or a filter's preview instead of the document's pixels (its
    // transparency shows the checkerboard). Below 100 % a copy at the current zoom is drawn as it
    // is, so that the repaints of the marching ants stay cheap.
    const QImage &shown = preview();
    if (!shown.isNull()) {
        const double scale = m_zoom * devicePixelRatioF();
        if (scale < 1.0) {
            const QSize target = (QSizeF(m_doc->size()) * scale).toSize().expandedTo(QSize(1, 1));
            if (m_previewScaled.size() != target)
                m_previewScaled = shown.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            p.drawImage(docWidget, m_previewScaled);
        } else {
            const double sx = double(shown.width()) / m_doc->size().width();
            const double sy = double(shown.height()) / m_doc->size().height();
            const QRectF source(visible.left() * sx, visible.top() * sy, visible.width() * sx, visible.height() * sy);
            p.drawImage(mapFromDoc(visible), shown, source);
        }
    } else {
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                const QImage &tile = tiles.tile(c, r, level);
                if (!tile.isNull())
                    p.drawImage(snapped(tiles.tileRect(c, r)), tile);
            }
        }
    }

    // Selection tint (not over a preview, whose colours it would hide).
    if (m_doc->hasSelection() && m_state->showSelectionOverlay() && shown.isNull()) {
        const QImage &sel = m_doc->selection();
        const QColor tint = pal.accent;
        m_overlay.update(visibleDoc, level, [&sel, tint](QImage &target, const QRect &rect) {
            target.fill(Qt::transparent);
            const int a = 110;
            for (int y = 0; y < rect.height(); ++y) {
                const uchar *m = sel.constScanLine(rect.top() + y) + rect.left();
                auto *out = reinterpret_cast<QRgb *>(target.scanLine(y));
                for (int x = 0; x < rect.width(); ++x) {
                    const int alpha = m[x] * a / 255;
                    out[x] = alpha ? qRgba(tint.red() * alpha / 255, tint.green() * alpha / 255, tint.blue() * alpha / 255, alpha) : 0;
                }
            }
        });
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                const QImage &tile = m_overlay.tile(c, r, level);
                if (!tile.isNull())
                    p.drawImage(snapped(m_overlay.tileRect(c, r)), tile);
            }
        }
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    // Control pose overlay.
    if (m_doc->controlPose() && m_state->showPoseOverlay()) {
        const pose::Pose &pose = *m_doc->controlPose();
        const double sx = double(m_doc->size().width()) / pose.canvas.width();
        const double sy = double(m_doc->size().height()) / pose.canvas.height();
        p.save();
        p.setOpacity(0.75);
        for (const pose::Person &person : pose.people) {
            const auto &limbs = pose::bodyLimbs();
            for (int i = 0; i < limbs.size(); ++i) {
                const auto &a = person.body[static_cast<size_t>(limbs[i].first)];
                const auto &b = person.body[static_cast<size_t>(limbs[i].second)];
                if (!a.visible || !b.visible)
                    continue;
                p.setPen(QPen(pose::bodyColor(i), 3, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(mapFromDoc(QPointF(a.pos.x() * sx, a.pos.y() * sy)), mapFromDoc(QPointF(b.pos.x() * sx, b.pos.y() * sy)));
            }
        }
        p.restore();
    }

    // Marching ants.
    if (!m_contour.isEmpty() && m_state->showSelectionOverlay()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.translate(m_pan);
        p.scale(m_zoom, m_zoom);
        QPen white(Qt::white, 0);
        white.setCosmetic(true);
        p.setPen(white);
        p.drawPath(m_contour);
        QPen black(Qt::black, 0, Qt::CustomDashLine);
        black.setCosmetic(true);
        black.setDashPattern({4, 4});
        black.setDashOffset(m_antsOffset);
        p.setPen(black);
        p.drawPath(m_contour);
        p.restore();
    }

    // Document border.
    p.setPen(QPen(Theme::mix(pal.borderStrong, pal.canvas, 0.3), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(docWidget.adjusted(-0.5, -0.5, 0.5, 0.5));

    if (m_tool)
        m_tool->paintOverlay(this, p);

    // Brush cursor.
    if (m_tool && m_tool->brushRadius() > 0 && m_surface->underMouse()) {
        const QPointF c = m_surface->mapFromGlobal(QCursor::pos());
        const double r = std::max(1.5, m_tool->brushRadius() * m_zoom);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1.5));
        p.drawEllipse(c, r, r);
        p.setPen(QPen(QColor(255, 255, 255, 220), 1));
        p.drawEllipse(c, r, r);
        if (r > 6) {
            p.drawLine(c - QPointF(3, 0), c + QPointF(3, 0));
            p.drawLine(c - QPointF(0, 3), c + QPointF(0, 3));
        }
    }
}

// ---- input ------------------------------------------------------------------------------------------

bool CanvasView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_surface)
        return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::MouseButtonDblClick:
        handleMouse(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        handleTablet(static_cast<QTabletEvent *>(event));
        return true;
    case QEvent::Wheel:
        handleWheel(static_cast<QWheelEvent *>(event));
        return true;
    case QEvent::KeyPress:
        handleKey(static_cast<QKeyEvent *>(event), true);
        return event->isAccepted();
    case QEvent::KeyRelease:
        handleKey(static_cast<QKeyEvent *>(event), false);
        return event->isAccepted();
    case QEvent::Leave:
    case QEvent::Enter:
        updateAll();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void CanvasView::dispatch(QEvent::Type type, const ToolEvent &ev)
{
    emit cursorMoved(ev.docPos);
    if (!m_tool || !m_doc) {
        updateAll();
        return;
    }
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        m_toolActive = true;
        emit interactionChanged(true);
        m_tool->press(this, ev);
        break;
    case QEvent::MouseMove:
    case QEvent::TabletMove:
        if (m_toolActive)
            m_tool->move(this, ev);
        else
            m_tool->hover(this, ev);
        break;
    case QEvent::MouseButtonRelease:
    case QEvent::TabletRelease:
        if (m_toolActive) {
            m_toolActive = false;
            m_tool->release(this, ev);
            emit interactionChanged(false);
        }
        break;
    default:
        break;
    }
    if (m_tool->brushRadius() > 0)
        updateAll();
}

void CanvasView::handleMouse(QMouseEvent *e)
{
    if (m_tabletActive && e->source() != Qt::MouseEventNotSynthesized)
        return;
    const QPointF pos = e->position();
    const bool panButton = e->button() == Qt::MiddleButton || (e->buttons() & Qt::MiddleButton) || m_spacePan;
    if (e->type() == QEvent::MouseButtonPress && panButton) {
        m_panning = true;
        m_lastPanPos = pos;
        m_surface->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_panning) {
        if (e->type() == QEvent::MouseMove) {
            panBy(pos - m_lastPanPos);
            m_lastPanPos = pos;
        } else if (e->type() == QEvent::MouseButtonRelease) {
            m_panning = false;
            if (m_spacePan)
                m_surface->setCursor(Qt::OpenHandCursor);
            else if (m_tool)
                m_surface->setCursor(m_tool->brushRadius() > 0 ? QCursor(Qt::BlankCursor) : m_tool->cursor());
            else
                m_surface->unsetCursor();
        }
        return;
    }
    ToolEvent ev;
    ev.widgetPos = pos;
    ev.docPos = mapToDoc(pos);
    ev.button = e->button();
    ev.buttons = e->buttons();
    ev.modifiers = e->modifiers();
    if (e->type() == QEvent::MouseButtonDblClick)
        return;
    if (e->type() == QEvent::MouseButtonPress && e->button() != Qt::LeftButton && e->button() != Qt::RightButton)
        return;
    dispatch(e->type(), ev);
}

void CanvasView::handleTablet(QTabletEvent *e)
{
    e->accept();
    const QPointF pos = e->position();
    if (m_spacePan || (e->buttons() & Qt::MiddleButton)) {
        if (e->type() == QEvent::TabletPress) {
            m_panning = true;
            m_lastPanPos = pos;
        } else if (e->type() == QEvent::TabletMove && m_panning) {
            panBy(pos - m_lastPanPos);
            m_lastPanPos = pos;
        } else if (e->type() == QEvent::TabletRelease) {
            m_panning = false;
        }
        return;
    }
    if (e->type() == QEvent::TabletPress)
        m_tabletActive = true;
    ToolEvent ev;
    ev.widgetPos = pos;
    ev.docPos = mapToDoc(pos);
    ev.button = e->button();
    ev.buttons = e->buttons();
    ev.modifiers = e->modifiers();
    ev.pressure = e->pressure();
    ev.tablet = true;
    dispatch(e->type(), ev);
    if (e->type() == QEvent::TabletRelease)
        m_tabletActive = false;
}

void CanvasView::handleWheel(QWheelEvent *e)
{
    if (!m_doc)
        return;
    const QPoint pixel = e->pixelDelta();
    const QPoint angle = e->angleDelta();
    if (!pixel.isNull() && !(e->modifiers() & Qt::ControlModifier)) {
        panBy(QPointF(pixel));
        return;
    }
    if (e->modifiers() & Qt::ShiftModifier) {
        panBy(QPointF(angle.y() / 2.0, angle.x() / 2.0));
        return;
    }
    const double steps = angle.y() / 120.0;
    if (steps == 0.0)
        return;
    setZoom(m_zoom * std::pow(1.18, steps), e->position());
}

void CanvasView::handleKey(QKeyEvent *e, bool press)
{
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spacePan = press;
        if (!m_panning) {
            if (press)
                m_surface->setCursor(Qt::OpenHandCursor);
            else if (m_tool)
                m_surface->setCursor(m_tool->brushRadius() > 0 ? QCursor(Qt::BlankCursor) : m_tool->cursor());
        }
        e->accept();
        return;
    }
    if (press && m_tool && m_tool->key(this, e)) {
        e->accept();
        return;
    }
    // The tool comes first (the move tool's arrows, the transform and crop tools' Enter and Esc).
    if (press && m_keyFallback && m_keyFallback(e)) {
        e->accept();
        return;
    }
    e->ignore();
}

} // namespace lumen::ui
