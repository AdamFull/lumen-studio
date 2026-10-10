#include "ai/Director.h"

#include "ai/LoraCaptions.h"
#include "ai/ProviderUtil.h"
#include "core/ImageOps.h"
#include "core/JsonUtil.h"
#include "pose/Anatomy.h"
#include "pose/PoseRenderer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace lumen::ai {

namespace {

const QStringList kAspects = {QStringLiteral("1:1"), QStringLiteral("2:3"), QStringLiteral("3:4"), QStringLiteral("9:16"),
                              QStringLiteral("3:2"), QStringLiteral("4:3"), QStringLiteral("16:9")};

QJsonObject str(const QString &description = QString())
{
    QJsonObject o{{QStringLiteral("type"), QStringLiteral("string")}};
    if (!description.isEmpty())
        o.insert(QStringLiteral("description"), description);
    return o;
}

QJsonObject strArray(const QString &description = QString())
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), str()}, {QStringLiteral("description"), description}};
}

QJsonObject object(const QJsonObject &props)
{
    QJsonArray req;
    for (auto it = props.begin(); it != props.end(); ++it)
        req.append(it.key());
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), props},
                       {QStringLiteral("required"), req},
                       {QStringLiteral("additionalProperties"), false}};
}

QJsonObject enumOf(const QStringList &values)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), json::toArray(values)}};
}

QString pretty(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

} // namespace

// ---- purposes ---------------------------------------------------------------------------------------

QList<PurposeProfile> purposeProfiles()
{
    return {
        {QStringLiteral("general"), QObject::tr("General illustrations"), QObject::tr("Any set of character images."),
         QStringLiteral("Plan varied, attractive illustrations that fit the user's request. Vary poses, framing and camera angles "
                        "meaningfully while keeping the character identical."),
         3, QStringLiteral("2:3")},
        {QStringLiteral("live2d"), QObject::tr("Live2D / VTuber model"), QObject::tr("Clean, rig-ready source art for Live2D Cubism."),
         QStringLiteral(
             "The images are source art for a Live2D / VTuber rig. Requirements: full body or knees-up framing, front view "
             "(yaw 0, head yaw 0, head pitch 0), symmetric A-pose with both arms angled 25-40 degrees away from the torso "
             "(raise 25-40, direction 90, bend 0-10) so arms never overlap the body, legs straight and slightly apart, hands "
             "relaxed or open and fully visible, hair not covering the eyes, eyes open looking at the viewer, closed neutral "
             "mouth unless an expression variant is required, no props, no motion blur, even soft lighting, plain flat "
             "light-grey background, clean line art and flat-ish cel shading that separates easily into parts. When several "
             "images are requested, the first must be the neutral front base; further images are variants with the SAME pose "
             "and framing for rig parts (e.g. eyes closed, mouth open/smile) or alternative angles (three-quarter, side) only "
             "if the user asks for turning motion."),
         3, QStringLiteral("2:3")},
        {QStringLiteral("turnaround"), QObject::tr("Character turnaround"), QObject::tr("Front, three-quarter, side and back views."),
         QStringLiteral("Plan a model sheet: the same neutral standing pose seen from the front (yaw 0), three-quarter (yaw 35), "
                        "side (yaw 90) and back (yaw 180), full body framing, orthographic camera (perspective 0), plain white "
                        "background, consistent scale, arms slightly away from the body."),
         4, QStringLiteral("2:3")},
        {QStringLiteral("expressions"), QObject::tr("Expression sheet"), QObject::tr("Headshots with different emotions."),
         QStringLiteral("Plan bust or headshot framing, front or slight three-quarter view, the same pose for every image, plain "
                        "background, each image showing a clearly different emotion (use the expression field and describe it "
                        "in the prompt)."),
         6, QStringLiteral("1:1")},
        {QStringLiteral("stickers"), QObject::tr("Sticker / emote set"), QObject::tr("Expressive poses for chat stickers or emotes."),
         QStringLiteral("Plan expressive, readable poses and emotions suitable for stickers: upper body or full body, bold "
                        "gestures (waving, thumbs up, peace sign, crying, laughing, thinking), simple solid white background, "
                        "clean outlines, the character centered with margin around it."),
         6, QStringLiteral("1:1")},
        {QStringLiteral("game"), QObject::tr("Game sprites / portraits"), QObject::tr("Idle, action and portrait shots for games."),
         QStringLiteral("Plan game-ready art: an idle stance, action poses (attack, run, jump) with dynamic angles, and a portrait. "
                        "Use plain backgrounds for sprites and an atmospheric background for the portrait."),
         4, QStringLiteral("2:3")},
        {QStringLiteral("portraits"), QObject::tr("Portrait set"), QObject::tr("Profile pictures and avatars."),
         QStringLiteral("Plan polished portraits (bust or headshot) with flattering lighting and varied but tasteful backgrounds, "
                        "different head angles and expressions."),
         3, QStringLiteral("1:1")},
        {QStringLiteral("lora_dataset"), QObject::tr("LoRA training set"),
         QObject::tr("20–32 varied images of one character for training its LoRA."),
         QStringLiteral("The images train a LoRA of this character, so only the character may stay the same. Vary the view (front, "
                        "three-quarter both ways, side, back), the framing (full body, cowboy shot, upper body, headshot), the "
                        "expression and the arm pose. Always the same outfit, one character, plain or simple backgrounds in "
                        "changing light colours, no props, no other people, no text."),
         24, QStringLiteral("2:3")},
    };
}

PurposeProfile purposeProfile(const QString &id)
{
    for (const PurposeProfile &p : purposeProfiles()) {
        if (p.id == id)
            return p;
    }
    return purposeProfiles().first();
}

// ---- schemas ---------------------------------------------------------------------------------------

