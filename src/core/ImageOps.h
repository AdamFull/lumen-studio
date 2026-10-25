#pragma once

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QString>

// Pure image utilities. Thread-safe (no shared state); intended to run on worker threads.
// Masks are QImage::Format_Grayscale8 where 255 = fully selected / to be generated.
namespace lumen::img {

// ---- Codecs ---------------------------------------------------------------------------------
// `quality`: JPEG/WebP quality (0-100), or for PNG the zlib level (0-9; default 1).
QByteArray encode(const QImage &image, const char *format = "PNG", int quality = -1);
QImage decode(const QByteArray &data);
QByteArray toBase64(const QImage &image, const char *format = "PNG", int quality = -1);
QString toDataUrl(const QImage &image, const char *format = "PNG", int quality = -1);
// Accepts raw base64 or a data URL ("data:image/png;base64,....").
QImage fromBase64(const QByteArray &data);
QString mimeForFormat(const char *format);

// Ensures the canonical in-memory pixel format used across the app.
QImage normalized(const QImage &image);

// ---- Masks ----------------------------------------------------------------------------------
QImage createMask(const QSize &size, uchar fill = 0);
// Converts any image to a mask using luminance (white = selected).
QImage maskFromLuminance(const QImage &image);
// Converts the alpha channel to a mask (opaque = selected).
QImage maskFromAlpha(const QImage &image);
// Mask where transparent pixels of the image are selected (OpenAI "edit" convention inverse).
QImage alphaFromMask(const QImage &image, const QImage &mask, bool transparentWhereMasked);
QRect maskBounds(const QImage &mask, int threshold = 1);
bool maskIsEmpty(const QImage &mask, int threshold = 1);
QImage invertMask(const QImage &mask);
QImage thresholdMask(const QImage &mask, int threshold = 128);
// Morphology with a square structuring element of the given radius (O(n) per pixel row/column).
QImage dilateMask(const QImage &mask, int radius);
QImage erodeMask(const QImage &mask, int radius);
// Approximate Gaussian blur (three box passes). Radius ~ feather width in pixels.
QImage blurMask(const QImage &mask, int radius);
// Combines two masks: max (union) or min (intersection) or subtract.
enum class MaskOp { Union, Intersect, Subtract, Replace };
QImage combineMasks(const QImage &a, const QImage &b, MaskOp op);
// Mask coverage ratio [0,1].
double maskCoverage(const QImage &mask);

// ---- Geometry -------------------------------------------------------------------------------
// Expands a rect by padding, optionally grows it to the target aspect (w/h), and clamps to bounds.
QRect expandRegion(const QRect &rect, int padding, const QSize &bounds, double targetAspect = 0.0);
// Picks a size with roughly targetPixels area, the same aspect as `size`, dimensions a multiple of `multiple`.
QSize fitToPixelBudget(const QSize &size, qint64 targetPixels, int multiple = 8);
// Rounds each dimension to the nearest multiple (minimum one multiple).
QSize roundToMultiple(const QSize &size, int multiple);
// Largest size with the given aspect that fits `bounds`.
QSize fitAspect(const QSize &bounds, double aspect);

// ---- Compositing ----------------------------------------------------------------------------
QImage scaled(const QImage &image, const QSize &size, bool smooth = true);
// Returns `image` with alpha multiplied by mask (layer ready, premultiplied ARGB32).
QImage applyMaskToAlpha(const QImage &image, const QImage &mask);
// Draws `overlay` on top of `base` at `pos` using `mask` (same size as overlay) as blend factor.
QImage compositeMasked(const QImage &base, const QImage &overlay, const QImage &mask, const QPoint &pos);
// Linear per-channel colour transfer so `generated` matches `reference` statistics in pixels where
// `mask` < 128 (the unchanged context area). Returns corrected copy of `generated`.
QImage colorMatch(const QImage &generated, const QImage &reference, const QImage &mask);
// Removes smooth colour drift that varies across `generated` (a model redrawing the context of an
// edit paler here, greener there): the difference to `reference` in the context (`mask` < 128) is
// smoothed over `radius` and spread into the masked area, fully at its edge and fading out
// `inner` pixels inside it, so a blended-in edit shows no halo while the edited content keeps the
// model's colours. Returns a corrected copy of `generated`.
QImage matchEdgeColors(const QImage &generated, const QImage &reference, const QImage &mask, int radius, int inner);
// Fills the transparent/new area of an outpaint canvas by smearing edge pixels (gives models context).
QImage fillTransparentFromEdges(const QImage &image);
// Flattens onto a solid colour (removes alpha).
QImage flatten(const QImage &image, const QColor &background);

// Thumbnail with the longest side = maxSide, smooth.
QImage thumbnail(const QImage &image, int maxSide);

} // namespace lumen::img
