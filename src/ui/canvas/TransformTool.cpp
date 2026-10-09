#include "ui/canvas/TransformTool.h"

#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>

#include <cmath>

namespace lumen::ui {

namespace {

// Handle directions in source space: corners and edge midpoints, clockwise from the top left.
constexpr int kHandleX[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
constexpr int kHandleY[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
constexpr double kHandleRadius = 8.0;

double degrees(double radians)
{
    return radians * 180.0 / M_PI;
}

// Rotates a vector by `angle` degrees (clockwise on screen, y down).
QPointF rotated(const QPointF &v, double angle)
{
    const double a = angle * M_PI / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return QPointF(v.x() * c - v.y() * s, v.x() * s + v.y() * c);
}

double clampScale(double s, double extent)
{
    const double minimum = 1.0 / std::max(1.0, extent);
    if (std::abs(s) >= minimum)
        return s;
    return s < 0 ? -minimum : minimum;
}

} // namespace

TransformTool::TransformTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

QString TransformTool::hint() const
{
    return tr("Inside: move · handles: scale (Shift keeps aspect, Alt from centre) · outside: rotate (Shift snaps) · "
              "Enter applies, Esc reverts");
}

void TransformTool::activated(CanvasView *view)
{
    m_view = view;
    endSession();
    emit overlayChanged();
}

void TransformTool::deactivated(CanvasView *)
{
    finish();
}

QTransform TransformTool::transformFor(const Params &params) const
{
    const QPointF sc = QRectF(m_src).center();
    QTransform t;
    t.translate(params.center.x(), params.center.y());
    t.rotate(params.angle);
    t.scale(params.sx, params.sy);
    t.translate(-sc.x(), -sc.y());
    return t;
}

QPolygonF TransformTool::boxFor(const Params &params) const
{
    return transformFor(params).map(QPolygonF(QRectF(m_src)));
}

QRect TransformTool::boundsFor(const Params &params) const
{
    if (!m_doc)
        return {};
    return transformFor(params).mapRect(QRectF(m_src)).toAlignedRect().adjusted(-1, -1, 1, 1).intersected(m_doc->bounds());
}

bool TransformTool::ensureSession(bool report)
{
    if (!m_view)
        return false;
    doc::Document *doc = m_view->document();
    if (!doc)
        return false;
    if (m_doc == doc && m_layer == doc->activeLayerId() && m_revision == doc->revision())
        return true;
    endSession();
    const doc::LayerPtr layer = doc->activeLayer();
    if (!layer)
        return false;
    if (layer->props.locked) {
        if (report)
            emit message(tr("The active layer is locked."));
        return false;
    }
    QRect src;
    QImage floating, mask;
    QImage rest = layer->image;
    if (doc->hasSelection()) {
        src = doc->selectionBounds().intersected(doc->bounds());
        if (src.isEmpty())
            return false;
        // The selected pixels float; the rest of the layer stays.
        const QImage selection = doc->selection().copy(src);
        mask = QImage(src.size(), QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::black); // setAlphaChannel multiplies with the existing alpha
        mask.setAlphaChannel(selection);
        floating = layer->image.copy(src);
        QPainter fp(&floating);
        fp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        fp.drawImage(0, 0, mask);
        fp.end();
        rest.detach();
        QPainter rp(&rest);
        rp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        rp.drawImage(src.topLeft(), mask);
        rp.end();
    } else {
        src = img::maskBounds(img::maskFromAlpha(layer->image));
        if (src.isEmpty()) {
            if (report)
                emit message(tr("The active layer is empty."));
            return false;
        }
        floating = layer->image.copy(src);
        rest = QImage(layer->image.size(), QImage::Format_ARGB32_Premultiplied);
        rest.fill(Qt::transparent);
    }
    m_doc = doc;
    m_layer = layer->id;
    m_revision = doc->revision();
    m_startIndex = doc->undoStack()->index();
    m_pushed = false;
    m_rest = rest;
    m_float = floating;
    m_floatMask = mask;
    m_src = src;
    m_params = Params{QRectF(src).center(), 1.0, 1.0, 0.0};
    m_committed = m_params;
    m_shown = boundsFor(m_params);
    return true;
}

void TransformTool::endSession()
{
    m_doc = nullptr;
    m_layer = QUuid();
    m_rest = QImage();
    m_float = QImage();
    m_floatMask = QImage();
    m_before = QImage();
    m_src = QRect();
    m_dragging = false;
    m_handle = -1;
}

void TransformTool::preview(const Params &params)
{
    QImage *img = m_doc->layerImageForPainting(m_layer);
    if (!img)
        return;
    const QRect next = boundsFor(params);
    const QRect dirty = m_shown.united(next);
    QPainter p(img);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(dirty.topLeft(), m_rest, dirty);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setClipRect(dirty);
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.setTransform(transformFor(params));
    p.drawImage(m_src.topLeft(), m_float);
    p.end();
    m_params = params;
    m_shown = next;
    m_doc->notifyPixelsChanged(m_layer, dirty);
    emit overlayChanged();
}

void TransformTool::commit(const QString &undoText)
{
    const QRect changed = boundsFor(m_committed).united(m_shown);
    if (changed.isEmpty() || m_before.isNull())
        return;
    QUndoStack *stack = m_doc->undoStack();
    stack->beginMacro(undoText);
    m_doc->commitPaint(m_layer, changed, m_before.copy(changed), undoText);
    if (!m_floatMask.isNull()) {
        // The selection follows the pixels.
        QImage moved(m_doc->size(), QImage::Format_ARGB32_Premultiplied);
        moved.fill(Qt::transparent);
        QPainter p(&moved);
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
        p.setTransform(transformFor(m_params));
        p.drawImage(m_src.topLeft(), m_floatMask);
        p.end();
        m_doc->applySelection(img::maskFromAlpha(moved), undoText);
    }
    stack->endMacro();
    m_before = QImage();
    m_committed = m_params;
    m_pushed = true;
    m_revision = m_doc->revision();
}

bool TransformTool::setParams(const Params &params)
{
    if (!ensureSession())
        return false;
    m_before = m_doc->findLayer(m_layer)->image;
    preview(params);
    commit(tr("Transform"));
    return true;
}

bool TransformTool::flip(Qt::Orientation orientation)
{
    if (!ensureSession())
        return false;
    // Mirroring on screen around the box centre: M·R(a)·S = R(-a)·M·S.
    Params p = m_params;
    if (orientation == Qt::Horizontal)
        p.sx = -p.sx;
    else
        p.sy = -p.sy;
    p.angle = -p.angle;
    return setParams(p);
}

bool TransformTool::rotateBy(double degreesDelta)
{
    if (!ensureSession())
        return false;
    Params p = m_params;
    p.angle = std::remainder(p.angle + degreesDelta, 360.0);
    return setParams(p);
}

void TransformTool::finish()
{
    if (m_dragging && m_doc)
        commit(tr("Transform"));
    endSession();
    emit overlayChanged();
}

void TransformTool::cancel()
{
    if (m_doc && m_dragging) {
        m_dragging = false;
        preview(m_committed);
        m_before = QImage();
        m_revision = m_doc->revision();
    }
    // Undo this session's steps when nothing else changed the document meanwhile.
    if (m_doc && m_pushed && m_revision == m_doc->revision() && m_doc->undoStack()->index() > m_startIndex)
        m_doc->undoStack()->setIndex(m_startIndex);
    endSession();
    emit overlayChanged();
}

int TransformTool::hitTest(const QPointF &w) const
{
    if (!m_view || !m_doc)
        return 9;
    const QTransform t = transformFor(m_params);
    const QPointF sc = QRectF(m_src).center();
    for (int i = 0; i < 8; ++i) {
        const QPointF local(sc.x() + kHandleX[i] * m_src.width() / 2.0, sc.y() + kHandleY[i] * m_src.height() / 2.0);
        if (QLineF(m_view->mapFromDoc(t.map(local)), w).length() <= kHandleRadius)
            return i;
    }
    QPolygonF box;
    for (const QPointF &p : boxFor(m_params))
        box << m_view->mapFromDoc(p);
    return box.containsPoint(w, Qt::OddEvenFill) ? 8 : 9;
}

void TransformTool::press(CanvasView *view, const ToolEvent &ev)
{
    m_view = view;
    if (ev.button != Qt::LeftButton || !ensureSession())
        return;
    m_handle = hitTest(ev.widgetPos);
    m_start = m_params;
    m_pressDoc = ev.docPos;
    m_before = m_doc->findLayer(m_layer)->image;
    m_dragging = true;
}

void TransformTool::move(CanvasView *, const ToolEvent &ev)
{
    if (!m_dragging || !m_doc)
        return;
    Params p = m_start;
    const QPointF mouse = ev.docPos;
    const bool shift = ev.modifiers & Qt::ShiftModifier;
    if (m_handle == 8) {
        QPointF d = mouse - m_pressDoc;
        if (shift)
            d = std::abs(d.x()) >= std::abs(d.y()) ? QPointF(d.x(), 0) : QPointF(0, d.y());
        p.center = m_start.center + d;
    } else if (m_handle == 9) {
        const QPointF a = m_pressDoc - m_start.center, b = mouse - m_start.center;
        double angle = m_start.angle + degrees(std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x()));
        if (shift)
            angle = std::round(angle / 15.0) * 15.0;
        p.angle = std::remainder(angle, 360.0);
    } else if (m_handle >= 0) {
        const int ux = kHandleX[m_handle], uy = kHandleY[m_handle];
        const double w = m_src.width(), h = m_src.height();
        const bool fromCenter = ev.modifiers & Qt::AltModifier;
        // Handle offsets from the anchor in the box's local (rotated) frame.
        const QPointF half(m_start.sx * w / 2.0, m_start.sy * h / 2.0);
        const QPointF anchor = fromCenter ? m_start.center : m_start.center + rotated(QPointF(-ux * half.x(), -uy * half.y()), m_start.angle);
        QPointF d = rotated(mouse - anchor, -m_start.angle);
        const double span = fromCenter ? 1.0 : 2.0;
        if (shift && ux != 0 && uy != 0) {
            const QPointF h0(ux * span * half.x(), uy * span * half.y());
            const double f = QPointF::dotProduct(d, h0) / std::max(1e-9, QPointF::dotProduct(h0, h0));
            d = h0 * f;
        }
        if (ux != 0)
            p.sx = clampScale(d.x() * ux * (fromCenter ? 2.0 : 1.0) / w, w);
        if (uy != 0)
            p.sy = clampScale(d.y() * uy * (fromCenter ? 2.0 : 1.0) / h, h);
        // The centre sits half a box from the anchor along the dragged axes.
        if (!fromCenter)
            p.center = anchor + rotated(QPointF(ux * p.sx * w / 2.0, uy * p.sy * h / 2.0), m_start.angle);
    } else {
        return;
    }
    preview(p);
}

void TransformTool::release(CanvasView *, const ToolEvent &)
{
    if (!m_dragging || !m_doc)
        return;
    m_dragging = false;
    const bool unchanged = m_params.center == m_committed.center && m_params.sx == m_committed.sx && m_params.sy == m_committed.sy
                           && m_params.angle == m_committed.angle;
    if (unchanged) {
        m_before = QImage();
        m_revision = m_doc->revision();
        return;
    }
    commit(tr("Transform"));
}

void TransformTool::hover(CanvasView *view, const ToolEvent &ev)
{
    m_view = view;
    if (!ensureSession(false))
        return;
    const int hit = hitTest(ev.widgetPos);
    if (hit == 8) {
        view->setCursorOverride(Qt::SizeAllCursor);
    } else if (hit == 9) {
        view->setCursorOverride(Qt::CrossCursor);
    } else {
        // Pick the resize cursor closest to the handle's on-screen direction.
        const QPointF dir = rotated(QPointF(kHandleX[hit] * m_params.sx, kHandleY[hit] * m_params.sy), m_params.angle);
        double a = std::fmod(degrees(std::atan2(dir.y(), dir.x())) + 360.0, 180.0);
        static const Qt::CursorShape shapes[4] = {Qt::SizeHorCursor, Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor};
        view->setCursorOverride(shapes[int(std::lround(a / 45.0)) % 4]);
    }
}

bool TransformTool::key(CanvasView *view, QKeyEvent *e)
{
    m_view = view;
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        finish();
        return true;
    case Qt::Key_Escape:
        cancel();
        return true;
    default:
        break;
    }
    QPointF d;
    const double step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    switch (e->key()) {
    case Qt::Key_Left: d = QPointF(-step, 0); break;
    case Qt::Key_Right: d = QPointF(step, 0); break;
    case Qt::Key_Up: d = QPointF(0, -step); break;
    case Qt::Key_Down: d = QPointF(0, step); break;
    default: return false;
    }
    if (!ensureSession())
        return true;
    Params p = m_params;
    p.center += d;
    setParams(p);
    return true;
}

void TransformTool::paintOverlay(CanvasView *view, QPainter &p)
{
    m_view = view;
    if (!m_dragging && !ensureSession(false))
        return;
    QPolygonF box;
    for (const QPointF &pt : boxFor(m_params))
        box << view->mapFromDoc(pt);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::palette().accent, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(box);
    const QTransform t = transformFor(m_params);
    const QPointF sc = QRectF(m_src).center();
    p.setBrush(Qt::white);
    for (int i = 0; i < 8; ++i) {
        const QPointF local(sc.x() + kHandleX[i] * m_src.width() / 2.0, sc.y() + kHandleY[i] * m_src.height() / 2.0);
        const QPointF w = view->mapFromDoc(t.map(local));
        p.drawRect(QRectF(w - QPointF(4, 4), QSizeF(8, 8)));
    }
    const QPointF c = view->mapFromDoc(m_params.center);
    p.drawLine(c - QPointF(5, 0), c + QPointF(5, 0));
    p.drawLine(c - QPointF(0, 5), c + QPointF(0, 5));
    if (m_dragging) {
        const QString info = m_handle == 9 ? QStringLiteral("%1°").arg(m_params.angle, 0, 'f', 1)
                             : m_handle == 8 ? QStringLiteral("%1, %2")
                                                   .arg(std::lround(m_params.center.x() - QRectF(m_src).center().x()))
                                                   .arg(std::lround(m_params.center.y() - QRectF(m_src).center().y()))
                                             : QStringLiteral("%1 × %2 px")
                                                   .arg(std::lround(std::abs(m_params.sx) * m_src.width()))
                                                   .arg(std::lround(std::abs(m_params.sy) * m_src.height()));
        const QRectF br = box.boundingRect();
        const QRectF label(br.center().x() - 60, br.bottom() + 10, 120, 22);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(label, 4, 4);
        p.setPen(Qt::white);
        p.drawText(label, Qt::AlignCenter, info);
    }
    p.restore();
}

QWidget *TransformTool::createOptions(QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
    auto button = [&](const QString &text, const QString &tip, auto &&slot) {
        auto *b = new QPushButton(text, row);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, this, slot);
        l->addWidget(b);
        return b;
    };
    button(tr("Flip H"), tr("Mirror horizontally"), [this] { flip(Qt::Horizontal); });
    button(tr("Flip V"), tr("Mirror vertically"), [this] { flip(Qt::Vertical); });
    button(tr("Rotate −90°"), tr("Rotate counter-clockwise"), [this] { rotateBy(-90); });
    button(tr("Rotate +90°"), tr("Rotate clockwise"), [this] { rotateBy(90); });
    auto *smooth = new QCheckBox(tr("Smooth"), row);
    smooth->setChecked(m_smooth);
    smooth->setToolTip(tr("Bilinear resampling (off: nearest neighbour, for pixel art)"));
    QObject::connect(smooth, &QCheckBox::toggled, this, [this](bool on) { m_smooth = on; });
    l->addSpacing(8);
    l->addWidget(smooth);
    l->addSpacing(8);
    auto *apply = button(tr("Apply"), tr("Finish the transform (Enter)"), [this] { finish(); });
    Theme::setRole(apply, QStringLiteral("primary"));
    button(tr("Revert"), tr("Undo every step of this transform (Esc)"), [this] { cancel(); });
    QLabel *note = mutedLabel(tr("With a selection, only the selected pixels move."), row);
    note->setWordWrap(false);
    l->addWidget(note);
    l->addStretch();
    return row;
}

} // namespace lumen::ui
