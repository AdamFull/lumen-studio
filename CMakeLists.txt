cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW) # normalized install() destinations
endif()

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
# No "Run" on the last page: started by the elevated installer, the program would run as
# administrator, where Windows blocks dropping files on it from Explorer.
# The executable is at the top of the install folder (CPack assumes bin\ for the shortcuts).
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
# Without a licence of its own, no page: CPack would show its placeholder text.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
else()
    set(CPACK_NSIS_IGNORE_LICENSE_PAGE ON)
endif()
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_CREATE_ICONS_EXTRA "CreateShortCut '$DESKTOP\\\\Lumen Studio.lnk' '$INSTDIR\\\\LumenStudio.exe'")
set(CPACK_NSIS_DELETE_ICONS_EXTRA "Delete '$DESKTOP\\\\Lumen Studio.lnk'")
# Double-clicking a project opens it. Uninstalling leaves the user's data and models in place.
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    WriteRegStr HKCR '.lumen' '' 'LumenStudio.Project'
    WriteRegStr HKCR 'LumenStudio.Project' '' 'Lumen Studio project'
    WriteRegStr HKCR 'LumenStudio.Project\\\\DefaultIcon' '' '$INSTDIR\\\\LumenStudio.exe,0'
    WriteRegStr HKCR 'LumenStudio.Project\\\\shell\\\\open\\\\command' '' '\\\"$INSTDIR\\\\LumenStudio.exe\\\" \\\"%1\\\"'
")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    DeleteRegKey HKCR '.lumen'
    DeleteRegKey HKCR 'LumenStudio.Project'
")
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
include(CPack)

if(LUMEN_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
