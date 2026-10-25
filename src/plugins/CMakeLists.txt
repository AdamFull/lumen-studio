# Built-in plugins (plan M17 §17.7): each is a static library on top of the core (lumen_ui) and
# registers a ui::ext::Extension; lumen_plugins registers them all (registerBuiltInPlugins).

add_library(lumen_plugin_characters STATIC
    characters/CharactersPlugin.h characters/CharactersPlugin.cpp
    characters/CreatePage.h characters/CreatePage.cpp
    characters/assistant/DirectorActions.h characters/assistant/DirectorActions.cpp
    characters/assistant/AssistantTools.h characters/assistant/AssistantTools.cpp
    characters/assistant/AssistantPanel.h characters/assistant/AssistantPanel.cpp
    characters/director/DirectorSession.h characters/director/DirectorSession.cpp
    characters/director/ShotCard.h characters/director/ShotCard.cpp
    characters/training/CharacterTraining.h characters/training/CharacterTraining.cpp
    characters/training/LoraEvaluation.h characters/training/LoraEvaluation.cpp
    characters/training/LoraDatasetDialog.h characters/training/LoraDatasetDialog.cpp
    characters/training/CloudTrainingPage.h characters/training/CloudTrainingPage.cpp
)
target_link_libraries(lumen_plugin_characters PUBLIC lumen_ui)
lumen_configure_target(lumen_plugin_characters)

add_library(lumen_plugin_poses STATIC
    poses/PosesPlugin.h poses/PosesPlugin.cpp
    poses/PosePage.h poses/PosePage.cpp
)
target_link_libraries(lumen_plugin_poses PUBLIC lumen_ui)
lumen_configure_target(lumen_plugin_poses)

add_library(lumen_plugin_parts STATIC
    parts/PartsPlugin.h parts/PartsPlugin.cpp
    parts/PartTools.h parts/PartTools.cpp
    parts/PartListPanel.h parts/PartListPanel.cpp
    parts/PuppetPreview.h parts/PuppetPreview.cpp
    parts/PartsPage.h parts/PartsPage.cpp
    parts/ExpressionCheck.h parts/ExpressionCheck.cpp
    parts/CubismView.h parts/CubismView.cpp
    parts/CubismPanel.h parts/CubismPanel.cpp
)
target_link_libraries(lumen_plugin_parts PUBLIC lumen_ui)
lumen_configure_target(lumen_plugin_parts)

add_library(lumen_plugins STATIC BuiltInPlugins.h BuiltInPlugins.cpp)
target_link_libraries(lumen_plugins PUBLIC lumen_plugin_characters lumen_plugin_poses lumen_plugin_parts)
lumen_configure_target(lumen_plugins)
