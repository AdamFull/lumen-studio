#pragma once

#include "ai/CharacterContext.h"
#include "ai/GenerationService.h"
#include "pose/CastLayout.h"

#include <optional>

namespace lumen::ai {

struct PromptStyle;

// Two characters in one image (M16 §16.2), "compose, then refine": a base image of the scene with
// a pose per person and no character LoRA, then one inpaint per character in its region with its
// own LoRA (or reference), from back to front. Works with any back-end that inpaints.
struct CastRequest
{
    QString providerId;          // empty = default image provider
    // The scene as the user wrote it (no character words), plus the usual settings: size, seed,
    // steps, CFG, sampler, model, negative prompt. Mode, references and controls are set here.
    ImageRequest scene;
    QList<CharacterContext> cast; // one or two, left (or front) first
    pose::Arrangement arrangement = pose::Arrangement::SideBySide;
    std::optional<pose::Pose> pose; // the people, one per member; else default rigs, arranged
    double refineDenoise = 0.55;
    QString modelFamily;         // the loaded checkpoint's family, for the LoRA check
    QString title;
    QJsonObject meta;
    bool saveAssets = true;
};

// "girl", "boy" or "other" from a sheet's gender presentation.
QString castSubject(const QJsonObject &sheet);
// Count tags for the cast: "1girl" / "2girls" / "1boy, 1girl" / "2boys"; empty when unknown.
QString castCountTags(const QList<CharacterContext> &cast);
// A character's appearance without subject and count tags ("1girl", "solo"), which describe the
// image, not the character.
QString castAppearance(const QJsonObject &sheet, const PromptStyle &style);
// The default people of a cast: a standing rig per member, arranged.
pose::Pose castPose(const CastRequest &request, const QSize &size);

// The base request: count tags, the scene, each member's appearance tied to its side; the whole
// pose; no character LoRA or reference.
ImageRequest castBaseRequest(const CastRequest &request, const pose::Pose &pose, const PromptStyle &style, const ImageCapabilities &caps);
// The refine request of member `index` (its person in the pose): its subject and appearance, the
// scene, then applyCharacter(Region) for its LoRA or reference; inpaint at `refineDenoise` with that
// person's pose only. `applied` receives what went in.
ImageRequest castRefineRequest(const CastRequest &request, int index, const pose::Pose &pose, const PromptStyle &style,
                               const ImageCapabilities &caps, CharacterApplied *applied = nullptr);

// Runs the pipeline as one Queue job in the provider's group; the result is stored like any
// generation (one asset, unless `saveAssets` is off), with the cast in its metadata ("cast").
Job *startCastGeneration(GenerationService *service, const CastRequest &request);

} // namespace lumen::ai
