#include "net/HttpClient.h"

#include "core/Log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <cmath>

namespace lumen::net {

HttpCall::HttpCall(QNetworkAccessManager *nam, ReplyFactory factory, CallOptions options, QObject *parent)
    : QObject(parent)
    , m_nam(nam)
    , m_factory(std::move(factory))
    , m_options(options)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &HttpCall::attempt);
}

HttpCall::~HttpCall()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
}

void HttpCall::start()
{
    if (m_attempts == 0 && !m_finished)
        attempt();
}

void HttpCall::abort()
{
    if (m_finished)
        return;
    m_aborted = true;
    m_retryTimer.stop();
    if (m_reply) {
        // finished() of the reply drives finish().
        m_reply->abort();
    } else {
        m_netError = QNetworkReply::OperationCanceledError;
        m_netErrorString = tr("Request cancelled");
        finish();
    }
}

bool HttpCall::ok() const
{
    return m_finished && !m_aborted && m_netError == QNetworkReply::NoError && m_status >= 200 && m_status < 300;
}

QJsonObject HttpCall::json(QString *parseError) const
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(m_body, &err);
    if (err.error != QJsonParseError::NoError && parseError)
        *parseError = err.errorString();
    return doc.object();
}

QString HttpCall::errorString() const
{
    if (m_aborted)
        return tr("Request cancelled");
    QString server = extractServerError(m_body);
    if (m_status >= 400) {
        QString msg = tr("HTTP %1").arg(m_status);
        if (!server.isEmpty())
            msg += QStringLiteral(": ") + server;
        return msg;
    }
    if (m_netError != QNetworkReply::NoError) {
        switch (m_netError) {
        case QNetworkReply::ConnectionRefusedError:
            return tr("Connection refused by %1 - is the server running?").arg(m_url.authority());
        case QNetworkReply::HostNotFoundError:
            return tr("Host not found: %1").arg(m_url.host());
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
            return tr("The server did not respond in time (%1)").arg(m_url.toDisplayString(QUrl::RemoveQuery));
        case QNetworkReply::SslHandshakeFailedError:
            return tr("Secure connection failed: %1").arg(m_netErrorString);
        default:
            return m_netErrorString.isEmpty() ? tr("Network error %1").arg(int(m_netError)) : m_netErrorString;
        }
    }
    return server;
}

void HttpCall::attempt()
{
    if (m_finished || m_aborted)
        return;
    if (!m_nam) {
        m_netError = QNetworkReply::UnknownNetworkError;
        m_netErrorString = tr("Network manager no longer available");
        finish();
        return;
    }
    ++m_attempts;
    m_body.clear();
    m_headers.clear();
    m_status = 0;
    m_netError = QNetworkReply::NoError;
    m_netErrorString.clear();
    m_streamed = false;

    QNetworkReply *reply = m_factory(*m_nam);
    if (!reply) {
        m_netError = QNetworkReply::UnknownNetworkError;
        m_netErrorString = tr("Could not create request");
        finish();
        return;
    }
    m_reply = reply;
    m_url = reply->url();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const QByteArray data = reply->readAll();
        m_body.append(data);
        if (m_options.streaming && !data.isEmpty()) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 200 && status < 300) {
                m_streamed = true;
                emit chunk(data);
            }
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &HttpCall::downloadProgress);
    connect(reply, &QNetworkReply::uploadProgress, this, &HttpCall::uploadProgress);
    connect(reply, &QNetworkReply::finished, this, &HttpCall::onReplyFinished);
}

void HttpCall::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    m_body.append(reply->readAll());
    m_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_netError = reply->error();
    m_netErrorString = reply->errorString();
    for (const auto &pair : reply->rawHeaderPairs())
        m_headers.insert(pair.first.toLower(), pair.second);
    reply->deleteLater();

    if (m_aborted) {
        finish();
        return;
    }
    if (shouldRetry()) {
        const int delay = retryDelayMs();
        qCInfo(lcNet).noquote() << "Retrying" << m_url.toDisplayString(QUrl::RemoveQuery) << "in" << delay
                                << "ms (attempt" << m_attempts << "status" << m_status << "error" << int(m_netError) << ")";
        m_retryTimer.start(delay);
        return;
    }
    if (!ok())
        qCWarning(lcNet).noquote() << "Request failed:" << m_url.toDisplayString(QUrl::RemoveQuery) << "-" << log::redact(errorString());
    finish();
}

bool HttpCall::shouldRetry() const
{
    if (m_attempts > m_options.maxRetries || m_streamed || m_aborted)
        return false;
    if (m_status == 429 || m_status == 502 || m_status == 503 || m_status == 504)
        return true;
    if (m_status == 0) {
        switch (m_netError) {
        case QNetworkReply::TimeoutError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::UnknownNetworkError:
            return true;
        default:
            return false;
        }
    }
    return false;
}

int HttpCall::retryDelayMs() const
{
    const QByteArray retryAfter = m_headers.value("retry-after");
    bool ok = false;
    const int seconds = retryAfter.trimmed().toInt(&ok);
    if (ok && seconds > 0)
        return qMin(seconds, 60) * 1000;
    const double base = m_options.retryBaseDelayMs * std::pow(2.0, m_attempts - 1);
    const double jitter = QRandomGenerator::global()->bounded(0.25) * base;
    return static_cast<int>(qMin(30000.0, base + jitter));
}

void HttpCall::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    emit finished();
    if (m_options.autoDelete)
        deleteLater();
}

// ---- HttpClient -----------------------------------------------------------------------------

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
{
    m_nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

QNetworkRequest HttpClient::makeRequest(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("LumenStudio/0.1"));
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    return req;
}

