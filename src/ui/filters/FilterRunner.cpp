#include "ui/filters/FilterRunner.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "filters/Ops.h"

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

namespace {

// The size the evaluator scales the source to for a preview (Evaluator.cpp), so the result and the
// scaled layers fit without another scaling.
QSize scaledSize(const QSize &size, double scale)
{
    if (scale >= 1.0)
        return size;
    return {std::max(1, static_cast<int>(std::lround(size.width() * scale))),
            std::max(1, static_cast<int>(std::lround(size.height() * scale)))};
}

QImage toMask(const QImage &image, const QSize &size)
{
    if (image.isNull())
        return {};
    const QImage m = image.format() == QImage::Format_Grayscale8 ? image : image.convertToFormat(QImage::Format_Grayscale8);
    return img::scaled(m, size, true);
}

// Keeps the opacity of `original` (an alpha-locked layer): the colours of `image` at the old alpha.
QImage withAlphaOf(const QImage &image, const QImage &original)
{
    QImage out = img::normalized(image);
    const QImage o = img::normalized(img::scaled(original, out.size(), true));
    for (int y = 0; y < out.height(); ++y) {
        auto *px = reinterpret_cast<QRgb *>(out.scanLine(y));
        const auto *op = reinterpret_cast<const QRgb *>(o.constScanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int a = qAlpha(op[x]);
            const QRgb c = qUnpremultiply(px[x]);
            px[x] = qPremultiply(qRgba(qRed(c), qGreen(c), qBlue(c), a));
        }
    }
    return out;
}

struct LayerInput
{
    QUuid id;
    quint64 revision = 0;
    QImage image;
    doc::LayerProps props;
};

struct PreviewJob
{
    QList<LayerInput> layers; // bottom to top
    int active = -1;
    QImage selection;
    QSize docSize;
    double scale = 1.0;
    FilterRunner::Target target = FilterRunner::Target::NewLayer;
    bool whole = false; // the filter reads the whole picture
};

struct PreviewOut
{
    bool needsRun = false;
    QImage image;
    QSize resultSize; // valid: the filter changes the picture's size to about this
    bool resultSizeExact = false;
};

struct ApplyJob
{
    QUuid layer;
    // What the result is put together with when apply() started: the layer's pixels, or the whole
    // picture for a filter that reads it and puts its result on a new layer.
    QImage image;
    quint64 revision = 0;  // the layer's revision then
    QByteArray pictureKey; // the picture's state then (wholePictureKey), for a filter that reads it
    QImage selection;
    QSize docSize;
    FilterRunner::Target target = FilterRunner::Target::NewLayer;
    bool alphaLocked = false;
    bool whole = false; // the filter reads the whole picture
    QString name;
};

struct ApplyOut
{
    FilterComposite composite;
    QSize scaleTo; // valid: the picture is scaled to this size first (an upscale)
    // The filter's result and its own mask, to put together again when the layer changed while
    // the filter ran.
    QImage result;
    QImage resultMask;
    bool cancelled = false;
};

// What the whole picture is made of: every layer's pixels and how it is blended, so the merged
// picture is made again only when one of them changes.
QByteArray wholePictureKey(const doc::Document *doc)
{
    QByteArray key = QByteArray::number(doc->size().width()) + 'x' + QByteArray::number(doc->size().height());
    for (const doc::LayerPtr &l : doc->layers()) {
        key += '|' + l->id.toByteArray() + ':' + QByteArray::number(l->revision) + ':' + QByteArray::number(l->image.cacheKey()) + ':'
             + QByteArray::number(l->props.visible) + ':' + QByteArray::number(l->props.opacity, 'g', 9) + ':'
             + QByteArray::number(static_cast<int>(l->props.blend));
    }
    return key;
}

bool isEverywhere(const QImage &mask)
{
    return mask.isNull() || img::maskIsEmpty(img::invertMask(mask), 1);
}

// Whether the result takes opacity away from the layer where it applies (a cut-out): enough
// pixels, not a few at an edge, turn from mostly opaque to mostly transparent.
bool cutsOut(const QImage &layer, const QImage &result, const QImage &mask)
{
    qint64 area = 0, cut = 0;
    for (int y = 0; y < layer.height(); ++y) {
        const auto *l = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
        const auto *r = reinterpret_cast<const QRgb *>(result.constScanLine(y));
        const uchar *m = mask.isNull() ? nullptr : mask.constScanLine(y);
        for (int x = 0; x < layer.width(); ++x) {
            if (m && m[x] < 128)
                continue;
            ++area;
            if (qAlpha(l[x]) - qAlpha(r[x]) >= 128)
                ++cut;
        }
    }
    return cut > 0 && cut * 500 >= area;
}

} // namespace

