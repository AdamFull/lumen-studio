#include "ui/filters/NodeScene.h"

#include "ai/FilterNodes.h"
#include "filters/Ops.h"
#include "filters/Registry.h"
#include "ui/Theme.h"
#include "ui/filters/GraphModel.h"
#include "ui/filters/ParamWidgets.h"

#include <QApplication>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QSet>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

using filters::PortType;

namespace {

constexpr double kHeader = 28.0;
constexpr double kPortRow = 22.0;
constexpr double kParamRow = 17.0;
constexpr double kPortRadius = 5.5;
constexpr int kMaxSummary = 5;

QColor categoryColor(const QString &category)
{
    namespace c = filters::categories;
    if (category == c::InputOutput)
        return Theme::palette().accent;
    if (category == c::Color)
        return QColor(0xc8, 0x5a, 0x8c);
    if (category == c::Detail)
        return QColor(0x4f, 0x8f, 0xc8);
    if (category == c::Combine)
        return QColor(0x52, 0xa0, 0x6e);
    if (category == c::Mask)
        return QColor(0x8a, 0x93, 0x9e);
    if (category == c::Transform)
        return QColor(0xc0, 0x8a, 0x3e);
    if (category == c::Ai)
        return QColor(0x9a, 0x6c, 0xe0);
    return Theme::palette().borderStrong;
}

QString formatNumber(double v, int decimals)
{
    return QLocale().toString(v, 'f', decimals);
}

// A setting in short, as a node shows it.
QString summaryOf(const filters::ParamSpec &spec, const QVariant &value)
{
    switch (spec.type) {
    case PortType::Number:
        return QStringLiteral("%1: %2%3").arg(spec.label, formatNumber(value.toDouble(), spec.decimals), unitSuffix(spec.unit));
    case PortType::Choice:
        for (const filters::ChoiceOption &o : spec.choices) {
            if (o.id == value.toString())
                return QStringLiteral("%1: %2").arg(spec.label, o.label);
        }
        return QStringLiteral("%1: %2").arg(spec.label, value.toString());
    case PortType::Color:
        return QStringLiteral("%1: %2").arg(spec.label, value.value<QColor>().name(QColor::HexRgb));
    case PortType::Text:
        if (spec.editor == QLatin1String("curve"))
            return NodeItem::tr("%1: %n point(s)", nullptr, int(filters::ops::parseCurve(value.toString()).size())).arg(spec.label);
        if (spec.editor == ai::filter_nodes::EngineEditor)
            return QStringLiteral("%1: %2").arg(spec.label, ai::filterNodeEngineName(value.toString()));
        return QStringLiteral("%1: %2").arg(spec.label, value.toString().simplified());
    case PortType::Image:
    case PortType::Mask:
        break;
    }
    return spec.label;
}

QFont smallFont(double points, bool bold = false)
{
    QFont f = QApplication::font();
    f.setPointSizeF(points);
    f.setBold(bold);
    return f;
}

} // namespace

// ---- PortItem ---------------------------------------------------------------------------------

PortItem::PortItem(NodeItem *node, const filters::PortSpec &spec, bool output)
    : QGraphicsItem(node)
    , m_node(node)
    , m_spec(spec)
    , m_output(output)
{
    setAcceptHoverEvents(true);
    setCursor(Qt::CrossCursor);
    setToolTip(QStringLiteral("%1 (%2)").arg(spec.label, filters::portTypeName(spec.type)));
}

QString PortItem::nodeId() const
{
    return m_node->nodeId();
}

void PortItem::setHighlight(Highlight highlight)
{
    if (m_highlight == highlight)
        return;
    m_highlight = highlight;
    update();
}

QRectF PortItem::boundingRect() const
{
    return {-10, -10, 20, 20};
}

void PortItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const Palette &pal = Theme::palette();
    painter->setRenderHint(QPainter::Antialiasing);
    if (m_highlight != Highlight::None) {
        painter->setPen(QPen(m_highlight == Highlight::Accept ? pal.success : pal.danger, 2.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(0, 0), kPortRadius + 3.5, kPortRadius + 3.5);
    }
    painter->setPen(QPen(pal.panel, 1.6));
    painter->setBrush(portColor(m_spec.type));
    painter->drawEllipse(QPointF(0, 0), kPortRadius, kPortRadius);
    if (m_spec.optional && !m_output) {
        // An optional input: a hollow centre.
        painter->setPen(Qt::NoPen);
        painter->setBrush(pal.panel);
        painter->drawEllipse(QPointF(0, 0), 2.2, 2.2);
    }
}

// ---- NodeItem ---------------------------------------------------------------------------------

NodeItem::NodeItem(const filters::Node &node)
    : m_id(node.id)
    , m_type(node.type)
{
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setZValue(1);
    setCursor(Qt::ArrowCursor);
    const filters::NodeTypeInfo &info = m_type->info();
    for (const filters::PortSpec &p : info.inputs)
        m_ports << new PortItem(this, p, false);
    for (const filters::PortSpec &p : info.outputs)
        m_ports << new PortItem(this, p, true);
    setNode(node);
    layoutPorts();
}

void NodeItem::setNode(const filters::Node &node)
{
    const filters::NodeTypeInfo &info = m_type->info();
    QStringList summary;
    for (const filters::ParamSpec &spec : info.params) {
        if (summary.size() == kMaxSummary) {
            summary << QStringLiteral("…");
            break;
        }
        summary << summaryOf(spec, node.param(spec.id));
    }
    const int rows = int(std::max(info.inputs.size(), info.outputs.size()));
    const double height = kHeader + rows * kPortRow + (summary.isEmpty() ? 6.0 : 8.0 + summary.size() * kParamRow) + 6.0;
    if (summary != m_summary || height != m_height) {
        prepareGeometryChange();
        m_summary = summary;
        m_height = height;
        layoutPorts();
        update();
    }
    QString tip = info.description;
    if (!m_problemText.isEmpty())
        tip += QStringLiteral("\n\n") + m_problemText;
    setToolTip(tip);
}

void NodeItem::layoutPorts()
{
    int in = 0, out = 0;
    for (PortItem *p : std::as_const(m_ports)) {
        const int row = p->isOutput() ? out++ : in++;
        p->setPos(p->isOutput() ? kWidth : 0.0, kHeader + row * kPortRow + kPortRow / 2.0 + 2.0);
    }
}

void NodeItem::setProblem(filters::Issue::Severity severity, const QString &message)
{
    m_problem = severity;
    m_problemText = message;
    setToolTip(m_type->info().description + QStringLiteral("\n\n") + message);
    update();
}

void NodeItem::clearProblem()
{
    if (!m_problem)
        return;
    m_problem.reset();
    m_problemText.clear();
    setToolTip(m_type->info().description);
    update();
}

PortItem *NodeItem::port(const QString &id, bool output) const
{
    for (PortItem *p : m_ports) {
        if (p->isOutput() == output && p->spec().id == id)
            return p;
    }
    return nullptr;
}

QRectF NodeItem::boundingRect() const
{
    return QRectF(0, 0, kWidth, m_height).adjusted(-4, -4, 4, 4);
}

void NodeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const Palette &pal = Theme::palette();
    const filters::NodeTypeInfo &info = m_type->info();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF body(0, 0, kWidth, m_height);
    QPainterPath shape;
    shape.addRoundedRect(body, 8, 8);
    painter->fillPath(shape, pal.surface);

    // Header in the category's colour.
    painter->save();
    painter->setClipPath(shape);
    painter->fillRect(QRectF(0, 0, kWidth, kHeader), Theme::mix(categoryColor(info.category), pal.surface, 0.55));
    painter->restore();
    painter->setPen(pal.text);
    painter->setFont(smallFont(9.5, true));
    const QRectF title(10, 0, kWidth - 20, kHeader);
    painter->drawText(title, Qt::AlignVCenter | Qt::AlignLeft,
                      painter->fontMetrics().elidedText(info.name, Qt::ElideRight, int(title.width())));

    // Port labels.
    painter->setFont(smallFont(8.5));
    const QFontMetrics fm = painter->fontMetrics();
    for (const PortItem *p : m_ports) {
        const double y = p->pos().y();
        const QRectF r = p->isOutput() ? QRectF(kWidth / 2, y - kPortRow / 2, kWidth / 2 - 12, kPortRow)
                                       : QRectF(12, y - kPortRow / 2, kWidth / 2 - 12, kPortRow);
        painter->setPen(pal.text);
        painter->drawText(r, Qt::AlignVCenter | (p->isOutput() ? Qt::AlignRight : Qt::AlignLeft),
                          fm.elidedText(p->spec().label, Qt::ElideRight, int(r.width())));
    }

    // Settings in short.
    const int rows = int(std::max(info.inputs.size(), info.outputs.size()));
    double y = kHeader + rows * kPortRow + 6.0;
    if (!m_summary.isEmpty()) {
        painter->setPen(QPen(pal.border, 1));
        painter->drawLine(QPointF(8, y), QPointF(kWidth - 8, y));
        y += 4.0;
        painter->setPen(pal.textMuted);
        for (const QString &line : m_summary) {
            painter->drawText(QRectF(10, y, kWidth - 20, kParamRow), Qt::AlignVCenter | Qt::AlignLeft,
                              fm.elidedText(line, Qt::ElideRight, int(kWidth - 20)));
            y += kParamRow;
        }
    }

    // Outline: selected, a problem, or plain.
    QPen outline(pal.borderStrong, 1.0);
    if (m_problem)
        outline = QPen(*m_problem == filters::Issue::Severity::Error ? pal.danger : pal.warning, 2.0);
    if (isSelected())
        outline = QPen(pal.accent, 2.4);
    painter->setPen(outline);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(shape);
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemPositionHasChanged) {
        if (auto *s = qobject_cast<NodeScene *>(scene()))
            s->nodeMoved(m_id);
    }
    return QGraphicsObject::itemChange(change, value);
}

// ---- LinkItem ---------------------------------------------------------------------------------

LinkItem::LinkItem(const filters::Link &link, PortItem *from, PortItem *to)
    : m_link(link)
    , m_from(from)
    , m_to(to)
{
    setFlag(ItemIsSelectable);
    setZValue(0);
    setToolTip(QObject::tr("%1 → %2").arg(from->spec().label, to->spec().label));
    updatePath();
}

QPainterPath LinkItem::curve(const QPointF &from, const QPointF &to)
{
    const double dx = std::max(40.0, std::abs(to.x() - from.x()) / 2.0);
    QPainterPath path(from);
    path.cubicTo(from + QPointF(dx, 0), to - QPointF(dx, 0), to);
    return path;
}

void LinkItem::updatePath()
{
    setPath(curve(m_from->center(), m_to->center()));
}

QPainterPath LinkItem::shape() const
{
    QPainterPathStroker stroker;
    stroker.setWidth(10);
    return stroker.createStroke(path());
}

void LinkItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    const Palette &pal = Theme::palette();
    painter->setRenderHint(QPainter::Antialiasing);
    const QColor c = portColor(m_from->spec().type);
    if (isSelected()) {
        painter->setPen(QPen(Theme::mix(pal.accent, pal.canvas, 0.4), 7, Qt::SolidLine, Qt::RoundCap));
        painter->drawPath(path());
    }
    painter->setPen(QPen(c, isSelected() ? 3.0 : 2.2, Qt::SolidLine, Qt::RoundCap));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path());
}

// ---- NodeScene --------------------------------------------------------------------------------

NodeScene::NodeScene(GraphModel *model, QObject *parent)
    : QGraphicsScene(parent)
    , m_model(model)
{
    setSceneRect(-4000, -4000, 8000, 8000);
    connect(model, &GraphModel::changed, this, &NodeScene::sync);
    sync();
}

