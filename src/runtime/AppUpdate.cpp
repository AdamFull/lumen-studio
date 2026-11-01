#include "runtime/AppUpdate.h"

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
#include <QVersionNumber>

#include <memory>

namespace lumen::runtime {

namespace {
struct Text
{
    Q_DECLARE_TR_FUNCTIONS(AppUpdate)
};

const QString kRepository = QStringLiteral("AdamFull/lumen-studio");

QVersionNumber versionOf(const QString &text)
{
    int end = 0;
    const QVersionNumber number = QVersionNumber::fromString(text, &end);
    return end == text.size() ? number : QVersionNumber();
}
} // namespace

bool isNewerVersion(const QString &candidate, const QString &current)
{
    const QVersionNumber a = versionOf(candidate);
    return !a.isNull() && a > versionOf(current);
}

AppRelease parseAppRelease(const QByteArray &json, bool requireHttps)
{
    AppRelease out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        out.error = Text::tr("Unexpected answer from GitHub.");
        return out;
    }
    const QJsonObject o = doc.object();
    QString tag = o.value(QStringLiteral("tag_name")).toString();
    if (tag.startsWith(u'v'))
        tag.remove(0, 1);
    if (versionOf(tag).isNull()) {
        out.error = Text::tr("Unexpected answer from GitHub.");
        return out;
    }
    out.version = tag;
    out.notes = o.value(QStringLiteral("body")).toString();
    const QUrl page(o.value(QStringLiteral("html_url")).toString());
    if (page.scheme() == QLatin1String("https") || !requireHttps)
        out.page = page;

    const QString wanted = QStringLiteral("LumenStudio-%1-win64.exe").arg(tag);
    for (const QJsonValue &v : o.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("name")).toString() != wanted)
            continue;
        const QUrl url(a.value(QStringLiteral("browser_download_url")).toString());
        const QString digest = a.value(QStringLiteral("digest")).toString();
        if (!url.isValid() || !digest.startsWith(QLatin1String("sha256:")))
            break;
        if (requireHttps && url.scheme() != QLatin1String("https"))
            break;
        out.installer = url;
        out.installerSize = qint64(a.value(QStringLiteral("size")).toDouble(-1));
        out.installerSha256 = digest.mid(7).toLower();
        break;
    }
    return out;
}

bool isInstalledCopy()
{
    return QFile::exists(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Uninstall.exe")));
}

QString appUpdateFolder()
{
    return QDir(paths::cache()).filePath(QStringLiteral("updates"));
}

void removeOldInstallers()
{
    QDir folder(appUpdateFolder());
    if (folder.exists())
        folder.removeRecursively();
}

AppUpdateCheck::AppUpdateCheck(net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

QFuture<AppRelease> AppUpdateCheck::latest()
{
    auto promise = std::make_shared<QPromise<AppRelease>>();
    promise->start();
    QFuture<AppRelease> future = promise->future();

    QUrl url = m_apiBase;
    url.setPath(QStringLiteral("/repos/%1/releases/latest").arg(kRepository));
    const QList<QPair<QByteArray, QByteArray>> headers = {{"Accept", "application/vnd.github+json"},
                                                          {"X-GitHub-Api-Version", "2022-11-28"},
                                                          {"User-Agent", "LumenStudio"}};
    net::CallOptions opts;
    opts.timeoutMs = 30000;
    opts.maxRetries = 1;
    opts.quiet = true;
    net::HttpCall *call = m_http->get(net::HttpClient::makeRequest(url, headers), opts);
    const bool requireHttps = m_apiBase.scheme() == QLatin1String("https");
    connect(call, &net::HttpCall::finished, this, [call, promise, requireHttps] {
        AppRelease release;
        if (call->ok()) {
            release = parseAppRelease(call->body(), requireHttps);
        } else if (call->status() == 404) {
            // Nothing is published yet.
        } else if (call->status() == 403 || call->status() == 429) {
            release.error = Text::tr("GitHub's hourly request limit is reached. Try again later.");
        } else {
            release.error = Text::tr("Cannot reach GitHub: %1").arg(call->errorString());
        }
        if (!release.ok())
            qCInfo(lcNet) << "Update check:" << release.error;
        promise->addResult(release);
        promise->finish();
    });
    return future;
}

} // namespace lumen::runtime
