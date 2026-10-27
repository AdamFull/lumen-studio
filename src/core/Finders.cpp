#include "core/Finders.h"

#include "core/ImageOps.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace lumen::img {

namespace {

// Brightness 0–255 of the picture over white (transparent parts read as paper).
std::vector<uchar> luminance(const QImage &image, int *w, int *h)
{
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    *w = src.width();
    *h = src.height();
    std::vector<uchar> out(size_t(*w) * *h);
    for (int y = 0; y < *h; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < *w; ++x) {
            const QRgb p = line[x];
            const int a = qAlpha(p);
            const int l = (qRed(p) * 299 + qGreen(p) * 587 + qBlue(p) * 114) / 1000;
            out[size_t(y) * *w + x] = uchar((l * a + 255 * (255 - a)) / 255);
        }
    }
    return out;
}

// The brightest (or darkest) channel of each pixel, over white where transparent. A black line over
// saturated red differs little from it in luminance (red is dark there) but a lot in its brightest
// channel; a white scratch over blue sky likewise in its darkest.
std::vector<uchar> extremeChannel(const QImage &image, bool brightest)
{
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    std::vector<uchar> out(size_t(src.width()) * src.height());
    for (int y = 0; y < src.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const QRgb p = line[x];
            const int a = qAlpha(p);
            const int v = brightest ? std::max({qRed(p), qGreen(p), qBlue(p)}) : std::min({qRed(p), qGreen(p), qBlue(p)});
            out[size_t(y) * src.width() + x] = uchar((v * a + 255 * (255 - a)) / 255);
        }
    }
    return out;
}

// The maximum (or minimum) over a window of 2 r + 1 along one line, in O(n) with a monotonic queue.
void runningExtreme(const uchar *in, uchar *out, int n, int stride, int r, bool max)
{
    std::deque<int> q;
    auto better = [max](uchar a, uchar b) { return max ? a >= b : a <= b; };
    int next = 0;
    for (int i = 0; i < n; ++i) {
        const int hi = std::min(n - 1, i + r);
        for (; next <= hi; ++next) {
            while (!q.empty() && better(in[next * stride], in[q.back() * stride]))
                q.pop_back();
            q.push_back(next);
        }
        while (q.front() < i - r)
            q.pop_front();
        out[i * stride] = in[q.front() * stride];
    }
}

// A square grayscale dilation (max) or erosion (min) of radius r.
std::vector<uchar> squareFilter(const std::vector<uchar> &in, int w, int h, int r, bool max)
{
    std::vector<uchar> tmp(in.size());
    std::vector<uchar> out(in.size());
    for (int y = 0; y < h; ++y)
        runningExtreme(in.data() + size_t(y) * w, tmp.data() + size_t(y) * w, w, 1, r, max);
    for (int x = 0; x < w; ++x)
        runningExtreme(tmp.data() + x, out.data() + x, h, w, r, max);
    return out;
}

// How much darker (dark) or lighter than the ground estimated over radius r each pixel is.
std::vector<uchar> hat(const std::vector<uchar> &l, int w, int h, int r, bool dark)
{
    // Closing (max, then min) fills dark strokes thinner than the window; opening fills light ones.
    const std::vector<uchar> ground = squareFilter(squareFilter(l, w, h, r, dark), w, h, r, !dark);
    std::vector<uchar> out(l.size());
    for (size_t i = 0; i < l.size(); ++i)
        out[i] = uchar(dark ? std::max(0, ground[i] - l[i]) : std::max(0, l[i] - ground[i]));
    return out;
}

struct Piece
{
    int minX, minY, maxX, maxY;
    int area = 0;
};

