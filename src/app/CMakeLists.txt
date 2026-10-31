set(LUMEN_APP_SOURCES main.cpp)
if(WIN32)
    # Executable icon and version information.
    set(LUMEN_ICON_PATH "${PROJECT_SOURCE_DIR}/resources/app/lumen.ico")
    configure_file("${PROJECT_SOURCE_DIR}/resources/app/lumen.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/lumen.rc" @ONLY)
    list(APPEND LUMEN_APP_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/lumen.rc")
endif()

qt_add_executable(lumen WIN32 MACOSX_BUNDLE ${LUMEN_APP_SOURCES})
set_target_properties(lumen PROPERTIES OUTPUT_NAME "LumenStudio")
target_link_libraries(lumen PRIVATE lumen_plugins lumen_ui)
target_compile_definitions(lumen PRIVATE LUMEN_VERSION="${PROJECT_VERSION}")
lumen_configure_target(lumen)

# ---- Translations (Qt Linguist) ------------------------------------------------------------------
# `cmake --build <dir> --target update_translations` refreshes translations/*.ts from the sources;
# the compiled .qm files are embedded under :/i18n.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(Qt6LinguistTools_FOUND)
    file(GLOB LUMEN_TS_FILES "${PROJECT_SOURCE_DIR}/translations/lumen_*.ts")
    # lumen_en.ts only holds English plural forms ("1 image" / "2 images").
    list(FILTER LUMEN_TS_FILES EXCLUDE REGEX "lumen_en\.ts$")
    if(LUMEN_TS_FILES)
        qt_add_translations(lumen
            SOURCE_TARGETS lumen lumen_plugins lumen_plugin_characters lumen_plugin_poses lumen_plugin_parts lumen_ui lumen_ai lumen_runtime lumen_doc lumen_core lumen_net lumen_pose lumen_live2d lumen_filters
            TS_FILES ${LUMEN_TS_FILES}
            PLURALS_TS_FILE "${PROJECT_SOURCE_DIR}/translations/lumen_en.ts"
            RESOURCE_PREFIX /i18n
            LUPDATE_OPTIONS -no-obsolete -locations relative)
    endif()
endif()

# Bundled data (workflow templates, presets) next to the executable: <bin>/data.
add_custom_command(TARGET lumen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources/workflows" "$<TARGET_FILE_DIR:lumen>/data/workflows"
    COMMENT "Copying bundled workflows")

# ---- Runnable build tree (Windows) ---------------------------------------------------------------
# Copies the Qt DLLs, plugins and the MinGW runtime next to the built executable, so it starts by
# double-click or from an IDE without Qt on PATH. windeployqt only copies files that changed.
if(WIN32)
    option(LUMEN_DEPLOY_ON_BUILD "Copy the Qt and compiler runtime next to LumenStudio.exe after each build" ON)
    if(LUMEN_DEPLOY_ON_BUILD)
        find_program(LUMEN_WINDEPLOYQT NAMES windeployqt6 windeployqt
                     HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${Qt6_DIR}/../../../bin" NO_DEFAULT_PATH)
        if(LUMEN_WINDEPLOYQT)
            add_custom_command(TARGET lumen POST_BUILD
                # qoffscreen: tests and headless runs (QT_QPA_PLATFORM=offscreen) use the same folder.
                COMMAND "${LUMEN_WINDEPLOYQT}" --verbose 0 --no-compiler-runtime --include-plugins qoffscreen
                        --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql
                        "$<TARGET_FILE:lumen>"
                COMMENT "Deploying the Qt runtime next to LumenStudio.exe")
        else()
            message(WARNING "windeployqt not found: run LumenStudio.exe with Qt's bin directory on PATH")
        endif()
        # The MinGW runtime from the compiler that built the executable.
        if(MINGW)
            get_filename_component(LUMEN_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
            foreach(dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
                if(EXISTS "${LUMEN_COMPILER_DIR}/${dll}")
                    add_custom_command(TARGET lumen POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LUMEN_COMPILER_DIR}/${dll}" "$<TARGET_FILE_DIR:lumen>")
                endif()
            endforeach()
        endif()
    endif()
endif()

# ---- Installation / deployment ------------------------------------------------------------------
# `cmake --install <build> --prefix <dir>` produces a self-contained folder: the executable at the
# top level, Qt and compiler runtime DLLs deployed by windeployqt, bundled data in data/.
install(TARGETS lumen
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE DESTINATION .)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/resources/workflows/" DESTINATION "${CMAKE_INSTALL_BINDIR}/data/workflows")
install(FILES "${PROJECT_SOURCE_DIR}/README.md" "${PROJECT_SOURCE_DIR}/docs/USER_GUIDE.md" "${PROJECT_SOURCE_DIR}/docs/PLUGINS.md"
              "${PROJECT_SOURCE_DIR}/docs/THIRD_PARTY_NOTICES.md" DESTINATION "${CMAKE_INSTALL_BINDIR}/docs")

qt_generate_deploy_app_script(
    TARGET lumen
    OUTPUT_SCRIPT LUMEN_DEPLOY_SCRIPT
    NO_UNSUPPORTED_PLATFORM_ERROR
    # Only the SQLite driver is used.
    DEPLOY_TOOL_OPTIONS --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql)
# QtDeploySupport.cmake was generated before CMAKE_INSTALL_BINDIR was flattened: pass it explicitly.
install(CODE "set(QT_DEPLOY_BIN_DIR \"${CMAKE_INSTALL_BINDIR}\")")
install(SCRIPT "${LUMEN_DEPLOY_SCRIPT}")
