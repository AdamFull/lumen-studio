#include "MockHttpServer.h"

#include "ai/Detector.h"
#include "ai/providers/AnthropicProvider.h"
#include "ai/providers/ComfyUiProvider.h"
#include "ai/providers/FalProvider.h"
#include "ai/providers/GeminiProvider.h"
#include "ai/providers/OpenAiImageProvider.h"
#include "ai/providers/StabilityProvider.h"
#include "core/ImageOps.h"
#include "net/HttpClient.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QPainter>
#include <QtTest>

using namespace lumen;
using namespace lumen::ai;

static QByteArray pngBytes(const QColor &c, const QSize &s = QSize(8, 8))
{
    QImage img(s, QImage::Format_ARGB32);
    img.fill(c);
    return img::encode(img);
}

static QImage solid(const QColor &c, const QSize &s = QSize(16, 16))
{
    QImage img(s, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

class TestProvidersCloud : public QObject
{
    Q_OBJECT
private slots:
    void anthropicBodyAndStreaming();
    void anthropicEchoesNativeContent();
    void geminiImageBody();
    void geminiImageGenerate();
    void openaiImageEditsMultipart();
    void openaiPickSize();
    void stabilityInpaint();
    void comfyStandardGraph();
    void comfyEndToEnd();
    void comfyTemplateBindings();
    void falBuildCall();
    void falQueueRoundTrip();
    void falErrorsAndCancel();
    void findAreaEngines();
    void comfyLoras();

private:
    net::HttpClient m_http;
};

void TestProvidersCloud::anthropicBodyAndStreaming()
{
    MockHttpServer server;
    QJsonObject captured;
    QHash<QByteArray, QByteArray> headers;
    server.route("POST", QStringLiteral("/v1/messages"), [&](const MockRequest &req) {
        captured = req.json();
        headers = req.headers;
        MockResponse r;
        r.contentType = "text/event-stream";
        r.chunks = {
            "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10}}}\n\n",
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"plan\"}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig\"}}\n\n",
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Sure\"}}\n\n",
            "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"plan\",\"input\":{}}}\n\n"
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"n\\\":\"}}\n\n",
            "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"3}\"}}\n\n"
            "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":2}\n\n",
            "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":20}}\n\n"
            "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
        };
        return r;
    });
    AnthropicProvider p(AnthropicProvider::describe(), QStringLiteral("a"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}, {QStringLiteral("api_key"), QStringLiteral("sk-test")}});
    ChatRequest req;
    req.messages = {ChatMessage::system(QStringLiteral("You plan.")), ChatMessage::user(QStringLiteral("go"), {solid(Qt::red)})};
    req.tools = {ToolDefinition{QStringLiteral("plan"), QStringLiteral("make a plan"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}};
    req.jsonSchema = QJsonDocument::fromJson(R"({"type":"object","properties":{"x":{"type":"integer","minimum":0}}})").object();
    ChatOperation *op = p.chat(req);
    op->setAutoDelete(false);
    QString streamed;
    connect(op, &ChatOperation::textDelta, this, [&](const QString &d) { streamed += d; });
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    const ChatResult &res = op->result();
    QCOMPARE(res.text, QStringLiteral("Sure"));
    QCOMPARE(streamed, QStringLiteral("Sure"));
    QCOMPARE(res.reasoning, QStringLiteral("plan"));
    QCOMPARE(res.toolCalls.size(), 1);
    QCOMPARE(res.toolCalls[0].arguments.value(QStringLiteral("n")).toInt(), 3);
    QCOMPARE(res.finishReason, QStringLiteral("tool_use"));
    QCOMPARE(res.providerContent.size(), 3);
    QCOMPARE(res.providerContent[0].toObject().value(QStringLiteral("signature")).toString(), QStringLiteral("sig"));
    // Request shape.
    QCOMPARE(headers.value("x-api-key"), QByteArray("sk-test"));
    QCOMPARE(headers.value("anthropic-version"), QByteArray("2023-06-01"));
    QCOMPARE(headers.value("anthropic-beta"), QByteArray("server-side-fallback-2026-07-01"));
    QCOMPARE(captured.value(QStringLiteral("model")).toString(), QStringLiteral("claude-opus-5-5"));
    QCOMPARE(captured.value(QStringLiteral("fallbacks")).toString(), QStringLiteral("default"));
    QCOMPARE(captured.value(QStringLiteral("system")).toString(), QStringLiteral("You plan."));
    QVERIFY(!captured.contains(QStringLiteral("temperature")));
    const QJsonObject oc = captured.value(QStringLiteral("output_config")).toObject();
    QCOMPARE(oc.value(QStringLiteral("effort")).toString(), QStringLiteral("medium"));
    QCOMPARE(oc.value(QStringLiteral("format")).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
    QCOMPARE(oc.value(QStringLiteral("format")).toObject().value(QStringLiteral("schema")).toObject().value(QStringLiteral("additionalProperties")).toBool(true), false);
    const QJsonArray content = captured.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("content")).toArray();
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("image"));
    QCOMPARE(content.at(1).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("go"));
    delete op;
}

