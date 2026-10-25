#pragma once

#include <QImage>
#include <QList>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>

// Pixel operations of the classic filter nodes. Pure functions, safe on worker threads. Images
// come in any format and are returned as ARGB32_Premultiplied; colour adjustments work on
// unpremultiplied colours and keep alpha, neighbourhood filters work on premultiplied pixels (no
// dark fringes at transparent edges) with the image edge extended. Masks are Grayscale8.
namespace lumen::filters::ops {

// Returns true when the work should stop; the result is then incomplete and is thrown away.
using Cancel = std::function<bool()>;

enum class Channels { All, Red, Green, Blue };

// ---- Colour -----------------------------------------------------------------------------------
QImage invert(const QImage &image);
enum class GreyMethod { Luminosity, Average, Lightness };
QImage desaturate(const QImage &image, GreyMethod method);
// White where the luminosity (0.299 R + 0.587 G + 0.114 B) is at least `level`, else black.
QImage threshold(const QImage &image, int level);
// `levels` evenly spaced values per channel (2 = black or full).
QImage posterize(const QImage &image, int levels);
// Maps [inBlack, inWhite] to [outBlack, outWhite] with a gamma for the midtones (> 1 brightens).
QImage levels(const QImage &image, Channels channels, double inBlack, double inWhite, double gamma,
              double outBlack, double outWhite);
// A smooth curve through the points (input, output in 0..255) that does not overshoot between
// them (monotone cubic); flat before the first point and after the last.
QImage curves(const QImage &image, Channels channels, const QList<QPointF> &points);
// "x y, x y, ..." <-> points; fewer than two valid points give the identity line.
QList<QPointF> parseCurve(const QString &text);
QString curveText(const QList<QPointF> &points);
// The 256-entry table a curve gives (for the node editor's curve widget and the tests).
QList<int> curveTable(const QList<QPointF> &points);
// brightness, contrast in -100..100. Brightness moves towards white or black; contrast stretches
// around mid grey (-100: everything grey, 100: nearly a threshold).
QImage brightnessContrast(const QImage &image, double brightness, double contrast);
// hue in degrees; saturation and lightness in -100..100 (-100: grey / black, 100: full / white).
QImage hueSaturation(const QImage &image, double hue, double saturation, double lightness);
enum class Tones { Shadows, Midtones, Highlights };
// Shifts one tone range towards red / green / blue (positive) or cyan / magenta / yellow
// (negative), each in -100..100; with keepBrightness the HSL lightness of each pixel stays.
QImage colorBalance(const QImage &image, Tones tones, double cyanRed, double magentaGreen, double yellowBlue,
                    bool keepBrightness);

// ---- Blur, sharpen, noise ---------------------------------------------------------------------
// Gaussian with standard deviation `radius` px: an exact kernel for small radii, three box
// passes for large ones. Keeps the mean and the symmetry of the image.
QImage gaussianBlur(const QImage &image, double radius, const Cancel &cancel = {});
// Each channel plus amount times its difference to the 4-neighbour mean (amount 1 = 100 %).
QImage sharpen(const QImage &image, double amount);
// Each channel plus amount times its difference to the blurred image, where that difference is
// at least `threshold` levels.
QImage unsharpMask(const QImage &image, double radius, double amount, int threshold, const Cancel &cancel = {});
// Adds noise of `amount` (0..1 of the full range); the same seed always gives the same noise at
// the same size. Gaussian noise has a standard deviation of half the amount; uniform noise spans
// plus or minus the amount.
QImage addNoise(const QImage &image, double amount, quint32 seed, bool monochrome, bool gaussian);
// Median of each channel over a (2 radius + 1) square.
QImage median(const QImage &image, int radius, const Cancel &cancel = {});

// ---- Combine ----------------------------------------------------------------------------------
// The layer blend modes of doc::BlendMode, in its order and with the same QPainter composition
// modes as doc::flatten (the filters library does not depend on doc).
QStringList blendModeIds();
QPainter::CompositionMode compositionMode(const QString &blendModeId);
// `top` over `base` with a blend mode and opacity (0..1), only where `mask` is white (optional).
// `top` and `mask` are stretched to the base's size.
QImage blend(const QImage &base, const QImage &top, const QString &mode, double opacity, const QImage &mask = QImage());
// `outside` where the mask is black, `inside` where it is white, mixed in between.
QImage mix(const QImage &outside, const QImage &inside, const QImage &mask);

// ---- Masks ------------------------------------------------------------------------------------
// Grows (grow > 0) or shrinks (grow < 0) by whole pixels, then softens the edge by `feather` px.
QImage growShrinkFeather(const QImage &mask, int grow, int feather);

// ---- Geometry ---------------------------------------------------------------------------------
QImage resize(const QImage &image, const QSize &size, bool smooth);
QImage crop(const QImage &image, const QRect &rect); // null when the rect misses the image
QImage flip(const QImage &image, Qt::Orientations orientations);
// Clockwise by `degrees`; the canvas grows to hold the corners (transparent). Multiples of 90
// are exact.
QImage rotate(const QImage &image, double degrees, bool smooth);

} // namespace lumen::filters::ops
