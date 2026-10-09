#include "core/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace lumen::paths {

QString appData()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString cache()
{
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