QJsonObject Director::characterSheetSchema()
{
    return object(QJsonObject{
        {QStringLiteral("name"), str(QStringLiteral("character name or a short descriptive name"))},
        {QStringLiteral("summary"), str(QStringLiteral("one sentence"))},
        {QStringLiteral("gender_presentation"), str()},
        {QStringLiteral("apparent_age"), str()},
        {QStringLiteral("body_type"), str()},
        {QStringLiteral("hair"), object(QJsonObject{{QStringLiteral("color"), str()}, {QStringLiteral("length"), str()}, {QStringLiteral("style"), str()}})},
        {QStringLiteral("eyes"), object(QJsonObject{{QStringLiteral("color"), str()}, {QStringLiteral("shape"), str()}})},
        {QStringLiteral("skin_tone"), str()},
        {QStringLiteral("outfit"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                               {QStringLiteral("items"), object(QJsonObject{{QStringLiteral("item"), str()},
                                                                                            {QStringLiteral("color"), str()},
                                                                                            {QStringLiteral("details"), str()}})}}},
        {QStringLiteral("accessories"), strArray()},
        {QStringLiteral("color_palette"), strArray(QStringLiteral("main colors, most important first"))},
        {QStringLiteral("distinctive_features"), strArray(QStringLiteral("details that must never change (marks, ears, tails...)"))},
        {QStringLiteral("art_style"), str(QStringLiteral("rendering style of the references"))},
        {QStringLiteral("tags"), str(QStringLiteral("comma-separated Danbooru-style appearance tags"))},
        {QStringLiteral("description"), str(QStringLiteral("one dense natural-language paragraph describing the full appearance"))},
        {QStringLiteral("negative_hints"), str(QStringLiteral("things image models tend to get wrong for this character"))},
    });
}

QJsonObject Director::shotSchema()
{
    return object(QJsonObject{
        {QStringLiteral("id"), str(QStringLiteral("short identifier, e.g. shot1"))},
        {QStringLiteral("title"), str()},
        {QStringLiteral("description"), str(QStringLiteral("what this image shows and why"))},
        {QStringLiteral("aspect_ratio"), enumOf(kAspects)},
        {QStringLiteral("background"), str()},
        {QStringLiteral("pose"), pose::rigParamsSchema()},
        {QStringLiteral("prompt"), str(QStringLiteral("complete positive prompt for the image model"))},
        {QStringLiteral("negative_prompt"), str()},
    });
}

QJsonObject Director::shotPlanSchema(int maxShots)
{
    QJsonObject shots{{QStringLiteral("type"), QStringLiteral("array")},
                      {QStringLiteral("minItems"), 1},
                      {QStringLiteral("maxItems"), std::max(1, maxShots)},
                      {QStringLiteral("items"), shotSchema()}};
    return object(QJsonObject{{QStringLiteral("title"), str()}, {QStringLiteral("summary"), str()}, {QStringLiteral("shots"), shots}});
}

QJsonObject Director::critiqueSchema()
{
    auto integer = [](const char *desc) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0},
                           {QStringLiteral("maximum"), 10}, {QStringLiteral("description"), QLatin1String(desc)}};
    };
    // Length limits keep small local models from looping inside a string (grammar-constrained servers enforce them).
    auto string = [](const char *desc, int maxLength) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), QLatin1String(desc)},
                           {QStringLiteral("maxLength"), maxLength}};
    };
    const QJsonObject properties{
        {QStringLiteral("overall"), integer("overall quality and fitness for the shot's purpose, 0-10")},
        {QStringLiteral("identity"), integer("how well the character matches the references and the sheet (face, hair, colors, outfit), 0-10")},
        {QStringLiteral("pose"), integer("how well pose, framing and camera match the planned shot, 0-10")},
        {QStringLiteral("issues"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                               {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                                                     {QStringLiteral("maxLength"), 200}}},
                                               {QStringLiteral("maxItems"), 6},
                                               {QStringLiteral("description"), QStringLiteral("concrete problems, most important first (at most 6)")}}},
        {QStringLiteral("revised_prompt"), string("complete improved prompt for a retry; keep the prompt's style and fix the issues", 800)},
        {QStringLiteral("negative_additions"), string("comma-separated terms to add to the negative prompt, or empty", 300)},
    };
    QJsonArray required;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        required.append(it.key());
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), required},
                       {QStringLiteral("additionalProperties"), false}};
}

int Director::critiqueScore(const QJsonObject &c)
{
    const int overall = c.value(QStringLiteral("overall")).toInt(0);
    const int identity = c.value(QStringLiteral("identity")).toInt(overall);
    const int pose = c.value(QStringLiteral("pose")).toInt(overall);
    return std::clamp(std::min({overall, identity, pose}), 0, 10);
}

bool Director::critiqueAccepts(const QJsonObject &critique, int threshold)
{
    return critiqueScore(critique) >= threshold;
}

QJsonObject Director::applyCritique(const QJsonObject &shot, const QJsonObject &critique)
{
    QJsonObject out = shot;
    // Small models pad the prompt with negations and repeats: keep each positive item once.
    QStringList items;
    for (const QString &part : cleanPrompt(critique.value(QStringLiteral("revised_prompt")).toString()).split(u',')) {
        const QString item = part.trimmed();
        const QString lower = item.toLower();
        if (item.isEmpty() || lower == QLatin1String("no") || lower.startsWith(QLatin1String("no ")) || lower.startsWith(QLatin1String("without "))
            || item.contains(QChar::ReplacementCharacter))
            continue;
        if (!items.contains(item, Qt::CaseInsensitive))
            items << item;
    }
    if (!items.isEmpty())
        out.insert(QStringLiteral("prompt"), items.join(QStringLiteral(", ")));
    QStringList negative;
    for (const QString &term : shot.value(QStringLiteral("negative_prompt")).toString().split(u',', Qt::SkipEmptyParts))
        negative << term.trimmed();
    int added = 0;
    for (const QString &term : critique.value(QStringLiteral("negative_additions")).toString().split(u',', Qt::SkipEmptyParts)) {
        const QString t = term.trimmed();
        if (added >= 8)
            break;
        if (t.isEmpty() || t.contains(QChar::ReplacementCharacter) || negative.contains(t, Qt::CaseInsensitive))
            continue;
        negative << t;
        ++added;
    }
    out.insert(QStringLiteral("negative_prompt"), negative.join(QStringLiteral(", ")));
    return out;
}

