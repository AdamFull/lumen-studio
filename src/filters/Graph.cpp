#include "filters/Graph.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>

namespace lumen::filters {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::Graph)
};

const QString FormatName = QStringLiteral("lumen-filter");

QJsonValue paramToJson(const ParamSpec *spec, const QVariant &value)
{
    if (!spec)
        return QJsonValue::fromVariant(value);
    switch (spec->type) {
    case PortType::Number:
        return value.toDouble();
    case PortType::Color: {
        const QColor c = value.typeId() == QMetaType::QColor ? value.value<QColor>() : QColor::fromString(value.toString());
        return c.isValid() ? c.name(QColor::HexArgb) : value.toString();
    }
    case PortType::Text:
    case PortType::Choice:
        return value.toString();
    case PortType::Image:
    case PortType::Mask:
        break;
    }
    return QJsonValue::fromVariant(value);
}

QVariant paramFromJson(const ParamSpec *spec, const QJsonValue &value)
{
    if (!spec)
        return value.toVariant();
    switch (spec->type) {
    case PortType::Number:
        return value.isDouble() ? QVariant(value.toDouble()) : value.toVariant();
    case PortType::Color: {
        const QColor c = QColor::fromString(value.toString());
        return c.isValid() ? QVariant(c) : value.toVariant();
    }
    case PortType::Text:
    case PortType::Choice:
        return value.toString();
    case PortType::Image:
    case PortType::Mask:
        break;
    }
    return value.toVariant();
}

QString typeMismatch(const QString &fromLabel, PortType from, const QString &inputLabel, const QString &toLabel,
                     PortType to)
{
    QString message = Text::tr("“%1” gives %2, but the “%3” input of “%4” takes %5.")
                          .arg(fromLabel, portTypeName(from), inputLabel, toLabel, portTypeName(to));
    if (from == PortType::Image && to == PortType::Mask)
        message += QLatin1Char(' ') + Text::tr("Put a “Mask from image” node between them.");
    return message;
}

} // namespace

QVariant Node::param(const QString &id) const
{
    const ParamSpec *spec = type ? type->info().param(id) : nullptr;
    return spec ? coerceParam(*spec, params.value(id)) : params.value(id);
}

Graph Graph::createDefault(const NodeRegistry &registry)
{
    Graph g;
    const QString in = g.addNode(QStringLiteral("input"), QPointF(0, 0), registry);
    const QString out = g.addNode(QStringLiteral("output"), QPointF(360, 0), registry);
    g.connect(in, QStringLiteral("image"), out, QStringLiteral("image"));
    return g;
}

const Node *Graph::node(const QString &id) const
{
    for (const Node &n : m_nodes) {
        if (n.id == id)
            return &n;
    }
    return nullptr;
}

Node *Graph::node(const QString &id)
{
    for (Node &n : m_nodes) {
        if (n.id == id)
            return &n;
    }
    return nullptr;
}

QString Graph::uniqueId() const
{
    int highest = 0;
    for (const Node &n : m_nodes) {
        if (n.id.startsWith(QLatin1Char('n'))) {
            bool ok = false;
            const int k = n.id.mid(1).toInt(&ok);
            if (ok)
                highest = std::max(highest, k);
        }
    }
    return QStringLiteral("n%1").arg(highest + 1);
}

QString Graph::addNode(NodeTypePtr type, const QPointF &position, const QString &id)
{
    if (!type)
        return {};
    Node n;
    n.id = (!id.isEmpty() && !node(id)) ? id : uniqueId();
    n.type = std::move(type);
    n.position = position;
    m_nodes.append(n);
    return n.id;
}

QString Graph::addNode(const QString &typeId, const QPointF &position, const NodeRegistry &registry)
{
    return addNode(registry.find(typeId), position);
}

bool Graph::removeNode(const QString &id)
{
    const auto removed = m_nodes.removeIf([&](const Node &n) { return n.id == id; });
    if (removed == 0)
        return false;
    m_links.removeIf([&](const Link &l) { return l.fromNode == id || l.toNode == id; });
    m_published.removeIf([&](const PublishedParam &p) { return p.node == id; });
    return true;
}

bool Graph::setParam(const QString &nodeId, const QString &paramId, const QVariant &value)
{
    Node *n = node(nodeId);
    if (!n || !n->type)
        return false;
    const ParamSpec *spec = n->type->info().param(paramId);
    if (!spec)
        return false;
    n->params.insert(paramId, coerceParam(*spec, value));
    return true;
}

QVariant Graph::param(const QString &nodeId, const QString &paramId) const
{
    const Node *n = node(nodeId);
    return n ? n->param(paramId) : QVariant();
}

void Graph::setPosition(const QString &nodeId, const QPointF &position)
{
    if (Node *n = node(nodeId))
        n->position = position;
}

