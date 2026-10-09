#include "ui/parts/PartTools.h"

#include "ai/PartPipeline.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/MaskOps.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace lumen::ui {

namespace {

QWidget *optionsRow(QWidget *parent)
{
    auto *w = new QWidget(parent);
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(14);
    return w;
}

SliderField *addSlider(QWidget *row, const QString &label, double min, double max, double value, const QString &suffix,
                       std::function<void(double)> onChange)
{
    auto *s = new SliderField(label, min, max, value, 0, suffix, row);
    s->setCompact(true);
    s->setFixedWidth(210);
    QObject::connect(s, &SliderField::valueChanged, row, onChange);
    static_cast<QHBoxLayout *>(row->layout())->addWidget(s);
    return s;
}

QCheckBox *addSilhouetteToggle(QWidget *row, PartEditContext *context)
{
    auto *box = new QCheckBox(QObject::tr("Only inside the character"), row);
    box->setToolTip(QObject::tr("Painting never claims background pixels outside the character silhouette."));
    box->setChecked(context->insideSilhouette());
    QObject::connect(box, &QCheckBox::toggled, context, &PartEditContext::setInsideSilhouette);
    row->layout()->addWidget(box);
    return box;
}

void beginDocPainter(QPainter &p, CanvasView *view)
{
    p.save();
    p.translate(view->mapFromDoc(QPointF(0, 0)));
    p.scale(view->zoom(), view->zoom());
}

const std::pair<int, int> kBones[] = {
    {pose::Neck, pose::RShoulder}, {pose::Neck, pose::LShoulder}, {pose::RShoulder, pose::RElbow}, {pose::RElbow, pose::RWrist},
    {pose::LShoulder, pose::LElbow}, {pose::LElbow, pose::LWrist}, {pose::Neck, pose::RHip}, {pose::RHip, pose::RKnee},
    {pose::RKnee, pose::RAnkle}, {pose::Neck, pose::LHip}, {pose::LHip, pose::LKnee}, {pose::LKnee, pose::LAnkle},
    {pose::Neck, pose::Nose}, {pose::Nose, pose::REye}, {pose::REye, pose::REar}, {pose::Nose, pose::LEye}, {pose::LEye, pose::LEar}};

QColor jointColor(int joint)
{
    switch (joint) {
    case pose::RShoulder: case pose::RElbow: case pose::RWrist: case pose::RHip: case pose::RKnee: case pose::RAnkle:
    case pose::REye: case pose::REar:
        return QColor(255, 150, 60);
    case pose::LShoulder: case pose::LElbow: case pose::LWrist: case pose::LHip: case pose::LKnee: case pose::LAnkle:
    case pose::LEye: case pose::LEar:
        return QColor(70, 200, 255);
    default:
        return QColor(250, 250, 250);
    }
}

} // namespace

// ---- Context ------------------------------------------------------------------------------------------

PartEditContext::PartEditContext(QObject *parent)
    : QObject(parent)
{
}

void PartEditContext::setSet(doc::PartSet *set)
{
    m_set = set;
    m_active = 0;
    m_skeleton.reset();
    emit setChanged();
    emit activeLabelChanged(0);
    emit skeletonChanged();
}

void PartEditContext::setActiveLabel(int label)
{
    if (m_active == label)
        return;
    m_active = label;
    emit activeLabelChanged(label);
}

void PartEditContext::setInsideSilhouette(bool on)
{
    m_insideSilhouette = on;
    emit optionsChanged();
}

void PartEditContext::setWandTolerance(int t)
{
    m_tolerance = t;
    emit optionsChanged();
}

void PartEditContext::setWandContiguous(bool on)
{
    m_contiguous = on;
    emit optionsChanged();
}

void PartEditContext::setSkeleton(const std::optional<pose::Person> &person)
{
    m_skeleton = person;
    emit skeletonChanged();
}

int PartEditContext::labelAt(const QPointF &docPos) const
{
    if (!m_set || m_set->labels().isNull())
        return 0;
    const QPoint p(int(std::floor(docPos.x())), int(std::floor(docPos.y())));
    if (!m_set->labels().rect().contains(p))
        return 0;
    return m_set->labels().constScanLine(p.y())[p.x()];
}

