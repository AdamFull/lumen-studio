#pragma once

#include "filters/NodeType.h"

#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>

// The Develop tab's effects as filter nodes (plan M20 step 6): Develop, Local contrast, Tone curve,
// Colour mixer, Colour grading, LUT, Detail, Vignette and grain, each with the controls its shader
// declares. They run on the graphics card and hand on a pending image (PendingImage), so a row of
// them runs as one chain and the picture comes back only where another node, or the result, reads
// it. Also what their inputs are made of: tone curves as a picture and LUTs from .cube files.
namespace lumen::filters {

class NodeRegistry;

namespace effects {

// Adds the effect nodes (described from the shaders, without the graphics card).
void registerNodes(NodeRegistry &registry);
// The node id of a built-in effect ("local" → "local-contrast"); empty when it has no node.
QString nodeIdOf(const QString &effect);
// One of your own effects (gpu::Effect::addCompiled) as a node "effect:<name>" in the category
// Your effects, added again when it changes; removed when the effect is.
QString yourNodeId(const QString &name);
void registerYourNode(NodeRegistry &registry, const QString &effectId, const QString &name);
void removeYourNode(NodeRegistry &registry, const QString &name);
// A node type for an effect of your own as it is now (the effect editor's preview runs it).
NodeTypePtr yourNode(const QString &effectId, const QString &name);
// How many runs of effects have been made into pictures so far, for tests: a row of effect nodes
// read once counts one.
int chainRuns();

// A curve that changes nothing, as CurveEditor writes it.
QString straightCurve();
// The tone curves as the "curve" input, 256 × 1 half floats: red, green and blue each channel's
// curve, alpha the curve for all three, from 0 to 1. Keys "rgb", "r", "g", "b"; missing ones are
// straight.
QImage curveImage(const QHash<QString, QString> &curves);

// A .cube file as the "lut" input (N² × N, half floats); null with the reason when it cannot be read.
// Kept while the file does not change.
QImage loadCube(const QString &path, QString *error = nullptr);
// The LUTs folder's .cube files by name, and adding one there (a copy, after checking it can be read;
// a different file of the same name gets a number). Returns the name in the folder.
QStringList availableLuts();
QString addLut(const QString &path, QString *error = nullptr);

} // namespace effects
} // namespace lumen::filters