QString Graph::nodeLabel(const QString &id) const
{
    const Node *n = node(id);
    return n && n->type ? n->type->info().name : id;
}

bool Graph::reaches(const QString &from, const QString &to) const
{
    QSet<QString> seen;
    QStringList queue{from};
    while (!queue.isEmpty()) {
        const QString current = queue.takeLast();
        if (current == to)
            return true;
        if (seen.contains(current))
            continue;
        seen.insert(current);
        for (const Link &l : m_links) {
            if (l.fromNode == current)
                queue.append(l.toNode);
        }
    }
    return false;
}

bool Graph::connect(const QString &fromNode, const QString &fromPort, const QString &toNode, const QString &toPort,
                    QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const Node *from = node(fromNode);
    const Node *to = node(toNode);
    const PortSpec *out = from && from->type ? from->type->info().output(fromPort) : nullptr;
    const PortSpec *in = to && to->type ? to->type->info().input(toPort) : nullptr;
    if (!out || !in)
        return fail(Text::tr("That connection point does not exist."));
    if (!canConnect(out->type, in->type))
        return fail(typeMismatch(nodeLabel(fromNode), out->type, in->label, nodeLabel(toNode), in->type));
    if (reaches(toNode, fromNode))
        return fail(Text::tr("This link would make a loop: the result of “%1” would come back into its own input.")
                        .arg(nodeLabel(fromNode)));
    disconnect(toNode, toPort);
    m_links.append({fromNode, fromPort, toNode, toPort});
    return true;
}

bool Graph::disconnect(const QString &toNode, const QString &toPort)
{
    return m_links.removeIf([&](const Link &l) { return l.toNode == toNode && l.toPort == toPort; }) > 0;
}

std::optional<Link> Graph::linkInto(const QString &toNode, const QString &toPort) const
{
    for (const Link &l : m_links) {
        if (l.toNode == toNode && l.toPort == toPort)
            return l;
    }
    return std::nullopt;
}

QList<Link> Graph::linksFrom(const QString &fromNode) const
{
    QList<Link> out;
    for (const Link &l : m_links) {
        if (l.fromNode == fromNode)
            out.append(l);
    }
    return out;
}

void Graph::publish(const PublishedParam &param)
{
    unpublish(param.node, param.param);
    m_published.append(param);
}

void Graph::unpublish(const QString &nodeId, const QString &paramId)
{
    m_published.removeIf([&](const PublishedParam &p) { return p.node == nodeId && p.param == paramId; });
}

QString Graph::outputNode() const
{
    for (const Node &n : m_nodes) {
        if (n.type && n.type->info().isOutput)
            return n.id;
    }
    return {};
}

bool Graph::needsExplicitRun() const
{
    for (const QString &id : evaluationOrder(outputNode())) {
        const Node *n = node(id);
        if (n && n->type && n->type->info().expensive)
            return true;
    }
    return false;
}

bool Graph::readsWholePicture() const
{
    for (const QString &id : evaluationOrder(outputNode())) {
        const Node *n = node(id);
        if (n && n->type && n->type->info().id == QLatin1String("input")
            && n->param(QStringLiteral("source")).toString() == QLatin1String("picture"))
            return true;
    }
    return false;
}

QStringList Graph::evaluationOrder(const QString &target) const
{
    if (!node(target))
        return {};
    // Depth-first over input links, in port order; a node is listed after everything it reads.
    QStringList order;
    QHash<QString, int> state; // 1 = being visited, 2 = done
    bool loop = false;
    std::function<void(const QString &)> visit = [&](const QString &id) {
        const int s = state.value(id);
        if (s == 2 || loop)
            return;
        if (s == 1) {
            loop = true;
            return;
        }
        state.insert(id, 1);
        const Node *n = node(id);
        if (n && n->type) {
            for (const PortSpec &in : n->type->info().inputs) {
                if (const auto link = linkInto(id, in.id); link && node(link->fromNode))
                    visit(link->fromNode);
            }
        }
        state.insert(id, 2);
        order.append(id);
    };
    visit(target);
    return loop ? QStringList() : order;
}

