#include "MockHttpServer.h"

#include "net/HttpClient.h"
#include "net/SseParser.h"

#include <QSignalSpy>
#include <QtTest>

using namespace lumen::net;

class TestNet : public QObject
{
    Q_OBJECT
private slots:
    void sseParsesSplitChunks();
    void sseCrLfAndMultiline();
    void lineSplitter();
    void multipartFormat();
    void serverErrorExtraction();
    void getJson();
    void retriesOn503();
    void noRetryOn400();
    void streamingChunks();
    void abortReportsCancelled();
    void connectionRefusedMessage();
};

void TestNet::sseParsesSplitChunks()
{
    SseParser p;
    QList<SseEvent> events = p.feed("data: {\"a\":");
    QVERIFY(events.isEmpty());
    events = p.feed("1}\n\nevent: done\ndata: [DONE]\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].data, QByteArray("{\"a\":1}"));
    events = p.feed("\n: keepalive\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].event, QStringLiteral("done"));
    QCOMPARE(events[0].data, QByteArray("[DONE]"));
}

void TestNet::sseCrLfAndMultiline()
{
    SseParser p;
    QList<SseEvent> events = p.feed("data: line1\r");
    events += p.feed("\ndata: line2\r\n\r\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events[0].data, QByteArray("line1\nline2"));
    events = p.feed("data: tail");
    QVERIFY(events.isEmpty());
    events = p.finish();
    QCOMPARE(events.size(), 1);
}

void TestNet::lineSplitter()
{
    LineSplitter s;
    auto lines = s.feed("{\"a\":1}\n{\"b\"");
    QCOMPARE(lines.size(), 1);
    lines = s.feed(":2}\n");
    QCOMPARE(lines.value(0), QByteArray("{\"b\":2}"));
}

void TestNet::multipartFormat()
{
    MultipartBuilder mb;
    mb.addField(QStringLiteral("prompt"), QStringLiteral("a cat"));
    mb.addFile(QStringLiteral("image[]"), QStringLiteral("a.png"), "PNGDATA", QStringLiteral("image/png"));
    const QByteArray body = mb.finish();
    const QByteArray ct = mb.contentType();
    const QByteArray boundary = ct.mid(ct.indexOf("boundary=") + 9);
    QVERIFY(body.startsWith("--" + boundary + "\r\n"));
    QVERIFY(body.contains("name=\"image[]\"; filename=\"a.png\"\r\nContent-Type: image/png\r\n\r\nPNGDATA\r\n"));
    QVERIFY(body.endsWith("--" + boundary + "--\r\n"));
}

void TestNet::serverErrorExtraction()
{
    QCOMPARE(extractServerError(R"({"error":{"message":"Invalid key"}})"), QStringLiteral("Invalid key"));
    QCOMPARE(extractServerError(R"({"detail":"Not Found"})"), QStringLiteral("Not Found"));
    QVERIFY(extractServerError(R"({"detail":[{"loc":["body","steps"],"msg":"must be int"}]})").contains(QStringLiteral("body.steps: must be int")));
    QVERIFY(extractServerError(R"({"error":{"message":"Prompt outputs failed validation"},"node_errors":{"3":{"class_type":"KSampler","errors":[{"message":"Value not in list","details":"ckpt"}]}}})")
                .contains(QStringLiteral("node 3 (KSampler): Value not in list ckpt")));
    QVERIFY(extractServerError("<html>bad gateway</html>").isEmpty());
}

void TestNet::getJson()
{
    MockHttpServer server;
    server.route("GET", QStringLiteral("/ping"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("ok"), true}});
    });
    HttpClient client;
    HttpCall *call = client.get(HttpClient::makeRequest(server.baseUrl().resolved(QUrl(QStringLiteral("/ping")))));
    QSignalSpy spy(call, &HttpCall::finished);
    bool ok = false;
    connect(call, &HttpCall::finished, this, [&] { ok = call->ok() && call->json().value(QStringLiteral("ok")).toBool(); });
    QVERIFY(spy.wait(5000));
    QVERIFY(ok);
}

