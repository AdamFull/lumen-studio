add_library(lumen_doc STATIC
    Layer.h Layer.cpp
    TileGrid.h TileGrid.cpp
    CompressedImage.h CompressedImage.cpp
    Document.h Document.cpp
    Commands.h Commands.cpp
    ProjectStore.h ProjectStore.cpp
    Project.h Project.cpp
)
target_include_directories(lumen_doc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_doc PUBLIC lumen_core lumen_pose Qt6::Gui Qt6::Sql Qt6::Concurrent)
lumen_configure_target(lumen_doc)