StructuredChatOperation *Director::critiqueShot(const QImage &result, const QJsonObject &shot, const QJsonObject &character,
                                                const QList<QImage> &references)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::Vision, &model);
    ChatRequest req;
    req.model = model;
    req.temperature = 0.1;
    req.jsonSchema = critiqueSchema();
    req.schemaName = QStringLiteral("shot_critique");
    req.maxTokens = 1500;
    req.messages << ChatMessage::system(QStringLiteral(
        "You are a strict art director reviewing AI-generated images of a recurring character.\n"
        "- identity: compare the character's face, hair, eye color, outfit and colors with the reference images and the "
        "character sheet. The references only show who the character is: ignore their pose, framing and background.\n"
        "- pose: compare pose, framing, camera angle and background with the PLANNED SHOT, not with the references.\n"
        "- overall: image quality; below 6 for anatomy errors, extra limbs, broken hands or artifacts.\n"
        "Score honestly: 9-10 only when everything matches. List at most 6 concrete issues.\n"
        "revised_prompt: the shot's prompt rewritten to fix the issues, in the same style (tags stay tags, prose stays prose), "
        "under 60 words, keeping the planned pose and framing. Never write negations like 'no X' in it; put unwanted things "
        "in negative_additions (a few comma-separated terms)."));
    ChatMessage user = ChatMessage::user(QStringLiteral("Planned shot:\n%1\n\nCharacter sheet:\n%2\n\nThe first image is the "
                                                        "generated result; the others are references of the character.")
                                             .arg(QString::fromUtf8(QJsonDocument(shot).toJson(QJsonDocument::Compact)),
                                                  QString::fromUtf8(QJsonDocument(character).toJson(QJsonDocument::Compact)).left(8000)));
    user.images.append(ChatImage{img::thumbnail(result, 1024), QStringLiteral("Generated result:")});
    for (int i = 0; i < std::min<int>(2, int(references.size())); ++i)
        user.images.append(ChatImage{img::thumbnail(references[i], 768), QStringLiteral("Reference %1:").arg(i + 1)});
    req.messages << user;
    if (!provider || !provider->capabilities().vision) {
        auto *op = new StructuredChatOperation(provider, req);
        failLater(op, provider ? tr("The model assigned to 'Vision analysis' (%1) cannot see images, so it cannot review results.")
                                     .arg(provider->displayName())
                               : tr("No language model is configured for 'Vision analysis'."));
        return op;
    }
    return requestJson(provider, req);
}

StructuredChatOperation *Director::tagImage(const QImage &image)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::Vision, &model);
    ChatRequest req;
    req.model = model;
    req.temperature = 0.1;
    req.jsonSchema = imageTagsSchema();
    req.schemaName = QStringLiteral("image_tags");
    req.maxTokens = 800;
    req.messages << ChatMessage::system(QStringLiteral(
        "You tag images for training an anime image model. Answer with Danbooru tags (lower case, spaces instead of "
        "underscores) for what is visible, most important first, 15-40 tags:\n"
        "- people count (1girl, 1boy, solo);\n"
        "- view and camera angle (from side, from behind, from above, from below, looking at viewer, looking back);\n"
        "- framing (full body, cowboy shot, upper body, portrait, close-up);\n"
        "- pose and gestures (standing, sitting, walking, arms at sides, hand on hip, waving, crossed arms, peace sign);\n"
        "- expression, mouth and eyes (smile, open mouth, closed eyes, angry, blush);\n"
        "- hair colour, length and style; eye colour; body features;\n"
        "- each clothing item and accessory with its colour;\n"
        "- background (simple background, white background, gradient background) and lighting.\n"
        "Only tag what you can see. No sentences, no artist or quality tags."));
    ChatMessage user = ChatMessage::user(QStringLiteral("Tag this image."));
    user.images.append(ChatImage{img::thumbnail(image, 1024), QStringLiteral("Image:")});
    req.messages << user;
    if (!provider || !provider->capabilities().vision) {
        auto *op = new StructuredChatOperation(provider, req);
        failLater(op, provider ? tr("The model assigned to 'Vision analysis' (%1) cannot see images, so it cannot write captions.")
                                     .arg(provider->displayName())
                               : tr("No language model is configured for 'Vision analysis'."));
        return op;
    }
    return requestJson(provider, req);
}

