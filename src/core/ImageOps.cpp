#include "core/ImageOps.h"

#include <QBuffer>
#include <QColor>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lumen::img {

// ---- Codecs ---------------------------------------------------------------------------------

QByteArray encode(const QImage &image, const char *format, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    if (quality >= 0)
        writer.setQuality(quality);
    if (qstricmp(format, "PNG") == 0)
        writer.setCompression(quality >= 0 ? quality : 1); // fast compression for interactive use
    QImage toWrite = image;
    if (qstricmp(format, "JPG") == 0 || qstricmp(format, "JPEG") == 0) {
        if (image.hasAlphaChannel())
            toWrite = flatten(image, Qt::white);
    } else if (toWrite.format() == QImage::Format_ARGB32_Premultiplied) {
        toWrite = toWrite.convertToFormat(QImage::Format_ARGB32);
    }
    if (!writer.write(toWrite))
        return {};
    return bytes;
}

QImage decode(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setAllocationLimit(1024); // MB; protects against decompression bombs
    QImage image = reader.read();
    return image;
}

QByteArray toBase64(const QImage &image, const char *format, int quality)
{
    return encode(image, format, quality).toBase64();
}

QString mimeForFormat(const char *format)
{
    if (qstricmp(format, "JPG") == 0 || qstricmp(format, "JPEG") == 0)
        return QStringLiteral("image/jpeg");
    if (qstricmp(format, "WEBP") == 0)
        return QStringLiteral("image/webp");
    return QStringLiteral("image/png");
}

QString toDataUrl(const QImage &image, const char *format, int quality)
{
    return QStringLiteral("data:") + mimeForFormat(format) + QStringLiteral(";base64,")
         + QString::fromLatin1(toBase64(image, format, quality));
}

QImage fromBase64(const QByteArray &data)
{
    QByteArray payload = data;
    const int comma = payload.indexOf(',');
    if (payload.startsWith("data:") && comma > 0)
        payload = payload.mid(comma + 1);
    return decode(QByteArray::fromBase64(payload));
}

QImage normalized(const QImage &image)
{
    if (image.isNull() || image.format() == QImage::Format_ARGB32_Premultiplied)
        return image;
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// ---- Masks ----------------------------------------------------------------------------------

QImage createMask(const QSize &size, uchar fill)
{
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(fill);
    return mask;
}

QImage maskFromLuminance(const QImage &image)
{
    if (image.format() == QImage::Format_Grayscale8)
        return image;
    if (!image.hasAlphaChannel())
        return image.convertToFormat(QImage::Format_Grayscale8);
    // Treat transparent pixels as unselected.
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    QImage mask(src.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < src.height(); ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *out = mask.scanLine(y);
        for (int x = 0; x < src.width(); ++x)
            out[x] = static_cast<uchar>(qGray(in[x]) * qAlpha(in[x]) / 255);
    }
    return mask;
}

QImage maskFromAlpha(const QImage &image)
{
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    QImage mask(src.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < src.height(); ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        uchar *out = mask.scanLine(y);
        for (int x = 0; x < src.width(); ++x)
            out[x] = static_cast<uchar>(qAlpha(in[x]));
    }
    return mask;
}

QImage alphaFromMask(const QImage &image, const QImage &mask, bool transparentWhereMasked)
{
    QImage out = image.convertToFormat(QImage::Format_ARGB32);
    const QImage m = mask.size() == image.size() ? mask : scaled(mask, image.size(), true);
    for (int y = 0; y < out.height(); ++y) {
        auto *px = reinterpret_cast<QRgb *>(out.scanLine(y));
        const uchar *mv = m.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const int a = transparentWhereMasked ? 255 - mv[x] : mv[x];
            px[x] = qRgba(qRed(px[x]), qGreen(px[x]), qBlue(px[x]), a);
        }
    }
    return out;
}

