#pragma once

#include "core/Psd.h"
#include "doc/Parts.h"
#include "pose/Skeleton.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QLineF>
#include <QList>

// Pure, thread-safe building blocks of character part slicing (segmentation post-processing,
// skeleton splits, occluded-area completion and export). Used by PartService on worker threads.
namespace lumen::ai::parts {

// ---- Segmentation -------------------------------------------------------------------------------

struct SegmentLayer
{
    QString label;
    QString side;                          // "", "L" or "R" (skeleton splits)
    QString kind = QStringLiteral("mask"); // "layer" (RGBA) or "mask" (Grayscale8)
    QImage image;                          // canvas-sized
    int order = -1;                        // back-to-front index from the model (-1 = unknown)
    double depth = -1.0;                   // 0 back .. 1 front (-1 = unknown)
};

enum class SegmentScope {
    ReplaceAll,     // new masks for everything (locked parts are preserved)
    SelectedKinds,  // only the requested kinds are replaced
    FillUnassigned  // new masks only claim pixels that have no part yet
};

struct SegmentOptions
{
    SegmentScope scope = SegmentScope::ReplaceAll;
    QStringList kinds;      // requested taxonomy tags (SelectedKinds scope; also sent to the segmenter)
    qint64 minIsland = 0;   // 0 = automatic (0.01% of the canvas, at least 24 px)
    bool clipToSilhouette = true;
    QString provenance;     // e.g. "auto:Lumen Part Server"
};

struct SegmentOutcome
{
    QImage labels;
    QList<doc::Part> parts;
    QStringList created;   // display names of new parts
    QStringList unmatched; // model labels kept as custom parts
    QStringList empty;     // requested kinds that were not found
};

// Canonical spelling of model / layer labels ("Front_Hair" -> "front hair").
QString normalizePartLabel(const QString &label);
// Splits a trailing side marker from a layer name ("Upper arm L" -> {"Upper arm", "L"}).
std::pair<QString, QString> splitSide(const QString &name);
// Visible pixel layers of a PSD as segmentation layers (bottom to top). Hidden layers and a layer
// named "Original" are skipped (counted in `skipped`).
QList<SegmentLayer> layersFromPsd(const psd::File &file, int *skipped = nullptr);

// Merges segmenter output into the current part set (see SegmentScope). Heavy; worker thread.
SegmentOutcome mergeSegmentation(const doc::PartSet::Snapshot &current, const QImage &silhouette,
                                 const doc::PartTaxonomy &taxonomy, const QList<SegmentLayer> &layers,
                                 const SegmentOptions &options);

// ---- Skeleton -----------------------------------------------------------------------------------

struct BoneGroup
{
    QString tag;   // taxonomy kind
    QString side;  // "", "L" or "R"
    QString name;  // display name
    QList<QLineF> bones;
};

// Bone groups of a body skeleton for the given taxonomy ("simple": head/body/arms/legs;
// "live2d": head/topwear/upper arm/forearm/hand/thigh/shin/footwear, sided).
QList<BoneGroup> skeletonGroups(const pose::Person &person, const QString &taxonomyId);
// Limb-only groups used to split an existing part (e.g. "arms" -> upper arm/forearm/hand L/R).
QList<BoneGroup> limbGroups(const pose::Person &person, const QString &taxonomyId, bool arms, bool legs);
// Partitions `mask` among the groups (nearest bone). Returns one mask per group (may be empty).
QList<QImage> splitBySkeleton(const QImage &mask, const QList<BoneGroup> &groups);
// Fits a front-facing A-pose skeleton to the character's silhouette (starting point for editing).
pose::Person fitSkeleton(const QImage &silhouette);

// ---- Completion ---------------------------------------------------------------------------------

struct CompletionContext
{
    QImage image;   // source pixels on the visible part, key colour elsewhere (ARGB32_Premultiplied)
    QImage mask;    // area to generate (extension minus visible, slightly grown into the visible edge)
    QColor key;     // background key colour
    QRect bounds;   // extension bounds
};

// Picks a flat background colour far from the colours of the part.
QColor chooseKeyColor(const QImage &source, const QImage &visible);
QString colorName(const QColor &c);
CompletionContext buildCompletionContext(const QImage &source, const QImage &visible, const QImage &extension);
// Turns a generated full-canvas image (context with the generated area) into the part layer:
// visible pixels from the source, generated pixels inside the extension with the key colour removed.
QImage extractCompletedPart(const QImage &generated, const QImage &source, const QImage &visible,
                            const QImage &extension, const QColor &key);
// Same from a transparent generated layer (LayerDiffuse): the model's alpha replaces colour keying.
QImage extractTransparentPart(const QImage &generatedLayer, const QImage &source, const QImage &visible, const QImage &extension);
bool hasTransparency(const QImage &image);
// Visible pixels only.
QImage cutOutPart(const QImage &source, const QImage &visible);
// A decomposition layer limited to the part's extension, with the visible pixels taken from the source.
QImage partFromLayer(const QImage &layer, const QImage &source, const QImage &visible, const QImage &extension);
// Crops a canvas-sized layer to its opaque content; returns the rect (empty when fully transparent).
QRect cropToContent(QImage &layer);

// Prompt for completing a part (natural language; tags style handled by the caller).
QString completionPrompt(const doc::Part &part, const doc::PartKind *kind, const QString &characterSummary,
                         const QColor &key);

// ---- Expression variants -------------------------------------------------------------------------

// Alternative drawings of facial parts for Live2D blink / lip-sync (exported as hidden layers).
struct ExpressionKind
{
    QString id;          // "eyes_closed", "eyes_smile", "mouth_open", "mouth_o", "mouth_smile"
    QString name;        // display / layer name
    QString tag;         // tag of the variant part (the puppet preview swaps layers by tag)
    QStringList baseTags; // parts whose area is repainted
    QString prompt;
};
const QList<ExpressionKind> &expressionKinds();
const ExpressionKind *findExpression(const QString &id);
// Area to repaint: the base parts grown proportionally to their size, clipped to the silhouette.
QImage expressionRegion(const QImage &labels, const QList<int> &baseLabels, const QImage &silhouette);
QString expressionPrompt(const ExpressionKind &kind, const QString &characterSummary);

// ---- Export ---------------------------------------------------------------------------------------

struct ExportLayer
{
    QString name;
    QString tag;
    QString side;
    QString group;
    int order = 0;
    double depth = 0.5;
    bool visible = true;
    QImage image;   // cropped
    QRect rect;     // canvas rect of `image`
};

// Collects the export stack (back to front). Parts without a result are cut out from the source.
QList<ExportLayer> exportLayers(const doc::PartSet::Snapshot &set, const QImage &source,
                                const QHash<QString, QImage> &results, bool includeCutOuts);
QJsonObject exportManifest(const QList<ExportLayer> &layers, const QSize &canvas, const QString &name);

} // namespace lumen::ai::parts
