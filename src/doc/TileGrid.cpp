#include "doc/TileGrid.h"

#include "core/Async.h"

#include <QtConcurrent/QtConcurrentMap>

#include <cmath>

namespace lumen::doc {

void TileGrid::reset(const QSize &documentSize, QImage::Format format)
{
    m_size = documentSize;
    m_format = format;
    m_cols = (documentSize.width() + kTileSize - 1) / kTileSize;
    m_rows = (documentSize.height() + kTileSize - 1) / kTileSize;
    m_tiles.assign(static_cast<size_t>(m_cols * m_rows), Tile());
}

QRect TileGrid::tileRect(int col, int row) const
{
    return QRect(col * kTileSize, row * kTileSize, kTileSize, kTileSize).intersected(QRect(QPoint(0, 0), m_size));
}

void TileGrid::invalidate(const QRect &docRect)
{
    const QRect r = docRect.intersected(QRect(QPoint(0, 0), m_size));
    if (r.isEmpty())
        return;
    const int c0 = r.left() / kTileSize, c1 = r.right() / kTileSize;
    const int r0 = r.top() / kTileSize, r1 = r.bottom() / kTileSize;
    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            Tile &t = at(col, row);
            for (bool &d : t.dirty)
                d = true;
        }
    }
}

void TileGrid::invalidateAll()
{
    for (Tile &t : m_tiles) {
        for (bool &d : t.dirty)
            d = true;
    }
}

void TileGrid::update(const QRect &docRect, int level, const RenderFn &render)
{
    level = std::clamp(level, 0, kLevels - 1);
    const QRect r = docRect.intersected(QRect(QPoint(0, 0), m_size));
    if (r.isEmpty())
        return;
    struct Work
    {
        int col, row;
    };
    std::vector<Work> work;
    const int c0 = r.left() / kTileSize, c1 = r.right() / kTileSize;
    const int r0 = r.top() / kTileSize, r1 = r.bottom() / kTileSize;
    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            if (at(col, row).dirty[level])
                work.push_back({col, row});
        }
    }
    if (work.empty())
        return;
    auto compute = [this, level, &render](const Work &w) {
        Tile &t = at(w.col, w.row);
        const QRect rect = tileRect(w.col, w.row);
        if (t.dirty[0]) {
            QImage img(rect.size(), m_format);
            render(img, rect);
            t.levels[0] = img;
            t.dirty[0] = false;
        }
        for (int l = 1; l <= level; ++l) {
            if (!t.dirty[l])
                continue;
            const QImage &src = t.levels[l - 1];
            const QSize half(std::max(1, (src.width() + 1) / 2), std::max(1, (src.height() + 1) / 2));
            t.levels[l] = src.scaled(half, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            t.dirty[l] = false;
        }
    };
    // Tiles are independent: render in parallel. The caller (GUI thread) waits, but the work is split
    // across all cores so even a full 4K invalidation completes in a few milliseconds.
    if (work.size() == 1)
        compute(work.front());
    else
        QtConcurrent::blockingMap(async::pool(), work, compute);
}

const QImage &TileGrid::tile(int col, int row, int level) const
{
    if (col < 0 || row < 0 || col >= m_cols || row >= m_rows)
        return m_empty;
    return at(col, row).levels[std::clamp(level, 0, kLevels - 1)];
}

int TileGrid::levelForZoom(double zoom)
{
    if (zoom <= 0.25)
        return 2;
    if (zoom <= 0.5)
        return 1;
    return 0;
}

} // namespace lumen::doc