void PartEditContext::assign(const QImage &maskIn, bool remove, const QString &undoText)
{
    if (!m_set)
        return;
    const QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8);
    const QRect bounds = img::maskBounds(mask, 128).intersected(m_set->labels().rect());
    if (bounds.isEmpty())
        return;
    const QImage before = m_set->labels().copy(bounds);
    const QVector<bool> locked = m_set->lockedLabels();
    const QImage &sil = m_set->silhouette();
    const bool clip = m_insideSilhouette && !sil.isNull() && sil.size() == mask.size();
    QImage &labels = m_set->labelsForPainting();
    bool changed = false;
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        const uchar *m = mask.constScanLine(y);
        const uchar *s = clip ? sil.constScanLine(y) : nullptr;
        uchar *row = labels.scanLine(y);
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            if (m[x] < 128)
                continue;
            const uchar v = row[x];
            if (remove) {
                if ((m_active == 0 && v != 0 && !locked[v]) || (m_active != 0 && v == m_active)) {
                    row[x] = 0;
                    changed = true;
                }
            } else if (v != m_active && !locked[v] && (!s || s[x] >= 128)) {
                row[x] = uchar(m_active);
                changed = true;
            }
        }
    }
    if (!changed)
        return;
    m_set->notifyLabelsChanged(bounds);
    m_set->commitLabelPaint(bounds, before, undoText);
}

// ---- Base ------------------------------------------------------------------------------------------------

PartToolBase::PartToolBase(EditorState *state, PartEditContext *context, QObject *parent)
    : Tool(state, parent)
    , m_context(context)
{
}

bool PartToolBase::pickWithCtrl(const ToolEvent &ev)
{
    if (!(ev.modifiers & Qt::ControlModifier))
        return false;
    m_context->setActiveLabel(m_context->labelAt(ev.docPos));
    return true;
}

bool PartToolBase::ensureActive()
{
    if (!m_context->set() || !m_context->set()->hasSource()) {
        emit message(tr("Choose a source image first."));
        return false;
    }
    if (!m_context->activePart()) {
        emit message(tr("Select a part in the list (or add one) before painting."));
        return false;
    }
    return true;
}

// ---- Brush -----------------------------------------------------------------------------------------------

PartBrushTool::PartBrushTool(EditorState *state, PartEditContext *context, Mode mode, QObject *parent)
    : PartToolBase(state, context, parent)
    , m_mode(mode)
{
}

QString PartBrushTool::id() const
{
    switch (m_mode) {
    case Mode::Paint: return QStringLiteral("part-brush");
    case Mode::Erase: return QStringLiteral("part-eraser");
    case Mode::Extend: return QStringLiteral("part-extend");
    }
    return {};
}

QString PartBrushTool::name() const
{
    switch (m_mode) {
    case Mode::Paint: return tr("Part brush");
    case Mode::Erase: return tr("Part eraser");
    case Mode::Extend: return tr("Hidden area brush");
    }
    return {};
}

QString PartBrushTool::iconName() const
{
    switch (m_mode) {
    case Mode::Paint: return QStringLiteral("brush");
    case Mode::Erase: return QStringLiteral("eraser");
    case Mode::Extend: return QStringLiteral("outpaint");
    }
    return {};
}

QString PartBrushTool::shortcut() const
{
    switch (m_mode) {
    case Mode::Paint: return QStringLiteral("B");
    case Mode::Erase: return QStringLiteral("E");
    case Mode::Extend: return QStringLiteral("X");
    }
    return {};
}

QString PartBrushTool::hint() const
{
    switch (m_mode) {
    case Mode::Paint:
        return tr("Paint pixels into the selected part (taken from other parts). Alt or right button removes. Ctrl+click picks a part. [ ] size.");
    case Mode::Erase:
        return tr("Remove pixels from the selected part (from every unlocked part when none is selected). Ctrl+click picks a part.");
    case Mode::Extend:
        return tr("Paint where the selected part continues behind other parts (hatched). Generation fills this area. Alt removes.");
    }
    return {};
}

double PartBrushTool::brushRadius() const
{
    return state()->maskBrushSize() / 2.0;
}