// Delivers progress from worker threads to the runner while it exists.
struct FilterRunner::ProgressSink
{
    QMutex mutex;
    FilterRunner *runner = nullptr;
};

// Scaled copies of the layers for previews, kept while a layer's pixels stay the same, so moving a
// slider scales only what changed. Used from worker threads.
class FilterPreviewCache
{
public:
    QImage get(const LayerInput &layer, const QSize &size)
    {
        {
            QMutexLocker lock(&m_mutex);
            const auto it = m_items.constFind(layer.id);
            if (it != m_items.constEnd() && it->revision == layer.revision && it->key == layer.image.cacheKey()
                && it->image.size() == size)
                return it->image;
        }
        const QImage scaled = img::scaled(img::normalized(layer.image), size, true);
        QMutexLocker lock(&m_mutex);
        m_items.insert(layer.id, {layer.revision, layer.image.cacheKey(), scaled});
        return scaled;
    }

    void retain(const QSet<QUuid> &ids)
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_items.begin(); it != m_items.end();)
            it = ids.contains(it.key()) ? std::next(it) : m_items.erase(it);
    }

private:
    struct Item
    {
        quint64 revision = 0;
        qint64 key = 0;
        QImage image;
    };
    QMutex m_mutex;
    QHash<QUuid, Item> m_items;
};

QImage combinedMask(const QImage &a, const QImage &b, const QSize &size)
{
    const QImage ma = toMask(a, size);
    const QImage mb = toMask(b, size);
    if (ma.isNull())
        return mb;
    if (mb.isNull())
        return ma;
    QImage out = ma.copy();
    for (int y = 0; y < out.height(); ++y) {
        uchar *o = out.scanLine(y);
        const uchar *p = mb.constScanLine(y);
        for (int x = 0; x < out.width(); ++x)
            o[x] = uchar((o[x] * p[x] + 127) / 255);
    }
    return out;
}

FilterComposite composeFilterResult(const QImage &layer, const QImage &result, const QImage &where,
                                    FilterRunner::Target target, bool keepAlpha)
{
    FilterComposite out;
    const QSize size = layer.size();
    const QImage stretched = img::normalized(img::scaled(result, size, true));
    const QImage mask = toMask(where, size);
    if (target == FilterRunner::Target::NewLayer) {
        out.rect = QRect(QPoint(0, 0), size);
        if (cutsOut(img::normalized(layer), stretched, mask)) {
            // Over the layer, the cut-out would not show: the new layer is the whole layer with the
            // result mixed in, and the layer is hidden under it (still there, unchanged).
            out.pixels = mask.isNull() ? stretched : filters::ops::mix(layer, stretched, mask);
            out.hidesLayer = true;
            return out;
        }
        out.pixels = mask.isNull() ? stretched : img::applyMaskToAlpha(stretched, mask);
        return out;
    }
    out.rect = mask.isNull() ? QRect(QPoint(0, 0), size) : img::maskBounds(mask);
    if (out.rect.isEmpty())
        return out;
    QImage mixed = mask.isNull() ? stretched : filters::ops::mix(layer, stretched, mask);
    if (keepAlpha)
        mixed = withAlphaOf(mixed, layer);
    out.pixels = mixed.copy(out.rect);
    return out;
}

bool resultScalesPicture(const QSize &layer, const QSize &result)
{
    if (layer.isEmpty() || result.isEmpty() || layer == result)
        return false;
    // The height the result would have at the layer's proportions, give or take rounding.
    const double height = double(result.width()) * layer.height() / layer.width();
    return std::abs(height - result.height()) <= 1.0;
}

