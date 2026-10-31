#include "ai/RunSettings.h"

#include "ai/ModelDefaults.h"
#include "ai/PromptStyle.h"
#include "ai/Provider.h"

namespace lumen::ai {

RunSettingsSpec runSettingsFor(const ImageProvider *provider)
{
    RunSettingsSpec s;
    if (!provider)
        return s;
    const ImageCapabilities caps = provider->capabilities();
    const ModelDefaults d = modelDefaultsOf(provider);
    const PromptStyle style = promptStyleFor(provider);
    s.key = d.isValid() ? QStringLiteral("preset:") + d.id : QStringLiteral("engine:") + provider->instanceId();
    s.modelName = d.name;
    s.steps = caps.steps;
    s.modelSteps = style.steps;
    // A model distilled to run at CFG 1 (its preset says 1) starts there, but the value can be raised
    // (the klein base models need it, and people tune it); the editor says what it costs.
    s.distilled = caps.cfg && d.isValid() && d.cfg > 0 && d.cfg <= 1.0;
    s.cfg = caps.cfg;
    s.modelCfg = style.cfg;
    s.guidance = d.guidance > 0;
    s.modelGuidance = d.guidance;
    s.imageCfg = d.edit && d.imageCfg > 0;
    s.modelImageCfg = d.imageCfg;
    s.flowShift = d.flowShift > 0;
    s.modelFlowShift = d.flowShift;
    s.sampler = caps.samplers;
    s.modelSampler = style.sampler;
    s.modelScheduler = style.scheduler;
    // CLIP skip is for the CLIP text encoders of SD 1.5 and SDXL; a preset knows the model, else the
    // prompt style's guess from its name decides.
    static const QStringList clipModels{QStringLiteral("sd15"), QStringLiteral("sdxl"), QStringLiteral("pony"), QStringLiteral("illustrious")};
    s.clipSkip = d.isValid() ? clipModels.contains(d.id) : clipModels.contains(style.family);
    return s;
}

QJsonObject RunSettings::toJson() const
{
    QJsonObject o;
    auto put = [&o](const char *key, double v) {
        if (v > 0)
            o.insert(QLatin1String(key), v);
    };
    put("steps", steps);
    put("cfg", cfg);
    put("guidance", guidance);
    put("image_cfg", imageCfg);
    put("flow_shift", flowShift);
    put("clip_skip", clipSkip);
    if (!sampler.isEmpty())
        o.insert(QStringLiteral("sampler"), sampler);
    if (!scheduler.isEmpty())
        o.insert(QStringLiteral("scheduler"), scheduler);
    return o;
}

RunSettings RunSettings::fromJson(const QJsonObject &o)
{
    RunSettings s;
    s.steps = o.value(QStringLiteral("steps")).toInt();
    s.cfg = o.value(QStringLiteral("cfg")).toDouble();
    s.guidance = o.value(QStringLiteral("guidance")).toDouble();
    s.imageCfg = o.value(QStringLiteral("image_cfg")).toDouble();
    s.flowShift = o.value(QStringLiteral("flow_shift")).toDouble();
    s.clipSkip = o.value(QStringLiteral("clip_skip")).toInt();
    s.sampler = o.value(QStringLiteral("sampler")).toString();
    s.scheduler = o.value(QStringLiteral("scheduler")).toString();
    return s;
}

void applyRunSettings(ImageRequest &r, const RunSettings &s, const RunSettingsSpec &spec)
{
    if (spec.steps && s.steps > 0) {
        r.steps = s.steps;
        r.stepsChosen = true;
    }
    if (spec.cfg && s.cfg > 0) {
        r.cfg = s.cfg;
        r.cfgChosen = true;
    }
    if (spec.guidance && s.guidance > 0)
        r.guidance = s.guidance;
    if (spec.imageCfg && s.imageCfg > 0)
        r.imageCfg = s.imageCfg;
    if (spec.flowShift && s.flowShift > 0)
        r.flowShift = s.flowShift;
    if (spec.sampler) {
        if (!s.sampler.isEmpty())
            r.sampler = s.sampler;
        if (!s.scheduler.isEmpty())
            r.scheduler = s.scheduler;
    }
    if (spec.clipSkip && s.clipSkip > 0)
        r.clipSkip = s.clipSkip;
}

} // namespace lumen::ai
