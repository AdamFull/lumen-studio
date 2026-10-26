#include "ui/canvas/Tools.h"

#include "core/ImageOps.h"
#include "ui/Theme.h"
#include "ui/canvas/EditorState.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>

#include <cmath>

namespace lumen::ui {

// ---- shared helpers -----------------------------------------------------------------------------------

QImage brushDab(double diameter, double hardness)
{
    struct Key
    {
        int d;
        int h;
        bool operator==(const Key &o) const { return d == o.d && h == o.h; }
    };
    static Key lastKey{-1, -1};
    static QImage last;
    const int d = std::max(1, int(std::ceil(diameter)));
    const int h = int(std::lround(std::clamp(hardness, 0.0, 1.0) * 100));
    if (lastKey == Key{d, h})
        return last;
    const int size = d + 2;
    QImage dab(size, size, QImage::Format_Grayscale8);
    const double c = size / 2.0;
    const double r = diameter / 2.0;
    const double inner = r * std::clamp(hardness, 0.0, 0.99);
    for (int y = 0; y < size; ++y) {
        uchar *row = dab.scanLine(y);
        for (int x = 0; x < size; ++x) {
            const double dist = std::hypot(x + 0.5 - c, y + 0.5 - c);
            double v;
            if (dist <= inner)
                v = 1.0;
            else if (dist >= r)
                v = std::clamp(r + 0.5 - dist, 0.0, 1.0) * (inner >= r - 0.5 ? 1.0 : 0.0);
            else {
                const double t = (dist - inner) / std::max(1e-6, r - inner);
                v = 1.0 - t * t * (3.0 - 2.0 * t);
            }
            row[x] = uchar(std::lround(v * 255.0));
        }
    }
    lastKey = Key{d, h};
    last = dab;
    return dab;
}

QImage floodFillMask(const QImage &source, const QPoint &seed, int tolerance)
{
    const QImage img = source.convertToFormat(QImage::Format_ARGB32);
    QImage mask = img::createMask(img.size());
    if (!img.rect().contains(seed))
        return mask;
    const QRgb target = img.pixel(seed);
    auto matches = [&](QRgb c) {
        return std::abs(qRed(c) - qRed(target)) <= tolerance && std::abs(qGreen(c) - qGreen(target)) <= tolerance
            && std::abs(qBlue(c) - qBlue(target)) <= tolerance && std::abs(qAlpha(c) - qAlpha(target)) <= tolerance;
    };
    QList<QPoint> stack{seed};
    const int w = img.width(), h = img.height();
    while (!stack.isEmpty()) {
        const QPoint p = stack.takeLast();
        int x = p.x();
        const int y = p.y();
        const auto *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        uchar *m = mask.scanLine(y);
        while (x > 0 && !m[x - 1] && matches(row[x - 1]))
            --x;
        bool spanUp = false, spanDown = false;
        for (; x < w && !m[x] && matches(row[x]); ++x) {
            m[x] = 255;
            for (int dy : {-1, 1}) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h)
                    continue;
                const auto *nrow = reinterpret_cast<const QRgb *>(img.constScanLine(ny));
                const bool open = !mask.constScanLine(ny)[x] && matches(nrow[x]);
                bool &span = dy < 0 ? spanUp : spanDown;
                if (open && !span) {
                    stack.append(QPoint(x, ny));
                    span = true;
                } else if (!open) {
                    span = false;
                }
            }
        }
    }
    return mask;
}

namespace {

QWidget *optionsRow(QWidget *parent)
{
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(14);
    return w;
}

SliderField *addSlider(QWidget *row, const QString &label, double min, double max, double value, int decimals, const QString &suffix,
                       std::function<void(double)> onChange, int width = 210)
{
    auto *s = new SliderField(label, min, max, value, decimals, suffix, row);
    s->setCompact(true);
    s->setFixedWidth(width);
    QObject::connect(s, &SliderField::valueChanged, row, onChange);
    static_cast<QHBoxLayout *>(row->layout())->addWidget(s);
    return s;
}

QRect dabRect(const QPointF &pos, double diameter)
{
    const double r = diameter / 2.0 + 1.0;
    return QRectF(pos.x() - r, pos.y() - r, 2 * r, 2 * r).toAlignedRect();
}

} // namespace