namespace {
QNetworkRequest withTimeout(QNetworkRequest req, const CallOptions &o)
{
    req.setTransferTimeout(o.timeoutMs);
    return req;
}
} // namespace

HttpCall *HttpClient::get(const QNetworkRequest &request, const CallOptions &options)
{
    const QNetworkRequest req = withTimeout(request, options);
    auto *call = new HttpCall(&m_nam, [req](QNetworkAccessManager &nam) { return nam.get(req); }, options, this);
    call->start();
    return call;
}

HttpCall *HttpClient::del(const QNetworkRequest &request, const CallOptions &options)
{
    const QNetworkRequest req = withTimeout(request, options);
    auto *call = new HttpCall(&m_nam, [req](QNetworkAccessManager &nam) { return nam.deleteResource(req); }, options, this);
    call->start();
    return call;
}

HttpCall *HttpClient::post(const QNetworkRequest &request, const QByteArray &body, const QByteArray &contentType,
                           const CallOptions &options)
{
    QNetworkRequest req = withTimeout(request, options);
    req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    auto *call = new HttpCall(&m_nam, [req, body](QNetworkAccessManager &nam) { return nam.post(req, body); }, options, this);
    call->start();
    return call;
}

HttpCall *HttpClient::postJson(const QNetworkRequest &request, const QJsonObject &body, const CallOptions &options)
{
    return postJsonBytes(request, QJsonDocument(body).toJson(QJsonDocument::Compact), options);
}

HttpCall *HttpClient::postJsonBytes(const QNetworkRequest &request, const QByteArray &body, const CallOptions &options)
{
    return post(request, body, QByteArrayLiteral("application/json"), options);
}

HttpCall *HttpClient::custom(HttpCall::ReplyFactory factory, const CallOptions &options)
{
    auto *call = new HttpCall(&m_nam, std::move(factory), options, this);
    call->start();
    return call;
}

// ---- Error extraction -------------------------------------------------------------------------

QString extractServerError(const QByteArray &body)
{
    if (body.isEmpty())
        return {};
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        QString text = QString::fromUtf8(body.left(400)).trimmed();
        if (text.startsWith(u'<'))
            return {}; // HTML error pages are not useful
        return text;
    }
    const QJsonObject obj = doc.object();
    const QJsonValue error = obj.value(QStringLiteral("error"));
    QStringList parts;
    if (error.isObject()) {
        const QJsonObject e = error.toObject();
        const QString msg = e.value(QStringLiteral("message")).toString();
        if (!msg.isEmpty())
            parts << msg;
        const QString details = e.value(QStringLiteral("details")).toString();
        if (!details.isEmpty())
            parts << details;
    } else if (error.isString()) {
        parts << error.toString();
    }
    const QJsonValue detail = obj.value(QStringLiteral("detail"));
    if (detail.isString()) {
        parts << detail.toString();
    } else if (detail.isArray()) {
        for (const QJsonValue &d : detail.toArray()) {
            const QJsonObject o = d.toObject();
            QStringList loc;
            for (const QJsonValue &l : o.value(QStringLiteral("loc")).toArray())
                loc << (l.isString() ? l.toString() : QString::number(l.toInt()));
            parts << loc.join(u'.') + QStringLiteral(": ") + o.value(QStringLiteral("msg")).toString();
        }
    }
    for (const char *key : {"errors", "message", "msg", "name"}) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (v.isString() && !v.toString().isEmpty() && !parts.contains(v.toString()))
            parts << v.toString();
    }
    // ComfyUI: {"node_errors": {"12": {"errors": [{"message":..., "details":...}], "class_type": "..."}}}
    const QJsonObject nodeErrors = obj.value(QStringLiteral("node_errors")).toObject();
    for (auto it = nodeErrors.begin(); it != nodeErrors.end(); ++it) {
        const QJsonObject node = it.value().toObject();
        for (const QJsonValue &e : node.value(QStringLiteral("errors")).toArray()) {
            const QJsonObject eo = e.toObject();
            parts << QStringLiteral("node %1 (%2): %3 %4")
                         .arg(it.key(), node.value(QStringLiteral("class_type")).toString(),
                              eo.value(QStringLiteral("message")).toString(), eo.value(QStringLiteral("details")).toString())
                         .trimmed();
        }
    }
    return parts.join(QStringLiteral(" - "));
}

// ---- Multipart ------------------------------------------------------------------------------

MultipartBuilder::MultipartBuilder()
{
    m_boundary = "----LumenBoundary" + QByteArray::number(QRandomGenerator::global()->generate64(), 16);
}

void MultipartBuilder::addField(const QString &name, const QString &value)
{
    m_body += "--" + m_boundary + "\r\n";
    m_body += "Content-Disposition: form-data; name=\"" + name.toUtf8() + "\"\r\n\r\n";
    m_body += value.toUtf8() + "\r\n";
}

void MultipartBuilder::addFile(const QString &name, const QString &fileName, const QByteArray &data, const QString &mimeType)
{
    m_body += "--" + m_boundary + "\r\n";
    m_body += "Content-Disposition: form-data; name=\"" + name.toUtf8() + "\"; filename=\"" + fileName.toUtf8() + "\"\r\n";
    m_body += "Content-Type: " + mimeType.toUtf8() + "\r\n\r\n";
    m_body += data;
    m_body += "\r\n";
}

QByteArray MultipartBuilder::finish()
{
    if (!m_finished) {
        m_body += "--" + m_boundary + "--\r\n";
        m_finished = true;
    }
    return m_body;
}

QByteArray MultipartBuilder::contentType() const
{
    return "multipart/form-data; boundary=" + m_boundary;
}

} // namespace lumen::net
