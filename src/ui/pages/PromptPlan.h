#pragma once

#include "ai/Types.h"

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

// What the prompt bar's Generate does now (promptPlan).
enum class PromptAction {
    NoEngine,           // nothing can draw yet
    EditArea,           // an instruction edit of the selection and its surroundings, blended back through it
    InpaintArea,        // the selection is painted anew from the description (inpainting)
    RedrawArea,         // the selection's surroundings are redrawn (img2img) and blended back through it
    EditImage,          // an instruction edit of the whole image
    CannotEditImage,    // the engine cannot change the whole image from words: select an area, or a variation
    NewImage,           // text to image at the canvas size (a blank canvas)
    CannotCreate,       // a blank canvas, and the engine cannot draw from a description
    CannotChangeImages, // the engine only makes new images
};

struct PromptPlan
{
    PromptAction action = PromptAction::NoEngine;
    ai::GenMode mode = ai::GenMode::TextToImage; // what the request asks for
    bool region = false;                          // through the selection (the region pipeline)
    bool strength = false;                        // the strength applies (inpainting, img2img)
    // CannotEditImage: a variation of the whole image (img2img at the variation strength) can be made.
    bool variation = false;

    bool canRun() const;
};

// Chooses what Generate does from whether there is a selection, whether the canvas is blank and what
// the image engine can do (null: no engine). With a selection, an instruction edit of the area when
// the engine edits regions (local edit models), else inpainting, else img2img of the area; without
// one, an instruction edit of the whole image, else nothing (a variation is offered); on a blank
// canvas, a new image.
PromptPlan promptPlan(bool selection, bool blankCanvas, const ai::ImageCapabilities *caps);

// What Remove sends for a plan with a selection: the same kind of request with words that ask for
// the area to be filled from its surroundings, at full strength.
struct RemovalRequest
{
    ai::GenMode mode;
    QString prompt;
    double strength;
};
RemovalRequest removalRequest(const PromptPlan &plan);

// Whether a document has nothing on it yet: every layer is empty or one flat colour (a new blank
// canvas, a new document from a prompt).
bool isBlankCanvas(const doc::Document &doc);

// The size an engine works at for a picture of this size: the closest of its fixed sizes, else the
// same shape at its native pixel count. Results are scaled back to the picture.
QSize workingSize(const QSize &picture, const ai::ImageCapabilities &caps);

} // namespace lumen::ui
