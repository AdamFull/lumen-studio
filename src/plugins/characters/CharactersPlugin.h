#pragma once

#include "ui/ext/Extension.h"

namespace lumen::plugins {

// Characters: the Create page (the AI director: character sheets from references, shot plans with
// poses), the assistant, and character LoRA training (dataset builder, local and cloud training).
class CharactersPlugin : public ui::ext::Extension
{
public:
    QString id() const override { return QStringLiteral("characters"); }
    QString name() const override;
    QString description() const override;
    QList<ui::ext::PageSpec> pages() const override;
    QList<ui::ext::PanelSpec> panels() const override;
    QList<ui::ext::SettingsTabSpec> settingsTabs() const override;
    // The director, the assistant and the dataset captions run on a language model.
    bool needsLanguageModel() const override { return true; }
};

} // namespace lumen::plugins
