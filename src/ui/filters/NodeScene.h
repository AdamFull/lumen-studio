#pragma once

#include "filters/Graph.h"

#include <QFrame>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHash>
#include <QPointer>

#include <optional>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace lumen::ui {

class GraphModel;
class NodeItem;

// A connection point of a node: inputs on the left edge, outputs on the right, coloured by the
// type of what flows through it (portColor()).
class PortItem : public QGraphicsItem
{
public:
    enum { Type = UserType + 0x4c21 };
    enum class Highlight { None, Accept, Refuse };

    PortItem(NodeItem *node, const filters::PortSpec &spec, bool output);

    int type() const override { return Type; }
    NodeItem *nodeItem() const { return m_node; }
    QString nodeId() const;
    const filters::PortSpec &spec() const { return m_spec; }
    bool isOutput() const { return m_output; }
    QPointF center() const { return scenePos(); }
    void setHighlight(Highlight highlight);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    NodeItem *m_node;
    filters::PortSpec m_spec;
    bool m_output;
    Highlight m_highlight = Highlight::None;
};

// A node of the graph: its name, its ports with their labels and its settings in short.
class NodeItem : public QGraphicsObject
{
    Q_OBJECT
public:
    enum { Type = UserType + 0x4c20 };

    explicit NodeItem(const filters::Node &node);

    int type() const override { return Type; }
    QString nodeId() const { return m_id; }
    const filters::NodeTypePtr &nodeType() const { return m_type; }
    // Takes the node's current values (the settings shown in short).
    void setNode(const filters::Node &node);
    // A problem validate() found, shown as the outline's colour and in the tooltip.
    void setProblem(filters::Issue::Severity severity, const QString &message);
    void clearProblem();
    PortItem *port(const QString &id, bool output) const;
    const QList<PortItem *> &ports() const { return m_ports; }
    // The settings as the node shows them ("Radius: 4 px").
    QStringList summary() const { return m_summary; }

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

    static constexpr double kWidth = 196.0;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;

private:
    void layoutPorts();

    QString m_id;
    filters::NodeTypePtr m_type;
    QList<PortItem *> m_ports;
    QStringList m_summary;
    double m_height = 60.0;
    std::optional<filters::Issue::Severity> m_problem;
    QString m_problemText;
};

// A link from an output to an input, drawn as a curve in the colour of its type.
class LinkItem : public QGraphicsPathItem
{
public:
    enum { Type = UserType + 0x4c22 };

    LinkItem(const filters::Link &link, PortItem *from, PortItem *to);

    int type() const override { return Type; }
    const filters::Link &link() const { return m_link; }
    void updatePath();
    // The curve between two points, leaving `from` to the right and entering `to` from the left.
    static QPainterPath curve(const QPointF &from, const QPointF &to);

    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    filters::Link m_link;
    PortItem *m_from;
    PortItem *m_to;
};

// The graph as items, kept in step with a GraphModel (sync()). Dragging from a port to another
// connects them when their types fit; otherwise the link flashes red and refused() tells why.
// Dragging from a connected input picks its link up (dropped on nothing, it is removed). Moving
// nodes, connecting and deleting are undo steps of the model.
class NodeScene : public QGraphicsScene
{
    Q_OBJECT
public:
    explicit NodeScene(GraphModel *model, QObject *parent = nullptr);

    // Makes the items match the model's graph (after every change, undo and redo).
    void sync();
    NodeItem *nodeItem(const QString &id) const { return m_nodes.value(id); }
    QList<LinkItem *> linkItems() const { return m_links; }
    QStringList selectedNodes() const;
    QList<filters::Link> selectedLinks() const;
    void selectNode(const QString &id); // empty: nothing

    // Connects an output to an input as a drop does: refused (with refused()) when the types do
    // not fit, it would make a loop, or both are inputs or outputs.
    bool connectPorts(const QString &fromNode, const QString &fromPort, const QString &toNode, const QString &toPort);
    // Removes the selected nodes and links as one undo step.
    bool deleteSelection();
    // A free place near `pos` for a new node (not on top of another one).
    QPointF freePlace(const QPointF &pos) const;

signals:
    void refused(const QString &message);
    // A right-click on an empty place: add a node there.
    void addRequested(const QPointF &scenePos, const QPoint &screenPos);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *e) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *e) override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent *e) override;

private:
    friend class NodeItem;
    void nodeMoved(const QString &id);
    PortItem *portAt(const QPointF &scenePos) const;
    bool fits(const PortItem *a, const PortItem *b) const;
    void endDrag();
    void refuse(const QString &message, const QPointF &from, const QPointF &to);
    void applyProblems();

    GraphModel *m_model;
    QHash<QString, NodeItem *> m_nodes;
    QList<LinkItem *> m_links;
    // A link being dragged: from this port (fixed end) to the mouse.
    PortItem *m_dragFrom = nullptr;
    std::optional<filters::Link> m_detached; // the link picked up from an input, if any
    QGraphicsPathItem *m_dragLink = nullptr;
    PortItem *m_hover = nullptr;
};

// The view of a NodeScene: a dotted grid, the wheel zooms around the mouse, the middle button
// (or Space and the left button) pans, a drag on empty space selects, Delete removes.
class NodeView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit NodeView(NodeScene *scene, QWidget *parent = nullptr);

    // Shows every node, at most at 100 %.
    void frameAll();
    double zoom() const;

protected:
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void drawBackground(QPainter *painter, const QRectF &rect) override;

private:
    NodeScene *m_scene;
    bool m_panning = false;
    bool m_space = false;
};

// The node types to add, searchable by name and description, grouped by category in plain
// names. Opens as a popup (the editor's "Add node" button, a right-click on the canvas).
class NodeMenu : public QFrame
{
    Q_OBJECT
public:
    explicit NodeMenu(const filters::NodeRegistry &registry, QWidget *parent = nullptr);

    void popup(const QPoint &globalPos);
    void setFilter(const QString &text);
    // The type ids the list shows now, in order.
    QStringList visibleTypes() const;
    QLineEdit *searchField() const { return m_search; }

signals:
    void picked(const QString &typeId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void pickCurrent();

    QLineEdit *m_search;
    QTreeWidget *m_tree;
};

} // namespace lumen::ui