QRect maskBounds(const QImage &mask, int threshold)
{
    if (mask.isNull())
        return {};
    int minX = mask.width(), minY = mask.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        int first = -1, last = -1;
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] >= threshold) {
                first = x;
                break;
            }
        }
        if (first < 0)
            continue;
        for (int x = mask.width() - 1; x >= first; --x) {
            if (row[x] >= threshold) {
                last = x;
                break;
            }
        }
        minX = std::min(minX, first);
        maxX = std::max(maxX, last);
        minY = std::min(minY, y);
        maxY = y;
    }
    if (maxX < 0)
        return {};
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

bool maskIsEmpty(const QImage &mask, int threshold)
{
    if (mask.isNull())
        return true;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] >= threshold)
                return false;
        }
    }
    return true;
}

QImage invertMask(const QImage &mask)
{
    QImage out = mask.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < out.height(); ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x)
            row[x] = static_cast<uchar>(255 - row[x]);
    }
    return out;
}

QImage thresholdMask(const QImage &mask, int threshold)
{
    QImage out = mask.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < out.height(); ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x)
            row[x] = row[x] >= threshold ? 255 : 0;
    }
    return out;
}

namespace {

// van Herk / Gil-Werman running max over a window of size 2r+1, outside values = 0.
void maxFilter1D(const uchar *in, uchar *out, int n, int stride, int r, std::vector<uchar> &buf,
                 std::vector<uchar> &g, std::vector<uchar> &h)
{
    const int w = 2 * r + 1;
    const int padded = n + 2 * r;
    const int total = ((padded + w - 1) / w) * w;
    buf.assign(static_cast<size_t>(total), 0);
    for (int i = 0; i < n; ++i)
        buf[static_cast<size_t>(i + r)] = in[static_cast<qsizetype>(i) * stride];
    g.resize(static_cast<size_t>(total));
    h.resize(static_cast<size_t>(total));
    for (int start = 0; start < total; start += w) {
        g[static_cast<size_t>(start)] = buf[static_cast<size_t>(start)];
        for (int i = 1; i < w; ++i) {
            const auto idx = static_cast<size_t>(start + i);
            g[idx] = std::max(g[idx - 1], buf[idx]);
        }
        const auto end = static_cast<size_t>(start + w - 1);
        h[end] = buf[end];
        for (int i = w - 2; i >= 0; --i) {
            const auto idx = static_cast<size_t>(start + i);
            h[idx] = std::max(h[idx + 1], buf[idx]);
        }
    }
    for (int i = 0; i < n; ++i) {
        // Window in padded coordinates: [i, i + w - 1]
        const auto lo = static_cast<size_t>(i);
        const auto hi = static_cast<size_t>(i + w - 1);
        out[static_cast<qsizetype>(i) * stride] = std::max(h[lo], g[hi]);
    }
}

QImage dilateImpl(const QImage &mask, int radius)
{
    QImage src = mask.convertToFormat(QImage::Format_Grayscale8);
    if (radius <= 0 || src.isNull())
        return src;
    const int w = src.width();
    const int hgt = src.height();
    QImage tmp(src.size(), QImage::Format_Grayscale8);
    std::vector<uchar> buf, g, h;
    for (int y = 0; y < hgt; ++y)
        maxFilter1D(src.constScanLine(y), tmp.scanLine(y), w, 1, radius, buf, g, h);
    QImage out(src.size(), QImage::Format_Grayscale8);
    std::vector<uchar> col(static_cast<size_t>(hgt));
    std::vector<uchar> colOut(static_cast<size_t>(hgt));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < hgt; ++y)
            col[static_cast<size_t>(y)] = tmp.constScanLine(y)[x];
        maxFilter1D(col.data(), colOut.data(), hgt, 1, radius, buf, g, h);
        for (int y = 0; y < hgt; ++y)
            out.scanLine(y)[x] = colOut[static_cast<size_t>(y)];
    }
    return out;
}

void boxBlurLine(const uchar *in, uchar *out, int n, int stride, int r)
{
    if (n <= 0)
        return;
    const int w = 2 * r + 1;
    int sum = 0;
    auto at = [&](int i) -> int { return in[static_cast<qsizetype>(std::clamp(i, 0, n - 1)) * stride]; };
    for (int i = -r; i <= r; ++i)
        sum += at(i);
    for (int i = 0; i < n; ++i) {
        out[static_cast<qsizetype>(i) * stride] = static_cast<uchar>((sum + w / 2) / w);
        sum += at(i + r + 1) - at(i - r);
    }
}

} // namespace