void TestProvidersCloud::anthropicEchoesNativeContent()
{
    AnthropicProvider p(AnthropicProvider::describe(), QStringLiteral("a"), &m_http, nullptr);
    ChatResult prev;
    prev.providerType = QStringLiteral("anthropic");
    prev.providerContent = QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("thinking")}, {QStringLiteral("thinking"), QString()}, {QStringLiteral("signature"), QStringLiteral("s")}},
                                      QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")}, {QStringLiteral("id"), QStringLiteral("t1")}, {QStringLiteral("name"), QStringLiteral("x")}, {QStringLiteral("input"), QJsonObject()}}};
    ChatRequest req;
    req.messages = {ChatMessage::user(QStringLiteral("a")), ChatMessage::fromResult(prev), ChatMessage::toolResult(QStringLiteral("t1"), QStringLiteral("x"), QStringLiteral("ok")),
                    ChatMessage::toolResult(QStringLiteral("t2"), QStringLiteral("y"), QStringLiteral("ok2"))};
    const QJsonArray msgs = p.buildBody(req).value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgs.size(), 3);
    QCOMPARE(msgs[1].toObject().value(QStringLiteral("content")).toArray(), prev.providerContent);
    // Both tool results merged into one user turn.
    QCOMPARE(msgs[2].toObject().value(QStringLiteral("content")).toArray().size(), 2);
}

void TestProvidersCloud::geminiImageBody()
{
    ImageRequest r;
    r.prompt = QStringLiteral("hero shot");
    r.size = QSize(832, 1216);
    ReferenceImage ref;
    ref.image = solid(Qt::red);
    r.references = {ref, ref};
    r.controls = {ControlInput{ControlType::OpenPose, solid(Qt::black), 1.0, 0.0, 1.0}};
    const QJsonObject body = GeminiImageProvider::buildBody(r, {QStringLiteral("1:1"), QStringLiteral("2:3"), QStringLiteral("16:9")});
    const QJsonArray parts = body.value(QStringLiteral("contents")).toArray().at(0).toObject().value(QStringLiteral("parts")).toArray();
    QCOMPARE(parts.size(), 4); // text + 2 refs + pose
    const QString text = parts.at(0).toObject().value(QStringLiteral("text")).toString();
    QVERIFY(text.contains(QStringLiteral("Image(s) 1, 2 show the character")));
    QVERIFY(text.contains(QStringLiteral("Image 3 is an OpenPose skeleton")));
    QVERIFY(text.endsWith(QStringLiteral("hero shot")));
    QCOMPARE(body.value(QStringLiteral("generationConfig")).toObject().value(QStringLiteral("imageConfig")).toObject()
                 .value(QStringLiteral("aspectRatio")).toString(), QStringLiteral("2:3"));
}

void TestProvidersCloud::geminiImageGenerate()
{
    MockHttpServer server;
    int calls = 0;
    server.route("POST", QStringLiteral("/v1beta/models/gemini-2.5-flash-image:generateContent"), [&](const MockRequest &req) {
        ++calls;
        if (req.headers.value("x-goog-api-key") != "k")
            return MockResponse::json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("bad key")}}}}, 403);
        const QJsonObject part{{QStringLiteral("inlineData"), QJsonObject{{QStringLiteral("mimeType"), QStringLiteral("image/png")},
                                                                         {QStringLiteral("data"), QString::fromLatin1(pngBytes(Qt::green).toBase64())}}}};
        return MockResponse::json({{QStringLiteral("candidates"), QJsonArray{QJsonObject{
            {QStringLiteral("content"), QJsonObject{{QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("ok")}}, part}}}},
            {QStringLiteral("finishReason"), QStringLiteral("STOP")}}}}});
    });
    GeminiImageProvider p(GeminiImageProvider::describe(), QStringLiteral("g"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1beta")}, {QStringLiteral("api_key"), QStringLiteral("k")}});
    ImageRequest r;
    r.prompt = QStringLiteral("x");
    r.batchCount = 2;
    ImageOperation *op = p.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().images.size(), 2);
    QCOMPARE(calls, 2);
    delete op;
}

void TestProvidersCloud::openaiImageEditsMultipart()
{
    MockHttpServer server;
    QByteArray body;
    QByteArray contentType;
    server.route("POST", QStringLiteral("/v1/images/edits"), [&](const MockRequest &req) {
        body = req.body;
        contentType = req.headers.value("content-type");
        return MockResponse::json({{QStringLiteral("data"), QJsonArray{QJsonObject{{QStringLiteral("b64_json"), QString::fromLatin1(pngBytes(Qt::blue).toBase64())}}}}});
    });
    OpenAiImageProvider p(OpenAiImageProvider::describe(), QStringLiteral("o"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}, {QStringLiteral("api_key"), QStringLiteral("k")}});
    ImageRequest r;
    r.mode = GenMode::Inpaint;
    r.prompt = QStringLiteral("add a hat");
    r.initImage = solid(Qt::white, QSize(64, 64));
    r.mask = img::createMask(QSize(64, 64), 255);
    ReferenceImage ref;
    ref.image = solid(Qt::red);
    r.references = {ref};
    ImageOperation *op = p.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QVERIFY(contentType.startsWith("multipart/form-data"));
    QCOMPARE(body.count("name=\"image[]\""), 2);
    QCOMPARE(body.count("name=\"mask\""), 1);
    QVERIFY(body.contains("Image 1 is the image to edit"));
    QVERIFY(body.contains("input_fidelity"));
    delete op;
}

