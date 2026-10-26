#include "filters/Evaluator.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QPromise>
#include <QSet>

#include <array>
#include <atomic>
#include <cmath>
#include <optional>

namespace lumen::filters {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::filters::Evaluator)
};

QByteArray canonical(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::LongLong:
        return QByteArray::number(v.toDouble(), 'g', 17);
    case QMetaType::QColor:
        return v.value<QColor>().name(QColor::HexArgb).toLatin1();
    default:
        return v.toString().toUtf8();
    }
}

// Adds one field of a key, its length first, so that two different lists of fields never hash
// the same bytes (the texts "a;b=" and "" stay apart from "a" and ";b=").
void addField(QCryptographicHash &hash, QByteArrayView bytes)
{
    hash.addData(QByteArray::number(bytes.size()) + ':');
    hash.addData(bytes);
}

QSize scaledSize(const QSize &size, double scale)
{
    if (scale >= 1.0)
        return size;
    return {std::max(1, static_cast<int>(std::lround(size.width() * scale))),
            std::max(1, static_cast<int>(std::lround(size.height() * scale)))};
}

QImage toMask(const QImage &image)
{
    return image.isNull() || image.format() == QImage::Format_Grayscale8 ? image
                                                                          : image.convertToFormat(QImage::Format_Grayscale8);
}

bool isPicture(PortType type)
{
    return type == PortType::Image || type == PortType::Mask;
}

// A value arriving at an input: a mask feeding an image input becomes a grey image.
QVariant convertedInput(PortType from, PortType to, const QVariant &value)
{
    if (from == PortType::Mask && to == PortType::Image)
        return img::normalized(value.value<QImage>());
    return value;
}

// A value input with the id of a parameter drives it, and is resolved like the parameter.
QVariant drivenValue(const NodeTypeInfo &info, const PortSpec &port, const QVariant &value, double scale)
{
    const ParamSpec *spec = isPicture(port.type) ? nullptr : info.param(port.id);
    return spec ? resolveParam(*spec, value, scale) : value;
}

// The outputs of a node, every declared one present and in its port's canonical format. The
// message does not name the node: the callers add the name.
Values checkedOutputs(const NodeTypeInfo &info, Values outputs)
{
    for (const PortSpec &p : info.outputs) {
        auto it = outputs.find(p.id);
        if (it == outputs.end() || !it->isValid() || (isPicture(p.type) && it->value<QImage>().isNull()))
            throw Error(Text::tr("It gave no result."));
        if (p.type == PortType::Image)
            *it = img::normalized(it->value<QImage>());
        else if (p.type == PortType::Mask)
            *it = toMask(it->value<QImage>());
    }
    return outputs;
}

// An expensive node's outputs in a preview: its full-resolution images and masks scaled down.
Values scaledDown(const NodeTypeInfo &info, Values outputs, double scale)
{
    for (const PortSpec &p : info.outputs) {
        auto it = outputs.find(p.id);
        if (it == outputs.end() || !isPicture(p.type))
            continue;
        const QImage image = it->value<QImage>();
        *it = img::scaled(image, scaledSize(image.size(), scale), true);
    }
    return checkedOutputs(info, outputs);
}

QString nodeFailed(const QString &name, const QString &message)
{
    return Text::tr("“%1”: %2").arg(name, message);
}

QString missingInput(const NodeTypeInfo &info, const PortSpec &port)
{
    return Text::tr("“%1” has nothing connected to its “%2” input.").arg(info.name, port.label);
}

// Where a step computes a node's result: at full resolution or at the preview scale. A preview
// runs its nodes in the Preview lane, except an expensive node and the nodes that feed it: they
// run in the Full lane, so that a Run while previewing gives the result that applying uses.
enum Lane { Full = 0, Preview = 1 };

struct Step
{
    QString node;
    Lane lane = Full;
    // An expensive node in the Preview lane: its Full result scaled down; the node does not run.
    bool scaledCopy = false;
    QByteArray key;
    QVariantMap params; // resolved for the lane's scale
    std::optional<Values> cached;
    bool needed = false; // false: it only feeds results that are cached
};

