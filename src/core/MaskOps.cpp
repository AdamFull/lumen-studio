#include "core/MaskOps.h"

#include "core/ImageOps.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lumen::img {

namespace {

QImage asMask(const QImage &mask)
{
    return mask.format() == QImage::Format_Grayscale8 ? mask : mask.convertToFormat(QImage::Format_Grayscale8);
}

struct UnionFind
{
    std::vector<int> parent;
    int make()
    {
        parent.push_back(int(parent.size()));
        return int(parent.size()) - 1;
    }
    int find(int x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
};

double pointSegmentDistanceSq(double px, double py, const QLineF &s)
{
    const double dx = s.dx(), dy = s.dy();
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > 1e-12)
        t = std::clamp(((px - s.x1()) * dx + (py - s.y1()) * dy) / len2, 0.0, 1.0);
    const double cx = s.x1() + t * dx - px;
    const double cy = s.y1() + t * dy - py;
    return cx * cx + cy * cy;
}

QImage maskFromComponentLabels(const Components &c, int component)
{
    QImage out = createMask(c.size, 0);
    const QRect b = c.bounds[size_t(component)];
    const int w = c.size.width();
    for (int y = b.top(); y <= b.bottom(); ++y) {
        uchar *row = out.scanLine(y);
        const int *lab = c.labels.data() + qsizetype(y) * w;
        for (int x = b.left(); x <= b.right(); ++x) {
            if (lab[x] == component)
                row[x] = 255;
        }
    }
    return out;
}

bool colorClose(QRgb a, QRgb b, int tol)
{
    return std::abs(qRed(a) - qRed(b)) <= tol && std::abs(qGreen(a) - qGreen(b)) <= tol
           && std::abs(qBlue(a) - qBlue(b)) <= tol && std::abs(qAlpha(a) - qAlpha(b)) <= tol;
}

// Scanline flood fill over pixels accepted by `accept(x, y)`, seeded from the given points.
template<typename Accept>
QImage floodFrom(const QSize &size, const QList<QPoint> &seeds, Accept accept)
{
    QImage out = createMask(size, 0);
    const int w = size.width(), h = size.height();
    std::vector<QPoint> stack;
    stack.reserve(1024);
    for (const QPoint &s : seeds) {
        if (s.x() >= 0 && s.y() >= 0 && s.x() < w && s.y() < h)
            stack.push_back(s);
    }
    while (!stack.empty()) {
        const QPoint p = stack.back();
        stack.pop_back();
        uchar *row = out.scanLine(p.y());
        if (row[p.x()] || !accept(p.x(), p.y()))
            continue;
        int x0 = p.x(), x1 = p.x();
        while (x0 > 0 && !row[x0 - 1] && accept(x0 - 1, p.y()))
            --x0;
        while (x1 < w - 1 && !row[x1 + 1] && accept(x1 + 1, p.y()))
            ++x1;
        std::fill(row + x0, row + x1 + 1, uchar(255));
        for (int ny : {p.y() - 1, p.y() + 1}) {
            if (ny < 0 || ny >= h)
                continue;
            const uchar *nrow = out.constScanLine(ny);
            bool inSpan = false;
            for (int x = x0; x <= x1; ++x) {
                const bool ok = !nrow[x] && accept(x, ny);
                if (ok && !inSpan)
                    stack.emplace_back(x, ny);
                inSpan = ok;
            }
        }
    }
    return out;
}

} // namespace

// ---- Connected components ---------------------------------------------------------------------

