add_library(lumen_ai STATIC
    Types.h Types.cpp
    Operation.h Operation.cpp
    Gate.h Gate.cpp
    Provider.h ProviderImpl.h Provider.cpp
    ProviderUtil.h ProviderUtil.cpp
    ProviderRegistry.h ProviderRegistry.cpp
    providers/SdWebUiProvider.h providers/SdWebUiProvider.cpp
    providers/SdCppProvider.h providers/SdCppProvider.cpp
    providers/OpenAiChatProvider.h providers/OpenAiChatProvider.cpp
    providers/AnthropicProvider.h providers/AnthropicProvider.cpp
    providers/GeminiProvider.h providers/GeminiProvider.cpp
    providers/OpenAiImageProvider.h providers/OpenAiImageProvider.cpp
    providers/StabilityProvider.h providers/StabilityProvider.cpp
    providers/FalProvider.h providers/FalProvider.cpp
    Agent.h Agent.cpp
    providers/ComfyUiProvider.h providers/ComfyUiProvider.cpp
    providers/PartServerProvider.h providers/PartServerProvider.cpp
    ProviderCatalog.h ProviderCatalog.cpp
    StructuredChat.h StructuredChat.cpp
    ChangePlan.h ChangePlan.cpp
    PromptRules.h PromptRules.cpp
    PromptStyle.h PromptStyle.cpp
    ModelDefaults.h ModelDefaults.cpp
    RunSettings.h RunSettings.cpp
    Onnx.h Onnx.cpp
    Segmenter.h Segmenter.cpp
    Detector.h Detector.cpp
    GenerationService.h GenerationService.cpp
    FilterNodes.h FilterNodes.cpp
    CastGeneration.h CastGeneration.cpp
    CharacterContext.h CharacterContext.cpp
    Director.h Director.cpp
    LoraCaptions.h LoraCaptions.cpp
    PartPipeline.h PartPipeline.cpp
    PartService.h PartService.cpp
)
target_include_directories(lumen_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# ONNX Runtime's C API headers (the runtime itself is loaded at run time, see Onnx.cpp).
target_include_directories(lumen_ai PRIVATE ${PROJECT_SOURCE_DIR}/third_party/onnxruntime/include)
# lumen_filters for the AI steps of node filters (FilterNodes); the filters library never depends
# on this one.
target_link_libraries(lumen_ai PUBLIC lumen_core lumen_net lumen_pose lumen_doc lumen_filters Qt6::Gui)
lumen_configure_target(lumen_ai)