// The 8-connected pieces of a binary map; `label` gets each pixel's piece (−1 = background).
std::vector<Piece> pieces(const std::vector<uchar> &on, int w, int h, std::vector<int> &label)
{
    label.assign(on.size(), -1);
    std::vector<Piece> out;
    std::vector<int> stack;
    for (int start = 0; start < int(on.size()); ++start) {
        if (!on[start] || label[start] >= 0)
            continue;
        const int id = int(out.size());
        Piece p{start % w, start / w, start % w, start / w, 0};
        stack.assign(1, start);
        label[start] = id;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % w;
            const int y = i / w;
            ++p.area;
            p.minX = std::min(p.minX, x);
            p.maxX = std::max(p.maxX, x);
            p.minY = std::min(p.minY, y);
            p.maxY = std::max(p.maxY, y);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if ((dx || dy) && nx >= 0 && ny >= 0 && nx < w && ny < h) {
                        const int j = ny * w + nx;
                        if (on[j] && label[j] < 0) {
                            label[j] = id;
                            stack.push_back(j);
                        }
                    }
                }
            }
        }
        out.push_back(p);
    }
    return out;
}

QImage toMask(const std::vector<uchar> &on, int w, int h, int grow)
{
    QImage mask = createMask(QSize(w, h));
    for (int y = 0; y < h; ++y) {
        uchar *line = mask.scanLine(y);
        for (int x = 0; x < w; ++x)
            line[x] = on[size_t(y) * w + x] ? 255 : 0;
    }
    return grow > 0 ? dilateMask(mask, grow) : mask;
}

// sRGB → CIE Lab (D65).
void toLab(int r8, int g8, int b8, double *L, double *a, double *b)
{
    static const std::vector<double> linear = [] {
        std::vector<double> t(256);
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return t;
    }();
    const double r = linear[r8], g = linear[g8], bl = linear[b8];
    const double x = (0.4124 * r + 0.3576 * g + 0.1805 * bl) / 0.95047;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * bl;
    const double z = (0.0193 * r + 0.1192 * g + 0.9505 * bl) / 1.08883;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
    *L = 116 * f(y) - 16;
    *a = 500 * (f(x) - f(y));
    *b = 200 * (f(y) - f(z));
}

// Hue (degrees), saturation and lightness (0–1), HSL.
void toHsl(int r, int g, int b, double *hue, double *sat, double *light)
{
    const int mx = std::max({r, g, b});
    const int mn = std::min({r, g, b});
    *light = (mx + mn) / 510.0;
    const double d = (mx - mn) / 255.0;
    *sat = d == 0 ? 0 : d / (1 - std::abs(2 * *light - 1));
    if (d == 0) {
        *hue = 0;
        return;
    }
    double h;
    if (mx == r)
        h = std::fmod((g - b) / 255.0 / d, 6.0);
    else if (mx == g)
        h = (b - r) / 255.0 / d + 2;
    else
        h = (r - g) / 255.0 / d + 4;
    h *= 60;
    *hue = h < 0 ? h + 360 : h;
}

double hueDistance(double a, double b)
{
    const double d = std::abs(a - b);
    return std::min(d, 360 - d);
}

