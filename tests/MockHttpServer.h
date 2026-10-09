#pragma once

// Minimal in-process HTTP/1.1 server for contract tests. Each route handler receives the parsed
// request and returns a response. Streaming responses are sent as a sequence of chunks with delays.

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

struct MockRequest
{
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

struct MockResponse
{
    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    QList<QByteArray> chunks; // when non-empty, the body is streamed chunk by chunk
    int chunkDelayMs = 5;
    int delayMs = 0;

    static MockResponse json(const QJsonObject &obj, int status = 200)
    {
        MockResponse r;
        r.status = status;
        r.body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        return r;
    }
};

class MockHttpServer : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<MockResponse(const MockRequest &)>;

    explicit MockHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection())
                handleSocket(s);
        });
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl baseUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }
    quint16 port() const { return m_server.serverPort(); }

    void route(const QByteArray &method, const QString &path, Handler handler)
    {
        m_routes.insert(method + ' ' + path.toUtf8(), std::move(handler));
    }

    QList<MockRequest> requests;

private:
    struct Conn
    {
        QByteArray buffer;
        bool done = false;
    };

    void handleSocket(QTcpSocket *socket)
    {
        auto *state = new Conn;
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QObject::destroyed, this, [state] { delete state; });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, state] {
            if (state->done)
                return;
            state->buffer += socket->readAll();
            const int headerEnd = state->buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            MockRequest req;
            const QList<QByteArray> lines = state->buffer.left(headerEnd).split('\n');
            const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
            req.method = first.value(0);
            const QUrl url(QString::fromUtf8(first.value(1)));
            req.path = url.path();
            req.query = QUrlQuery(url.query());
            for (int i = 1; i < lines.size(); ++i) {
                const int colon = lines[i].indexOf(':');
                if (colon > 0)
                    req.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
            }
            const int length = req.headers.value("content-length").toInt();
            if (state->buffer.size() - headerEnd - 4 < length)
                return;
            req.body = state->buffer.mid(headerEnd + 4, length);
            state->done = true;
            requests.append(req);
            respond(socket, req);
        });
    }

    void respond(QTcpSocket *socket, const MockRequest &req)
    {
        MockResponse resp;
        auto it = m_routes.find(req.method + ' ' + req.path.toUtf8());
        if (it == m_routes.end()) {
            resp.status = 404;
            resp.body = R"({"detail":"Not Found"})";
        } else {
            resp = it.value()(req);
        }
        QPointer<QTcpSocket> guard(socket);
        auto send = [guard, resp] {
            if (!guard)
                return;
            QByteArray head = "HTTP/1.1 " + QByteArray::number(resp.status) + " X\r\n";
            head += "Content-Type: " + resp.contentType + "\r\n";
            for (const auto &h : resp.headers)
                head += h.first + ": " + h.second + "\r\n";
            head += "Connection: close\r\n";
            if (resp.chunks.isEmpty()) {
                head += "Content-Length: " + QByteArray::number(resp.body.size()) + "\r\n\r\n";
                guard->write(head + resp.body);
                guard->flush();
                guard->disconnectFromHost();
                return;
            }
            head += "Transfer-Encoding: chunked\r\n\r\n";
            guard->write(head);
            auto index = std::make_shared<int>(0);
            auto *timer = new QTimer(guard);
            timer->setInterval(resp.chunkDelayMs);
            QObject::connect(timer, &QTimer::timeout, guard, [guard, resp, index, timer] {
                if (*index >= resp.chunks.size()) {
                    timer->stop();
                    guard->write("0\r\n\r\n");
                    guard->flush();
                    guard->disconnectFromHost();
                    return;
                }
                const QByteArray &c = resp.chunks[*index];
                guard->write(QByteArray::number(c.size(), 16) + "\r\n" + c + "\r\n");
                guard->flush();
                ++*index;
            });
            timer->start();
        };
        if (resp.delayMs > 0)
            QTimer::singleShot(resp.delayMs, this, send);
        else
            send();
    }

    QTcpServer m_server;
    QHash<QByteArray, Handler> m_routes;
};