Components connectedComponents(const QImage &maskIn, int threshold, bool eightConnected)
{
    Components c;
    const QImage mask = asMask(maskIn);
    c.size = mask.size();
    const int w = mask.width(), h = mask.height();
    c.labels.assign(size_t(qsizetype(w) * h), 0);
    if (w == 0 || h == 0)
        return c;

    UnionFind uf;
    uf.make(); // 0 = background
    for (int y = 0; y < h; ++y) {
        const uchar *row = mask.constScanLine(y);
        int *lab = c.labels.data() + qsizetype(y) * w;
        const int *up = y > 0 ? lab - w : nullptr;
        for (int x = 0; x < w; ++x) {
            if (row[x] < threshold)
                continue;
            int best = 0;
            auto consider = [&](int l) {
                if (l == 0)
                    return;
                if (best == 0)
                    best = l;
                else if (l != best)
                    uf.unite(best, l);
            };
            if (x > 0)
                consider(lab[x - 1]);
            if (up) {
                consider(up[x]);
                if (eightConnected) {
                    if (x > 0)
                        consider(up[x - 1]);
                    if (x < w - 1)
                        consider(up[x + 1]);
                }
            }
            lab[x] = best ? best : uf.make();
        }
    }

    // Resolve to compact ids.
    std::vector<int> compact(uf.parent.size(), 0);
    int next = 0;
    for (size_t i = 1; i < uf.parent.size(); ++i) {
        const int root = uf.find(int(i));
        if (compact[size_t(root)] == 0)
            compact[size_t(root)] = ++next;
        compact[i] = compact[size_t(root)];
    }
    c.count = next;
    c.areas.assign(size_t(next) + 1, 0);
    c.bounds.assign(size_t(next) + 1, QRect());
    std::vector<int> minX(size_t(next) + 1, w), minY(size_t(next) + 1, h), maxX(size_t(next) + 1, -1), maxY(size_t(next) + 1, -1);
    for (int y = 0; y < h; ++y) {
        int *lab = c.labels.data() + qsizetype(y) * w;
        for (int x = 0; x < w; ++x) {
            if (!lab[x])
                continue;
            const int id = compact[size_t(lab[x])];
            lab[x] = id;
            ++c.areas[size_t(id)];
            minX[size_t(id)] = std::min(minX[size_t(id)], x);
            maxX[size_t(id)] = std::max(maxX[size_t(id)], x);
            minY[size_t(id)] = std::min(minY[size_t(id)], y);
            maxY[size_t(id)] = std::max(maxY[size_t(id)], y);
        }
    }
    for (int i = 1; i <= next; ++i)
        c.bounds[size_t(i)] = QRect(QPoint(minX[size_t(i)], minY[size_t(i)]), QPoint(maxX[size_t(i)], maxY[size_t(i)]));
    return c;
}

QList<QImage> splitComponents(const QImage &mask, qint64 minArea, int threshold)
{
    const Components c = connectedComponents(mask, threshold);
    std::vector<int> order;
    for (int i = 1; i <= c.count; ++i) {
        if (c.areas[size_t(i)] >= std::max<qint64>(1, minArea))
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return c.areas[size_t(a)] > c.areas[size_t(b)]; });
    QList<QImage> out;
    for (int id : order)
        out.append(maskFromComponentLabels(c, id));
    return out;
}

QImage removeSmallIslands(const QImage &maskIn, qint64 minArea, int threshold)
{
    const QImage mask = asMask(maskIn);
    const Components c = connectedComponents(mask, threshold);
    QImage out = mask.copy();
    const int w = mask.width();
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = out.scanLine(y);
        const int *lab = c.labels.data() + qsizetype(y) * w;
        for (int x = 0; x < w; ++x) {
            if (lab[x] && c.areas[size_t(lab[x])] < minArea)
                row[x] = 0;
        }
    }
    return out;
}

QImage fillHoles(const QImage &maskIn, qint64 maxArea, int threshold)
{
    const QImage mask = asMask(maskIn);
    const QImage inverted = invertMask(thresholdMask(mask, threshold));
    const Components c = connectedComponents(inverted, 128, false);
    QImage out = mask.copy();
    const int w = mask.width(), h = mask.height();
    std::vector<char> touchesBorder(size_t(c.count) + 1, 0);
    for (int i = 1; i <= c.count; ++i) {
        const QRect b = c.bounds[size_t(i)];
        touchesBorder[size_t(i)] = b.left() == 0 || b.top() == 0 || b.right() == w - 1 || b.bottom() == h - 1;
    }
    for (int y = 0; y < h; ++y) {
        uchar *row = out.scanLine(y);
        const int *lab = c.labels.data() + qsizetype(y) * w;
        for (int x = 0; x < w; ++x) {
            const int id = lab[x];
            if (id && !touchesBorder[size_t(id)] && (maxArea < 0 || c.areas[size_t(id)] <= maxArea))
                row[x] = 255;
        }
    }
    return out;
}

QImage smoothMask(const QImage &mask, int radius)
{
    if (radius <= 0)
        return asMask(mask);
    return thresholdMask(blurMask(asMask(mask), radius), 128);
}

// ---- Splitting --------------------------------------------------------------------------------

QPair<QImage, QImage> splitByVerticalLine(const QImage &maskIn, double x)
{
    const QImage mask = asMask(maskIn);
    QImage left = mask.copy(), right = mask.copy();
    const int cut = std::clamp(int(std::lround(x)), 0, mask.width());
    for (int y = 0; y < mask.height(); ++y) {
        uchar *l = left.scanLine(y);
        uchar *r = right.scanLine(y);
        std::fill(l + cut, l + mask.width(), uchar(0));
        std::fill(r, r + cut, uchar(0));
    }
    return {left, right};
}

