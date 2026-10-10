#include "runtime/PodAgent.h"

#include "core/Error.h"
#include "net/HttpClient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QPromise>
#include <QUrlQuery>

namespace lumen::runtime {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::PodAgent)
};

template<typename T>
void failPromise(const std::shared_ptr<QPromise<T>> &promise, const QString &message)
{
    promise->setException(std::make_exception_ptr(Error(message)));
    promise->finish();
}

} // namespace

PodAgentStatus PodAgentStatus::fromJson(const QJsonObject &o)
{
    PodAgentStatus s;
    const QJsonObject setup = o.value(QStringLiteral("setup")).toObject();
    s.setupState = setup.value(QStringLiteral("state")).toString();
    s.setupMessage = setup.value(QStringLiteral("message")).toString();
    s.fetch = o.value(QStringLiteral("fetch")).toObject();
    const QJsonObject training = o.value(QStringLiteral("training")).toObject();
    s.trainingState = training.value(QStringLiteral("state")).toString();
    s.exitCode = training.value(QStringLiteral("exit_code")).toInt();
    const QJsonObject files = o.value(QStringLiteral("files")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it)
        s.files.insert(it.key(), qint64(it.value().toDouble()));
    s.logSize = qint64(o.value(QStringLiteral("log_size")).toDouble());
    s.uptime = o.value(QStringLiteral("uptime")).toInt();
    return s;
}

PodAgentClient::PodAgentClient(net::HttpClient *http, const QString &baseUrl, const QString &token, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_base(baseUrl.endsWith(QLatin1Char('/')) ? baseUrl.chopped(1) : baseUrl)
    , m_auth("Bearer " + token.toUtf8())
{
}

QByteArray PodAgentClient::agentScript()
{
    QFile f(QStringLiteral(":/runtimes/cloud/pod_agent.py"));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QFuture<PodAgentClient::Reply> PodAgentClient::send(const QByteArray &method, const QString &path, const QByteArray &body,
                                                    const QByteArray &contentType, QList<int> okStatuses)
{
    auto promise = std::make_shared<QPromise<Reply>>();
    promise->start();
    QFuture<Reply> future = promise->future();
    QNetworkRequest req = net::HttpClient::makeRequest(QUrl(m_base + path), {{"Authorization", m_auth}});
    net::CallOptions o;
    o.timeoutMs = 90000; // under the proxy's 100 s
    o.quiet = true;
    // Reads and offset-checked chunks can be repeated; starting things cannot.
    o.maxRetries = method == "POST" ? 0 : 3;
    net::HttpCall *c = nullptr;
    if (method == "GET") {
        c = m_http->get(req, o);
    } else if (method == "PUT") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        c = m_http->custom([req, body](QNetworkAccessManager &nam) { return nam.put(req, body); }, o);
    } else {
        c = m_http->post(req, body, contentType, o);
    }
    connect(c, &net::HttpCall::finished, this, [c, promise, okStatuses] {
        if (c->ok() || okStatuses.contains(c->status())) {
            promise->addResult(Reply{c->status(), c->body()});
            promise->finish();
        } else {
            failPromise(promise, Text::tr("The cloud GPU's agent: %1").arg(c->errorString()));
        }
    });
    return future;
}

QFuture<QJsonObject> PodAgentClient::postJson(const QString &path, const QJsonObject &body)
{
    return send("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json").then(this, [](const Reply &r) {
        return QJsonDocument::fromJson(r.body).object();
    });
}

QFuture<PodAgentStatus> PodAgentClient::status()
{
    return send("GET", QStringLiteral("/status")).then(this, [](const Reply &r) {
        return PodAgentStatus::fromJson(QJsonDocument::fromJson(r.body).object());
    });
}

QFuture<void> PodAgentClient::upload(const QString &name, const QString &localPath, Progress progress, async::CancelToken cancel)
{
    auto promise = std::make_shared<QPromise<void>>();
    promise->start();
    QFuture<void> future = promise->future();
    const qint64 total = QFileInfo(localPath).size();
    const qint64 chunk = chunkSize;
    auto step = std::make_shared<std::function<void(qint64)>>();
    QPointer<PodAgentClient> self(this);
    auto finish = [promise, step](const QString &error) {
        if (error.isEmpty())
            promise->finish();
        else
            failPromise(promise, error);
        *step = nullptr; // breaks the cycle
    };
    *step = [self, name, localPath, total, chunk, progress, cancel, step, finish](qint64 offset) {
        if (!self)
            return finish(Text::tr("Lumen is shutting down."));
        if (cancel.isCancelled())
            return finish(Text::tr("Cancelled."));
        async::run([localPath, offset, chunk] {
            QFile f(localPath);
            if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
                throw Error(Text::tr("Cannot read %1.").arg(QDir::toNativeSeparators(localPath)));
            return f.read(chunk);
        }).then(self, [self, name, offset, total, progress, step, finish](const QByteArray &data) {
              self->send("PUT", QStringLiteral("/files/%1?offset=%2").arg(QString::fromUtf8(QUrl::toPercentEncoding(name))).arg(offset), data,
                         "application/octet-stream", {409})
                  .then(self, [total, progress, step, finish](const Reply &r) {
                      // 409: the agent has a different size (an earlier attempt arrived); go on from there.
                      const qint64 size = qint64(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("size")).toDouble());
                      if (progress)
                          progress(size, total);
                      if (size >= total && r.status != 409)
                          return finish(QString());
                      if (*step)
                          (*step)(size);
                  })
                  .onFailed(self, [finish](const std::exception &e) { finish(QString::fromUtf8(e.what())); });
          }).onFailed(self, [finish](const std::exception &e) { finish(QString::fromUtf8(e.what())); });
    };
    (*step)(0);
    return future;
}

