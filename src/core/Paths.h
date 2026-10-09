#pragma once

#include <QString>

namespace lumen::paths {

// True when a portable.txt marker sits next to the executable: writable data then lives in
// <exe dir>/userdata (LUMEN_DATA_DIR still takes precedence). Resolved once, after QApplication exists.
bool isPortable();
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
