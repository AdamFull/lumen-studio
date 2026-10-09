cmake_minimum_required(VERSION 3.25)

project(LumenStudio
    VERSION 0.1.0
    DESCRIPTION "AI-assisted image generation and editing studio"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(LUMEN_BUILD_TESTS "Build unit and contract tests" ON)
option(LUMEN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LumenCompilerOptions)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core Gui Widgets Network Concurrent Sql Svg SvgWidgets OpenGL OpenGLWidgets)
find_package(Qt6 6.5 QUIET OPTIONAL_COMPONENTS WebSockets)

if(TARGET Qt6::WebSockets)
    set(LUMEN_HAVE_WEBSOCKETS ON)
    message(STATUS "Lumen: Qt WebSockets found - ComfyUI live progress enabled")
else()
    set(LUMEN_HAVE_WEBSOCKETS OFF)
    message(STATUS "Lumen: Qt WebSockets NOT found - ComfyUI progress falls back to polling")
endif()

qt_standard_project_setup(REQUIRES 6.5)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(GNUInstallDirs)
# Windows installs are flat: the executable and its DLLs at the top of the install folder
# (a normal variable, so it wins over the "bin" cache entry GNUInstallDirs creates).
if(WIN32)
    set(CMAKE_INSTALL_BINDIR ".")
endif()

add_subdirectory(src)

# ---- Packaging (cpack): NSIS installer when makensis is available, a plain ZIP otherwise ----------
set(CPACK_PACKAGE_NAME "LumenStudio")
set(CPACK_PACKAGE_VENDOR "Lumen")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Lumen Studio")
set(CPACK_PACKAGE_FILE_NAME "LumenStudio-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_EXECUTABLES "LumenStudio;Lumen Studio")
# scripts/package.sh builds the portable ZIP itself (it adds the portable.txt marker).
find_program(LUMEN_MAKENSIS makensis PATHS "C:/Program Files (x86)/NSIS" "C:/Program Files/NSIS")
if(LUMEN_MAKENSIS)
    set(CPACK_GENERATOR "NSIS")
else()
    set(CPACK_GENERATOR "ZIP")
endif()
set(CPACK_NSIS_DISPLAY_NAME "Lumen Studio")
set(CPACK_NSIS_PACKAGE_NAME "Lumen Studio ${PROJECT_VERSION}")
set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/lumen.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app/lumen.ico")
set(CPACK_NSIS_INSTALLED_ICON_NAME "LumenStudio.exe")
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "LumenStudio.exe")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$DESKTOP\\\\Lumen Studio.lnk' '$INSTDIR\\\\LumenStudio.exe'")
set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$DESKTOP\\\\Lumen Studio.lnk'")
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
include(CPack)

if(LUMEN_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
