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