QList<QImage> splitByCutLine(const QImage &maskIn, const QPolygonF &lineIn, qint64 minArea)
{
    const QImage mask = thresholdMask(asMask(maskIn), 128);
    if (lineIn.size() < 2)
        return {mask};
    // Extend the end segments well beyond the image.
    QPolygonF line = lineIn;
    const double far = std::hypot(mask.width(), mask.height()) * 2.0;
    auto extend = [far](const QPointF &from, const QPointF &to) {
        QLineF l(from, to);
        if (l.length() < 1e-6)
            return to;
        l.setLength(l.length() + far);
        return l.p2();
    };
    line.first() = extend(line[1], line[0]);
    line.last() = extend(line[line.size() - 2], line.last());

    QImage barrier(mask.size(), QImage::Format_Alpha8);
    barrier.fill(0);
    {
        QPainter p(&barrier);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor(0, 0, 0, 255), 2.0, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.drawPolyline(line);
    }
    QImage cut = mask.copy();
    const int w = mask.width(), h = mask.height();
    for (int y = 0; y < h; ++y) {
        uchar *row = cut.scanLine(y);
        const uchar *b = barrier.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (b[x])
                row[x] = 0;
        }
    }
    Components c = connectedComponents(cut, 128, false);
    // Discard tiny slivers; their pixels join the neighbours like the barrier pixels.
    std::vector<int> keep(size_t(c.count) + 1, 0);
    int kept = 0;
    for (int i = 1; i <= c.count; ++i) {
        if (c.areas[size_t(i)] >= minArea)
            keep[size_t(i)] = ++kept;
    }
    if (kept <= 1)
        return {mask};
    for (int &l : c.labels)
        l = l ? keep[size_t(l)] : 0;

    // Grow kept components into the remaining mask pixels (barrier + slivers) by BFS.
    std::vector<int> queue;
    queue.reserve(size_t(qsizetype(w) * 4));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (c.labels[size_t(qsizetype(y) * w + x)])
                queue.push_back(y * w + x);
        }
    }
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int idx = queue[qi];
        const int x = idx % w, y = idx / w;
        const int lab = c.labels[size_t(idx)];
        const int nbs[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto &n : nbs) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                continue;
            const int ni = n[1] * w + n[0];
            if (c.labels[size_t(ni)] == 0 && mask.constScanLine(n[1])[n[0]]) {
                c.labels[size_t(ni)] = lab;
                queue.push_back(ni);
            }
        }
    }
    QList<QPair<qint64, QImage>> pieces;
    for (int k = 1; k <= kept; ++k) {
        QImage m = createMask(mask.size(), 0);
        qint64 area = 0;
        for (int y = 0; y < h; ++y) {
            uchar *row = m.scanLine(y);
            const int *lab = c.labels.data() + qsizetype(y) * w;
            for (int x = 0; x < w; ++x) {
                if (lab[x] == k) {
                    row[x] = 255;
                    ++area;
                }
            }
        }
        pieces.append({area, m});
    }
    std::sort(pieces.begin(), pieces.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QList<QImage> out;
    for (const auto &p : pieces)
        out.append(p.second);
    return out;
}

QList<QImage> splitByNearestSegmentGroups(const QImage &maskIn, const QList<QLineF> &segments,
                                          const QList<int> &segmentGroup, int groupCount)
{
    const QImage mask = asMask(maskIn);
    QList<QImage> out;
    for (int g = 0; g < groupCount; ++g)
        out.append(createMask(mask.size(), 0));
    if (segments.isEmpty() || groupCount <= 0)
        return out;
    const QRect b = maskBounds(mask, 1);
    if (b.isEmpty())
        return out;
    for (int y = b.top(); y <= b.bottom(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = b.left(); x <= b.right(); ++x) {
            if (!row[x])
                continue;
            double best = std::numeric_limits<double>::max();
            int bestSeg = 0;
            for (int s = 0; s < segments.size(); ++s) {
                const double d = pointSegmentDistanceSq(x + 0.5, y + 0.5, segments[s]);
                if (d < best) {
                    best = d;
                    bestSeg = s;
                }
            }
            const int g = segmentGroup.value(bestSeg, bestSeg);
            if (g >= 0 && g < groupCount)
                out[g].scanLine(y)[x] = row[x];
        }
    }
    return out;
}