// ---- BrushTool ----------------------------------------------------------------------------------------

BrushTool::BrushTool(EditorState *state, bool eraser, QObject *parent)
    : Tool(state, parent)
    , m_eraser(eraser)
{
}

double BrushTool::brushRadius() const
{
    return state()->brushSize() / 2.0;
}

void BrushTool::press(CanvasView *view, const ToolEvent &ev)
{
    doc::Document *doc = view->document();
    const doc::LayerPtr layer = doc->activeLayer();
    if (!layer) {
        emit message(tr("Add a layer first."));
        return;
    }
    if (layer->props.locked || !layer->props.visible) {
        emit message(layer->props.locked ? tr("The active layer is locked.") : tr("The active layer is hidden."));
        return;
    }
    m_layer = layer->id;
    m_base = layer->image;
    m_base.detach();
    m_stroke = img::createMask(doc->size());
    m_dirty = QRect();
    m_carry = 0.0;
    m_active = true;
    const bool straight = (ev.modifiers & Qt::ShiftModifier) && m_hasLastStroke;
    m_last = straight ? m_lastStrokeEnd : ev.docPos;
    if (straight)
        stampLine(view, m_last, ev.docPos, ev.pressure);
    else
        stamp(view, ev.docPos, ev.pressure);
    m_last = ev.docPos;
}

void BrushTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!m_active)
        return;
    stampLine(view, m_last, ev.docPos, ev.pressure);
    m_last = ev.docPos;
}

void BrushTool::release(CanvasView *view, const ToolEvent &)
{
    if (!m_active)
        return;
    m_active = false;
    m_lastStrokeEnd = m_last;
    m_hasLastStroke = true;
    if (!m_dirty.isEmpty())
        view->document()->commitPaint(m_layer, m_dirty, m_base.copy(m_dirty), m_eraser ? tr("Erase") : tr("Brush stroke"));
    m_base = QImage();
    m_stroke = QImage();
}

void BrushTool::stampLine(CanvasView *view, const QPointF &from, const QPointF &to, double pressure)
{
    const double size = std::max(1.0, state()->brushSize());
    const double spacing = std::max(0.75, size * 0.12);
    const QPointF delta = to - from;
    const double dist = std::hypot(delta.x(), delta.y());
    double t = spacing - m_carry;
    while (t <= dist) {
        stamp(view, from + delta * (t / std::max(1e-6, dist)), pressure);
        t += spacing;
    }
    m_carry = dist - (t - spacing);
}