namespace {

QImage composePreview(const PreviewJob &job, const filters::EvalResult &r, FilterPreviewCache &cache);

// What applying the filter's result to `layer` (the active layer's pixels) gives. Worker threads,
// or the GUI thread when the layer changed while the filter ran.
ApplyOut composeApply(const ApplyJob &job, const QImage &layer, const QImage &result, const QImage &resultMask)
{
    ApplyOut out;
    out.result = result;
    out.resultMask = resultMask;
    // A result of the picture's proportions at another size (an upscale, a resize) makes the
    // picture that size: every layer is scaled, and the result goes on at its own size through
    // the selection scaled with the picture, instead of being stretched back into the layer
    // (which would undo the upscale). Outside the selection the picture is only resized.
    if (layer.size() == job.docSize && resultScalesPicture(layer.size(), result.size())) {
        out.scaleTo = result.size();
        // As Document::scaleImage scales the layer, so pixels outside the selection fit it exactly.
        const QImage scaled = img::scaled(img::normalized(layer), out.scaleTo, true);
        out.composite = composeFilterResult(scaled, result, combinedMask(job.selection, resultMask, out.scaleTo), job.target,
                                            job.alphaLocked);
        return out;
    }
    out.composite = composeFilterResult(layer, result, combinedMask(job.selection, resultMask, layer.size()), job.target,
                                        job.alphaLocked);
    return out;
}

} // namespace

FilterRunner::FilterRunner(QObject *parent)
    : QObject(parent)
    , m_evaluator(std::make_unique<filters::Evaluator>())
    , m_scaled(std::make_shared<FilterPreviewCache>())
    , m_sink(std::make_shared<ProgressSink>())
{
    m_sink->runner = this;
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(40); // a slider being dragged previews at most this often
    connect(&m_previewTimer, &QTimer::timeout, this, [this] { startPreview(false); });
}

FilterRunner::~FilterRunner()
{
    *m_alive = false;
    QMutexLocker lock(&m_sink->mutex);
    m_sink->runner = nullptr;
}

std::function<void(int, int, const QString &)> FilterRunner::progressRelay() const
{
    return [sink = m_sink](int done, int total, const QString &step) {
        QMutexLocker lock(&sink->mutex);
        if (FilterRunner *runner = sink->runner) {
            // Queued to the runner's thread; dropped there if the runner is gone by then.
            QMetaObject::invokeMethod(runner, [runner, done, total, step] { emit runner->progress(done, total, step); },
                                      Qt::QueuedConnection);
        }
    };
}

std::function<void(double, const QString &)> FilterRunner::stepProgressRelay() const
{
    return [sink = m_sink](double fraction, const QString &status) {
        QMutexLocker lock(&sink->mutex);
        if (FilterRunner *runner = sink->runner) {
            QMetaObject::invokeMethod(runner, [runner, fraction, status] { emit runner->stepProgress(fraction, status); },
                                      Qt::QueuedConnection);
        }
    };
}

void FilterRunner::setDocument(doc::Document *doc)
{
    if (m_doc == doc)
        return;
    for (const QMetaObject::Connection &c : std::as_const(m_docConnections))
        disconnect(c);
    m_docConnections.clear();
    m_doc = doc;
    ++m_changes;
    m_suspended = false;
    invalidatePreview();
    clearPreview(); // the old document's
    setResultSize({}, false);
    if (doc) {
        auto changed = [this] { documentChanged(); };
        m_docConnections << connect(doc, &doc::Document::pixelsChanged, this, changed)
                         << connect(doc, &doc::Document::layersChanged, this, changed)
                         << connect(doc, &doc::Document::layerPropsChanged, this, changed)
                         << connect(doc, &doc::Document::selectionChanged, this, changed)
                         << connect(doc, &doc::Document::activeLayerChanged, this, changed)
                         << connect(doc, &doc::Document::sizeChanged, this, [this] {
                                setResultSize({}, false); // known again from the next preview
                                documentChanged();
                            });
    }
    m_scaled->retain({});
    schedulePreview();
}

