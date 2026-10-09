set(LUMEN_APP_SOURCES main.cpp)
if(WIN32)
    # Executable icon and version information.
    set(LUMEN_ICON_PATH "${PROJECT_SOURCE_DIR}/resources/app/lumen.ico")
    configure_file("${PROJECT_SOURCE_DIR}/resources/app/lumen.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/lumen.rc" @ONLY)
    list(APPEND LUMEN_APP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/lumen.rc")
endif()

qt_add_executable(lumen WIN32 MACOSX_BUNDLE ${LUMEN_APP_SOURCES})
set_target_properties(lumen PROPERTIES OUTPUT_NAME "LumenStudio")
target_link_libraries(lumen PRIVATE lumen_ui)
target_compile_definitions(lumen PRIVATE LUMEN_VERSION="${PROJECT_VERSION}")
lumen_configure_target(lumen)

# ---- Translations (Qt Linguist) ------------------------------------------------------------------
# `cmake --build <dir> --target update_translations` refreshes translations/*.ts from the sources;
# the compiled .qm files are embedded under :/i18n.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND)
    file(GLOB LUMEN_TS_FILES "${PROJECT_SOURCE_DIR}/translations/lumen_*.ts")
    if(LUMEN_TS_FILES)
        qt_add_translations(lumen
            SOURCE_TARGETS lumen lumen_ui lumen_ai lumen_doc lumen_core lumen_net lumen_pose lumen_live2d
            TS_FILES ${LUMEN_TS_FILES}
            RESOURCE_PREFIX /i18n
            LUPDATE_OPTIONS -no-obsolete -locations relative)
    endif()
endif()

# Bundled data (workflow templates, presets) next to the executable: <bin>/data.
add_custom_command(TARGET lumen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources/workflows" "$<TARGET_FILE_DIR:lumen>/data/workflows"
    COMMENT "Copying bundled workflows")

# ---- Installation / deployment ------------------------------------------------------------------
# `cmake --install <build> --prefix <dir>` produces a self-contained folder: the executable at the
# top level, Qt and compiler runtime DLLs deployed by windeployqt, bundled data in data/.
install(TARGETS lumen
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE DESTINATION .)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/resources/workflows/" DESTINATION "${CMAKE_INSTALL_BINDIR}/data/workflows")
install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/docs/USER_GUIDE.md" DESTINATION "${CMAKE_INSTALL_BINDIR}/docs")

qt_generate_deploy_app_script(
    TARGET lumen
    OUTPUT_SCRIPT LUMEN_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR
    # Only the SQLite driver is used.
    DEPLOY_TOOL_OPTIONS --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql)
# QtDeploySupport.cmake was generated before CMAKE_INSTALL_BINDIR was flattened: pass it explicitly.
install(CODE "set(QT_DEPLOY_BIN_DIR \"${CMAKE_INSTALL_BINDIR}\")")
install(SCRIPT "${LUMEN_DEPLOY_SCRIPT}")
