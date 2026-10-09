#include "core/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace lumen::paths {

namespace {
// LUMEN_DATA_DIR relocates all writable data (tests, separate profiles). A portable.txt file next to
// the executable (the portable ZIP) keeps everything in <exe dir>/userdata.
QString overrideRoot()
{
    static const QString root = [] {
        const QString env = qEnvironmentVariable("LUMEN_DATA_DIR");
        if (!env.isEmpty())
            return env;
        if (QCoreApplication::instance() && isPortable())
            return QCoreApplication::applicationDirPath() + QStringLiteral("/userdata");
        return QString();
    }();
    return root;
}
} // namespace

bool isPortable()
{
    return QFile::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/portable.txt"));
}

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
