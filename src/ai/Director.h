#pragma once

#include "ai/PromptStyle.h"
#include "ai/ProviderRegistry.h"
#include "ai/StructuredChat.h"
#include "pose/Rig.h"

#include <QJsonObject>
#include <QObject>

namespace lumen::ai {

// What the set of images is for; shapes the plan (views, poses, backgrounds, expressions).
struct PurposeProfile
{
    QString id;
    QString name;
    QString description;
    QString guidance;      // instructions for the director LLM
    int defaultCount = 3;
    QString defaultAspect; // e.g. "2:3"
};
QList<PurposeProfile> purposeProfiles();
PurposeProfile purposeProfile(const QString &id);

struct PlanInput
{
    QString request;          // user's description of what they need
    QJsonObject character;    // character sheet (may be empty)
    // A second character's sheet (M16 §16.4): shots then show both, with a pose per person.
    QJsonObject partner;
    QString purposeId = QStringLiteral("general");
    int count = 0;            // 0 = let the director decide (purpose default)
    QList<QImage> references; // sent to vision-capable models when no sheet is available
    PromptStyle style;
};

// LLM-driven planning: analyses references into a character sheet, plans shots with poses and
// prompts for a purpose, and revises poses from natural-language instructions.
class Director : public QObject
{
    Q_OBJECT
public:
    explicit Director(ProviderRegistry *registry, QObject *parent = nullptr);

    StructuredChatOperation *analyzeCharacter(const QList<QImage> &references, const QString &notes);
    StructuredChatOperation *planShots(const PlanInput &input);
    StructuredChatOperation *revisePose(const QJsonObject &shot, const QString &instruction);
    StructuredChatOperation *rewritePrompt(const QJsonObject &shot, const QJsonObject &character, const QString &instruction,
                                           const PromptStyle &style);

    // Critic: a vision model compares a generated shot with the character references, the sheet and
    // the planned shot. The answer follows critiqueSchema().
    StructuredChatOperation *critiqueShot(const QImage &result, const QJsonObject &shot, const QJsonObject &character,
                                          const QList<QImage> &references);
    static QJsonObject critiqueSchema();
    // Danbooru-style tags of a training image (view, framing, pose, expression, clothes, hair, eyes,
    // background) for LoRA captions; the answer follows imageTagsSchema() (ai/LoraCaptions.h).
    StructuredChatOperation *tagImage(const QImage &image);
    // Overall score 0-10 of a critique (the minimum of overall, identity and pose).
    static int critiqueScore(const QJsonObject &critique);
    static bool critiqueAccepts(const QJsonObject &critique, int threshold);
    // The shot with the critic's revised prompt and negative additions applied (for a retry).
    static QJsonObject applyCritique(const QJsonObject &shot, const QJsonObject &critique);

    // Removes pose-rig parameters that models sometimes copy into prompts ("raise 35, bend_twist 0")
    // and tidies the separators left behind.
    static QString cleanPrompt(const QString &prompt);
    // A plan with cleaned prompts, negative prompts and descriptions.
    static QJsonObject sanitizePlan(const QJsonObject &plan);

    // Shots for a character LoRA's training set (M13), built without a language model: views ×
    // framings × expressions × arm poses on plain, changing backgrounds, so that only the character
    // stays the same. count 0 = 24; at most kMaxDatasetShots.
    static constexpr int kMaxDatasetShots = 32;
    static QJsonObject datasetPlan(const QJsonObject &sheet, int count = 0);
    // Test shots for a trained LoRA (M13 §13.6): poses, views and backgrounds that are not in the
    // training set. The prompt spells out the appearance tags, as shots planned in Create do; with
    // `trigger` it starts with the trigger word (for the LoRA candidates).
    static QList<QJsonObject> loraTestShots(const QJsonObject &sheet, const QString &trigger);
    // Score of a test image: identity counts twice as much as the pose. Takes a checkIdentity()
    // answer (identity from the checklist) or a critiqueShot() verdict.
    static double loraTestScore(const QJsonObject &critique);
    // Identity checklist (LoRA test): the vision model marks each of the character's features as
    // matching, close, different or not visible in the image, and scores the pose against the
    // planned shot. Stricter than critiqueShot()'s identity, which gives 9-10 to anything close.
    StructuredChatOperation *checkIdentity(const QImage &result, const QJsonObject &shot, const QJsonObject &character,
                                           const QList<QImage> &references);
    // The features checked: the face, then the sheet's appearance tags (at most 11).
    static QStringList identityChecklist(const QJsonObject &sheet);
    static QJsonObject identityChecklistSchema(int features);
    // Identity 0-10 from a checklist answer: matching features count 1, close ones ½, among the
    // visible ones. -1 when no feature is visible.
    static double checklistIdentity(const QJsonObject &answer);
    // Appearance tags of a character sheet: its "tags", else built from hair, eyes and outfit.
    static QString appearanceTags(const QJsonObject &sheet);

    static QJsonObject characterSheetSchema();
    // With `cast` (two characters), a shot also has "people" (per person: the character's name, its
    // pose and what it does, left or front first) and "arrangement".
    static QJsonObject shotSchema(bool cast = false);
    static QJsonObject shotPlanSchema(int maxShots = 12, bool cast = false);
    // Plain-text summary of a character sheet (for prompts and UI).
    static QString describeCharacter(const QJsonObject &sheet);

    // Converts a planned shot into an image request. The pose map is rendered at the generation size.
    static ImageRequest buildShotRequest(const QJsonObject &shot, const QList<ReferenceImage> &references,
                                         const pose::Pose &pose, const PromptStyle &style, const ImageCapabilities &caps);
    static QSize sizeForAspect(const QString &aspect, qint64 pixels, int multiple);
    // Adds a character's LoRA and puts its trigger word first in the prompt. Returns false (and
    // leaves the request alone) when the provider cannot use LoRAs.
    static bool applyCharacterLora(ImageRequest &request, const CharacterLora &lora, const ImageCapabilities &caps);
    // Pose for a shot (rig parameters → keypoints) on a canvas of the given size.
    // One person from the shot's "pose", or with "people" of two, both, arranged as the shot says.
    static pose::Pose poseForShot(const QJsonObject &shot, const QSize &canvas);

private:
    LlmProvider *llm(LlmRole role, QString *model) const;
    ProviderRegistry *m_registry;
};

} // namespace lumen::ai
