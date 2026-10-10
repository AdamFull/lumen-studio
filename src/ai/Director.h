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
    // Appearance tags of a character sheet: its "tags", else built from hair, eyes and outfit.
    static QString appearanceTags(const QJsonObject &sheet);

    static QJsonObject characterSheetSchema();
    static QJsonObject shotSchema();
    static QJsonObject shotPlanSchema(int maxShots = 12);
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
    static pose::Pose poseForShot(const QJsonObject &shot, const QSize &canvas);

private:
    LlmProvider *llm(LlmRole role, QString *model) const;
    ProviderRegistry *m_registry;
};

} // namespace lumen::ai