void TestProvidersCloud::openaiPickSize()
{
    QCOMPARE(OpenAiImageProvider::pickSize(QSize(832, 1216)), QStringLiteral("1024x1536"));
    QCOMPARE(OpenAiImageProvider::pickSize(QSize(1920, 1080)), QStringLiteral("1536x1024"));
    QCOMPARE(OpenAiImageProvider::pickSize(QSize(1000, 1100)), QStringLiteral("1024x1024"));
}

void TestProvidersCloud::stabilityInpaint()
{
    MockHttpServer server;
    QByteArray body;
    server.route("POST", QStringLiteral("/v2beta/stable-image/edit/inpaint"), [&](const MockRequest &req) {
        body = req.body;
        return MockResponse::json({{QStringLiteral("image"), QString::fromLatin1(pngBytes(Qt::yellow).toBase64())},
                                   {QStringLiteral("seed"), 77}, {QStringLiteral("finish_reason"), QStringLiteral("SUCCESS")}});
    });
    StabilityProvider p(StabilityProvider::describe(), QStringLiteral("s"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v2beta")}, {QStringLiteral("api_key"), QStringLiteral("k")}});
    ImageRequest r;
    r.mode = GenMode::Inpaint;
    r.prompt = QStringLiteral("p");
    r.initImage = solid(Qt::white, QSize(32, 32));
    r.mask = img::createMask(QSize(32, 32), 255);
    ImageOperation *op = p.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().images[0].seed, 77);
    QVERIFY(body.contains("name=\"mask\""));
    QVERIFY(body.contains("name=\"output_format\""));
    delete op;
}

void TestProvidersCloud::comfyStandardGraph()
{
    ComfyUiProvider p(ComfyUiProvider::describe(), QStringLiteral("c"), &m_http, nullptr);
    p.configure({{QStringLiteral("model"), QStringLiteral("sdxl.safetensors")}, {QStringLiteral("pose_controlnet"), QStringLiteral("openpose_xl.safetensors")}});
    ImageRequest r;
    r.mode = GenMode::Inpaint;
    r.prompt = QStringLiteral("p");
    r.batchCount = 2;
    ReferenceImage ref;
    r.references = {ref, ref};
    r.controls = {ControlInput{ControlType::OpenPose, QImage(), 0.9, 0.0, 1.0}};
    ComfyUiProvider::UploadedImages up;
    up.init = QStringLiteral("lumen/init.png");
    up.mask = QStringLiteral("lumen/mask.png");
    up.pose = QStringLiteral("lumen/pose.png");
    up.references = {QStringLiteral("lumen/r0.png"), QStringLiteral("lumen/r1.png")};
    const QJsonObject g = p.buildStandardGraph(r, up, true);
    QHash<QString, int> classes;
    for (auto it = g.begin(); it != g.end(); ++it)
        classes[it.value().toObject().value(QStringLiteral("class_type")).toString()]++;
    QCOMPARE(classes.value(QStringLiteral("IPAdapterAdvanced")), 1);
    QCOMPARE(classes.value(QStringLiteral("ImageBatch")), 1);
    QCOMPARE(classes.value(QStringLiteral("ControlNetApplyAdvanced")), 1);
    QCOMPARE(classes.value(QStringLiteral("InpaintModelConditioning")), 1);
    QCOMPARE(classes.value(QStringLiteral("RepeatLatentBatch")), 1);
    QCOMPARE(classes.value(QStringLiteral("LoadImage")), 5);
    QCOMPARE(classes.value(QStringLiteral("PreviewImage")), 1);
    // Without IP-Adapter nodes the references are skipped, the rest still builds.
    const QJsonObject g2 = p.buildStandardGraph(r, up, false);
    QVERIFY(!QJsonDocument(g2).toJson().contains("IPAdapter"));
}

void TestProvidersCloud::comfyEndToEnd()
{
    MockHttpServer server;
    QJsonObject queued;
    int uploads = 0;
    bool done = false;
    server.route("POST", QStringLiteral("/upload/image"), [&](const MockRequest &) {
        ++uploads;
        return MockResponse::json({{QStringLiteral("name"), QStringLiteral("u%1.png").arg(uploads)}, {QStringLiteral("subfolder"), QStringLiteral("lumen")}, {QStringLiteral("type"), QStringLiteral("input")}});
    });
    server.route("GET", QStringLiteral("/object_info/ControlNetLoader"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("ControlNetLoader"), QJsonObject{{QStringLiteral("input"), QJsonObject{{QStringLiteral("required"), QJsonObject{
            {QStringLiteral("control_net_name"), QJsonArray{QJsonArray{QStringLiteral("canny.safetensors"), QStringLiteral("OpenPoseXL2.safetensors")}}}}}}}}}});
    });
    server.route("POST", QStringLiteral("/prompt"), [&](const MockRequest &req) {
        queued = req.json();
        QTimer::singleShot(300, &server, [&] { done = true; });
        return MockResponse::json({{QStringLiteral("prompt_id"), QStringLiteral("pid")}, {QStringLiteral("number"), 1}});
    });
    server.route("GET", QStringLiteral("/history/pid"), [&](const MockRequest &) {
        if (!done)
            return MockResponse::json({});
        return MockResponse::json({{QStringLiteral("pid"), QJsonObject{
            {QStringLiteral("status"), QJsonObject{{QStringLiteral("status_str"), QStringLiteral("success")}}},
            {QStringLiteral("outputs"), QJsonObject{{QStringLiteral("9"), QJsonObject{{QStringLiteral("images"), QJsonArray{
                QJsonObject{{QStringLiteral("filename"), QStringLiteral("a.png")}, {QStringLiteral("subfolder"), QString()}, {QStringLiteral("type"), QStringLiteral("temp")}},
                QJsonObject{{QStringLiteral("filename"), QStringLiteral("b.png")}, {QStringLiteral("subfolder"), QString()}, {QStringLiteral("type"), QStringLiteral("temp")}}}}}}}}}}});
    });
    server.route("GET", QStringLiteral("/view"), [](const MockRequest &req) {
        MockResponse r;
        r.contentType = "image/png";
        r.body = pngBytes(req.query.queryItemValue(QStringLiteral("filename")) == QLatin1String("a.png") ? Qt::red : Qt::blue);
        return r;
    });
    ComfyUiProvider p(ComfyUiProvider::describe(), QStringLiteral("c"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), server.baseUrl().toString()}, {QStringLiteral("model"), QStringLiteral("animagineXL.safetensors")},
                 {QStringLiteral("ipadapter_preset"), QStringLiteral("none")}, {QStringLiteral("free_vram"), true}});
    QJsonObject freed;
    server.route("POST", QStringLiteral("/free"), [&](const MockRequest &req) {
        freed = req.json();
        return MockResponse::json({});
    });
    ImageRequest r;
    r.prompt = QStringLiteral("p");
    r.controls = {ControlInput{ControlType::OpenPose, solid(Qt::black), 1.0, 0.0, 1.0}};
    ImageOperation *op = p.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().images.size(), 2);
    QCOMPARE(op->result().images[0].image.pixelColor(1, 1), QColor(Qt::red));
    QCOMPARE(uploads, 1);
    const QByteArray graph = QJsonDocument(queued.value(QStringLiteral("prompt")).toObject()).toJson();
    QVERIFY(graph.contains("OpenPoseXL2.safetensors")); // auto-picked
    QVERIFY(graph.contains("lumen/u1.png"));
    QVERIFY(!queued.value(QStringLiteral("client_id")).toString().isEmpty());
    delete op;
    // "Free VRAM when idle": the models are unloaded a few seconds after the last job.
    QVERIFY(freed.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(freed.value(QStringLiteral("unload_models")).toBool(), 8000);
}

