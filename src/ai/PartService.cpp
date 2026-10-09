#include "ai/PartService.h"

#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "core/MaskOps.h"
#include "core/Psd.h"
#include "ai/providers/ComfyUiProvider.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>

namespace lumen::ai {

namespace {

const QString kLocalGroup = QStringLiteral("parts-local");

QImage opaqueForModel(const QImage &source)
{
    return img::flatten(source, Qt::white);
}

QString safeFileName(const QString &name)
{
    QString s = name;
    s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    return s.trimmed().left(80);
}

QJsonArray requestedParts(const doc::PartTaxonomy &taxonomy, const parts::SegmentOptions &options)
{
    QJsonArray parts;
    for (const doc::PartKind &k : taxonomy.kinds) {
        const bool wanted = options.scope == parts::SegmentScope::SelectedKinds ? options.kinds.contains(k.tag) : k.core;
        if (wanted)
            parts.append(QJsonObject{{QStringLiteral("tag"), k.tag}, {QStringLiteral("prompt"), k.segmentPrompt}});
    }
    return parts;
}

} // namespace

QString completionStrategyName(CompletionStrategy s)
{
    switch (s) {
    case CompletionStrategy::Inpaint: return QStringLiteral("inpaint");
    case CompletionStrategy::Decomposition: return QStringLiteral("decomposition");
    case CompletionStrategy::CutOnly: return QStringLiteral("cut");
    case CompletionStrategy::LayerDiffuse: return QStringLiteral("layerdiffuse");
    }
    return QStringLiteral("inpaint");
}

CompletionStrategy completionStrategyFromName(const QString &name)
{
    if (name == QLatin1String("decomposition"))
        return CompletionStrategy::Decomposition;
    if (name == QLatin1String("cut"))
        return CompletionStrategy::CutOnly;
    if (name == QLatin1String("layerdiffuse"))
        return CompletionStrategy::LayerDiffuse;
    return CompletionStrategy::Inpaint;
}

PartService::PartService(ProviderRegistry *registry, JobQueue *queue, GenerationService *generation, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_queue(queue)
    , m_generation(generation)
{
    m_queue->setConcurrency(kLocalGroup, 2);
}

QList<ImageProvider *> PartService::segmenters() const
{
    QList<ImageProvider *> list;
    for (ImageProvider *p : m_registry->imageProviders()) {
        if (p->supportsUtility(Utility::SegmentParts))
            list.append(p);
    }
    return list;
}

ImageProvider *PartService::layerDiffuseProvider(const QString &preferredId, QString *templateId) const
{
    const auto tpl = ComfyUiProvider::layerDiffuseTemplate();
    if (templateId)
        *templateId = tpl ? tpl->id : QString();
    if (!tpl)
        return nullptr;
    ImageProvider *preferred = m_registry->imageProvider(preferredId);
    if (preferred && qobject_cast<ComfyUiProvider *>(preferred))
        return preferred;
    for (ImageProvider *p : m_registry->imageProviders()) {
        if (qobject_cast<ComfyUiProvider *>(p))
            return p;
    }
    return nullptr;
}

// ---- Segmentation -----------------------------------------------------------------------------------

Job *PartService::segment(doc::PartSet *setIn, const QString &providerId, const parts::SegmentOptions &optionsIn)
{
    QPointer<doc::PartSet> set(setIn);
    ImageProvider *provider = m_generation->providerForUtility(Utility::SegmentParts, providerId);
    QPointer<ImageProvider> guard(provider);
    parts::SegmentOptions options = optionsIn;
    if (provider && options.provenance.isEmpty())
        options.provenance = QStringLiteral("auto:") + provider->displayName();
    const QJsonArray requested = setIn ? requestedParts(setIn->taxonomy(), options) : QJsonArray();
    QPointer<PartService> self(this);
    Job *job = m_queue->submit(tr("Segment character parts"), provider ? provider->instanceId() : QStringLiteral("none"),
                               [self, set, guard, options, requested](Job *job) {
        if (!guard) {
            job->fail(tr("No provider can segment character parts. Add a Lumen Part Server, a ComfyUI segmentation "
                         "workflow or Google Gemini in Settings → Providers, or use the skeleton segmenter."));
            return;
        }
        if (!set || !set->hasSource()) {
            job->fail(tr("The part set is no longer open"));
            return;
        }
        const QJsonObject params{{QStringLiteral("parts"), requested}};
        ImageOperation *op = guard->runUtility(Utility::SegmentParts, opaqueForModel(set->source()), params);
        job->setCancelHandler([op = QPointer<ImageOperation>(op), job] {
            if (op)
                op->cancel();
            else
                job->reportCancelled();
        });
        QObject::connect(op, &Operation::progress, job, [job](double f, const QString &s) { job->setProgress(f, s); });
        QObject::connect(op, &Operation::finished, job, [self, job, op, set, options] {
            if (op->state() == Operation::State::Cancelled) {
                job->reportCancelled();
                return;
            }
            if (op->state() == Operation::State::Failed) {
                job->fail(op->errorString());
                return;
            }
            if (!self) {
                job->fail(tr("Cancelled"));
                return;
            }
            QList<parts::SegmentLayer> layers;
            for (const GeneratedImage &g : op->result().images) {
                parts::SegmentLayer l;
                l.label = g.info.value(QStringLiteral("label")).toString();
                l.kind = g.info.value(QStringLiteral("kind")).toString(QStringLiteral("mask"));
                l.image = g.image;
                l.order = g.info.value(QStringLiteral("order")).toInt(-1);
                l.depth = g.info.value(QStringLiteral("depth")).toDouble(-1.0);
                layers.append(l);
            }
            self->applySegmentation(job, set, layers, options, op->result().info);
        });
    }, QStringLiteral("parts"));
    return job;
}

Job *PartService::segmentWithSkeleton(doc::PartSet *setIn, const pose::Person &skeleton, const parts::SegmentOptions &optionsIn)
{
    QPointer<doc::PartSet> set(setIn);
    QPointer<PartService> self(this);
    parts::SegmentOptions options = optionsIn;
    if (options.provenance.isEmpty())
        options.provenance = QStringLiteral("auto:skeleton");
    return m_queue->submit(tr("Segment parts from skeleton"), kLocalGroup, [self, set, skeleton, options](Job *job) {
        if (!set || !set->hasSource()) {
            job->fail(tr("The part set is no longer open"));
            return;
        }
        const QImage source = set->source();
        const QImage silhouette = set->silhouette();
        const QString taxonomy = set->taxonomyId();
        job->setProgress(-1.0, tr("Assigning pixels to bones..."));
        async::run([source, silhouette, skeleton, taxonomy] {
            const QImage sil = silhouette.isNull() ? img::silhouetteMask(source) : silhouette;
            const QList<parts::BoneGroup> groups = parts::skeletonGroups(skeleton, taxonomy);
            if (groups.isEmpty())
                throw ProviderError(QObject::tr("The skeleton has no visible joints."));
            const QList<QImage> masks = parts::splitBySkeleton(sil, groups);
            QList<parts::SegmentLayer> layers;
            for (int i = 0; i < groups.size(); ++i) {
                parts::SegmentLayer l;
                l.label = groups[i].tag;
                l.side = groups[i].side;
                l.image = masks[i];
                layers.append(l);
            }
            return layers;
        }).then(job, [self, job, set, options](QList<parts::SegmentLayer> layers) {
            if (self)
                self->applySegmentation(job, set, layers, options, {});
        }).onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
    }, QStringLiteral("parts"));
}

void PartService::applySegmentation(Job *job, QPointer<doc::PartSet> set, QList<parts::SegmentLayer> layers,
                                    parts::SegmentOptions options, QJsonObject decompositionInfo)
{
    if (!set) {
        job->fail(tr("The part set is no longer open"));
        return;
    }
    job->setProgress(-1.0, tr("Building part masks..."));
    const doc::PartSet::Snapshot snapshot = set->snapshot();
    const QImage source = set->source();
    QImage silhouette = set->silhouette();
    const doc::PartTaxonomy taxonomy = set->taxonomy();
    QPointer<PartService> self(this);
    QPointer<doc::Project> project = m_generation->project();
    async::run([snapshot, source, silhouette, taxonomy, layers, options] {
        const QImage sil = silhouette.isNull() ? img::silhouetteMask(source) : silhouette;
        return std::make_pair(parts::mergeSegmentation(snapshot, sil, taxonomy, layers, options), sil);
    }).then(job, [self, job, set, layers, project, decompositionInfo](std::pair<parts::SegmentOutcome, QImage> result) {
        if (!set || !self) {
            job->fail(tr("The part set is no longer open"));
            return;
        }
        const parts::SegmentOutcome &outcome = result.first;
        if (set->silhouette().isNull())
            set->setSilhouette(result.second);
        if (outcome.parts.isEmpty() && outcome.created.isEmpty()) {
            job->fail(tr("No parts were found in the image."));
            return;
        }
        set->replaceParts(outcome.labels, outcome.parts, tr("Auto-segment parts"));
        emit self->segmentationApplied(set, outcome.created, outcome.empty);
        QString summary = tr("%n part(s) found", nullptr, int(outcome.created.size()));
        if (!outcome.empty.isEmpty())
            summary += QStringLiteral(" · ") + tr("not found: %1").arg(outcome.empty.join(QStringLiteral(", ")));

        // Keep RGBA layers of model decompositions for the "decomposition" completion strategy.
        QList<parts::SegmentLayer> rgba;
        for (const parts::SegmentLayer &l : layers) {
            if (l.kind == QLatin1String("layer"))
                rgba.append(l);
        }
        if (rgba.isEmpty() || !project || !project->isOpen()) {
            job->succeed(summary);
            return;
        }
        job->setProgress(-1.0, tr("Saving model layers..."));
        struct Pending
        {
            int remaining = 0;
            QJsonObject decomposition;
        };
        auto pending = std::make_shared<Pending>();
        pending->remaining = int(rgba.size());
        const QString setId = set->id();
        const QString sourceId = set->sourceAssetId();
        const doc::PartTaxonomy taxonomy = set->taxonomy();
        for (const parts::SegmentLayer &l : std::as_const(rgba)) {
            const doc::PartKind *kind = taxonomy.find(l.label);
            const QString tag = kind ? kind->tag : l.label;
            const QImage layerImage = l.image;
            async::run([layerImage] {
                QImage cropped = img::normalized(layerImage);
                const QRect rect = parts::cropToContent(cropped);
                return std::make_pair(cropped, rect);
            }).then(job, [project, job, pending, setId, sourceId, tag, l, decompositionInfo](std::pair<QImage, QRect> cropped) {
                auto finishOne = [pending]() { --pending->remaining; };
                if (cropped.first.isNull() || !project) {
                    finishOne();
                    return QtFuture::makeReadyVoidFuture();
                }
                const QJsonObject meta{{QStringLiteral("role"), QStringLiteral("decomposition")},
                                       {QStringLiteral("part_set"), setId},
                                       {QStringLiteral("tag"), tag},
                                       {QStringLiteral("x"), cropped.second.x()},
                                       {QStringLiteral("y"), cropped.second.y()},
                                       {QStringLiteral("order"), l.order},
                                       {QStringLiteral("depth"), l.depth},
                                       {QStringLiteral("model_info"), decompositionInfo}};
                return project->addAsset(cropped.first, doc::AssetKind::Part, tr("Model layer · %1").arg(tag), meta, sourceId)
                    .then(job, [pending, tag, cropped, l, finishOne](doc::AssetInfo info) {
                        QJsonArray entries = pending->decomposition.value(tag).toArray();
                        entries.append(QJsonObject{{QStringLiteral("asset"), info.id},
                                                   {QStringLiteral("x"), cropped.second.x()},
                                                   {QStringLiteral("y"), cropped.second.y()},
                                                   {QStringLiteral("order"), l.order},
                                                   {QStringLiteral("depth"), l.depth}});
                        pending->decomposition.insert(tag, entries);
                        finishOne();
                    });
            }).unwrap().then(job, [job, pending, set, summary] {
                if (pending->remaining > 0)
                    return;
                if (set)
                    set->setDecomposition(pending->decomposition);
                job->succeed(summary);
            }).onFailed(job, [job, summary](const std::exception &e) {
                qCWarning(lcAi) << "Saving decomposition layer failed:" << e.what();
                if (!job->isFinished())
                    job->succeed(summary);
            });
        }
    }).onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
}

// ---- Completion -------------------------------------------------------------------------------------

QList<Job *> PartService::complete(doc::PartSet *setIn, const QList<int> &labels, const CompletionOptions &optionsIn)
{
    QList<Job *> jobs;
    CompletionOptions options = optionsIn;
    QString layerDiffuseTemplate;
    if (options.strategy == CompletionStrategy::LayerDiffuse) {
        ImageProvider *comfy = layerDiffuseProvider(options.providerId, &layerDiffuseTemplate);
        options.providerId = comfy ? comfy->instanceId() : QString();
        options.variants = 1; // the workflow decodes one transparent image per run
    }
    QPointer<doc::PartSet> set(setIn);
    QPointer<PartService> self(this);
    for (int label : labels) {
        const doc::Part *part = setIn->part(label);
        if (!part || part->pixels == 0)
            continue;
        const QString partId = part->id;
        const bool generates = options.strategy == CompletionStrategy::Inpaint || options.strategy == CompletionStrategy::LayerDiffuse;
        const QString group = generates ? m_generation->groupFor(options.providerId) : kLocalGroup;
        setIn->setPartState(label, doc::PartState::Queued);
        Job *job = m_queue->submit(tr("Part · %1").arg(part->displayName()), group, [self, set, partId, options, layerDiffuseTemplate](Job *job) {
            if (!self || !set) {
                job->fail(tr("The part set is no longer open"));
                return;
            }
            if (!set->partById(partId)) {
                job->fail(tr("The part was deleted"));
                return;
            }
            const doc::Part part = *set->partById(partId); // copy: state changes may reallocate the list
            const int label = part.label;
            set->setPartState(label, doc::PartState::Generating);
            const QImage source = set->source();
            const doc::PartSet::Snapshot snapshot = set->snapshot();
            const QImage silhouette = set->silhouette();
            const QJsonObject meta{{QStringLiteral("part_set"), set->id()},
                                   {QStringLiteral("part_id"), partId},
                                   {QStringLiteral("tag"), part.tag},
                                   {QStringLiteral("side"), part.side},
                                   {QStringLiteral("strategy"), completionStrategyName(options.strategy)}};
            job->setProgress(-1.0, tr("Preparing part..."));
            async::run([snapshot, silhouette, label] {
                return std::make_pair(img::maskForLabel(snapshot.labels, label), doc::PartSet::extensionMaskFor(snapshot, silhouette, label));
            }).then(job, [self, job, set, partId, options, part, label, source, meta, layerDiffuseTemplate](std::pair<QImage, QImage> masks) {
                if (!self || !set) {
                    job->fail(tr("The part set is no longer open"));
                    return;
                }
                const QImage visible = masks.first;
                const QImage extension = masks.second;

                if (options.strategy == CompletionStrategy::CutOnly) {
                    async::run([source, visible] { return parts::cutOutPart(source, visible); })
                        .then(job, [self, job, set, label, partId, meta](QImage layer) {
                            if (self)
                                self->storeResult(job, set, label, partId, layer, meta);
                        });
                    return;
                }

                if (options.strategy == CompletionStrategy::Decomposition) {
                    const doc::PartKind *kind = set->taxonomy().find(part.tag);
                    const QString tag = kind ? kind->tag : part.tag;
                    QJsonArray entries = set->decomposition().value(tag).toArray();
                    if (entries.isEmpty()) {
                        job->fail(tr("No model layer for \"%1\". Run a model segmentation (see-through) first or use Inpaint.").arg(part.displayName()));
                        return;
                    }
                    QPointer<doc::Project> project = self->m_generation->project();
                    if (!project) {
                        job->fail(tr("No project is open"));
                        return;
                    }
                    // Several model layers may map to one kind; stack them in model order.
                    auto images = std::make_shared<QList<std::pair<QJsonObject, QImage>>>();
                    auto remaining = std::make_shared<int>(int(entries.size()));
                    for (const QJsonValue &v : entries) {
                        const QJsonObject e = v.toObject();
                        project->loadImage(e.value(QStringLiteral("asset")).toString())
                            .then(job, [self, job, set, label, partId, meta, source, visible, extension, images, remaining, e](QImage img) {
                                images->append({e, img});
                                if (--*remaining > 0)
                                    return;
                                const QList<std::pair<QJsonObject, QImage>> all = *images;
                                async::run([all, source, visible, extension] {
                                    QImage layer(source.size(), QImage::Format_ARGB32_Premultiplied);
                                    layer.fill(Qt::transparent);
                                    QList<std::pair<QJsonObject, QImage>> sorted = all;
                                    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
                                        return a.first.value(QStringLiteral("order")).toInt() < b.first.value(QStringLiteral("order")).toInt();
                                    });
                                    QPainter painter(&layer);
                                    for (const auto &[entry, image] : sorted)
                                        painter.drawImage(QPoint(entry.value(QStringLiteral("x")).toInt(), entry.value(QStringLiteral("y")).toInt()), image);
                                    painter.end();
                                    return parts::partFromLayer(layer, source, visible, extension);
                                }).then(job, [self, job, set, label, partId, meta](QImage layer) {
                                    if (self)
                                        self->storeResult(job, set, label, partId, layer, meta);
                                });
                            })
                            .onFailed(job, [job](const std::exception &ex) { job->fail(describeException(ex)); });
                    }
                    return;
                }

                // Inpaint / LayerDiffuse: build the isolated context, generate the hidden area, then key out
                // the background (or take the model's alpha).
                const bool layerDiffuse = options.strategy == CompletionStrategy::LayerDiffuse;
                if (layerDiffuse && (options.providerId.isEmpty() || layerDiffuseTemplate.isEmpty())) {
                    job->fail(layerDiffuseTemplate.isEmpty()
                                  ? tr("The LayerDiffuse workflow template is missing (data/workflows/comfyui/complete_part_layerdiffuse.json).")
                                  : tr("LayerDiffuse needs a ComfyUI provider with the ComfyUI-layerdiffuse nodes."));
                    return;
                }
                const doc::PartKind *kind = set->taxonomy().find(part.tag);
                const doc::PartKind kindCopy = kind ? *kind : doc::PartKind();
                const bool hasKind = kind != nullptr;
                async::run([source, visible, extension] { return parts::buildCompletionContext(source, visible, extension); })
                    .then(job, [self, job, set, label, partId, meta, options, part, kindCopy, hasKind, source, visible, extension, layerDiffuse,
                                layerDiffuseTemplate](parts::CompletionContext ctx) {
                        if (!self || !set) {
                            job->fail(tr("The part set is no longer open"));
                            return;
                        }
                        if (img::maskIsEmpty(ctx.mask, 128)) {
                            // Nothing hidden: the visible pixels are the whole part.
                            async::run([source, visible] { return parts::cutOutPart(source, visible); })
                                .then(job, [self, job, set, label, partId, meta](QImage layer) {
                                    if (self)
                                        self->storeResult(job, set, label, partId, layer, meta);
                                });
                            return;
                        }
                        GenerationSpec spec;
                        spec.providerId = options.providerId;
                        spec.title = job->title();
                        spec.saveAssets = false;
                        spec.request.mode = GenMode::Inpaint;
                        spec.request.prompt = parts::completionPrompt(part, hasKind ? &kindCopy : nullptr, options.characterSummary, ctx.key);
                        spec.request.negativePrompt = options.negativePrompt;
                        spec.request.steps = options.steps;
                        spec.request.cfg = options.cfg;
                        spec.request.denoise = options.denoise;
                        spec.request.model = options.model;
                        spec.request.batchCount = std::max(1, options.variants);
                        if (layerDiffuse) {
                            spec.request.extras.insert(QLatin1String(ComfyUiProvider::kTemplateExtra), layerDiffuseTemplate);
                            spec.request.transparentBackground = true;
                        }
                        spec.request.initImage = ctx.image;
                        spec.request.mask = ctx.mask;
                        RegionInput region;
                        region.source = ctx.image;
                        region.mask = ctx.mask;
                        region.padding = 64;
                        region.grow = 4;
                        region.feather = 6;
                        region.colorMatch = false;
                        spec.region = region;
                        QJsonObject fullMeta = meta;
                        fullMeta.insert(QStringLiteral("prompt"), spec.request.prompt);
                        fullMeta.insert(QStringLiteral("key_color"), ctx.key.name());
                        const QImage contextImage = ctx.image;
                        const QColor key = ctx.key;
                        self->m_generation->execute(job, spec, [self, set, label, partId, fullMeta, contextImage, key, source, visible, extension,
                                                                layerDiffuse](Job *job, const GenerationSpec &used, GenerationOutput out) {
                            if (out.images.isEmpty()) {
                                job->fail(out.text.isEmpty() ? tr("The provider returned no images") : out.text);
                                return;
                            }
                            QJsonObject m = fullMeta;
                            m.insert(QStringLiteral("request"), used.request.describe());
                            const QList<QImage> generated = out.images;
                            const QList<QImage> raw = out.rawImages;
                            const QRect region = out.region;
                            job->setProgress(-1.0, tr("Extracting part..."));
                            async::run([generated, raw, region, layerDiffuse, contextImage, key, source, visible, extension] {
                                QList<QImage> layers;
                                for (int i = 0; i < generated.size(); ++i) {
                                    const QImage &g = generated[i];
                                    if (layerDiffuse && i < raw.size() && !region.isEmpty() && parts::hasTransparency(raw[i])) {
                                        // The model returned a transparent crop: place it back on the canvas.
                                        QImage full(source.size(), QImage::Format_ARGB32_Premultiplied);
                                        full.fill(Qt::transparent);
                                        QPainter painter(&full);
                                        painter.setRenderHint(QPainter::SmoothPixmapTransform);
                                        painter.drawImage(QRectF(region), img::normalized(raw[i]));
                                        painter.end();
                                        layers.append(parts::extractTransparentPart(full, source, visible, extension));
                                        continue;
                                    }
                                    QImage full = contextImage;
                                    QPainter painter(&full);
                                    painter.drawImage(0, 0, g);
                                    painter.end();
                                    layers.append(parts::extractCompletedPart(full, source, visible, extension, key));
                                }
                                return layers;
                            }).then(job, [self, job, set, label, partId, m](QList<QImage> layers) {
                                if (!self)
                                    return;
                                // Every variant is stored; the first becomes the chosen result and the job
                                // completes with the last one.
                                for (int i = 0; i < layers.size(); ++i) {
                                    QJsonObject vm = m;
                                    vm.insert(QStringLiteral("variant"), i);
                                    self->storeResult(i == layers.size() - 1 ? job : nullptr, set, label, partId, layers[i], vm, i == 0);
                                }
                            });
                        });
                    })
                    .onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
            }).onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
        }, QStringLiteral("parts"));
        connect(job, &Job::finished, this, [set, partId](Job *j) {
            if (!set)
                return;
            const doc::Part *p = set->partById(partId);
            if (!p)
                return;
            if (j->state() == Job::State::Failed)
                set->setPartState(p->label, doc::PartState::Failed, j->errorString());
            else if (j->state() == Job::State::Cancelled)
                set->setPartState(p->label, p->hasResult() ? doc::PartState::Ready : doc::PartState::Masked);
        });
        jobs.append(job);
    }
    return jobs;
}