void FilterRunner::documentChanged()
{
    // Until the next preview the canvas shows the document as it is: a preview of the pixels
    // before the change would hide a brush stroke, a move or an undo while it happens.
    invalidatePreview();
    clearPreview();
    schedulePreview();
}

void FilterRunner::invalidatePreview()
{
    ++m_generation;
    if (!isBusy() && m_previewFuture.isRunning())
        m_previewFuture.cancel(); // stops at its next node
}

void FilterRunner::setGraph(const std::optional<filters::Graph> &graph, const QString &name)
{
    m_graph = graph;
    m_name = name.isEmpty() && graph ? graph->name : name;
    ++m_changes;
    m_suspended = false;
    invalidatePreview(); // another filter's preview must not show for this one
    setResultSize({}, false);
    setNeedsRun(m_graph && m_graph->needsExplicitRun());
    schedulePreview();
}

bool FilterRunner::setParam(const QString &node, const QString &param, const QVariant &value)
{
    if (!m_graph || !m_graph->setParam(node, param, value))
        return false;
    ++m_changes;
    m_suspended = false;
    schedulePreview();
    return true;
}

void FilterRunner::setTarget(Target target)
{
    if (m_target == target)
        return;
    m_target = target;
    ++m_changes;
    m_suspended = false;
    schedulePreview();
}

void FilterRunner::setPreviewEnabled(bool on)
{
    // Not a reason to preview after apply(): the panel is shown again or the window restored.
    m_previewEnabled = on;
    schedulePreview();
}

void FilterRunner::resumePreview()
{
    m_suspended = false;
    schedulePreview();
}

void FilterRunner::setSharedResults(std::shared_ptr<filters::SharedResults> results)
{
    m_evaluator->setSharedResults(std::move(results));
}

QImage FilterRunner::wholePicture()
{
    if (!m_doc)
        return {};
    // The same image while nothing changed: its cacheKey is part of the Input node's key, so a
    // slider moved after an AI step does not run the step again.
    const QByteArray key = wholePictureKey(m_doc);
    if (m_picture.isNull() || key != m_pictureKey) {
        m_pictureKey = key;
        m_picture = m_doc->flattened();
    }
    return m_picture;
}

bool FilterRunner::readsWholePicture() const
{
    return m_graph && m_graph->readsWholePicture();
}

void FilterRunner::setResultSize(const QSize &size, bool exact)
{
    if (m_resultSize == size && m_resultSizeExact == exact)
        return;
    m_resultSize = size;
    m_resultSizeExact = exact;
    emit resultSizeChanged(size);
}

void FilterRunner::setPreviewSize(int longestSide)
{
    longestSide = std::max(64, longestSide);
    if (m_previewSize == longestSide)
        return;
    m_previewSize = longestSide;
    schedulePreview();
}

void FilterRunner::setNeedsRun(bool needsRun)
{
    if (m_needsRun == needsRun)
        return;
    m_needsRun = needsRun;
    emit needsRunChanged(needsRun);
}

void FilterRunner::setBusy(Busy busy)
{
    const bool was = isBusy();
    m_busy = busy;
    if (was != isBusy())
        emit busyChanged(isBusy());
    if (!isBusy() && m_previewPending) {
        m_previewPending = false;
        schedulePreview();
    }
}

void FilterRunner::clearPreview()
{
    if (m_preview.isNull())
        return;
    m_preview = QImage();
    emit previewChanged(m_preview);
}

void FilterRunner::schedulePreview()
{
    if (isBusy()) {
        // A running step is not cut short by a change; the preview follows when it ends.
        m_previewPending = true;
        return;
    }
    if (!m_previewEnabled || m_suspended || !m_graph || !m_doc) {
        m_previewTimer.stop();
        if (m_previewFuture.isRunning())
            m_previewFuture.cancel();
        ++m_generation;
        clearPreview();
        return;
    }
    m_previewTimer.start();
}