void PartBrushTool::press(CanvasView *, const ToolEvent &ev)
{
    if (pickWithCtrl(ev))
        return;
    doc::PartSet *set = context()->set();
    const bool needsPart = m_mode == Mode::Extend || (m_mode == Mode::Paint && !(ev.modifiers & Qt::AltModifier) && ev.button != Qt::RightButton);
    if (needsPart ? !ensureActive() : (!set || !set->hasSource()))
        return;
    m_remove = m_mode == Mode::Erase || (ev.modifiers & Qt::AltModifier) || ev.button == Qt::RightButton;
    m_locked = set->lockedLabels();
    m_dirty = QRect();
    if (m_mode == Mode::Extend)
        m_extension = set->extensionMask(context()->activeLabel());
    else
        m_before = set->labels(); // shared copy; painting detaches the live map
    m_active = true;
    m_last = ev.docPos;
    stamp(ev.docPos);
}

void PartBrushTool::move(CanvasView *, const ToolEvent &ev)
{
    if (!m_active)
        return;
    stampTo(ev.docPos);
}

void PartBrushTool::release(CanvasView *, const ToolEvent &)
{
    if (!m_active)
        return;
    m_active = false;
    doc::PartSet *set = context()->set();
    if (!set || m_dirty.isEmpty()) {
        m_before = QImage();
        m_extension = QImage();
        return;
    }
    if (m_mode == Mode::Extend) {
        set->setExtension(context()->activeLabel(), m_extension, m_remove ? tr("Shrink hidden area") : tr("Paint hidden area"));
        m_extension = QImage();
        return;
    }
    set->commitLabelPaint(m_dirty, m_before.copy(m_dirty), m_remove ? tr("Erase part pixels") : tr("Paint part pixels"));
    m_before = QImage();
}

void PartBrushTool::stampTo(const QPointF &pos)
{
    const double size = std::max(1.0, state()->maskBrushSize());
    const double spacing = std::max(1.0, size * 0.2);
    const QPointF delta = pos - m_last;
    const double dist = std::hypot(delta.x(), delta.y());
    if (dist < spacing)
        return;
    const int steps = int(dist / spacing);
    for (int i = 1; i <= steps; ++i)
        stamp(m_last + delta * (double(i) / steps));
    m_last = pos;
}

void PartBrushTool::stamp(const QPointF &pos)
{
    doc::PartSet *set = context()->set();
    if (!set)
        return;
    const double r = std::max(0.5, state()->maskBrushSize() / 2.0);
    const QRect rect = QRectF(pos.x() - r - 1, pos.y() - r - 1, 2 * r + 2, 2 * r + 2).toAlignedRect().intersected(set->labels().rect());
    if (rect.isEmpty())
        return;
    const double r2 = r * r;
    const int active = context()->activeLabel();
    if (m_mode == Mode::Extend) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            uchar *row = m_extension.scanLine(y);
            const double dy = y + 0.5 - pos.y();
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const double dx = x + 0.5 - pos.x();
                if (dx * dx + dy * dy <= r2)
                    row[x] = m_remove ? 0 : 255;
            }
        }
        m_dirty |= rect;
        emit context()->extensionPreview(m_extension, rect);
        return;
    }
    const QImage &sil = set->silhouette();
    const bool clip = context()->insideSilhouette() && !sil.isNull() && sil.size() == set->labels().size();
    QImage &labels = set->labelsForPainting();
    bool changed = false;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        uchar *row = labels.scanLine(y);
        const uchar *s = clip ? sil.constScanLine(y) : nullptr;
        const double dy = y + 0.5 - pos.y();
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const double dx = x + 0.5 - pos.x();
            if (dx * dx + dy * dy > r2)
                continue;
            const uchar v = row[x];
            if (m_remove) {
                if ((active == 0 && v != 0 && !m_locked[v]) || (active != 0 && v == active)) {
                    row[x] = 0;
                    changed = true;
                }
            } else if (v != active && !m_locked[v] && (!s || s[x] >= 128)) {
                row[x] = uchar(active);
                changed = true;
            }
        }
    }
    if (changed) {
        m_dirty |= rect;
        set->notifyLabelsChanged(rect);
    }
}