QImage dilateMask(const QImage &mask, int radius)
{
    return dilateImpl(mask, radius);
}

QImage erodeMask(const QImage &mask, int radius)
{
    if (radius <= 0)
        return mask.convertToFormat(QImage::Format_Grayscale8);
    // Erosion = complement of the dilated complement. The image border is treated as selected so
    // masks touching the canvas edge do not shrink away from it.
    QImage inv = invertMask(mask);
    return invertMask(dilateImpl(inv, radius));
}

QImage blurMask(const QImage &mask, int radius)
{
    QImage src = mask.convertToFormat(QImage::Format_Grayscale8);
    if (radius <= 0 || src.isNull())
        return src;
    const int r = std::max(1, radius / 2);
    const int w = src.width();
    const int h = src.height();
    QImage a = src;
    QImage b(src.size(), QImage::Format_Grayscale8);
    std::vector<uchar> col(static_cast<size_t>(h)), colOut(static_cast<size_t>(h));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y)
            boxBlurLine(a.constScanLine(y), b.scanLine(y), w, 1, r);
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y)
                col[static_cast<size_t>(y)] = b.constScanLine(y)[x];
            boxBlurLine(col.data(), colOut.data(), h, 1, r);
            for (int y = 0; y < h; ++y)
                a.scanLine(y)[x] = colOut[static_cast<size_t>(y)];
        }
    }
    return a;
}

QImage combineMasks(const QImage &a, const QImage &b, MaskOp op)
{
    if (op == MaskOp::Replace || a.isNull())
        return b.convertToFormat(QImage::Format_Grayscale8);
    QImage out = a.convertToFormat(QImage::Format_Grayscale8);
    const QImage bb = b.convertToFormat(QImage::Format_Grayscale8);
    const int w = std::min(out.width(), bb.width());
    const int h = std::min(out.height(), bb.height());
    for (int y = 0; y < h; ++y) {
        uchar *o = out.scanLine(y);
        const uchar *s = bb.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            switch (op) {
            case MaskOp::Union: o[x] = std::max(o[x], s[x]); break;
            case MaskOp::Intersect: o[x] = std::min(o[x], s[x]); break;
            case MaskOp::Subtract: o[x] = static_cast<uchar>(o[x] * (255 - s[x]) / 255); break;
            case MaskOp::Replace: o[x] = s[x]; break;
            }
        }
    }
    return out;
}

double maskCoverage(const QImage &mask)
{
    if (mask.isNull() || mask.width() == 0 || mask.height() == 0)
        return 0.0;
    quint64 sum = 0;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            sum += row[x];
    }
    return static_cast<double>(sum) / (255.0 * mask.width() * mask.height());
}

// ---- Geometry -------------------------------------------------------------------------------

QRect expandRegion(const QRect &rect, int padding, const QSize &bounds, double targetAspect)
{
    const QRect limits(QPoint(0, 0), bounds);
    QRect r = rect.adjusted(-padding, -padding, padding, padding).intersected(limits);
    if (targetAspect > 0.0 && !r.isEmpty()) {
        double w = r.width();
        double h = r.height();
        if (w / h < targetAspect)
            w = h * targetAspect;
        else
            h = w / targetAspect;
        w = std::min<double>(w, bounds.width());
        h = std::min<double>(h, bounds.height());
        const QPointF c = QRectF(r).center();
        QRectF grown(c.x() - w / 2.0, c.y() - h / 2.0, w, h);
        // Shift inside the bounds instead of clipping so the aspect is preserved where possible.
        if (grown.left() < 0)
            grown.moveLeft(0);
        if (grown.top() < 0)
            grown.moveTop(0);
        if (grown.right() > bounds.width())
            grown.moveRight(bounds.width());
        if (grown.bottom() > bounds.height())
            grown.moveBottom(bounds.height());
        r = grown.toAlignedRect().intersected(limits);
    }
    return r;
}