void NodeScene::sync()
{
    if (m_dragFrom)
        endDrag(); // an undo during a drag: the drag's ports may be gone
    const filters::Graph &g = m_model->graph();
    // Links are cheap: made again, keeping which ones were selected. They go first, as they point
    // at ports of nodes that may go.
    const QList<filters::Link> selected = selectedLinks();
    qDeleteAll(m_links);
    m_links.clear();
    QSet<QString> present;
    for (const filters::Node &n : g.nodes()) {
        if (!n.type)
            continue;
        present.insert(n.id);
        NodeItem *item = m_nodes.value(n.id);
        if (item && item->nodeType() != n.type) {
            // The same id for another kind of node (another graph was loaded): new ports.
            delete item;
            item = nullptr;
        }
        if (!item) {
            item = new NodeItem(n);
            addItem(item);
            m_nodes.insert(n.id, item);
        } else {
            item->setNode(n);
        }
        if (item->pos() != n.position)
            item->setPos(n.position);
    }
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        if (present.contains(it.key())) {
            ++it;
            continue;
        }
        delete it.value();
        it = m_nodes.erase(it);
    }

    for (const filters::Link &l : g.links()) {
        NodeItem *from = m_nodes.value(l.fromNode);
        NodeItem *to = m_nodes.value(l.toNode);
        PortItem *out = from ? from->port(l.fromPort, true) : nullptr;
        PortItem *in = to ? to->port(l.toPort, false) : nullptr;
        if (!out || !in)
            continue;
        auto *item = new LinkItem(l, out, in);
        addItem(item);
        item->setSelected(selected.contains(l));
        m_links << item;
    }
    applyProblems();
}

void NodeScene::applyProblems()
{
    // Per node: the outline of its worst problem, and every problem in its tooltip. A step that
    // cannot run now (an AI step without a capable engine) is marked like a warning.
    QHash<QString, filters::Issue> worst;
    for (const filters::Issue &issue : m_model->graph().validate() + m_model->graph().unavailable()) {
        if (issue.node.isEmpty())
            continue;
        const auto it = worst.find(issue.node);
        if (it == worst.end()) {
            worst.insert(issue.node, issue);
            continue;
        }
        if (it->severity == filters::Issue::Severity::Warning && issue.severity == filters::Issue::Severity::Error)
            it->severity = issue.severity;
        it->message += QLatin1Char('\n') + issue.message;
    }
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it) {
        const auto w = worst.constFind(it.key());
        if (w == worst.constEnd())
            it.value()->clearProblem();
        else
            it.value()->setProblem(w->severity, w->message);
    }
}

QStringList NodeScene::selectedNodes() const
{
    QStringList out;
    for (QGraphicsItem *item : selectedItems()) {
        if (item->type() == NodeItem::Type)
            out << static_cast<NodeItem *>(item)->nodeId();
    }
    // In the graph's order, so a selection reads the same however it was made.
    std::sort(out.begin(), out.end(), [this](const QString &a, const QString &b) {
        const QList<filters::Node> &nodes = m_model->graph().nodes();
        auto index = [&](const QString &id) {
            return std::find_if(nodes.begin(), nodes.end(), [&](const filters::Node &n) { return n.id == id; }) - nodes.begin();
        };
        return index(a) < index(b);
    });
    return out;
}

QList<filters::Link> NodeScene::selectedLinks() const
{
    QList<filters::Link> out;
    for (const LinkItem *l : m_links) {
        if (l->isSelected())
            out << l->link();
    }
    return out;
}

void NodeScene::selectNode(const QString &id)
{
    clearSelection();
    if (NodeItem *item = m_nodes.value(id))
        item->setSelected(true);
}

void NodeScene::nodeMoved(const QString &id)
{
    for (LinkItem *l : std::as_const(m_links)) {
        if (l->link().fromNode == id || l->link().toNode == id)
            l->updatePath();
    }
}

