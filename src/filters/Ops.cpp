#include "filters/Ops.h"

#include "core/ImageOps.h"

#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace lumen::filters::ops {

namespace {

using Lut = std::array<int, 256>;

int clampByte(double v)
{
    return static_cast<int>(std::clamp(std::lround(v), 0L, 255L));
}

Lut identityLut()
{
    Lut lut{};
    for (int i = 0; i < 256; ++i)
        lut[static_cast<size_t>(i)] = i;
    return lut;
}

// Calls f(r, g, b) on every pixel's unpremultiplied colour; alpha stays.
template<typename F>
QImage mapColors(const QImage &image, F &&f)
{
    if (image.isNull())
        return image;
    QImage out = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < out.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            int r = qRed(row[x]);
            int g = qGreen(row[x]);
            int b = qBlue(row[x]);
            f(r, g, b);
            row[x] = qRgba(r, g, b, qAlpha(row[x]));
        }
    }
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage applyLut(const QImage &image, Channels channels, const Lut &lut)
{
    const Lut id = identityLut();
    const Lut &r = channels == Channels::All || channels == Channels::Red ? lut : id;
    const Lut &g = channels == Channels::All || channels == Channels::Green ? lut : id;
    const Lut &b = channels == Channels::All || channels == Channels::Blue ? lut : id;
    return mapColors(image, [&](int &cr, int &cg, int &cb) {
        cr = r[static_cast<size_t>(cr)];
        cg = g[static_cast<size_t>(cg)];
        cb = b[static_cast<size_t>(cb)];
    });
}

// HSL with every component in 0..1 except the hue (degrees).
void toHsl(double r, double g, double b, double &h, double &s, double &l)
{
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    l = (mx + mn) / 2.0;
    const double d = mx - mn;
    if (d <= 0.0) {
        h = 0.0;
        s = 0.0;
        return;
    }
    s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r)
        h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g)
        h = (b - r) / d + 2.0;
    else
        h = (r - g) / d + 4.0;
    h *= 60.0;
}

void fromHsl(double h, double s, double l, double &r, double &g, double &b)
{
    h = std::fmod(h, 360.0);
    if (h < 0.0)
        h += 360.0;
    const double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
    const double hp = h / 60.0;
    const double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));
    double r1 = 0, g1 = 0, b1 = 0;
    if (hp < 1.0) {
        r1 = c;
        g1 = x;
    } else if (hp < 2.0) {
        r1 = x;
        g1 = c;
    } else if (hp < 3.0) {
        g1 = c;
        b1 = x;
    } else if (hp < 4.0) {
        g1 = x;
        b1 = c;
    } else if (hp < 5.0) {
        r1 = x;
        b1 = c;
    } else {
        r1 = c;
        b1 = x;
    }
    const double m = l - c / 2.0;
    r = r1 + m;
    g = g1 + m;
    b = b1 + m;
}

