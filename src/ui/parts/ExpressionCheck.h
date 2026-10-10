#pragma once

#include "ai/CharacterContext.h"

#include <QImage>
#include <QRect>
#include <utility>

namespace lumen::doc {
class PartSet;
}

namespace lumen::ui {

class Studio;

// Face check of an expression variant (M14 §14.4): the identity checklist (Director::checkIdentity)
// compares the face with the variant painted in against the face before, and the character's
// reference. A feature marked "different" (another eye colour, a changed face shape) or an
// expression that barely shows becomes the variant part's warning; a clean check clears it.
// Needs a vision model; without one, or when the check fails, nothing changes.
void checkExpressionVariant(Studio *studio, doc::PartSet *set, int label, const ai::CharacterContext &character,
                            const QString &characterSummary, const QString &expression);

// The face area around a variant: before and after, at least 256 px wide and high where the
// source allows. Worker-thread safe.
std::pair<QImage, QImage> expressionCrops(const QImage &source, const QImage &layer, const QRect &rect);

} // namespace lumen::ui
