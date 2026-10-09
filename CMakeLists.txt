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

add_subdirectory(src)

if(LUMEN_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
