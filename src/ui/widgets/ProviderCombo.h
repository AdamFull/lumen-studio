#pragma once

#include "ai/ProviderRegistry.h"

#include <QComboBox>

namespace lumen::ui {

// Combo box listing enabled image (or LLM) providers; keeps itself in sync with the registry.
// An image provider combo shows the engine Settings → AI names for its use, and follows it when it
// changes there (or in the setup wizard), until a provider is picked in the combo by hand.
class ProviderCombo : public QComboBox
{
    Q_OBJECT
public:
    // The engine an image provider combo follows: the image engine (new images), or the engine for
    // changes to an existing image (the editor).
    enum class Use { NewImages, Changes };

    ProviderCombo(ai::ProviderRegistry *registry, ai::ProviderKind kind, QWidget *parent = nullptr);

    void setUse(Use use);
    QString providerId() const { return currentData().toString(); }
    // Picks a provider as the user would; picking the engine of Settings → AI follows it again.
    void setProviderId(const QString &id);
    ai::ImageProvider *imageProvider() const;
    ai::LlmProvider *llmProvider() const;
    // Whether it shows the engine of Settings → AI (nothing picked by hand).
    bool followsSettings() const { return m_follows; }

signals:
    void providerChanged(const QString &id);

private:
    void reload();
    QString settingsChoice() const;
    ai::ProviderRegistry *m_registry;
    ai::ProviderKind m_kind;
    Use m_use = Use::NewImages;
    bool m_follows = true;
};

} // namespace lumen::ui
