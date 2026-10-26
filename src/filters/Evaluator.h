#pragma once

#include "filters/Graph.h"
#include "filters/NodeType.h"

#include <QFuture>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace lumen::filters {

// Results of expensive nodes (AI steps) that several evaluators share, so a step that one of them
// ran (the filter editor's Run AI steps) is not run again by another (the Filters panel's Apply)
// while its key is the same: the node's type, its parameter values and the keys of what feeds it,
// the source picture and the selection included. Keeps the newest results within a memory budget
// (the newest one always: they are full-resolution pictures). Thread-safe.
class SharedResults
{
public:
    explicit SharedResults(qint64 budgetBytes = qint64(512) << 20);

    std::optional<Values> find(const QByteArray &key) const;
    void store(const QByteArray &key, const Values &outputs);
    int size() const;
    void clear();

private:
    struct Item
    {
        QByteArray key;
        Values outputs;
        qint64 bytes = 0;
    };
    mutable QMutex m_mutex;
    qint64 m_budget;
    QList<Item> m_items; // oldest first
};

// What a filter is applied to.
struct EvalInputs
{
    QImage image;     // the active layer, or the picture the filter is shown on (any format)
    QImage selection; // a mask, stretched to the image when its size differs; null: everything
    // The whole picture (every visible layer merged), for an Input node that reads it; null: the
    // image is the whole picture. Of the image's size. Callers give it only to a graph that reads it
    // (Graph::readsWholePicture), since its cacheKey is part of the Input node's cache key.
    QImage picture;
};

struct EvalOptions
{
    // Preview scale in (0, 1]: the source is scaled down first and pixel lengths with it, so a
    // preview of a large image is quick while it looks like the result. 1 when the filter is applied.
    double scale = 1.0;
    // The node whose outputs to return (the node editor's preview of the selected node); empty
    // for the output node. Only what it depends on runs.
    QString target;
    // false while previewing: an expensive node (an AI step) that is not in the cache is not run;
    // the evaluation then stops with needsRun set before it runs anything. Its cached result is
    // still used, so a slider after an AI step previews live once the step has run. An expensive
    // node always runs at full resolution, on full-resolution inputs, and a preview at any scale
    // takes its result scaled down: a Run while previewing gives the result that applying uses.
    bool runExpensive = true;
    // Called on a worker thread before each node that runs, with the number of nodes finished so
    // far, the number that will run and the node's name, and once more when the evaluation ends
    // well (done == total, no name). Nodes taken from the cache are not counted. For progress bars.
    std::function<void(int done, int total, const QString &node)> progress;
    // Called while a node that takes long reports how far it is (an AI step's queue job, see
    // EvalContext::progressReporter), from any thread: the fraction of that node done in [0, 1]
    // (negative when not known) and its status in plain words. Between two `progress` calls.
    std::function<void(double fraction, const QString &status)> stepProgress;
};

struct EvalResult
{
    QImage image;          // the output node's image, at the evaluation's scale
    QImage mask;           // the output node's mask (where the result applies), at the image's size;
                           // null when unconnected
    Values outputs;        // the target node's outputs
    // The nodes that ran, in order; the others came from the cache or were not needed. In a
    // preview, the nodes that feed an expensive node may be listed twice: at full size and scaled.
    QStringList evaluated;
    bool needsRun = false; // stopped before an expensive node (EvalOptions::runExpensive false)
    double scale = 1.0;
};

// Evaluates graphs on the worker pool (core/Async.h), one node after the other, and keeps each
// node's last outputs, one result at full resolution and one at the preview scale. A cached result
// is used again while the node's key is the same: its type, its parameter values, the scale and
// the keys of the nodes feeding it (the input node's key also covers the source image and the
// selection, by QImage::cacheKey), so changing one parameter re-runs only that node and what
// depends on it, and nodes that only feed cached results do not run.
//
// A node may return its outputs later as a future (NodeResult): the evaluation then gives up its
// worker thread and continues on the pool when the future finishes.
//
// Use from one thread (the GUI thread); evaluations run in the background and may overlap.
class Evaluator
{
public:
    Evaluator();
    ~Evaluator(); // cancels what still runs
    Evaluator(const Evaluator &) = delete;
    Evaluator &operator=(const Evaluator &) = delete;

    // Starts an evaluation. The future gives the result, fails with lumen::Error (a validation
    // error or a node that failed, in plain words) or is cancelled. Cancelling the future stops
    // the evaluation at the next node; cancel() also reaches a node that is running or waiting.
    QFuture<EvalResult> evaluate(const Graph &graph, const EvalInputs &inputs, const EvalOptions &options = {});
    // Cancels every evaluation of this evaluator that still runs. A node that waits for a future
    // has the future and its onCancel handlers cancelled, and the evaluation's future finishes as
    // cancelled at once. A node that runs has its onCancel handlers run and stops at its next
    // isCancelled() check (blur, unsharp mask and median check while they work; the other
    // classic nodes finish their pass first); the future finishes when it returns.
    void cancel();

    void clearCache();
    int cachedResults() const; // full-size and preview results together

    // Expensive nodes look in `results` too and leave theirs there (null: this evaluator's cache
    // only). Evaluations that started before keep what they had.
    void setSharedResults(std::shared_ptr<SharedResults> results) { m_shared = std::move(results); }

private:
    struct Cache;
    class Run;
    std::shared_ptr<Cache> m_cache;
    std::shared_ptr<SharedResults> m_shared;
    QMutex m_mutex;
    QList<std::weak_ptr<Run>> m_runs;
};

// Runs one node on the given inputs in the calling thread, waiting for an asynchronous node.
// Throws lumen::Error when the node fails or a required input is missing. For tests and tools;
// the editor uses Evaluator.
Values evaluateNode(const NodeType &type, const Values &inputs, const QVariantMap &params = {}, double scale = 1.0);

} // namespace lumen::filters