QPointF NodeScene::freePlace(const QPointF &pos) const
{
    QPointF p = pos;
    for (int i = 0; i < 40; ++i) {
        const QRectF want(p, QSizeF(NodeItem::kWidth, 80));
        bool taken = false;
        for (const NodeItem *n : m_nodes) {
            if (n->sceneBoundingRect().intersects(want)) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return p;
        p += QPointF(24, 24);
    }
    return p;
}

PortItem *NodeScene::portAt(const QPointF &scenePos) const
{
    // A little around the point, so a port is easy to hit at any zoom.
    for (QGraphicsItem *item : items(QRectF(scenePos - QPointF(6, 6), QSizeF(12, 12)), Qt::IntersectsItemShape,
                                     Qt::DescendingOrder)) {
        if (item->type() == PortItem::Type)
            return static_cast<PortItem *>(item);
    }
    return nullptr;
}

bool NodeScene::fits(const PortItem *a, const PortItem *b) const
{
    if (!a || !b || a == b || a->isOutput() == b->isOutput() || a->nodeId() == b->nodeId())
        return false;
    const PortItem *out = a->isOutput() ? a : b;
    const PortItem *in = a->isOutput() ? b : a;
    return filters::canConnect(out->spec().type, in->spec().type);
}

void NodeScene::mousePressEvent(QGraphicsSceneMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        if (PortItem *port = portAt(e->scenePos())) {
            m_detached.reset();
            m_dragFrom = port;
            if (!port->isOutput()) {
                // A connected input: pick its link up, to drop it on another input or nowhere.
                if (const auto link = m_model->graph().linkInto(port->nodeId(), port->spec().id)) {
                    NodeItem *from = m_nodes.value(link->fromNode);
                    if (PortItem *out = from ? from->port(link->fromPort, true) : nullptr) {
                        m_detached = link;
                        m_dragFrom = out;
                        for (LinkItem *l : std::as_const(m_links)) {
                            if (l->link() == *link)
                                l->hide();
                        }
                    }
                }
            }
            m_dragLink = new QGraphicsPathItem;
            QPen pen(portColor(m_dragFrom->spec().type), 2.2, Qt::DashLine, Qt::RoundCap);
            m_dragLink->setPen(pen);
            m_dragLink->setZValue(5);
            m_dragLink->setAcceptedMouseButtons(Qt::NoButton);
            addItem(m_dragLink);
            const QPointF a = m_dragFrom->center();
            m_dragLink->setPath(m_dragFrom->isOutput() ? LinkItem::curve(a, e->scenePos()) : LinkItem::curve(e->scenePos(), a));
            e->accept();
            return;
        }
    }
    QGraphicsScene::mousePressEvent(e);
}

void NodeScene::mouseMoveEvent(QGraphicsSceneMouseEvent *e)
{
    if (!m_dragFrom) {
        QGraphicsScene::mouseMoveEvent(e);
        return;
    }
    const QPointF a = m_dragFrom->center();
    PortItem *target = portAt(e->scenePos());
    const QPointF b = target ? target->center() : e->scenePos();
    m_dragLink->setPath(m_dragFrom->isOutput() ? LinkItem::curve(a, b) : LinkItem::curve(b, a));
    if (target != m_hover) {
        if (m_hover)
            m_hover->setHighlight(PortItem::Highlight::None);
        m_hover = target;
    }
    if (m_hover && m_hover != m_dragFrom)
        m_hover->setHighlight(fits(m_dragFrom, m_hover) ? PortItem::Highlight::Accept : PortItem::Highlight::Refuse);
    e->accept();
}