QList<Issue> Graph::validate() const
{
    QList<Issue> issues;
    auto error = [&](const QString &nodeId, const QString &message) {
        issues.append({Issue::Severity::Error, nodeId, message});
    };

    int outputs = 0;
    for (const Node &n : m_nodes) {
        if (n.type && n.type->info().isOutput)
            ++outputs;
    }
    if (outputs == 0)
        error({}, Text::tr("The filter has no output. Add an “Output” node and connect the result to it."));
    else if (outputs > 1)
        error({}, Text::tr("The filter has more than one output node; keep one."));

    QSet<QString> linkedInputs;
    for (const Link &l : m_links) {
        const Node *from = node(l.fromNode);
        const Node *to = node(l.toNode);
        const PortSpec *out = from && from->type ? from->type->info().output(l.fromPort) : nullptr;
        const PortSpec *in = to && to->type ? to->type->info().input(l.toPort) : nullptr;
        if (!out || !in) {
            error(l.toNode, Text::tr("A link points to a node or a connection point that is not in the filter."));
            continue;
        }
        const QString key = l.toNode + QLatin1Char('/') + l.toPort;
        if (linkedInputs.contains(key))
            error(l.toNode, Text::tr("“%1” has more than one link into its “%2” input.").arg(nodeLabel(l.toNode), in->label));
        linkedInputs.insert(key);
        if (!canConnect(out->type, in->type))
            error(l.toNode, typeMismatch(nodeLabel(l.fromNode), out->type, in->label, nodeLabel(l.toNode), in->type));
    }

    bool hasLoop = false;
    for (const Node &n : m_nodes) {
        for (const Link &l : linksFrom(n.id)) {
            if (reaches(l.toNode, n.id)) {
                error(n.id, Text::tr("“%1” is part of a loop: its result comes back into its own input. "
                                     "Remove one of the links of the loop.")
                                .arg(nodeLabel(n.id)));
                hasLoop = true;
                break;
            }
        }
    }

    const QString output = outputNode();
    if (!output.isEmpty() && !hasLoop) {
        const QStringList upstream = evaluationOrder(output);
        const QSet<QString> used(upstream.begin(), upstream.end());
        for (const Node &n : m_nodes) {
            if (!n.type)
                continue;
            if (!used.contains(n.id)) {
                if (!n.type->info().isOutput)
                    issues.append({Issue::Severity::Warning, n.id,
                                   Text::tr("“%1” is not connected to the output, so it has no effect.").arg(nodeLabel(n.id))});
                continue;
            }
            for (const PortSpec &in : n.type->info().inputs) {
                if (!in.optional && !linkInto(n.id, in.id))
                    error(n.id, Text::tr("“%1” has nothing connected to its “%2” input.").arg(nodeLabel(n.id), in.label));
            }
        }
    }

    for (const PublishedParam &p : m_published) {
        const Node *n = node(p.node);
        const ParamSpec *spec = n && n->type ? n->type->info().param(p.param) : nullptr;
        if (!spec) {
            error(p.node, Text::tr("The slider “%1” belongs to a setting that is not in the filter.").arg(p.label));
            continue;
        }
        if (spec->type != PortType::Number || (p.minimum == 0.0 && p.maximum == 0.0))
            continue;
        if (p.minimum >= p.maximum)
            error(p.node, Text::tr("The slider “%1” has no range: its lowest value is not below its highest.").arg(p.label));
        else if (p.minimum < spec->minimum || p.maximum > spec->maximum)
            error(p.node, Text::tr("The slider “%1” reaches values that its setting does not take; keep it within %2 to %3.")
                              .arg(p.label, QString::number(spec->minimum), QString::number(spec->maximum)));
    }
    return issues;
}

QList<Issue> Graph::unavailable() const
{
    QList<Issue> issues;
    for (const Node &n : m_nodes) {
        if (!n.type)
            continue;
        const QString reason = n.type->unavailableReason(n.params);
        if (!reason.isEmpty())
            issues.append({Issue::Severity::Warning, n.id, Text::tr("“%1”: %2").arg(nodeLabel(n.id), reason)});
    }
    return issues;
}

bool Graph::isValid() const
{
    for (const Issue &i : validate()) {
        if (i.severity == Issue::Severity::Error)
            return false;
    }
    return true;
}

QJsonObject Graph::toJson() const
{
    QJsonArray nodes;
    for (const Node &n : m_nodes) {
        QJsonObject o{{QStringLiteral("id"), n.id},
                      {QStringLiteral("type"), n.typeId()},
                      {QStringLiteral("x"), n.position.x()},
                      {QStringLiteral("y"), n.position.y()}};
        if (!n.params.isEmpty()) {
            QJsonObject params;
            for (auto it = n.params.cbegin(); it != n.params.cend(); ++it)
                params.insert(it.key(), paramToJson(n.type ? n.type->info().param(it.key()) : nullptr, it.value()));
            o.insert(QStringLiteral("params"), params);
        }
        nodes.append(o);
    }
    QJsonArray links;
    for (const Link &l : m_links) {
        links.append(QJsonObject{{QStringLiteral("from"), l.fromNode},
                                 {QStringLiteral("output"), l.fromPort},
                                 {QStringLiteral("to"), l.toNode},
                                 {QStringLiteral("input"), l.toPort}});
    }
    QJsonArray published;
    for (const PublishedParam &p : m_published) {
        QJsonObject o{{QStringLiteral("node"), p.node}, {QStringLiteral("param"), p.param}, {QStringLiteral("label"), p.label}};
        if (p.minimum != 0.0 || p.maximum != 0.0) {
            o.insert(QStringLiteral("min"), p.minimum);
            o.insert(QStringLiteral("max"), p.maximum);
        }
        published.append(o);
    }
    QJsonObject json{{QStringLiteral("format"), FormatName},
                     {QStringLiteral("version"), FormatVersion},
                     {QStringLiteral("nodes"), nodes},
                     {QStringLiteral("links"), links}};
    if (!name.isEmpty())
        json.insert(QStringLiteral("name"), name);
    if (!description.isEmpty())
        json.insert(QStringLiteral("description"), description);
    if (!published.isEmpty())
        json.insert(QStringLiteral("published"), published);
    return json;
}

