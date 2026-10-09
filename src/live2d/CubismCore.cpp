#include "live2d/CubismCore.h"

#include "core/Log.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace lumen::live2d {

namespace {

template<typename Fn>
void resolve(QLibrary &lib, Fn &target, const char *name)
{
    target = reinterpret_cast<Fn>(lib.resolve(name));
}

} // namespace

QStringList CoreApi::missingRequired() const
{
    QStringList missing;
    auto check = [&missing](const void *fn, const char *name) {
        if (!fn)
            missing << QString::fromLatin1(name);
    };
    check(reinterpret_cast<const void *>(getVersion), "csmGetVersion");
    check(reinterpret_cast<const void *>(reviveMocInPlace), "csmReviveMocInPlace");
    check(reinterpret_cast<const void *>(getSizeofModel), "csmGetSizeofModel");
    check(reinterpret_cast<const void *>(initializeModelInPlace), "csmInitializeModelInPlace");
    check(reinterpret_cast<const void *>(updateModel), "csmUpdateModel");
    check(reinterpret_cast<const void *>(readCanvasInfo), "csmReadCanvasInfo");
    check(reinterpret_cast<const void *>(getParameterCount), "csmGetParameterCount");
    check(reinterpret_cast<const void *>(getParameterIds), "csmGetParameterIds");
    check(reinterpret_cast<const void *>(getParameterMinimumValues), "csmGetParameterMinimumValues");
    check(reinterpret_cast<const void *>(getParameterMaximumValues), "csmGetParameterMaximumValues");
    check(reinterpret_cast<const void *>(getParameterDefaultValues), "csmGetParameterDefaultValues");
    check(reinterpret_cast<const void *>(getParameterValues), "csmGetParameterValues");
    check(reinterpret_cast<const void *>(getPartCount), "csmGetPartCount");
    check(reinterpret_cast<const void *>(getPartIds), "csmGetPartIds");
    check(reinterpret_cast<const void *>(getPartOpacities), "csmGetPartOpacities");
    check(reinterpret_cast<const void *>(getDrawableCount), "csmGetDrawableCount");
    check(reinterpret_cast<const void *>(getDrawableIds), "csmGetDrawableIds");
    check(reinterpret_cast<const void *>(getDrawableConstantFlags), "csmGetDrawableConstantFlags");
    check(reinterpret_cast<const void *>(getDrawableDynamicFlags), "csmGetDrawableDynamicFlags");
    check(reinterpret_cast<const void *>(getDrawableTextureIndices), "csmGetDrawableTextureIndices");
    check(reinterpret_cast<const void *>(getDrawableOpacities), "csmGetDrawableOpacities");
    check(reinterpret_cast<const void *>(getDrawableMaskCounts), "csmGetDrawableMaskCounts");
    check(reinterpret_cast<const void *>(getDrawableMasks), "csmGetDrawableMasks");
    check(reinterpret_cast<const void *>(getDrawableVertexCounts), "csmGetDrawableVertexCounts");
    check(reinterpret_cast<const void *>(getDrawableVertexPositions), "csmGetDrawableVertexPositions");
    check(reinterpret_cast<const void *>(getDrawableVertexUvs), "csmGetDrawableVertexUvs");
    check(reinterpret_cast<const void *>(getDrawableIndexCounts), "csmGetDrawableIndexCounts");
    check(reinterpret_cast<const void *>(getDrawableIndices), "csmGetDrawableIndices");
    check(reinterpret_cast<const void *>(resetDrawableDynamicFlags), "csmResetDrawableDynamicFlags");
    if (!getDrawableRenderOrders && !getRenderOrders && !getDrawableDrawOrders)
        missing << QStringLiteral("csmGetDrawableRenderOrders");
    return missing;
}

CubismCore &CubismCore::instance()
{
    static CubismCore core;
    return core;
}

QString CubismCore::libraryFileName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Live2DCubismCore.dll");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("libLive2DCubismCore.dylib");
#else
    return QStringLiteral("libLive2DCubismCore.so");
#endif
}

QStringList CubismCore::candidatePaths()
{
    QStringList paths;
    const QString configured = Settings::instance().value(QStringLiteral("live2d/coreLibrary")).toString();
    if (!configured.isEmpty())
        paths << configured;
    const QString env = qEnvironmentVariable("LUMEN_CUBISM_CORE");
    if (!env.isEmpty())
        paths << env;
    if (QCoreApplication::instance())
        paths << QDir(QCoreApplication::applicationDirPath()).filePath(libraryFileName());
    paths << libraryFileName(); // system search path
    return paths;
}

