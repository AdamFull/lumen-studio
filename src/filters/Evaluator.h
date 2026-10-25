#pragma once

#include "filters/Graph.h"
#include "filters/NodeType.h"

#include <QFuture>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QStringList>

#include <memory>

namespace lumen::filters {

// What a filter is applied to.
struct EvalInputs
{
    QImage image;     // the layer or the whole picture (any format)
    QImage selection; // a mask, stretched to the image when its size differs; null: everything
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
    // the evaluation then stops with needsRun set. Its cached result is still used, so a slider
    // after an AI step previews live once the step has run.
    bool runExpensive = true;
};

struct EvalResult
{
    QImage image;          // the output node's image, at the evaluation's scale
    QImage mask;           // the output node's mask (where the result applies); null when unconnected
    Values outputs;        // the target node's outputs
    QStringList evaluated; // the nodes that ran, in order; the others came from the cache
    bool needsRun = false; // stopped before an expensive node (EvalOptions::runExpensive false)
    double scale = 1.0;
};

// Evaluates graphs on the worker pool (core/Async.h), one node after the other, and keeps each
// node's last outputs. A cached result is used again while the node's key is the same: its type,
// its parameter values, the preview scale and the keys of the nodes feeding it (the input node's
// key also covers the source image and the selection, by QImage::cacheKey), so changing one
// parameter re-runs only that node and what depends on it.
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
    // the evaluation at the next node; cancel() also interrupts a node that is running or waiting.
    QFuture<EvalResult> evaluate(const Graph &graph, const EvalInputs &inputs, const EvalOptions &options = {});
    // Cancels every evaluation of this evaluator that still runs: the node that runs stops at its
    // next check, a node that waits for a future has the future and its onCancel handlers
    // cancelled, and the evaluation's future finishes as cancelled.
    void cancel();

    void clearCache();
    int cachedNodes() const;

private:
    struct Cache;
    class Run;
    std::shared_ptr<Cache> m_cache;
    QMutex m_mutex;
    QList<std::weak_ptr<Run>> m_runs;
};

// Runs one node on the given inputs in the calling thread, waiting for an asynchronous node.
// Throws lumen::Error when the node fails or a required input is missing. For tests and tools;
// the editor uses Evaluator.
Values evaluateNode(const NodeType &type, const Values &inputs, const QVariantMap &params = {}, double scale = 1.0);

} // namespace lumen::filters