void TestProvidersCloud::comfyTemplateBindings()
{
    const QJsonObject json = QJsonDocument::fromJson(R"({
        "name": "Qwen edit",
        "modes": ["edit", "txt2img"],
        "workflow": {
            "1": {"class_type": "LoadImage", "inputs": {"image": "x.png"}},
            "2": {"class_type": "LoadImage", "inputs": {"image": "y.png"}},
            "3": {"class_type": "TextEncodeQwenImageEditPlus", "inputs": {"prompt": "", "image1": ["1", 0], "image2": ["2", 0]}},
            "4": {"class_type": "KSampler", "inputs": {"seed": 0, "steps": 4}}
        },
        "bindings": {
            "prompt": {"node": "3", "input": "prompt"},
            "seed": {"node": "4", "input": "seed"},
            "steps": {"node": "4", "input": "steps"},
            "reference_images": [{"node": "1", "input": "image"}, {"node": "2", "input": "image"}]
        }
    })").object();
    QString error;
    auto tpl = ComfyTemplate::fromJson(json, QStringLiteral("qwen"), &error);
    QVERIFY2(tpl.has_value(), qPrintable(error));
    QCOMPARE(tpl->maxReferences, 2);
    ImageRequest r;
    r.prompt = QStringLiteral("make it night");
    r.seed = 42;
    r.steps = 8;
    ComfyUiProvider::UploadedImages up;
    up.references = {QStringLiteral("lumen/a.png")};
    const QJsonObject wf = ComfyUiProvider::applyTemplate(*tpl, r, up);
    QCOMPARE(wf.value(QStringLiteral("3")).toObject().value(QStringLiteral("inputs")).toObject().value(QStringLiteral("prompt")).toString(), QStringLiteral("make it night"));
    QCOMPARE(wf.value(QStringLiteral("4")).toObject().value(QStringLiteral("inputs")).toObject().value(QStringLiteral("seed")).toInt(), 42);
    QCOMPARE(wf.value(QStringLiteral("2")).toObject().value(QStringLiteral("inputs")).toObject().value(QStringLiteral("image")).toString(), QStringLiteral("lumen/a.png"));
    QVERIFY(!ComfyTemplate::fromJson(QJsonObject{{QStringLiteral("workflow"), QJsonObject{{QStringLiteral("1"), QJsonObject{{QStringLiteral("type"), 1}}}}}}, QStringLiteral("x")).has_value());
}

