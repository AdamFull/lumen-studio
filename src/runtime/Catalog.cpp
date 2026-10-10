#include "runtime/Catalog.h"

#include "core/Error.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "net/HttpClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QRegularExpression>
#include <QSysInfo>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::Catalog)
};

QStringList toStringList(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &x : v.toArray())
        out << x.toString();
    return out;
}

} // namespace

const BackendSpec *RuntimeSpec::backend(const QString &backendId) const
{
    for (const BackendSpec &b : backends) {
        if (b.id == backendId)
            return &b;
    }
    return nullptr;
}

QString currentPlatform()
{
    QString os;
#if defined(Q_OS_WIN)
    os = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    os = QStringLiteral("macos");
#else
    os = QStringLiteral("linux");
#endif
    const QString arch = QSysInfo::currentCpuArchitecture();
    return os + QLatin1Char('-') + (arch == QLatin1String("arm64") ? QStringLiteral("arm64") : QStringLiteral("x64"));
}

Catalog Catalog::fromJson(const QByteArray &json, const QString &platform)
{
    Catalog c;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    for (const QJsonValue &v : root.value(QStringLiteral("runtimes")).toArray()) {
        const QJsonObject o = v.toObject();
        RuntimeSpec r;
        r.id = o.value(QStringLiteral("id")).toString();
        r.name = o.value(QStringLiteral("name")).toString();
        r.description = o.value(QStringLiteral("description")).toString();
        r.role = o.value(QStringLiteral("role")).toString();
        r.repo = o.value(QStringLiteral("repo")).toString();
        r.adoptOnly = o.value(QStringLiteral("adoptOnly")).toBool();
        r.homepage = QUrl(o.value(QStringLiteral("homepage")).toString());
        r.license = o.value(QStringLiteral("license")).toString();
        r.tagPattern = o.value(QStringLiteral("tagPattern")).toString();
        r.executable = o.value(QStringLiteral("executable")).toString();
        r.versionArgs = toStringList(o.value(QStringLiteral("versionArgs")));
        r.defaultPort = o.value(QStringLiteral("defaultPort")).toInt();
        r.healthPath = o.value(QStringLiteral("healthPath")).toString();
        for (const QJsonValue &bv : o.value(QStringLiteral("platforms")).toObject().value(platform).toArray()) {
            const QJsonObject b = bv.toObject();
            BackendSpec spec;
            spec.id = b.value(QStringLiteral("id")).toString();
            spec.label = b.value(QStringLiteral("label")).toString();
            const QJsonObject req = b.value(QStringLiteral("requires")).toObject();
            spec.vendor = req.value(QStringLiteral("vendor")).toString();
            spec.minCuda = QVersionNumber::fromString(req.value(QStringLiteral("cuda")).toString());
            spec.needsGpu = req.value(QStringLiteral("gpu")).toBool() || !spec.vendor.isEmpty();
            spec.assetPattern = b.value(QStringLiteral("asset")).toString();
            spec.libraryPatterns = toStringList(b.value(QStringLiteral("libraries")));
            r.backends << spec;
        }
        if (!r.id.isEmpty() && (!r.repo.isEmpty() || r.adoptOnly))
            c.runtimes << r;
    }
    return c;
}

const Catalog &Catalog::instance()
{
    static const Catalog catalog = [] {
        QFile f(QStringLiteral(":/runtimes/catalog.json"));
        if (!f.open(QIODevice::ReadOnly)) {
            qCWarning(lcApp) << "runtime catalogue missing";
            return Catalog();
        }
        return fromJson(f.readAll(), currentPlatform());
    }();
    return catalog;
}

const RuntimeSpec *Catalog::runtime(const QString &id) const
{
    for (const RuntimeSpec &r : runtimes) {
        if (r.id == id)
            return &r;
    }
    return nullptr;
}

const RuntimeSpec *Catalog::runtimeForExecutable(const QString &fileName) const
{
    for (const RuntimeSpec &r : runtimes) {
        if (r.executable.compare(fileName, Qt::CaseInsensitive) == 0)
            return &r;
        // Linux/macOS builds have no ".exe".
        if (QFileInfo(r.executable).completeBaseName().compare(fileName, Qt::CaseInsensitive) == 0)
            return &r;
    }
    return nullptr;
}

