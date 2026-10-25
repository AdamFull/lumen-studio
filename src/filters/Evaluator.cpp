#include "filters/Evaluator.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QPromise>
#include <QSet>

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

// A value arriving at an input: a mask feeding an image input becomes a grey image.
QVariant convertedInput(PortType from, PortType to, const QVariant &value)
{
    if (from == PortType::Mask && to == PortType::Image)
        return img::normalized(value.value<QImage>());
    return value;
}

// The outputs of a node, every declared one present and in its port's canonical format.
Values checkedOutputs(const NodeTypeInfo &info, Values outputs)
{
    for (const PortSpec &p : info.outputs) {
        auto it = outputs.find(p.id);
        const bool isPicture = p.type == PortType::Image || p.type == PortType::Mask;
        if (it == outputs.end() || !it->isValid() || (isPicture && it->value<QImage>().isNull()))
            throw Error(Text::tr("“%1” gave no result.").arg(info.name));
        if (p.type == PortType::Image)
            *it = img::normalized(it->value<QImage>());
        else if (p.type == PortType::Mask)
            *it = toMask(it->value<QImage>());
    }
    return outputs;
}

QString nodeFailed(const QString &name, const QString &message)
{
    return Text::tr("“%1”: %2").arg(name, message);
}

QString missingInput(const NodeTypeInfo &info, const PortSpec &port)
{
    return Text::tr("“%1” has nothing connected to its “%2” input.").arg(info.name, port.label);
}

} // namespace

struct Evaluator::Cache
{
    struct Entry
    {
        QByteArray key;
        Values outputs;
    };

    std::optional<Values> find(const QString &node, const QByteArray &key) const
    {
        QMutexLocker lock(&mutex);
        const auto it = entries.constFind(node);
        if (it == entries.constEnd() || it->key != key)
            return std::nullopt;
        return it->outputs;
    }

    void store(const QString &node, const QByteArray &key, const Values &outputs)
    {
        QMutexLocker lock(&mutex);
        entries.insert(node, {key, outputs});
    }

    // Drops the results of nodes that are no longer in the graph.
    void retain(const QSet<QString> &nodes)
    {
        QMutexLocker lock(&mutex);
        for (auto it = entries.begin(); it != entries.end();) {
            if (nodes.contains(it.key()))
                ++it;
            else
                it = entries.erase(it);
        }
    }

    mutable QMutex mutex;
    QHash<QString, Entry> entries; // node id -> its last result
};

// One evaluation. Runs on the worker pool; between nodes that return futures it is held only by
// the futures' continuations. The evaluation state (index, keys, outputs) is touched by one
// thread at a time, since every hand-over goes through a future; cancel() comes from the caller's
// thread and shares only the members under m_mutex.
class Evaluator::Run final : public EvalContext::Run, public std::enable_shared_from_this<Evaluator::Run>
{
public:
    Run(Graph graph, QStringList order, EvalOptions options, EvalInputs inputs, std::shared_ptr<Cache> cache)
        : m_graph(std::move(graph))
        , m_order(std::move(order))
        , m_options(std::move(options))
        , m_inputs(std::move(inputs))
        , m_cache(std::move(cache))
    {
    }

    QFuture<EvalResult> start()
    {
        m_promise.start();
        return m_promise.future();
    }

