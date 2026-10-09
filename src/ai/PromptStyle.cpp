#include "ai/PromptStyle.h"

#include "ai/Provider.h"

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

PromptStyle promptStyleFor(const ImageProvider *provider, const QString &model)
{
    PromptStyle s;
    if (!provider)
        return s;
    const QString type = provider->type().id;
    const ImageCapabilities caps = provider->capabilities();
    s.supportsNegative = caps.negativePrompt;
    if (type != QLatin1String("sdwebui") && type != QLatin1String("comfyui")) {
        s.family = QStringLiteral("cloud");
        s.dialect = PromptStyle::Dialect::Natural;
        return s;
    }
    const QString m = (model.isEmpty() ? provider->text(QStringLiteral("model")) : model).toLower();
    if (m.contains(QLatin1String("pony"))) {
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