bool backendSupported(const BackendSpec &backend, const HardwareInfo &hw, QString *why)
{
    auto refuse = [why](const QString &reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (backend.vendor == QLatin1String("nvidia")) {
        const bool any = std::any_of(hw.gpus.begin(), hw.gpus.end(), [](const GpuInfo &g) { return g.isNvidia(); });
        if (!any)
            return refuse(Text::tr("needs an NVIDIA GPU"));
        if (!backend.minCuda.isNull()) {
            if (hw.cudaVersion.isNull())
                return refuse(Text::tr("the NVIDIA driver version is unknown"));
            if (hw.cudaVersion < backend.minCuda)
                return refuse(Text::tr("needs a driver with CUDA %1 (installed: %2); update the NVIDIA driver")
                                  .arg(backend.minCuda.toString(), hw.cudaVersion.toString()));
        }
    } else if (!backend.vendor.isEmpty()) {
        const bool any = std::any_of(hw.gpus.begin(), hw.gpus.end(), [&](const GpuInfo &g) {
            return g.vendor().compare(backend.vendor, Qt::CaseInsensitive) == 0;
        });
        if (!any)
            return refuse(Text::tr("needs a %1 GPU").arg(backend.vendor));
    }
    if (backend.needsGpu && hw.gpus.isEmpty())
        return refuse(Text::tr("needs a GPU"));
    return true;
}

QString recommendedBackend(const RuntimeSpec &runtime, const HardwareInfo &hw)
{
    for (const BackendSpec &b : runtime.backends) {
        if (backendSupported(b, hw))
            return b.id;
    }
    return runtime.backends.isEmpty() ? QString() : runtime.backends.last().id;
}

// ---- releases -------------------------------------------------------------------------------------

qint64 InstallPlan::downloadSize() const
{
    qint64 n = main.size;
    for (const ReleaseAsset &a : libraries)
        n += a.size;
    return n;
}

QList<Release> parseReleases(const QByteArray &json, QString *error)
{
    QList<Release> out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isArray()) {
        if (error)
            *error = pe.error != QJsonParseError::NoError ? pe.errorString() : Text::tr("unexpected response");
        return out;
    }
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("draft")).toBool())
            continue;
        Release r;
        r.tag = o.value(QStringLiteral("tag_name")).toString();
        r.published = QDateTime::fromString(o.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
        r.prerelease = o.value(QStringLiteral("prerelease")).toBool();
        r.page = QUrl(o.value(QStringLiteral("html_url")).toString());
        for (const QJsonValue &av : o.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject a = av.toObject();
            ReleaseAsset asset;
            asset.name = a.value(QStringLiteral("name")).toString();
            asset.size = qint64(a.value(QStringLiteral("size")).toDouble());
            asset.url = QUrl(a.value(QStringLiteral("browser_download_url")).toString());
            const QString digest = a.value(QStringLiteral("digest")).toString();
            if (digest.startsWith(QLatin1String("sha256:")))
                asset.sha256 = digest.mid(7).toLower();
            r.assets << asset;
        }
        out << r;
    }
    std::stable_sort(out.begin(), out.end(), [](const Release &a, const Release &b) { return a.published > b.published; });
    return out;
}

QString assetPatternFor(const RuntimeSpec &runtime, const QString &pattern, const QString &tag)
{
    const QRegularExpression tagRe(runtime.tagPattern);
    const QRegularExpressionMatch m = tagRe.match(tag);
    if (!m.hasMatch())
        return {};
    QString out = pattern;
    out.replace(QStringLiteral("{tag}"), QRegularExpression::escape(tag));
    for (int i = 1; i <= m.lastCapturedIndex(); ++i)
        out.replace(QStringLiteral("{%1}").arg(i), QRegularExpression::escape(m.captured(i)));
    return out;
}

