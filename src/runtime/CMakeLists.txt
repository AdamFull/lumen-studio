add_library(lumen_runtime STATIC
    Zip.h Zip.cpp
    Download.h Download.cpp
    Hardware.h Hardware.cpp
    Catalog.h Catalog.cpp
    RuntimeStore.h RuntimeStore.cpp
    ServerProfile.h ServerProfile.cpp
    Supervisor.h Supervisor.cpp
    ModelFormats.h ModelFormats.cpp
    ModelLibrary.h ModelLibrary.cpp
    ModelHub.h ModelHub.cpp
    Trainer.h Trainer.cpp
    LoraTraining.h LoraTraining.cpp
)
qt_add_resources(lumen_runtime runtime_catalog
    PREFIX "/runtimes"
    BASE "${PROJECT_SOURCE_DIR}/resources/runtimes"
    FILES "${PROJECT_SOURCE_DIR}/resources/runtimes/catalog.json"
          "${PROJECT_SOURCE_DIR}/resources/runtimes/trainers.json"
          "${PROJECT_SOURCE_DIR}/resources/runtimes/trainers/kohya-v0.12.0-cu128.txt"
          "${PROJECT_SOURCE_DIR}/resources/runtimes/trainers/kohya-v0.12.0-cu124.txt"
)
qt_add_resources(lumen_runtime model_starters
    PREFIX "/models"
    BASE "${PROJECT_SOURCE_DIR}/resources/models"
    FILES "${PROJECT_SOURCE_DIR}/resources/models/starters.json"
)
find_package(ZLIB QUIET)
target_include_directories(lumen_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_runtime PUBLIC lumen_core lumen_net Qt6::Network)
if(ZLIB_FOUND)
    target_link_libraries(lumen_runtime PRIVATE ZLIB::ZLIB)
else()
    # MinGW ships zlib as part of the toolchain (libz.a + zlib.h).
    target_link_libraries(lumen_runtime PRIVATE z)
endif()
if(WIN32)
    target_link_libraries(lumen_runtime PRIVATE dxgi)
endif()
lumen_configure_target(lumen_runtime)
