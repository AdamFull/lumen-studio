#pragma once

#include "runtime/Hardware.h"

#include <QDateTime>
#include <QFuture>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVersionNumber>

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

// One downloadable build of a runtime (e.g. llama.cpp for CUDA 12) for the current platform.
struct BackendSpec
{
    QString id;                 // "cuda-12", "vulkan", "cpu"
    QString label;
    QString vendor;             // required GPU vendor ("nvidia"), empty = any
    QVersionNumber minCuda;     // required driver CUDA version
    bool needsGpu = false;
    QString assetPattern;       // regex; {tag}, {1}, {2} expand to the (escaped) tag and its captures
    QStringList libraryPatterns; // companion archives (CUDA runtime DLLs), shared between versions
};

// A server program Lumen knows how to install and run (data from resources/runtimes/catalog.json).
struct RuntimeSpec
{
    QString id;                 // "llama.cpp", "sd.cpp"
    QString name;
    QString description;
    QString role;               // "llm" | "image"
    QString repo;               // GitHub owner/name — the only source Lumen downloads from
    QUrl homepage;
    QString license;
    QString tagPattern;
    QString executable;         // server executable inside the archive
    QStringList versionArgs;
    int defaultPort = 0;
    QString healthPath;
    QList<BackendSpec> backends; // for the current platform, in order of preference

    const BackendSpec *backend(const QString &id) const;
};

class Catalog
{
public:
    // Bundled catalogue for the current platform.
    static const Catalog &instance();
    static Catalog fromJson(const QByteArray &json, const QString &platform);

    QList<RuntimeSpec> runtimes;
    const RuntimeSpec *runtime(const QString &id) const;
    // Runtime whose server executable has this file name (for adopting existing installs).
    const RuntimeSpec *runtimeForExecutable(const QString &fileName) const;
};

QString currentPlatform(); // "windows-x64", "linux-x64", ...

// Whether the hardware can run the backend; *why explains a refusal.
bool backendSupported(const BackendSpec &backend, const HardwareInfo &hw, QString *why = nullptr);
// First supported backend in catalogue order (the catalogue lists the fastest first).
QString recommendedBackend(const RuntimeSpec &runtime, const HardwareInfo &hw);

// ---- GitHub releases ----------------------------------------------------------------------------

struct ReleaseAsset
{
    QString name;
    qint64 size = 0;
    QUrl url;
    QString sha256; // from the GitHub API "digest"; empty when GitHub has none
};

struct Release
{
    QString tag;
    QDateTime published;
    bool prerelease = false;
    QUrl page;
    QList<ReleaseAsset> assets;
};

QList<Release> parseReleases(const QByteArray &json, QString *error = nullptr);

struct InstallPlan
{
    QString runtimeId;
    QString backendId;
    QString tag;
    QDateTime published;
    QUrl page;
    ReleaseAsset main;
    QList<ReleaseAsset> libraries;
    QString error;

    bool ok() const { return error.isEmpty(); }
    qint64 downloadSize() const;
};

// Expands the backend's asset pattern for a tag ({tag}, {1}, {2}...). Empty if the tag doesn't match.
QString assetPatternFor(const RuntimeSpec &runtime, const QString &pattern, const QString &tag);

// Newest release whose tag matches the runtime's pattern and that carries the backend's archive.
// Every asset must have a published SHA-256; plans without one are refused.
InstallPlan planInstall(const RuntimeSpec &runtime, const QString &backendId, const QList<Release> &releases);

// Lists a runtime's GitHub releases. Responses are cached with ETags (GitHub allows 60 anonymous
// requests per hour); a cached copy is used when GitHub cannot be reached.
class ReleaseFeed : public QObject
{
    Q_OBJECT
public:
    explicit ReleaseFeed(net::HttpClient *http, QObject *parent = nullptr);

    QFuture<QList<Release>> fetch(const RuntimeSpec &runtime);

    // Tests point this at a mock server.
    void setApiBase(const QUrl &base) { m_apiBase = base; }
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }

private:
    net::HttpClient *m_http;
    QUrl m_apiBase{QStringLiteral("https://api.github.com")};
    QString m_cacheDir;
};

} // namespace lumen::runtime
