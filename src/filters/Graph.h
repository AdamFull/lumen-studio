#pragma once

#include "filters/NodeType.h"
#include "filters/Registry.h"

#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <QVariantMap>

#include <optional>

namespace lumen::filters {

// One node of a graph: an instance of a node type with its parameter values.
struct Node
{
    QString id; // unique in its graph ("n1", "n2"...)
    NodeTypePtr type;
    // Values set by the user, by parameter id; a missing one takes the type's default. Values of
    // parameters the type does not have (from a newer file) are kept and saved again.
    QVariantMap params;
    QPointF position; // top-left corner in the node editor

    QString typeId() const { return type ? type->id() : QString(); }
    // The value the node runs with (stored or default, coerced to the spec; not scaled).
    QVariant param(const QString &id) const;
};

// An output port of one node feeding an input port of another. An input takes at most one link;
// an output may feed many.
struct Link
{
    QString fromNode;
    QString fromPort;
    QString toNode;
    QString toPort;

    bool operator==(const Link &o) const = default;
};

// A node parameter shown as a slider when the filter is used (the Filters panel), with its own
// label and range.
struct PublishedParam
{
    QString node;
    QString param;
    QString label;
    // Slider range, within the parameter's own (validate() reports one that is not); both 0
    // takes the parameter's range.
    double minimum = 0.0;
    double maximum = 0.0;

    bool operator==(const PublishedParam &o) const = default;
};

// A problem found by Graph::validate(), in plain words.
struct Issue
{
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    QString node; // the node it is about (for the editor to highlight); empty for the whole graph
    QString message;
};

// A filter: nodes, links, published parameters. A value type; copies share the node types.
class Graph
{
public:
    static constexpr int FormatVersion = 1;
    static QString fileExtension() { return QStringLiteral(".lumenfilter"); }

    // An input node linked to an output node: the graph a new filter starts from.
    static Graph createDefault(const NodeRegistry &registry = NodeRegistry::global());

    QString name;
    QString description;

    const QList<Node> &nodes() const { return m_nodes; }
    const QList<Link> &links() const { return m_links; }
    const QList<PublishedParam> &published() const { return m_published; }

    const Node *node(const QString &id) const;
    Node *node(const QString &id);
    // Adds a node and returns its id (`id` when given and free, else a new one); empty when the
    // type is null or unknown.
    QString addNode(NodeTypePtr type, const QPointF &position = {}, const QString &id = {});
    QString addNode(const QString &typeId, const QPointF &position = {},
                    const NodeRegistry &registry = NodeRegistry::global());
    // Removes the node, its links and its published parameters.
    bool removeNode(const QString &id);
    bool setParam(const QString &node, const QString &param, const QVariant &value);
    QVariant param(const QString &node, const QString &param) const;
    void setPosition(const QString &node, const QPointF &position);

    // Links an output to an input, replacing the input's link. Refuses ports that do not exist,
    // types that do not fit and links that would make a loop, with a message in plain words.
    bool connect(const QString &fromNode, const QString &fromPort, const QString &toNode, const QString &toPort,
                 QString *error = nullptr);
    bool disconnect(const QString &toNode, const QString &toPort);
    std::optional<Link> linkInto(const QString &toNode, const QString &toPort) const;
    QList<Link> linksFrom(const QString &fromNode) const;

    void setPublished(const QList<PublishedParam> &published) { m_published = published; }
    void publish(const PublishedParam &param);
    void unpublish(const QString &node, const QString &param);

    // The id of the output node (the first one when there are several); empty without one.
    QString outputNode() const;
    // Whether a node needs an explicit Run (an expensive node feeds the output).
    bool needsExplicitRun() const;

    // Errors (no output or several, loops, links between ports that do not fit, unconnected
    // required inputs of nodes that feed the output, published parameters that do not exist or
    // whose slider range is empty or beyond the parameter's) and warnings (nodes that do not feed
    // the output). Messages in plain words.
    QList<Issue> validate() const;
    bool isValid() const;

    // The nodes `target` depends on, inputs before the nodes they feed, ending with `target`.
    // Empty when the target does not exist or is part of a loop.
    QStringList evaluationOrder(const QString &target) const;

    // ---- Files (.lumenfilter, JSON) ----------------------------------------------------------
    QJsonObject toJson() const;
    // Fails, with a message in plain words, on a file of a newer format version, a node type the
    // registry does not have, or links and ids that do not fit together. Problems that the
    // editor can fix (types, loops, missing links) load and are reported by validate().
    static std::optional<Graph> fromJson(const QJsonObject &json, const NodeRegistry &registry = NodeRegistry::global(),
                                         QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;
    static std::optional<Graph> load(const QString &path, const NodeRegistry &registry = NodeRegistry::global(),
                                     QString *error = nullptr);

private:
    QString uniqueId() const;
    bool reaches(const QString &from, const QString &to) const; // follows links downstream
    QString nodeLabel(const QString &id) const;

    QList<Node> m_nodes;
    QList<Link> m_links;
    QList<PublishedParam> m_published;
};

} // namespace lumen::filters