double smoothstep(double edge0, double edge1, double x)
{
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ---- Float pixel planes (premultiplied RGBA, 4 floats per pixel) -------------------------------

struct Planes
{
    int w = 0;
    int h = 0;
    std::vector<float> px;
};

Planes toPlanes(const QImage &premultiplied)
{
    Planes p;
    p.w = premultiplied.width();
    p.h = premultiplied.height();
    p.px.resize(static_cast<size_t>(p.w) * p.h * 4);
    for (int y = 0; y < p.h; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(premultiplied.constScanLine(y));
        float *out = p.px.data() + static_cast<size_t>(y) * p.w * 4;
        for (int x = 0; x < p.w; ++x) {
            out[x * 4 + 0] = float(qRed(row[x]));
            out[x * 4 + 1] = float(qGreen(row[x]));
            out[x * 4 + 2] = float(qBlue(row[x]));
            out[x * 4 + 3] = float(qAlpha(row[x]));
        }
    }
    return p;
}

QImage fromPlanes(const Planes &p)
{
    QImage out(p.w, p.h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < p.h; ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        const float *in = p.px.data() + static_cast<size_t>(y) * p.w * 4;
        for (int x = 0; x < p.w; ++x) {
            const int a = clampByte(in[x * 4 + 3]);
            auto c = [&](int i) { return std::min(clampByte(in[x * 4 + i]), a); };
            row[x] = qRgba(c(0), c(1), c(2), a);
        }
    }
    return out;
}

// Calls fn(in, out, n) for every row (horizontal) or column of the planes: `in` holds the line's n
// pixels with `pad` copies of the edge pixel before and after, `out` receives the n new pixels.
template<typename F>
bool forEachLine(Planes &p, bool horizontal, int pad, const Cancel &cancel, F &&fn)
{
    const int n = horizontal ? p.w : p.h;
    const int lines = horizontal ? p.h : p.w;
    const size_t along = horizontal ? 4 : static_cast<size_t>(p.w) * 4; // floats to the next pixel
    const size_t across = horizontal ? static_cast<size_t>(p.w) * 4 : 4; // floats to the next line
    std::vector<float> in(static_cast<size_t>(n + 2 * pad) * 4);
    std::vector<float> out(static_cast<size_t>(n) * 4);
    for (int line = 0; line < lines; ++line) {
        if (cancel && line % 32 == 0 && cancel())
            return false;
        float *base = p.px.data() + static_cast<size_t>(line) * across;
        for (int i = -pad; i < n + pad; ++i) {
            const float *src = base + static_cast<size_t>(std::clamp(i, 0, n - 1)) * along;
            std::copy(src, src + 4, in.data() + static_cast<size_t>(i + pad) * 4);
        }
        fn(in.data(), out.data(), n);
        for (int i = 0; i < n; ++i)
            std::copy(out.data() + static_cast<size_t>(i) * 4, out.data() + static_cast<size_t>(i) * 4 + 4,
                      base + static_cast<size_t>(i) * along);
    }
    return true;
}

bool convolve(Planes &p, const std::vector<double> &kernel, const Cancel &cancel)
{
    const int r = static_cast<int>(kernel.size() / 2);
    auto pass = [&](const float *in, float *out, int n) {
        for (int i = 0; i < n; ++i) {
            double acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < kernel.size(); ++k) {
                const float *s = in + (static_cast<size_t>(i) + k) * 4;
                for (int c = 0; c < 4; ++c)
                    acc[c] += kernel[k] * s[c];
            }
            for (int c = 0; c < 4; ++c)
                out[static_cast<size_t>(i) * 4 + c] = float(acc[c]);
        }
    };
    return forEachLine(p, true, r, cancel, pass) && forEachLine(p, false, r, cancel, pass);
}

bool boxBlur(Planes &p, int r, const Cancel &cancel)
{
    if (r <= 0)
        return true;
    const double norm = 1.0 / (2 * r + 1);
    auto pass = [&](const float *in, float *out, int n) {
        double acc[4] = {0, 0, 0, 0};
        for (int k = 0; k <= 2 * r; ++k)
            for (int c = 0; c < 4; ++c)
                acc[c] += in[k * 4 + c];
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 4; ++c)
                out[static_cast<size_t>(i) * 4 + c] = float(acc[c] * norm);
            if (i + 1 < n) {
                for (int c = 0; c < 4; ++c)
                    acc[c] += in[static_cast<size_t>(i + 2 * r + 1) * 4 + c] - in[static_cast<size_t>(i) * 4 + c];
            }
        }
    };
    return forEachLine(p, true, r, cancel, pass) && forEachLine(p, false, r, cancel, pass);
}

// Widths of three box blurs whose result is close to a Gaussian (Kutskir, "Fastest Gaussian blur").
std::array<int, 3> boxWidths(double sigma)
{
    constexpr int n = 3;
    const double ideal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = static_cast<int>(std::floor(ideal));
    if (wl % 2 == 0)
        --wl;
    const int wu = wl + 2;
    const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    const int m = static_cast<int>(std::lround(mIdeal));
    return {0 < m ? wl : wu, 1 < m ? wl : wu, 2 < m ? wl : wu};
}