QList<QImage> splitByNearestSegment(const QImage &mask, const QList<QLineF> &segments)
{
    QList<int> groups(segments.size());
    std::iota(groups.begin(), groups.end(), 0);
    return splitByNearestSegmentGroups(mask, segments, groups, int(segments.size()));
}

QImage colorRegion(const QImage &imageIn, const QPoint &seed, int tolerance, bool contiguous, const QImage &limitIn)
{
    const QImage image = imageIn.convertToFormat(QImage::Format_ARGB32);
    const QImage limit = limitIn.isNull() ? QImage() : asMask(limitIn);
    if (!image.rect().contains(seed))
        return createMask(image.size(), 0);
    const QRgb ref = image.pixel(seed);
    auto accept = [&](int x, int y) {
        if (!limit.isNull() && limit.constScanLine(y)[x] < 128)
            return false;
        return colorClose(reinterpret_cast<const QRgb *>(image.constScanLine(y))[x], ref, tolerance);
    };
    if (contiguous)
        return floodFrom(image.size(), {seed}, accept);
    QImage out = createMask(image.size(), 0);
    for (int y = 0; y < image.height(); ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
            row[x] = accept(x, y) ? 255 : 0;
    }
    return out;
}

// ---- Label maps -------------------------------------------------------------------------------

QImage maskForLabel(const QImage &labels, int label, const QRect &rectIn)
{
    const QRect rect = rectIn.isNull() ? labels.rect() : rectIn.intersected(labels.rect());
    QImage out = createMask(rectIn.isNull() ? labels.size() : rect.size(), 0);
    const QPoint origin = rectIn.isNull() ? QPoint() : rect.topLeft();
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *src = labels.constScanLine(y);
        uchar *dst = out.scanLine(y - origin.y());
        for (int x = rect.left(); x <= rect.right(); ++x)
            dst[x - origin.x()] = src[x] == label ? 255 : 0;
    }
    return out;
}

QVector<qint64> labelHistogram(const QImage &labels)
{
    QVector<qint64> hist(256, 0);
    for (int y = 0; y < labels.height(); ++y) {
        const uchar *row = labels.constScanLine(y);
        for (int x = 0; x < labels.width(); ++x)
            ++hist[row[x]];
    }
    return hist;
}

QRect labelBounds(const QImage &labels, int label)
{
    int minX = labels.width(), minY = labels.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < labels.height(); ++y) {
        const uchar *row = labels.constScanLine(y);
        const uchar *first = std::find(row, row + labels.width(), uchar(label));
        if (first == row + labels.width())
            continue;
        minY = std::min(minY, y);
        maxY = y;
        minX = std::min(minX, int(first - row));
        for (int x = labels.width() - 1; x >= 0; --x) {
            if (row[x] == label) {
                maxX = std::max(maxX, x);
                break;
            }
        }
    }
    if (maxX < 0)
        return {};
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

void replaceLabel(QImage &labels, int from, int to)
{
    for (int y = 0; y < labels.height(); ++y) {
        uchar *row = labels.scanLine(y);
        std::replace(row, row + labels.width(), uchar(from), uchar(to));
    }
}

void paintLabel(QImage &labels, const QImage &maskIn, int label, int threshold)
{
    const QImage mask = asMask(maskIn);
    const QRect b = maskBounds(mask, threshold);
    for (int y = b.top(); y <= b.bottom(); ++y) {
        const uchar *m = mask.constScanLine(y);
        uchar *row = labels.scanLine(y);
        for (int x = b.left(); x <= b.right(); ++x) {
            if (m[x] >= threshold)
                row[x] = uchar(label);
        }
    }
}

QImage labelsFromMasks(const QSize &size, const QList<QImage> &masks, const QList<int> &labelIds,
                       const QImage &limitIn, int threshold)
{
    QImage labels = createMask(size, 0);
    for (int i = 0; i < masks.size() && i < labelIds.size(); ++i) {
        if (masks[i].size() == size)
            paintLabel(labels, masks[i], labelIds[i], threshold);
    }
    if (!limitIn.isNull()) {
        const QImage limit = asMask(limitIn);
        for (int y = 0; y < size.height(); ++y) {
            uchar *row = labels.scanLine(y);
            const uchar *l = limit.constScanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                if (l[x] < threshold)
                    row[x] = 0;
            }
        }
    }
    return labels;
}

