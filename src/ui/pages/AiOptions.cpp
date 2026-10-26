#include "ui/pages/AiOptions.h"

#include "ai/ProviderRegistry.h"
#include "ai/Types.h"
#include "core/Settings.h"
#include "ui/widgets/ProviderCombo.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <utility>

namespace lumen::ui {

namespace {
const QString kVariantsKey = QStringLiteral("editor/ai/variants");
// {engine id: [ChosenLora…]} as JSON.
const QString kLorasKey = QStringLiteral("editor/ai/loras");
// {RunSettingsSpec::key: RunSettings} as JSON.
const QString kRunSettingsKey = QStringLiteral("editor/ai/settings");
}

QJsonObject ChosenLora::toJson() const
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("path"), path},
            {QStringLiteral("file"), file},
            {QStringLiteral("weight"), weight},
            {QStringLiteral("on"), on},
            {QStringLiteral("trigger_words"), QJsonArray::fromStringList(triggerWords)}};
}

ChosenLora ChosenLora::fromJson(const QJsonObject &o)
{
    ChosenLora l;
    l.name = o.value(QStringLiteral("name")).toString();
    l.path = o.value(QStringLiteral("path")).toString();
    l.file = o.value(QStringLiteral("file")).toString();
    l.weight = o.value(QStringLiteral("weight")).toDouble(0.8);
    l.on = o.value(QStringLiteral("on")).toBool(true);
    for (const QJsonValue &w : o.value(QStringLiteral("trigger_words")).toArray())
        l.triggerWords << w.toString();
    return l;
}

AiOptions::AiOptions(QObject *parent)
    : QObject(parent)
{
    m_variants = std::clamp(Settings::instance().value(kVariantsKey, m_variants).toInt(), 1, kMaxVariants);
    const QJsonObject saved = QJsonDocument::fromJson(Settings::instance().value(kLorasKey).toByteArray()).object();
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        QList<ChosenLora> list;
        for (const QJsonValue &v : it.value().toArray())
            list << ChosenLora::fromJson(v.toObject());
        if (!list.isEmpty())
            m_loras.insert(it.key(), list);
    }
    const QJsonObject settings = QJsonDocument::fromJson(Settings::instance().value(kRunSettingsKey).toByteArray()).object();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const ai::RunSettings s = ai::RunSettings::fromJson(it.value().toObject());
        if (!s.isEmpty())
            m_runSettings.insert(it.key(), s);
    }
}

void AiOptions::setRunSettings(const QString &key, const ai::RunSettings &settings)
{
    if (key.isEmpty() || m_runSettings.value(key) == settings)
        return;
    if (settings.isEmpty())
        m_runSettings.remove(key);
    else
        m_runSettings.insert(key, settings);
    QJsonObject saved;
    for (auto it = m_runSettings.cbegin(); it != m_runSettings.cend(); ++it)
        saved.insert(it.key(), it.value().toJson());
    Settings::instance().setValue(kRunSettingsKey, QJsonDocument(saved).toJson(QJsonDocument::Compact));
    emit changed();
}

void AiOptions::setLoras(const QString &providerId, const QList<ChosenLora> &loras)
{
    if (providerId.isEmpty() || m_loras.value(providerId) == loras)
        return;
    if (loras.isEmpty())
        m_loras.remove(providerId);
    else
        m_loras.insert(providerId, loras);
    QJsonObject saved;
    for (auto it = m_loras.cbegin(); it != m_loras.cend(); ++it) {
        QJsonArray list;
        for (const ChosenLora &l : it.value())
            list.append(l.toJson());
        saved.insert(it.key(), list);
    }
    Settings::instance().setValue(kLorasKey, QJsonDocument(saved).toJson(QJsonDocument::Compact));
    emit changed();
}

