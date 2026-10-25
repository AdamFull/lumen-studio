#include "plugins/BuiltInPlugins.h"

#include "plugins/characters/CharactersPlugin.h"
#include "plugins/parts/PartsPlugin.h"
#include "plugins/poses/PosesPlugin.h"

namespace lumen::plugins {

void registerBuiltInPlugins()
{
    ui::ext::Extensions &registry = ui::ext::Extensions::instance();
    registry.add(std::make_unique<CharactersPlugin>());
    registry.add(std::make_unique<PosesPlugin>());
    registry.add(std::make_unique<PartsPlugin>());
}

} // namespace lumen::plugins
