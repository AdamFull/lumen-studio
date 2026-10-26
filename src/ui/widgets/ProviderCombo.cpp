#include "ui/widgets/ProviderCombo.h"

#include "ui/Icons.h"

namespace lumen::ui {

ProviderCombo::ProviderCombo(ai::ProviderRegistry *registry, ai::ProviderKind kind, QWidget *parent)
    : QComboBox(parent)
    , m_registry(registry)
    , m_kind(kind)
{
    setMinimumWidth(160);
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
    connect(this, &QComboBox::activated, this, [this] { m_follows = m_kind != ai::ProviderKind::Image || providerId() == settingsChoice(); });
}

void ProviderCombo::setUse(Use use)
{
    m_use = use;
    m_follows = true;
    reload();
}

QString ProviderCombo::settingsChoice() const
{
    if (m_kind != ai::ProviderKind::Image)
        return {};
    ai::ImageProvider *p = m_registry->imageProviderFor(m_use == Use::Changes);
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
        addItem(Icons::get(p->type().local ? QStringLiteral("server") : QStringLiteral("cloud")), p->displayName(), p->instanceId());
    }
    // A provider picked by hand that is gone or turned off: back to the engine of Settings → AI.
    if (!m_follows && findData(current) < 0)
        m_follows = true;
    QString preferred = current;
    if (m_kind == ai::ProviderKind::Image && (m_follows || preferred.isEmpty()))
        preferred = settingsChoice();
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
    if (idx >= 0) {
        setCurrentIndex(idx);
        m_follows = m_kind != ai::ProviderKind::Image || id == settingsChoice();
    }
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