bool PartBrushTool::key(CanvasView *, QKeyEvent *e)
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

QWidget *PartBrushTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    EditorState *s = state();
    auto *size = addSlider(row, tr("Size"), 2, 1500, s->maskBrushSize(), QStringLiteral(" px"), [s](double v) { s->setMaskBrushSize(v); });
    QObject::connect(s, &EditorState::changed, size, [s, size] {
        if (std::abs(size->value() - s->maskBrushSize()) > 0.5)
            size->setValue(s->maskBrushSize());
    });
    if (m_mode != Mode::Extend)
        addSilhouetteToggle(row, context());
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    return row;
}

// ---- Lasso -----------------------------------------------------------------------------------------------

PartLassoTool::PartLassoTool(EditorState *state, PartEditContext *context, QObject *parent)
    : PartToolBase(state, context, parent)
{
}

QString PartLassoTool::hint() const
{
    return tr("Draw around an area to add it to the selected part. Alt or right button removes it. Ctrl+click picks a part.");
}

void PartLassoTool::press(CanvasView *view, const ToolEvent &ev)
{
    if (pickWithCtrl(ev))
        return;
    m_remove = (ev.modifiers & Qt::AltModifier) || ev.button == Qt::RightButton;
    if (!m_remove && !ensureActive())
        return;
    m_points = {ev.docPos};
    m_active = true;
    view->updateAll();
}

void PartLassoTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!m_active)
        return;
    if (QLineF(m_points.last(), ev.docPos).length() * view->zoom() >= 2.0) {
        m_points << ev.docPos;
        view->updateAll();
    }
}

void PartLassoTool::release(CanvasView *view, const ToolEvent &)
{
    if (!m_active)
        return;
    m_active = false;
    doc::PartSet *set = context()->set();
    if (set && m_points.size() >= 3) {
        QImage area(set->size(), QImage::Format_Alpha8);
        area.fill(0);
        QPainter p(&area);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 255));
        p.drawPolygon(m_points);
        p.end();
        // Alpha8 -> Grayscale8 mask.
        QImage mask = img::createMask(set->size(), 0);
        for (int y = 0; y < mask.height(); ++y)
            memcpy(mask.scanLine(y), area.constScanLine(y), size_t(mask.width()));
        context()->assign(mask, m_remove, m_remove ? tr("Remove lasso area") : tr("Add lasso area"));
    }
    m_points.clear();
    view->updateAll();
}

void PartLassoTool::paintOverlay(CanvasView *view, QPainter &p)
{
    if (!m_active || m_points.size() < 2)
        return;
    beginDocPainter(p, view);
    QPen white(Qt::white, 0);
    white.setCosmetic(true);
    p.setPen(white);
    QColor fill = m_remove ? QColor(255, 80, 80, 50) : Theme::palette().selection;
    p.setBrush(fill);
    p.drawPolygon(m_points);
    QPen dash(Qt::black, 0, Qt::DashLine);
    dash.setCosmetic(true);
    p.setPen(dash);
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(m_points);
    p.restore();
}

QWidget *PartLassoTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    addSilhouetteToggle(row, context());
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    return row;
}

// ---- Wand ------------------------------------------------------------------------------------------------

PartWandTool::PartWandTool(EditorState *state, PartEditContext *context, QObject *parent)
    : PartToolBase(state, context, parent)
{
}

QString PartWandTool::hint() const
{
    return tr("Click a colour area to add it to the selected part. Alt or right button removes it. Ctrl+click picks a part.");
}

void PartWandTool::press(CanvasView *, const ToolEvent &ev)
{
    if (pickWithCtrl(ev))
        return;
    const bool remove = (ev.modifiers & Qt::AltModifier) || ev.button == Qt::RightButton;
    if (!remove && !ensureActive())
        return;
    doc::PartSet *set = context()->set();
    if (!set)
        return;
    const QPoint seed(int(std::floor(ev.docPos.x())), int(std::floor(ev.docPos.y())));
    const QImage limit = context()->insideSilhouette() ? set->silhouette() : QImage();
    const QImage region = img::colorRegion(set->source(), seed, context()->wandTolerance(), context()->wandContiguous(), limit);
    context()->assign(region, remove, remove ? tr("Remove colour area") : tr("Add colour area"));
}

