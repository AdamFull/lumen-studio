#pragma once

#include <QImage>
#include <QRect>
#include <QSize>

#include <functional>
#include <vector>

namespace lumen::doc {

// Display cache split into fixed-size tiles with up to three mip levels. Only dirty tiles are
// recomputed (in parallel on the worker pool) and only their GPU textures are re-uploaded, which keeps
// brushing on very large documents smooth.
class TileGrid
{
public:
    static constexpr int kTileSize = 512;
    static constexpr int kLevels = 3;

    // Renders the given document-space rect into the provided image (already sized to the rect).
    using RenderFn = std::function<void(QImage &target, const QRect &docRect)>;

    void reset(const QSize &documentSize, QImage::Format format = QImage::Format_ARGB32_Premultiplied);
    void invalidate(const QRect &docRect);
    void invalidateAll();

    // Makes sure all tiles intersecting docRect are up to date at the requested level.
    void update(const QRect &docRect, int level, const RenderFn &render);

    int columns() const { return m_cols; }
    int rows() const { return m_rows; }
    QSize documentSize() const { return m_size; }
    QRect tileRect(int col, int row) const;
    const QImage &tile(int col, int row, int level) const;
    // Picks the mip level for a zoom factor (1 = 100 %).
    static int levelForZoom(double zoom);

private:
    struct Tile
    {
        QImage levels[kLevels];
        bool dirty[kLevels] = {true, true, true};
    };
    Tile &at(int col, int row) { return m_tiles[static_cast<size_t>(row * m_cols + col)]; }
    const Tile &at(int col, int row) const { return m_tiles[static_cast<size_t>(row * m_cols + col)]; }

    QSize m_size;
    QImage::Format m_format = QImage::Format_ARGB32_Premultiplied;
    int m_cols = 0;
    int m_rows = 0;
    std::vector<Tile> m_tiles;
    QImage m_empty;
};

} // namespace lumen::doc
