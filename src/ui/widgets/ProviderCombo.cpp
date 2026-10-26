#include "ui/widgets/ProviderCombo.h"

#include "ai/ModelDefaults.h"
#include "ui/Icons.h"

#include <QCoreApplication>

namespace lumen::ui {

ai::ImageProvider *imageEngineInUse(const ai::ProviderRegistry *registry)
{
    if (ai::ImageProvider *p = registry->defaultImageProvider(); p && registry->isEnabled(p->instanceId()))
        return p;
    for (ai::ImageProvider *p : registry->imageProviders()) {
        if (registry->isEnabled(p->instanceId()))
            return p;
    }
    return nullptr;
}

QString imageEngineName(const ai::Provider *provider)
{
    if (!provider)
        return QString();
    // A server Lumen starts itself: the model it runs says more than the provider's name.
    if (!provider->text(QStringLiteral("managed_server")).isEmpty()) {
        const ai::ModelDefaults preset = ai::modelDefaultsOf(provider);
        return QCoreApplication::translate("lumen::ui::ProviderCombo", "%1 on this computer")
            .arg(preset.isValid() ? preset.name : provider->displayName());
    }
    return provider->displayName();
}

ProviderCombo::ProviderCombo(ai::ProviderRegistry *registry, ai::ProviderKind kind, QWidget *parent)
    : QComboBox(parent)
    , m_registry(registry)
    , m_kind(kind)
{
    setMinimumWidth(160);
    setAccessibleName(kind == ai::ProviderKind::Image ? tr("Image engine") : tr("Language model"));
    reload();
    connect(registry, &ai::ProviderRegistry::providersChanged, this, &ProviderCombo::reload);
    connect(this, &QComboBox::currentIndexChanged, this, [this] { emit providerChanged(providerId()); });
}

void ProviderCombo::reload()
{
    const QString current = providerId();
    QSignalBlocker block(this);
    clear();
    QString preferred = current;
    if (preferred.isEmpty() && m_kind == ai::ProviderKind::Image) {
        if (ai::ImageProvider *p = m_registry->defaultImageProvider())
            preferred = p->instanceId();
    }
    for (ai::Provider *p : m_registry->providers(m_kind)) {
        if (!m_registry->isEnabled(p->instanceId()))
            continue;
        addItem(Icons::get(p->type().local ? QStringLiteral("server") : QStringLiteral("cloud")), p->displayName(), p->instanceId());
    }
    if (count() == 0) {
        addItem(Icons::get(QStringLiteral("warning")), tr("No provider configured"), QString());
        setEnabled(false);
    } else {
        setEnabled(true);
    }
    const int idx = findData(preferred);
    setCurrentIndex(idx >= 0 ? idx : 0);
    block.unblock();
    if (providerId() != current)
        emit providerChanged(providerId());
}

void ProviderCombo::setProviderId(const QString &id)
{
    const int idx = findData(id);
    if (idx >= 0)
        setCurrentIndex(idx);
}

ai::ImageProvider *ProviderCombo::imageProvider() const
{
    return m_registry->imageProvider(providerId());
}

ai::LlmProvider *ProviderCombo::llmProvider() const
{
    return m_registry->llmProvider(providerId());
}

} // namespace lumen::ui
