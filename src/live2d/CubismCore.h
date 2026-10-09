#pragma once

#include <QLibrary>
#include <QString>
#include <QStringList>

#include <memory>

// Runtime binding to the Live2D Cubism Core library (Live2DCubismCore.dll / .so / .dylib).
// The Core is proprietary and cannot be redistributed: users download the Cubism SDK for Native,
// accept its licence and point Lumen to the library. Nothing is linked at build time.
namespace lumen::live2d {

struct CsmVector2
{
    float x, y;
};

struct CsmVector4
{
    float x, y, z, w;
};

// Constant drawable flags.
enum : unsigned char {
    CsmBlendAdditive = 1 << 0,
    CsmBlendMultiplicative = 1 << 1,
    CsmIsDoubleSided = 1 << 2,
    CsmIsInvertedMask = 1 << 3,
};
// Dynamic drawable flags.
enum : unsigned char {
    CsmIsVisible = 1 << 0,
};

constexpr int kMocAlignment = 64;
constexpr int kModelAlignment = 16;

// Function table of the Core C API (csmMoc* / csmModel* are opaque here). Optional entries are
// null when the loaded Core version does not provide them.
struct CoreApi
{
    unsigned (*getVersion)() = nullptr;
    unsigned (*getLatestMocVersion)() = nullptr;                          // optional
    unsigned (*getMocVersion)(const void *address, unsigned size) = nullptr; // optional
    int (*hasMocConsistency)(void *address, unsigned size) = nullptr;      // optional (4.2+)
    void *(*reviveMocInPlace)(void *address, unsigned size) = nullptr;
    unsigned (*getSizeofModel)(const void *moc) = nullptr;
    void *(*initializeModelInPlace)(const void *moc, void *address, unsigned size) = nullptr;
    void (*updateModel)(void *model) = nullptr;
    void (*readCanvasInfo)(const void *model, CsmVector2 *size, CsmVector2 *origin, float *pixelsPerUnit) = nullptr;

    int (*getParameterCount)(const void *model) = nullptr;
    const char **(*getParameterIds)(const void *model) = nullptr;
    const float *(*getParameterMinimumValues)(const void *model) = nullptr;
    const float *(*getParameterMaximumValues)(const void *model) = nullptr;
    const float *(*getParameterDefaultValues)(const void *model) = nullptr;
    float *(*getParameterValues)(void *model) = nullptr;

    int (*getPartCount)(const void *model) = nullptr;
    const char **(*getPartIds)(const void *model) = nullptr;
    float *(*getPartOpacities)(void *model) = nullptr;

    int (*getDrawableCount)(const void *model) = nullptr;
    const char **(*getDrawableIds)(const void *model) = nullptr;
    const unsigned char *(*getDrawableConstantFlags)(const void *model) = nullptr;
    const unsigned char *(*getDrawableDynamicFlags)(const void *model) = nullptr;
    const int *(*getDrawableTextureIndices)(const void *model) = nullptr;
    const int *(*getDrawableDrawOrders)(const void *model) = nullptr;   // optional
    const int *(*getDrawableRenderOrders)(const void *model) = nullptr; // optional (Core < 5.3)
    const int *(*getRenderOrders)(const void *model) = nullptr;         // optional (Core 5.3+)
    const float *(*getDrawableOpacities)(const void *model) = nullptr;
    const int *(*getDrawableMaskCounts)(const void *model) = nullptr;
    const int **(*getDrawableMasks)(const void *model) = nullptr;
    const int *(*getDrawableVertexCounts)(const void *model) = nullptr;
    const CsmVector2 **(*getDrawableVertexPositions)(const void *model) = nullptr;
    const CsmVector2 **(*getDrawableVertexUvs)(const void *model) = nullptr;
    const int *(*getDrawableIndexCounts)(const void *model) = nullptr;
    const unsigned short **(*getDrawableIndices)(const void *model) = nullptr;
    const CsmVector4 *(*getDrawableMultiplyColors)(const void *model) = nullptr; // optional (4.2+)
    const CsmVector4 *(*getDrawableScreenColors)(const void *model) = nullptr;   // optional (4.2+)
    void (*resetDrawableDynamicFlags)(void *model) = nullptr;

    // Names of required functions that are missing.
    QStringList missingRequired() const;
};

class CubismCore
{
public:
    static CubismCore &instance();

    // Loads the library at `path`. Replaces a previously loaded Core.
    bool load(const QString &path, QString *error = nullptr);
    // Tries the configured path (setting "live2d/coreLibrary"), $LUMEN_CUBISM_CORE, the application
    // directory and the system search path.
    bool loadDefault(QString *error = nullptr);
    bool isLoaded() const { return m_api != nullptr; }
    QString path() const { return m_path; }
    QString versionString() const;
    const CoreApi &api() const { return *m_api; }

    // Installs a function table without a library (unit tests).
    void setApiForTesting(const CoreApi &api, const QString &name);
    void unload();

    static QString libraryFileName();
    static QStringList candidatePaths();

private:
    CubismCore() = default;
    std::unique_ptr<QLibrary> m_library;
    std::unique_ptr<CoreApi> m_api;
    QString m_path;
};

} // namespace lumen::live2d