QString Director::cleanPrompt(const QString &prompt)
{
    static const QRegularExpression rigValue(
        QStringLiteral("\\b(?:camera_pitch|bend_twist|raise|direction|bend|twist|yaw|pitch|roll|lean|tilt|perspective)\\s*[:=]?\\s*"
                       "-?\\d+(?:\\.\\d+)?\\s*(?:degrees|deg|°)?"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rigName(QStringLiteral("\\b(?:left|right)_(?:arm|leg|hand)\\b|\\b(?:camera_pitch|bend_twist|include_hands|include_face)\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    QString out = prompt;
    out.remove(rigValue);
    out.remove(rigName);
    // Tidy what is left: empty comma-separated items and doubled spaces.
    QStringList parts;
    for (const QString &part : out.split(u',')) {
        const QString p = part.simplified();
        if (!p.isEmpty())
            parts << p;
    }
    return parts.join(QStringLiteral(", "));
}

QJsonObject Director::sanitizePlan(const QJsonObject &plan)
{
    QJsonObject out = plan;
    QJsonArray shots;
    for (const QJsonValue &v : plan.value(QStringLiteral("shots")).toArray()) {
        QJsonObject shot = v.toObject();
        for (const char *key : {"prompt", "negative_prompt", "description"}) {
            const QString k = QLatin1String(key);
            if (shot.contains(k))
                shot.insert(k, cleanPrompt(shot.value(k).toString()));
        }
        shots.append(shot);
    }
    out.insert(QStringLiteral("shots"), shots);
    return out;
}

QString Director::describeCharacter(const QJsonObject &s)
{
    if (s.isEmpty())
        return {};
    QStringList parts;
    const QString desc = s.value(QStringLiteral("description")).toString();
    if (!desc.isEmpty())
        parts << desc;
    QStringList outfit;
    for (const QJsonValue &v : s.value(QStringLiteral("outfit")).toArray()) {
        const QJsonObject o = v.toObject();
        outfit << QStringLiteral("%1 %2 %3").arg(o.value(QStringLiteral("color")).toString(), o.value(QStringLiteral("item")).toString(),
                                                 o.value(QStringLiteral("details")).toString()).simplified();
    }
    if (!outfit.isEmpty())
        parts << QStringLiteral("Outfit: ") + outfit.join(QStringLiteral("; "));
    const QStringList features = json::toStringList(s.value(QStringLiteral("distinctive_features")).toArray());
    if (!features.isEmpty())
        parts << QStringLiteral("Never change: ") + features.join(QStringLiteral(", "));
    return parts.join(u'\n');
}

// ---- LLM calls ---------------------------------------------------------------------------------------

Director::Director(ProviderRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
}

LlmProvider *Director::llm(LlmRole role, QString *model) const
{
    return m_registry->llmFor(role, model);
}

StructuredChatOperation *Director::analyzeCharacter(const QList<QImage> &references, const QString &notes)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::Vision, &model);
    ChatRequest req;
    req.model = model;
    req.temperature = 0.2;
    req.jsonSchema = characterSheetSchema();
    req.schemaName = QStringLiteral("character_sheet");
    req.messages << ChatMessage::system(QStringLiteral(
        "You are a meticulous character designer creating a model sheet for image generation. Study every reference image "
        "carefully and describe the character's appearance precisely: exact hair color and style, eye color, body type, every "
        "clothing item with its colors and details, accessories, color palette and distinctive features. When references "
        "disagree, prefer what appears most often. Describe only what is visible; never invent a backstory."));
    ChatMessage user = ChatMessage::user(
        QStringLiteral("Create the character sheet for the character shown in the %1 reference image(s).%2")
            .arg(references.size())
            .arg(notes.isEmpty() ? QString() : QStringLiteral("\nNotes from the user: ") + notes));
    for (int i = 0; i < references.size(); ++i)
        user.images.append(ChatImage{references[i], QStringLiteral("Reference %1:").arg(i + 1)});
    req.messages << user;
    if (provider && !provider->capabilities().vision) {
        auto *op = new StructuredChatOperation(provider, req);
        failLater(op, tr("The model assigned to 'Vision analysis' (%1) cannot see images. Choose a vision-capable model "
                         "in Settings → Providers → Roles.").arg(provider->displayName()));
        return op;
    }
    return requestJson(provider, req);
}

StructuredChatOperation *Director::planShots(const PlanInput &in)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::Director, &model);
    const PurposeProfile purpose = purposeProfile(in.purposeId);
    const int count = in.count > 0 ? in.count : 0;
    ChatRequest req;
    req.model = model;
    req.temperature = 0.5;
    req.jsonSchema = shotPlanSchema(count > 0 ? count : 12);
    req.schemaName = QStringLiteral("shot_plan");
    QString system = QStringLiteral(
        "You are the art director of an AI image studio. You turn a user's request into a precise shot plan for an image "
        "generation model. Every shot gets a pose expressed with the rig parameters below, an aspect ratio, a background and a "
        "complete prompt.\n\n"
        "Purpose of the images: %1\n%2\n\n"
        "%3\n\n"
        "Prompt writing rules:\n- %4\n- Every prompt must fully restate the character's appearance (hair, eyes, outfit, colors, "
        "distinctive features) because each image is generated independently.\n- Describe the pose, expression, framing and "
        "background in the prompt too, consistent with the pose parameters.\n- Keep the character identical across all shots.\n"
        "- negative_prompt lists what to avoid (may be empty).\n- Never copy rig parameter names or numbers (raise, direction, "
        "bend, twist, yaw, pitch, left_arm...) into prompt, description or negative_prompt: describe the pose in plain words, "
        "e.g. 'arms slightly away from the body, palms open'.")
        .arg(purpose.name, purpose.guidance, pose::rigParamsGuide(), in.style.describeForLlm());
    if (!in.character.isEmpty())
        system += QStringLiteral("\n\nCharacter sheet (authoritative):\n") + pretty(in.character);
    req.messages << ChatMessage::system(system);
    QString user = in.request.trimmed();
    if (user.isEmpty())
        user = QStringLiteral("Create the images for this purpose.");
    user += count > 0 ? QStringLiteral("\n\nPlan exactly %1 shot(s).").arg(count)
                      : QStringLiteral("\n\nPlan the number of shots the request implies (default %1).").arg(purpose.defaultCount);
    if (!purpose.defaultAspect.isEmpty())
        user += QStringLiteral(" Prefer aspect ratio %1 unless a shot needs another.").arg(purpose.defaultAspect);
    ChatMessage msg = ChatMessage::user(user);
    if (in.character.isEmpty()) {
        for (int i = 0; i < std::min<qsizetype>(in.references.size(), 6); ++i)
            msg.images.append(ChatImage{in.references[i], QStringLiteral("Character reference %1:").arg(i + 1)});
    }
    req.messages << msg;
    return requestJson(provider, req);
}

StructuredChatOperation *Director::revisePose(const QJsonObject &shot, const QString &instruction)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::Director, &model);
    ChatRequest req;
    req.model = model;
    req.temperature = 0.3;
    req.jsonSchema = pose::rigParamsSchema();
    req.schemaName = QStringLiteral("pose");
    req.messages << ChatMessage::system(QStringLiteral("You adjust character poses expressed as rig parameters. Change only what the "
                                                       "instruction asks for and keep everything else.\n\n")
                                        + pose::rigParamsGuide());
    req.messages << ChatMessage::user(QStringLiteral("Shot: %1\nCurrent pose:\n%2\n\nInstruction: %3")
                                          .arg(shot.value(QStringLiteral("description")).toString(),
                                               pretty(shot.value(QStringLiteral("pose")).toObject()), instruction));
    return requestJson(provider, req);
}

StructuredChatOperation *Director::rewritePrompt(const QJsonObject &shot, const QJsonObject &character, const QString &instruction,
                                                 const PromptStyle &style)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::PromptWriter, &model);
    ChatRequest req;
    req.model = model;
    req.temperature = 0.5;
    req.jsonSchema = object(QJsonObject{{QStringLiteral("prompt"), str()}, {QStringLiteral("negative_prompt"), str()}});
    req.schemaName = QStringLiteral("prompt");
    QString system = QStringLiteral("You write prompts for image generation models. %1").arg(style.describeForLlm());
    if (!character.isEmpty())
        system += QStringLiteral("\nCharacter:\n") + describeCharacter(character);
    req.messages << ChatMessage::system(system);
    req.messages << ChatMessage::user(QStringLiteral("Current prompt: %1\nCurrent negative prompt: %2\nShot: %3\n\nInstruction: %4")
                                          .arg(shot.value(QStringLiteral("prompt")).toString(),
                                               shot.value(QStringLiteral("negative_prompt")).toString(),
                                               shot.value(QStringLiteral("description")).toString(),
                                               instruction.isEmpty() ? QStringLiteral("Improve the prompt for this model.") : instruction));
    return requestJson(provider, req);
}