// The steps of an evaluation, every step after the steps it reads, ending with the target.
QList<Step> planSteps(const Graph &graph, const QStringList &order, double scale)
{
    QList<Step> steps;
    if (scale >= 1.0) {
        for (const QString &id : order)
            steps.append({id, Full});
        return steps;
    }
    QSet<QString> full; // the expensive nodes and what feeds them
    for (const QString &id : order) {
        if (graph.node(id)->type->info().expensive) {
            const QStringList upstream = graph.evaluationOrder(id);
            full.unite(QSet<QString>(upstream.begin(), upstream.end()));
        }
    }
    QSet<QString> preview; // what the target needs at the preview scale, up to the expensive nodes
    QStringList pending{order.last()};
    while (!pending.isEmpty()) {
        const QString id = pending.takeLast();
        if (preview.contains(id))
            continue;
        preview.insert(id);
        const NodeTypeInfo &info = graph.node(id)->type->info();
        if (info.expensive)
            continue;
        for (const PortSpec &port : info.inputs) {
            if (const auto link = graph.linkInto(id, port.id))
                pending.append(link->fromNode);
        }
    }
    for (const QString &id : order) {
        if (full.contains(id))
            steps.append({id, Full});
    }
    for (const QString &id : order) {
        if (preview.contains(id))
            steps.append({id, Preview, graph.node(id)->type->info().expensive});
    }
    return steps;
}

} // namespace

struct Evaluator::Cache
{
    struct Entry
    {
        QByteArray key;
        Values outputs;
    };

    std::optional<Values> find(const QString &node, Lane lane, const QByteArray &key) const
    {
        QMutexLocker lock(&mutex);
        const auto it = lanes[lane].constFind(node);
        if (it == lanes[lane].constEnd() || it->key != key)
            return std::nullopt;
        return it->outputs;
    }

    void store(const QString &node, Lane lane, const QByteArray &key, const Values &outputs)
    {
        QMutexLocker lock(&mutex);
        lanes[lane].insert(node, {key, outputs});
    }

    // Drops the results of nodes that are no longer in the graph.
    void retain(const QSet<QString> &nodes)
    {
        QMutexLocker lock(&mutex);
        for (QHash<QString, Entry> &entries : lanes) {
            for (auto it = entries.begin(); it != entries.end();) {
                if (nodes.contains(it.key()))
                    ++it;
                else
                    it = entries.erase(it);
            }
        }
    }

    int size() const
    {
        QMutexLocker lock(&mutex);
        return static_cast<int>(lanes[Full].size() + lanes[Preview].size());
    }

    mutable QMutex mutex;
    // Node id -> its last result at full resolution [Full] and at the preview scale [Preview], so
    // that going from previewing to applying and back computes nothing again.
    std::array<QHash<QString, Entry>, 2> lanes;
};

// One evaluation. Runs on the worker pool; between nodes that return futures it is held only by
// the futures' continuations. The evaluation state (steps, index, outputs) is touched by one
// thread at a time, since every hand-over goes through a future; cancel() comes from the caller's
// thread and shares only the members under m_mutex.
class Evaluator::Run final : public EvalContext::Run, public std::enable_shared_from_this<Evaluator::Run>
{
public:
    Run(Graph graph, const QStringList &order, EvalOptions options, EvalInputs inputs, std::shared_ptr<Cache> cache,
        std::shared_ptr<SharedResults> shared)
        : m_graph(std::move(graph))
        , m_options(std::move(options))
        , m_inputs(std::move(inputs))
        , m_cache(std::move(cache))
        , m_shared(std::move(shared))
        , m_steps(planSteps(m_graph, order, m_options.scale))
    {
    }

    QFuture<EvalResult> start()
    {
        m_promise.start();
        return m_promise.future();
    }

    void begin()
    {
        if (isCancelled())
            return finishCancelled();
        if (!prepare())
            return;
        // A preview that would have to run an expensive node stops before it runs anything.
        if (m_result.needsRun)
            return finishOk();
        for (const Step &s : std::as_const(m_steps)) {
            if (s.needed && !s.cached && !s.scaledCopy)
                ++m_total;
        }
        step();
    }