bool blurPlanes(Planes &p, double sigma, const Cancel &cancel)
{
    if (sigma < 0.05)
        return true;
    if (sigma <= 3.0) {
        const int r = static_cast<int>(std::ceil(3.0 * sigma));
        std::vector<double> kernel(static_cast<size_t>(2 * r + 1));
        double sum = 0;
        for (int k = -r; k <= r; ++k) {
            const double v = std::exp(-(k * k) / (2.0 * sigma * sigma));
            kernel[static_cast<size_t>(k + r)] = v;
            sum += v;
        }
        for (double &v : kernel)
            v /= sum;
        return convolve(p, kernel, cancel);
    }
    for (int width : boxWidths(sigma)) {
        if (!boxBlur(p, (width - 1) / 2, cancel))
            return false;
    }
    return true;
}

// Stateless hash (lowbias32): the noise of a pixel depends only on the seed and its position.
quint32 mixBits(quint32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double unitNoise(quint32 seed, int x, int y, int channel)
{
    const quint32 h = mixBits(seed * 0x9e3779b9U
                              ^ mixBits(quint32(x) * 0x85ebca6bU ^ mixBits(quint32(y) * 0xc2b2ae35U + quint32(channel))));
    return h / 4294967296.0; // [0, 1)
}

} // namespace

// ---- Colour -----------------------------------------------------------------------------------

QImage invert(const QImage &image)
{
    return mapColors(image, [](int &r, int &g, int &b) {
        r = 255 - r;
        g = 255 - g;
        b = 255 - b;
    });
}

QImage desaturate(const QImage &image, GreyMethod method)
{
    return mapColors(image, [method](int &r, int &g, int &b) {
        int v = 0;
        switch (method) {
        case GreyMethod::Luminosity: v = clampByte(0.299 * r + 0.587 * g + 0.114 * b); break;
        case GreyMethod::Average: v = clampByte((r + g + b) / 3.0); break;
        case GreyMethod::Lightness: v = clampByte((std::max({r, g, b}) + std::min({r, g, b})) / 2.0); break;
        }
        r = g = b = v;
    });
}

QImage threshold(const QImage &image, int level)
{
    return mapColors(image, [level](int &r, int &g, int &b) {
        r = g = b = (299 * r + 587 * g + 114 * b) >= level * 1000 ? 255 : 0;
    });
}

QImage posterize(const QImage &image, int levels)
{
    const int n = std::clamp(levels, 2, 256);
    Lut lut{};
    for (int i = 0; i < 256; ++i)
        lut[static_cast<size_t>(i)] = clampByte(std::round(i * (n - 1) / 255.0) * 255.0 / (n - 1));
    return applyLut(image, Channels::All, lut);
}

QImage levels(const QImage &image, Channels channels, double inBlack, double inWhite, double gamma, double outBlack,
              double outWhite)
{
    Lut lut{};
    for (int i = 0; i < 256; ++i) {
        double v = inWhite > inBlack ? std::clamp((i - inBlack) / (inWhite - inBlack), 0.0, 1.0) : (i >= inBlack ? 1.0 : 0.0);
        if (gamma > 0.0 && gamma != 1.0)
            v = std::pow(v, 1.0 / gamma);
        lut[static_cast<size_t>(i)] = clampByte(outBlack + v * (outWhite - outBlack));
    }
    return applyLut(image, channels, lut);
}

QList<QPointF> parseCurve(const QString &text)
{
    QList<QPointF> points;
    for (const QString &pair : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList xy = pair.simplified().split(QLatin1Char(' '));
        if (xy.size() != 2)
            continue;
        bool okX = false, okY = false;
        const double x = xy[0].toDouble(&okX);
        const double y = xy[1].toDouble(&okY);
        if (okX && okY && std::isfinite(x) && std::isfinite(y))
            points.append({std::clamp(x, 0.0, 255.0), std::clamp(y, 0.0, 255.0)});
    }
    std::stable_sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    QList<QPointF> unique; // one point per input value: the last one given wins
    for (const QPointF &p : points) {
        if (!unique.isEmpty() && unique.last().x() == p.x())
            unique.last() = p;
        else
            unique.append(p);
    }
    if (unique.size() < 2)
        return {{0, 0}, {255, 255}};
    return unique;
}