const QHash<QString, QString> &colorWords()
{
    static const QHash<QString, QString> words{
        {QStringLiteral("red"), QStringLiteral("red")},       {QStringLiteral("rot"), QStringLiteral("red")},
        {QStringLiteral("rote"), QStringLiteral("red")},      {QStringLiteral("roten"), QStringLiteral("red")},
        {QStringLiteral("orange"), QStringLiteral("orange")}, {QStringLiteral("yellow"), QStringLiteral("yellow")},
        {QStringLiteral("gelb"), QStringLiteral("yellow")},   {QStringLiteral("gelbe"), QStringLiteral("yellow")},
        {QStringLiteral("green"), QStringLiteral("green")},   {QStringLiteral("grün"), QStringLiteral("green")},
        {QStringLiteral("grüne"), QStringLiteral("green")},   {QStringLiteral("cyan"), QStringLiteral("cyan")},
        {QStringLiteral("turquoise"), QStringLiteral("cyan")}, {QStringLiteral("türkis"), QStringLiteral("cyan")},
        {QStringLiteral("blue"), QStringLiteral("blue")},     {QStringLiteral("blau"), QStringLiteral("blue")},
        {QStringLiteral("blaue"), QStringLiteral("blue")},    {QStringLiteral("purple"), QStringLiteral("purple")},
        {QStringLiteral("violet"), QStringLiteral("purple")}, {QStringLiteral("lila"), QStringLiteral("purple")},
        {QStringLiteral("pink"), QStringLiteral("pink")},     {QStringLiteral("rosa"), QStringLiteral("pink")},
        {QStringLiteral("magenta"), QStringLiteral("pink")},  {QStringLiteral("brown"), QStringLiteral("brown")},
        {QStringLiteral("braun"), QStringLiteral("brown")},   {QStringLiteral("braune"), QStringLiteral("brown")},
        {QStringLiteral("black"), QStringLiteral("black")},   {QStringLiteral("schwarz"), QStringLiteral("black")},
        {QStringLiteral("schwarze"), QStringLiteral("black")}, {QStringLiteral("schwarzen"), QStringLiteral("black")},
        {QStringLiteral("white"), QStringLiteral("white")},   {QStringLiteral("weiß"), QStringLiteral("white")},
        {QStringLiteral("weiss"), QStringLiteral("white")},   {QStringLiteral("weiße"), QStringLiteral("white")},
        {QStringLiteral("weißen"), QStringLiteral("white")},  {QStringLiteral("gray"), QStringLiteral("gray")},
        {QStringLiteral("grey"), QStringLiteral("gray")},     {QStringLiteral("grau"), QStringLiteral("gray")},
        {QStringLiteral("graue"), QStringLiteral("gray")},
    };
    return words;
}

} // namespace

QImage findStrokes(const QImage &image, const StrokeOptions &o)
{
    if (image.isNull())
        return {};
    int w = 0, h = 0;
    const std::vector<uchar> l = luminance(image, &w, &h);
    // A window just wider than the widest stroke (2 r + 1 > maxWidth), so wider shapes stay.
    const int r = std::max(1, std::max(1, o.maxWidth) / 2 + 1);
    const int threshold = std::max(1, o.contrast);
    std::vector<uchar> on(l.size(), 0);
    // In luminance, and in the brightest channel for dark strokes (the darkest for light ones): a
    // black line over red is found by the second. In either, a stroke differs from its ground by the
    // contrast, or it is at most half as bright as the ground (half as dark, for light strokes) and
    // differs by a third of the contrast: a black line in a dark room.
    for (const std::vector<uchar> &values : {l, extremeChannel(image, o.dark)}) {
        const std::vector<uchar> ground = squareFilter(squareFilter(values, w, h, r, o.dark), w, h, r, !o.dark);
        for (size_t i = 0; i < on.size(); ++i) {
            const int v = o.dark ? values[i] : 255 - values[i];
            const int g = o.dark ? ground[i] : 255 - ground[i];
            const int diff = g - v;
            if (diff >= threshold || (2 * v <= g && 3 * diff >= threshold))
                on[i] = 1;
        }
    }
    std::vector<int> label;
    const std::vector<Piece> found = pieces(on, w, h, label);
    const int minLength = o.minLength > 0 ? o.minLength : 4 * std::max(1, o.maxWidth);
    std::vector<char> keep(found.size());
    for (size_t i = 0; i < found.size(); ++i) {
        const Piece &p = found[i];
        keep[i] = std::max(p.maxX - p.minX, p.maxY - p.minY) + 1 >= minLength;
    }
    for (size_t i = 0; i < on.size(); ++i)
        on[i] = label[i] >= 0 && keep[size_t(label[i])];
    return toMask(on, w, h, o.grow);
}