    void step()
    {
        while (m_index < m_steps.size()) {
            if (isCancelled())
                return finishCancelled();
            const Step &s = m_steps.at(m_index);
            if (!s.needed) {
                ++m_index;
                continue;
            }
            if (s.cached) {
                m_outputs[s.lane].insert(s.node, *s.cached);
                ++m_index;
                continue;
            }
            const Node *node = m_graph.node(s.node);
            const NodeTypeInfo &info = node->type->info();
            try {
                if (s.scaledCopy) {
                    accept(scaledDown(info, m_outputs[Full].value(s.node), m_options.scale));
                    continue;
                }
                m_result.evaluated.append(s.node);
                if (m_options.progress)
                    m_options.progress(m_ran, m_total, info.name);
                EvalContext ctx(this, s.node, laneScale(s.lane));
                const NodeResult result = node->type->evaluate(NodeArgs(inputsOf(s, info), s.params), ctx);
                if (!result.isReady())
                    return wait(result.future());
                // A node that saw the cancellation returned early; its outputs are not kept.
                if (isCancelled())
                    return finishCancelled();
                accept(checkedOutputs(info, result.outputs()));
            } catch (const std::exception &e) {
                return fail(nodeFailed(info.name, describeException(e)));
            } catch (...) {
                return fail(nodeFailed(info.name, describeException(std::current_exception())));
            }
        }
        finishOk();
    }

    void cancel()
    {
        QList<std::function<void()>> handlers;
        bool waiting = false;
        QFuture<Values> pending;
        {
            QMutexLocker lock(&m_mutex);
            if (m_cancelled || m_done)
                return;
            m_cancelled = true;
            m_cancelledFlag.store(true);
            handlers.swap(m_cancelHandlers);
            waiting = m_waiting;
            pending = m_pending;
        }
        for (const auto &handler : handlers)
            handler();
        if (waiting) {
            // Nothing runs on a worker now: finish at once rather than when the future ends.
            pending.cancel();
            finishCancelled();
        }
    }

    // ---- EvalContext::Run ------------------------------------------------------------------
    bool isCancelled() const override { return m_cancelledFlag.load() || m_promise.isCanceled(); }

    void onCancel(std::function<void()> handler) override
    {
        {
            QMutexLocker lock(&m_mutex);
            if (!m_cancelled) {
                m_cancelHandlers.append(std::move(handler));
                return;
            }
        }
        handler();
    }

    QImage source(double scale) override
    {
        QImage &source = m_source[laneOf(scale)];
        if (source.isNull() && !m_inputs.image.isNull()) {
            const QImage image = img::normalized(m_inputs.image);
            source = img::scaled(image, scaledSize(image.size(), scale), true);
        }
        return source;
    }

    QImage selection(double scale) override
    {
        QImage &selection = m_selection[laneOf(scale)];
        if (selection.isNull()) {
            const QSize size = source(scale).size();
            selection = m_inputs.selection.isNull() ? img::createMask(size, 255)
                                                    : img::scaled(toMask(m_inputs.selection), size, true);
        }
        return selection;
    }

    QSize sourceSize() const override { return m_inputs.image.size(); }

    std::function<void(double, const QString &)> progressReporter() override
    {
        if (!m_options.stepProgress)
            return {};
        // Held by an asynchronous node's job after evaluate() returned: it must not keep the run.
        std::weak_ptr<Run> weak = weak_from_this();
        return [weak](double fraction, const QString &status) {
            const std::shared_ptr<Run> run = weak.lock();
            if (!run)
                return;
            {
                QMutexLocker lock(&run->m_mutex);
                if (run->m_done || run->m_cancelled)
                    return;
            }
            run->m_options.stepProgress(fraction, status);
        };
    }

private:
    static Lane laneOf(double scale) { return scale < 1.0 ? Preview : Full; }
    double laneScale(Lane lane) const { return lane == Full ? 1.0 : m_options.scale; }

    QByteArray sourceKey() const
    {
        return QByteArray::number(m_inputs.image.cacheKey()) + 'x' + QByteArray::number(m_inputs.image.width()) + 'x'
             + QByteArray::number(m_inputs.image.height()) + '/' + QByteArray::number(m_inputs.selection.cacheKey());
    }

