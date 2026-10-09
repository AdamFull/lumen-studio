#pragma once

#include "live2d/CubismModel.h"

#include <QColor>
#include <QImage>
#include <QTransform>

namespace lumen::live2d {

struct RenderOptions
{
    QSize size{512, 512};
    QColor background = Qt::transparent;
    double zoom = 1.0;  // 1 = the model canvas fits the output
    QPointF pan;        // output pixels
};

// Model units -> output pixels for a frame rendered with `options`.
QTransform frameTransform(const ModelFrame &frame, const RenderOptions &options);

// Software renderer for Cubism drawables: textured meshes with bilinear sampling, normal /
// additive / multiplicative blending, clipping masks (also inverted) and multiply / screen colours.
// Rows are rendered in parallel bands. Thread-safe; textures must be ARGB32_Premultiplied.
QImage renderFrame(const ModelFrame &frame, const QList<QImage> &textures, const RenderOptions &options);

} // namespace lumen::live2d