InstallPlan planInstall(const RuntimeSpec &runtime, const QString &backendId, const QList<Release> &releases)
{
    InstallPlan plan;
    plan.runtimeId = runtime.id;
    plan.backendId = backendId;
    const BackendSpec *backend = runtime.backend(backendId);
    if (!backend) {
        plan.error = Text::tr("%1 has no '%2' build for this platform.").arg(runtime.name, backendId);
        return plan;
    }
    QString missingDigest;
    for (const Release &r : releases) {
        const QString pattern = assetPatternFor(runtime, backend->assetPattern, r.tag);
        if (pattern.isEmpty())
            continue;
        const QRegularExpression re(pattern);
        const ReleaseAsset *main = nullptr;
        for (const ReleaseAsset &a : r.assets) {
            if (re.match(a.name).hasMatch()) {
                main = &a;
                break;
            }
        }
        if (!main)
            continue; // builds still uploading, or this backend was skipped in that release
        QList<ReleaseAsset> libs;
        bool complete = true;
        for (const QString &libPattern : backend->libraryPatterns) {
            const QRegularExpression lre(assetPatternFor(runtime, libPattern, r.tag));
            const ReleaseAsset *lib = nullptr;
            for (const ReleaseAsset &a : r.assets) {
                if (lre.match(a.name).hasMatch()) {
                    lib = &a;
                    break;
                }
            }
            if (!lib) {
                complete = false;
                break;
            }
            libs << *lib;
        }
        if (!complete)
            continue;
        bool digests = !main->sha256.isEmpty();
        for (const ReleaseAsset &a : std::as_const(libs))
            digests &= !a.sha256.isEmpty();
        if (!digests) {
            if (missingDigest.isEmpty())
                missingDigest = r.tag;
            continue;
        }
        plan.tag = r.tag;
        plan.published = r.published;
        plan.page = r.page;
        plan.main = *main;
        plan.libraries = libs;
        return plan;
    }
    plan.error = missingDigest.isEmpty()
                     ? Text::tr("No release of %1 has a %2 build.").arg(runtime.name, backend->label)
                     : Text::tr("Release %1 of %2 has no published checksum, so it cannot be verified.").arg(missingDigest, runtime.name);
    return plan;
}

// ---- ReleaseFeed ----------------------------------------------------------------------------------

ReleaseFeed::ReleaseFeed(net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

QFuture<QList<Release>> ReleaseFeed::fetch(const RuntimeSpec &runtime)
{
    auto promise = std::make_shared<QPromise<QList<Release>>>();
    promise->start();
    QFuture<QList<Release>> future = promise->future();

    const QString dir = m_cacheDir.isEmpty() ? QDir(paths::cache()).filePath(QStringLiteral("github")) : m_cacheDir;
    QDir().mkpath(dir);
    const QString cacheFile = QDir(dir).filePath(QString(runtime.repo).replace(QLatin1Char('/'), QLatin1Char('_')) + QStringLiteral(".json"));
    QByteArray cachedBody;
    QByteArray etag;
    {
        QFile f(cacheFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            etag = o.value(QStringLiteral("etag")).toString().toUtf8();
            cachedBody = o.value(QStringLiteral("body")).toString().toUtf8();
        }
    }

    QUrl url = m_apiBase;
    url.setPath(QStringLiteral("/repos/%1/releases").arg(runtime.repo));
    url.setQuery(QStringLiteral("per_page=20"));
    QList<QPair<QByteArray, QByteArray>> headers = {{"Accept", "application/vnd.github+json"},
                                                    {"X-GitHub-Api-Version", "2022-11-28"},
                                                    {"User-Agent", "LumenStudio"}};
    if (!etag.isEmpty() && !cachedBody.isEmpty())
        headers.append({"If-None-Match", etag});
    net::CallOptions opts;
    opts.timeoutMs = 30000;
    opts.maxRetries = 1;
    opts.quiet = true;
    net::HttpCall *call = m_http->get(net::HttpClient::makeRequest(url, headers), opts);
    connect(call, &net::HttpCall::finished, this, [call, promise, cacheFile, cachedBody, repo = runtime.repo] {
        QByteArray body;
        if (call->status() == 304) {
            body = cachedBody;
        } else if (call->ok()) {
            body = call->body();
            QFile f(cacheFile);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("etag"), QString::fromUtf8(call->rawHeader("ETag"))},
                                                  {QStringLiteral("body"), QString::fromUtf8(body)}})
                            .toJson(QJsonDocument::Compact));
            }
        } else if (!cachedBody.isEmpty()) {
            qCInfo(lcNet) << "GitHub unreachable, using cached releases of" << repo << call->errorString();
            body = cachedBody;
        } else {
            QString message = Text::tr("Cannot list the releases of %1: %2").arg(repo, call->errorString());
            if (call->status() == 403 || call->status() == 429)
                message = Text::tr("GitHub's hourly request limit is reached. Try again later.");
            promise->setException(std::make_exception_ptr(Error(message)));
            promise->finish();
            return;
        }
        QString error;
        QList<Release> releases = parseReleases(body, &error);
        if (!error.isEmpty()) {
            promise->setException(std::make_exception_ptr(Error(Text::tr("Unexpected answer from GitHub: %1").arg(error))));
        } else {
            promise->addResult(releases);
        }
        promise->finish();
    });
    return future;
}

} // namespace lumen::runtime
