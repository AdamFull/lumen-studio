#include "ai/GenerationService.h"

#include "ai/Gate.h"
#include "ai/PromptStyle.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/Log.h"

#include <QJsonArray>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>

#include <cmath>

namespace lumen::ai {

namespace {

constexpr int kMinRegionSide = 384;

double aspectOf(const QSize &s)
{
    return s.isEmpty() ? 1.0 : double(s.width()) / s.height();
}

double ratioValue(const QString &r)
{
    const QStringList parts = r.split(u':');
    return parts.size() == 2 ? parts[0].toDouble() / std::max(1e-6, parts[1].toDouble()) : 1.0;
}

QRect ensureMinimumSize(QRect r, const QSize &bounds, int minSide)
{
    const int w = std::min(bounds.width(), std::max(r.width(), minSide));
    const int h = std::min(bounds.height(), std::max(r.height(), minSide));
    const QPoint c = r.center();
    r = QRect(c.x() - w / 2, c.y() - h / 2, w, h);
    if (r.left() < 0)
        r.moveLeft(0);
    if (r.top() < 0)
        r.moveTop(0);
    if (r.right() >= bounds.width())
        r.moveRight(bounds.width() - 1);
    if (r.bottom() >= bounds.height())
        r.moveBottom(bounds.height() - 1);
    return r.intersected(QRect(QPoint(0, 0), bounds));
}

} // namespace

RegionPlan planRegion(const QRect &maskBounds, const QSize &documentSize, int padding, const ImageCapabilities &caps)
{
    RegionPlan plan;
    QRect grown = ensureMinimumSize(maskBounds.adjusted(-padding, -padding, padding, padding), documentSize, kMinRegionSide);
    double targetAspect = 0.0;
    QSize fixed;
    if (!caps.fixedSizes.isEmpty()) {
        double best = 1e9;
        for (const QSize &s : caps.fixedSizes) {
            const double d = std::abs(std::log(aspectOf(s) / aspectOf(grown.size())));
            if (d < best) {
                best = d;
                fixed = s;
            }
        }
        targetAspect = aspectOf(fixed);
    } else if (!caps.aspectRatios.isEmpty()) {
        double best = 1e9;
        for (const QString &r : caps.aspectRatios) {
            const double d = std::abs(std::log(ratioValue(r) / aspectOf(grown.size())));
            if (d < best) {
                best = d;
                targetAspect = ratioValue(r);
            }
        }
    }
    plan.rect = img::expandRegion(grown, 0, documentSize, targetAspect);
    if (fixed.isValid()) {
        plan.modelSize = fixed;
    } else {
        const qint64 pixels = std::min<qint64>(caps.nativePixels, caps.maxPixels);
        // Upscale small regions (more detail) but never more than 4x per side.
        QSize s = img::fitToPixelBudget(plan.rect.size(), pixels, std::max(1, caps.sizeMultiple));
        const double scale = double(s.width()) / std::max(1, plan.rect.width());
        if (scale > 4.0)
            s = img::roundToMultiple(plan.rect.size() * 4.0, std::max(1, caps.sizeMultiple));
        plan.modelSize = s;
    }
    return plan;
}

ImageRequest prepareRegionRequest(const ImageRequest &base, const RegionInput &region, const RegionPlan &plan, const ImageCapabilities &caps)
{
    ImageRequest req = base;
    QImage crop = region.source.copy(plan.rect);
    // Outpainting: transparent areas get plausible colour so the model sees continuous context.
    bool hasTransparency = false;
    if (crop.hasAlphaChannel()) {
        const QImage alpha = img::maskFromAlpha(crop);
        hasTransparency = !img::maskIsEmpty(img::invertMask(alpha), 1);
    }
    if (hasTransparency)
        crop = img::fillTransparentFromEdges(crop);
    crop = img::flatten(crop, Qt::white);
    QImage maskCrop = img::dilateMask(region.mask.copy(plan.rect), region.grow);
    req.initImage = img::normalized(img::scaled(crop, plan.modelSize, true));
    req.mask = img::thresholdMask(img::scaled(maskCrop, plan.modelSize, true), 128);
    req.size = plan.modelSize;
    if (caps.inpaint || caps.edit)
        req.mode = GenMode::Inpaint;
    else
        req.mode = GenMode::ImageToImage;
    // Control maps are document-sized: crop them to the same region.
    for (ControlInput &c : req.controls) {
        if (c.image.size() == region.source.size())
            c.image = img::scaled(c.image.copy(plan.rect), plan.modelSize, true);
    }
    return req;
}

QImage compositeRegionResult(const QImage &generated, const RegionInput &region, const RegionPlan &plan)
{
    QImage g = img::normalized(img::scaled(generated, plan.rect.size(), true));
    const QImage maskCrop = region.mask.copy(plan.rect);
    if (region.colorMatch)
        g = img::normalized(img::colorMatch(g, region.source.copy(plan.rect), img::dilateMask(maskCrop, region.grow + region.feather)));
    const QImage blend = img::blurMask(img::dilateMask(maskCrop, region.grow + region.feather / 2), region.feather);
    QImage layer(region.source.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    QPainter p(&layer);
    p.drawImage(plan.rect.topLeft(), img::applyMaskToAlpha(g, blend));
    p.end();
    return layer;
}

// ---- service ------------------------------------------------------------------------------------

GenerationService::GenerationService(ProviderRegistry *registry, JobQueue *queue, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
    , m_queue(queue)
{
    qRegisterMetaType<GenerationOutput>();
}

GenerationOutput GenerationService::outputOf(const Job *job)
{
    return job ? job->result().value<GenerationOutput>() : GenerationOutput();
}

ImageProvider *GenerationService::providerForUtility(Utility utility, const QString &preferredId) const
{
    if (ImageProvider *p = m_registry->imageProvider(preferredId); p && p->supportsUtility(utility))
        return p;
    if (ImageProvider *p = m_registry->defaultImageProvider(); p && p->supportsUtility(utility))
        return p;
    for (ImageProvider *p : m_registry->imageProviders()) {
        if (p->supportsUtility(utility))
            return p;
    }
    return nullptr;
}

QString GenerationService::safeNegativeTerms()
{
    return QStringLiteral("nsfw, nude, nudity, explicit, cleavage, underwear, lingerie");
}

QString GenerationService::withSafeNegative(const QString &negative)
{
    QStringList parts;
    const QString trimmed = negative.trimmed();
    if (!trimmed.isEmpty())
        parts << trimmed;
    for (const QString &term : safeNegativeTerms().split(QStringLiteral(", "))) {
        static const QRegularExpression separators(QStringLiteral("[,\\n]"));
        bool present = false;
        for (const QString &t : trimmed.split(separators))
            present = present || t.trimmed().compare(term, Qt::CaseInsensitive) == 0;
        if (!present)
            parts << term;
    }
    return parts.join(QStringLiteral(", "));
}

Job *GenerationService::submit(const GenerationSpec &specIn)
{
    ImageProvider *provider = specIn.providerId.isEmpty() ? m_registry->defaultImageProvider() : m_registry->imageProvider(specIn.providerId);
    const QString group = provider ? provider->instanceId() : QStringLiteral("none");
    const QString title = specIn.title.isEmpty() ? specIn.request.prompt.left(60) : specIn.title;
    QPointer<GenerationService> self(this);
    GenerationSpec spec = specIn;
    if (provider)
        spec.providerId = provider->instanceId();

    Job *job = m_queue->submit(title.isEmpty() ? tr("Image generation") : title, group, [self, spec](Job *job) {
        if (!self) {
            job->fail(tr("The generation service is no longer available"));
            return;
        }
        self->execute(job, spec, [self](Job *job, const GenerationSpec &used, GenerationOutput out) {
            if (!self) {
                job->succeed(QVariant::fromValue(out));
                return;
            }
            self->finishWithAssets(job, used, out, self->m_registry->imageProvider(used.providerId));
        });
    }, QStringLiteral("generation"));
    job->metadata().insert(QStringLiteral("prompt"), spec.request.prompt);
    job->metadata().insert(QStringLiteral("provider"), provider ? provider->displayName() : QString());
    for (auto it = spec.meta.begin(); it != spec.meta.end(); ++it)
        job->metadata().insert(it.key(), it.value().toVariant());
    connect(job, &Job::finished, this, &GenerationService::generationFinished);
    return job;
}

QString GenerationService::groupFor(const QString &providerId) const
{
    ImageProvider *provider = providerId.isEmpty() ? m_registry->defaultImageProvider() : m_registry->imageProvider(providerId);
    return provider ? provider->instanceId() : QStringLiteral("none");
}

void GenerationService::execute(Job *job, const GenerationSpec &specIn, OutputHandler onOutput)
{
    ImageProvider *provider = specIn.providerId.isEmpty() ? m_registry->defaultImageProvider() : m_registry->imageProvider(specIn.providerId);
    if (!provider) {
        job->fail(tr("No image provider is configured. Add one in Settings → Providers."));
        return;
    }
    QPointer<ImageProvider> providerGuard(provider);
    GenerationSpec spec = specIn;
    spec.providerId = provider->instanceId();
    const ImageCapabilities caps = provider->capabilitiesFor(specIn.request);
    if (m_safeContent && caps.negativePrompt)
        spec.request.negativePrompt = withSafeNegative(spec.request.negativePrompt);
    if (m_safeContent)
        spec.request.prompt = promptStyleFor(provider).withSafeTag(spec.request.prompt); // Anima: "safe"
    job->setProgress(-1.0, tr("Preparing..."));

    auto launch = [job, providerGuard, spec, onOutput](ImageRequest request, std::optional<RegionPlan> plan) {
        if (!providerGuard) {
            job->fail(tr("The provider is no longer available"));
            return;
        }
        ImageOperation *op = gatedGenerate(providerGuard, request);
        job->setCancelHandler([op = QPointer<ImageOperation>(op), job] {
            if (op)
                op->cancel();
            else
                job->reportCancelled();
        });
        QObject::connect(op, &Operation::progress, job, [job](double f, const QString &s) { job->setProgress(f, s); });
        QObject::connect(op, &Operation::preview, job, [job](const QImage &img) { job->setPreview(img); });
        GenerationSpec used = spec;
        used.request.mode = request.mode;
        used.request.denoise = request.denoise;
        used.request.seed = request.seed;
        QObject::connect(op, &Operation::finished, job, [job, op, spec = used, request, plan, onOutput] {
            if (op->state() == Operation::State::Cancelled) {
                job->reportCancelled();
                return;
            }
            if (op->state() == Operation::State::Failed) {
                job->fail(op->errorString());
                return;
            }
            const ImageResult result = op->result();
            job->metadata().insert(QStringLiteral("request"), request.describe());
            if (!spec.region) {
                GenerationOutput out;
                for (const GeneratedImage &g : result.images) {
                    out.images << g.image;
                    out.rawImages << g.image;
                }
                out.text = result.text;
                out.info = result.info;
                onOutput(job, spec, out);
                return;
            }
            job->setProgress(-1.0, tr("Blending result..."));
            const RegionInput region = *spec.region;
            const RegionPlan rp = *plan;
            async::run([result, region, rp] {
                GenerationOutput out;
                out.region = rp.rect;
                out.text = result.text;
                out.info = result.info;
                for (const GeneratedImage &g : result.images) {
                    out.rawImages << g.image;
                    out.images << compositeRegionResult(g.image, region, rp);
                }
                return out;
            }).then(job, [job, spec, onOutput](GenerationOutput out) {
                onOutput(job, spec, out);
            }).onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
        });
    };

    if (!spec.region) {
        ImageRequest req = spec.request;
        if (req.mode == GenMode::Inpaint && !caps.inpaint && !caps.edit)
            req.mode = GenMode::ImageToImage;
        launch(req, std::nullopt);
        return;
    }
    const RegionInput region = *spec.region;
    const ImageRequest base = spec.request;
    async::run([region, base, caps] {
        const QRect bounds = img::maskBounds(region.mask, 8);
        if (bounds.isEmpty())
            throw ProviderError(QObject::tr("The selection is empty - paint a mask or make a selection first."));
        const RegionPlan plan = planRegion(bounds, region.source.size(), region.padding, caps);
        return std::make_pair(prepareRegionRequest(base, region, plan, caps), plan);
    })
        .then(job, [launch](std::pair<ImageRequest, RegionPlan> prepared) { launch(prepared.first, prepared.second); })
        .onFailed(job, [job](const std::exception &e) { job->fail(describeException(e)); });
}

void GenerationService::finishWithAssets(Job *job, const GenerationSpec &spec, GenerationOutput output, ImageProvider *provider)
{
    if (output.images.isEmpty()) {
        job->fail(output.text.isEmpty() ? tr("The provider returned no images") : output.text);
        return;
    }
    if (!spec.saveAssets || !m_project || !m_project->isOpen()) {
        job->succeed(QVariant::fromValue(output));
        return;
    }
    job->setProgress(-1.0, tr("Saving..."));
    const ImageRequest &r = spec.request;
    const QString model = r.model.isEmpty() && provider ? provider->text(QStringLiteral("model")) : r.model;
    QJsonObject baseMeta = r.describe();
    baseMeta.insert(QStringLiteral("provider"), provider ? provider->displayName() : QString());
    baseMeta.insert(QStringLiteral("provider_type"), provider ? provider->type().id : QString());
    if (!model.isEmpty())
        baseMeta.insert(QStringLiteral("model"), model);
    for (auto it = spec.meta.begin(); it != spec.meta.end(); ++it)
        baseMeta.insert(it.key(), it.value());

    struct Pending
    {
        int remaining;
        GenerationOutput output;
        QStringList errors;
    };
    auto pending = std::make_shared<Pending>(Pending{int(output.images.size()), output, {}});
    pending->output.assets.resize(output.images.size());
    const QImage source = spec.region ? spec.region->source : QImage();
    for (int i = 0; i < output.images.size(); ++i) {
        QJsonObject meta = baseMeta;
        const QImage rawImage = output.rawImages.value(i);
        meta.insert(QStringLiteral("parameters"),
                    buildInfoText(r.prompt, r.negativePrompt, r.steps, r.sampler, r.scheduler, r.cfg, r.seed,
                                  rawImage.isNull() ? output.images[i].width() : rawImage.width(),
                                  rawImage.isNull() ? output.images[i].height() : rawImage.height(), model,
                                  r.mode == GenMode::TextToImage ? -1.0 : r.denoise));
        const QImage layer = output.images[i];
        QFuture<QImage> image = spec.region ? async::run([source, layer] { return img::compositeMasked(source, layer, img::createMask(layer.size(), 255), QPoint(0, 0)); })
                                            : QtFuture::makeReadyValueFuture(layer);
        QPointer<doc::Project> project = m_project;
        const QString name = spec.title.isEmpty() ? r.prompt.left(48) : spec.title;
        image.then(job, [project, job, pending, meta, name, spec, i](QImage full) {
                if (!project)
                    throw ProviderError(QObject::tr("Project closed"));
                return project->addAsset(full, spec.assetKind, name, meta, spec.parentAssetId);
            })
            .unwrap()
            .then(job, [job, pending, i](doc::AssetInfo info) {
                pending->output.assets[i] = info;
                if (--pending->remaining == 0)
                    job->succeed(QVariant::fromValue(pending->output));
            })
            .onFailed(job, [job, pending](const std::exception &e) {
                pending->errors << describeException(e);
                // Images are still delivered even if storing them failed.
                if (--pending->remaining == 0) {
                    qCWarning(lcAi) << "Saving generated assets failed:" << pending->errors;
                    job->succeed(QVariant::fromValue(pending->output));
                }
            });
    }
}

QFuture<void> GenerationService::releaseGpu()
{
    QList<QFuture<void>> futures;
    for (Provider *p : m_registry->providers()) {
        if (auto *image = qobject_cast<ImageProvider *>(p))
            futures << image->releaseGpu();
    }
    if (futures.isEmpty())
        return QtFuture::makeReadyVoidFuture();
    return QtFuture::whenAll(futures.begin(), futures.end()).then([](const QList<QFuture<void>> &) {});
}

Job *GenerationService::submitUtility(const QString &providerId, Utility utility, const QImage &image, const QJsonObject &params,
                                      const QString &title, bool saveAsset)
{
    return submitUtilityImpl(providerId, utility, [image] { return QtFuture::makeReadyValueFuture(image); }, params, title, saveAsset,
                             QString());
}

Job *GenerationService::submitUtilityForAsset(const QString &providerId, Utility utility, const QString &assetId, const QJsonObject &params,
                                              const QString &title)
{
    QPointer<doc::Project> project = m_project;
    auto load = [project, assetId]() -> QFuture<QImage> {
        if (!project)
            return QtFuture::makeExceptionalFuture<QImage>(std::make_exception_ptr(ProviderError(tr("No project is open."))));
        return project->loadImage(assetId);
    };
    return submitUtilityImpl(providerId, utility, load, params, title, true, assetId);
}

Job *GenerationService::submitUtilityImpl(const QString &providerId, Utility utility, std::function<QFuture<QImage>()> image,
                                          const QJsonObject &params, const QString &title, bool saveAsset, const QString &sourceAssetId)
{
    ImageProvider *provider = providerForUtility(utility, providerId);
    QPointer<ImageProvider> guard(provider);
    QPointer<GenerationService> self(this);
    const QString group = provider ? provider->instanceId() : QStringLiteral("none");
    Job *job = m_queue->submit(title, group, [self, guard, utility, image, params, title, saveAsset, sourceAssetId](Job *job) {
        if (!guard) {
            job->fail(tr("No configured provider supports %1.").arg(utilityName(utility)));
            return;
        }
        auto run = [self, guard, utility, params, title, saveAsset, sourceAssetId, job](const QImage &input) {
            if (!guard) {
                job->fail(tr("The provider was removed."));
                return;
            }
            ImageOperation *op = gatedUtility(guard, utility, input, params);
            job->setCancelHandler([op = QPointer<ImageOperation>(op), job] {
                if (op)
                    op->cancel();
                else
                    job->reportCancelled();
            });
            QObject::connect(op, &Operation::progress, job, [job](double f, const QString &s) { job->setProgress(f, s); });
            QObject::connect(op, &Operation::finished, job, [self, job, op, guard, utility, title, saveAsset, sourceAssetId] {
                if (op->state() == Operation::State::Cancelled) {
                    job->reportCancelled();
                    return;
                }
                if (op->state() == Operation::State::Failed) {
                    job->fail(op->errorString());
                    return;
                }
                GenerationOutput out;
                for (const GeneratedImage &g : op->result().images) {
                    out.images << g.image;
                    out.rawImages << g.image;
                }
                out.text = op->result().text;
                out.info = op->result().info;
                if (utility == Utility::Caption || !saveAsset || !self) {
                    job->succeed(QVariant::fromValue(out));
                    return;
                }
                GenerationSpec spec;
                spec.title = title;
                spec.assetKind = utility == Utility::DetectPose ? doc::AssetKind::Pose : doc::AssetKind::Edit;
                spec.meta = QJsonObject{{QStringLiteral("utility"), utilityName(utility)}};
                if (!sourceAssetId.isEmpty()) {
                    spec.meta.insert(QStringLiteral("source"), sourceAssetId);
                    spec.parentAssetId = sourceAssetId;
                }
                spec.request.mode = GenMode::ImageToImage;
                spec.request.prompt = title;
                spec.request.steps = 0;
                spec.request.cfg = 0;
                self->finishWithAssets(job, spec, out, guard);
            });
        };
        job->setProgress(-1.0, tr("Loading..."));
        image()
            .then(job, [job, run](QImage input) {
                if (job->isFinished())
                    return;
                if (input.isNull()) {
                    job->fail(tr("The image could not be loaded."));
                    return;
                }
                run(input);
            })
            .onFailed(job, [job](const std::exception &e) {
                if (!job->isFinished())
                    job->fail(QString::fromUtf8(e.what()));
            });
    }, QStringLiteral("utility"));
    connect(job, &Job::finished, this, &GenerationService::generationFinished);
    return job;
}

} // namespace lumen::ai
