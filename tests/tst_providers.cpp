#include "MockHttpServer.h"

#include "ai/ProviderRegistry.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "ai/providers/SdWebUiProvider.h"
#include "core/ImageOps.h"
#include "net/HttpClient.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest>

using namespace lumen;
using namespace lumen::ai;

static QString pngB64(const QColor &c, const QSize &s = QSize(8, 8))
{
    QImage img(s, QImage::Format_ARGB32);
    img.fill(c);
    return QString::fromLatin1(img::toBase64(img));
}

class TestProviders : public QObject
{
    Q_OBJECT
private slots:
    void sdwebuiPickModels();
    void sdwebuiTxt2ImgWithPoseAndReference();
    void sdwebuiInpaintPayload();
    void sdwebuiCancelInterrupts();
    void sdwebuiMissingPoseModelFails();
    void openaiStreamingText();
    void openaiToolCallsAccumulate();
    void openaiSchemaBody();
    void openaiHttpError();
    void thinkSplitting();
    void registryPersistsRoles();

private:
    net::HttpClient m_http;
};

void TestProviders::sdwebuiPickModels()
{
    const QStringList models = {QStringLiteral("control_v11p_sd15_openpose [cab727d4]"),
                                QStringLiteral("thibaud_xl_openpose [c7b9cadd]"),
                                QStringLiteral("ip-adapter-plus_sdxl_vit-h [f1f19f7d]"),
                                QStringLiteral("ip-adapter_sd15 [6a3f6166]")};
    QCOMPARE(SdWebUiProvider::pickControlNetModel(models, QStringLiteral("pose"), QStringLiteral("ponyDiffusionV6XL")),
             QStringLiteral("thibaud_xl_openpose [c7b9cadd]"));
    QCOMPARE(SdWebUiProvider::pickControlNetModel(models, QStringLiteral("pose"), QStringLiteral("v1-5-pruned-emaonly")),
             QStringLiteral("control_v11p_sd15_openpose [cab727d4]"));
    QCOMPARE(SdWebUiProvider::pickControlNetModel(models, QStringLiteral("ipadapter"), QStringLiteral("illustriousXL")),
             QStringLiteral("ip-adapter-plus_sdxl_vit-h [f1f19f7d]"));
}