    void step()
    {
        while (m_index < m_order.size()) {
            if (isCancelled())
                return finishCancelled();
            const Node *node = m_graph.node(m_order.at(m_index));
            const NodeTypeInfo &info = node->type->info();
            const QVariantMap params = resolveParams(info, node->params, m_options.scale);

            // The key: the type, the parameters, the scale and the keys of the nodes feeding it.
            QCryptographicHash hash(QCryptographicHash::Sha1);
            hash.addData(info.id.toUtf8());
            hash.addData(QByteArrayView("|"));
            for (const ParamSpec &spec : info.params) {
                hash.addData(spec.id.toUtf8());
                hash.addData(QByteArrayView("="));
                hash.addData(canonical(params.value(spec.id)));
                hash.addData(QByteArrayView(";"));
            }
            hash.addData(QByteArray::number(m_options.scale, 'g', 17));
            Values inputs;
            for (const PortSpec &port : info.inputs) {
                hash.addData(port.id.toUtf8());
                hash.addData(QByteArrayView(":"));
                const auto link = m_graph.linkInto(node->id, port.id);
                if (!link) {
                    if (!port.optional)
                        return fail(missingInput(info, port));
                    hash.addData(QByteArrayView("-;"));
                    continue;
                }
                const PortSpec *out = m_graph.node(link->fromNode)->type->info().output(link->fromPort);
                hash.addData(m_keys.value(link->fromNode));
                hash.addData(link->fromPort.toUtf8());
                hash.addData(QByteArrayView(";"));
                inputs.insert(port.id, convertedInput(out->type, port.type, m_outputs.value(link->fromNode).value(link->fromPort)));
            }
            if (info.readsSource)
                hash.addData(sourceKey());
            const QByteArray key = hash.result();
            m_keys.insert(node->id, key);

            if (std::optional<Values> cached = m_cache->find(node->id, key)) {
                m_outputs.insert(node->id, *cached);
                ++m_index;
                continue;
            }
            if (info.expensive && !m_options.runExpensive) {
                m_result.needsRun = true;
                return finishOk();
            }

            m_result.evaluated.append(node->id);
            EvalContext ctx(this, node->id, m_options.scale);
            try {
                const NodeResult result = node->type->evaluate(NodeArgs(inputs, params), ctx);
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

    QImage source() override
    {
        if (m_source.isNull() && !m_inputs.image.isNull()) {
            const QImage image = img::normalized(m_inputs.image);
            m_source = img::scaled(image, scaledSize(image.size(), m_options.scale), true);
        }
        return m_source;
    }

    QImage selection() override
    {
        if (m_selection.isNull()) {
            const QSize size = source().size();
            m_selection = m_inputs.selection.isNull() ? img::createMask(size, 255)
                                                      : img::scaled(toMask(m_inputs.selection), size, true);
        }
        return m_selection;
    }

    QSize sourceSize() const override { return m_inputs.image.size(); }

private:
    QByteArray sourceKey() const
    {
        return QByteArray::number(m_inputs.image.cacheKey()) + 'x' + QByteArray::number(m_inputs.image.width()) + 'x'
             + QByteArray::number(m_inputs.image.height()) + '/' + QByteArray::number(m_inputs.selection.cacheKey());
    }

    void accept(const Values &outputs)
    {
        const QString id = m_order.at(m_index);
        m_cache->store(id, m_keys.value(id), outputs);
        m_outputs.insert(id, outputs);
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
        const NodeTypeInfo &info = m_graph.node(m_order.at(m_index))->type->info();
        try {
            if (future.resultCount() == 0) {
                future.waitForFinished(); // rethrows the node's exception, if it has one
                throw Error(Text::tr("“%1” gave no result.").arg(info.name));
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
        m_result.scale = m_options.scale;
        m_result.outputs = m_outputs.value(m_options.target);
        const Values output = m_outputs.value(m_graph.outputNode());
        m_result.image = output.value(QStringLiteral("image")).value<QImage>();
        m_result.mask = output.value(QStringLiteral("mask")).value<QImage>();
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
    const QStringList m_order;
    const EvalOptions m_options;
    const EvalInputs m_inputs;
    const std::shared_ptr<Cache> m_cache;
    QPromise<EvalResult> m_promise;

    qsizetype m_index = 0;
    QHash<QString, QByteArray> m_keys;
    QHash<QString, Values> m_outputs;
    EvalResult m_result;
    QImage m_source;
    QImage m_selection;

    mutable QMutex m_mutex;
    std::atomic_bool m_cancelledFlag = false; // m_cancelled, readable without the lock
    bool m_cancelled = false;
    bool m_done = false;
    bool m_waiting = false;
    QFuture<Values> m_pending;
    QList<std::function<void()>> m_cancelHandlers;
};

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

    auto run = std::make_shared<Run>(graph, order, opts, inputs, m_cache);
    QFuture<EvalResult> future = run->start();
    {
        QMutexLocker lock(&m_mutex);
        m_runs.removeIf([](const std::weak_ptr<Run> &r) { return r.expired(); });
        m_runs.append(run);
    }
    async::run([run] { run->step(); });
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

int Evaluator::cachedNodes() const
{
    QMutexLocker lock(&m_cache->mutex);
    return static_cast<int>(m_cache->entries.size());
}

namespace {

// evaluateNode()'s context: no source, never cancelled.
class DirectRun final : public EvalContext::Run
{
public:
    bool isCancelled() const override { return false; }
    void onCancel(std::function<void()>) override {}
    QImage source() override { return {}; }
    QImage selection() override { return {}; }
    QSize sourceSize() const override { return {}; }
};

} // namespace

Values evaluateNode(const NodeType &type, const Values &inputs, const QVariantMap &params, double scale)
{
    const NodeTypeInfo &info = type.info();
    Values given;
    for (const PortSpec &port : info.inputs) {
        const auto it = inputs.constFind(port.id);
        const bool isPicture = port.type == PortType::Image || port.type == PortType::Mask;
        if (it == inputs.constEnd() || (isPicture && it->value<QImage>().isNull())) {
            if (!port.optional)
                throw Error(missingInput(info, port));
            continue;
        }
        given.insert(port.id, port.type == PortType::Image   ? QVariant(img::normalized(it->value<QImage>()))
                              : port.type == PortType::Mask ? QVariant(toMask(it->value<QImage>()))
                                                            : *it);
    }
    DirectRun run;
    EvalContext ctx(&run, info.id, scale);
    const NodeResult result = type.evaluate(NodeArgs(given, resolveParams(info, params, scale)), ctx);
    if (result.isReady())
        return checkedOutputs(info, result.outputs());
    QFuture<Values> future = result.future();
    future.waitForFinished();
    if (future.isCanceled() && future.resultCount() == 0)
        throw Error(Text::tr("“%1” was cancelled.").arg(info.name));
    return checkedOutputs(info, future.result());
}

} // namespace lumen::filters
