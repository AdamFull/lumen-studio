#pragma once

#include "ai/ProviderRegistry.h"
#include "ai/Types.h"
#include "core/Job.h"
#include "doc/Project.h"

#include <QObject>
#include <QPointer>

#include <functional>
#include <optional>

namespace lumen {
class JobQueue;
}

namespace lumen::ai {

// Region ("only masked") pipeline input: the full composite and a document-sized mask.
struct RegionInput
{
    QImage source;          // full document composite (ARGB32_Premultiplied)
    QImage mask;            // Grayscale8, document-sized, white = regenerate
    int padding = 48;       // context around the mask bounds (document pixels)
    int grow = 6;           // mask dilation before sending (covers seams)
    int feather = 16;       // blend width when compositing back
    bool colorMatch = true; // correct global colour drift of the generated region
};

struct GenerationSpec
{
    QString providerId;     // empty = default image provider
    ImageRequest request;
    std::optional<RegionInput> region;
    // Whole-image changes: the full-size picture the init image was scaled down from. The results
    // come back at its size, with its fine detail where they kept its shapes (img::restoreDetail).
    QImage detailSource;
    QString title;
    QJsonObject meta;       // extra metadata stored with assets (shot id, document id...)
    doc::AssetKind assetKind = doc::AssetKind::Generation;
    bool saveAssets = true;
    QString parentAssetId;
};

struct GenerationOutput
{
    QList<doc::AssetInfo> assets;
    // Full-size results. In region mode these are document-sized layers (transparent outside the mask).
    QList<QImage> images;
    QList<QImage> rawImages; // provider output before compositing
    QRect region;            // region used (region mode)
    QString text;            // optional text returned by the provider (captions, notes)
    QJsonObject info;
};

// Computes the crop region and model-space size for the region pipeline (exposed for tests).
struct RegionPlan
{
    QRect rect;       // document-space crop
    QSize modelSize;  // size sent to the provider
};
RegionPlan planRegion(const QRect &maskBounds, const QSize &documentSize, int padding, const ImageCapabilities &caps);

// Prepares/finishes the region pipeline on worker threads (exposed for tests).
ImageRequest prepareRegionRequest(const ImageRequest &base, const RegionInput &region, const RegionPlan &plan, const ImageCapabilities &caps);
QImage compositeRegionResult(const QImage &generated, const RegionInput &region, const RegionPlan &plan);

// Front door for all image generation. Schedules jobs per provider, runs the region pipeline,
// and stores results as project assets with full generation metadata.
class GenerationService : public QObject
{
    Q_OBJECT
public:
    GenerationService(ProviderRegistry *registry, JobQueue *queue, QObject *parent = nullptr);

    void setProject(doc::Project *project) { m_project = project; }
    ProviderRegistry *registry() const { return m_registry; }
    JobQueue *queue() const { return m_queue; }
    doc::Project *project() const { return m_project; }

    Job *submit(const GenerationSpec &spec);
    // `spec.request.batchCount` results: one job for engines that make several images per request,
    // else one job per image (a fixed seed counts up, so the results differ).
    QList<Job *> submitVariants(const GenerationSpec &spec);
    // Runs a generation inside an existing job (progress, previews, cancellation) and hands the
    // output to `onOutput` instead of storing assets. `used` carries the request values actually sent.
    using OutputHandler = std::function<void(Job *job, const GenerationSpec &used, GenerationOutput output)>;
    void execute(Job *job, const GenerationSpec &spec, OutputHandler onOutput);
    // Queue group (concurrency bucket) of a provider id; empty = default image provider.
    QString groupFor(const QString &providerId) const;
    Job *submitUtility(const QString &providerId, Utility utility, const QImage &image, const QJsonObject &params,
                       const QString &title, bool saveAsset = true);
    // Runs a utility on a project asset; the result is stored as a new asset derived from it.
    Job *submitUtilityForAsset(const QString &providerId, Utility utility, const QString &assetId, const QJsonObject &params,
                               const QString &title);

    // Asks every image provider to free GPU memory between jobs (see ImageProvider::releaseGpu);
    // resolves when all are done. Call before running a local language model after image jobs.
    QFuture<void> releaseGpu();

    // Image provider able to run a utility (preferred provider first, then any other).
    ImageProvider *providerForUtility(Utility utility, const QString &preferredId = QString()) const;

    static GenerationOutput outputOf(const Job *job);
    // Finishes a job the way submit() does: stores the output as assets with the spec's metadata
    // (or only hands it over when `saveAssets` is off). For pipelines of several generations.
    void storeOutput(Job *job, const GenerationSpec &spec, GenerationOutput output);

    // "Safe for work": every request to a provider that takes a negative prompt gets
    // safeNegativeTerms() added to it. Off by default; the Studio follows the setting.
    void setSafeContent(bool on) { m_safeContent = on; }
    bool safeContent() const { return m_safeContent; }
    static QString safeNegativeTerms();
    // `negative` with the safe terms it does not have yet.
    static QString withSafeNegative(const QString &negative);

signals:
    void generationFinished(lumen::Job *job);

private:
    void finishWithAssets(Job *job, const GenerationSpec &spec, GenerationOutput output, ImageProvider *provider);
    Job *submitUtilityImpl(const QString &providerId, Utility utility, std::function<QFuture<QImage>()> image, const QJsonObject &params,
                           const QString &title, bool saveAsset, const QString &sourceAssetId);

    ProviderRegistry *m_registry;
    JobQueue *m_queue;
    bool m_safeContent = false;
    QPointer<doc::Project> m_project;
};

} // namespace lumen::ai

Q_DECLARE_METATYPE(lumen::ai::GenerationOutput)
