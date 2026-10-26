#include "ui/filters/GraphModel.h"

#include <QPointer>
#include <QUndoCommand>

namespace lumen::ui {

namespace {

constexpr int kMergeId = 0x4c46; // any id; commands merge only when their keys match too

class GraphCommand : public QUndoCommand
{
public:
    GraphCommand(GraphModel *model, filters::Graph before, filters::Graph after, const QString &text, QString mergeKey)
        : QUndoCommand(text)
        , m_model(model)
        , m_before(std::move(before))
        , m_after(std::move(after))
        , m_mergeKey(std::move(mergeKey))
    {
    }

    void redo() override
    {
        if (m_model)
            m_model->setGraph(m_after);
    }

    void undo() override
    {
        if (m_model)
            m_model->setGraph(m_before);
    }

    int id() const override { return m_mergeKey.isEmpty() ? -1 : kMergeId; }

    bool mergeWith(const QUndoCommand *other) override
    {
        const auto *o = static_cast<const GraphCommand *>(other);
        if (o->m_mergeKey != m_mergeKey)
            return false;
        m_after = o->m_after;
        // A slider dragged back to where it was leaves no step.
        setObsolete(sameGraph(m_after, m_before));
        return true;
    }

private:
    QPointer<GraphModel> m_model;
    filters::Graph m_before;
    filters::Graph m_after;
    QString m_mergeKey;
};

} // namespace

bool sameGraph(const filters::Graph &a, const filters::Graph &b)
{
    if (a.name != b.name || a.description != b.description || a.links() != b.links() || a.published() != b.published()
        || a.nodes().size() != b.nodes().size())
        return false;
    for (qsizetype i = 0; i < a.nodes().size(); ++i) {
        const filters::Node &x = a.nodes().at(i);
        const filters::Node &y = b.nodes().at(i);
        if (x.id != y.id || x.type != y.type || x.position != y.position || x.params != y.params)
            return false;
    }
    return true;
}

GraphModel::GraphModel(QObject *parent)
    : QObject(parent)
{
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) { emit modifiedChanged(!clean); });
}

void GraphModel::reset(const filters::Graph &graph)
{
    m_undo.clear();
    m_graph = graph;
    m_undo.setClean();
    emit changed();
}

bool GraphModel::change(const QString &text, const std::function<bool(filters::Graph &)> &edit, const QString &mergeKey)
{
    filters::Graph after = m_graph;
    if (!edit(after) || sameGraph(after, m_graph))
        return false;
    m_undo.push(new GraphCommand(this, m_graph, after, text, mergeKey));
    return true;
}

void GraphModel::setGraph(const filters::Graph &graph)
{
    m_graph = graph;
    emit changed();
}

} // namespace lumen::ui
