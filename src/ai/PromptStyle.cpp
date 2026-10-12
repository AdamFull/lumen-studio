#include "ai/PromptStyle.h"

#include "ai/Provider.h"

#include <QFileInfo>

namespace lumen::ai {

namespace {
const QString kAnimeNegative = QStringLiteral(
    "lowres, bad anatomy, bad hands, missing fingers, extra digits, fewer digits, extra limbs, deformed, text, signature, "
    "watermark, username, error, cropped, worst quality, low quality, jpeg artifacts, blurry");
const QString kPhotoNegative = QStringLiteral(
    "lowres, deformed, distorted, bad anatomy, extra limbs, extra fingers, mutated hands, text, watermark, signature, blurry, "
    "jpeg artifacts, worst quality, low quality");
} // namespace

QString PromptStyle::describeForLlm() const
{
    if (family == QLatin1String("anima")) {
        return QStringLiteral("The image model is Anima, an anime model trained on Danbooru tags and natural language. Write prompts as "
                              "comma-separated tags in lowercase with spaces instead of underscores, in this order: subject count "
                              "(e.g. 1girl, solo), character name and series if any, character appearance, outfit, pose/action, "
                              "expression, framing (full body, upper body...), background, lighting. Then add one or two plain "
                              "sentences describing the scene. Do not include quality or rating tags; they are added automatically.");
    }
    if (dialect == Dialect::Tags) {
        return QStringLiteral("The image model is an anime-style Stable Diffusion checkpoint (%1). Write prompts as comma-separated "
                              "Danbooru-style tags in order of importance: subject count (e.g. 1girl, solo), character appearance tags, "
                              "outfit, pose/action, expression, framing (full body, upper body...), background, lighting, style. "
                              "No sentences. Do not include quality tags; they are added automatically.").arg(family);
    }
    return QStringLiteral("The image model understands natural language. Write each prompt as one vivid, concrete paragraph describing "
                          "the subject, appearance, outfit, pose, expression, framing, background, lighting and art style.");
}

QString PromptStyle::apply(const QString &prompt) const
{
    if (qualityPrefix.isEmpty() || prompt.startsWith(qualityPrefix.left(8)))
        return prompt;
    return qualityPrefix + prompt;
}

QString PromptStyle::withSafeTag(const QString &prompt) const
{
    if (safeTag.isEmpty())
        return prompt;
    for (const QString &tag : prompt.split(QLatin1Char(','))) {
        const QString t = tag.trimmed().toLower();
        if (t == QLatin1String("safe") || t == QLatin1String("sensitive") || t == QLatin1String("nsfw") || t == QLatin1String("explicit"))
            return prompt; // a rating is there already
    }
    if (!qualityPrefix.isEmpty() && prompt.startsWith(qualityPrefix))
        return qualityPrefix + safeTag + QStringLiteral(", ") + prompt.mid(qualityPrefix.size());
    return safeTag + QStringLiteral(", ") + prompt;
}

PromptStyle promptStyleFor(const ImageProvider *provider, const QString &model)
{
    PromptStyle s;
    if (!provider)
        return s;
    const QString type = provider->type().id;
    const ImageCapabilities caps = provider->capabilities();
    s.supportsNegative = caps.negativePrompt;
    // Local Stable Diffusion back-ends: the checkpoint's name tells the prompt dialect.
    if (type != QLatin1String("sdwebui") && type != QLatin1String("comfyui") && type != QLatin1String("sdcpp")) {
        s.family = QStringLiteral("cloud");
        s.dialect = PromptStyle::Dialect::Natural;
        return s;
    }
    const QString m = (model.isEmpty() ? provider->text(QStringLiteral("model")) : model).toLower();
    // The family read from the server's model file applies when the model asked about is that one.
    const QString serverModel = QFileInfo(provider->text(QStringLiteral("model"))).completeBaseName();
    const bool sameModel = model.isEmpty() || QFileInfo(model).completeBaseName().compare(serverModel, Qt::CaseInsensitive) == 0;
    const QString fileFamily = sameModel ? provider->text(QStringLiteral("model_family")) : QString();
    if (fileFamily == QLatin1String("Anima") || (m.contains(QLatin1String("anima")) && !m.contains(QLatin1String("animagine")))) {
        // circlestone-labs/Anima's model card: quality tags, then a rating, then the rest.
        s.family = QStringLiteral("anima");
        s.dialect = PromptStyle::Dialect::Tags;
        s.qualityPrefix = QStringLiteral("masterpiece, best quality, score_7, ");
        s.defaultNegative = QStringLiteral("worst quality, low quality, score_1, score_2, score_3, artist name, blurry, jpeg artifacts, "
                                           "chromatic aberration");
        s.safeTag = QStringLiteral("safe");
    } else if (m.contains(QLatin1String("pony"))) {
        s.family = QStringLiteral("pony");
        s.dialect = PromptStyle::Dialect::Tags;
        s.qualityPrefix = QStringLiteral("score_9, score_8_up, score_7_up, ");
        s.defaultNegative = QStringLiteral("score_4, score_5, score_6, ") + kAnimeNegative;
    } else if (m.contains(QLatin1String("illustrious")) || m.contains(QLatin1String("noob")) || m.contains(QLatin1String("animagine"))
               || m.contains(QLatin1String("anime")) || m.contains(QLatin1String("waifu"))) {
        s.family = QStringLiteral("illustrious");
        s.dialect = PromptStyle::Dialect::Tags;
        s.qualityPrefix = QStringLiteral("masterpiece, best quality, amazing quality, very aesthetic, absurdres, ");
        s.defaultNegative = kAnimeNegative;
    } else if (m.contains(QLatin1String("flux")) || m.contains(QLatin1String("qwen")) || m.contains(QLatin1String("sd3"))) {
        s.family = QStringLiteral("flux");
        s.dialect = PromptStyle::Dialect::Natural;
        s.supportsNegative = false;
    } else {
        s.family = caps.nativePixels <= 512 * 512 ? QStringLiteral("sd15") : QStringLiteral("sdxl");
        s.dialect = PromptStyle::Dialect::Natural;
        s.qualityPrefix = QStringLiteral("high quality, detailed, ");
        s.defaultNegative = kPhotoNegative;
    }
    if (provider->type().id == QLatin1String("comfyui") && provider->text(QStringLiteral("workflow")) != QLatin1String("builtin")
        && !provider->text(QStringLiteral("workflow")).isEmpty()) {
        // Custom workflows (FLUX Kontext, Qwen edit...) are typically instruction/natural-language models.
        s.dialect = PromptStyle::Dialect::Natural;
        s.qualityPrefix.clear();
    }
    return s;
}

QString buildInfoText(const QString &prompt, const QString &negative, int steps, const QString &sampler, const QString &scheduler,
                      double cfg, qint64 seed, int width, int height, const QString &model, double denoise)
{
    QString s = prompt;
    if (!negative.isEmpty())
        s += QStringLiteral("\nNegative prompt: ") + negative;
    QStringList kv;
    if (steps > 0)
        kv << QStringLiteral("Steps: %1").arg(steps);
    if (!sampler.isEmpty())
        kv << QStringLiteral("Sampler: %1").arg(sampler);
    if (!scheduler.isEmpty())
        kv << QStringLiteral("Schedule type: %1").arg(scheduler);
    if (cfg > 0)
        kv << QStringLiteral("CFG scale: %1").arg(cfg);
    if (seed >= 0)
        kv << QStringLiteral("Seed: %1").arg(seed);
    kv << QStringLiteral("Size: %1x%2").arg(width).arg(height);
    if (!model.isEmpty())
        kv << QStringLiteral("Model: %1").arg(model);
    if (denoise >= 0)
        kv << QStringLiteral("Denoising strength: %1").arg(denoise);
    kv << QStringLiteral("Version: Lumen Studio");
    return s + u'\n' + kv.join(QStringLiteral(", "));
}

} // namespace lumen::ai