QString curveText(const QList<QPointF> &points)
{
    QStringList pairs;
    for (const QPointF &p : points)
        pairs << QString::number(p.x()) + QLatin1Char(' ') + QString::number(p.y());
    return pairs.join(QStringLiteral(", "));
}

QList<int> curveTable(const QList<QPointF> &input)
{
    const QList<QPointF> pts = input.size() < 2 ? QList<QPointF>{{0, 0}, {255, 255}} : input;
    const qsizetype n = pts.size();
    // Fritsch-Carlson tangents: a cubic Hermite spline that keeps monotone runs monotone.
    std::vector<double> secant(static_cast<size_t>(n - 1)), tangent(static_cast<size_t>(n));
    for (qsizetype k = 0; k + 1 < n; ++k)
        secant[static_cast<size_t>(k)] = (pts[k + 1].y() - pts[k].y()) / (pts[k + 1].x() - pts[k].x());
    tangent[0] = secant[0];
    tangent[static_cast<size_t>(n - 1)] = secant[static_cast<size_t>(n - 2)];
    for (qsizetype k = 1; k + 1 < n; ++k) {
        const double a = secant[static_cast<size_t>(k - 1)], b = secant[static_cast<size_t>(k)];
        tangent[static_cast<size_t>(k)] = a * b <= 0.0 ? 0.0 : (a + b) / 2.0;
    }
    for (qsizetype k = 0; k + 1 < n; ++k) {
        const double m = secant[static_cast<size_t>(k)];
        if (m == 0.0) {
            tangent[static_cast<size_t>(k)] = tangent[static_cast<size_t>(k + 1)] = 0.0;
            continue;
        }
        const double a = tangent[static_cast<size_t>(k)] / m, b = tangent[static_cast<size_t>(k + 1)] / m;
        const double s = a * a + b * b;
        if (s > 9.0) {
            const double tau = 3.0 / std::sqrt(s);
            tangent[static_cast<size_t>(k)] = tau * a * m;
            tangent[static_cast<size_t>(k + 1)] = tau * b * m;
        }
    }
    QList<int> table(256);
    qsizetype seg = 0;
    for (int i = 0; i < 256; ++i) {
        double y = 0;
        if (i <= pts.first().x()) {
            y = pts.first().y();
        } else if (i >= pts.last().x()) {
            y = pts.last().y();
        } else {
            while (seg + 2 < n && i > pts[seg + 1].x())
                ++seg;
            const double h = pts[seg + 1].x() - pts[seg].x();
            const double t = (i - pts[seg].x()) / h;
            const double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[seg].y() + (t3 - 2 * t2 + t) * h * tangent[static_cast<size_t>(seg)]
              + (-2 * t3 + 3 * t2) * pts[seg + 1].y() + (t3 - t2) * h * tangent[static_cast<size_t>(seg + 1)];
        }
        table[i] = clampByte(y);
    }
    return table;
}

QImage curves(const QImage &image, Channels channels, const QList<QPointF> &points)
{
    const QList<int> table = curveTable(points);
    Lut lut{};
    std::copy(table.begin(), table.end(), lut.begin());
    return applyLut(image, channels, lut);
}

QImage brightnessContrast(const QImage &image, double brightness, double contrast)
{
    const double b = std::clamp(brightness, -100.0, 100.0) / 100.0;
    const double c = std::clamp(contrast, -100.0, 100.0) / 100.0;
    // The contrast slope runs from 0 (flat grey) through 1 to nearly vertical (GIMP's formula).
    const double slope = std::tan((std::min(c, 0.999) + 1.0) * std::numbers::pi / 4.0);
    Lut lut{};
    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        v = b < 0.0 ? v * (1.0 + b) : v + (1.0 - v) * b;
        v = (v - 0.5) * slope + 0.5;
        lut[static_cast<size_t>(i)] = clampByte(v * 255.0);
    }
    return applyLut(image, Channels::All, lut);
}