QList<ai::LoraRef> AiOptions::loraRefs(const QString &providerId) const
{
    QList<ai::LoraRef> out;
    for (const ChosenLora &l : m_loras.value(providerId)) {
        if (!l.on)
            continue;
        ai::LoraRef ref;
        ref.name = l.name;
        // The provider finds it among the server's by this path (its relative path is a suffix of it).
        ref.file = l.file.isEmpty() ? l.path : l.file;
        ref.weight = l.weight;
        out << ref;
    }
    return out;
}

QString AiOptions::withTriggerWords(const QString &providerId, const QString &prompt) const
{
    QStringList missing;
    for (const ChosenLora &l : m_loras.value(providerId)) {
        if (!l.on)
            continue;
        for (const QString &w : l.triggerWords) {
            if (!w.isEmpty() && !prompt.contains(w, Qt::CaseInsensitive) && !missing.contains(w))
                missing << w;
        }
    }
    if (missing.isEmpty())
        return prompt;
    return prompt.trimmed().isEmpty() ? missing.join(QStringLiteral(", ")) : missing.join(QStringLiteral(", ")) + QStringLiteral(", ") + prompt;
}

void AiOptions::setPrompt(const QString &prompt)
{
    if (prompt == m_prompt)
        return;
    m_prompt = prompt;
    emit promptChanged();
}

void AiOptions::setProviderId(bool changes, const QString &id)
{
    set(m_providerId[changes ? 1 : 0], id);
}

ai::ImageProvider *AiOptions::provider(const ai::ProviderRegistry *registry, bool changes) const
{
    const QString &id = m_providerId[changes ? 1 : 0];
    if (!id.isEmpty() && registry->isEnabled(id)) {
        if (ai::ImageProvider *p = registry->imageProvider(id))
            return p;
    }
    return imageEngineInUse(registry, changes);
}

void AiOptions::followSettings(const ai::ProviderRegistry *registry)
{
    // Settings → AI's choice for each use, as stored (whether it is on or not, which changes without
    // a new choice).
    auto look = [this, registry] {
        bool cleared = false;
        for (const bool changes : {false, true}) {
            const ai::ImageProvider *p = registry->imageProviderFor(changes);
            const QString id = p ? p->instanceId() : QString();
            QString &seen = m_settingsEngine[changes ? 1 : 0];
            if (id == seen)
                continue;
            seen = id;
            cleared = !std::exchange(m_providerId[changes ? 1 : 0], QString()).isEmpty() || cleared;
        }
        if (cleared)
            emit changed();
    };
    for (const bool changes : {false, true}) {
        const ai::ImageProvider *p = registry->imageProviderFor(changes);
        m_settingsEngine[changes ? 1 : 0] = p ? p->instanceId() : QString();
    }
    connect(registry, &ai::ProviderRegistry::rolesChanged, this, look);
}

void AiOptions::setRegionStrength(double strength)
{
    set(m_regionStrength, std::clamp(strength, 0.05, 1.0));
}

void AiOptions::setVariationStrength(double strength)
{
    set(m_variationStrength, std::clamp(strength, 0.05, 1.0));
}

void AiOptions::setSeed(qint64 seed)
{
    set(m_seed, seed < 0 ? qint64(-1) : seed);
}

void AiOptions::setVariants(int variants)
{
    variants = std::clamp(variants, 1, kMaxVariants);
    if (variants == m_variants)
        return;
    Settings::instance().setValue(kVariantsKey, variants);
    set(m_variants, variants);
}

void AiOptions::setNegative(const QString &negative)
{
    set(m_negative, negative);
}

void AiOptions::setUsePose(bool use)
{
    set(m_usePose, use);
}

void AiOptions::setPadding(int pixels)
{
    set(m_padding, std::max(0, pixels));
}

void AiOptions::setFeather(int pixels)
{
    set(m_feather, std::max(0, pixels));
}

void AiOptions::setColorMatch(bool on)
{
    set(m_colorMatch, on);
}

} // namespace lumen::ui