    // Computes every step's key and takes what the cache holds for it, before anything runs: a
    // key depends only on the graph, the parameters and the source (the type, the parameter
    // values, the lane's scale and the keys of the steps feeding it). Then, from the target back,
    // a step is needed when a needed step without a cached result reads it, so nothing that only
    // feeds cached results runs, and a preview knows at once whether it would run an expensive node.
    bool prepare()
    {
        std::array<QHash<QString, qsizetype>, 2> index; // node -> its step, per lane
        // planSteps() puts every node that a step reads into the step's lane, before the step.
        auto feeding = [&](const Step &s, const Link &link) -> Step & {
            return m_steps[index[s.lane].value(link.fromNode)];
        };
        for (qsizetype i = 0; i < m_steps.size(); ++i) {
            Step &s = m_steps[i];
            index[s.lane].insert(s.node, i);
            const Node *node = m_graph.node(s.node);
            const NodeTypeInfo &info = node->type->info();
            const double scale = laneScale(s.lane);
            QCryptographicHash hash(QCryptographicHash::Sha1);
            if (s.scaledCopy) {
                addField(hash, "scaled copy");
                addField(hash, m_steps.at(index[Full].value(s.node)).key);
            } else {
                s.params = resolveParams(info, node->params, scale);
                addField(hash, info.id.toUtf8());
                for (const ParamSpec &spec : info.params) {
                    addField(hash, spec.id.toUtf8());
                    addField(hash, canonical(s.params.value(spec.id)));
                }
                for (const PortSpec &port : info.inputs) {
                    addField(hash, port.id.toUtf8());
                    const auto link = m_graph.linkInto(s.node, port.id);
                    if (!link) {
                        if (!port.optional) {
                            fail(missingInput(info, port));
                            return false;
                        }
                        addField(hash, {});
                        continue;
                    }
                    addField(hash, feeding(s, *link).key);
                    addField(hash, link->fromPort.toUtf8());
                }
                if (info.readsSource)
                    addField(hash, sourceKey());
            }
            addField(hash, QByteArray::number(scale, 'g', 17));
            s.key = hash.result();
            s.cached = m_cache->find(s.node, s.lane, s.key);
            // An AI step another evaluator ran on the same inputs (the filter editor's Run).
            if (!s.cached && !s.scaledCopy && m_shared && info.expensive) {
                s.cached = m_shared->find(s.key);
                if (s.cached)
                    m_cache->store(s.node, s.lane, s.key, *s.cached);
            }
        }

        m_steps.last().needed = true;
        for (qsizetype i = m_steps.size() - 1; i >= 0; --i) {
            const Step &s = m_steps.at(i);
            if (!s.needed || s.cached)
                continue;
            if (s.scaledCopy) {
                m_steps[index[Full].value(s.node)].needed = true;
                continue;
            }
            const NodeTypeInfo &info = m_graph.node(s.node)->type->info();
            if (info.expensive && !m_options.runExpensive)
                m_result.needsRun = true;
            for (const PortSpec &port : info.inputs) {
                if (const auto link = m_graph.linkInto(s.node, port.id))
                    feeding(s, *link).needed = true;
            }
        }
        return true;
    }

    Values inputsOf(const Step &s, const NodeTypeInfo &info) const
    {
        Values inputs;
        for (const PortSpec &port : info.inputs) {
            const auto link = m_graph.linkInto(s.node, port.id);
            if (!link)
                continue;
            const PortSpec *out = m_graph.node(link->fromNode)->type->info().output(link->fromPort);
            const QVariant value = m_outputs[s.lane].value(link->fromNode).value(link->fromPort);
            inputs.insert(port.id, drivenValue(info, port, convertedInput(out->type, port.type, value), laneScale(s.lane)));
        }
        return inputs;
    }

    void accept(const Values &outputs)
    {
        const Step &s = m_steps.at(m_index);
        if (!s.scaledCopy)
            ++m_ran;
        m_cache->store(s.node, s.lane, s.key, outputs);
        if (m_shared && !s.scaledCopy && m_graph.node(s.node)->type->info().expensive)
            m_shared->store(s.key, outputs);
        m_outputs[s.lane].insert(s.node, outputs);
        ++m_index;
        QMutexLocker lock(&m_mutex);
        m_cancelHandlers.clear(); // they belonged to the node that finished
    }

    void wait(QFuture<Values> future)
    {
        bool cancelled = false;
        {
            QMutexLocker lock(&m_mutex);
            cancelled = m_cancelled;
            if (!cancelled) {
                m_waiting = true;
                m_pending = future;
            }
        }
        if (cancelled) {
            future.cancel();
            return finishCancelled();
        }
        // A continuation that takes the future runs on success and on failure; a cancelled future
        // skips it and cancels the continuation's future instead, which onCanceled() catches.
        auto self = shared_from_this();
        future.then(async::pool(), [self](QFuture<Values> f) { self->resume(f); })
            .onCanceled([self] { self->finishCancelled(); });
    }