QImage hueSaturation(const QImage &image, double hue, double saturation, double lightness)
{
    const double sat = std::clamp(saturation, -100.0, 100.0) / 100.0;
    const double light = std::clamp(lightness, -100.0, 100.0) / 100.0;
    return mapColors(image, [&](int &r, int &g, int &b) {
        double h, s, l;
        toHsl(r / 255.0, g / 255.0, b / 255.0, h, s, l);
        h += hue;
        s = std::clamp(s * (1.0 + sat), 0.0, 1.0);
        l = light >= 0.0 ? l + (1.0 - l) * light : l * (1.0 + light);
        double rr, gg, bb;
        fromHsl(h, s, l, rr, gg, bb);
        r = clampByte(rr * 255.0);
        g = clampByte(gg * 255.0);
        b = clampByte(bb * 255.0);
    });
}

QImage colorBalance(const QImage &image, Tones tones, double cyanRed, double magentaGreen, double yellowBlue,
                    bool keepBrightness)
{
    // At full weight, 100 moves a channel by a quarter of the range.
    constexpr double Reach = 0.25;
    const double dr = std::clamp(cyanRed, -100.0, 100.0) / 100.0 * Reach;
    const double dg = std::clamp(magentaGreen, -100.0, 100.0) / 100.0 * Reach;
    const double db = std::clamp(yellowBlue, -100.0, 100.0) / 100.0 * Reach;
    return mapColors(image, [&](int &r, int &g, int &b) {
        const double fr = r / 255.0, fg = g / 255.0, fb = b / 255.0;
        const double l = (std::max({fr, fg, fb}) + std::min({fr, fg, fb})) / 2.0;
        double w = 0;
        switch (tones) {
        case Tones::Shadows: w = 1.0 - smoothstep(0.0, 0.5, l); break;
        case Tones::Midtones: w = 4.0 * l * (1.0 - l); break;
        case Tones::Highlights: w = smoothstep(0.5, 1.0, l); break;
        }
        double nr = std::clamp(fr + w * dr, 0.0, 1.0);
        double ng = std::clamp(fg + w * dg, 0.0, 1.0);
        double nb = std::clamp(fb + w * db, 0.0, 1.0);
        if (keepBrightness && w > 0.0) {
            double h, s, nl;
            toHsl(nr, ng, nb, h, s, nl);
            fromHsl(h, s, l, nr, ng, nb);
        }
        r = clampByte(nr * 255.0);
        g = clampByte(ng * 255.0);
        b = clampByte(nb * 255.0);
    });
}

// ---- Blur, sharpen, noise ---------------------------------------------------------------------

QImage gaussianBlur(const QImage &image, double radius, const Cancel &cancel)
{
    const QImage src = img::normalized(image);
    if (src.isNull() || radius < 0.05)
        return src;
    Planes p = toPlanes(src);
    if (!blurPlanes(p, radius, cancel))
        return src;
    return fromPlanes(p);
}

QImage sharpen(const QImage &image, double amount)
{
    const QImage src = img::normalized(image);
    if (src.isNull() || amount <= 0.0)
        return src;
    const int w = src.width(), h = src.height();
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    auto at = [&](int x, int y) {
        return reinterpret_cast<const QRgb *>(src.constScanLine(std::clamp(y, 0, h - 1)))[std::clamp(x, 0, w - 1)];
    };
    for (int y = 0; y < h; ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = at(x, y), n = at(x, y - 1), s = at(x, y + 1), e = at(x + 1, y), wv = at(x - 1, y);
            const int a = qAlpha(c);
            auto channel = [&](int (*get)(QRgb)) {
                const double mean = (get(n) + get(s) + get(e) + get(wv)) / 4.0;
                return std::min(clampByte(get(c) + amount * (get(c) - mean)), a);
            };
            row[x] = qRgba(channel(qRed), channel(qGreen), channel(qBlue), a);
        }
    }
    return out;
}

QImage unsharpMask(const QImage &image, double radius, double amount, int threshold, const Cancel &cancel)
{
    const QImage src = img::normalized(image);
    if (src.isNull() || radius < 0.05 || amount <= 0.0)
        return src;
    Planes blurred = toPlanes(src);
    if (!blurPlanes(blurred, radius, cancel))
        return src;
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < src.height(); ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const float *soft = blurred.px.data() + static_cast<size_t>(y) * blurred.w * 4;
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const int a = qAlpha(in[x]); // alpha stays
            auto channel = [&](int value, int c) {
                const double diff = value - double(soft[x * 4 + c]);
                return std::abs(diff) >= threshold ? std::min(clampByte(value + amount * diff), a) : value;
            };
            row[x] = qRgba(channel(qRed(in[x]), 0), channel(qGreen(in[x]), 1), channel(qBlue(in[x]), 2), a);
        }
    }
    return out;
}