QImage findSpecks(const QImage &image, const SpeckOptions &o)
{
    if (image.isNull())
        return {};
    int w = 0, h = 0;
    const std::vector<uchar> l = luminance(image, &w, &h);
    const int size = std::max(1, o.maxSize);
    const int r = std::max(1, size / 2 + 1);
    const int threshold = std::max(1, o.contrast);
    std::vector<uchar> on(l.size(), 0);
    if (o.tone != SpeckOptions::Tone::Light) {
        const std::vector<uchar> contrast = hat(l, w, h, r, true);
        for (size_t i = 0; i < on.size(); ++i)
            on[i] = contrast[i] >= threshold;
    }
    if (o.tone != SpeckOptions::Tone::Dark) {
        const std::vector<uchar> contrast = hat(l, w, h, r, false);
        // Paper caught between dark marks reads as "lighter than its ground" too; with dark specks
        // searched as well, light ones next to dark ones are not specks.
        std::vector<uchar> nearDark;
        if (o.tone == SpeckOptions::Tone::Both)
            nearDark = squareFilter(on, w, h, r, true);
        for (size_t i = 0; i < on.size(); ++i) {
            if (contrast[i] >= threshold && (nearDark.empty() || !nearDark[i]))
                on[i] = 1;
        }
    }
    std::vector<int> label;
    const std::vector<Piece> found = pieces(on, w, h, label);
    std::vector<char> keep(found.size());
    for (size_t i = 0; i < found.size(); ++i) {
        const Piece &p = found[i];
        keep[i] = std::max(p.maxX - p.minX, p.maxY - p.minY) + 1 <= size && p.area <= size * size;
    }
    for (size_t i = 0; i < on.size(); ++i)
        on[i] = label[i] >= 0 && keep[size_t(label[i])];
    return toMask(on, w, h, o.grow);
}

QString colorNameOf(const QString &word)
{
    return colorWords().value(word.toLower());
}

QImage findColorName(const QImage &image, const QString &nameIn, int tolerance)
{
    const QString name = colorNameOf(nameIn).isEmpty() ? nameIn.toLower() : colorNameOf(nameIn);
    // Hue centre and half width (degrees) of the colours with a hue.
    static const QHash<QString, std::pair<double, double>> hues{
        {QStringLiteral("red"), {0, 18}},     {QStringLiteral("orange"), {30, 14}}, {QStringLiteral("yellow"), {55, 14}},
        {QStringLiteral("green"), {120, 45}}, {QStringLiteral("cyan"), {185, 18}},  {QStringLiteral("blue"), {225, 32}},
        {QStringLiteral("purple"), {275, 22}}, {QStringLiteral("pink"), {320, 25}}, {QStringLiteral("brown"), {28, 22}},
    };
    const bool achromatic = name == QLatin1String("black") || name == QLatin1String("white") || name == QLatin1String("gray");
    if (image.isNull() || (!achromatic && !hues.contains(name)))
        return {};
    const double t = std::clamp(tolerance, 0, 100) / 50.0; // 1 = the usual range
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    QImage mask = createMask(src.size());
    for (int y = 0; y < src.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *out = mask.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const QRgb p = line[x];
            if (qAlpha(p) < 128)
                continue;
            double hue, sat, light;
            toHsl(qRed(p), qGreen(p), qBlue(p), &hue, &sat, &light);
            bool in = false;
            if (name == QLatin1String("black"))
                in = light < 0.12 + 0.08 * t;
            else if (name == QLatin1String("white"))
                in = light > 0.92 - 0.07 * t && sat < 0.15 + 0.15 * t;
            else if (name == QLatin1String("gray"))
                in = sat < 0.06 + 0.08 * t && light > 0.15 && light < 0.85;
            else {
                const auto [centre, half] = hues.value(name);
                const bool chroma = sat > 0.32 - 0.12 * t && light > 0.06 && light < 0.96;
                in = chroma && hueDistance(hue, centre) <= half * (0.6 + 0.4 * t);
                // Brown is a dark orange; orange and pink are not their dark and pale shades.
                if (name == QLatin1String("brown"))
                    in = in && light < 0.42 + 0.06 * t;
                else if (name == QLatin1String("orange"))
                    in = in && light >= 0.38;
                else if (name == QLatin1String("pink"))
                    in = in || (chroma && hueDistance(hue, 350) <= 15 && light > 0.68);
            }
            out[x] = in ? 255 : 0;
        }
    }
    return mask;
}

