#pragma once

#include "filters/Graph.h"

#include <QObject>
#include <QUndoStack>

#include <functional>

namespace lumen::ui {

// The filter graph a node editor window edits, with its undo history (M17 §17.6). Every change
// goes through change(): it records the graph before and after as one undo step, so undo and redo
// never have to know what an edit did. Graphs are small value types; copies share the node types.
class GraphModel : public QObject
{
    Q_OBJECT
public:
    explicit GraphModel(QObject *parent = nullptr);

    const filters::Graph &graph() const { return m_graph; }
    QUndoStack *undoStack() { return &m_undo; }

    // Starts over with `graph`: no history, nothing to save.
    void reset(const filters::Graph &graph);
    // Edits a copy of the graph and records it as one step named `text`. Consecutive changes with
    // the same non-empty `mergeKey` (a slider being dragged, a name being typed) become one step.
    // Returns false, recording nothing, when `edit` returns false or changes nothing.
    bool change(const QString &text, const std::function<bool(filters::Graph &)> &edit, const QString &mergeKey = {});

    bool isModified() const { return !m_undo.isClean(); }
    void markSaved() { m_undo.setClean(); }

    // Used by the undo commands.
    void setGraph(const filters::Graph &graph);

signals:
    // The graph changed (an edit, undo or redo, reset).
    void changed();
    void modifiedChanged(bool modified);

private:
    filters::Graph m_graph;
    QUndoStack m_undo;
};

// Whether two graphs are the same filter (nodes, values, positions, links, published settings).
bool sameGraph(const filters::Graph &a, const filters::Graph &b);

} // namespace lumen::ui