QFuture<bool> PodAgentClient::check(const QString &name, const QString &sha256)
{
    return postJson(QStringLiteral("/check"), {{QStringLiteral("name"), name}, {QStringLiteral("sha256"), sha256}})
        .then(this, [](const QJsonObject &o) { return o.value(QStringLiteral("ok")).toBool(); });
}

QFuture<void> PodAgentClient::fetch(const QString &name, const QString &url, const QString &sha256)
{
    return postJson(QStringLiteral("/fetch"), {{QStringLiteral("name"), name}, {QStringLiteral("url"), url}, {QStringLiteral("sha256"), sha256}})
        .then(this, [](const QJsonObject &) {});
}

QFuture<void> PodAgentClient::setup(const QJsonObject &recipe)
{
    return postJson(QStringLiteral("/setup"), recipe).then(this, [](const QJsonObject &) {});
}

QFuture<void> PodAgentClient::unzip(const QString &name, const QString &to)
{
    return postJson(QStringLiteral("/unzip"), {{QStringLiteral("name"), name}, {QStringLiteral("to"), to}}).then(this, [](const QJsonObject &) {});
}

QFuture<void> PodAgentClient::train(const QStringList &args)
{
    return postJson(QStringLiteral("/train"), {{QStringLiteral("args"), QJsonArray::fromStringList(args)}}).then(this, [](const QJsonObject &) {});
}

QFuture<QByteArray> PodAgentClient::log(qint64 offset)
{
    return send("GET", QStringLiteral("/log?offset=%1").arg(offset)).then(this, [](const Reply &r) { return r.body; });
}

QFuture<QList<PodOutput>> PodAgentClient::outputs()
{
    return send("GET", QStringLiteral("/outputs")).then(this, [](const Reply &r) {
        QList<PodOutput> out;
        for (const QJsonValue &v : QJsonDocument::fromJson(r.body).object().value(QStringLiteral("outputs")).toArray()) {
            const QJsonObject o = v.toObject();
            out << PodOutput{o.value(QStringLiteral("path")).toString(), qint64(o.value(QStringLiteral("size")).toDouble()),
                             o.value(QStringLiteral("sha256")).toString()};
        }
        return out;
    });
}

QFuture<void> PodAgentClient::download(const PodOutput &file, const QString &localPath, Progress progress, async::CancelToken cancel)
{
    auto promise = std::make_shared<QPromise<void>>();
    promise->start();
    QFuture<void> future = promise->future();
    const QString part = localPath + QStringLiteral(".part");
    QFile::remove(part);
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    const qint64 chunk = chunkSize;
    auto step = std::make_shared<std::function<void(qint64)>>();
    QPointer<PodAgentClient> self(this);
    auto finish = [promise, step](const QString &error) {
        if (error.isEmpty())
            promise->finish();
        else
            failPromise(promise, error);
        *step = nullptr;
    };
    *step = [self, file, part, localPath, chunk, progress, cancel, step, finish](qint64 offset) {
        if (!self)
            return finish(Text::tr("Lumen is shutting down."));
        if (cancel.isCancelled())
            return finish(Text::tr("Cancelled."));
        if (offset >= file.size) {
            // Whole: check it and put it in place.
            async::run([part, localPath, sha = file.sha256] {
                QFile f(part);
                if (!f.open(QIODevice::ReadOnly))
                    return Text::tr("Cannot read %1.").arg(QDir::toNativeSeparators(part));
                QCryptographicHash h(QCryptographicHash::Sha256);
                h.addData(&f);
                f.close();
                if (!sha.isEmpty() && QString::fromLatin1(h.result().toHex()).compare(sha, Qt::CaseInsensitive) != 0) {
                    QFile::remove(part);
                    return Text::tr("The downloaded file does not match its checksum.");
                }
                QFile::remove(localPath);
                return QFile::rename(part, localPath) ? QString() : Text::tr("Cannot write %1.").arg(QDir::toNativeSeparators(localPath));
            }).then(self, [finish](const QString &error) { finish(error); });
            return;
        }
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("path"), file.path);
        q.addQueryItem(QStringLiteral("offset"), QString::number(offset));
        q.addQueryItem(QStringLiteral("length"), QString::number(chunk));
        self->send("GET", QStringLiteral("/download?") + q.toString(QUrl::FullyEncoded))
            .then(self, [self, part, offset, file, progress, step, finish](const Reply &r) {
                if (r.body.isEmpty())
                    return finish(Text::tr("The cloud GPU's agent sent an empty part of %1.").arg(file.path));
                async::run([part, data = r.body] {
                    QFile f(part);
                    return f.open(QIODevice::Append) && f.write(data) == data.size();
                }).then(self, [part, offset, size = r.body.size(), file, progress, step, finish](bool ok) {
                    if (!ok)
                        return finish(Text::tr("Cannot write %1.").arg(QDir::toNativeSeparators(part)));
                    if (progress)
                        progress(offset + size, file.size);
                    if (*step)
                        (*step)(offset + size);
                });
            })
            .onFailed(self, [finish](const std::exception &e) { finish(QString::fromUtf8(e.what())); });
    };
    (*step)(0);
    return future;
}

QFuture<void> PodAgentClient::cancelTraining()
{
    return postJson(QStringLiteral("/cancel"), {}).then(this, [](const QJsonObject &) {});
}

QFuture<void> PodAgentClient::shutdown()
{
    return postJson(QStringLiteral("/shutdown"), {}).then(this, [](const QJsonObject &) {});
}

} // namespace lumen::runtime