// ---- shot → request ---------------------------------------------------------------------------------

QSize Director::sizeForAspect(const QString &aspect, qint64 pixels, int multiple)
{
    const QStringList parts = aspect.split(u':');
    double a = 1.0;
    if (parts.size() == 2 && parts[1].toDouble() > 0)
        a = parts[0].toDouble() / parts[1].toDouble();
    const QSize base(int(std::sqrt(double(pixels) * a)), int(std::sqrt(double(pixels) / a)));
    return img::roundToMultiple(base, std::max(8, multiple));
}

pose::Pose Director::poseForShot(const QJsonObject &shot, const QSize &canvas)
{
    const pose::RigParams rig = pose::RigParams::fromJson(shot.value(QStringLiteral("pose")).toObject());
    return pose::solvePose({rig}, canvas);
}

namespace {

struct DatasetShot
{
    const char *title;
    double yaw;
    const char *framing;
    const char *expression;
    const char *pose;
    const char *background;
};

// Ordered so that any prefix covers the views and framings evenly.
const DatasetShot kDatasetShots[] = {
    {"Front, full body", 0, "full_body", "neutral", "stand", "white"},
    {"Three-quarter right, full body", 45, "full_body", "smile", "stand", "grey"},
    {"Three-quarter left, cowboy shot", -45, "cowboy_shot", "smile", "hip", "blue"},
    {"Side, full body", 90, "full_body", "neutral", "stand", "white"},
    {"Back, full body", 180, "full_body", "neutral", "stand", "grey"},
    {"Front, upper body, waving", 0, "upper_body", "smile", "wave", "gradient"},
    {"Front, portrait", 0, "headshot", "neutral", "stand", "white"},
    {"Three-quarter right, portrait", 45, "headshot", "smile", "stand", "beige"},
    {"Three-quarter left, upper body, peace sign", -45, "upper_body", "smile", "peace", "white"},
    {"Front, cowboy shot, hands on hips", 0, "cowboy_shot", "open_mouth", "hips", "grey"},
    {"Front, upper body, surprised", 0, "upper_body", "surprised", "stand", "blue"},
    {"Three-quarter right, upper body, crossed arms", 45, "upper_body", "angry", "cross", "white"},
    {"Front, portrait, eyes closed", 0, "headshot", "closed_eyes", "stand", "gradient"},
    {"Side left, upper body", -90, "upper_body", "neutral", "stand", "grey"},
    {"Front, full body, walking", 0, "full_body", "smile", "walk", "beige"},
    {"Three-quarter left, full body, arms behind back", -45, "full_body", "smile", "behind", "white"},
    {"Three-quarter right, cowboy shot, reaching", 45, "cowboy_shot", "open_mouth", "reach", "blue"},
    {"Front, upper body, sad", 0, "upper_body", "sad", "stand", "grey"},
    {"Three-quarter left, portrait", -45, "headshot", "neutral", "stand", "white"},
    {"Front, full body, waving", 0, "full_body", "open_mouth", "wave", "gradient"},
    {"Looking back, cowboy shot", 150, "cowboy_shot", "neutral", "stand", "white"},
    {"Front, cowboy shot", 0, "cowboy_shot", "neutral", "stand", "beige"},
    {"Three-quarter right, full body, hands on hips", 45, "full_body", "smile", "hips", "grey"},
    {"Front, upper body, reaching", 0, "upper_body", "smile", "reach", "white"},
    {"Three-quarter left, cowboy shot, waving", -45, "cowboy_shot", "open_mouth", "wave", "grey"},
    {"Front, portrait, angry", 0, "headshot", "angry", "stand", "blue"},
    {"Three-quarter right, portrait, surprised", 45, "headshot", "surprised", "stand", "white"},
    {"Side, cowboy shot, walking", 90, "cowboy_shot", "neutral", "walk", "gradient"},
    {"Front, full body, crossed arms", 0, "full_body", "neutral", "cross", "white"},
    {"Three-quarter left, upper body, hand on hip", -45, "upper_body", "smile", "hip", "beige"},
    {"Front, upper body, peace sign", 0, "upper_body", "smile", "peace", "grey"},
    {"Three-quarter right, full body, eyes closed", 45, "full_body", "closed_eyes", "stand", "blue"},
};
static_assert(std::size(kDatasetShots) == Director::kMaxDatasetShots);

QString viewTags(double yaw)
{
    if (yaw >= 170)
        return QStringLiteral("from behind");
    if (yaw >= 120)
        return QStringLiteral("from behind, looking back, looking at viewer");
    if (std::abs(yaw) >= 80)
        return QStringLiteral("from side, profile");
    return QStringLiteral("looking at viewer");
}

QString framingTags(const QString &framing)
{
    if (framing == QLatin1String("full_body"))
        return QStringLiteral("full body, standing");
    if (framing == QLatin1String("cowboy_shot"))
        return QStringLiteral("cowboy shot");
    if (framing == QLatin1String("upper_body"))
        return QStringLiteral("upper body");
    return QStringLiteral("portrait, close-up");
}

QString expressionTags(const QString &e)
{
    if (e == QLatin1String("smile"))
        return QStringLiteral("smile");
    if (e == QLatin1String("open_mouth"))
        return QStringLiteral("open mouth, happy");
    if (e == QLatin1String("surprised"))
        return QStringLiteral("surprised, open mouth");
    if (e == QLatin1String("closed_eyes"))
        return QStringLiteral("closed eyes, smile");
    if (e == QLatin1String("angry"))
        return QStringLiteral("angry, frown");
    if (e == QLatin1String("sad"))
        return QStringLiteral("sad");
    return QStringLiteral("closed mouth");
}

QString backgroundTags(const QString &b)
{
    if (b == QLatin1String("grey"))
        return QStringLiteral("simple background, grey background");
    if (b == QLatin1String("blue"))
        return QStringLiteral("simple background, light blue background");
    if (b == QLatin1String("gradient"))
        return QStringLiteral("gradient background");
    if (b == QLatin1String("beige"))
        return QStringLiteral("simple background, beige background");
    return QStringLiteral("simple background, white background");
}

// Arm (and leg) pose: rig values and the matching tags.
QString applyArmPose(pose::RigParams &rig, const QString &pose)
{
    auto set = [](pose::LimbParams &l, double raise, double direction, double bend, double twist) { l = {raise, direction, bend, twist}; };
    if (pose == QLatin1String("hip")) {
        set(rig.rightArm, 40, 90, 110, 90);
        return QStringLiteral("hand on hip");
    }
    if (pose == QLatin1String("hips")) {
        set(rig.leftArm, 40, 90, 110, 90);
        set(rig.rightArm, 40, 90, 110, 90);
        return QStringLiteral("hands on hips");
    }
    if (pose == QLatin1String("wave")) {
        set(rig.rightArm, 100, 90, 70, -90);
        rig.rightHand = QStringLiteral("open");
        return QStringLiteral("waving, raised hand");
    }
    if (pose == QLatin1String("peace")) {
        set(rig.rightArm, 60, 30, 130, -90);
        rig.rightHand = QStringLiteral("peace");
        return QStringLiteral("peace sign");
    }
    if (pose == QLatin1String("reach")) {
        set(rig.rightArm, 85, 10, 10, 0);
        rig.rightHand = QStringLiteral("open");
        return QStringLiteral("reaching towards viewer, outstretched arm");
    }
    if (pose == QLatin1String("behind")) {
        set(rig.leftArm, 20, 180, 60, 90);
        set(rig.rightArm, 20, 180, 60, 90);
        rig.leftHand = rig.rightHand = QStringLiteral("none");
        return QStringLiteral("arms behind back");
    }
    if (pose == QLatin1String("cross")) {
        set(rig.leftArm, 40, -60, 120, 90);
        set(rig.rightArm, 40, -60, 120, 90);
        return QStringLiteral("crossed arms");
    }
    if (pose == QLatin1String("walk")) {
        set(rig.leftLeg, 20, 0, 0, 0);
        set(rig.rightLeg, 15, 180, 20, 0);
        set(rig.leftArm, 15, 180, 10, 0);
        set(rig.rightArm, 15, 0, 15, 0);
        return QStringLiteral("walking");
    }
    return QStringLiteral("arms at sides");
}

} // namespace