    void resume(QFuture<Values> future)
    {
        {
            QMutexLocker lock(&m_mutex);
            m_waiting = false;
            m_pending = QFuture<Values>();
        }
        if (isCancelled())
            return finishCancelled();
        const NodeTypeInfo &info = m_graph.node(m_steps.at(m_index).node)->type->info();
        try {
            if (future.resultCount() == 0) {
                future.waitForFinished(); // rethrows the node's exception, if it has one
                throw Error(Text::tr("It gave no result."));
            }
            accept(checkedOutputs(info, future.result()));
        } catch (const std::exception &e) {
            return fail(nodeFailed(info.name, describeException(e)));
        } catch (...) {
            return fail(nodeFailed(info.name, describeException(std::current_exception())));
        }
        step();
    }

    bool markDone()
    {
        QMutexLocker lock(&m_mutex);
        if (m_done)
            return false;
        m_done = true;
        m_cancelHandlers.clear();
        m_pending = QFuture<Values>(); // its continuation holds this run
        return true;
    }

    void finishOk()
    {
        if (!markDone())
            return;
        const QHash<QString, Values> &outputs = m_outputs[laneOf(m_options.scale)];
        m_result.scale = m_options.scale;
        m_result.outputs = outputs.value(m_options.target);
        const Values output = outputs.value(m_graph.outputNode());
        m_result.image = output.value(QStringLiteral("image")).value<QImage>();
        m_result.mask = output.value(QStringLiteral("mask")).value<QImage>();
        if (m_options.progress && !m_result.needsRun)
            m_options.progress(m_total, m_total, QString());
        m_promise.addResult(m_result);
        m_promise.finish();
    }

    void fail(const QString &message)
    {
        if (!markDone())
            return;
        m_promise.setException(Error(message));
        m_promise.finish();
    }

    void finishCancelled()
    {
        if (!markDone())
            return;
        m_promise.future().cancel();
        m_promise.finish();
    }

    const Graph m_graph;
    const EvalOptions m_options;
    const EvalInputs m_inputs;
    const std::shared_ptr<Cache> m_cache;
    const std::shared_ptr<SharedResults> m_shared;
    QPromise<EvalResult> m_promise;

    QList<Step> m_steps;
    qsizetype m_index = 0;
    int m_total = 0; // the steps that run (for EvalOptions::progress)
    int m_ran = 0;
    std::array<QHash<QString, Values>, 2> m_outputs; // per lane: node id -> outputs
    EvalResult m_result;
    std::array<QImage, 2> m_source; // per lane
    std::array<QImage, 2> m_selection;

    mutable QMutex m_mutex;
    std::atomic_bool m_cancelledFlag = false; // m_cancelled, readable without the lock
    bool m_cancelled = false;
    bool m_done = false;
    bool m_waiting = false;
    QFuture<Values> m_pending;
    QList<std::function<void()>> m_cancelHandlers;
};

SharedResults::SharedResults(qint64 budgetBytes)
    : m_budget(budgetBytes)
{
}

std::optional<Values> SharedResults::find(const QByteArray &key) const
{
    QMutexLocker lock(&m_mutex);
    for (const Item &item : m_items) {
        if (item.key == key)
            return item.outputs;
    }
    return std::nullopt;
}

void SharedResults::store(const QByteArray &key, const Values &outputs)
{
    qint64 bytes = 0;
    for (const QVariant &v : outputs) {
        if (v.typeId() == QMetaType::QImage)
            bytes += v.value<QImage>().sizeInBytes();
    }
    QMutexLocker lock(&m_mutex);
    m_items.removeIf([&](const Item &item) { return item.key == key; });
    m_items.append({key, outputs, bytes});
    qint64 total = 0;
    for (const Item &item : std::as_const(m_items))
        total += item.bytes;
    while (m_items.size() > 1 && total > m_budget) {
        total -= m_items.first().bytes;
        m_items.removeFirst();
    }
}

int SharedResults::size() const
{
    QMutexLocker lock(&m_mutex);
    return int(m_items.size());
}

