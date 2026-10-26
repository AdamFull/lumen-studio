add_library(lumen_ui STATIC
    Theme.h Theme.cpp
    Icons.h Icons.cpp
    Shortcuts.h Shortcuts.cpp
    Accessibility.h Accessibility.cpp
    ext/Extension.h ext/Extension.cpp
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
    dialogs/PoseEditDialog.h dialogs/PoseEditDialog.cpp
    widgets/ThumbnailCache.h widgets/ThumbnailCache.cpp
    pose/PoseCanvas.h pose/PoseCanvas.cpp
    pages/LibraryPage.h pages/LibraryPage.cpp
    pages/GeneratePage.h pages/GeneratePage.cpp
    filters/GraphModel.h filters/GraphModel.cpp
    filters/ParamWidgets.h filters/ParamWidgets.cpp
    filters/FilterCatalog.h filters/FilterCatalog.cpp
    filters/FilterRunner.h filters/FilterRunner.cpp
    filters/NodeScene.h filters/NodeScene.cpp
    filters/NodeEditorWindow.h filters/NodeEditorWindow.cpp
    filters/FiltersPanel.h filters/FiltersPanel.cpp
)
target_include_directories(lumen_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_ui PUBLIC lumen_ai lumen_runtime lumen_live2d lumen_doc lumen_pose lumen_filters lumen_core
    Qt6::Widgets Qt6::Svg Qt6::OpenGL Qt6::OpenGLWidgets)
lumen_configure_target(lumen_ui)

# The core never depends on a plugin: plugins link against lumen_ui, not the reverse
# (src/plugins). Fails the configure step when a core source includes a plugin header.
get_target_property(LUMEN_UI_SOURCES lumen_ui SOURCES)
foreach(source IN LISTS LUMEN_UI_SOURCES)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" plugin_includes REGEX "#include \"plugins/")
    if(plugin_includes)
        message(FATAL_ERROR "src/ui/${source} includes a plugin header (${plugin_includes}); the core must not depend on plugins.")
    endif()
endforeach()

# Built-in filters (M17 §17.6), read-only, as :/filters/builtin/*.lumenfilter. After the check
# above, which reads the sources and not the generated resource file.
file(GLOB LUMEN_BUILTIN_FILTERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/resources/filters/*.lumenfilter")
qt_add_resources(lumen_ui builtin_filters
    PREFIX "/filters/builtin"
    BASE "${PROJECT_SOURCE_DIR}/resources/filters"
    FILES ${LUMEN_BUILTIN_FILTERS}
)