std::optional<Graph> Graph::fromJson(const QJsonObject &json, const NodeRegistry &registry, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Graph> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const int version = json.value(QStringLiteral("version")).toInt(0);
    if (json.value(QStringLiteral("format")).toString() != FormatName || version < 1)
        return fail(Text::tr("This is not a Lumen filter file."));
    if (version > FormatVersion)
        return fail(Text::tr("This filter was made with a newer version of Lumen Studio (file version %1; this version "
                             "reads up to %2). Update Lumen Studio to use it.")
                        .arg(version)
                        .arg(FormatVersion));

    Graph g;
    g.name = json.value(QStringLiteral("name")).toString();
    g.description = json.value(QStringLiteral("description")).toString();
    for (const QJsonValue &v : json.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject o = v.toObject();
        Node n;
        n.id = o.value(QStringLiteral("id")).toString();
        if (n.id.isEmpty() || g.node(n.id))
            return fail(Text::tr("The filter file is damaged: two nodes have the same name, or one has none."));
        const QString typeId = o.value(QStringLiteral("type")).toString();
        n.type = registry.find(typeId);
        if (!n.type)
            return fail(Text::tr("This filter uses a node that this version of Lumen Studio does not have (“%1”). It may "
                                 "come from a plugin that is turned off.")
                            .arg(typeId));
        n.position = QPointF(o.value(QStringLiteral("x")).toDouble(), o.value(QStringLiteral("y")).toDouble());
        const QJsonObject params = o.value(QStringLiteral("params")).toObject();
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            n.params.insert(it.key(), paramFromJson(n.type->info().param(it.key()), it.value()));
        g.m_nodes.append(n);
    }
    for (const QJsonValue &v : json.value(QStringLiteral("links")).toArray()) {
        const QJsonObject o = v.toObject();
        const Link l{o.value(QStringLiteral("from")).toString(), o.value(QStringLiteral("output")).toString(),
                     o.value(QStringLiteral("to")).toString(), o.value(QStringLiteral("input")).toString()};
        const Node *from = g.node(l.fromNode);
        const Node *to = g.node(l.toNode);
        if (!from || !to)
            return fail(Text::tr("The filter file is damaged: a link points to a node that is not in it."));
        if (!from->type->info().output(l.fromPort))
            return fail(Text::tr("The filter does not fit this version of Lumen Studio: “%1” has no “%2” output.")
                            .arg(from->type->info().name, l.fromPort));
        if (!to->type->info().input(l.toPort))
            return fail(Text::tr("The filter does not fit this version of Lumen Studio: “%1” has no “%2” input.")
                            .arg(to->type->info().name, l.toPort));
        if (g.linkInto(l.toNode, l.toPort))
            return fail(Text::tr("The filter file is damaged: an input of “%1” has two links.").arg(to->type->info().name));
        g.m_links.append(l);
    }
    for (const QJsonValue &v : json.value(QStringLiteral("published")).toArray()) {
        const QJsonObject o = v.toObject();
        g.m_published.append({o.value(QStringLiteral("node")).toString(), o.value(QStringLiteral("param")).toString(),
                              o.value(QStringLiteral("label")).toString(), o.value(QStringLiteral("min")).toDouble(),
                              o.value(QStringLiteral("max")).toDouble()});
    }
    return g;
}

bool Graph::save(const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = Text::tr("Could not save the filter: %1").arg(file.errorString());
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = Text::tr("Could not save the filter: %1").arg(file.errorString());
        return false;
    }
    return true;
}

std::optional<Graph> Graph::load(const QString &path, const NodeRegistry &registry, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = Text::tr("Could not open the filter: %1").arg(file.errorString());
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isObject()) {
        if (error)
            *error = Text::tr("This is not a Lumen filter file.");
        return std::nullopt;
    }
    return fromJson(doc.object(), registry, error);
}

} // namespace lumen::filters