QList<QJsonObject> Director::loraTestShots(const QJsonObject &sheet, const QString &trigger)
{
    struct Test
    {
        const char *id;
        const char *title;
        double yaw;
        const char *framing;
        const char *expression;
        const char *tags;
        const char *background;
    };
    // Different from the training recipe: dynamic poses and real backgrounds.
    static const Test tests[] = {
        {"t1", "Cheering, outdoors", 0, "full_body", "open_mouth", "arms up, cheering, standing", "outdoors, park, trees, blue sky"},
        {"t2", "Three-quarter, city street", 40, "cowboy_shot", "smile", "hand on own chest, looking at viewer", "city street, buildings, daytime"},
        {"t3", "Side view, running", 90, "full_body", "neutral", "running, motion lines", "beach, ocean, sand"},
        {"t4", "Portrait, indoors", -30, "headshot", "smile", "looking at viewer, head tilt", "indoors, cafe, warm lighting"},
    };
    const QString gender = sheet.value(QStringLiteral("gender_presentation")).toString().toLower();
    QString subject = QStringLiteral("solo");
    if (gender.contains(QLatin1String("female")) || gender.contains(QLatin1String("girl")) || gender.contains(QLatin1String("woman")))
        subject = QStringLiteral("1girl, solo");
    else if (gender.contains(QLatin1String("male")) || gender.contains(QLatin1String("boy")) || gender.contains(QLatin1String("man")))
        subject = QStringLiteral("1boy, solo");
    QList<QJsonObject> out;
    for (const Test &t : tests) {
        pose::RigParams rig;
        rig.yaw = t.yaw;
        rig.framing = QLatin1String(t.framing);
        rig.expression = QLatin1String(t.expression);
        if (QLatin1String(t.id) == QLatin1String("t1")) {
            rig.leftArm = {160, 80, 20, 0};
            rig.rightArm = {160, 80, 20, 0};
            rig.leftHand = rig.rightHand = QStringLiteral("open");
        } else if (QLatin1String(t.id) == QLatin1String("t2")) {
            rig.rightArm = {35, -40, 120, 90};
        } else if (QLatin1String(t.id) == QLatin1String("t3")) {
            rig.leftLeg = {45, 0, 70, 0};
            rig.rightLeg = {30, 180, 60, 0};
            rig.leftArm = {40, 180, 80, 0};
            rig.rightArm = {45, 0, 90, 0};
            rig.torsoLean = 15;
        } else {
            rig.headRoll = 12;
        }
        QStringList prompt;
        if (!trigger.isEmpty())
            prompt << trigger;
        prompt << subject << appearanceTags(sheet);
        prompt << QLatin1String(t.tags) << QLatin1String(t.background);
        out << QJsonObject{{QStringLiteral("id"), QLatin1String(t.id)},
                           {QStringLiteral("title"), QLatin1String(t.title)},
                           {QStringLiteral("description"), QStringLiteral("LoRA test shot")},
                           {QStringLiteral("aspect_ratio"), QLatin1String(t.framing) == QLatin1String("headshot") ? QStringLiteral("1:1") : QStringLiteral("2:3")},
                           {QStringLiteral("background"), QLatin1String(t.background)},
                           {QStringLiteral("pose"), rig.toJson()},
                           {QStringLiteral("prompt"), prompt.join(QStringLiteral(", "))},
                           {QStringLiteral("negative_prompt"), QStringLiteral("multiple views, 2girls, 2boys, text, watermark")}};
    }
    return out;
}

