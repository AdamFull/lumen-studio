add_library(lumen_net STATIC
    HttpClient.h HttpClient.cpp
    SseParser.h SseParser.cpp
)
target_include_directories(lumen_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_net PUBLIC lumen_core Qt6::Network)
if(LUMEN_HAVE_WEBSOCKETS)
    target_link_libraries(lumen_net PUBLIC Qt6::WebSockets)
    target_compile_definitions(lumen_net PUBLIC LUMEN_HAVE_WEBSOCKETS=1)
endif()
lumen_configure_target(lumen_net)
