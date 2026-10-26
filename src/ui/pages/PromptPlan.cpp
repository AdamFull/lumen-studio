#include "ui/pages/PromptPlan.h"

#include "ai/GenerationService.h"
#include "doc/Document.h"

#include <cmath>

namespace lumen::ui {

bool PromptPlan::canRun() const
{
    switch (action) {
    case PromptAction::EditArea:
    case PromptAction::InpaintArea:
    case PromptAction::RedrawArea:
    case PromptAction::EditImage:
    case PromptAction::NewImage:
        return true;
    default:
        return false;
    }
}

PromptPlan promptPlan(bool selection, bool blankCanvas, const ai::ImageCapabilities *caps)
{
    PromptPlan p;
    if (!caps)
        return p;
    const bool changes = caps->edit || caps->inpaint || caps->imageToImage;
    // A selection wins over a blank canvas: drawing something into an area of an empty canvas is
    // still a change of that area.
    if (selection && changes) {
        p.region = true;
        if (caps->edit && caps->regionEdit) {
            p.action = PromptAction::EditArea;
            p.mode = ai::GenMode::Edit;
        } else if (caps->inpaint || caps->edit) {
            // The region pipeline inpaints for engines that edit with a mask (cloud edit APIs).
            p.action = PromptAction::InpaintArea;
            p.mode = ai::GenMode::Inpaint;
            p.strength = true;
        } else {
            p.action = PromptAction::RedrawArea;
            p.mode = ai::GenMode::ImageToImage;
            p.strength = true;
        }
        return p;
    }
    if (blankCanvas) {
        p.action = caps->textToImage ? PromptAction::NewImage : PromptAction::CannotCreate;
        p.mode = ai::GenMode::TextToImage;
        return p;
    }
    if (!changes) {
        p.action = PromptAction::CannotChangeImages;
        return p;
    }
    if (caps->edit) {
        p.action = PromptAction::EditImage;
        p.mode = ai::GenMode::Edit;
        return p;
    }
    p.action = PromptAction::CannotEditImage;
    p.mode = ai::GenMode::ImageToImage;
    p.strength = true;
    p.variation = caps->imageToImage;
    return p;
}

RemovalRequest removalRequest(const PromptPlan &plan)
{
    // The words go to the model as they are (English, like the rest of a prompt).
    if (plan.mode == ai::GenMode::Edit)
        return {ai::GenMode::Edit,
                QStringLiteral("Remove the object in the middle of the picture and fill its place with what is around it, as if it had "
                               "never been there. Keep everything else exactly as it is."),
                1.0};
    return {plan.mode == ai::GenMode::Inpaint ? ai::GenMode::Inpaint : ai::GenMode::ImageToImage,
            QStringLiteral("empty background, seamless continuation of the surroundings"), 1.0};
}

bool isBlankCanvas(const doc::Document &doc)
{
    for (const doc::LayerPtr &layer : doc.layers()) {
        const QImage &im = layer->image;
        if (im.isNull() || im.width() == 0 || im.height() == 0)
            continue;
        if (im.depth() != 32)
            return false;
        // One colour (transparent counts): stops at the first pixel that differs, so a picture is
        // told from an empty canvas quickly.
        const QRgb first = reinterpret_cast<const QRgb *>(im.constScanLine(0))[0];
        for (int y = 0; y < im.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(im.constScanLine(y));
            for (int x = 0; x < im.width(); ++x) {
                if (line[x] != first)
                    return false;
            }
        }
    }
    return true;
}

QSize workingSize(const QSize &picture, const ai::ImageCapabilities &caps)
{
    if (picture.isEmpty())
        return picture;
    // The size a whole picture is changed at, shared with the AI panel's actions and the filters'
    // AI steps; a new image needs one of the fixed sizes, while a change leaves that to the provider.
    if (caps.fixedSizes.isEmpty())
        return ai::wholeImageWorkSize(picture, caps);
    const double aspect = double(picture.width()) / picture.height();
    QSize best = caps.fixedSizes.first();
    for (const QSize &s : caps.fixedSizes) {
        if (std::abs(std::log(double(s.width()) / s.height() / aspect)) < std::abs(std::log(double(best.width()) / best.height() / aspect)))
            best = s;
    }
    return best;
}

} // namespace lumen::ui