void BrushTool::stamp(CanvasView *view, const QPointF &pos, double pressure)
{
    doc::Document *doc = view->document();
    QImage *layerImg = doc->layerImageForPainting(m_layer);
    const doc::LayerPtr layer = doc->findLayer(m_layer);
    if (!layerImg || !layer)
        return;
    const double size = std::max(1.0, state()->brushSize() * (state()->pressureSize() ? std::clamp(pressure, 0.05, 1.0) : 1.0));
    const double flow = state()->brushFlow() * (state()->pressureOpacity() ? std::clamp(pressure, 0.0, 1.0) : 1.0);
    const QRect rect = dabRect(pos, size).intersected(doc->bounds());
    if (rect.isEmpty())
        return;
    // Accumulate coverage in the stroke mask (max blend) so overlapping dabs don't build up beyond the stroke opacity.
    {
        QPainter p(&m_stroke);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setCompositionMode(QPainter::CompositionMode_Lighten);
        p.setOpacity(std::clamp(flow, 0.0, 1.0));
        const double r = size / 2.0;
        p.drawImage(QRectF(pos.x() - r - 1, pos.y() - r - 1, size + 2, size + 2), brushDab(size, state()->brushHardness()));
    }
    const double opacity = state()->brushOpacity();
    const QColor color = state()->foreground();
    const int cr = color.red(), cg = color.green(), cb = color.blue();
    const bool alphaLock = layer->props.alphaLocked;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *m = m_stroke.constScanLine(y);
        const auto *base = reinterpret_cast<const QRgb *>(m_base.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(layerImg->scanLine(y));
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const int a = int(m[x] * opacity * color.alphaF());
            if (a == 0) {
                out[x] = base[x];
                continue;
            }
            const QRgb b = base[x];
            const int inv = 255 - a;
            if (m_eraser) {
                out[x] = qRgba(qRed(b) * inv / 255, qGreen(b) * inv / 255, qBlue(b) * inv / 255, qAlpha(b) * inv / 255);
            } else if (alphaLock) {
                const int ba = qAlpha(b);
                out[x] = qRgba((qRed(b) * inv + cr * ba * a / 255) / 255, (qGreen(b) * inv + cg * ba * a / 255) / 255,
                               (qBlue(b) * inv + cb * ba * a / 255) / 255, ba);
            } else {
                out[x] = qRgba(cr * a / 255 + qRed(b) * inv / 255, cg * a / 255 + qGreen(b) * inv / 255,
                               cb * a / 255 + qBlue(b) * inv / 255, a + qAlpha(b) * inv / 255);
            }
        }
    }
    m_dirty = m_dirty.united(rect);
    doc->notifyPixelsChanged(m_layer, rect);
}

bool BrushTool::key(CanvasView *, QKeyEvent *e)
{
    if (e->key() == Qt::Key_BracketLeft) {
        state()->setBrushSize(std::max(1.0, state()->brushSize() / 1.15));
        return true;
    }
    if (e->key() == Qt::Key_BracketRight) {
        state()->setBrushSize(std::min(2000.0, state()->brushSize() * 1.15));
        return true;
    }
    return false;
}

QWidget *BrushTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    EditorState *s = state();
    auto *size = addSlider(row, tr("Size"), 1, 1000, s->brushSize(), 0, QStringLiteral(" px"), [s](double v) { s->setBrushSize(v); });
    addSlider(row, tr("Hardness"), 0, 100, s->brushHardness() * 100, 0, QStringLiteral(" %"), [s](double v) { s->setBrushHardness(v / 100.0); });
    addSlider(row, tr("Opacity"), 1, 100, s->brushOpacity() * 100, 0, QStringLiteral(" %"), [s](double v) { s->setBrushOpacity(v / 100.0); });
    if (!m_eraser)
        addSlider(row, tr("Flow"), 1, 100, s->brushFlow() * 100, 0, QStringLiteral(" %"), [s](double v) { s->setBrushFlow(v / 100.0); });
    auto *ps = new QCheckBox(tr("Pressure → size"), row);
    ps->setChecked(s->pressureSize());
    QObject::connect(ps, &QCheckBox::toggled, row, [s](bool v) { s->setPressureSize(v); });
    auto *po = new QCheckBox(tr("Pressure → opacity"), row);
    po->setChecked(s->pressureOpacity());
    QObject::connect(po, &QCheckBox::toggled, row, [s](bool v) { s->setPressureOpacity(v); });
    row->layout()->addWidget(ps);
    row->layout()->addWidget(po);
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    QObject::connect(s, &EditorState::changed, size, [s, size] {
        if (std::abs(size->value() - s->brushSize()) > 0.5)
            size->setValue(s->brushSize());
    });
    return row;
}

// ---- MaskBrushTool -------------------------------------------------------------------------------------

MaskBrushTool::MaskBrushTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

double MaskBrushTool::brushRadius() const
{
    return state()->maskBrushSize() / 2.0;
}

void MaskBrushTool::press(CanvasView *view, const ToolEvent &ev)
{
    doc::Document *doc = view->document();
    m_wasEmpty = !doc->hasSelection();
    m_before = m_wasEmpty ? img::createMask(doc->size()) : doc->selection().copy();
    doc->selectionForPainting();
    m_subtract = (ev.modifiers & Qt::AltModifier) || ev.button == Qt::RightButton;
    m_dirty = QRect();
    m_carry = 0.0;
    m_active = true;
    m_last = ev.docPos;
    stamp(view, ev.docPos, ev.pressure);
}

void MaskBrushTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!m_active)
        return;
    const double size = std::max(1.0, state()->maskBrushSize());
    const double spacing = std::max(1.0, size * 0.15);
    const QPointF delta = ev.docPos - m_last;
    const double dist = std::hypot(delta.x(), delta.y());
    double t = spacing - m_carry;
    while (t <= dist) {
        stamp(view, m_last + delta * (t / std::max(1e-6, dist)), ev.pressure);
        t += spacing;
    }
    m_carry = dist - (t - spacing);
    m_last = ev.docPos;
}

void MaskBrushTool::release(CanvasView *view, const ToolEvent &)
{
    if (!m_active)
        return;
    m_active = false;
    if (!m_dirty.isEmpty())
        view->document()->commitSelectionPaint(m_dirty, m_before.copy(m_dirty), m_wasEmpty, m_subtract ? tr("Erase from selection") : tr("Paint selection"));
    m_before = QImage();
}

void MaskBrushTool::stamp(CanvasView *view, const QPointF &pos, double pressure)
{
    doc::Document *doc = view->document();
    QImage &mask = doc->selectionForPainting();
    const double size = std::max(1.0, state()->maskBrushSize() * (state()->pressureSize() ? std::clamp(pressure, 0.1, 1.0) : 1.0));
    const QRect rect = dabRect(pos, size).intersected(doc->bounds());
    if (rect.isEmpty())
        return;
    QImage dab = brushDab(size, state()->maskHardness());
    QPainter p(&mask);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double r = size / 2.0;
    const QRectF target(pos.x() - r - 1, pos.y() - r - 1, size + 2, size + 2);
    if (m_subtract) {
        p.setCompositionMode(QPainter::CompositionMode_Darken);
        p.drawImage(target, img::invertMask(dab));
    } else {
        p.setCompositionMode(QPainter::CompositionMode_Lighten);
        p.drawImage(target, dab);
    }
    p.end();
    m_dirty = m_dirty.united(rect);
    doc->notifySelectionChanged(rect);
}

bool MaskBrushTool::key(CanvasView *, QKeyEvent *e)
{
    if (e->key() == Qt::Key_BracketLeft) {
        state()->setMaskBrushSize(std::max(2.0, state()->maskBrushSize() / 1.15));
        return true;
    }
    if (e->key() == Qt::Key_BracketRight) {
        state()->setMaskBrushSize(std::min(2000.0, state()->maskBrushSize() * 1.15));
        return true;
    }
    return false;
}

QWidget *MaskBrushTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    EditorState *s = state();
    auto *size = addSlider(row, tr("Size"), 2, 1500, s->maskBrushSize(), 0, QStringLiteral(" px"), [s](double v) { s->setMaskBrushSize(v); });
    addSlider(row, tr("Hardness"), 0, 100, s->maskHardness() * 100, 0, QStringLiteral(" %"), [s](double v) { s->setMaskHardness(v / 100.0); });
    auto *overlay = new QCheckBox(tr("Tint the selection"), row);
    overlay->setChecked(s->showSelectionOverlay());
    QObject::connect(overlay, &QCheckBox::toggled, row, [s](bool v) { s->setShowSelectionOverlay(v); });
    row->layout()->addWidget(overlay);
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    QObject::connect(s, &EditorState::changed, size, [s, size] {
        if (std::abs(size->value() - s->maskBrushSize()) > 0.5)
            size->setValue(s->maskBrushSize());
    });
    return row;
}

// ---- SelectionTool ---------------------------------------------------------------------------------------

SelectionTool::SelectionTool(EditorState *state, Shape shape, QObject *parent)
    : Tool(state, parent)
    , m_shape(shape)
{
}

