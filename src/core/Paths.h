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
// Default folder for Lumen's own models: <Documents>/Lumen/Models, where people find it. While
// writable data is relocated (LUMEN_DATA_DIR, the portable ZIP, Qt's test mode) it is
// <data root>/models, so those stay self-contained.
QString defaultModels();
// Folder of the user's saved filters (.lumenfilter), next to the models folder:
// <Documents>/Lumen/Filters, or <data root>/filters while data is relocated, as for the models.
QString defaultFilters();
// Directory with bundled read-only resources shipped next to the executable (optional).
QString bundledData();

// Creates all writable directories. Returns false if any could not be created.
bool ensure();

} // namespace lumen::paths