void NodeScene::mouseReleaseEvent(QGraphicsSceneMouseEvent *e)
{
    if (!m_dragFrom) {
        QGraphicsScene::mouseReleaseEvent(e);
        // Nodes that were dragged: their new places, as one undo step.
        QHash<QString, QPointF> moved;
        for (const filters::Node &n : m_model->graph().nodes()) {
            if (const NodeItem *item = m_nodes.value(n.id); item && item->pos() != n.position)
                moved.insert(n.id, item->pos());
        }
        if (!moved.isEmpty()) {
            m_model->change(moved.size() == 1 ? tr("Move node") : tr("Move nodes"), [&](filters::Graph &g) {
                for (auto it = moved.cbegin(); it != moved.cend(); ++it)
                    g.setPosition(it.key(), it.value());
                return true;
            });
        }
        return;
    }
    e->accept();
    PortItem *from = m_dragFrom;
    PortItem *target = portAt(e->scenePos());
    const std::optional<filters::Link> detached = m_detached;
    if (!target || target == from) {
        endDrag();
        if (detached) {
            m_model->change(tr("Disconnect"), [&](filters::Graph &g) { return g.disconnect(detached->toNode, detached->toPort); });
            sync();
        }
        return;
    }
    const PortItem *out = from->isOutput() ? from : target;
    const PortItem *in = from->isOutput() ? target : from;
    const QPointF a = from->center(), b = target->center();
    const QString fromNode = out->nodeId(), fromPort = out->spec().id, toNode = in->nodeId(), toPort = in->spec().id;
    const bool sameSide = from->isOutput() == target->isOutput();
    endDrag();
    if (sameSide) {
        refuse(from->isOutput() ? tr("Both are outputs. Connect an output (on the right of a node) to an input (on the left).")
                                : tr("Both are inputs. Connect an output (on the right of a node) to an input (on the left)."),
               a, b);
        sync();
        return;
    }
    QString error;
    const bool ok = m_model->change(tr("Connect"), [&](filters::Graph &g) {
        if (detached)
            g.disconnect(detached->toNode, detached->toPort);
        return g.connect(fromNode, fromPort, toNode, toPort, &error);
    });
    if (!ok && !error.isEmpty())
        refuse(error, a, b);
    sync(); // shows a picked-up link again when nothing changed
}

bool NodeScene::connectPorts(const QString &fromNode, const QString &fromPort, const QString &toNode, const QString &toPort)
{
    NodeItem *a = m_nodes.value(fromNode);
    NodeItem *b = m_nodes.value(toNode);
    PortItem *out = a ? a->port(fromPort, true) : nullptr;
    PortItem *in = b ? b->port(toPort, false) : nullptr;
    const QPointF pa = out ? out->center() : QPointF(), pb = in ? in->center() : QPointF();
    if (!out && a && a->port(fromPort, false) && in) {
        refuse(tr("Both are inputs. Connect an output (on the right of a node) to an input (on the left)."), pa, pb);
        return false;
    }
    QString error;
    const bool ok = m_model->change(tr("Connect"), [&](filters::Graph &g) { return g.connect(fromNode, fromPort, toNode, toPort, &error); });
    if (!ok && !error.isEmpty())
        refuse(error, pa, pb);
    return ok;
}

void NodeScene::endDrag()
{
    if (m_hover)
        m_hover->setHighlight(PortItem::Highlight::None);
    m_hover = nullptr;
    delete m_dragLink;
    m_dragLink = nullptr;
    m_dragFrom = nullptr;
    m_detached.reset();
}

void NodeScene::refuse(const QString &message, const QPointF &from, const QPointF &to)
{
    // The refused link flashes red for a moment, the message says why.
    if (from != to) {
        auto *flash = new QGraphicsPathItem(LinkItem::curve(from, to));
        flash->setPen(QPen(Theme::palette().danger, 2.6, Qt::DashLine, Qt::RoundCap));
        flash->setZValue(5);
        flash->setAcceptedMouseButtons(Qt::NoButton);
        addItem(flash);
        QTimer::singleShot(900, this, [this, flash] {
            if (items().contains(flash))
                delete flash;
        });
    }
    emit refused(message);
}