QString SelectionTool::id() const
{
    switch (m_shape) {
    case Shape::Rect: return QStringLiteral("select-rect");
    case Shape::Ellipse: return QStringLiteral("select-ellipse");
    case Shape::Lasso: return QStringLiteral("lasso");
    }
    return {};
}

QString SelectionTool::name() const
{
    switch (m_shape) {
    case Shape::Rect: return tr("Rectangle selection");
    case Shape::Ellipse: return tr("Ellipse selection");
    case Shape::Lasso: return tr("Lasso selection");
    }
    return {};
}

QString SelectionTool::iconName() const
{
    return id();
}

QString SelectionTool::shortcut() const
{
    switch (m_shape) {
    case Shape::Rect: return QStringLiteral("M");
    case Shape::Ellipse: return QStringLiteral("Shift+M");
    case Shape::Lasso: return QStringLiteral("L");
    }
    return {};
}

QPainterPath SelectionTool::shapePath() const
{
    QPainterPath path;
    const QRectF r = QRectF(m_start, m_current).normalized();
    switch (m_shape) {
    case Shape::Rect: path.addRect(r); break;
    case Shape::Ellipse: path.addEllipse(r); break;
    case Shape::Lasso:
        path.addPolygon(m_lasso);
        path.closeSubpath();
        break;
    }
    return path;
}

void SelectionTool::press(CanvasView *, const ToolEvent &ev)
{
    m_active = true;
    m_mods = ev.modifiers;
    m_start = m_current = ev.docPos;
    m_lasso = QPolygonF{ev.docPos};
    emit overlayChanged();
}

void SelectionTool::move(CanvasView *, const ToolEvent &ev)
{
    if (!m_active)
        return;
    m_current = ev.docPos;
    if (m_shape == Shape::Lasso && QLineF(m_lasso.last(), ev.docPos).length() > 1.5)
        m_lasso << ev.docPos;
    emit overlayChanged();
}

void SelectionTool::release(CanvasView *view, const ToolEvent &ev)
{
    if (!m_active)
        return;
    m_active = false;
    m_current = ev.docPos;
    doc::Document *doc = view->document();
    const bool add = m_mods & Qt::ShiftModifier;
    const bool sub = m_mods & Qt::AltModifier;
    const bool tiny = m_shape == Shape::Lasso ? m_lasso.size() < 3 : QRectF(m_start, m_current).normalized().width() * view->zoom() < 3;
    if (tiny) {
        if (!add && !sub)
            doc->clearSelection();
        emit overlayChanged();
        return;
    }
    QImage shape = img::createMask(doc->size());
    {
        QPainter p(&shape);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawPath(shapePath());
    }
    img::MaskOp op = img::MaskOp::Replace;
    if (add && sub)
        op = img::MaskOp::Intersect;
    else if (add)
        op = img::MaskOp::Union;
    else if (sub)
        op = img::MaskOp::Subtract;
    const QImage current = doc->hasSelection() ? doc->selection() : img::createMask(doc->size());
    doc->applySelection(img::combineMasks(current, shape, op), name());
    m_lasso.clear();
    emit overlayChanged();
}

void SelectionTool::paintOverlay(CanvasView *view, QPainter &p)
{
    if (!m_active)
        return;
    p.save();
    p.translate(view->mapFromDoc(QPointF(0, 0)));
    p.scale(view->zoom(), view->zoom());
    QPen white(Qt::white, 0);
    white.setCosmetic(true);
    p.setPen(white);
    p.setBrush(Theme::palette().selection);
    p.drawPath(shapePath());
    QPen dash(Qt::black, 0, Qt::DashLine);
    dash.setCosmetic(true);
    p.setPen(dash);
    p.setBrush(Qt::NoBrush);
    p.drawPath(shapePath());
    p.restore();
}

// ---- EyedropperTool ---------------------------------------------------------------------------------------

EyedropperTool::EyedropperTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

