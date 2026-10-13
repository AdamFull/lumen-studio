add_library(lumen_pose STATIC
    Skeleton.h Skeleton.cpp
    Anatomy.h Anatomy.cpp
    PoseRenderer.h PoseRenderer.cpp
    PoseEditing.h PoseEditing.cpp
    Rig.h Rig.cpp
    PosePresets.h PosePresets.cpp
    CastLayout.h CastLayout.cpp
)
target_include_directories(lumen_pose PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_pose PUBLIC lumen_core Qt6::Gui)
lumen_configure_target(lumen_pose)