Job *PartService::generateExpression(doc::PartSet *setIn, const QList<int> &baseLabelsIn, const ExpressionOptions &options)
{
    QPointer<doc::PartSet> set(setIn);
    QPointer<PartService> self(this);
    const parts::ExpressionKind *kindPtr = parts::findExpression(options.kind);
    const parts::ExpressionKind kind = kindPtr ? *kindPtr : parts::ExpressionKind{};
    QList<int> base = baseLabelsIn;
    if (base.isEmpty() && setIn) {
        for (const doc::Part &p : setIn->parts()) {
            if (kind.baseTags.contains(p.tag) && p.pixels > 0)
                base.append(p.label);
        }
    }
    const QString title = tr("Expression · %1").arg(kind.name.isEmpty() ? options.kind : kind.name);
    return m_queue->submit(title, m_generation->groupFor(options.providerId), [self, set, base, kind, options](Job *job) {
        if (!self || !set || !set->hasSource()) {
            job->fail(tr("The part set is no longer open"));
            return;
        }
        if (kind.id.isEmpty()) {
            job->fail(tr("Unknown expression \"%1\"").arg(options.kind));
            return;
        }
        if (base.isEmpty()) {
            job->fail(tr("No %1 parts found - select the parts to repaint first.").arg(kind.baseTags.join(QStringLiteral("/"))));
            return;
        }
        const QImage labels = set->labels();
        const QImage silhouette = set->silhouette();
        const QImage source = set->source();
        job->setProgress(-1.0, tr("Preparing..."));
        async::run([labels, base, silhouette, source] {
            const QImage region = parts::expressionRegion(labels, base, silhouette);
            if (img::maskIsEmpty(region, 128))
                throw ProviderError(QObject::tr("The selected parts are empty."));
            return std::make_pair(region, img::flatten(source, Qt::white));
        }).then(job, [self, set, job, kind, options, silhouette](std::pair<QImage, QImage> prepared) {
            if (!self || !set) {
                job->fail(tr("The part set is no longer open"));
                return;
            }
            GenerationSpec spec;
            spec.providerId = options.providerId;
            spec.saveAssets = false;
            spec.title = job->title();
            spec.request.mode = GenMode::Inpaint;
            spec.request.prompt = parts::expressionPrompt(kind, options.characterSummary);
            spec.request.negativePrompt = QStringLiteral("different character, different style, extra eyes, text, watermark");
            spec.request.steps = options.steps;
            spec.request.cfg = options.cfg;
            spec.request.denoise = options.denoise;
            spec.request.model = options.model;
            spec.request.initImage = prepared.second;
            spec.request.mask = prepared.first;
            RegionInput region;
            region.source = prepared.second;
            region.mask = prepared.first;
            region.padding = 64;
            region.grow = 4;
            region.feather = 8;
            region.colorMatch = true;
            spec.region = region;
            self->m_generation->execute(job, spec, [self, set, kind, silhouette](Job *job, const GenerationSpec &used, GenerationOutput out) {
                if (!self || !set) {
                    job->fail(tr("The part set is no longer open"));
                    return;
                }
                if (out.images.isEmpty()) {
                    job->fail(out.text.isEmpty() ? tr("The provider returned no images") : out.text);
                    return;
                }
                QImage layer = out.images.first();
                if (!silhouette.isNull() && silhouette.size() == layer.size())
                    layer = img::applyMaskToAlpha(layer, img::dilateMask(silhouette, 1));
                // One variant part per expression; regenerating adds a variant to it.
                int label = 0;
                for (const doc::Part &p : set->parts()) {
                    if (p.tag == kind.tag)
                        label = p.label;
                }
                if (label == 0) {
                    label = set->addPart(kind.tag, kind.name);
                    if (label == 0) {
                        job->fail(tr("A part set holds at most %1 parts.").arg(doc::PartSet::kMaxParts));
                        return;
                    }
                    doc::Part p = *set->part(label);
                    p.visible = false; // alternates are hidden layers in the export
                    p.provenance = QStringLiteral("variant:") + kind.id;
                    int order = 0;
                    for (const doc::Part &o : set->parts()) {
                        if (kind.baseTags.contains(o.tag))
                            order = std::max(order, o.order + 1);
                    }
                    p.order = order;
                    set->updatePart(p, tr("Add expression variant"));
                }
                const QJsonObject meta{{QStringLiteral("part_set"), set->id()},
                                       {QStringLiteral("part_id"), set->part(label)->id},
                                       {QStringLiteral("tag"), kind.tag},
                                       {QStringLiteral("expression"), kind.id},
                                       {QStringLiteral("prompt"), used.request.prompt},
                                       {QStringLiteral("request"), used.request.describe()}};
                self->storeResult(job, set, label, set->part(label)->id, layer, meta);
            });
        }).onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
    }, QStringLiteral("parts"));
}

