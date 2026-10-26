#pragma once

#include "ai/ProviderRegistry.h"

#include <QComboBox>

namespace lumen::ui {

// The image provider work goes to unless the user picks another: the engine of Settings → AI for
// new images, or for changes to an existing image (`changes`; the image engine unless another is
// set for changes), when it is on, else the first one that is on. Null without one.
ai::ImageProvider *imageEngineInUse(const ai::ProviderRegistry *registry, bool changes = false);
// The image engine in plain words: "FLUX.2 [klein] 4B on this computer" for a server Lumen runs
// with a model it knows, "<name> on this computer" for another server Lumen runs, else the
// provider's name.
QString imageEngineName(const ai::Provider *provider);

// Combo box listing enabled image (or LLM) providers by the names the status bar uses; keeps itself
// in sync with the registry. An image provider combo shows the engine Settings → AI names for its
// use (imageEngineInUse), and follows it when it changes there (or in the setup wizard), until the
// user picks another one in the combo (activated, not a change of the list); picking the engine of
// Settings → AI again follows it again. The editor's combos (the prompt bar's ⋯ menu, the AI panel
// and New from prompt's) keep that choice in AiOptions, one per use (pinnedProviderId, picked).
class ProviderCombo : public QComboBox
{
    Q_OBJECT
public:
    // The engine an image provider combo follows: the image engine (new images), or the engine for
    // changes to an existing image (the editor).
    enum class Use { NewImages, Changes };

    ProviderCombo(ai::ProviderRegistry *registry, ai::ProviderKind kind, QWidget *parent = nullptr);

    void setUse(Use use);
    Use use() const { return m_use; }
    QString providerId() const { return currentData().toString(); }
    // Picks a provider as the user would (picked); picking the engine of Settings → AI follows it
    // again.
    void setProviderId(const QString &id);
    // The provider picked by hand, empty while the combo follows Settings → AI. Setting one shows
    // what was picked elsewhere (no picked); an empty one, or one that is gone, follows Settings → AI.
    QString pinnedProviderId() const { return m_follows ? QString() : providerId(); }
    void setPinnedProviderId(const QString &id);
    ai::ImageProvider *imageProvider() const;
    ai::LlmProvider *llmProvider() const;
    // Whether it shows the engine of Settings → AI (nothing picked by hand).
    bool followsSettings() const { return m_follows; }

signals:
    void providerChanged(const QString &id);
    // The user picked a provider in the list, or setProviderId did (pinnedProviderId says whether it
    // is pinned).
    void picked(const QString &id);

private:
    void reload();
    QString settingsChoice() const;
    ai::ProviderRegistry *m_registry;
    ai::ProviderKind m_kind;
    Use m_use = Use::NewImages;
    bool m_follows = true;
};

} // namespace lumen::ui