void TestProviders::sdwebuiTxt2ImgWithPoseAndReference()
{
    MockHttpServer server;
    server.route("GET", QStringLiteral("/controlnet/model_list"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model_list"),
                                    QJsonArray{QStringLiteral("thibaud_xl_openpose [c7b9cadd]"), QStringLiteral("ip-adapter-plus_sdxl_vit-h [f1f19f7d]")}}});
    });
    QJsonObject captured;
    server.route("POST", QStringLiteral("/sdapi/v1/txt2img"), [&](const MockRequest &req) {
        captured = req.json();
        const QJsonObject info{{QStringLiteral("seed"), 123}, {QStringLiteral("all_seeds"), QJsonArray{123, 124}},
                               {QStringLiteral("infotexts"), QJsonArray{QStringLiteral("a\nSteps: 20, Seed: 123"), QStringLiteral("b")}}};
        MockResponse r = MockResponse::json({{QStringLiteral("images"), QJsonArray{pngB64(Qt::red), pngB64(Qt::green), pngB64(Qt::black)}},
                                             {QStringLiteral("info"), QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact))}});
        r.delayMs = 1200;
        return r;
    });
    int polls = 0;
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [&](const MockRequest &) {
        ++polls;
        return MockResponse::json({{QStringLiteral("progress"), 0.5},
                                   {QStringLiteral("state"), QJsonObject{{QStringLiteral("sampling_step"), 10}, {QStringLiteral("sampling_steps"), 20}}},
                                   {QStringLiteral("current_image"), pngB64(Qt::blue)}});
    });

    SdWebUiProvider provider(SdWebUiProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    provider.configure({{QStringLiteral("url"), server.baseUrl().toString()}, {QStringLiteral("model"), QStringLiteral("ponyDiffusionV6XL")}});
    ImageRequest req;
    req.prompt = QStringLiteral("1girl");
    req.size = QSize(832, 1216);
    req.batchCount = 2;
    req.controls.append(ControlInput{ControlType::OpenPose, QImage(64, 64, QImage::Format_RGB32), 1.0, 0.0, 1.0});
    ReferenceImage ref;
    ref.image = QImage(64, 64, QImage::Format_RGB32);
    req.references.append(ref);
    ImageOperation *op = provider.generate(req);
    op->setAutoDelete(false);
    QSignalSpy progress(op, &Operation::progress);
    QSignalSpy previews(op, &Operation::preview);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().images.size(), 2); // ControlNet map dropped
    QCOMPARE(op->result().images[0].seed, 123);
    QCOMPARE(op->result().images[1].seed, 124);
    QCOMPARE(op->result().images[0].image.pixelColor(1, 1), QColor(Qt::red));
    QVERIFY(op->result().images[0].parameters.contains(QStringLiteral("Seed: 123")));
    QVERIFY(polls >= 1);
    QVERIFY(previews.count() >= 1);
    // Payload checks.
    QCOMPARE(captured.value(QStringLiteral("width")).toInt(), 832);
    QCOMPARE(captured.value(QStringLiteral("batch_size")).toInt(), 2);
    QCOMPARE(captured.value(QStringLiteral("override_settings")).toObject().value(QStringLiteral("sd_model_checkpoint")).toString(),
             QStringLiteral("ponyDiffusionV6XL"));
    const QJsonArray units = captured.value(QStringLiteral("alwayson_scripts")).toObject().value(QStringLiteral("controlnet")).toObject()
                                 .value(QStringLiteral("args")).toArray();
    QCOMPARE(units.size(), 2);
    QCOMPARE(units[0].toObject().value(QStringLiteral("model")).toString(), QStringLiteral("thibaud_xl_openpose [c7b9cadd]"));
    QCOMPARE(units[0].toObject().value(QStringLiteral("module")).toString(), QStringLiteral("none"));
    QCOMPARE(units[1].toObject().value(QStringLiteral("model")).toString(), QStringLiteral("ip-adapter-plus_sdxl_vit-h [f1f19f7d]"));
    QVERIFY(!units[1].toObject().value(QStringLiteral("image")).toString().isEmpty());
    delete op;
}

void TestProviders::sdwebuiInpaintPayload()
{
    SdWebUiProvider provider(SdWebUiProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    ImageRequest req;
    req.mode = GenMode::Inpaint;
    req.initImage = QImage(16, 16, QImage::Format_ARGB32);
    req.initImage.fill(Qt::white);
    req.mask = img::createMask(QSize(16, 16));
    req.mask.scanLine(3)[3] = 40;
    req.denoise = 0.75;
    const QJsonObject p = provider.buildPayload(req, {});
    QCOMPARE(p.value(QStringLiteral("init_images")).toArray().size(), 1);
    QCOMPARE(p.value(QStringLiteral("denoising_strength")).toDouble(), 0.75);
    QCOMPARE(p.value(QStringLiteral("inpainting_fill")).toInt(), 1);
    const QImage mask = img::fromBase64(p.value(QStringLiteral("mask")).toString().toLatin1());
    QCOMPARE(qGray(mask.pixel(3, 3)), 255); // soft mask values are binarised for the server
    QVERIFY(!p.contains(QStringLiteral("alwayson_scripts")));
}

void TestProviders::sdwebuiCancelInterrupts()
{
    MockHttpServer server;
    server.route("POST", QStringLiteral("/sdapi/v1/txt2img"), [](const MockRequest &) {
        MockResponse r = MockResponse::json({});
        r.delayMs = 4000;
        return r;
    });
    bool interrupted = false;
    server.route("POST", QStringLiteral("/sdapi/v1/interrupt"), [&](const MockRequest &) {
        interrupted = true;
        return MockResponse::json({});
    });
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [](const MockRequest &) { return MockResponse::json({}); });
    SdWebUiProvider provider(SdWebUiProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    provider.configure({{QStringLiteral("url"), server.baseUrl().toString()}});
    ImageOperation *op = provider.generate(ImageRequest());
    op->setAutoDelete(false);
    QTimer::singleShot(300, op, &Operation::cancel);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 3000);
    QCOMPARE(op->state(), Operation::State::Cancelled);
    QTRY_VERIFY_WITH_TIMEOUT(interrupted, 3000);
    delete op;
}