QWidget *PartWandTool::createOptions(QWidget *parent)
{
    QWidget *row = optionsRow(parent);
    PartEditContext *ctx = context();
    addSlider(row, tr("Tolerance"), 0, 255, ctx->wandTolerance(), QString(), [ctx](double v) { ctx->setWandTolerance(int(v)); });
    auto *contiguous = new QCheckBox(tr("Contiguous"), row);
    contiguous->setChecked(ctx->wandContiguous());
    QObject::connect(contiguous, &QCheckBox::toggled, ctx, &PartEditContext::setWandContiguous);
    row->layout()->addWidget(contiguous);
    addSilhouetteToggle(row, ctx);
    static_cast<QHBoxLayout *>(row->layout())->addStretch();
    return row;
}

// ---- Cut line ---------------------------------------------------------------------------------------------

PartCutTool::PartCutTool(EditorState *state, PartEditContext *context, QObject *parent)
    : PartToolBase(state, context, parent)
{
}

QString PartCutTool::hint() const
{
    return tr("Drag a line across the selected part to cut it in two (for example to separate an arm from the hand). Ctrl+click picks a part.");
}

void PartCutTool::press(CanvasView *view, const ToolEvent &ev)
{
    if (pickWithCtrl(ev) || m_busy)
        return;
    if (!ensureActive())
        return;
    m_points = {ev.docPos};
    m_active = true;
    view->updateAll();
}

void PartCutTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!m_active)
        return;
    if (QLineF(m_points.last(), ev.docPos).length() * view->zoom() >= 3.0) {
        m_points << ev.docPos;
        view->updateAll();
    }
}

void PartCutTool::release(CanvasView *view, const ToolEvent &)
{
    if (!m_active)
        return;
    m_active = false;
    doc::PartSet *set = context()->set();
    const doc::Part *part = context()->activePart();
    if (!set || !part || m_points.size() < 2) {
        m_points.clear();
        view->updateAll();
        return;
    }
    const int label = part->label;
    const QString baseName = part->name;
    const QImage mask = set->partMask(label);
    const QPolygonF line = m_points;
    QPointer<doc::PartSet> guard(set);
    QPointer<PartCutTool> self(this);
    QPointer<CanvasView> viewGuard(view);
    m_busy = true;
    async::run([mask, line] { return img::splitByCutLine(mask, line); }).then(this, [self, guard, viewGuard, label, baseName](QList<QImage> pieces) {
        if (!self)
            return;
        self->m_busy = false;
        self->m_points.clear();
        if (viewGuard)
            viewGuard->updateAll();
        if (!guard)
            return;
        if (pieces.size() < 2) {
            emit self->message(tr("The line does not cut the part into separate pieces - draw it all the way across."));
            return;
        }
        QStringList names{baseName};
        for (int i = 1; i < pieces.size(); ++i)
            names << QStringLiteral("%1 %2").arg(baseName).arg(i + 1);
        const QList<int> labels = guard->splitPart(label, pieces, names, {}, tr("Cut part"));
        emit self->message(tr("Cut into %n parts.", nullptr, int(labels.size())));
    });
}

void PartCutTool::paintOverlay(CanvasView *view, QPainter &p)
{
    if (m_points.size() < 2)
        return;
    beginDocPainter(p, view);
    QPen outline(QColor(0, 0, 0, 200), 4.0);
    outline.setCosmetic(true);
    p.setPen(outline);
    p.drawPolyline(m_points);
    QPen line(QColor(255, 70, 90), 2.0, Qt::DashLine);
    line.setCosmetic(true);
    p.setPen(line);
    p.drawPolyline(m_points);
    p.restore();
}

// ---- Pick -------------------------------------------------------------------------------------------------

PartPickTool::PartPickTool(EditorState *state, PartEditContext *context, QObject *parent)
    : PartToolBase(state, context, parent)
{
}

QString PartPickTool::hint() const
{
    return tr("Click a part on the canvas to select it.");
}

void PartPickTool::press(CanvasView *, const ToolEvent &ev)
{
    context()->setActiveLabel(context()->labelAt(ev.docPos));
}