void EyedropperTool::press(CanvasView *view, const ToolEvent &ev)
{
    doc::Document *doc = view->document();
    const QPoint pt = ev.docPos.toPoint();
    if (!doc->bounds().contains(pt))
        return;
    QColor c;
    if (state()->sampleMerged()) {
        c = doc->flattened(QRect(pt, QSize(1, 1))).pixelColor(0, 0);
    } else if (const doc::LayerPtr l = doc->activeLayer()) {
        c = l->image.pixelColor(pt);
    }
    if (!c.isValid() || c.alpha() == 0)
        return;
    c.setAlpha(255);
    if ((ev.modifiers & Qt::AltModifier) || ev.button == Qt::RightButton)
        state()->setBackground(c);
    else
        state()->setForeground(c);
}

QWidget *EyedropperTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    auto *merged = new QCheckBox(tr("Sample all layers"), row);
    merged->setChecked(state()->sampleMerged());
    EditorState *s = state();
    QObject::connect(merged, &QCheckBox::toggled, row, [s](bool v) { s->setSampleMerged(v); });
    row->layout()->addWidget(merged);
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    return row;
}

// ---- FillTool ---------------------------------------------------------------------------------------------

FillTool::FillTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

void FillTool::press(CanvasView *view, const ToolEvent &ev)
{
    doc::Document *doc = view->document();
    const doc::LayerPtr layer = doc->activeLayer();
    const QPoint pt = ev.docPos.toPoint();
    if (!layer || !doc->bounds().contains(pt))
        return;
    if (layer->props.locked) {
        emit message(tr("The active layer is locked."));
        return;
    }
    QImage region;
    if (doc->hasSelection() && doc->selection().constScanLine(pt.y())[pt.x()] > 0) {
        region = doc->selection();
    } else {
        const QImage sample = state()->sampleMerged() ? doc->flattened() : layer->image;
        region = floodFillMask(sample, pt, state()->fillTolerance());
        if (doc->hasSelection())
            region = img::combineMasks(region, doc->selection(), img::MaskOp::Intersect);
    }
    const QRect bounds = img::maskBounds(region);
    if (bounds.isEmpty())
        return;
    QImage pixels = layer->image.copy(bounds);
    QImage fill(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    fill.fill(state()->foreground());
    {
        QPainter p(&pixels);
        p.drawImage(0, 0, img::applyMaskToAlpha(fill, region.copy(bounds)));
    }
    doc->applyPixels(layer->id, bounds, pixels, tr("Fill"));
}

QWidget *FillTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    EditorState *s = state();
    addSlider(row, tr("Tolerance"), 0, 255, s->fillTolerance(), 0, QString(), [s](double v) { s->setFillTolerance(int(v)); });
    auto *merged = new QCheckBox(tr("Sample all layers"), row);
    merged->setChecked(s->sampleMerged());
    QObject::connect(merged, &QCheckBox::toggled, row, [s](bool v) { s->setSampleMerged(v); });
    row->layout()->addWidget(merged);
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    return row;
}

// ---- MoveTool -------------------------------------------------------------------------------------------------

MoveTool::MoveTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

void MoveTool::press(CanvasView *view, const ToolEvent &ev)
{
    const doc::LayerPtr layer = view->document()->activeLayer();
    if (!layer || layer->props.locked) {
        if (layer)
            emit message(tr("The active layer is locked."));
        return;
    }
    m_layer = layer->id;
    m_base = layer->image;
    m_base.detach();
    m_start = ev.docPos;
    m_offset = QPoint();
    m_active = true;
}

void MoveTool::apply(CanvasView *view, const QPoint &offset)
{
    QImage *img = view->document()->layerImageForPainting(m_layer);
    if (!img)
        return;
    img->fill(Qt::transparent);
    QPainter p(img);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(offset, m_base);
    p.end();
    view->document()->notifyPixelsChanged(m_layer, view->document()->bounds());
}

void MoveTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!m_active)
        return;
    const QPoint offset = (ev.docPos - m_start).toPoint();
    if (offset == m_offset)
        return;
    m_offset = offset;
    apply(view, offset);
}