void TestProvidersCloud::falBuildCall()
{
    FalProvider p(FalProvider::describe(), QStringLiteral("f"), &m_http, nullptr);
    p.configure({{QStringLiteral("api_key"), QStringLiteral("k")}});
    ImageRequest r;
    r.prompt = QStringLiteral("a fox");
    r.size = QSize(1024, 768);
    r.seed = 5;
    r.batchCount = 2;
    FalProvider::Call c = p.buildCall(r);
    QCOMPARE(c.model, QStringLiteral("fal-ai/flux/dev"));
    QCOMPARE(c.input.value(QStringLiteral("image_size")).toObject().value(QStringLiteral("height")).toInt(), 768);
    QCOMPARE(c.input.value(QStringLiteral("num_images")).toInt(), 2);
    QCOMPARE(c.input.value(QStringLiteral("seed")).toInt(), 5);
    r.model = QStringLiteral("fal-ai/flux-pro/v1.1-ultra");
    c = p.buildCall(r);
    QCOMPARE(c.input.value(QStringLiteral("aspect_ratio")).toString(), QStringLiteral("4:3"));
    QVERIFY(!c.input.contains(QStringLiteral("image_size")));

    r.mode = GenMode::Inpaint;
    r.initImage = solid(Qt::white, QSize(64, 48));
    r.mask = img::createMask(QSize(64, 48), 255);
    r.extras = {{QStringLiteral("safety_tolerance"), QStringLiteral("5")}};
    c = p.buildCall(r);
    QCOMPARE(c.model, QStringLiteral("fal-ai/flux-pro/v1/fill"));
    QVERIFY(c.input.value(QStringLiteral("image_url")).toString().startsWith(QLatin1String("data:image/png;base64,")));
    const QImage mask = img::fromBase64(c.input.value(QStringLiteral("mask_url")).toString().toLatin1());
    QCOMPARE(mask.size(), QSize(64, 48));
    QCOMPARE(qGray(mask.pixel(10, 10)), 255);
    QCOMPARE(c.input.value(QStringLiteral("safety_tolerance")).toString(), QStringLiteral("5")); // extras last
    r.mode = GenMode::Edit;
    QCOMPARE(p.buildCall(r).model, QStringLiteral("fal-ai/flux-pro/kontext"));

    QCOMPARE(FalProvider::imageUrls(QJsonObject{{QStringLiteral("images"), QJsonArray{QJsonObject{{QStringLiteral("url"), QStringLiteral("a")}},
                                                                                     QJsonObject{{QStringLiteral("url"), QStringLiteral("b")}}}}}),
             (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    QCOMPARE(FalProvider::imageUrls(QJsonObject{{QStringLiteral("image"), QJsonObject{{QStringLiteral("url"), QStringLiteral("c")}}}}),
             QStringList{QStringLiteral("c")});
}

void TestProvidersCloud::falQueueRoundTrip()
{
    MockHttpServer server;
    const QString base = server.baseUrl().toString();
    QJsonObject submitted;
    QByteArray auth;
    int polls = 0;
    // Sub-path models are rooted at the app id; the provider must use the URLs from the submit response.
    server.route("POST", QStringLiteral("/fal-ai/flux/dev"), [&](const MockRequest &req) {
        submitted = req.json();
        auth = req.headers.value("authorization");
        return MockResponse::json({{QStringLiteral("request_id"), QStringLiteral("r1")},
                                   {QStringLiteral("status_url"), base + QStringLiteral("/fal-ai/flux/requests/r1/status")},
                                   {QStringLiteral("response_url"), base + QStringLiteral("/fal-ai/flux/requests/r1")},
                                   {QStringLiteral("cancel_url"), base + QStringLiteral("/fal-ai/flux/requests/r1/cancel")}});
    });
    server.route("GET", QStringLiteral("/fal-ai/flux/requests/r1/status"), [&](const MockRequest &) {
        ++polls;
        if (polls == 1)
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("IN_QUEUE")}, {QStringLiteral("queue_position"), 0}});
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("COMPLETED")}});
    });
    server.route("GET", QStringLiteral("/fal-ai/flux/requests/r1"), [&](const MockRequest &) {
        return MockResponse::json(
            {{QStringLiteral("seed"), 42},
             {QStringLiteral("images"), QJsonArray{QJsonObject{{QStringLiteral("url"), base + QStringLiteral("/files/one.png")}},
                                                   QJsonObject{{QStringLiteral("url"), img::toDataUrl(solid(Qt::green))}}}}});
    });
    server.route("GET", QStringLiteral("/files/one.png"), [&](const MockRequest &) {
        MockResponse r;
        r.contentType = "image/png";
        r.body = pngBytes(Qt::red);
        return r;
    });
    FalProvider p(FalProvider::describe(), QStringLiteral("f"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), base}, {QStringLiteral("api_key"), QStringLiteral("secret")}});
    ImageRequest r;
    r.prompt = QStringLiteral("p");
    r.batchCount = 2;
    ImageOperation *op = p.generate(r);
    op->setAutoDelete(false);
    QStringList statuses;
    connect(op, &Operation::progress, this, [&](double, const QString &s) { statuses << s; });
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(auth, QByteArray("Key secret"));
    QCOMPARE(submitted.value(QStringLiteral("prompt")).toString(), QStringLiteral("p"));
    QCOMPARE(op->result().images.size(), 2);
    QCOMPARE(op->result().images[0].image.pixelColor(2, 2), QColor(Qt::red));
    QCOMPARE(op->result().images[1].image.pixelColor(2, 2), QColor(Qt::green));
    QCOMPARE(op->result().images[1].seed, 43);
    QVERIFY(statuses.contains(QStringLiteral("Queued (position 1)")));
    delete op;
}