void PartPickTool::hover(CanvasView *, const ToolEvent &ev)
{
    emit hovered(context()->labelAt(ev.docPos));
}

// ---- Skeleton ---------------------------------------------------------------------------------------------

void paintSkeleton(QPainter &p, const pose::Person &person, double zoom, int highlightJoint)
{
    const double w = 3.0 / std::max(0.01, zoom);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const auto &[a, b] : kBones) {
        const pose::Keypoint &ka = person.body[size_t(a)];
        const pose::Keypoint &kb = person.body[size_t(b)];
        if (!ka.visible || !kb.visible)
            continue;
        p.setPen(QPen(QColor(0, 0, 0, 170), w * 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(ka.pos, kb.pos);
        p.setPen(QPen(jointColor(b), w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(ka.pos, kb.pos);
    }
    for (int j = 0; j < pose::kBodyCount; ++j) {
        const pose::Keypoint &k = person.body[size_t(j)];
        if (!k.visible)
            continue;
        const double r = (j == highlightJoint ? 7.0 : 5.0) / std::max(0.01, zoom);
        p.setPen(QPen(Qt::black, w * 0.6));
        p.setBrush(j == highlightJoint ? Theme::palette().accent : jointColor(j));
        p.drawEllipse(k.pos, r, r);
    }
}

SkeletonTool::SkeletonTool(EditorState *state, PartEditContext *context, QObject *parent)
    : PartToolBase(state, context, parent)
{
}

QString SkeletonTool::hint() const
{
    return tr("Drag the joints onto the character. Shift+drag moves the whole skeleton. Used by the skeleton segmenter and skeleton splits.");
}

void SkeletonTool::activated(CanvasView *view)
{
    if (context()->skeleton() || !context()->set() || !context()->set()->hasSource())
        return;
    // Start from an A-pose fitted to the silhouette.
    const QImage sil = context()->set()->silhouette().isNull() ? img::silhouetteMask(context()->set()->source())
                                                               : context()->set()->silhouette();
    QPointer<PartEditContext> ctx(context());
    QPointer<CanvasView> viewGuard(view);
    async::run([sil] { return ai::parts::fitSkeleton(sil); }).then(this, [ctx, viewGuard](pose::Person person) {
        if (ctx && !ctx->skeleton())
            ctx->setSkeleton(person);
        if (viewGuard)
            viewGuard->updateAll();
    });
}

int SkeletonTool::jointAt(CanvasView *view, const QPointF &widgetPos) const
{
    if (!context()->skeleton())
        return -1;
    int best = -1;
    double bestDist = 12.0;
    for (int j = 0; j < pose::kBodyCount; ++j) {
        const pose::Keypoint &k = context()->skeleton()->body[size_t(j)];
        if (!k.visible)
            continue;
        const double d = QLineF(view->mapFromDoc(k.pos), widgetPos).length();
        if (d < bestDist) {
            bestDist = d;
            best = j;
        }
    }
    return best;
}

void SkeletonTool::press(CanvasView *view, const ToolEvent &ev)
{
    if (!context()->skeleton()) {
        activated(view);
        return;
    }
    m_translate = ev.modifiers & Qt::ShiftModifier;
    m_joint = m_translate ? -1 : jointAt(view, ev.widgetPos);
    m_last = ev.docPos;
    view->updateAll();
}

void SkeletonTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!context()->skeleton() || (!m_translate && m_joint < 0))
        return;
    pose::Person person = *context()->skeleton();
    if (m_translate)
        person.translate(ev.docPos - m_last);
    else
        person.body[size_t(m_joint)].pos = ev.docPos;
    m_last = ev.docPos;
    context()->setSkeleton(person);
    view->updateAll();
}

void SkeletonTool::release(CanvasView *view, const ToolEvent &)
{
    m_joint = -1;
    m_translate = false;
    view->updateAll();
}

void SkeletonTool::paintOverlay(CanvasView *view, QPainter &p)
{
    if (!context()->skeleton())
        return;
    beginDocPainter(p, view);
    paintSkeleton(p, *context()->skeleton(), view->zoom(), m_joint);
    p.restore();
}

} // namespace lumen::ui
