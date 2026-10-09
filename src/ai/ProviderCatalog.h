#pragma once

namespace lumen::ai {

class ProviderRegistry;

// Registers every built-in provider type (image and LLM back-ends) with the registry.
void registerBuiltInProviders(ProviderRegistry &registry);

} // namespace lumen::ai
