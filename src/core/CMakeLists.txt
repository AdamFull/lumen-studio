add_library(lumen_core STATIC
    Log.h Log.cpp
    Paths.h Paths.cpp
    Settings.h Settings.cpp
    SecretStore.h SecretStore.cpp
    Async.h
    Error.h Error.cpp
    Job.h Job.cpp
    JobQueue.h JobQueue.cpp
    ImageOps.h ImageOps.cpp
    MaskOps.h MaskOps.cpp
    Psd.h Psd.cpp
    JsonSchema.h JsonSchema.cpp
    JsonUtil.h JsonUtil.cpp
    PngText.h PngText.cpp
    LatencyWatchdog.h LatencyWatchdog.cpp
)
target_include_directories(lumen_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent)
if(WIN32)
    target_link_libraries(lumen_core PRIVATE advapi32)
endif()
lumen_configure_target(lumen_core)
