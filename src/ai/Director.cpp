#include "ai/Director.h"

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