QImage colorizeLabels(const QImage &labels, const QVector<QRgb> &palette, const QRect &rectIn)
{
    const QRect rect = rectIn.intersected(labels.rect());
    QImage out(rect.size(), QImage::Format_ARGB32_Premultiplied);
    if (rect.isEmpty())
        return out;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *src = labels.constScanLine(y) + rect.left();
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y - rect.top()));
        for (int x = 0; x < rect.width(); ++x)
            dst[x] = palette.value(src[x], 0);
    }
    return out;
}

QImage labelEdges(const QImage &labels, const QRect &rectIn, QRgb color)
{
    const QRect rect = rectIn.intersected(labels.rect());
    QImage out(rect.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    const int w = labels.width(), h = labels.height();
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *row = labels.constScanLine(y);
        const uchar *up = y > 0 ? labels.constScanLine(y - 1) : row;
        const uchar *down = y < h - 1 ? labels.constScanLine(y + 1) : row;
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y - rect.top()));
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const uchar v = row[x];
            if (!v)
                continue;
            const bool edge = (x > 0 && row[x - 1] != v) || (x < w - 1 && row[x + 1] != v) || up[x] != v || down[x] != v;
            if (edge)
                dst[x - rect.left()] = color;
        }
    }
    return out;
}

// ---- Layer stacks -----------------------------------------------------------------------------

QList<QImage> visibleMasksFromLayers(const QList<QImage> &layers, int alphaThreshold)
{
    QList<QImage> out;
    if (layers.isEmpty())
        return out;
    const QSize size = layers.first().size();
    QImage covered = createMask(size, 0);
    for (int i = 0; i < layers.size(); ++i)
        out.append(createMask(size, 0));
    for (int i = int(layers.size()) - 1; i >= 0; --i) {
        const QImage layer = layers[i].convertToFormat(QImage::Format_ARGB32);
        if (layer.size() != size)
            continue;
        for (int y = 0; y < size.height(); ++y) {
            const QRgb *src = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
            uchar *cov = covered.scanLine(y);
            uchar *dst = out[i].scanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                if (cov[x] || qAlpha(src[x]) < alphaThreshold)
                    continue;
                dst[x] = 255;
                cov[x] = 255;
            }
        }
    }
    return out;
}

QImage extensionMask(const QImage &own, const QImage &occluders, int radius)
{
    const QImage grown = dilateMask(asMask(own), radius);
    const QImage behind = combineMasks(grown, asMask(occluders), MaskOp::Intersect);
    return combineMasks(asMask(own), behind, MaskOp::Union);
}

int adaptiveExtensionRadius(qint64 area)
{
    return std::clamp(int(std::lround(std::sqrt(double(std::max<qint64>(area, 0))) * 0.35)), 8, 160);
}

QImage silhouetteMask(const QImage &imageIn, int tolerance)
{
    const QImage image = imageIn.convertToFormat(QImage::Format_ARGB32);
    const int w = image.width(), h = image.height();
    if (w == 0 || h == 0)
        return {};
    if (image.hasAlphaChannel()) {
        bool transparent = false;
        for (int y = 0; y < h && !transparent; y += std::max(1, h / 256)) {
            const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                if (qAlpha(row[x]) < 250) {
                    transparent = true;
                    break;
                }
            }
        }
        if (transparent)
            return thresholdMask(maskFromAlpha(image), 32);
    }
    // Background colour = median of the border pixels.
    std::vector<int> r, g, b;
    QList<QPoint> seeds;
    auto sample = [&](int x, int y) {
        const QRgb c = image.pixel(x, y);
        r.push_back(qRed(c));
        g.push_back(qGreen(c));
        b.push_back(qBlue(c));
        seeds.append(QPoint(x, y));
    };
    const int step = std::max(1, std::max(w, h) / 512);
    for (int x = 0; x < w; x += step) {
        sample(x, 0);
        sample(x, h - 1);
    }
    for (int y = 0; y < h; y += step) {
        sample(0, y);
        sample(w - 1, y);
    }
    auto median = [](std::vector<int> &v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const QRgb bg = qRgb(median(r), median(g), median(b));
    auto accept = [&](int x, int y) {
        const QRgb c = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x];
        return std::abs(qRed(c) - qRed(bg)) <= tolerance && std::abs(qGreen(c) - qGreen(bg)) <= tolerance
               && std::abs(qBlue(c) - qBlue(bg)) <= tolerance;
    };
    const QImage background = floodFrom(image.size(), seeds, accept);
    return invertMask(background);
}

} // namespace lumen::img