bool CubismCore::load(const QString &path, QString *error)
{
    auto lib = std::make_unique<QLibrary>(path);
    if (!lib->load()) {
        if (error)
            *error = QObject::tr("Cannot load %1: %2").arg(QDir::toNativeSeparators(path), lib->errorString());
        return false;
    }
    auto api = std::make_unique<CoreApi>();
    QLibrary &l = *lib;
    resolve(l, api->getVersion, "csmGetVersion");
    resolve(l, api->getLatestMocVersion, "csmGetLatestMocVersion");
    resolve(l, api->getMocVersion, "csmGetMocVersion");
    resolve(l, api->hasMocConsistency, "csmHasMocConsistency");
    resolve(l, api->reviveMocInPlace, "csmReviveMocInPlace");
    resolve(l, api->getSizeofModel, "csmGetSizeofModel");
    resolve(l, api->initializeModelInPlace, "csmInitializeModelInPlace");
    resolve(l, api->updateModel, "csmUpdateModel");
    resolve(l, api->readCanvasInfo, "csmReadCanvasInfo");
    resolve(l, api->getParameterCount, "csmGetParameterCount");
    resolve(l, api->getParameterIds, "csmGetParameterIds");
    resolve(l, api->getParameterMinimumValues, "csmGetParameterMinimumValues");
    resolve(l, api->getParameterMaximumValues, "csmGetParameterMaximumValues");
    resolve(l, api->getParameterDefaultValues, "csmGetParameterDefaultValues");
    resolve(l, api->getParameterValues, "csmGetParameterValues");
    resolve(l, api->getPartCount, "csmGetPartCount");
    resolve(l, api->getPartIds, "csmGetPartIds");
    resolve(l, api->getPartOpacities, "csmGetPartOpacities");
    resolve(l, api->getDrawableCount, "csmGetDrawableCount");
    resolve(l, api->getDrawableIds, "csmGetDrawableIds");
    resolve(l, api->getDrawableConstantFlags, "csmGetDrawableConstantFlags");
    resolve(l, api->getDrawableDynamicFlags, "csmGetDrawableDynamicFlags");
    resolve(l, api->getDrawableTextureIndices, "csmGetDrawableTextureIndices");
    resolve(l, api->getDrawableDrawOrders, "csmGetDrawableDrawOrders");
    resolve(l, api->getDrawableRenderOrders, "csmGetDrawableRenderOrders");
    resolve(l, api->getRenderOrders, "csmGetRenderOrders");
    resolve(l, api->getDrawableOpacities, "csmGetDrawableOpacities");
    resolve(l, api->getDrawableMaskCounts, "csmGetDrawableMaskCounts");
    resolve(l, api->getDrawableMasks, "csmGetDrawableMasks");
    resolve(l, api->getDrawableVertexCounts, "csmGetDrawableVertexCounts");
    resolve(l, api->getDrawableVertexPositions, "csmGetDrawableVertexPositions");
    resolve(l, api->getDrawableVertexUvs, "csmGetDrawableVertexUvs");
    resolve(l, api->getDrawableIndexCounts, "csmGetDrawableIndexCounts");
    resolve(l, api->getDrawableIndices, "csmGetDrawableIndices");
    resolve(l, api->getDrawableMultiplyColors, "csmGetDrawableMultiplyColors");
    resolve(l, api->getDrawableScreenColors, "csmGetDrawableScreenColors");
    resolve(l, api->resetDrawableDynamicFlags, "csmResetDrawableDynamicFlags");
    const QStringList missing = api->missingRequired();
    if (!missing.isEmpty()) {
        if (error)
            *error = QObject::tr("%1 is not a supported Cubism Core library (missing %2).")
                         .arg(QFileInfo(path).fileName(), missing.join(QStringLiteral(", ")));
        lib->unload();
        return false;
    }
    m_api = std::move(api);
    m_library = std::move(lib);
    m_path = path;
    qCInfo(lcApp).noquote() << "Loaded Cubism Core" << versionString() << "from" << path;
    return true;
}

bool CubismCore::loadDefault(QString *error)
{
    if (isLoaded())
        return true;
    QStringList errors;
    for (const QString &path : candidatePaths()) {
        QString e;
        if (load(path, &e))
            return true;
        errors << e;
    }
    if (error)
        *error = QObject::tr("The Live2D Cubism Core library was not found. Download the Cubism SDK for Native, then choose "
                             "%1 from its Core folder.").arg(libraryFileName());
    return false;
}

QString CubismCore::versionString() const
{
    if (!m_api || !m_api->getVersion)
        return {};
    const unsigned v = m_api->getVersion();
    return QStringLiteral("%1.%2.%3").arg((v >> 24) & 0xff).arg((v >> 16) & 0xff).arg(v & 0xffff);
}

void CubismCore::setApiForTesting(const CoreApi &api, const QString &name)
{
    unload();
    m_api = std::make_unique<CoreApi>(api);
    m_path = name;
}

void CubismCore::unload()
{
    m_api.reset();
    if (m_library)
        m_library->unload();
    m_library.reset();
    m_path.clear();
}

} // namespace lumen::live2d