void MoveTool::release(CanvasView *view, const ToolEvent &)
{
    if (!m_active)
        return;
    m_active = false;
    if (!m_offset.isNull())
        view->document()->commitPaint(m_layer, view->document()->bounds(), m_base, tr("Move layer"));
    m_base = QImage();
}

bool MoveTool::key(CanvasView *view, QKeyEvent *e)
{
    QPoint d;
    const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    switch (e->key()) {
    case Qt::Key_Left: d = QPoint(-step, 0); break;
    case Qt::Key_Right: d = QPoint(step, 0); break;
    case Qt::Key_Up: d = QPoint(0, -step); break;
    case Qt::Key_Down: d = QPoint(0, step); break;
    default: return false;
    }
    const doc::LayerPtr layer = view->document()->activeLayer();
    if (!layer || layer->props.locked)
        return true;
    QImage moved(layer->image.size(), QImage::Format_ARGB32_Premultiplied);
    moved.fill(Qt::transparent);
    QPainter p(&moved);
    p.drawImage(d, layer->image);
    p.end();
    view->document()->applyPixels(layer->id, view->document()->bounds(), moved, tr("Nudge layer"));
    return true;
}

// ---- CropTool ---------------------------------------------------------------------------------------------------

CropTool::CropTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

void CropTool::activated(CanvasView *view)
{
    m_view = view;
    reset();
}

void CropTool::reset()
{
    if (m_view && m_view->document())
        m_rect = QRectF(m_view->document()->bounds());
    emit overlayChanged();
}

void CropTool::setAspect(double aspect)
{
    m_aspect = aspect;
    if (aspect > 0 && !m_rect.isEmpty()) {
        const QPointF c = m_rect.center();
        double w = m_rect.width(), h = m_rect.height();
        if (w / h > aspect)
            w = h * aspect;
        else
            h = w / aspect;
        m_rect = QRectF(c.x() - w / 2, c.y() - h / 2, w, h);
    }
    emit overlayChanged();
}

void CropTool::apply()
{
    if (!m_view || !m_view->document())
        return;
    doc::Document *doc = m_view->document();
    const QRect r = m_rect.toRect().normalized();
    if (r.width() < 2 || r.height() < 2 || r == doc->bounds())
        return;
    const bool extends = !doc->bounds().contains(r);
    doc->resizeCanvas(r, extends ? tr("Resize canvas") : tr("Crop"));
    reset();
}

int CropTool::hitTest(CanvasView *view, const QPointF &w) const
{
    const QRectF r = view->mapFromDoc(m_rect);
    const QPointF pts[8] = {r.topLeft(), QPointF(r.center().x(), r.top()), r.topRight(), QPointF(r.right(), r.center().y()),
                            r.bottomRight(), QPointF(r.center().x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), r.center().y())};
    for (int i = 0; i < 8; ++i) {
        if (QLineF(pts[i], w).length() < 9)
            return i;
    }
    if (r.contains(w))
        return 8;
    return 9;
}

void CropTool::press(CanvasView *view, const ToolEvent &ev)
{
    m_view = view;
    m_handle = hitTest(view, ev.widgetPos);
    m_start = ev.docPos;
    m_startRect = m_rect;
    if (m_handle == 9)
        m_rect = QRectF(ev.docPos, QSizeF(0, 0));
}

void CropTool::move(CanvasView *, const ToolEvent &ev)
{
    const QPointF d = ev.docPos - m_start;
    QRectF r = m_startRect;
    switch (m_handle) {
    case 0: r.setTopLeft(r.topLeft() + d); break;
    case 1: r.setTop(r.top() + d.y()); break;
    case 2: r.setTopRight(r.topRight() + d); break;
    case 3: r.setRight(r.right() + d.x()); break;
    case 4: r.setBottomRight(r.bottomRight() + d); break;
    case 5: r.setBottom(r.bottom() + d.y()); break;
    case 6: r.setBottomLeft(r.bottomLeft() + d); break;
    case 7: r.setLeft(r.left() + d.x()); break;
    case 8: r.translate(d); break;
    case 9: r = QRectF(m_start, ev.docPos); break;
    default: return;
    }
    r = r.normalized();
    if (m_aspect > 0 && m_handle != 8) {
        if (m_handle == 1 || m_handle == 5)
            r.setWidth(r.height() * m_aspect);
        else
            r.setHeight(r.width() / m_aspect);
    }
    m_rect = QRectF(std::round(r.left()), std::round(r.top()), std::round(r.width()), std::round(r.height()));
    emit overlayChanged();
}

