#pragma once

namespace lumen::plugins {

// Registers the plugins built into Lumen Studio (characters, poses, Live2D parts) with
// ui::ext::Extensions. Called once at start, before the Studio and the main window; repeated
// calls change nothing.
void registerBuiltInPlugins();

} // namespace lumen::plugins
