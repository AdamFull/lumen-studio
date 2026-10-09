#pragma once

#include "pose/Skeleton.h"

#include <QImage>

namespace lumen::pose {

struct RenderOptions
{
    bool body = true;
    bool hands = true;
    bool face = true;
    // Stroke scale relative to the reference 512 px rendering of controlnet_aux. 0 = automatic
    // (short side / 512, at least 1), so maps rendered at high resolution match upscaled detector output.
    double thickness = 0.0;
    QColor background = Qt::black;
};

// Renders an OpenPose control map exactly following the controlnet_aux conventions
// (limb ellipses at 60 % colour, joint discs, rainbow hand edges, white face dots).
QImage renderOpenPose(const Pose &pose, const QSize &outputSize, const RenderOptions &options = {});

// Draws onto an existing painter-compatible image, mapping pose canvas coordinates into `target`.
void paintOpenPose(QImage &image, const Pose &pose, const QRectF &target, const RenderOptions &options);

} // namespace lumen::pose