void FilterRunner::startPreview(bool runExpensive)
{
    if (m_previewFuture.isRunning())
        m_previewFuture.cancel(); // stale: stops at its next node
    const int generation = ++m_generation;
    const doc::LayerPtr active = m_doc ? m_doc->activeLayer() : nullptr;
    if (!m_graph || !active || (!runExpensive && (!m_previewEnabled || m_suspended))) {
        clearPreview();
        if (runExpensive) {
            setBusy(Busy::No);
            emit finished(false, tr("Open an image first."));
        }
        return;
    }
    PreviewJob job;
    job.docSize = m_doc->size();
    job.scale = std::min(1.0, double(m_previewSize) / std::max(job.docSize.width(), job.docSize.height()));
    job.selection = m_doc->selection();
    job.target = m_target;
    job.whole = readsWholePicture();
    QSet<QUuid> ids;
    for (const doc::LayerPtr &l : m_doc->layers()) {
        if (l == active)
            job.active = int(job.layers.size());
        job.layers.append({l->id, l->revision, l->image, l->props});
        ids.insert(l->id);
    }
    m_scaled->retain(ids);

    filters::EvalOptions options;
    options.scale = job.scale;
    options.runExpensive = runExpensive;
    if (runExpensive) {
        options.progress = progressRelay();
        options.stepProgress = stepProgressRelay();
    }
    const bool show = m_previewEnabled;
    std::shared_ptr<FilterPreviewCache> cache = m_scaled;
    m_previewFuture = m_evaluator->evaluate(*m_graph, {active->image, job.selection, job.whole ? wholePicture() : QImage()}, options);
    m_previewFuture
        .then(async::pool(), [job, cache, show](const filters::EvalResult &r) {
            PreviewOut out;
            out.needsRun = r.needsRun;
            if (r.needsRun)
                return out;
            // The size applying gives the picture, when it changes it: the result's size relative to
            // the layer's at the preview's scale (exact at full scale).
            const QSize layer = scaledSize(job.docSize, job.scale);
            if (resultScalesPicture(layer, r.image.size())) {
                out.resultSize = QSize(int(std::lround(double(job.docSize.width()) * r.image.width() / layer.width())),
                                       int(std::lround(double(job.docSize.height()) * r.image.height() / layer.height())));
                out.resultSizeExact = job.scale >= 1.0;
            }
            if (show)
                out.image = composePreview(job, r, *cache);
            return out;
        })
        .then(this, [this, alive = m_alive, generation, runExpensive](const PreviewOut &out) {
            if (!*alive)
                return;
            if (runExpensive)
                setBusy(Busy::No);
            // Something changed meanwhile (a stroke, another filter): a newer preview follows.
            if (generation == m_generation) {
                setNeedsRun(out.needsRun);
                if (!out.needsRun)
                    setResultSize(out.resultSize, out.resultSizeExact);
                m_preview = out.image;
                emit previewChanged(m_preview);
            }
            if (runExpensive)
                emit finished(true, {});
        })
        .onFailed(this, [this, alive = m_alive, generation, runExpensive](const std::exception &e) {
            if (!*alive)
                return;
            if (runExpensive) {
                setBusy(Busy::No);
                emit finished(false, describeException(e));
            }
            if (generation != m_generation)
                return;
            clearPreview();
            emit previewFailed(describeException(e));
        })
        .onCanceled(this, [this, alive = m_alive, runExpensive] {
            if (!*alive)
                return;
            if (runExpensive) {
                setBusy(Busy::No);
                emit finished(false, tr("Cancelled."));
            }
        });
}

bool FilterRunner::run(QString *why)
{
    auto fail = [why](const QString &message) {
        if (why)
            *why = message;
        return false;
    };
    if (isBusy())
        return fail(tr("The filter is already running."));
    if (!m_graph)
        return fail(tr("Choose a filter first."));
    if (!m_doc || !m_doc->activeLayer())
        return fail(tr("Open an image first."));
    m_previewTimer.stop();
    m_suspended = false;
    setBusy(Busy::Running);
    startPreview(true);
    return true;
}