QSize roundToMultiple(const QSize &size, int multiple)
{
    if (multiple <= 1)
        return size;
    auto round = [multiple](int v) {
        const int m = static_cast<int>(std::lround(static_cast<double>(v) / multiple)) * multiple;
        return std::max(multiple, m);
    };
    return {round(size.width()), round(size.height())};
}

QSize fitToPixelBudget(const QSize &size, qint64 targetPixels, int multiple)
{
    if (size.isEmpty() || targetPixels <= 0)
        return size;
    const double scale = std::sqrt(static_cast<double>(targetPixels) / (static_cast<double>(size.width()) * size.height()));
    const QSize s(static_cast<int>(std::lround(size.width() * scale)), static_cast<int>(std::lround(size.height() * scale)));
    return roundToMultiple(s, multiple);
}

QSize fitAspect(const QSize &bounds, double aspect)
{
    if (aspect <= 0.0 || bounds.isEmpty())
        return bounds;
    int w = bounds.width();
    int h = static_cast<int>(std::lround(w / aspect));
    if (h > bounds.height()) {
        h = bounds.height();
        w = static_cast<int>(std::lround(h * aspect));
    }
    return {w, h};
}

// ---- Compositing ----------------------------------------------------------------------------

QImage scaled(const QImage &image, const QSize &size, bool smooth)
{
    if (image.size() == size)
        return image;
    return image.scaled(size, Qt::IgnoreAspectRatio, smooth ? Qt::SmoothTransformation : Qt::FastTransformation);
}

QImage applyMaskToAlpha(const QImage &image, const QImage &mask)
{
    QImage out = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage m = scaled(mask.convertToFormat(QImage::Format_Grayscale8), out.size(), true);
    for (int y = 0; y < out.height(); ++y) {
        auto *px = reinterpret_cast<QRgb *>(out.scanLine(y));
        const uchar *mv = m.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            const uint f = mv[x];
            if (f == 255)
                continue;
            const QRgb p = px[x];
            px[x] = qRgba(qRed(p) * f / 255, qGreen(p) * f / 255, qBlue(p) * f / 255, qAlpha(p) * f / 255);
        }
    }
    return out;
}

QImage compositeMasked(const QImage &base, const QImage &overlay, const QImage &mask, const QPoint &pos)
{
    QImage out = normalized(base);
    const QImage layer = applyMaskToAlpha(overlay, mask);
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(pos, layer);
    p.end();
    return out;
}

QImage colorMatch(const QImage &generated, const QImage &reference, const QImage &mask)
{
    const QImage g = generated.convertToFormat(QImage::Format_ARGB32);
    const QImage r = scaled(reference.convertToFormat(QImage::Format_ARGB32), g.size(), true);
    const QImage m = scaled(mask.convertToFormat(QImage::Format_Grayscale8), g.size(), true);
    double sumG[3] = {0, 0, 0}, sumR[3] = {0, 0, 0}, sqG[3] = {0, 0, 0}, sqR[3] = {0, 0, 0};
    quint64 n = 0;
    for (int y = 0; y < g.height(); ++y) {
        const auto *gp = reinterpret_cast<const QRgb *>(g.constScanLine(y));
        const auto *rp = reinterpret_cast<const QRgb *>(r.constScanLine(y));
        const uchar *mp = m.constScanLine(y);
        for (int x = 0; x < g.width(); ++x) {
            if (mp[x] >= 128 || qAlpha(rp[x]) < 128)
                continue;
            const int gv[3] = {qRed(gp[x]), qGreen(gp[x]), qBlue(gp[x])};
            const int rv[3] = {qRed(rp[x]), qGreen(rp[x]), qBlue(rp[x])};
            for (int c = 0; c < 3; ++c) {
                sumG[c] += gv[c];
                sumR[c] += rv[c];
                sqG[c] += double(gv[c]) * gv[c];
                sqR[c] += double(rv[c]) * rv[c];
            }
            ++n;
        }
    }
    if (n < 64)
        return generated;
    double scale[3], offset[3];
    for (int c = 0; c < 3; ++c) {
        const double mg = sumG[c] / n, mr = sumR[c] / n;
        const double sg = std::sqrt(std::max(1e-6, sqG[c] / n - mg * mg));
        const double sr = std::sqrt(std::max(1e-6, sqR[c] / n - mr * mr));
        scale[c] = std::clamp(sr / sg, 0.5, 2.0);
        offset[c] = mr - mg * scale[c];
    }
    QImage out = g;
    for (int y = 0; y < out.height(); ++y) {
        auto *px = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QRgb p = px[x];
            const int v[3] = {qRed(p), qGreen(p), qBlue(p)};
            int o[3];
            for (int c = 0; c < 3; ++c)
                o[c] = std::clamp(static_cast<int>(std::lround(v[c] * scale[c] + offset[c])), 0, 255);
            px[x] = qRgba(o[0], o[1], o[2], qAlpha(p));
        }
    }
    return out.convertToFormat(generated.format());
}

