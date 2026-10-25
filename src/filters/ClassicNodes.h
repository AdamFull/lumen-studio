#pragma once

namespace lumen::filters {

class NodeRegistry;

// Adds the classic image nodes (§17.6): input, output; levels, curves, brightness / contrast,
// hue / saturation, colour balance, invert, desaturate, threshold, posterize; Gaussian blur,
// sharpen, unsharp mask, noise, median; blend, mix by mask; mask from image, grow / shrink /
// feather, invert mask; resize, crop, flip, rotate. NodeRegistry::global() has them.
void registerClassicNodes(NodeRegistry &registry);

} // namespace lumen::filters
