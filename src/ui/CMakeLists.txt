add_library(lumen_ui STATIC
    Theme.h Theme.cpp
    Icons.h Icons.cpp
    Shortcuts.h Shortcuts.cpp
    Accessibility.h Accessibility.cpp
    assistant/DirectorActions.h
    assistant/AssistantTools.h assistant/AssistantTools.cpp
    assistant/AssistantPanel.h assistant/AssistantPanel.cpp
    Studio.h Studio.cpp
    MainWindow.h MainWindow.cpp
    PageFactory.h PageFactory.cpp
    canvas/EditorState.h canvas/EditorState.cpp
    canvas/CanvasView.h canvas/CanvasView.cpp
    canvas/Tool.h
    canvas/Tools.h canvas/Tools.cpp
    canvas/TransformTool.h canvas/TransformTool.cpp
    canvas/ContextBar.h canvas/ContextBar.cpp
    widgets/Controls.h widgets/Controls.cpp
    widgets/FlowLayout.h widgets/FlowLayout.cpp
    widgets/ReferenceStrip.h widgets/ReferenceStrip.cpp
    widgets/CharacterChip.h widgets/CharacterChip.cpp
    widgets/LayersPanel.h widgets/LayersPanel.cpp
    widgets/ProviderCombo.h widgets/ProviderCombo.cpp
    widgets/JobCard.h widgets/JobCard.cpp
    widgets/Toasts.h widgets/Toasts.cpp
    widgets/ServerStatus.h widgets/ServerStatus.cpp
    pages/AiPanel.h pages/AiPanel.cpp
    pages/EditorPage.h pages/EditorPage.cpp
    dialogs/SettingsDialog.h dialogs/SettingsDialog.cpp
    dialogs/ShortcutsPage.h dialogs/ShortcutsPage.cpp
    dialogs/LocalServersPage.h dialogs/LocalServersPage.cpp
    dialogs/ModelsPage.h dialogs/ModelsPage.cpp
    dialogs/ModelHubDialog.h dialogs/ModelHubDialog.cpp
    dialogs/LocalSetupDialog.h dialogs/LocalSetupDialog.cpp
    dialogs/CharacterLoraDialog.h dialogs/CharacterLoraDialog.cpp
    dialogs/CloudTrainingPage.h dialogs/CloudTrainingPage.cpp
    dialogs/LoraDatasetDialog.h dialogs/LoraDatasetDialog.cpp
    CharacterTraining.h CharacterTraining.cpp
    LoraEvaluation.h LoraEvaluation.cpp
    dialogs/PoseEditDialog.h dialogs/PoseEditDialog.cpp
    widgets/ThumbnailCache.h widgets/ThumbnailCache.cpp
    pose/PoseCanvas.h pose/PoseCanvas.cpp
    director/DirectorSession.h director/DirectorSession.cpp
    director/ShotCard.h director/ShotCard.cpp
    pages/LibraryPage.h pages/LibraryPage.cpp
    pages/GeneratePage.h pages/GeneratePage.cpp
    pages/PosePage.h pages/PosePage.cpp
    pages/CreatePage.h pages/CreatePage.cpp
    parts/PartTools.h parts/PartTools.cpp
    parts/PartListPanel.h parts/PartListPanel.cpp
    parts/PuppetPreview.h parts/PuppetPreview.cpp
    parts/PartsPage.h parts/PartsPage.cpp
    parts/ExpressionCheck.h parts/ExpressionCheck.cpp
    parts/CubismView.h parts/CubismView.cpp
    parts/CubismPanel.h parts/CubismPanel.cpp
)
target_include_directories(lumen_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_ui PUBLIC lumen_ai lumen_runtime lumen_live2d lumen_doc lumen_pose lumen_core
    Qt6::Widgets Qt6::Svg Qt6::OpenGL Qt6::OpenGLWidgets)
lumen_configure_target(lumen_ui)
