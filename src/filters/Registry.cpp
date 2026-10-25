#include "filters/Registry.h"

#include "filters/ClassicNodes.h"

#include <QCoreApplication>

namespace lumen::filters {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::NodeRegistry)
};

} // namespace

NodeRegistry::NodeRegistry(const NodeRegistry &other)
{
    QMutexLocker lock(&other.m_mutex);
    m_types = other.m_types;
}

NodeRegistry &NodeRegistry::operator=(const NodeRegistry &other)
{
    if (this != &other) {
        const QList<NodeTypePtr> types = other.types();
        QMutexLocker lock(&m_mutex);
        m_types = types;
    }
    return *this;
}

void NodeRegistry::add(NodeTypePtr type)
{
    if (!type)
        return;
    QMutexLocker lock(&m_mutex);
    for (NodeTypePtr &t : m_types) {
        if (t->id() == type->id()) {
            t = std::move(type);
            return;
        }
    }
    m_types.append(std::move(type));
}

NodeTypePtr NodeRegistry::find(const QString &id) const
{
    QMutexLocker lock(&m_mutex);
    for (const NodeTypePtr &t : m_types) {
        if (t->id() == id)
            return t;
    }
    return nullptr;
}

QList<NodeTypePtr> NodeRegistry::types() const
{
    QMutexLocker lock(&m_mutex);
    return m_types;
}

QList<NodeTypePtr> NodeRegistry::typesInCategory(const QString &category) const
{
    QList<NodeTypePtr> out;
    for (const NodeTypePtr &t : types()) {
        if (t->info().category == category)
            out.append(t);
    }
    return out;
}

NodeRegistry &NodeRegistry::global()
{
    static NodeRegistry *registry = [] {
        auto *r = new NodeRegistry;
        registerClassicNodes(*r);
        return r;
    }();
    return *registry;
}

QStringList categoryIds()
{
    return {categories::InputOutput, categories::Color, categories::Detail, categories::Combine,
            categories::Mask, categories::Transform, categories::Ai};
}

QString categoryName(const QString &id)
{
    if (id == categories::InputOutput)
        return Text::tr("Input and output");
    if (id == categories::Color)
        return Text::tr("Colour");
    if (id == categories::Detail)
        return Text::tr("Blur, sharpen and noise");
    if (id == categories::Combine)
        return Text::tr("Combine");
    if (id == categories::Mask)
        return Text::tr("Masks");
    if (id == categories::Transform)
        return Text::tr("Size and orientation");
    if (id == categories::Ai)
        return Text::tr("AI");
    return id;
}

} // namespace lumen::filters