void TestProvidersCloud::falErrorsAndCancel()
{
    MockHttpServer server;
    const QString base = server.baseUrl().toString();
    bool cancelled = false;
    // No URLs in the submit response: the provider derives them from the model id.
    server.route("POST", QStringLiteral("/fal-ai/flux-pro/v1/fill"), [&](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("request_id"), QStringLiteral("r2")}});
    });
    server.route("GET", QStringLiteral("/fal-ai/flux-pro/v1/fill/requests/r2/status"), [&](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("COMPLETED")}, {QStringLiteral("error"), QStringLiteral("bad mask")}});
    });
    server.route("POST", QStringLiteral("/fal-ai/flux/dev"), [&](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("request_id"), QStringLiteral("r3")}});
    });
    server.route("GET", QStringLiteral("/fal-ai/flux/dev/requests/r3/status"), [&](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("IN_PROGRESS")}});
    });
    server.route("PUT", QStringLiteral("/fal-ai/flux/dev/requests/r3/cancel"), [&](const MockRequest &) {
        cancelled = true;
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("CANCELLATION_REQUESTED")}}, 202);
    });
    FalProvider p(FalProvider::describe(), QStringLiteral("f"), &m_http, nullptr);
    p.configure({{QStringLiteral("url"), base}, {QStringLiteral("api_key"), QStringLiteral("k")}});

    ImageRequest inpaint;
    inpaint.mode = GenMode::Inpaint;
    inpaint.initImage = solid(Qt::white, QSize(32, 32));
    inpaint.mask = img::createMask(QSize(32, 32), 255);
    ImageOperation *op = p.generate(inpaint);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
    QCOMPARE(op->state(), Operation::State::Failed);
    QVERIFY(op->errorString().contains(QLatin1String("bad mask")));
    delete op;

    ImageRequest t2i;
    t2i.prompt = QStringLiteral("p");
    op = p.generate(t2i);
    op->setAutoDelete(false);
    QStringList statuses;
    connect(op, &Operation::progress, this, [&](double, const QString &s) { statuses << s; });
    QTRY_VERIFY_WITH_TIMEOUT(statuses.contains(QStringLiteral("Generating")), 10000);
    op->cancel();
    QCOMPARE(op->state(), Operation::State::Cancelled);
    QTRY_VERIFY_WITH_TIMEOUT(cancelled, 5000);
    delete op;

    // Connection test: an unknown request id answers 404 when the key is valid, 401 when it is not.
    server.route("GET", QStringLiteral("/fal-ai/flux/dev/requests/00000000-0000-0000-0000-000000000000/status"), [&](const MockRequest &req) {
        const bool good = req.headers.value("authorization") == "Key k";
        return MockResponse::json({{QStringLiteral("detail"), good ? QStringLiteral("Request not found") : QStringLiteral("Unauthorized")}},
                                  good ? 404 : 401);
    });
    QFuture<QString> test = p.testConnection();
    QTRY_VERIFY_WITH_TIMEOUT(test.isFinished(), 5000);
    QCOMPARE(test.result(), QStringLiteral("Connected to fal.ai"));
    FalProvider wrongKey(FalProvider::describe(), QStringLiteral("w"), &m_http, nullptr);
    wrongKey.configure({{QStringLiteral("url"), base}, {QStringLiteral("api_key"), QStringLiteral("bad")}});
    test = wrongKey.testConnection();
    QTRY_VERIFY_WITH_TIMEOUT(test.isFinished(), 5000);
    QVERIFY_THROWS_EXCEPTION(ProviderError, test.result());

    // No key: fails without a request.
    FalProvider noKey(FalProvider::describe(), QStringLiteral("n"), &m_http, nullptr);
    op = noKey.generate(t2i);
    op->setAutoDelete(false);
    QTRY_VERIFY(op->isFinished());
    QCOMPARE(op->state(), Operation::State::Failed);
    delete op;
}