void PartService::storeResult(Job *job, QPointer<doc::PartSet> set, int, QString partId, QImage layerIn, QJsonObject meta, bool choose)
{
    QPointer<doc::Project> project = m_generation->project();
    QPointer<Job> jobGuard(job);
    async::run([layerIn] {
        QImage layer = img::normalized(layerIn);
        const QRect rect = parts::cropToContent(layer);
        return std::make_pair(layer, rect);
    }).then(this, [project, jobGuard, set, partId, meta, choose](std::pair<QImage, QRect> cropped) {
        if (cropped.first.isNull())
            throw ProviderError(tr("The generated part is empty"));
        if (!project || !project->isOpen())
            throw ProviderError(tr("No project is open"));
        QJsonObject m = meta;
        m.insert(QStringLiteral("x"), cropped.second.x());
        m.insert(QStringLiteral("y"), cropped.second.y());
        const doc::Part *p = set ? set->partById(partId) : nullptr;
        const QString name = p ? tr("Part · %1").arg(p->displayName()) : tr("Part");
        const QString parent = set ? set->sourceAssetId() : QString();
        return project->addAsset(cropped.first, doc::AssetKind::Part, name, m, parent)
            .then(project, [set, partId, cropped, choose](doc::AssetInfo info) {
                if (!set)
                    return info;
                set->setResultImage(info.id, cropped.first);
                if (const doc::Part *p = set->partById(partId))
                    set->addVariant(p->label, info.id, cropped.second, choose);
                return info;
            });
    }).unwrap().then(this, [jobGuard](doc::AssetInfo) {
        if (jobGuard && !jobGuard->isFinished())
            jobGuard->succeed();
    }).onFailed(this, [jobGuard](const std::exception &e) {
        if (jobGuard && !jobGuard->isFinished())
            jobGuard->fail(describeException(e));
    });
}

