#include "plugins/characters/CharactersPlugin.h"

#include "plugins/characters/CreatePage.h"
#include "plugins/characters/assistant/AssistantPanel.h"
#include "plugins/characters/training/CloudTrainingPage.h"

#include <QCoreApplication>

namespace lumen::plugins {

QString CharactersPlugin::name() const
{
    return QCoreApplication::translate("lumen::plugins::CharactersPlugin", "Characters");
}

QString CharactersPlugin::description() const
{
    return QCoreApplication::translate("lumen::plugins::CharactersPlugin",
                                       "Tools → Create: an AI director that builds a character sheet from reference images and plans "
                                       "sets of shots with poses. Also the assistant (View → Assistant), and training a character LoRA "
                                       "(locally or on a rented cloud GPU).");
}

// Labels keep the translation contexts they had before the plugin (MainWindow, SettingsDialog).
QList<ui::ext::PageSpec> CharactersPlugin::pages() const
{
    ui::ext::PageSpec create;
    create.id = QStringLiteral("create");
    create.icon = QStringLiteral("chat");
    create.label = QCoreApplication::translate("lumen::ui::MainWindow", "Create");
    create.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "AI director: plan character shots from references and a description");
    create.order = 10;
    create.shortcut = QKeySequence(QStringLiteral("Alt+1"));
    create.create = [](ui::Studio *studio, QWidget *parent) -> QWidget * { return new ui::CreatePage(studio, parent); };
    return {create};
}

QList<ui::ext::PanelSpec> CharactersPlugin::panels() const
{
    ui::ext::PanelSpec assistant;
    assistant.id = QStringLiteral("assistant");
    assistant.icon = QStringLiteral("chat");
    assistant.label = QCoreApplication::translate("lumen::ui::MainWindow", "Assistant");
    assistant.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "Ask the assistant to plan, generate, edit and pose for you");
    assistant.shortcutId = QStringLiteral("app.assistant");
    assistant.shortcut = QKeySequence(QStringLiteral("Ctrl+Shift+A"));
    assistant.order = 10;
    assistant.create = [](ui::Studio *studio, QWidget *parent, std::function<void()> close) -> QWidget * {
        auto *panel = new ui::AssistantPanel(studio, parent);
        QObject::connect(panel, &ui::AssistantPanel::closeRequested, panel, close);
        return panel;
    };
    return {assistant};
}

QList<ui::ext::SettingsTabSpec> CharactersPlugin::settingsTabs() const
{
    ui::ext::SettingsTabSpec cloud;
    cloud.id = QStringLiteral("cloud");
    cloud.icon = QStringLiteral("cloud");
    cloud.label = QCoreApplication::translate("lumen::ui::SettingsDialog", "Cloud training");
    cloud.order = 50;
    cloud.create = [](ui::Studio *studio, QWidget *parent) -> QWidget * { return new ui::CloudTrainingPage(studio, parent); };
    return {cloud};
}

} // namespace lumen::plugins
