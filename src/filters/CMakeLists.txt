# Node filter graphs (M17 §17.6): the graph model, files, evaluation and the classic image nodes.
# Depends on the core only (no UI, AI, document or runtime code), so the AI layer can add node
# types and the UI can build the editor on top.
add_library(lumen_filters STATIC
    NodeType.h NodeType.cpp
    Registry.h Registry.cpp
    Graph.h Graph.cpp
    Evaluator.h Evaluator.cpp
    Library.h Library.cpp
    Ops.h Ops.cpp
    ClassicNodes.h ClassicNodes.cpp
)
target_include_directories(lumen_filters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_filters PUBLIC lumen_core Qt6::Core Qt6::Gui Qt6::Concurrent)
lumen_configure_target(lumen_filters)
