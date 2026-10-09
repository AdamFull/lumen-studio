#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>

namespace lumen::net {

struct CallOptions
{
    int timeoutMs = 300000;     // inactivity timeout (no bytes transferred)
    int maxRetries = 2;         // retries for transient failures (timeouts, 429, 502-504)
    int retryBaseDelayMs = 800; // exponential back-off base
    bool streaming = false;     // emit chunk() as data arrives; no retry after first chunk
    bool autoDelete = true;     // deleteLater() after finished() was emitted
    bool quiet = false;         // don't log failures (probes, polling)
};

// One logical HTTP request, including its retries. Lives on the GUI thread; fully asynchronous.
class HttpCall : public QObject
{
    Q_OBJECT
public:
    using ReplyFactory = std::function<QNetworkReply *(QNetworkAccessManager &)>;

    HttpCall(QNetworkAccessManager *nam, ReplyFactory factory, CallOptions options, QObject *parent = nullptr);
    ~HttpCall() override;

    void start();
    void abort();

    bool isFinished() const { return m_finished; }
    bool isAborted() const { return m_aborted; }
    bool ok() const;
    int status() const { return m_status; }
    QNetworkReply::NetworkError networkError() const { return m_netError; }
    const QByteArray &body() const { return m_body; }
    QJsonObject json(QString *parseError = nullptr) const;
    QByteArray rawHeader(const QByteArray &name) const { return m_headers.value(name.toLower()); }
    QUrl url() const { return m_url; }
    // Human readable error: server-provided message when available.
    QString errorString() const;

signals:
    void chunk(const QByteArray &data);
    void downloadProgress(qint64 received, qint64 total);
    void uploadProgress(qint64 sent, qint64 total);
    void finished();

private:
    void attempt();
    void onReplyFinished();
    bool shouldRetry() const;
    int retryDelayMs() const;
    void finish();

    QPointer<QNetworkAccessManager> m_nam;
    ReplyFactory m_factory;
    CallOptions m_options;
    QPointer<QNetworkReply> m_reply;
    QTimer m_retryTimer;
    int m_attempts = 0;
    bool m_finished = false;
    bool m_aborted = false;
    bool m_streamed = false;
    int m_status = 0;
    QNetworkReply::NetworkError m_netError = QNetworkReply::NoError;
    QString m_netErrorString;
    QByteArray m_body;
    QHash<QByteArray, QByteArray> m_headers;
    QUrl m_url;
};

// Factory for HTTP calls. One instance per thread (GUI thread in practice).
class HttpClient : public QObject
{
    Q_OBJECT
public:
    explicit HttpClient(QObject *parent = nullptr);

    QNetworkAccessManager *nam() { return &m_nam; }

    static QNetworkRequest makeRequest(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers = {});

    HttpCall *get(const QNetworkRequest &request, const CallOptions &options = {});
    HttpCall *del(const QNetworkRequest &request, const CallOptions &options = {});
    HttpCall *post(const QNetworkRequest &request, const QByteArray &body, const QByteArray &contentType,
                   const CallOptions &options = {});
    HttpCall *postJson(const QNetworkRequest &request, const QJsonObject &body, const CallOptions &options = {});
    HttpCall *postJsonBytes(const QNetworkRequest &request, const QByteArray &body, const CallOptions &options = {});
    HttpCall *custom(HttpCall::ReplyFactory factory, const CallOptions &options = {});

private:
    QNetworkAccessManager m_nam;
};

// Extracts a meaningful error message from typical API error bodies
// (OpenAI/Anthropic/Gemini {"error":{"message"}}, FastAPI {"detail"}, ComfyUI node_errors, plain text).
QString extractServerError(const QByteArray &body);

// Builds multipart/form-data bodies in memory (re-sendable for retries).
class MultipartBuilder
{
public:
    MultipartBuilder();
    void addField(const QString &name, const QString &value);
    void addFile(const QString &name, const QString &fileName, const QByteArray &data, const QString &mimeType);
    QByteArray finish();
    QByteArray contentType() const;

private:
    QByteArray m_boundary;
    QByteArray m_body;
    bool m_finished = false;
};

} // namespace lumen::net
