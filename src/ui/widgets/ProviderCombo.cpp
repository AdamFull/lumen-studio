#include "ui/widgets/ProviderCombo.h"

#include "ai/ModelDefaults.h"
#include "ui/Icons.h"

#include <QCoreApplication>

namespace lumen::ui {

ai::ImageProvider *imageEngineInUse(const ai::ProviderRegistry *registry, bool changes)
{
    if (ai::ImageProvider *p = registry->imageProviderFor(changes); p && registry->isEnabled(p->instanceId()))
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
    // A new engine chosen in Settings → AI or set up by the wizard shows at once.
    connect(registry, &ai::ProviderRegistry::rolesChanged, this, [this] {
        if (m_kind != ai::ProviderKind::Image || !m_follows)
            return;
        const int idx = findData(settingsChoice());
        if (idx >= 0 && idx != currentIndex())
            setCurrentIndex(idx);
    });
    connect(this, &QComboBox::currentIndexChanged, this, [this] { emit providerChanged(providerId()); });
    // Only a choice of the user's pins a provider: the list also changes when engines come and go.
    connect(this, &QComboBox::activated, this, [this] {
        m_follows = m_kind != ai::ProviderKind::Image || providerId() == settingsChoice();
        emit picked(providerId());
    });
}

void ProviderCombo::setUse(Use use)
{
    if (m_use == use)
        return;
    m_use = use;
    m_follows = true;
    reload();
}

QString ProviderCombo::settingsChoice() const
{
    if (m_kind != ai::ProviderKind::Image)
        return {};
    ai::ImageProvider *p = imageEngineInUse(m_registry, m_use == Use::Changes);
    return p ? p->instanceId() : QString();
}

void ProviderCombo::reload()
{
    const QString current = providerId();
    QSignalBlocker block(this);
    clear();
    for (ai::Provider *p : m_registry->providers(m_kind)) {
        if (!m_registry->isEnabled(p->instanceId()))
            continue;
        // Image engines by the name the status bar uses ("FLUX.2 [klein] 4B on this computer").
        addItem(Icons::get(p->type().local ? QStringLiteral("server") : QStringLiteral("cloud")),
                m_kind == ai::ProviderKind::Image ? imageEngineName(p) : p->displayName(), p->instanceId());
        setItemData(count() - 1, p->displayName(), Qt::ToolTipRole);
    }
    // A provider picked by hand that is gone or turned off: back to the engine of Settings → AI.
    if (!m_follows && findData(current) < 0)
        m_follows = true;
    QString preferred = current;
    if (m_kind == ai::ProviderKind::Image && (m_follows || preferred.isEmpty()))
        preferred = settingsChoice();
    if (count() == 0) {
        addItem(Icons::get(QStringLiteral("warning")), m_kind == ai::ProviderKind::Image ? tr("No image engine") : tr("No language model"),
                QString());
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
    if (idx >= 0) {
        setCurrentIndex(idx);
        m_follows = m_kind != ai::ProviderKind::Image || id == settingsChoice();
        emit picked(id);
    }
}

void ProviderCombo::setPinnedProviderId(const QString &id)
{
    // Nothing picked, or a provider that is gone or off: the engine of Settings → AI.
    const int pinned = id.isEmpty() ? -1 : findData(id);
    m_follows = pinned < 0 || m_kind != ai::ProviderKind::Image || id == settingsChoice();
    const int idx = pinned >= 0 ? pinned : findData(settingsChoice());
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
