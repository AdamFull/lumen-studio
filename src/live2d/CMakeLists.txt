add_library(lumen_live2d STATIC
    CubismCore.h CubismCore.cpp
    Motion.h Motion.cpp
    CubismModel.h CubismModel.cpp
    CubismRenderer.h CubismRenderer.cpp
)
target_include_directories(lumen_live2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_live2d PUBLIC lumen_core Qt6::Gui Qt6::Concurrent)
lumen_configure_target(lumen_live2d)
