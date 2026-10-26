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

# The AI nodes live in lumen_ai, which links this library; never the reverse. Fails the configure
# step when a filters source includes a header of the AI, document, runtime or UI code.
get_target_property(LUMEN_FILTERS_SOURCES lumen_filters SOURCES)
foreach(source IN LISTS LUMEN_FILTERS_SOURCES)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" layer_includes REGEX "#include \"(ai|doc|runtime|ui|plugins)/")
    if(layer_includes)
        message(FATAL_ERROR "src/filters/${source} includes ${layer_includes}; the filters library depends on the core only.")
    endif()
endforeach()
