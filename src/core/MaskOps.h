#pragma once

#include <QImage>
#include <QLineF>
#include <QList>
#include <QPolygonF>
#include <QRect>
#include <QVector>

#include <vector>

// Mask analysis and label-map utilities used by part slicing. Thread-safe pure functions.
// Masks are Format_Grayscale8 (255 = inside). A *label map* is a Grayscale8 image whose pixel
// value is a part label (0 = unassigned), so part masks are mutually exclusive by construction.
namespace lumen::img {

// ---- Connected components ---------------------------------------------------------------------
struct Components
{
    QSize size;
    std::vector<int> labels; // per pixel, 0 = background, 1..count = component
    int count = 0;
    std::vector<qint64> areas;  // index 1..count (index 0 unused)
    std::vector<QRect> bounds;  // index 1..count
};

Components connectedComponents(const QImage &mask, int threshold = 128, bool eightConnected = true);
// One mask per island, sorted by area (largest first); islands smaller than minArea are dropped.
QList<QImage> splitComponents(const QImage &mask, qint64 minArea = 0, int threshold = 128);
QImage removeSmallIslands(const QImage &mask, qint64 minArea, int threshold = 128);
// Fills enclosed holes up to maxArea pixels (maxArea < 0: all enclosed holes).
QImage fillHoles(const QImage &mask, qint64 maxArea = -1, int threshold = 128);
// Blur + threshold: rounds jagged edges and removes speckles smaller than ~radius.
QImage smoothMask(const QImage &mask, int radius);

// ---- Splitting --------------------------------------------------------------------------------
// Pixels left of x (image coordinates) go to the first mask, the rest to the second.
QPair<QImage, QImage> splitByVerticalLine(const QImage &mask, double x);
// Cuts the mask along a polyline (ends are extended past the image so a short stroke across a limb
// cuts it fully). Returns the resulting pieces, largest first; a single entry means the line did
// not separate anything.
QList<QImage> splitByCutLine(const QImage &mask, const QPolygonF &line, qint64 minArea = 16);
// Assigns every mask pixel to the nearest segment; returns one mask per segment (possibly empty).
QList<QImage> splitByNearestSegment(const QImage &mask, const QList<QLineF> &segments);
// Pixels of `mask` grouped by segment groups: each group is a list of segment indices.
QList<QImage> splitByNearestSegmentGroups(const QImage &mask, const QList<QLineF> &segments,
                                          const QList<int> &segmentGroup, int groupCount);

// Colour-similarity region (magic wand). contiguous = flood fill from the seed, otherwise global.
// `limit` (optional mask) restricts the result.
QImage colorRegion(const QImage &image, const QPoint &seed, int tolerance, bool contiguous,
                   const QImage &limit = QImage());

// ---- Label maps -------------------------------------------------------------------------------
QImage maskForLabel(const QImage &labels, int label, const QRect &rect = QRect());
QVector<qint64> labelHistogram(const QImage &labels); // 256 entries
QRect labelBounds(const QImage &labels, int label);
void replaceLabel(QImage &labels, int from, int to);
// Writes `label` where mask >= threshold (inside rect of the mask's coordinates, mask doc-sized).
void paintLabel(QImage &labels, const QImage &mask, int label, int threshold = 128);
// Builds a label map from masks; later masks win where they overlap. `limit` clips (optional).
QImage labelsFromMasks(const QSize &size, const QList<QImage> &masks, const QList<int> &labelIds,
                       const QImage &limit = QImage(), int threshold = 128);
// ARGB32_Premultiplied rendering of a label-map region with a premultiplied palette (256 entries).
QImage colorizeLabels(const QImage &labels, const QVector<QRgb> &palette, const QRect &rect);
// Outline of label boundaries (alpha 255 on pixels whose 4-neighbour has another label) in rect.
QImage labelEdges(const QImage &labels, const QRect &rect, QRgb color);

// ---- Layer stacks -----------------------------------------------------------------------------
// Visible masks of stacked RGBA layers (index 0 = bottom): a pixel belongs to the top-most layer
// whose alpha is >= alphaThreshold.
QList<QImage> visibleMasksFromLayers(const QList<QImage> &layers, int alphaThreshold = 64);

// Region where a part may continue behind the parts in front of it:
// own ∪ (dilate(own, radius) ∩ occluders).
QImage extensionMask(const QImage &own, const QImage &occluders, int radius);
// Growth radius proportional to the part size (sqrt of area), clamped to [8, 160].
int adaptiveExtensionRadius(qint64 area);

// Character silhouette: alpha when the image has transparency, otherwise everything that is not
// connected to the border through near-background colours.
QImage silhouetteMask(const QImage &image, int tolerance = 24);

} // namespace lumen::img