QImage findColor(const QImage &image, const QColor &color, int tolerance)
{
    if (image.isNull() || !color.isValid())
        return {};
    double tl, ta, tb;
    toLab(color.red(), color.green(), color.blue(), &tl, &ta, &tb);
    // ΔE up to the tolerance is selected, then a soft edge over a quarter more.
    const double inner = std::max(1.0, double(std::clamp(tolerance, 0, 100)));
    const double outer = inner * 1.25 + 1;
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    QImage mask = createMask(src.size());
    for (int y = 0; y < src.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *out = mask.scanLine(y);
        for (int x = 0; x < src.width(); ++x) {
            const QRgb p = line[x];
            if (qAlpha(p) < 128)
                continue;
            double l, a, b;
            toLab(qRed(p), qGreen(p), qBlue(p), &l, &a, &b);
            const double d = std::sqrt((l - tl) * (l - tl) + (a - ta) * (a - ta) + (b - tb) * (b - tb));
            out[x] = d <= inner ? 255 : d >= outer ? 0 : uchar(std::lround(255 * (outer - d) / (outer - inner)));
        }
    }
    return mask;
}

FinderRequest parseFinderRequest(const QString &description)
{
    static const QRegularExpression split(QStringLiteral("[^\\p{L}\\p{N}]+"), QRegularExpression::UseUnicodePropertiesOption);
    const QStringList words = description.toLower().split(split, Qt::SkipEmptyParts);
    auto has = [&words](std::initializer_list<const char *> list) {
        for (const char *w : list) {
            if (words.contains(QString::fromUtf8(w)))
                return true;
        }
        return false;
    };
    FinderRequest r;
    const bool light = has({"white", "light", "bright", "pale", "weiß", "weiss", "weiße", "weißen", "helle", "hellen", "hell"});
    const bool dark = has({"black", "dark", "schwarz", "schwarze", "schwarzen", "dunkle", "dunklen", "dunkel"});
    const bool thin = has({"thin", "fine", "hairline", "dünn", "dünne", "dünnen", "feine", "feinen"});
    const bool thick = has({"thick", "bold", "wide", "heavy", "dick", "dicke", "dicken", "breite", "breiten"});
    if (has({"line", "lines", "stroke", "strokes", "scratch", "scratches", "crack", "cracks", "hair", "hairs", "wire", "wires",
             "linie", "linien", "strich", "striche", "kratzer", "riss", "risse"})) {
        r.kind = FinderRequest::Kind::Strokes;
        r.strokes.dark = !(light && !dark);
        r.strokes.maxWidth = thin ? 3 : thick ? 12 : 6;
        return r;
    }
    if (has({"dust", "speck", "specks", "spot", "spots", "dot", "dots", "noise", "staub", "fleck", "flecken", "punkte", "sprenkel"})) {
        r.kind = FinderRequest::Kind::Specks;
        r.specks.tone = light && !dark ? SpeckOptions::Tone::Light : dark && !light ? SpeckOptions::Tone::Dark : SpeckOptions::Tone::Both;
        r.specks.maxSize = thick ? 12 : thin ? 3 : 6;
        return r;
    }
    for (const QString &w : words) {
        const QString c = colorNameOf(w);
        if (!c.isEmpty()) {
            r.kind = FinderRequest::Kind::Color;
            r.color = c;
            return r;
        }
    }
    return r;
}

QImage runFinder(const QImage &image, const FinderRequest &r)
{
    switch (r.kind) {
    case FinderRequest::Kind::Strokes: return findStrokes(image, r.strokes);
    case FinderRequest::Kind::Specks: return findSpecks(image, r.specks);
    case FinderRequest::Kind::Color: return findColorName(image, r.color, r.tolerance);
    case FinderRequest::Kind::None: break;
    }
    return {};
}

} // namespace lumen::img