bool NodeScene::deleteSelection()
{
    const QStringList nodes = selectedNodes();
    const QList<filters::Link> links = selectedLinks();
    if (nodes.isEmpty() && links.isEmpty())
        return false;
    const QString text = nodes.size() > 1 ? tr("Delete nodes") : nodes.size() == 1 ? tr("Delete node") : tr("Delete link");
    return m_model->change(text, [&](filters::Graph &g) {
        for (const filters::Link &l : links)
            g.disconnect(l.toNode, l.toPort);
        for (const QString &id : nodes)
            g.removeNode(id);
        return true;
    });
}

void NodeScene::contextMenuEvent(QGraphicsSceneContextMenuEvent *e)
{
    QGraphicsItem *item = itemAt(e->scenePos(), QTransform());
    if (!item || item->type() == LinkItem::Type) {
        emit addRequested(e->scenePos(), e->screenPos());
        e->accept();
        return;
    }
    QGraphicsScene::contextMenuEvent(e);
}

// ---- NodeView ---------------------------------------------------------------------------------

NodeView::NodeView(NodeScene *scene, QWidget *parent)
    : QGraphicsView(scene, parent)
    , m_scene(scene)
{
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    setDragMode(QGraphicsView::RubberBandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
    setFrameShape(QFrame::NoFrame);
    // The canvas is far larger than the graph: it is moved with the middle button or Space.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Filter graph"));
    setMinimumSize(320, 240);
}

double NodeView::zoom() const
{
    return transform().m11();
}

void NodeView::frameAll()
{
    QRectF r;
    for (QGraphicsItem *item : m_scene->items()) {
        if (item->type() == NodeItem::Type)
            r = r.united(item->sceneBoundingRect());
    }
    if (r.isEmpty()) {
        resetTransform();
        centerOn(0, 0);
        return;
    }
    r.adjust(-40, -40, 40, 40);
    fitInView(r, Qt::KeepAspectRatio);
    if (zoom() > 1.0) {
        resetTransform();
        centerOn(r.center());
    }
}

void NodeView::wheelEvent(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        QGraphicsView::wheelEvent(e);
        return;
    }
    const double factor = std::pow(1.15, steps);
    const double target = std::clamp(zoom() * factor, 0.2, 2.5);
    scale(target / zoom(), target / zoom());
    e->accept();
}

void NodeView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_space)) {
        // Pan with the hand: QGraphicsView does it for a left-button drag in ScrollHandDrag mode.
        m_panning = true;
        setDragMode(QGraphicsView::ScrollHandDrag);
        QMouseEvent press(QEvent::MouseButtonPress, e->position(), e->globalPosition(), Qt::LeftButton, Qt::LeftButton,
                          e->modifiers());
        QGraphicsView::mousePressEvent(&press);
        return;
    }
    QGraphicsView::mousePressEvent(e);
}

void NodeView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_panning && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        QMouseEvent release(QEvent::MouseButtonRelease, e->position(), e->globalPosition(), Qt::LeftButton, Qt::NoButton,
                            e->modifiers());
        QGraphicsView::mouseReleaseEvent(&release);
        setDragMode(QGraphicsView::RubberBandDrag);
        m_panning = false;
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
}

void NodeView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        m_scene->deleteSelection();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_space = true;
        viewport()->setCursor(Qt::OpenHandCursor);
        e->accept();
        return;
    }
    QGraphicsView::keyPressEvent(e);
}

void NodeView::keyReleaseEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_space = false;
        viewport()->unsetCursor();
        e->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(e);
}

void NodeView::drawBackground(QPainter *painter, const QRectF &rect)
{
    const Palette &pal = Theme::palette();
    painter->fillRect(rect, pal.canvas);
    // A dotted grid, thinned out when zoomed out so it does not turn into noise.
    const double step = zoom() < 0.5 ? 80.0 : 20.0;
    const QColor dot = Theme::mix(pal.borderStrong, pal.canvas, 0.5);
    painter->setPen(QPen(dot, 0));
    const double left = std::floor(rect.left() / step) * step;
    const double top = std::floor(rect.top() / step) * step;
    QList<QPointF> dots;
    for (double x = left; x < rect.right(); x += step) {
        for (double y = top; y < rect.bottom(); y += step)
            dots << QPointF(x, y);
    }
    painter->drawPoints(dots.constData(), int(dots.size()));
}