void TestProviders::sdwebuiMissingPoseModelFails()
{
    MockHttpServer server;
    server.route("GET", QStringLiteral("/controlnet/model_list"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model_list"), QJsonArray{}}});
    });
    SdWebUiProvider provider(SdWebUiProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    provider.configure({{QStringLiteral("url"), server.baseUrl().toString()}});
    ImageRequest req;
    req.controls.append(ControlInput{ControlType::OpenPose, QImage(8, 8, QImage::Format_RGB32), 1.0, 0.0, 1.0});
    ImageOperation *op = provider.generate(req);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QCOMPARE(op->state(), Operation::State::Failed);
    QVERIFY2(op->errorString().contains(QStringLiteral("OpenPose ControlNet model")), qPrintable(op->errorString()));
    delete op;
}

void TestProviders::openaiStreamingText()
{
    MockHttpServer server;
    QJsonObject captured;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        captured = req.json();
        MockResponse r;
        r.contentType = "text/event-stream";
        r.chunks = {"data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"Hel\"}}]}\n\n",
                    "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
                    "data: [DONE]\n\n"};
        return r;
    });
    OpenAiChatProvider provider(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("llm"), &m_http, nullptr);
    provider.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}, {QStringLiteral("model"), QStringLiteral("qwen")}});
    ChatRequest req;
    QImage img(32, 32, QImage::Format_RGB32);
    img.fill(Qt::red);
    req.messages = {ChatMessage::system(QStringLiteral("sys")), ChatMessage::user(QStringLiteral("hi"), {img})};
    ChatOperation *op = provider.chat(req);
    op->setAutoDelete(false);
    QStringList deltas;
    connect(op, &ChatOperation::textDelta, this, [&](const QString &d) { deltas << d; });
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().text, QStringLiteral("Hello"));
    QCOMPARE(op->result().finishReason, QStringLiteral("stop"));
    QCOMPARE(deltas, QStringList({QStringLiteral("Hel"), QStringLiteral("lo")}));
    const QJsonArray msgs = captured.value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgs.size(), 2);
    const QJsonArray parts = msgs[1].toObject().value(QStringLiteral("content")).toArray();
    QCOMPARE(parts.size(), 2);
    QVERIFY(parts[1].toObject().value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString().startsWith(QStringLiteral("data:image/jpeg;base64,")));
    QCOMPARE(captured.value(QStringLiteral("model")).toString(), QStringLiteral("qwen"));
    delete op;
}

void TestProviders::openaiToolCallsAccumulate()
{
    OpenAiStreamAccumulator acc;
    QString t, r;
    acc.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"generate","arguments":"{\"pro"}}]}}]})", &t, &r);
    acc.feed(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"mpt\": \"cat\"}"}}]}}]})", &t, &r);
    acc.feed(R"({"choices":[{"delta":{"reasoning_content":"thinking"},"finish_reason":"tool_calls"}]})", &t, &r);
    const ChatResult res = acc.finish();
    QCOMPARE(res.toolCalls.size(), 1);
    QCOMPARE(res.toolCalls[0].id, QStringLiteral("call_1"));
    QCOMPARE(res.toolCalls[0].name, QStringLiteral("generate"));
    QCOMPARE(res.toolCalls[0].arguments.value(QStringLiteral("prompt")).toString(), QStringLiteral("cat"));
    QCOMPARE(res.reasoning, QStringLiteral("thinking"));
    QCOMPARE(r, QStringLiteral("thinking"));
}

