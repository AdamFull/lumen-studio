#include "ui/pages/AiOptions.h"

#include "ai/ProviderRegistry.h"
#include "core/Settings.h"
#include "ui/widgets/ProviderCombo.h"

#include <algorithm>

namespace lumen::ui {

namespace {
const QString kVariantsKey = QStringLiteral("editor/ai/variants");
}

AiOptions::AiOptions(QObject *parent)
    : QObject(parent)
{
    m_variants = std::clamp(Settings::instance().value(kVariantsKey, m_variants).toInt(), 1, kMaxVariants);
}

void AiOptions::setPrompt(const QString &prompt)
{
    if (prompt == m_prompt)
        return;
    m_prompt = prompt;
    emit promptChanged();
}

void AiOptions::setProviderId(const QString &id)
{
    set(m_providerId, id);
}

ai::ImageProvider *AiOptions::provider(const ai::ProviderRegistry *registry) const
{
    if (!m_providerId.isEmpty() && registry->isEnabled(m_providerId)) {
        if (ai::ImageProvider *p = registry->imageProvider(m_providerId))
            return p;
    }
    return imageEngineInUse(registry);
}

void AiOptions::setRegionStrength(double strength)
{
    set(m_regionStrength, std::clamp(strength, 0.05, 1.0));
}

void AiOptions::setVariationStrength(double strength)
{
    set(m_variationStrength, std::clamp(strength, 0.05, 1.0));
}

void AiOptions::setSteps(int steps)
{
    set(m_steps, std::max(0, steps));
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
