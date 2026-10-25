#pragma once

#include "filters/NodeType.h"

#include <QList>
#include <QMutex>
#include <QString>

namespace lumen::filters {

// The node types a graph can use, by id. Other libraries add theirs at start (the AI nodes live
// in the ai layer and register themselves into global()); graphs keep a pointer to each node's
// type, so evaluation never looks types up. Thread-safe.
class NodeRegistry
{
public:
    NodeRegistry() = default;
    NodeRegistry(const NodeRegistry &other);
    NodeRegistry &operator=(const NodeRegistry &other);

    // Adds a type, replacing one with the same id.
    void add(NodeTypePtr type);
    NodeTypePtr find(const QString &id) const;
    bool contains(const QString &id) const { return find(id) != nullptr; }
    // In the order they were added (the classic nodes in menu order).
    QList<NodeTypePtr> types() const;
    QList<NodeTypePtr> typesInCategory(const QString &category) const;

    // The registry of the app: the classic nodes, added on first use, plus what other libraries
    // add. Built lazily so node names are translated by the translator installed at start.
    static NodeRegistry &global();

private:
    mutable QMutex m_mutex;
    QList<NodeTypePtr> m_types;
};

// Category ids of the built-in nodes, in menu order.
namespace categories {
inline const QString InputOutput = QStringLiteral("io");
inline const QString Color = QStringLiteral("color");
inline const QString Detail = QStringLiteral("detail");
inline const QString Combine = QStringLiteral("combine");
inline const QString Mask = QStringLiteral("mask");
inline const QString Transform = QStringLiteral("transform");
inline const QString Ai = QStringLiteral("ai");
} // namespace categories

QStringList categoryIds();
// Label of a category ("Colour"); an unknown id is shown as it is.
QString categoryName(const QString &id);

} // namespace lumen::filters
