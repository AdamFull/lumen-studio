#pragma once

#include "core/Async.h"

#include <QFuture>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>

#include <functional>

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

// What the pod agent reports (resources/runtimes/cloud/pod_agent.py, GET /status).
struct PodAgentStatus
{
    QString setupState;        // waiting, running, ready, failed
    QString setupMessage;
    QJsonObject fetch;         // name → {state, received, total, error}
    QString trainingState;     // idle, running, done, failed, cancelled
    int exitCode = 0;
    QMap<QString, qint64> files;
    qint64 logSize = 0;
    int uptime = 0;

    static PodAgentStatus fromJson(const QJsonObject &o);
};

// One output file of a training on the pod.
struct PodOutput
{
    QString path;              // "output/<name>.safetensors"
    qint64 size = 0;
    QString sha256;
};

// Talks to the agent in a cloud pod (M15). Every request is short (RunPod's proxy closes
// connections after 100 s): files move in chunks; an upload resumes from the size the agent has.
// Failures are lumen::Error. GUI-thread object; file reads and writes run on worker threads.
class PodAgentClient : public QObject
{
    Q_OBJECT
public:
    PodAgentClient(net::HttpClient *http, const QString &baseUrl, const QString &token, QObject *parent = nullptr);

    // The agent script shipped with Lumen.
    static QByteArray agentScript();

    qint64 chunkSize = 8 * 1024 * 1024;
    using Progress = std::function<void(qint64 done, qint64 total)>;

    QFuture<PodAgentStatus> status();
    QFuture<void> upload(const QString &name, const QString &localPath, Progress progress = {}, async::CancelToken cancel = {});
    QFuture<bool> check(const QString &name, const QString &sha256);
    QFuture<void> fetch(const QString &name, const QString &url, const QString &sha256);
    QFuture<void> setup(const QJsonObject &recipe);
    QFuture<void> unzip(const QString &name, const QString &to);
    // Runs [python] + args on the pod; "{work}" in an argument stands for the pod's work folder.
    QFuture<void> train(const QStringList &args);
    QFuture<QByteArray> log(qint64 offset);
    QFuture<QList<PodOutput>> outputs();
    // Downloads a file in chunks and checks its SHA-256.
    QFuture<void> download(const PodOutput &file, const QString &localPath, Progress progress = {}, async::CancelToken cancel = {});
    QFuture<void> cancelTraining();
    QFuture<void> shutdown();

private:
    struct Reply
    {
        int status = 0;
        QByteArray body;
    };
    // `okStatuses` besides 2xx that resolve instead of failing (e.g. 409 for a resumed upload).
    QFuture<Reply> send(const QByteArray &method, const QString &path, const QByteArray &body = {}, const QByteArray &contentType = {},
                        QList<int> okStatuses = {});
    QFuture<QJsonObject> postJson(const QString &path, const QJsonObject &body);

    net::HttpClient *m_http;
    QString m_base;
    QByteArray m_auth;
};

} // namespace lumen::runtime