void CropTool::release(CanvasView *, const ToolEvent &)
{
    m_handle = -1;
}

void CropTool::hover(CanvasView *view, const ToolEvent &ev)
{
    static const Qt::CursorShape shapes[10] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                               Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                               Qt::SizeAllCursor, Qt::CrossCursor};
    view->setCursorOverride(shapes[hitTest(view, ev.widgetPos)]);
}

bool CropTool::key(CanvasView *, QKeyEvent *e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        apply();
        return true;
    }
    if (e->key() == Qt::Key_Escape) {
        reset();
        return true;
    }
    return false;
}

void CropTool::paintOverlay(CanvasView *view, QPainter &p)
{
    if (m_rect.isEmpty())
        return;
    const QRectF r = view->mapFromDoc(m_rect);
    p.save();
    QPainterPath outside;
    outside.addRect(QRectF(QPointF(-100000, -100000), QSizeF(200000, 200000)));
    QPainterPath inside;
    inside.addRect(r);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 120));
    p.setPen(QPen(QColor(255, 255, 255, 90), 1));
    for (int i = 1; i < 3; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 3, r.top()), QPointF(r.left() + r.width() * i / 3, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 3), QPointF(r.right(), r.top() + r.height() * i / 3));
    }
    p.setPen(QPen(Theme::palette().accent, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    p.setBrush(Qt::white);
    p.setPen(QPen(Theme::palette().accent, 1.5));
    const QPointF pts[8] = {r.topLeft(), QPointF(r.center().x(), r.top()), r.topRight(), QPointF(r.right(), r.center().y()),
                            r.bottomRight(), QPointF(r.center().x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), r.center().y())};
    for (const QPointF &pt : pts)
        p.drawRect(QRectF(pt - QPointF(4, 4), QSizeF(8, 8)));
    p.setPen(Qt::white);
    p.drawText(r.adjusted(6, 4, 0, 0), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("%1 × %2").arg(int(m_rect.width())).arg(int(m_rect.height())));
    p.restore();
}

QWidget *CropTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    auto *aspect = new QComboBox(row);
    aspect->addItem(tr("Free"), 0.0);
    for (const auto &[label, value] : std::initializer_list<std::pair<const char *, double>>{
             {"1:1", 1.0}, {"2:3", 2.0 / 3.0}, {"3:2", 1.5}, {"3:4", 0.75}, {"4:3", 4.0 / 3.0}, {"9:16", 9.0 / 16.0}, {"16:9", 16.0 / 9.0}})
        aspect->addItem(QString::fromLatin1(label), value);
    QObject::connect(aspect, &QComboBox::currentIndexChanged, this, [this, aspect] { setAspect(aspect->currentData().toDouble()); });
    row->layout()->addWidget(new QLabel(tr("Aspect"), row));
    row->layout()->addWidget(aspect);
    auto *applyBtn = new QPushButton(tr("Apply"), row);
    Theme::setRole(applyBtn, QStringLiteral("primary"));
    auto *resetBtn = new QPushButton(tr("Reset"), row);
    QObject::connect(applyBtn, &QPushButton::clicked, this, &CropTool::apply);
    QObject::connect(resetBtn, &QPushButton::clicked, this, &CropTool::reset);
    row->layout()->addWidget(applyBtn);
    row->layout()->addWidget(resetBtn);
    row->layout()->addWidget(mutedLabel(tr("Drag beyond the image to extend the canvas (for outpainting)."), row));
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    return row;
}

HandTool::HandTool(EditorState *state, QObject *parent)
    : Tool(state, parent)
{
}

} // namespace lumen::ui
