#include "ai/ModelDefaults.h"

#include "ai/Provider.h"

namespace lumen::ai {

QVariantMap ModelDefaults::toVariant() const
{
    if (!isValid())
        return {};
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("cfg"), cfg},
            {QStringLiteral("guidance"), guidance},
            {QStringLiteral("image_cfg"), imageCfg},
            {QStringLiteral("flow_shift"), flowShift},
            {QStringLiteral("steps"), steps},
            {QStringLiteral("sampler"), sampler},
            {QStringLiteral("scheduler"), scheduler},
            {QStringLiteral("dialect"), dialect},
            {QStringLiteral("native_resolution"), nativeResolution},
            {QStringLiteral("generate"), generate},
            {QStringLiteral("edit"), edit},
            {QStringLiteral("references"), references},
            {QStringLiteral("negative_prompt"), negativePrompt}};
}

ModelDefaults ModelDefaults::fromVariant(const QVariantMap &map)
{
    ModelDefaults d;
    d.id = map.value(QStringLiteral("id")).toString();
    if (d.id.isEmpty())
        return d;
    d.name = map.value(QStringLiteral("name")).toString();
    d.cfg = map.value(QStringLiteral("cfg")).toDouble();
    d.guidance = map.value(QStringLiteral("guidance")).toDouble();
    d.imageCfg = map.value(QStringLiteral("image_cfg")).toDouble();
    d.flowShift = map.value(QStringLiteral("flow_shift")).toDouble();
    d.steps = map.value(QStringLiteral("steps")).toInt();
    d.sampler = map.value(QStringLiteral("sampler")).toString();
    d.scheduler = map.value(QStringLiteral("scheduler")).toString();
    d.dialect = map.value(QStringLiteral("dialect")).toString();
    d.nativeResolution = map.value(QStringLiteral("native_resolution")).toInt();
    d.generate = map.value(QStringLiteral("generate"), true).toBool();
    d.edit = map.value(QStringLiteral("edit")).toBool();
    d.references = map.value(QStringLiteral("references")).toInt();
    d.negativePrompt = map.value(QStringLiteral("negative_prompt"), true).toBool();
    return d;
}

ModelDefaults modelDefaultsOf(const Provider *provider)
{
    return provider ? ModelDefaults::fromVariant(provider->setting(QStringLiteral("model_preset")).toMap()) : ModelDefaults();
}

} // namespace lumen::ai
