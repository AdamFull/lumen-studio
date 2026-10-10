#include "ai/ProviderCatalog.h"

#include "ai/ProviderRegistry.h"
#include "ai/providers/AnthropicProvider.h"
#include "ai/providers/ComfyUiProvider.h"
#include "ai/providers/FalProvider.h"
#include "ai/providers/GeminiProvider.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "ai/providers/OpenAiImageProvider.h"
#include "ai/providers/PartServerProvider.h"
#include "ai/providers/SdCppProvider.h"
#include "ai/providers/SdWebUiProvider.h"
#include "ai/providers/StabilityProvider.h"

namespace lumen::ai {

namespace {

template<typename T>
ProviderRegistry::Factory factory()
{
    return [](const ProviderType &type, const QString &id, net::HttpClient *http, QObject *parent) -> Provider * {
        return new T(type, id, http, parent);
    };
}

} // namespace

void registerBuiltInProviders(ProviderRegistry &registry)
{
    // Image back-ends: local first.
    registry.registerType(SdCppProvider::describe(), factory<SdCppProvider>());
    registry.registerType(SdWebUiProvider::describe(), factory<SdWebUiProvider>());
    registry.registerType(ComfyUiProvider::describe(), factory<ComfyUiProvider>());
    registry.registerType(OpenAiImageProvider::describe(), factory<OpenAiImageProvider>());
    registry.registerType(GeminiImageProvider::describe(), factory<GeminiImageProvider>());
    registry.registerType(StabilityProvider::describe(), factory<StabilityProvider>());
    registry.registerType(FalProvider::describe(), factory<FalProvider>());
    registry.registerType(PartServerProvider::describe(), factory<PartServerProvider>());
    // LLM back-ends.
    for (const ProviderType &t : OpenAiChatProvider::describeAll())
        registry.registerType(t, factory<OpenAiChatProvider>());
    registry.registerType(AnthropicProvider::describe(), factory<AnthropicProvider>());
    registry.registerType(GeminiChatProvider::describe(), factory<GeminiChatProvider>());
}

} // namespace lumen::ai
