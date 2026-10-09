#pragma once

#include <QString>

namespace lumen::paths {

// Per-user writable application data (settings, provider configs, user presets).
QString appData();
// Disposable cache (thumbnails, temporary renders).
QString cache();
QString logs();
// Library of user-created presets (poses, prompts, styles).
QString userPresets();
// Default location for new projects.
QString defaultProjects();
// Directory with bundled read-only resources shipped next to the executable (optional).
QString bundledData();

// Creates all writable directories. Returns false if any could not be created.
bool ensure();

} // namespace lumen::paths
