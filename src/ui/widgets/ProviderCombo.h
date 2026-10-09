#pragma once

#include "ai/ProviderRegistry.h"

#include <QComboBox>

namespace lumen::ui {

// Combo box listing enabled image (or LLM) providers; keeps itself in sync with the registry.
class ProviderCombo : public QComboBox
{
    Q_OBJECT
public:
    ProviderCombo(ai::ProviderRegistry *registry, ai::ProviderKind kind, QWidget *parent = nullptr);

    QString providerId() const { return currentData().toString(); }
    void setProviderId(const QString &id);
    ai::ImageProvider *imageProvider() const;
    ai::LlmProvider *llmProvider() const;

signals:
    void providerChanged(const QString &id);

private:
    void reload();
    ai::ProviderRegistry *m_registry;
    ai::ProviderKind m_kind;
};

} // namespace lumen::ui