QImage addNoise(const QImage &image, double amount, quint32 seed, bool monochrome, bool gaussian)
{
    if (image.isNull() || amount <= 0.0)
        return img::normalized(image);
    QImage out = image.convertToFormat(QImage::Format_ARGB32);
    const double reach = std::clamp(amount, 0.0, 1.0) * 255.0;
    auto noise = [&](int x, int y, int channel) {
        if (!gaussian)
            return (unitNoise(seed, x, y, channel) * 2.0 - 1.0) * reach;
        // Box-Muller from two independent values of the hash.
        const double u1 = 1.0 - unitNoise(seed, x, y, channel * 2);
        const double u2 = unitNoise(seed, x, y, channel * 2 + 1);
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2) * reach / 2.0;
    };
    for (int y = 0; y < out.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QRgb p = row[x];
            const double nr = noise(x, y, 0);
            const double ng = monochrome ? nr : noise(x, y, 1);
            const double nb = monochrome ? nr : noise(x, y, 2);
            row[x] = qRgba(clampByte(qRed(p) + nr), clampByte(qGreen(p) + ng), clampByte(qBlue(p) + nb), qAlpha(p));
        }
    }
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage median(const QImage &image, int radius, const Cancel &cancel)
{
    const QImage src = img::normalized(image);
    if (src.isNull() || radius <= 0)
        return src;
    const int w = src.width(), h = src.height();
    const int window = 2 * radius + 1;
    const int half = window * window / 2 + 1; // the median is the half-th smallest value
    // A histogram per channel, with the current median and the number of values below it, so
    // sliding the window moves the median by a few steps instead of searching 256 bins.
    struct Histogram
    {
        std::array<int, 256> bins{};
        int median = 0;
        int below = 0;
    };
    auto at = [&](int x, int y) {
        return reinterpret_cast<const QRgb *>(src.constScanLine(std::clamp(y, 0, h - 1)))[std::clamp(x, 0, w - 1)];
    };
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        if (cancel && y % 16 == 0 && cancel())
            return src;
        std::array<Histogram, 4> hist{};
        auto change = [&](QRgb p, int delta) {
            for (int c = 0; c < 4; ++c) {
                const int v = int((p >> (8 * c)) & 0xff);
                Histogram &hh = hist[static_cast<size_t>(c)];
                hh.bins[static_cast<size_t>(v)] += delta;
                if (v < hh.median)
                    hh.below += delta;
            }
        };
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx)
                change(at(dx, y + dy), 1);
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            if (x > 0) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    change(at(x - radius - 1, y + dy), -1);
                    change(at(x + radius, y + dy), 1);
                }
            }
            QRgb result = 0;
            for (int c = 0; c < 4; ++c) {
                Histogram &hh = hist[static_cast<size_t>(c)];
                while (hh.below >= half) {
                    --hh.median;
                    hh.below -= hh.bins[static_cast<size_t>(hh.median)];
                }
                while (hh.below + hh.bins[static_cast<size_t>(hh.median)] < half) {
                    hh.below += hh.bins[static_cast<size_t>(hh.median)];
                    ++hh.median;
                }
                result |= QRgb(hh.median) << (8 * c);
            }
            row[x] = result;
        }
    }
    return out;
}

// ---- Combine ----------------------------------------------------------------------------------

QStringList blendModeIds()
{
    return {QStringLiteral("normal"),      QStringLiteral("multiply"),   QStringLiteral("screen"),
            QStringLiteral("overlay"),     QStringLiteral("darken"),     QStringLiteral("lighten"),
            QStringLiteral("color-dodge"), QStringLiteral("color-burn"), QStringLiteral("hard-light"),
            QStringLiteral("soft-light"),  QStringLiteral("difference"), QStringLiteral("exclusion"),
            QStringLiteral("add")};
}