void SharedResults::clear()
{
    QMutexLocker lock(&m_mutex);
    m_items.clear();
}

Evaluator::Evaluator()
    : m_cache(std::make_shared<Cache>())
{
}

Evaluator::~Evaluator()
{
    cancel();
}

QFuture<EvalResult> Evaluator::evaluate(const Graph &graph, const EvalInputs &inputs, const EvalOptions &options)
{
    const QString target = options.target.isEmpty() ? graph.outputNode() : options.target;
    const QStringList order = graph.evaluationOrder(target);
    for (const Issue &issue : graph.validate()) {
        // Previewing one node only needs the nodes it depends on to be right.
        const bool blocks = options.target.isEmpty() || order.contains(issue.node);
        if (issue.severity == Issue::Severity::Error && blocks)
            return QtFuture::makeExceptionalFuture<EvalResult>(Error(issue.message));
    }
    if (order.isEmpty())
        return QtFuture::makeExceptionalFuture<EvalResult>(Error(Text::tr("That node cannot be shown: it is not in the filter or is part of a loop.")));
    if (inputs.image.isNull())
        return QtFuture::makeExceptionalFuture<EvalResult>(Error(Text::tr("There is no image to apply the filter to.")));

    EvalOptions opts = options;
    opts.target = target;
    if (!(opts.scale > 0.0 && opts.scale < 1.0))
        opts.scale = 1.0;
    QSet<QString> ids;
    for (const Node &n : graph.nodes())
        ids.insert(n.id);
    m_cache->retain(ids);

    auto run = std::make_shared<Run>(graph, order, opts, inputs, m_cache, m_shared);
    QFuture<EvalResult> future = run->start();
    {
        QMutexLocker lock(&m_mutex);
        m_runs.removeIf([](const std::weak_ptr<Run> &r) { return r.expired(); });
        m_runs.append(run);
    }
    async::run([run] { run->begin(); });
    return future;
}

void Evaluator::cancel()
{
    QList<std::weak_ptr<Run>> runs;
    {
        QMutexLocker lock(&m_mutex);
        runs.swap(m_runs);
    }
    for (const std::weak_ptr<Run> &weak : runs) {
        if (const std::shared_ptr<Run> run = weak.lock())
            run->cancel();
    }
}

void Evaluator::clearCache()
{
    m_cache->retain({});
}

int Evaluator::cachedResults() const
{
    return m_cache->size();
}

namespace {

// evaluateNode()'s context: no source, never cancelled.
class DirectRun final : public EvalContext::Run
{
public:
    bool isCancelled() const override { return false; }
    void onCancel(std::function<void()>) override {}
    QImage source(double) override { return {}; }
    QImage selection(double) override { return {}; }
    QSize sourceSize() const override { return {}; }
};

} // namespace

Values evaluateNode(const NodeType &type, const Values &inputs, const QVariantMap &params, double scale)
{
    const NodeTypeInfo &info = type.info();
    Values given;
    for (const PortSpec &port : info.inputs) {
        const auto it = inputs.constFind(port.id);
        if (it == inputs.constEnd() || (isPicture(port.type) && it->value<QImage>().isNull())) {
            if (!port.optional)
                throw Error(missingInput(info, port));
            continue;
        }
        given.insert(port.id, port.type == PortType::Image  ? QVariant(img::normalized(it->value<QImage>()))
                              : port.type == PortType::Mask ? QVariant(toMask(it->value<QImage>()))
                                                            : drivenValue(info, port, *it, scale));
    }
    auto checked = [&info](const Values &outputs) {
        try {
            return checkedOutputs(info, outputs);
        } catch (const Error &e) {
            throw Error(nodeFailed(info.name, e.message()));
        }
    };
    DirectRun run;
    EvalContext ctx(&run, info.id, scale);
    const NodeResult result = type.evaluate(NodeArgs(given, resolveParams(info, params, scale)), ctx);
    if (result.isReady())
        return checked(result.outputs());
    QFuture<Values> future = result.future();
    future.waitForFinished(); // rethrows the node's exception, if it has one
    if (future.resultCount() == 0) {
        if (future.isCanceled())
            throw Error(Text::tr("“%1” was cancelled.").arg(info.name));
        return checked({});
    }
    return checked(future.result());
}

} // namespace lumen::filters