QImage fillTransparentFromEdges(const QImage &image)
{
    // Push-pull fill: build a mip pyramid of premultiplied colour (ignoring transparent pixels),
    // then fill transparent pixels from coarser levels. Produces smooth, plausible context colour.
    QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    std::vector<QImage> levels;
    levels.push_back(src);
    while (levels.back().width() > 1 || levels.back().height() > 1) {
        const QImage &prev = levels.back();
        const int w = std::max(1, (prev.width() + 1) / 2);
        const int h = std::max(1, (prev.height() + 1) / 2);
        QImage next(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            auto *o = reinterpret_cast<QRgb *>(next.scanLine(y));
            for (int x = 0; x < w; ++x) {
                int r = 0, g = 0, b = 0, a = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    const int sy = std::min(prev.height() - 1, y * 2 + dy);
                    const auto *row = reinterpret_cast<const QRgb *>(prev.constScanLine(sy));
                    for (int dx = 0; dx < 2; ++dx) {
                        const QRgb p = row[std::min(prev.width() - 1, x * 2 + dx)];
                        r += qRed(p);
                        g += qGreen(p);
                        b += qBlue(p);
                        a += qAlpha(p);
                    }
                }
                if (a > 0) {
                    // Renormalise to full opacity so colour does not fade toward black.
                    const double k = 255.0 / a;
                    o[x] = qRgba(std::min(255, int(r * k)), std::min(255, int(g * k)), std::min(255, int(b * k)), 255);
                } else {
                    o[x] = 0;
                }
            }
        }
        levels.push_back(next);
    }
    // Pull: fill each level's transparent pixels from the next coarser level.
    for (int i = static_cast<int>(levels.size()) - 2; i >= 0; --i) {
        QImage &cur = levels[static_cast<size_t>(i)];
        const QImage up = levels[static_cast<size_t>(i + 1)].scaled(cur.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < cur.height(); ++y) {
            auto *c = reinterpret_cast<QRgb *>(cur.scanLine(y));
            const auto *u = reinterpret_cast<const QRgb *>(up.constScanLine(y));
            for (int x = 0; x < cur.width(); ++x) {
                const int a = qAlpha(c[x]);
                if (a == 255)
                    continue;
                const QRgb uu = u[x];
                const int inv = 255 - a;
                c[x] = qRgba(qRed(c[x]) + qRed(uu) * inv / 255, qGreen(c[x]) + qGreen(uu) * inv / 255,
                             qBlue(c[x]) + qBlue(uu) * inv / 255, 255);
            }
        }
    }
    return levels.front();
}

QImage flatten(const QImage &image, const QColor &background)
{
    QImage out(image.size(), QImage::Format_RGB32);
    out.fill(background);
    QPainter p(&out);
    p.drawImage(0, 0, image);
    p.end();
    return out;
}

QImage thumbnail(const QImage &image, int maxSide)
{
    if (image.isNull())
        return {};
    if (image.width() <= maxSide && image.height() <= maxSide)
        return image;
    return image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace lumen::img