// Objects by name on the image engines (plan M18 §18.1, Utility::FindArea): Gemini's masks for the
// words (none is an answer), fal.ai's EVF-SAM and a ComfyUI text-prompted segmentation template;
// ai::areaFromMasks puts the answers together.
void TestProvidersCloud::findAreaEngines()
{
    const QSize canvas(200, 100);
    QImage cup(40, 40, QImage::Format_Grayscale8);
    cup.fill(255);

    // Gemini.
    MockHttpServer gemini;
    QString asked;
    bool none = false;
    gemini.route("POST", QStringLiteral("/v1beta/models/gemini-2.5-flash:generateContent"), [&](const MockRequest &req) {
        const QJsonArray parts = req.json().value(QStringLiteral("contents")).toArray().first().toObject().value(QStringLiteral("parts")).toArray();
        asked = parts.last().toObject().value(QStringLiteral("text")).toString();
        const QString list = none ? QStringLiteral("[]")
                                  : QStringLiteral("[{\"box_2d\": [0, 0, 500, 250], \"mask\": \"%1\"}]").arg(img::toDataUrl(cup));
        return MockResponse::json({{QStringLiteral("candidates"), QJsonArray{QJsonObject{{QStringLiteral("content"), QJsonObject{
            {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), list}}}}}}}}}});
    });
    GeminiImageProvider g(GeminiImageProvider::describe(), QStringLiteral("g"), &m_http, nullptr);
    g.configure({{QStringLiteral("url"), gemini.baseUrl().toString() + QStringLiteral("/v1beta")}, {QStringLiteral("api_key"), QStringLiteral("k")}});
    QVERIFY(g.supportsUtility(Utility::FindArea));
    QImage picture(canvas, QImage::Format_ARGB32);
    picture.fill(Qt::gray);
    for (bool empty : {false, true}) {
        none = empty;
        ImageOperation *op = g.runUtility(Utility::FindArea, picture, {{QStringLiteral("what"), QStringLiteral("the cup")}});
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
        QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
        QVERIFY(asked.contains(QStringLiteral("Give the segmentation masks for: the cup.")));
        int objects = -1;
        QList<QImage> masks;
        for (const GeneratedImage &m : op->result().images)
            masks << m.image;
        const QImage area = ai::areaFromMasks(masks, canvas, &objects);
        if (empty) {
            QVERIFY(area.isNull());
            QCOMPARE(objects, 0);
        } else {
            QCOMPARE(objects, 1);
            QCOMPARE(area.size(), canvas);
            QCOMPARE(qGray(area.pixel(20, 20)), 255); // inside the box: 0-250 of 1000 across, 0-500 down
            QCOMPARE(qGray(area.pixel(150, 20)), 0);
            QCOMPARE(qGray(area.pixel(20, 80)), 0);
        }
        delete op;
    }

    // fal.ai.
    MockHttpServer fal;
    const QString base = fal.baseUrl().toString();
    QJsonObject submitted;
    fal.route("POST", QStringLiteral("/fal-ai/evf-sam"), [&](const MockRequest &req) {
        submitted = req.json();
        return MockResponse::json({{QStringLiteral("request_id"), QStringLiteral("e1")},
                                   {QStringLiteral("status_url"), base + QStringLiteral("/fal-ai/evf-sam/requests/e1/status")},
                                   {QStringLiteral("response_url"), base + QStringLiteral("/fal-ai/evf-sam/requests/e1")}});
    });
    fal.route("GET", QStringLiteral("/fal-ai/evf-sam/requests/e1/status"),
              [](const MockRequest &) { return MockResponse::json({{QStringLiteral("status"), QStringLiteral("COMPLETED")}}); });
    fal.route("GET", QStringLiteral("/fal-ai/evf-sam/requests/e1"), [](const MockRequest &) {
        QImage mask(100, 50, QImage::Format_RGB32);
        mask.fill(Qt::black);
        QPainter(&mask).fillRect(QRect(50, 0, 50, 50), Qt::white);
        return MockResponse::json({{QStringLiteral("image"), QJsonObject{{QStringLiteral("url"), img::toDataUrl(mask)}}}});
    });
    FalProvider f(FalProvider::describe(), QStringLiteral("f"), &m_http, nullptr);
    f.configure({{QStringLiteral("url"), base}, {QStringLiteral("api_key"), QStringLiteral("secret")}});
    QVERIFY(f.supportsUtility(Utility::FindArea));
    {
        ImageOperation *op = f.runUtility(Utility::FindArea, picture, {{QStringLiteral("what"), QStringLiteral("people")}});
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
        QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
        QCOMPARE(submitted.value(QStringLiteral("prompt")).toString(), QStringLiteral("people"));
        QVERIFY(submitted.value(QStringLiteral("mask_only")).toBool());
        QVERIFY(submitted.value(QStringLiteral("image_url")).toString().startsWith(QStringLiteral("data:image/")));
        QCOMPARE(op->result().images.size(), 1);
        const QImage area = ai::areaFromMasks({op->result().images.first().image}, canvas);
        QCOMPARE(qGray(area.pixel(150, 50)), 255); // the right half, scaled up to the picture
        QCOMPARE(qGray(area.pixel(50, 50)), 0);
        delete op;
    }

    // ComfyUI: no text-prompted template, no finding; with one, the words go to its prompt.
    ComfyUiProvider c(ComfyUiProvider::describe(), QStringLiteral("c"), &m_http, nullptr);
    MockHttpServer comfy;
    c.configure({{QStringLiteral("url"), comfy.baseUrl().toString()}, {QStringLiteral("segment_workflow"), QStringLiteral("test_find")}});
    const QString tplPath = ComfyUiProvider::workflowDirectory() + QStringLiteral("/test_find.json");
    QFile::remove(tplPath);
    QDir().mkpath(ComfyUiProvider::workflowDirectory());
    {
        QFile file(tplPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"name": "find", "modes": ["segment_parts"], "output_kind": "mask", "outputs": {"6": "mask"},
                       "bindings": {"init_image": {"node": "1", "input": "image"}, "prompt": {"node": "4", "input": "prompt"}},
                       "workflow": {"1": {"class_type": "LoadImage", "inputs": {"image": "x.png"}},
                                    "4": {"class_type": "Segment", "inputs": {"image": ["1", 0], "prompt": "p"}},
                                    "6": {"class_type": "PreviewImage", "inputs": {"images": ["4", 0]}}}})");
    }
    QStringList prompts;
    bool found = true;
    comfy.route("POST", QStringLiteral("/upload/image"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("name"), QStringLiteral("in.png")}, {QStringLiteral("subfolder"), QString()}, {QStringLiteral("type"), QStringLiteral("input")}});
    });
    comfy.route("POST", QStringLiteral("/prompt"), [&](const MockRequest &req) {
        prompts << req.json().value(QStringLiteral("prompt")).toObject().value(QStringLiteral("4")).toObject()
                       .value(QStringLiteral("inputs")).toObject().value(QStringLiteral("prompt")).toString();
        return MockResponse::json({{QStringLiteral("prompt_id"), QStringLiteral("p%1").arg(prompts.size())}});
    });
    for (int i = 1; i <= 2; ++i) {
        comfy.route("GET", QStringLiteral("/history/p%1").arg(i), [i](const MockRequest &) {
            return MockResponse::json({{QStringLiteral("p%1").arg(i), QJsonObject{
                {QStringLiteral("status"), QJsonObject{{QStringLiteral("status_str"), QStringLiteral("success")}}},
                {QStringLiteral("outputs"), QJsonObject{{QStringLiteral("6"), QJsonObject{{QStringLiteral("images"), QJsonArray{
                    QJsonObject{{QStringLiteral("filename"), QStringLiteral("m.png")}, {QStringLiteral("subfolder"), QString()},
                                {QStringLiteral("type"), QStringLiteral("temp")}}}}}}}}}}});
        });
    }
    comfy.route("GET", QStringLiteral("/view"), [&](const MockRequest &) {
        QImage m(canvas, QImage::Format_RGB32);
        m.fill(Qt::black);
        if (found)
            QPainter(&m).fillRect(QRect(0, 0, 100, 100), Qt::white);
        MockResponse r;
        r.contentType = "image/png";
        r.body = img::encode(m);
        return r;
    });
    QVERIFY(c.supportsUtility(Utility::FindArea));
    for (bool there : {true, false}) {
        found = there;
        ImageOperation *op = c.runUtility(Utility::FindArea, picture, {{QStringLiteral("what"), QStringLiteral("the cup")}});
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 15000);
        QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
        QCOMPARE(prompts.last(), QStringLiteral("the cup"));
        QCOMPARE(op->result().images.size(), there ? 1 : 0); // nothing found is an answer, not a failure
        if (there)
            QCOMPARE(qGray(op->result().images.first().image.pixel(50, 50)), 255);
        delete op;
    }
    QFile::remove(tplPath);
}

