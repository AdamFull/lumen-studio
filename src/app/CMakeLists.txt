qt_add_executable(lumen WIN32 MACOSX_BUNDLE main.cpp)
set_target_properties(lumen PROPERTIES OUTPUT_NAME "LumenStudio")
target_link_libraries(lumen PRIVATE lumen_ui)
target_compile_definitions(lumen PRIVATE LUMEN_VERSION="${PROJECT_VERSION}")
lumen_configure_target(lumen)

# Bundled data (workflow templates, presets) next to the executable: <bin>/data.
add_custom_command(TARGET lumen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources/workflows" "$<TARGET_FILE_DIR:lumen>/data/workflows"
    COMMENT "Copying bundled workflows")
