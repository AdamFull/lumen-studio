#include "runtime/RunPod.h"

#include "core/Error.h"
#include "core/SecretStore.h"
#include "net/HttpClient.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPromise>

namespace lumen::runtime {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::RunPod)
};

// RFC 9457 problem object: "detail" (else "title"), plus validation messages in "errors".
QString problem(const QByteArray &body)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    QStringList parts;
    const QString detail = o.value(QStringLiteral("detail")).toString();
    parts << (detail.isEmpty() ? o.value(QStringLiteral("title")).toString() : detail);
    for (const QJsonValue &e : o.value(QStringLiteral("errors")).toArray()) {
        if (e.isString())
            parts << e.toString();
    }
    parts.removeAll(QString());
    return parts.join(QStringLiteral("; "));
}

} // namespace

QString RunPodPod::proxyUrl(int port) const
{
    return QStringLiteral("https://%1-%2.proxy.runpod.net").arg(id).arg(port);
}

RunPodPod RunPodPod::fromJson(const QJsonObject &o)
{
    RunPodPod p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.status = o.value(QStringLiteral("status")).toString().toUpper();
    p.costPerHour = o.value(QStringLiteral("cost")).toDouble();
    p.raw = o;
    return p;
}

RunPodClient::RunPodClient(net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

QString RunPodClient::apiKeyName()
{
    return QStringLiteral("lumen/runpod-api-key");
}

QString RunPodClient::key() const
{
    return keyProvider ? keyProvider() : SecretStore::read(apiKeyName());
}

QJsonObject RunPodClient::podRequest(const RunPodPodSpec &spec)
{
    QJsonObject gpu{{QStringLiteral("id"), spec.gpuId}, {QStringLiteral("count"), spec.gpuCount}};
    if (!spec.minCudaVersion.isEmpty())
        gpu.insert(QStringLiteral("minCudaVersion"), spec.minCudaVersion);
    QJsonObject env;
    for (auto it = spec.env.begin(); it != spec.env.end(); ++it)
        env.insert(it.key(), it.value());
    QJsonObject o{{QStringLiteral("name"), spec.name},
                  {QStringLiteral("image"), spec.image},
                  {QStringLiteral("gpu"), gpu},
                  {QStringLiteral("cloud"), spec.community ? QStringLiteral("COMMUNITY") : QStringLiteral("SECURE")},
                  {QStringLiteral("disk"), spec.diskGb},
                  {QStringLiteral("env"), env},
                  {QStringLiteral("ports"), QJsonArray::fromStringList(spec.ports)}};
    if (!spec.cmd.isEmpty())
        o.insert(QStringLiteral("cmd"), QJsonArray::fromStringList(spec.cmd));
    return o;
}

QFuture<QJsonDocument> RunPodClient::call(const QByteArray &method, const QString &path, const QJsonObject &body, bool notFoundOk)
{
    auto promise = std::make_shared<QPromise<QJsonDocument>>();
    promise->start();
    QFuture<QJsonDocument> future = promise->future();
    const QString apiKey = key();
    if (apiKey.isEmpty()) {
        promise->setException(std::make_exception_ptr(Error(Text::tr("No RunPod API key: add one under Settings → Cloud training."))));
        promise->finish();
        return future;
    }
    const QNetworkRequest req = net::HttpClient::makeRequest(QUrl(m_base + path), {{"Authorization", "Bearer " + apiKey.toUtf8()},
                                                                                    {"Accept", "application/json"}});
    net::CallOptions o;
    o.timeoutMs = 60000;
    o.quiet = true;
    // Creating and deleting pods is not repeated blindly (a retry could start a second, paid pod).
    o.maxRetries = method == "GET" ? 2 : 0;
    net::HttpCall *c = method == "GET"    ? m_http->get(req, o)
                     : method == "DELETE" ? m_http->del(req, o)
                                          : m_http->postJson(req, body, o);
    connect(c, &net::HttpCall::finished, this, [c, promise, notFoundOk] {
        if (notFoundOk && c->status() == 404) {
            promise->addResult(QJsonDocument());
        } else if (!c->ok()) {
            QString message = problem(c->body());
            if (c->status() == 401 || c->status() == 403)
                message = Text::tr("RunPod refused the API key (%1). Check it under Settings → Cloud training.").arg(c->status());
            else if (message.isEmpty())
                message = c->errorString();
            else
                message = Text::tr("RunPod: %1").arg(message);
            promise->setException(std::make_exception_ptr(Error(message)));
        } else {
            promise->addResult(c->body().isEmpty() ? QJsonDocument() : QJsonDocument::fromJson(c->body()));
        }
        promise->finish();
    });
    return future;
}

QFuture<QList<RunPodGpu>> RunPodClient::gpus()
{
    return call("GET", QStringLiteral("/catalog/gpus")).then(this, [](const QJsonDocument &d) {
        // {"gpus": [...]} (v2 wraps lists by resource name); a bare array is accepted too.
        const QJsonArray items = d.isArray() ? d.array() : d.object().value(QStringLiteral("gpus")).toArray();
        QList<RunPodGpu> out;
        for (const QJsonValue &v : items) {
            const QJsonObject o = v.toObject();
            RunPodGpu g;
            g.id = o.value(QStringLiteral("id")).toString();
            g.name = o.value(QStringLiteral("name")).toString(g.id);
            g.memoryGb = o.value(QStringLiteral("memory")).toInt();
            const QJsonObject price = o.value(QStringLiteral("price")).toObject();
            g.securePrice = price.value(QStringLiteral("secure")).toDouble();
            g.communityPrice = price.value(QStringLiteral("community")).toDouble();
            const QJsonObject max = o.value(QStringLiteral("maxCount")).toObject();
            g.maxSecure = max.value(QStringLiteral("secure")).toInt(g.securePrice > 0 ? 1 : 0);
            g.maxCommunity = max.value(QStringLiteral("community")).toInt(g.communityPrice > 0 ? 1 : 0);
            if (!g.id.isEmpty())
                out << g;
        }
        std::sort(out.begin(), out.end(), [](const RunPodGpu &a, const RunPodGpu &b) {
            return a.securePrice != b.securePrice ? a.securePrice < b.securePrice : a.name < b.name;
        });
        return out;
    });
}

QFuture<RunPodPod> RunPodClient::createPod(const RunPodPodSpec &spec)
{
    return call("POST", QStringLiteral("/pods"), podRequest(spec)).then(this, [](const QJsonDocument &d) {
        const RunPodPod p = RunPodPod::fromJson(d.object());
        if (p.id.isEmpty())
            throw Error(Text::tr("RunPod did not return a pod."));
        return p;
    });
}

QFuture<RunPodPod> RunPodClient::pod(const QString &id)
{
    return call("GET", QStringLiteral("/pods/") + id).then(this, [](const QJsonDocument &d) { return RunPodPod::fromJson(d.object()); });
}

QFuture<QList<RunPodPod>> RunPodClient::pods(const QString &namePrefix)
{
    return call("GET", QStringLiteral("/pods")).then(this, [namePrefix](const QJsonDocument &d) {
        const QJsonArray items = d.isArray() ? d.array() : d.object().value(QStringLiteral("pods")).toArray();
        QList<RunPodPod> out;
        for (const QJsonValue &v : items) {
            const RunPodPod p = RunPodPod::fromJson(v.toObject());
            if (namePrefix.isEmpty() || p.name.startsWith(namePrefix))
                out << p;
        }
        return out;
    });
}

QFuture<void> RunPodClient::terminate(const QString &id)
{
    return call("DELETE", QStringLiteral("/pods/") + id, {}, true).then(this, [](const QJsonDocument &) {});
}

} // namespace lumen::runtime