// ComfyUI lists its LoRAs (plan M18 §18.3): the LoraLoader node's choices, for the LoRA picker.
void TestProvidersCloud::comfyLoras()
{
    MockHttpServer server;
    server.route("GET", QStringLiteral("/object_info/LoraLoader"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("LoraLoader"), QJsonObject{{QStringLiteral("input"), QJsonObject{{QStringLiteral("required"), QJsonObject{
            {QStringLiteral("lora_name"), QJsonArray{QJsonArray{QStringLiteral("styles\\ink.safetensors"), QStringLiteral("pixel.safetensors")}}}}}}}}}});
    });
    ComfyUiProvider c(ComfyUiProvider::describe(), QStringLiteral("c"), &m_http, nullptr);
    c.configure({{QStringLiteral("url"), server.baseUrl().toString()}});
    QVERIFY(c.capabilities().loras);
    QFuture<QList<ServerLora>> listed = c.availableLoras();
    QTRY_VERIFY_WITH_TIMEOUT(listed.isFinished(), 5000);
    const QList<ServerLora> loras = listed.result();
    QCOMPARE(loras.size(), 2);
    QCOMPARE(loras[0].name, QStringLiteral("ink"));
    QCOMPARE(loras[0].path, QStringLiteral("styles/ink.safetensors"));
    QCOMPARE(loras[1].name, QStringLiteral("pixel"));
}

QTEST_MAIN(TestProvidersCloud)
#include "tst_providers_cloud.moc"