void TestNet::retriesOn503()
{
    MockHttpServer server;
    int hits = 0;
    server.route("POST", QStringLiteral("/gen"), [&](const MockRequest &req) {
        ++hits;
        if (hits < 3) {
            MockResponse r = MockResponse::json({{QStringLiteral("error"), QStringLiteral("busy")}}, 503);
            r.headers.append(qMakePair(QByteArray("Retry-After"), QByteArray("0")));
            return r;
        }
        return MockResponse::json({{QStringLiteral("echo"), req.json().value(QStringLiteral("x"))}});
    });
    HttpClient client;
    CallOptions opts;
    opts.retryBaseDelayMs = 10;
    opts.maxRetries = 3;
    opts.autoDelete = false;
    HttpCall *call = client.postJson(HttpClient::makeRequest(server.baseUrl().resolved(QUrl(QStringLiteral("/gen")))),
                                     {{QStringLiteral("x"), 7}}, opts);
    QTRY_VERIFY_WITH_TIMEOUT(call->isFinished(), 10000);
    QVERIFY(call->ok());
    QCOMPARE(hits, 3);
    QCOMPARE(call->json().value(QStringLiteral("echo")).toInt(), 7);
    delete call;
}

void TestNet::noRetryOn400()
{
    MockHttpServer server;
    int hits = 0;
    server.route("POST", QStringLiteral("/gen"), [&](const MockRequest &) {
        ++hits;
        return MockResponse::json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("bad prompt")}}}}, 400);
    });
    HttpClient client;
    CallOptions opts;
    opts.autoDelete = false;
    HttpCall *call = client.postJson(HttpClient::makeRequest(server.baseUrl().resolved(QUrl(QStringLiteral("/gen")))), {}, opts);
    QTRY_VERIFY_WITH_TIMEOUT(call->isFinished(), 5000);
    QCOMPARE(hits, 1);
    QVERIFY(!call->ok());
    QCOMPARE(call->status(), 400);
    QCOMPARE(call->errorString(), QStringLiteral("HTTP 400: bad prompt"));
    delete call;
}

void TestNet::streamingChunks()
{
    MockHttpServer server;
    server.route("POST", QStringLiteral("/stream"), [](const MockRequest &) {
        MockResponse r;
        r.contentType = "text/event-stream";
        r.chunks = {"data: one\n\n", "data: two\n", "\ndata: [DONE]\n\n"};
        return r;
    });
    HttpClient client;
    CallOptions opts;
    opts.streaming = true;
    opts.autoDelete = false;
    HttpCall *call = client.postJson(HttpClient::makeRequest(server.baseUrl().resolved(QUrl(QStringLiteral("/stream")))), {}, opts);
    SseParser parser;
    QStringList got;
    connect(call, &HttpCall::chunk, this, [&](const QByteArray &data) {
        for (const SseEvent &e : parser.feed(data))
            got << QString::fromUtf8(e.data);
    });
    QTRY_VERIFY_WITH_TIMEOUT(call->isFinished(), 5000);
    QVERIFY(call->ok());
    QCOMPARE(got, QStringList({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("[DONE]")}));
    delete call;
}

void TestNet::abortReportsCancelled()
{
    MockHttpServer server;
    server.route("GET", QStringLiteral("/slow"), [](const MockRequest &) {
        MockResponse r = MockResponse::json({});
        r.delayMs = 3000;
        return r;
    });
    HttpClient client;
    CallOptions opts;
    opts.autoDelete = false;
    HttpCall *call = client.get(HttpClient::makeRequest(server.baseUrl().resolved(QUrl(QStringLiteral("/slow")))), opts);
    QTimer::singleShot(50, call, &HttpCall::abort);
    QTRY_VERIFY_WITH_TIMEOUT(call->isFinished(), 2000);
    QVERIFY(call->isAborted());
    QCOMPARE(call->errorString(), QStringLiteral("Request cancelled"));
    delete call;
}

void TestNet::connectionRefusedMessage()
{
    quint16 port;
    {
        QTcpServer tmp;
        tmp.listen(QHostAddress::LocalHost, 0);
        port = tmp.serverPort();
    }
    HttpClient client;
    CallOptions opts;
    opts.autoDelete = false;
    HttpCall *call = client.get(HttpClient::makeRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/x").arg(port))), opts);
    QTRY_VERIFY_WITH_TIMEOUT(call->isFinished(), 5000);
    QVERIFY(!call->ok());
    QVERIFY2(call->errorString().contains(QStringLiteral("Connection refused")), qPrintable(call->errorString()));
    delete call;
}

QTEST_MAIN(TestNet)
#include "tst_net.moc"