QStringList Director::identityChecklist(const QJsonObject &sheet)
{
    // Subject and count tags say nothing about who the character is.
    static const QRegularExpression subject(QStringLiteral("^(\\d\\+?(girl|boy|other)s?|solo|solo focus|multiple \\w+)$"));
    QStringList out{QStringLiteral("the same face as in the references (face shape, eye shape, proportions)")};
    for (const QString &t : splitTags(appearanceTags(sheet))) {
        const QString tag = modelTag(t);
        if (out.size() >= 12)
            break;
        if (!tag.isEmpty() && !subject.match(tag).hasMatch() && !out.contains(tag))
            out << tag;
    }
    return out;
}

QJsonObject Director::identityChecklistSchema(int features)
{
    QJsonObject props;
    QJsonArray required;
    for (int i = 1; i <= features; ++i) {
        const QString key = QStringLiteral("f%1").arg(i);
        props.insert(key, QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                      {QStringLiteral("enum"), QJsonArray{QStringLiteral("match"), QStringLiteral("close"),
                                                                         QStringLiteral("different"), QStringLiteral("not_visible")}}});
        required.append(key);
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("features"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                  {QStringLiteral("properties"), props},
                                  {QStringLiteral("required"), required},
                                  {QStringLiteral("additionalProperties"), false}}},
                     {QStringLiteral("pose"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                                          {QStringLiteral("minimum"), 0},
                                                          {QStringLiteral("maximum"), 10}}},
                     {QStringLiteral("issues"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                                            {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                                                            {QStringLiteral("maxItems"), 5}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("features"), QStringLiteral("pose"), QStringLiteral("issues")}},
        {QStringLiteral("additionalProperties"), false}};
}

double Director::checklistIdentity(const QJsonObject &answer)
{
    const QJsonObject features = answer.value(QStringLiteral("features")).toObject();
    double points = 0;
    int visible = 0;
    for (auto it = features.begin(); it != features.end(); ++it) {
        const QString v = it.value().toString();
        if (v == QLatin1String("match"))
            points += 1;
        else if (v == QLatin1String("close"))
            points += 0.5;
        else if (v != QLatin1String("different"))
            continue; // not visible
        ++visible;
    }
    return visible > 0 ? 10.0 * points / visible : -1;
}

StructuredChatOperation *Director::checkIdentity(const QImage &result, const QJsonObject &shot, const QJsonObject &character,
                                                 const QList<QImage> &references)
{
    QString model;
    LlmProvider *provider = llm(LlmRole::Vision, &model);
    const QStringList checklist = identityChecklist(character);
    ChatRequest req;
    req.model = model;
    req.temperature = 0.1;
    req.jsonSchema = identityChecklistSchema(int(checklist.size()));
    req.schemaName = QStringLiteral("identity_checklist");
    req.maxTokens = 800;
    req.messages << ChatMessage::system(QStringLiteral(
        "You check whether an AI-generated image shows the same character as the reference images, feature by feature.\n"
        "For each numbered feature answer:\n"
        "- match: visible and the same as in the references and the description;\n"
        "- close: visible but somewhat off (shade, length, shape, a missing detail);\n"
        "- different: visible and wrong, or absent although this view would show it (no choker on a visible neck, "
        "another garment instead of the described one);\n"
        "- not_visible: hidden by the view, framing or pose (eyes in a back view, shoes in a portrait).\n"
        "Judge only the character. The references only show who the character is: ignore their pose, framing and background.\n"
        "pose: 0-10, how well pose, framing, camera angle and background follow the PLANNED SHOT.\n"
        "issues: at most 5 short notes on what differs."));
    QStringList lines;
    for (int i = 0; i < checklist.size(); ++i)
        lines << QStringLiteral("f%1: %2").arg(i + 1).arg(checklist[i]);
    ChatMessage user = ChatMessage::user(QStringLiteral("Planned shot:\n%1\n\nFeatures of the character:\n%2\n\nThe first image is the "
                                                        "generated result; the others are references of the character.")
                                             .arg(QString::fromUtf8(QJsonDocument(shot).toJson(QJsonDocument::Compact)), lines.join(QLatin1Char('\n'))));
    user.images.append(ChatImage{img::thumbnail(result, 1024), QStringLiteral("Generated result:")});
    for (int i = 0; i < std::min<int>(2, int(references.size())); ++i)
        user.images.append(ChatImage{img::thumbnail(references[i], 768), QStringLiteral("Reference %1:").arg(i + 1)});
    req.messages << user;
    if (!provider || !provider->capabilities().vision) {
        auto *op = new StructuredChatOperation(provider, req);
        failLater(op, provider ? tr("The model assigned to 'Vision analysis' (%1) cannot see images, so it cannot review results.")
                                     .arg(provider->displayName())
                               : tr("No language model is configured for 'Vision analysis'."));
        return op;
    }
    return requestJson(provider, req);
}

double Director::loraTestScore(const QJsonObject &critique)
{
    if (critique.contains(QStringLiteral("features"))) {
        const double pose = critique.value(QStringLiteral("pose")).toDouble(0);
        const double identity = checklistIdentity(critique);
        // Nothing of the character visible: the pose decides alone (rare; the test shots show the face).
        return identity < 0 ? pose : (2.0 * identity + pose) / 3.0;
    }
    const double overall = critique.value(QStringLiteral("overall")).toDouble(0);
    const double identity = critique.value(QStringLiteral("identity")).toDouble(overall);
    const double pose = critique.value(QStringLiteral("pose")).toDouble(overall);
    return (2.0 * identity + pose) / 3.0;
}

