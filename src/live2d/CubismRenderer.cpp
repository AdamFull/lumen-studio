#include "live2d/CubismRenderer.h"

#include <QHash>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>

namespace lumen::live2d {

namespace {

QThreadPool *renderPool()
{
    static QThreadPool *pool = [] {
        auto *p = new QThreadPool;
        p->setObjectName(QStringLiteral("CubismRender"));
        p->setMaxThreadCount(std::max(2, QThread::idealThreadCount()));
        return p;
    }();
    return pool;
}

struct Vec
{
    float x, y;
};

struct Rgba
{
    float r, g, b, a;
};

// Screen-space geometry of one drawable (shared by all bands).
struct Mesh
{
    QVector<Vec> screen;
    QVector<Vec> uv; // texture pixel coordinates
    const DrawableFrame *drawable = nullptr;
    const QImage *texture = nullptr;
};

inline Rgba sampleBilinear(const QImage &tex, float x, float y)
{
    const int w = tex.width(), h = tex.height();
    x = std::clamp(x - 0.5f, 0.0f, float(w - 1));
    y = std::clamp(y - 0.5f, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - x0, fy = y - y0;
    const QRgb *r0 = reinterpret_cast<const QRgb *>(tex.constScanLine(y0));
    const QRgb *r1 = reinterpret_cast<const QRgb *>(tex.constScanLine(y1));
    const QRgb c00 = r0[x0], c10 = r0[x1], c01 = r1[x0], c11 = r1[x1];
    const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    constexpr float k = 1.0f / 255.0f;
    return {(qRed(c00) * w00 + qRed(c10) * w10 + qRed(c01) * w01 + qRed(c11) * w11) * k,
            (qGreen(c00) * w00 + qGreen(c10) * w10 + qGreen(c01) * w01 + qGreen(c11) * w11) * k,
            (qBlue(c00) * w00 + qBlue(c10) * w10 + qBlue(c01) * w01 + qBlue(c11) * w11) * k,
            (qAlpha(c00) * w00 + qAlpha(c10) * w10 + qAlpha(c01) * w01 + qAlpha(c11) * w11) * k};
}

inline float edge(const Vec &a, const Vec &b, float px, float py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

// Consistent tie-break for pixels exactly on an edge so shared edges are drawn once.
inline bool ownsEdge(const Vec &a, const Vec &b)
{
    const float dy = b.y - a.y, dx = b.x - a.x;
    return dy > 0 || (dy == 0 && dx < 0);
}

// Calls fn(x, y, l0, l1, l2) for every pixel centre of the triangle inside rows [y0, y1).
template<typename Fn>
void rasterTriangle(Vec a, Vec b, Vec c, int width, int y0, int y1, Fn &&fn)
{
    float area = edge(a, b, c.x, c.y);
    if (std::abs(area) < 1e-6f)
        return;
    bool swapped = false;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
        swapped = true;
    }
    const int minX = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int maxX = std::min(width - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int minY = std::max(y0, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int maxY = std::min(y1 - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    if (minX > maxX || minY > maxY)
        return;
    const bool own0 = ownsEdge(b, c), own1 = ownsEdge(c, a), own2 = ownsEdge(a, b);
    const float inv = 1.0f / area;
    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;
        for (int x = minX; x <= maxX; ++x) {
            const float px = x + 0.5f;
            const float w0 = edge(b, c, px, py), w1 = edge(c, a, px, py), w2 = edge(a, b, px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            if ((w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2))
                continue;
            float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            if (swapped)
                std::swap(l1, l2); // weights follow the original vertex order (a, b, c)
            fn(x, y, l0, l1, l2);
        }
    }
}

struct Band
{
    int y0 = 0, y1 = 0;
};

} // namespace

QTransform frameTransform(const ModelFrame &frame, const RenderOptions &options)
{
    const double cw = std::max(1.0, frame.canvasSize.width()), ch = std::max(1.0, frame.canvasSize.height());
    const double scale = std::min(options.size.width() / cw, options.size.height() / ch) * 0.96 * options.zoom;
    const double ox = (options.size.width() - cw * scale) / 2.0 + options.pan.x();
    const double oy = (options.size.height() - ch * scale) / 2.0 + options.pan.y();
    // Model units (y up, relative to the canvas origin) -> canvas pixels -> output pixels.
    QTransform t;
    t.translate(ox, oy);
    t.scale(scale, scale);
    t.translate(frame.origin.x(), frame.origin.y());
    t.scale(frame.pixelsPerUnit, -frame.pixelsPerUnit);
    return t;
}

QImage renderFrame(const ModelFrame &frame, const QList<QImage> &textures, const RenderOptions &options)
{
    const int W = std::max(1, options.size.width()), H = std::max(1, options.size.height());
    QImage out(W, H, QImage::Format_ARGB32_Premultiplied);
    const QTransform t = frameTransform(frame, options);

    // Geometry in output pixels.
    QVector<Mesh> meshes(frame.drawables.size());
    for (int i = 0; i < frame.drawables.size(); ++i) {
        const DrawableFrame &d = frame.drawables[i];
        Mesh &m = meshes[i];
        m.drawable = &d;
        m.texture = (d.texture >= 0 && d.texture < textures.size() && !textures[d.texture].isNull()) ? &textures[d.texture] : nullptr;
        if (!m.texture)
            continue;
        m.screen.resize(d.positions.size());
        m.uv.resize(d.uvs.size());
        for (int v = 0; v < d.positions.size(); ++v) {
            const QPointF p = t.map(d.positions[v]);
            m.screen[v] = Vec{float(p.x()), float(p.y())};
        }
        const float tw = m.texture->width(), th = m.texture->height();
        for (int v = 0; v < d.uvs.size(); ++v)
            m.uv[v] = Vec{float(d.uvs[v].x()) * tw, float(1.0 - d.uvs[v].y()) * th}; // v points up
    }
    QVector<int> order;
    for (int i = 0; i < frame.drawables.size(); ++i) {
        const DrawableFrame &d = frame.drawables[i];
        if (d.visible && d.opacity > 0.0f && meshes[i].texture && !d.indices.isEmpty())
            order.append(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&frame](int a, int b) { return frame.drawables[a].renderOrder < frame.drawables[b].renderOrder; });

    const float bg[4] = {float(options.background.redF() * options.background.alphaF()),
                         float(options.background.greenF() * options.background.alphaF()),
                         float(options.background.blueF() * options.background.alphaF()), float(options.background.alphaF())};

    QList<Band> bands;
    const int bandHeight = std::max(16, H / (renderPool()->maxThreadCount() * 2));
    for (int y = 0; y < H; y += bandHeight)
        bands.append(Band{y, std::min(H, y + bandHeight)});

    QtConcurrent::blockingMap(renderPool(), bands, [&](const Band &band) {
        const int rows = band.y1 - band.y0;
        QVector<Rgba> fb(qsizetype(W) * rows, Rgba{0, 0, 0, 0});
        QHash<QString, QVector<float>> maskCache;

        // Coverage of a set of mask drawables (texture alpha) in this band.
        auto maskCoverage = [&](const DrawableFrame &d) -> const QVector<float> & {
            QString key;
            for (int m : d.masks)
                key += QString::number(m) + QLatin1Char(',');
            auto it = maskCache.find(key);
            if (it != maskCache.end())
                return *it;
            QVector<float> cov(qsizetype(W) * rows, 0.0f);
            for (int m : d.masks) {
                if (m < 0 || m >= meshes.size() || !meshes[m].texture)
                    continue;
                const Mesh &mesh = meshes[m];
                const QVector<quint16> &idx = mesh.drawable->indices;
                for (int k = 0; k + 2 < idx.size(); k += 3) {
                    const int i0 = idx[k], i1 = idx[k + 1], i2 = idx[k + 2];
                    if (i0 >= mesh.screen.size() || i1 >= mesh.screen.size() || i2 >= mesh.screen.size())
                        continue;
                    rasterTriangle(mesh.screen[i0], mesh.screen[i1], mesh.screen[i2], W, band.y0, band.y1,
                                   [&](int x, int y, float l0, float l1, float l2) {
                                       const float u = mesh.uv[i0].x * l0 + mesh.uv[i1].x * l1 + mesh.uv[i2].x * l2;
                                       const float v = mesh.uv[i0].y * l0 + mesh.uv[i1].y * l1 + mesh.uv[i2].y * l2;
                                       float &c = cov[qsizetype(y - band.y0) * W + x];
                                       c = std::max(c, sampleBilinear(*mesh.texture, u, v).a);
                                   });
                }
            }
            return *maskCache.insert(key, cov);
        };

        for (int di : std::as_const(order)) {
            const Mesh &mesh = meshes[di];
            const DrawableFrame &d = *mesh.drawable;
            const QVector<float> *cov = d.masks.isEmpty() ? nullptr : &maskCoverage(d);
            const bool tinted = d.multiply[0] != 1.0f || d.multiply[1] != 1.0f || d.multiply[2] != 1.0f || d.screen[0] != 0.0f
                                || d.screen[1] != 0.0f || d.screen[2] != 0.0f;
            const QVector<quint16> &idx = d.indices;
            for (int k = 0; k + 2 < idx.size(); k += 3) {
                const int i0 = idx[k], i1 = idx[k + 1], i2 = idx[k + 2];
                if (i0 >= mesh.screen.size() || i1 >= mesh.screen.size() || i2 >= mesh.screen.size())
                    continue;
                rasterTriangle(mesh.screen[i0], mesh.screen[i1], mesh.screen[i2], W, band.y0, band.y1,
                               [&](int x, int y, float l0, float l1, float l2) {
                                   const qsizetype p = qsizetype(y - band.y0) * W + x;
                                   float alpha = d.opacity;
                                   if (cov) {
                                       const float c = (*cov)[p];
                                       alpha *= d.invertedMask ? 1.0f - c : c;
                                       if (alpha <= 0.0f)
                                           return;
                                   }
                                   const float u = mesh.uv[i0].x * l0 + mesh.uv[i1].x * l1 + mesh.uv[i2].x * l2;
                                   const float v = mesh.uv[i0].y * l0 + mesh.uv[i1].y * l1 + mesh.uv[i2].y * l2;
                                   Rgba s = sampleBilinear(*mesh.texture, u, v);
                                   if (s.a <= 0.0f)
                                       return;
                                   if (tinted) {
                                       // Colour operations work on straight colour.
                                       float rgb[3] = {s.r / s.a, s.g / s.a, s.b / s.a};
                                       for (int ch = 0; ch < 3; ++ch) {
                                           rgb[ch] *= d.multiply[ch];
                                           rgb[ch] = rgb[ch] + d.screen[ch] - rgb[ch] * d.screen[ch];
                                       }
                                       s = Rgba{rgb[0] * s.a, rgb[1] * s.a, rgb[2] * s.a, s.a};
                                   }
                                   s = Rgba{s.r * alpha, s.g * alpha, s.b * alpha, s.a * alpha};
                                   Rgba &dst = fb[p];
                                   switch (d.blend) {
                                   case DrawableFrame::Blend::Normal:
                                       dst = Rgba{s.r + dst.r * (1 - s.a), s.g + dst.g * (1 - s.a), s.b + dst.b * (1 - s.a),
                                                  s.a + dst.a * (1 - s.a)};
                                       break;
                                   case DrawableFrame::Blend::Additive:
                                       dst.r = std::min(1.0f, dst.r + s.r);
                                       dst.g = std::min(1.0f, dst.g + s.g);
                                       dst.b = std::min(1.0f, dst.b + s.b);
                                       break;
                                   case DrawableFrame::Blend::Multiplicative:
                                       dst.r = s.r * dst.r + dst.r * (1 - s.a);
                                       dst.g = s.g * dst.g + dst.g * (1 - s.a);
                                       dst.b = s.b * dst.b + dst.b * (1 - s.a);
                                       break;
                                   }
                               });
            }
        }

        // Composite over the background into the output rows.
        for (int y = band.y0; y < band.y1; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
            const Rgba *src = fb.constData() + qsizetype(y - band.y0) * W;
            for (int x = 0; x < W; ++x) {
                const Rgba &s = src[x];
                const float inv = 1.0f - s.a;
                auto to8 = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                const int a = to8(s.a + bg[3] * inv);
                row[x] = qRgba(std::min(a, to8(s.r + bg[0] * inv)), std::min(a, to8(s.g + bg[1] * inv)),
                               std::min(a, to8(s.b + bg[2] * inv)), a);
            }
        }
    });
    return out;
}

} // namespace lumen::live2d
