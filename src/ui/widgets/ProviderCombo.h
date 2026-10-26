#pragma once

#include "ai/ProviderRegistry.h"

#include <QComboBox>

namespace lumen::ui {

// The image provider new work goes to unless the user picks another: the default one when it is
// on, else the first one that is on (what a ProviderCombo shows first). Null without one.
ai::ImageProvider *imageEngineInUse(const ai::ProviderRegistry *registry);
// The image engine in plain words: "FLUX.2 [klein] 4B on this computer" for a server Lumen runs
// with a model it knows, "<name> on this computer" for another server Lumen runs, else the
// provider's name.
QString imageEngineName(const ai::Provider *provider);

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
