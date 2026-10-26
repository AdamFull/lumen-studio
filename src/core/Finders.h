#pragma once

#include <QColor>
#include <QImage>
#include <QString>

// Finding an area by what it looks like, without a model (plan M18 §18.1): thin lines and scratches,
// specks of dust, and colours. Each returns a Grayscale8 mask of the image's size (255 = found), like
// the masks of core/ImageOps.
namespace lumen::img {

// Thin strokes darker (or lighter) than what is around them: pen lines, scratches, cracks, hairs.
// A grayscale closing (opening for light strokes) over `maxWidth` estimates the paper under them;
// where the picture differs from it by `contrast` (0–255) or more, and the piece is at least
// `minLength` long, it is a stroke. Larger dark shapes (a black coat, a shadow) are left out, since
// the closing keeps them. `grow` widens the result to cover the strokes' soft edges.
struct StrokeOptions
{
    bool dark = true;      // dark strokes on lighter ground; false = light strokes on darker ground
    int maxWidth = 6;      // the widest stroke, in pixels
    int contrast = 40;     // how much darker (lighter) than the ground
    int minLength = 0;     // the shortest piece kept; 0 = 4 × maxWidth
    int grow = 1;
};
QImage findStrokes(const QImage &image, const StrokeOptions &options = {});

// Small isolated spots (dust, specks, noise) darker or lighter than what is around them, no larger
// than `maxSize` pixels across.
struct SpeckOptions
{
    enum class Tone { Dark, Light, Both };
    Tone tone = Tone::Both;
    int maxSize = 6;
    int contrast = 40;
    int grow = 1;
};
QImage findSpecks(const QImage &image, const SpeckOptions &options = {});

// The pixels of a named colour ("red", "green", "white", "gray", …; also a few German names) by hue,
// saturation and lightness ranges, so dark and light shades count; `tolerance` (0–100) widens the
// range. Empty for a name it does not know.
QImage findColorName(const QImage &image, const QString &name, int tolerance = 50);
// The pixels close to `color` (distance in CIE Lab, `tolerance` 0–100), with a soft edge.
QImage findColor(const QImage &image, const QColor &color, int tolerance = 25);
// Whether `name` is a colour findColorName knows (in its canonical English form, else empty).
QString colorNameOf(const QString &word);

// What a description asks to select, for the finders above (a small rule-based reading: "the
// black lines", "white scratches", "dust", "the red", in English and some German). Kind None when it
// asks for something only a model can find ("the cup").
struct FinderRequest
{
    enum class Kind { None, Strokes, Specks, Color };
    Kind kind = Kind::None;
    StrokeOptions strokes;
    SpeckOptions specks;
    QString color;         // canonical colour name for Kind::Color
    int tolerance = 50;
};
FinderRequest parseFinderRequest(const QString &description);
// Runs the finder a request names; a null image for Kind::None.
QImage runFinder(const QImage &image, const FinderRequest &request);

} // namespace lumen::img