void TestProviders::openaiSchemaBody()
{
    OpenAiChatProvider openai(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::OpenAI), QStringLiteral("o"), &m_http, nullptr);
    ChatRequest req;
    req.messages = {ChatMessage::user(QStringLiteral("x"))};
    req.jsonSchema = QJsonDocument::fromJson(R"({"type":"object","properties":{"a":{"type":"integer","minimum":1}}})").object();
    QJsonObject body = openai.buildBody(req);
    const QJsonObject rf = body.value(QStringLiteral("response_format")).toObject();
    QCOMPARE(rf.value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
    QVERIFY(rf.value(QStringLiteral("json_schema")).toObject().value(QStringLiteral("strict")).toBool());
    QVERIFY(body.contains(QStringLiteral("max_completion_tokens")));

    OpenAiChatProvider kobold(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::KoboldCpp), QStringLiteral("k"), &m_http, nullptr);
    body = kobold.buildBody(req);
    QCOMPARE(body.value(QStringLiteral("response_format")).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("json_object"));
    // Schema guidance is appended as a system message.
    QVERIFY(body.value(QStringLiteral("messages")).toArray().last().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("JSON schema")));
}

void TestProviders::openaiHttpError()
{
    MockHttpServer server;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("model not loaded")}}}}, 400);
    });
    OpenAiChatProvider provider(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::Ollama), QStringLiteral("llm"), &m_http, nullptr);
    provider.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}});
    ChatOperation *op = provider.chat(ChatRequest());
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QCOMPARE(op->state(), Operation::State::Failed);
    QCOMPARE(op->errorString(), QStringLiteral("HTTP 400: model not loaded"));
    delete op;
}

void TestProviders::thinkSplitting()
{
    QString text = QStringLiteral("<think>plan it</think>\n{\"a\":1}");
    QString reasoning;
    splitThinking(text, reasoning);
    QCOMPARE(text, QStringLiteral("{\"a\":1}"));
    QCOMPARE(reasoning, QStringLiteral("plan it"));
}

void TestProviders::registryPersistsRoles()
{
    QStandardPaths::setTestModeEnabled(true);
    ProviderRegistry registry(&m_http);
    registry.registerType(SdWebUiProvider::describe(), [](const ProviderType &t, const QString &id, net::HttpClient *h, QObject *p) {
        return new SdWebUiProvider(t, id, h, p);
    });
    for (const ProviderType &t : OpenAiChatProvider::describeAll()) {
        registry.registerType(t, [](const ProviderType &tt, const QString &id, net::HttpClient *h, QObject *p) {
            return new OpenAiChatProvider(tt, id, h, p);
        });
    }
    QFile::remove(registry.configPath());
    QDir().mkpath(QFileInfo(registry.configPath()).absolutePath());
    Provider *sd = registry.addProvider(QStringLiteral("sdwebui"), QStringLiteral("My SD"), {});
    Provider *llm = registry.addProvider(QStringLiteral("ollama"), QStringLiteral("Ollama"), {{QStringLiteral("model"), QStringLiteral("gemma3")}});
    QVERIFY(sd && llm);
    QCOMPARE(registry.defaultImageProvider(), sd);
    QString model;
    QCOMPARE(registry.llmFor(LlmRole::Vision, &model), llm);
    registry.setRole(LlmRole::Vision, RoleBinding{llm->instanceId(), QStringLiteral("qwen2.5vl")});

    ProviderRegistry again(&m_http);
    again.registerType(SdWebUiProvider::describe(), [](const ProviderType &t, const QString &id, net::HttpClient *h, QObject *p) {
        return new SdWebUiProvider(t, id, h, p);
    });
    for (const ProviderType &t : OpenAiChatProvider::describeAll()) {
        again.registerType(t, [](const ProviderType &tt, const QString &id, net::HttpClient *h, QObject *p) {
            return new OpenAiChatProvider(tt, id, h, p);
        });
    }
    again.load();
    QCOMPARE(again.providers().size(), 2);
    QCOMPARE(again.provider(llm->instanceId())->text(QStringLiteral("model")), QStringLiteral("gemma3"));
    QVERIFY(again.llmFor(LlmRole::Vision, &model));
    QCOMPARE(model, QStringLiteral("qwen2.5vl"));
    QFile::remove(registry.configPath());
}

QTEST_MAIN(TestProviders)
#include "tst_providers.moc"