bool FilterRunner::apply(Target target, QString *why)
{
    auto fail = [why](const QString &message) {
        if (why)
            *why = message;
        return false;
    };
    if (isBusy())
        return fail(tr("The filter is already running."));
    if (!m_graph)
        return fail(tr("Choose a filter first."));
    const doc::LayerPtr layer = m_doc ? m_doc->activeLayer() : nullptr;
    if (!layer)
        return fail(tr("Open an image first."));
    if (target == Target::Layer && layer->props.locked)
        return fail(tr("The layer is locked. Unlock it, or put the result on a new layer."));
    for (const filters::Issue &issue : m_graph->validate()) {
        if (issue.severity == filters::Issue::Severity::Error)
            return fail(issue.message);
    }

    m_previewTimer.stop();
    if (m_previewFuture.isRunning())
        m_previewFuture.cancel();
    ++m_generation;
    setBusy(Busy::Applying);

    ApplyJob job;
    job.layer = layer->id;
    job.whole = readsWholePicture();
    const QImage picture = job.whole ? wholePicture() : QImage();
    // A filter of the whole picture puts its result over every layer: on a new layer, it is put
    // together with the picture; into the layer, with the layer as for any filter.
    job.image = job.whole && target == Target::NewLayer ? picture : layer->image;
    job.revision = layer->revision;
    job.pictureKey = job.whole ? m_pictureKey : QByteArray();
    job.selection = m_doc->selection();
    job.docSize = m_doc->size();
    job.target = target;
    job.alphaLocked = layer->props.alphaLocked;
    job.name = m_name.isEmpty() ? tr("Filter") : m_name;
    filters::EvalOptions options;
    options.progress = progressRelay();
    options.stepProgress = stepProgressRelay();
    QPointer<doc::Document> doc = m_doc;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
    const int changes = m_changes;
    m_evaluator->evaluate(*m_graph, {layer->image, job.selection, picture}, options)
        .then(async::pool(), [job, cancelled](const filters::EvalResult &r) {
            if (*cancelled) {
                ApplyOut out;
                out.cancelled = true;
                return out;
            }
            return composeApply(job, job.image, r.image, r.mask);
        })
        .then(this, [this, alive = m_alive, doc, job, cancelled, changes](ApplyOut result) {
            if (!*alive)
                return;
            setBusy(Busy::No);
            // Cancel pressed while the result was being put together.
            if (result.cancelled || *cancelled) {
                emit finished(false, tr("Cancelled. Nothing was changed."));
                return;
            }
            const doc::LayerPtr layer = doc ? doc->findLayer(job.layer) : nullptr;
            if (!layer || doc->size() != job.docSize) {
                emit finished(false, tr("The picture changed while the filter ran (a layer was deleted or the size changed), "
                                        "so nothing was changed."));
                return;
            }
            if (job.target == Target::Layer && layer->props.locked) {
                emit finished(false, tr("The layer was locked while the filter ran, so nothing was changed."));
                return;
            }
            QStringList notes;
            // Painted on, undone or otherwise changed while the filter ran: put together again with
            // the layer (or the picture) as it is now, so nothing outside where the filter applies
            // goes back.
            const bool overPicture = job.whole && job.target == Target::NewLayer;
            if (overPicture ? wholePictureKey(doc) != job.pictureKey
                            : layer->revision != job.revision || layer->image.cacheKey() != job.image.cacheKey()) {
                ApplyJob now = job;
                now.alphaLocked = layer->props.alphaLocked;
                now.image = overPicture ? doc->flattened() : layer->image;
                result = composeApply(now, now.image, result.result, result.resultMask);
                notes << (overPicture ? tr("The picture changed while the filter ran: your changes stay outside the area the filter changed.")
                                      : tr("The layer changed while the filter ran: your changes stay outside the area the filter changed."));
            }
            const FilterComposite &out = result.composite;
            const QString undoText = tr("Filter: %1").arg(job.name);
            // Scaling the picture or hiding the layer goes into the filter's one undo step.
            const bool several = result.scaleTo.isValid() || out.hidesLayer;
            if (several)
                doc->undoStack()->beginMacro(undoText);
            if (result.scaleTo.isValid())
                doc->scaleImage(result.scaleTo);
            QUuid changed = job.layer;
            if (job.target == Target::NewLayer) {
                const QJsonObject meta{{QStringLiteral("filter"), job.name}};
                // Above the layer, or above them all for a filter of the whole picture.
                QList<QUuid> hide;
                if (out.hidesLayer) {
                    for (const doc::LayerPtr &l : doc->layers()) {
                        if ((job.whole || l->id == job.layer) && l->props.visible)
                            hide << l->id;
                    }
                }
                const int index = job.whole ? doc->layerCount() : doc->indexOf(job.layer) + 1;
                changed = doc->addLayer(job.name, out.pixels, meta, index, undoText);
                for (const QUuid &id : std::as_const(hide)) {
                    doc::LayerProps props = doc->findLayer(id)->props;
                    props.visible = false;
                    doc->setLayerProps(id, props, undoText);
                }
            } else if (!out.rect.isEmpty()) {
                doc->applyPixels(job.layer, out.rect, out.pixels, undoText);
            }
            if (several)
                doc->undoStack()->endMacro();
            if (result.scaleTo.isValid()) {
                notes << tr("The picture is now %1 × %2 pixels.").arg(result.scaleTo.width()).arg(result.scaleTo.height());
                if (!isEverywhere(job.selection))
                    notes << tr("Outside the selection it was only resized.");
            }
            if (out.hidesLayer)
                notes << (job.whole ? tr("The layers are hidden under it, so the cut-out shows.")
                                    : tr("The original layer is hidden under it, so the cut-out shows."));
            m_appliedNote = notes.join(QLatin1Char(' '));
            m_appliedName = job.name;
            // The active layer may now be the result: previewing again would apply the filter
            // twice. Not when another filter, a setting or the document was chosen meanwhile.
            if (changes == m_changes)
                m_suspended = true;
            setResultSize({}, false); // the picture has its new size now
            invalidatePreview();
            clearPreview();
            schedulePreview();
            emit applied(changed);
            emit finished(true, {});
        })
        .onFailed(this, [this, alive = m_alive](const std::exception &e) {
            if (!*alive)
                return;
            setBusy(Busy::No);
            emit finished(false, describeException(e));
        })
        .onCanceled(this, [this, alive = m_alive] {
            if (!*alive)
                return;
            setBusy(Busy::No);
            emit finished(false, tr("Cancelled. Nothing was changed."));
        });
    return true;
}

