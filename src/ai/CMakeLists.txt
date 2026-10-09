add_library(lumen_ai STATIC
    Types.h Types.cpp
    Operation.h Operation.cpp
    Provider.h ProviderImpl.h Provider.cpp
    ProviderUtil.h ProviderUtil.cpp
    ProviderRegistry.h ProviderRegistry.cpp
    providers/SdWebUiProvider.h providers/SdWebUiProvider.cpp
    providers/OpenAiChatProvider.h providers/OpenAiChatProvider.cpp
)
target_include_directories(lumen_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_ai PUBLIC lumen_core lumen_net lumen_pose lumen_doc Qt6::Gui)
lumen_configure_target(lumen_ai)
