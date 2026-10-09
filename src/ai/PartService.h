#pragma once

#include "ai/GenerationService.h"
#include "ai/PartPipeline.h"

#include <QObject>
#include <QPointer>

namespace lumen {
class JobQueue;
}

namespace lumen::ai {

enum class CompletionStrategy {
    Inpaint,       // generate the hidden areas with an image provider (region pipeline)
    Decomposition, // use the layers of the last model decomposition (e.g. see-through)
    CutOnly        // visible pixels only, no generation
};
QString completionStrategyName(CompletionStrategy s);
CompletionStrategy completionStrategyFromName(const QString &name);

struct CompletionOptions
{
    CompletionStrategy strategy = CompletionStrategy::Inpaint;
    QString providerId;            // image provider (Inpaint)
    QString characterSummary;      // appended to every prompt
    QString negativePrompt = QStringLiteral("background scenery, other body parts, text, watermark, frame, shadow");
    int variants = 1;
    int steps = 28;
    double cfg = 6.0;
    double denoise = 1.0;
    QString model;
    bool refineAlpha = false;      // run background removal on the result when a provider offers it
};

struct ExportOptions
{
    bool includeCutOuts = true;    // parts without a generated result are exported as cut-outs
    bool includeSource = true;     // hidden "Original" reference layer at the bottom (PSD)
};

// Orchestrates character part slicing: segmentation (provider utility or skeleton), per-part
// completion jobs, result storage and export. All heavy pixel work runs on worker threads; the
// part set is only touched on the GUI thread.
class PartService : public QObject
{
    Q_OBJECT
public:
    PartService(ProviderRegistry *registry, JobQueue *queue, GenerationService *generation, QObject *parent = nullptr);

    // Image providers that can segment parts.
    QList<ImageProvider *> segmenters() const;

    Job *segment(doc::PartSet *set, const QString &providerId, const parts::SegmentOptions &options);
    // Local segmentation from a body skeleton (no model needed).
    Job *segmentWithSkeleton(doc::PartSet *set, const pose::Person &skeleton, const parts::SegmentOptions &options);
    // One job per part; parts without pixels are skipped.
    QList<Job *> complete(doc::PartSet *set, const QList<int> &labels, const CompletionOptions &options);

    Job *exportPsd(doc::PartSet *set, const QString &path, const ExportOptions &options);
    Job *exportPngFolder(doc::PartSet *set, const QString &directory, const ExportOptions &options);

    // Loads the chosen result images of every part into the set's cache (asynchronously).
    void loadResults(doc::PartSet *set);

signals:
    void segmentationApplied(doc::PartSet *set, const QStringList &created, const QStringList &missing);

private:
    void applySegmentation(Job *job, QPointer<doc::PartSet> set, QList<parts::SegmentLayer> layers,
                           parts::SegmentOptions options, QJsonObject decompositionInfo);
    void storeResult(Job *job, QPointer<doc::PartSet> set, int label, QString partId, QImage layer, QJsonObject meta,
                     bool choose = true);
    Job *runExport(doc::PartSet *set, const QString &title, const ExportOptions &options,
                   std::function<void(const QList<parts::ExportLayer> &, const doc::PartSet::Snapshot &, const QImage &)> write);

    ProviderRegistry *m_registry;
    JobQueue *m_queue;
    GenerationService *m_generation;
};

} // namespace lumen::ai