QString Director::appearanceTags(const QJsonObject &sheet)
{
    const QString tags = sheet.value(QStringLiteral("tags")).toString().trimmed();
    if (!tags.isEmpty())
        return tags;
    QStringList out;
    const QJsonObject hair = sheet.value(QStringLiteral("hair")).toObject();
    for (const char *key : {"color", "length"}) {
        const QString v = hair.value(QLatin1String(key)).toString().trimmed().toLower();
        if (!v.isEmpty())
            out << (v.endsWith(QLatin1String("hair")) ? v : v + QStringLiteral(" hair"));
    }
    if (!hair.value(QStringLiteral("style")).toString().trimmed().isEmpty())
        out << hair.value(QStringLiteral("style")).toString().trimmed().toLower();
    const QString eyes = sheet.value(QStringLiteral("eyes")).toObject().value(QStringLiteral("color")).toString().trimmed().toLower();
    if (!eyes.isEmpty())
        out << (eyes.endsWith(QLatin1String("eyes")) ? eyes : eyes + QStringLiteral(" eyes"));
    for (const QJsonValue &v : sheet.value(QStringLiteral("outfit")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString item = (o.value(QStringLiteral("color")).toString() + QLatin1Char(' ') + o.value(QStringLiteral("item")).toString()).trimmed().toLower();
        if (!item.isEmpty())
            out << item;
    }
    for (const QJsonValue &v : sheet.value(QStringLiteral("accessories")).toArray())
        out << v.toString().trimmed().toLower();
    for (const QJsonValue &v : sheet.value(QStringLiteral("distinctive_features")).toArray())
        out << v.toString().trimmed().toLower();
    out.removeAll(QString());
    return out.join(QStringLiteral(", "));
}

QJsonObject Director::datasetPlan(const QJsonObject &sheet, int count)
{
    const int n = std::clamp(count > 0 ? count : 24, 1, kMaxDatasetShots);
    const QString gender = sheet.value(QStringLiteral("gender_presentation")).toString().toLower();
    QString subject = QStringLiteral("solo");
    if (gender.contains(QLatin1String("female")) || gender.contains(QLatin1String("girl")) || gender.contains(QLatin1String("woman"))
        || gender.contains(QLatin1String("feminine")))
        subject = QStringLiteral("1girl, solo");
    else if (gender.contains(QLatin1String("male")) || gender.contains(QLatin1String("boy")) || gender.contains(QLatin1String("man"))
             || gender.contains(QLatin1String("masculine")))
        subject = QStringLiteral("1boy, solo");
    const QString appearance = appearanceTags(sheet);
    const QString name = sheet.value(QStringLiteral("name")).toString();
    QJsonArray shots;
    for (int i = 0; i < n; ++i) {
        const DatasetShot &d = kDatasetShots[i];
        pose::RigParams rig;
        rig.yaw = d.yaw;
        rig.framing = QLatin1String(d.framing);
        rig.expression = QLatin1String(d.expression);
        rig.perspective = 0.1;
        const QString poseTags = applyArmPose(rig, QLatin1String(d.pose));
        const QString framing = QLatin1String(d.framing);
        QStringList prompt{subject};
        if (!appearance.isEmpty())
            prompt << appearance;
        prompt << viewTags(d.yaw) << framingTags(framing) << poseTags;
        if (d.yaw < 120)
            prompt << expressionTags(QLatin1String(d.expression));
        prompt << backgroundTags(QLatin1String(d.background));
        const QString aspect = framing == QLatin1String("headshot") ? QStringLiteral("1:1")
                               : framing == QLatin1String("upper_body") ? QStringLiteral("3:4")
                                                                        : QStringLiteral("2:3");
        shots.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("ds%1").arg(i + 1, 2, 10, QLatin1Char('0'))},
                                 {QStringLiteral("title"), QLatin1String(d.title)},
                                 {QStringLiteral("description"), QStringLiteral("Training image %1 of %2").arg(i + 1).arg(n)},
                                 {QStringLiteral("aspect_ratio"), aspect},
                                 {QStringLiteral("background"), backgroundTags(QLatin1String(d.background))},
                                 {QStringLiteral("pose"), rig.toJson()},
                                 {QStringLiteral("prompt"), prompt.join(QStringLiteral(", "))},
                                 {QStringLiteral("negative_prompt"),
                                  QStringLiteral("multiple views, 2girls, 2boys, text, watermark, signature, detailed background, scenery, props")}});
    }
    return QJsonObject{{QStringLiteral("title"), name.isEmpty() ? QStringLiteral("LoRA training set") : QStringLiteral("LoRA training set: %1").arg(name)},
                       {QStringLiteral("summary"), QStringLiteral("%1 shots: front, three-quarter, side and back views; full body to portrait; "
                                                                  "several expressions and arm poses on plain backgrounds.").arg(n)},
                       {QStringLiteral("shots"), shots}};
}

bool Director::applyCharacterLora(ImageRequest &r, const CharacterLora &lora, const ImageCapabilities &caps)
{
    if (!lora.isValid() || !caps.loras)
        return false;
    const LoraRef ref = lora.ref();
    for (const LoraRef &existing : std::as_const(r.loras)) {
        if (existing.name.compare(ref.name, Qt::CaseInsensitive) == 0)
            return true;
    }
    r.loras.append(ref);
    const QString trigger = lora.trigger.trimmed();
    if (!trigger.isEmpty() && !r.prompt.contains(trigger, Qt::CaseInsensitive))
        r.prompt = r.prompt.trimmed().isEmpty() ? trigger : trigger + QStringLiteral(", ") + r.prompt.trimmed();
    return true;
}

ImageRequest Director::buildShotRequest(const QJsonObject &shot, const QList<ReferenceImage> &references, const pose::Pose &pose,
                                        const PromptStyle &style, const ImageCapabilities &caps)
{
    ImageRequest r;
    r.mode = GenMode::TextToImage;
    r.size = sizeForAspect(shot.value(QStringLiteral("aspect_ratio")).toString(QStringLiteral("2:3")), caps.nativePixels, caps.sizeMultiple);
    r.prompt = style.apply(cleanPrompt(shot.value(QStringLiteral("prompt")).toString()));
    QString negative = cleanPrompt(shot.value(QStringLiteral("negative_prompt")).toString());
    if (style.supportsNegative && !style.defaultNegative.isEmpty())
        negative = negative.isEmpty() ? style.defaultNegative : negative + QStringLiteral(", ") + style.defaultNegative;
    r.negativePrompt = style.supportsNegative ? negative : QString();
    if (caps.maxReferences > 0)
        r.references = references.mid(0, caps.maxReferences);
    if (caps.poseControl && !pose.isEmpty()) {
        ControlInput c;
        c.type = ControlType::OpenPose;
        c.image = pose::renderOpenPose(pose, r.size);
        c.weight = 1.0;
        r.controls << c;
    }
    return r;
}

} // namespace lumen::ai