void PartService::loadResults(doc::PartSet *setIn)
{
    QPointer<doc::PartSet> set(setIn);
    doc::Project *project = m_generation->project();
    if (!set || !project)
        return;
    for (const doc::Part &p : set->parts()) {
        const QString id = p.chosenAssetId();
        if (id.isEmpty() || set->hasResultImage(id))
            continue;
        project->loadImage(id).then(set, [set, id](QImage image) {
            if (set && !image.isNull())
                set->setResultImage(id, img::normalized(image));
        }).onFailed(set, [id](const std::exception &e) { qCWarning(lcAi) << "Loading part result" << id << "failed:" << e.what(); });
    }
}

// ---- Export ---------------------------------------------------------------------------------------

Job *PartService::runExport(doc::PartSet *setIn, const QString &title, const ExportOptions &options,
                            std::function<void(const QList<parts::ExportLayer> &, const doc::PartSet::Snapshot &, const QImage &)> write)
{
    QPointer<doc::PartSet> set(setIn);
    QPointer<doc::Project> project = m_generation->project();
    return m_queue->submit(title, kLocalGroup, [set, project, options, write](Job *job) {
        if (!set || !set->hasSource()) {
            job->fail(tr("The part set is no longer open"));
            return;
        }
        const doc::PartSet::Snapshot snapshot = set->snapshot();
        const QImage source = set->source();
        // Collect chosen results (load the ones not in memory yet).
        auto results = std::make_shared<QHash<QString, QImage>>();
        QStringList missing;
        for (const doc::Part &p : snapshot.parts) {
            const QString id = p.chosenAssetId();
            if (id.isEmpty())
                continue;
            if (set->hasResultImage(id))
                results->insert(id, set->resultImage(p.label));
            else
                missing.append(id);
        }
        auto run = [job, snapshot, source, results, options, write] {
            job->setProgress(-1.0, tr("Writing files..."));
            async::run([snapshot, source, results, options, write] {
                const QList<parts::ExportLayer> layers = parts::exportLayers(snapshot, source, *results, options.includeCutOuts);
                if (layers.isEmpty())
                    throw ProviderError(QObject::tr("There are no parts to export."));
                write(layers, snapshot, source);
            }).then(job, [job] { job->succeed(); }).onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
        };
        if (missing.isEmpty() || !project) {
            run();
            return;
        }
        auto remaining = std::make_shared<int>(int(missing.size()));
        for (const QString &id : missing) {
            project->loadImage(id)
                .then(job, [results, remaining, id, run](QImage image) {
                    results->insert(id, img::normalized(image));
                    if (--*remaining == 0)
                        run();
                })
                .onFailed(job, [remaining, run, id](const std::exception &e) {
                    qCWarning(lcAi) << "Export: cannot load" << id << e.what();
                    if (--*remaining == 0)
                        run();
                });
        }
    }, QStringLiteral("export"));
}