QPainter::CompositionMode compositionMode(const QString &blendModeId)
{
    // The same table as doc::toCompositionMode, in doc::BlendMode's order.
    static const QPainter::CompositionMode modes[] = {
        QPainter::CompositionMode_SourceOver, QPainter::CompositionMode_Multiply,   QPainter::CompositionMode_Screen,
        QPainter::CompositionMode_Overlay,    QPainter::CompositionMode_Darken,     QPainter::CompositionMode_Lighten,
        QPainter::CompositionMode_ColorDodge, QPainter::CompositionMode_ColorBurn,  QPainter::CompositionMode_HardLight,
        QPainter::CompositionMode_SoftLight,  QPainter::CompositionMode_Difference, QPainter::CompositionMode_Exclusion,
        QPainter::CompositionMode_Plus};
    const qsizetype i = blendModeIds().indexOf(blendModeId);
    return i >= 0 ? modes[i] : QPainter::CompositionMode_SourceOver;
}

QImage blend(const QImage &base, const QImage &top, const QString &mode, double opacity, const QImage &mask)
{
    QImage out = img::normalized(base);
    if (out.isNull() || top.isNull())
        return out;
    QImage layer = img::normalized(img::scaled(top, out.size(), true));
    if (!mask.isNull())
        layer = img::applyMaskToAlpha(layer, mask);
    QPainter p(&out);
    p.setOpacity(std::clamp(opacity, 0.0, 1.0));
    p.setCompositionMode(compositionMode(mode));
    p.drawImage(0, 0, layer);
    p.end();
    return out;
}

QImage mix(const QImage &outside, const QImage &inside, const QImage &mask)
{
    QImage out = img::normalized(outside);
    if (out.isNull() || inside.isNull() || mask.isNull())
        return out;
    const QImage in = img::normalized(img::scaled(inside, out.size(), true));
    const QImage m = img::scaled(mask.convertToFormat(QImage::Format_Grayscale8), out.size(), true);
    for (int y = 0; y < out.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(in.constScanLine(y));
        const uchar *f = m.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const uint k = f[x];
            if (k == 0)
                continue;
            if (k == 255) {
                row[x] = b[x];
                continue;
            }
            auto c = [&](int (*get)(QRgb)) { return int((get(row[x]) * (255 - k) + get(b[x]) * k + 127) / 255); };
            row[x] = qRgba(c(qRed), c(qGreen), c(qBlue), c(qAlpha));
        }
    }
    return out;
}

// ---- Masks ------------------------------------------------------------------------------------

QImage growShrinkFeather(const QImage &mask, int grow, int feather)
{
    QImage m = mask.convertToFormat(QImage::Format_Grayscale8);
    if (grow > 0)
        m = img::dilateMask(m, grow);
    else if (grow < 0)
        m = img::erodeMask(m, -grow);
    if (feather > 0)
        m = img::blurMask(m, feather);
    return m;
}

// ---- Geometry ---------------------------------------------------------------------------------

QImage resize(const QImage &image, const QSize &size, bool smooth)
{
    const QImage src = img::normalized(image);
    if (src.isNull() || size.isEmpty() || size == src.size())
        return src;
    return img::normalized(src.scaled(size, Qt::IgnoreAspectRatio, smooth ? Qt::SmoothTransformation : Qt::FastTransformation));
}

QImage crop(const QImage &image, const QRect &rect)
{
    const QRect r = rect.intersected(image.rect());
    if (r.isEmpty())
        return {};
    return img::normalized(image).copy(r);
}

QImage flip(const QImage &image, Qt::Orientations orientations)
{
    return img::normalized(image).flipped(orientations);
}

QImage rotate(const QImage &image, double degrees, bool smooth)
{
    const QImage src = img::normalized(image);
    double d = std::fmod(degrees, 360.0);
    if (d < 0.0)
        d += 360.0;
    if (src.isNull() || d == 0.0)
        return src;
    QTransform t;
    t.rotate(d); // exact for multiples of 90
    return img::normalized(src.transformed(t, smooth ? Qt::SmoothTransformation : Qt::FastTransformation));
}

} // namespace lumen::filters::ops