// ---- NodeMenu ---------------------------------------------------------------------------------

NodeMenu::NodeMenu(const filters::NodeRegistry &registry, QWidget *parent)
    : QFrame(parent, Qt::Popup)
{
    setObjectName(QStringLiteral("Panel"));
    setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search nodes"));
    m_search->setAccessibleName(tr("Search nodes"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(10);
    m_tree->setAccessibleName(tr("Nodes"));
    layout->addWidget(m_tree, 1);
    for (const QString &category : filters::categoryIds()) {
        const QList<filters::NodeTypePtr> types = registry.typesInCategory(category);
        if (types.isEmpty())
            continue;
        auto *group = new QTreeWidgetItem(m_tree, {filters::categoryName(category)});
        group->setFlags(Qt::ItemIsEnabled);
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        for (const filters::NodeTypePtr &t : types) {
            auto *item = new QTreeWidgetItem(group, {t->info().name});
            item->setData(0, Qt::UserRole, t->id());
            item->setToolTip(0, t->info().description);
            item->setData(0, Qt::AccessibleDescriptionRole, t->info().description);
        }
    }
    m_tree->expandAll();
    resize(280, 420);
    connect(m_search, &QLineEdit::textChanged, this, &NodeMenu::setFilter);
    connect(m_search, &QLineEdit::returnPressed, this, &NodeMenu::pickCurrent);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, Qt::UserRole).toString().isEmpty()) {
            m_tree->setCurrentItem(item);
            pickCurrent();
        }
    });
    m_search->installEventFilter(this);
    m_tree->installEventFilter(this);
}

void NodeMenu::popup(const QPoint &globalPos)
{
    m_search->clear();
    setFilter({});
    move(globalPos);
    show();
    raise();
    m_search->setFocus();
}

void NodeMenu::setFilter(const QString &text)
{
    if (m_search->text() != text) {
        QSignalBlocker block(m_search);
        m_search->setText(text);
    }
    const QString needle = text.trimmed();
    QTreeWidgetItem *first = nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        int shown = 0;
        for (int j = 0; j < group->childCount(); ++j) {
            QTreeWidgetItem *item = group->child(j);
            const bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive)
                            || item->toolTip(0).contains(needle, Qt::CaseInsensitive)
                            || group->text(0).contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match) {
                ++shown;
                if (!first)
                    first = item;
            }
        }
        group->setHidden(shown == 0);
    }
    m_tree->setCurrentItem(first);
}

QStringList NodeMenu::visibleTypes() const
{
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *group = m_tree->topLevelItem(i);
        if (group->isHidden())
            continue;
        for (int j = 0; j < group->childCount(); ++j) {
            if (!group->child(j)->isHidden())
                out << group->child(j)->data(0, Qt::UserRole).toString();
        }
    }
    return out;
}

void NodeMenu::pickCurrent()
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    const QString id = item && !item->isHidden() ? item->data(0, Qt::UserRole).toString() : QString();
    if (id.isEmpty())
        return;
    hide();
    emit picked(id);
}

bool NodeMenu::eventFilter(QObject *watched, QEvent *event)
{
    // Up and down in the search field move through the list, so typing and Enter is enough.
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            pickCurrent();
            return true;
        }
    }
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(event);
        if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
            QTreeWidgetItem *current = m_tree->currentItem();
            QTreeWidgetItem *next = current;
            do {
                next = next ? (k->key() == Qt::Key_Down ? m_tree->itemBelow(next) : m_tree->itemAbove(next)) : nullptr;
            } while (next && (next->isHidden() || next->data(0, Qt::UserRole).toString().isEmpty()));
            if (next)
                m_tree->setCurrentItem(next);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace lumen::ui