void FilterRunner::cancel()
{
    if (!isBusy())
        return;
    if (isApplying())
        *m_cancelled = true; // also once the steps are done and the result is being put together
    m_evaluator->cancel();
}

namespace {

QImage composePreview(const PreviewJob &job, const filters::EvalResult &r, FilterPreviewCache &cache)
{
    const QSize size = scaledSize(job.docSize, job.scale);
    if (job.whole && job.target == FilterRunner::Target::NewLayer) {
        // The result over every layer, as apply() puts it on top.
        doc::DocumentSnapshot all;
        for (const LayerInput &l : job.layers)
            all.append({cache.get(l, size), l.props});
        const QImage picture = doc::flatten(all, size);
        const FilterComposite c = composeFilterResult(picture, r.image, combinedMask(job.selection, r.mask, size), job.target, false);
        if (c.hidesLayer) // a cut-out hides the layers under it
            return c.pixels;
        all.append({c.pixels, doc::LayerProps()});
        return doc::flatten(all, size);
    }
    doc::DocumentSnapshot snapshot;
    for (int i = 0; i < job.layers.size(); ++i) {
        const LayerInput &l = job.layers.at(i);
        const QImage scaled = cache.get(l, size);
        if (i != job.active) {
            snapshot.append({scaled, l.props});
            continue;
        }
        const QImage where = combinedMask(job.selection, r.mask, size);
        const FilterComposite c = composeFilterResult(scaled, r.image, where, job.target, l.props.alphaLocked);
        if (job.target == FilterRunner::Target::NewLayer) {
            if (!c.hidesLayer) // a cut-out hides the layer under it
                snapshot.append({scaled, l.props});
            snapshot.append({c.pixels, doc::LayerProps()});
        } else {
            QImage image = scaled;
            if (!c.rect.isEmpty()) {
                image = scaled.copy();
                QPainter p(&image);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                p.drawImage(c.rect.topLeft(), c.pixels);
            }
            snapshot.append({image, l.props});
        }
    }
    return doc::flatten(snapshot, size);
}

} // namespace

} // namespace lumen::ui