Job *PartService::exportPsd(doc::PartSet *set, const QString &path, const ExportOptions &options)
{
    return runExport(set, tr("Export layered PSD"), options,
                     [path, options](const QList<parts::ExportLayer> &layers, const doc::PartSet::Snapshot &, const QImage &source) {
                         psd::File file;
                         file.size = source.size();
                         // PSD panels list the front-most layer first.
                         for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
                             psd::Layer l = psd::Layer::pixels(it->name, it->image, it->rect.topLeft());
                             l.visible = it->visible;
                             file.layers.append(l);
                         }
                         if (options.includeSource) {
                             psd::Layer original = psd::Layer::pixels(QStringLiteral("Original"), source);
                             original.visible = false;
                             file.layers.append(original);
                         }
                         QString error;
                         if (!psd::write(path, file, &error))
                             throw ProviderError(QObject::tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), error));
                     });
}

Job *PartService::exportPngFolder(doc::PartSet *set, const QString &directory, const ExportOptions &options)
{
    return runExport(set, tr("Export part PNGs"), options,
                     [directory](const QList<parts::ExportLayer> &layers, const doc::PartSet::Snapshot &snapshot, const QImage &source) {
                         QDir dir(directory);
                         if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
                             throw ProviderError(QObject::tr("Cannot create %1").arg(QDir::toNativeSeparators(directory)));
                         QJsonObject manifest = parts::exportManifest(layers, source.size(), snapshot.name);
                         QJsonArray entries = manifest.value(QStringLiteral("layers")).toArray();
                         QImage preview(source.size(), QImage::Format_ARGB32_Premultiplied);
                         preview.fill(Qt::transparent);
                         QPainter previewPainter(&preview);
                         for (int i = 0; i < layers.size(); ++i) {
                             const parts::ExportLayer &l = layers[i];
                             QImage full(source.size(), QImage::Format_ARGB32_Premultiplied);
                             full.fill(Qt::transparent);
                             QPainter p(&full);
                             p.drawImage(l.rect.topLeft(), l.image);
                             p.end();
                             if (l.visible)
                                 previewPainter.drawImage(l.rect.topLeft(), l.image);
                             const QString file = QStringLiteral("%1_%2.png").arg(i, 2, 10, QLatin1Char('0')).arg(safeFileName(l.name));
                             QJsonObject e = entries[i].toObject();
                             e.insert(QStringLiteral("file"), file);
                             entries[i] = e;
                             if (!full.save(dir.filePath(file), "PNG"))
                                 throw ProviderError(QObject::tr("Could not write %1").arg(QDir::toNativeSeparators(dir.filePath(file))));
                         }
                         previewPainter.end();
                         manifest.insert(QStringLiteral("layers"), entries);
                         preview.save(dir.filePath(QStringLiteral("preview.png")), "PNG");
                         QSaveFile f(dir.filePath(QStringLiteral("parts.json")));
                         if (!f.open(QIODevice::WriteOnly))
                             throw ProviderError(f.errorString());
                         f.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
                         if (!f.commit())
                             throw ProviderError(f.errorString());
                     });
}

} // namespace lumen::ai
