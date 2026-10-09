#include "core/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace lumen::paths {

namespace {
// LUMEN_DATA_DIR relocates all writable data (portable installs, tests, separate profiles).
QString overrideRoot()
{
    static const QString root = qEnvironmentVariable("LUMEN_DATA_DIR");
    return root;
}
} // namespace

QString appData()
{
    if (!overrideRoot().isEmpty())
        return overrideRoot();
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString cache()
{
    if (!overrideRoot().isEmpty())
        return overrideRoot() + QStringLiteral("/cache");
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString logs()
{
    return appData() + QStringLiteral("/logs");
}

QString userPresets()
{
    return appData() + QStringLiteral("/presets");
}

QString defaultProjects()
{
    if (!overrideRoot().isEmpty())
        return overrideRoot() + QStringLiteral("/projects");
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Lumen Studio");
}

QString bundledData()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/data");
}

bool ensure()
{
    bool ok = true;
    for (const QString &dir : {appData(), cache(), logs(), userPresets()})
        ok = QDir().mkpath(dir) && ok;
    return ok;
}

} // namespace lumen::paths
