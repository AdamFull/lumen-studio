#include "MockHttpServer.h"

#include "ai/ProviderRegistry.h"
#include "ai/Director.h"
#include "ai/ModelDefaults.h"
#include "ai/PromptRules.h"
#include "ai/PromptStyle.h"
#include "ai/ProviderUtil.h"
#include "ai/providers/ComfyUiProvider.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "ai/providers/SdCppProvider.h"
#include "ai/providers/SdWebUiProvider.h"
#include "core/ImageOps.h"
#include "net/HttpClient.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
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
    void sdcppGenerateWithPoseAndReference();
    void sdcppFailureCancelAndUpscale();
    void lorasAcrossProviders();
    void sdCppPromptStyle();
    void promptRules();
    void sdcppPresetDefaultsAndEdit();

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

namespace {

QString pngBase64(const QSize &size, const QColor &color)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QString::fromLatin1(bytes.toBase64());
}

} // namespace

void TestProviders::sdcppGenerateWithPoseAndReference()
{
    MockHttpServer server;
    QJsonObject submitted;
    int polls = 0;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model"), QJsonObject{{QStringLiteral("name"), QStringLiteral("waiIllustriousSDXL_v170.safetensors")}}},
                                   {QStringLiteral("samplers"), QJsonArray{QStringLiteral("euler"), QStringLiteral("euler_a")}},
                                   {QStringLiteral("schedulers"), QJsonArray{QStringLiteral("discrete"), QStringLiteral("karras")}}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        submitted = req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")},
                                   {QStringLiteral("status"), QStringLiteral("queued")},
                                   {QStringLiteral("poll_url"), QStringLiteral("/sdcpp/v1/jobs/job_1")}},
                                  202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [&](const MockRequest &) {
        ++polls;
        if (polls == 1)
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("queued")}, {QStringLiteral("queue_position"), 1}});
        if (polls == 2) {
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("generating")},
                                       {QStringLiteral("preview"), QJsonObject{{QStringLiteral("pass"), 1},
                                                                                {QStringLiteral("step"), 7},
                                                                                {QStringLiteral("total_steps"), 28},
                                                                                {QStringLiteral("b64_json"), pngBase64(QSize(64, 96), Qt::gray)}}}});
        }
        const QJsonArray images{
            QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngBase64(QSize(832, 1216), Qt::red)}},
            QJsonObject{{QStringLiteral("index"), 1}, {QStringLiteral("b64_json"), pngBase64(QSize(832, 1216), Qt::blue)}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")},
                                   {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });

    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    sd.pollIntervalMs = 20;
    sd.configure({{QStringLiteral("url"), server.baseUrl().toString()}});
    // Without a ControlNet / IP-Adapter on the server, pose and references are not offered.
    QVERIFY(!sd.capabilities().poseControl);
    QCOMPARE(sd.capabilities().maxReferences, 0);
    sd.configure({{QStringLiteral("control_net"), true}, {QStringLiteral("ip_adapter"), true}});
    QVERIFY(sd.capabilities().poseControl);
    QCOMPARE(sd.capabilities().maxReferences, 1);
    QVERIFY(!sd.supportsUtility(Utility::Upscale));

    QFuture<QString> test = sd.testConnection();
    QTRY_VERIFY(test.isFinished());
    QCOMPARE(test.result(), QStringLiteral("Connected - model: waiIllustriousSDXL_v170.safetensors"));
    QFuture<QStringList> samplers = sd.listOptions(QStringLiteral("sampler"));
    QTRY_VERIFY(samplers.isFinished());
    QCOMPARE(samplers.result(), (QStringList{QStringLiteral("euler"), QStringLiteral("euler_a")}));

    ImageRequest r;
    r.prompt = QStringLiteral("1girl, red hair");
    r.negativePrompt = QStringLiteral("lowres");
    r.size = QSize(832, 1216);
    r.seed = 1234;
    r.steps = 28;
    r.cfg = 6.0;
    r.sampler = QStringLiteral("euler_a");
    r.batchCount = 2;
    ControlInput pose;
    pose.image = QImage(832, 1216, QImage::Format_RGB32);
    pose.image.fill(Qt::black);
    pose.weight = 0.9;
    r.controls << pose;
    ReferenceImage style;
    style.image = QImage(64, 64, QImage::Format_ARGB32);
    style.image.fill(Qt::transparent);
    style.role = ReferenceRole::Style;
    ReferenceImage character;
    character.image = QImage(64, 64, QImage::Format_ARGB32);
    character.image.fill(Qt::green);
    character.role = ReferenceRole::Character;
    character.weight = 0.8;
    r.references << style << character;

    ImageOperation *op = sd.generate(r);
    op->setAutoDelete(false);
    QSignalSpy progress(op, &Operation::progress);
    QSignalSpy preview(op, &Operation::preview);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().images.size(), 2);
    QCOMPARE(op->result().images[0].image.pixelColor(10, 10), QColor(Qt::red));
    QCOMPARE(op->result().images[1].seed, qint64(1235));
    QVERIFY(preview.size() >= 1);
    bool sawStep = false;
    bool sawQueue = false;
    for (const QList<QVariant> &args : progress) {
        sawStep |= args.at(1).toString() == QString::fromUtf8("Image 1 of 2 · step 7 / 28");
        sawQueue |= args.at(1).toString() == QStringLiteral("Queued (position 1)");
    }
    QVERIFY(sawStep);
    QVERIFY(sawQueue);
    delete op;

    QCOMPARE(submitted.value(QStringLiteral("prompt")).toString(), r.prompt);
    QCOMPARE(submitted.value(QStringLiteral("width")).toInt(), 832);
    QCOMPARE(submitted.value(QStringLiteral("batch_count")).toInt(), 2);
    QCOMPARE(submitted.value(QStringLiteral("seed")).toInteger(), 1234);
    const QJsonObject sample = submitted.value(QStringLiteral("sample_params")).toObject();
    QCOMPARE(sample.value(QStringLiteral("sample_method")).toString(), QStringLiteral("euler_a"));
    QCOMPARE(sample.value(QStringLiteral("sample_steps")).toInt(), 28);
    QCOMPARE(sample.value(QStringLiteral("guidance")).toObject().value(QStringLiteral("txt_cfg")).toDouble(), 6.0);
    QVERIFY(submitted.value(QStringLiteral("vae_tiling_params")).toObject().value(QStringLiteral("enabled")).toBool());
    QVERIFY(!submitted.value(QStringLiteral("control_image")).toString().isEmpty());
    QCOMPARE(submitted.value(QStringLiteral("control_strength")).toDouble(), 0.9);
    // The character reference is used, and its weight is limited because a pose map is present.
    const QImage ipa = decodeBase64Image(submitted.value(QStringLiteral("ip_adapter_image")).toString());
    QCOMPARE(ipa.pixelColor(5, 5), QColor(Qt::green));
    QCOMPARE(submitted.value(QStringLiteral("ip_adapter_strength")).toDouble(), 0.3);
    QVERIFY(!submitted.contains(QStringLiteral("init_image")));

    // img2img + inpaint, no pose: the reference keeps its own weight.
    ImageRequest inpaint = r;
    inpaint.mode = GenMode::Inpaint;
    inpaint.controls.clear();
    inpaint.initImage = QImage(512, 512, QImage::Format_ARGB32);
    inpaint.initImage.fill(Qt::white);
    inpaint.mask = QImage(512, 512, QImage::Format_Grayscale8);
    inpaint.mask.fill(200);
    inpaint.denoise = 0.55;
    const QJsonObject body = sd.buildPayload(inpaint);
    QCOMPARE(body.value(QStringLiteral("strength")).toDouble(), 0.55);
    QVERIFY(!body.value(QStringLiteral("init_image")).toString().isEmpty());
    const QImage mask = decodeBase64Image(body.value(QStringLiteral("mask_image")).toString());
    QCOMPARE(qGray(mask.pixel(3, 3)), 255);
    QCOMPARE(body.value(QStringLiteral("ip_adapter_strength")).toDouble(), 0.8);
    QVERIFY(!body.contains(QStringLiteral("control_image")));
    // Instruction edits are not offered.
    ImageRequest edit = r;
    edit.mode = GenMode::Edit;
    op = sd.generate(edit);
    op->setAutoDelete(false);
    QTRY_VERIFY(op->isFinished());
    QVERIFY(op->state() == Operation::State::Failed);
    delete op;
}

void TestProviders::sdcppFailureCancelAndUpscale()
{
    MockHttpServer server;
    int jobs = 0;
    int cancels = 0;
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &) {
        ++jobs;
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_%1").arg(jobs)}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("failed")},
                                   {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("generation_failed")},
                                                                         {QStringLiteral("message"), QStringLiteral("CUDA out of memory")}}}});
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_2"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("generating")}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/jobs/job_2/cancel"), [&](const MockRequest &) {
        ++cancels;
        return MockResponse::json({{QStringLiteral("error"), QStringLiteral("cannot cancel a running job")}}, 409);
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/upscale"), [](const MockRequest &req) {
        const QImage in = decodeBase64Image(req.json().value(QStringLiteral("image")).toString());
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngBase64(in.size() * 4, Qt::yellow)}}};
        return MockResponse::json({{QStringLiteral("images"), images}, {QStringLiteral("upscaler"), QStringLiteral("RealESRGAN_x4plus_anime_6B")}});
    });
    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    sd.pollIntervalMs = 20;
    sd.configure({{QStringLiteral("url"), server.baseUrl().toString()}, {QStringLiteral("upscaler"), true}});
    ImageRequest r;
    r.prompt = QStringLiteral("x");

    ImageOperation *op = sd.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QCOMPARE(op->errorString(), QStringLiteral("CUDA out of memory"));
    delete op;

    // A running job cannot be stopped by the server; the operation still ends at once.
    op = sd.generate(r);
    op->setAutoDelete(false);
    QTRY_COMPARE_WITH_TIMEOUT(jobs, 2, 5000);
    QTest::qWait(100);
    op->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY(op->state() == Operation::State::Cancelled);
    QCOMPARE(cancels, 1);
    delete op;

    QVERIFY(sd.supportsUtility(Utility::Upscale));
    QImage small(100, 60, QImage::Format_ARGB32);
    small.fill(QColor(0, 0, 0, 0));
    op = sd.runUtility(Utility::Upscale, small, {{QStringLiteral("factor"), 2.0}});
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    // The model's own 4x is brought to the requested 2x.
    QCOMPARE(op->result().images.first().image.size(), QSize(200, 120));
    delete op;
}

void TestProviders::sdCppPromptStyle()
{
    // stable-diffusion.cpp is a local Stable Diffusion back-end: the checkpoint's name picks the
    // dialect (it used to be treated as a cloud model, so anime checkpoints got prose prompts).
    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    sd.configure({{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")},
                  {QStringLiteral("model"), QStringLiteral("waiIllustriousSDXL_v170.safetensors")}});
    PromptStyle style = promptStyleFor(&sd);
    QCOMPARE(style.family, QStringLiteral("illustrious"));
    QVERIFY(style.dialect == PromptStyle::Dialect::Tags);
    QVERIFY(style.apply(QStringLiteral("1girl")).startsWith(QStringLiteral("masterpiece")));
    sd.configure({{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")}});
    QVERIFY(promptStyleFor(&sd).family != QLatin1String("cloud"));

    // Anima: by the family read from the file (any name), or by name; Animagine is not Anima.
    sd.configure({{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")},
                  {QStringLiteral("model"), QStringLiteral("miaomiaoHarem_29BBETA11.safetensors")},
                  {QStringLiteral("model_family"), QStringLiteral("Anima")}});
    style = promptStyleFor(&sd);
    QCOMPARE(style.family, QStringLiteral("anima"));
    QVERIFY(style.dialect == PromptStyle::Dialect::Tags);
    QVERIFY(style.describeForLlm().contains(QStringLiteral("spaces instead of underscores")));
    QVERIFY(style.defaultNegative.startsWith(QStringLiteral("worst quality, low quality, score_1")));
    QCOMPARE(style.cfg, 4.5);
    QCOMPARE(promptStyleFor(&sd, QStringLiteral("waiIllustriousSDXL_v170")).cfg, 6.0);
    const QString prompt = style.apply(QStringLiteral("1girl, red hair, smile"));
    QCOMPARE(prompt, QStringLiteral("masterpiece, best quality, score_7, 1girl, red hair, smile"));
    // "Safe for work": the rating tag after the quality tags, unless a rating is there.
    QCOMPARE(style.withSafeTag(prompt), QStringLiteral("masterpiece, best quality, score_7, safe, 1girl, red hair, smile"));
    QCOMPARE(style.withSafeTag(QStringLiteral("1girl")), QStringLiteral("safe, 1girl"));
    QCOMPARE(style.withSafeTag(QStringLiteral("sensitive, 1girl")), QStringLiteral("sensitive, 1girl"));
    // The server's model by its name from the Generate page: still Anima; another model is not.
    QCOMPARE(promptStyleFor(&sd, QStringLiteral("miaomiaoHarem_29BBETA11")).family, QStringLiteral("anima"));
    QCOMPARE(promptStyleFor(&sd, QStringLiteral("waiIllustriousSDXL_v170")).family, QStringLiteral("illustrious"));
    QCOMPARE(promptStyleFor(&sd, QStringLiteral("anima-preview2.safetensors")).family, QStringLiteral("anima"));
    QCOMPARE(promptStyleFor(&sd, QStringLiteral("animagine-xl-4.0-opt.safetensors")).family, QStringLiteral("illustrious"));
    // Other models have no rating tag.
    QVERIFY(promptStyleFor(&sd, QStringLiteral("waiIllustriousSDXL_v170")).withSafeTag(QStringLiteral("1girl")) == QStringLiteral("1girl"));
}

namespace {

// The FLUX.2 klein 4B preset as the studio hands it to the provider of a server it runs.
QVariantMap kleinPreset()
{
    ModelDefaults d;
    d.id = QStringLiteral("flux2-klein-4b");
    d.name = QStringLiteral("FLUX.2 [klein] 4B");
    d.cfg = 1.0;
    d.steps = 4;
    d.sampler = QStringLiteral("euler");
    d.dialect = QStringLiteral("natural");
    d.nativeResolution = 1024;
    d.edit = true;
    d.references = 4;
    d.negativePrompt = false;
    return d.toVariant();
}

} // namespace

// A server Lumen runs knows its model's preset: FLUX.2 klein edits with reference images, takes
// 4 steps at CFG 1 and no negative prompt; FLUX.1 dev gets its distilled guidance.
void TestProviders::sdcppPresetDefaultsAndEdit()
{
    MockHttpServer server;
    QJsonObject submitted;
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        submitted = req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [&](const MockRequest &) {
        const QSize size(submitted.value(QStringLiteral("width")).toInt(), submitted.value(QStringLiteral("height")).toInt());
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngBase64(size, QColor(255, 128, 0))}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });
    auto run = [&](SdCppProvider &sd, const ImageRequest &r) {
        submitted = {};
        ImageOperation *op = sd.generate(r);
        op->setAutoDelete(false);
        if (!QTest::qWaitFor([op] { return op->isFinished(); }, 5000)) {
            delete op;
            return QStringLiteral("timed out");
        }
        const bool ok = op->state() == Operation::State::Succeeded;
        const QString error = op->errorString();
        delete op;
        return ok ? QString() : error;
    };

    // Without a preset: no instruction edits, and the request says why.
    SdCppProvider plain(SdCppProvider::describe(), QStringLiteral("plain"), &m_http, nullptr);
    plain.pollIntervalMs = 20;
    plain.configure({{QStringLiteral("url"), server.baseUrl().toString()}, {QStringLiteral("model"), QStringLiteral("waiIllustriousSDXL_v170.safetensors")}});
    QVERIFY(!plain.capabilities().edit);
    QVERIFY(plain.capabilities().negativePrompt);
    QCOMPARE(plain.capabilities().maxReferences, 0);
    ImageRequest edit;
    edit.mode = GenMode::Edit;
    edit.prompt = QStringLiteral("colorize the photo");
    edit.size = QSize(1024, 768);
    edit.initImage = QImage(1024, 768, QImage::Format_ARGB32);
    edit.initImage.fill(Qt::gray);
    edit.steps = 4;
    edit.cfg = 1.0;
    QVERIFY(run(plain, edit).contains(QLatin1String("FLUX.2 [klein]")));

    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
    sd.pollIntervalMs = 20;
    sd.configure({{QStringLiteral("url"), server.baseUrl().toString()},
                  {QStringLiteral("model"), QStringLiteral("flux-2-klein-4b.safetensors")},
                  {QStringLiteral("model_family"), QStringLiteral("FLUX.2")},
                  {QStringLiteral("model_preset"), kleinPreset()}});
    ImageCapabilities caps = sd.capabilities();
    QVERIFY(caps.edit);
    QVERIFY(caps.regionEdit);
    QVERIFY(!caps.negativePrompt);
    QCOMPARE(caps.maxReferences, 4);
    QCOMPARE(caps.nativePixels, qint64(1024) * 1024);
    QCOMPARE(sd.capabilitiesFor(edit).maxReferences, 3); // the source is the first reference

    // The prompt style follows the preset.
    const PromptStyle style = promptStyleFor(&sd);
    QCOMPARE(style.preset, QStringLiteral("flux2-klein-4b"));
    QCOMPARE(style.cfg, 1.0);
    QCOMPARE(style.steps, 4);
    QCOMPARE(style.sampler, QStringLiteral("euler"));
    QVERIFY(!style.supportsNegative);
    QVERIFY(style.defaultNegative.isEmpty());
    QVERIFY(style.dialect == PromptStyle::Dialect::Natural);
    // Another model than the server's: not its preset.
    QVERIFY(promptStyleFor(&sd, QStringLiteral("waiIllustriousSDXL_v170")).preset.isEmpty());

    // An edit: the source and a reference as ref_images; no init image, strength or mask.
    ReferenceImage character;
    character.image = QImage(64, 64, QImage::Format_ARGB32);
    character.image.fill(Qt::green);
    character.role = ReferenceRole::Character;
    edit.references << character;
    edit.mask = img::createMask(edit.size, 255);
    QCOMPARE(run(sd, edit), QString());
    const QJsonArray refs = submitted.value(QStringLiteral("ref_images")).toArray();
    QCOMPARE(refs.size(), 2);
    QCOMPARE(img::fromBase64(refs[0].toString().toLatin1()).pixelColor(5, 5), QColor(Qt::gray));
    QCOMPARE(img::fromBase64(refs[1].toString().toLatin1()).pixelColor(5, 5), QColor(Qt::green));
    QVERIFY(!submitted.contains(QStringLiteral("init_image")));
    QVERIFY(!submitted.contains(QStringLiteral("mask_image")));
    QVERIFY(!submitted.contains(QStringLiteral("strength")));
    QVERIFY(!submitted.contains(QStringLiteral("ip_adapter_image")));
    QJsonObject sample = submitted.value(QStringLiteral("sample_params")).toObject();
    QCOMPARE(sample.value(QStringLiteral("sample_steps")).toInt(), 4);
    QCOMPARE(sample.value(QStringLiteral("sample_method")).toString(), QStringLiteral("euler")); // the preset's
    QCOMPARE(sample.value(QStringLiteral("guidance")).toObject().value(QStringLiteral("txt_cfg")).toDouble(), 1.0);
    QVERIFY(!sample.value(QStringLiteral("guidance")).toObject().contains(QStringLiteral("distilled_guidance")));

    // A new image with a reference: guided by it through ref_images (there is no IP-Adapter).
    ImageRequest t2i;
    t2i.prompt = QStringLiteral("the same character at the beach");
    t2i.size = QSize(1024, 1024);
    t2i.steps = 4;
    t2i.cfg = 1.0;
    t2i.sampler = QStringLiteral("euler_a"); // the request's sampler wins
    t2i.references << character;
    QCOMPARE(run(sd, t2i), QString());
    QCOMPARE(submitted.value(QStringLiteral("ref_images")).toArray().size(), 1);
    QCOMPARE(submitted.value(QStringLiteral("sample_params")).toObject().value(QStringLiteral("sample_method")).toString(), QStringLiteral("euler_a"));

    // FLUX.1 dev: CFG 1 plus distilled guidance 3.5; Qwen-Image-Edit: image CFG and flow shift.
    ModelDefaults dev;
    dev.id = QStringLiteral("flux1-dev");
    dev.cfg = 1.0;
    dev.guidance = 3.5;
    dev.steps = 20;
    dev.negativePrompt = false;
    ModelDefaults qwen;
    qwen.id = QStringLiteral("qwen-image-edit");
    qwen.cfg = 2.5;
    qwen.imageCfg = 1.5;
    qwen.flowShift = 3;
    qwen.edit = true;
    qwen.references = 3;
    for (const ModelDefaults &d : {dev, qwen}) {
        sd.configure({{QStringLiteral("model_preset"), d.toVariant()}});
        ImageRequest r = d.edit ? edit : t2i;
        r.steps = 0; // not chosen: the preset's (or the server's 20)
        QCOMPARE(run(sd, r), QString());
        sample = submitted.value(QStringLiteral("sample_params")).toObject();
        QCOMPARE(sample.value(QStringLiteral("sample_steps")).toInt(), d.steps > 0 ? d.steps : 20);
        const QJsonObject guidance = sample.value(QStringLiteral("guidance")).toObject();
        QCOMPARE(guidance.value(QStringLiteral("distilled_guidance")).toDouble(), d.guidance);
        QCOMPARE(guidance.value(QStringLiteral("img_cfg")).toDouble(), d.imageCfg);
        QCOMPARE(sample.value(QStringLiteral("flow_shift")).toDouble(), d.flowShift);
    }
}

void TestProviders::lorasAcrossProviders()
{
    // Matching: by the server's relative path (a suffix of the file), else by name.
    {
        QList<LoraRef> loras(2);
        loras[0].file = QStringLiteral("C:/Lumen/models/loras/chars/akane_v2.safetensors");
        loras[0].name = QStringLiteral("akane_v2");
        loras[1].name = QStringLiteral("watercolor");
        const QList<ServerLora> available{{QStringLiteral("akane_v2"), QStringLiteral("old/akane_v2.safetensors")},
                                          {QStringLiteral("akane_v2"), QStringLiteral("chars/akane_v2.safetensors")},
                                          {QStringLiteral("Watercolor"), QStringLiteral("styles/Watercolor.safetensors")}};
        QString error;
        QVERIFY(resolveLoras(loras, available, &error));
        QCOMPARE(loras[0].resolved, QStringLiteral("chars/akane_v2.safetensors"));
        QCOMPARE(loras[1].resolved, QStringLiteral("styles/Watercolor.safetensors"));
        QList<LoraRef> missing(1);
        missing[0].name = QStringLiteral("nobody");
        QVERIFY(!resolveLoras(missing, available, &error));
        QVERIFY(error.contains(QStringLiteral("nobody")));
    }

    // A character's LoRA: trigger word first, once; skipped for providers without LoRAs.
    {
        CharacterLora c;
        c.file = QStringLiteral("C:/loras/akane_v2.safetensors");
        c.trigger = QStringLiteral("lmn_akane");
        c.weight = 0.75;
        ImageRequest r;
        r.prompt = QStringLiteral("1girl, running, park");
        ImageCapabilities caps;
        QVERIFY(!Director::applyCharacterLora(r, c, caps));
        QVERIFY(r.loras.isEmpty());
        caps.loras = true;
        QVERIFY(Director::applyCharacterLora(r, c, caps));
        QVERIFY(Director::applyCharacterLora(r, c, caps));
        QCOMPARE(r.prompt, QStringLiteral("lmn_akane, 1girl, running, park"));
        QCOMPARE(r.loras.size(), 1);
        QCOMPARE(r.loras[0].name, QStringLiteral("akane_v2"));
        QCOMPARE(r.loras[0].weight, 0.75);
        QCOMPARE(CharacterLora::fromJson(c.toJson()).trigger, c.trigger);
        QCOMPARE(r.describe().value(QStringLiteral("loras")).toArray().size(), 1);
    }

    // sd.cpp: the server's LoRA list resolves the path sent in `lora`.
    {
        MockHttpServer server;
        QJsonObject submitted;
        server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
            return MockResponse::json({{QStringLiteral("loras"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("akane_v2")},
                                                                                        {QStringLiteral("path"), QStringLiteral("chars/akane_v2.safetensors")}}}}});
        });
        server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
            submitted = req.json();
            return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
        });
        server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [](const MockRequest &) {
            const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngBase64(QSize(64, 64), Qt::red)}}};
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")},
                                       {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
        });
        SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("sd"), &m_http, nullptr);
        sd.pollIntervalMs = 20;
        sd.configure({{QStringLiteral("url"), server.baseUrl().toString()}});
        QVERIFY(sd.capabilities().loras);
        ImageRequest r;
        r.prompt = QStringLiteral("lmn_akane, 1girl");
        r.size = QSize(64, 64);
        LoraRef l;
        l.file = QStringLiteral("D:/models/loras/chars/akane_v2.safetensors");
        l.name = QStringLiteral("akane_v2");
        l.weight = 0.7;
        r.loras << l;
        ImageOperation *op = sd.generate(r);
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
        QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
        delete op;
        const QJsonArray sent = submitted.value(QStringLiteral("lora")).toArray();
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent[0].toObject().value(QStringLiteral("path")).toString(), QStringLiteral("chars/akane_v2.safetensors"));
        QCOMPARE(sent[0].toObject().value(QStringLiteral("multiplier")).toDouble(), 0.7);
        // A LoRA the server does not have: a clear error, nothing submitted.
        submitted = {};
        r.loras[0].file.clear();
        r.loras[0].name = QStringLiteral("unknown_lora");
        op = sd.generate(r);
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 10000);
        QCOMPARE(op->state(), Operation::State::Failed);
        QVERIFY(op->errorString().contains(QStringLiteral("unknown_lora")));
        QVERIFY(submitted.isEmpty());
        delete op;
    }

    // WebUI: LoRAs go into the prompt by the server's name for them.
    {
        SdWebUiProvider webui(SdWebUiProvider::describe(), QStringLiteral("w"), &m_http, nullptr);
        ImageRequest r;
        r.prompt = QStringLiteral("lmn_akane, 1girl");
        LoraRef l;
        l.name = QStringLiteral("akane_v2");
        l.resolved = QStringLiteral("akane v2");
        l.weight = 0.8;
        r.loras << l;
        QVERIFY(webui.capabilities().loras);
        QCOMPARE(webui.buildPayload(r, {}).value(QStringLiteral("prompt")).toString(), QStringLiteral("lmn_akane, 1girl <lora:akane v2:0.8>"));
    }

    // ComfyUI: a LoraLoader chain between the checkpoint and its model / CLIP users; the VAE stays.
    {
        const QJsonObject graph = QJsonDocument::fromJson(R"({
            "4": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": "animagine.safetensors"}},
            "6": {"class_type": "CLIPTextEncode", "inputs": {"text": "a", "clip": ["4", 1]}},
            "7": {"class_type": "CLIPTextEncode", "inputs": {"text": "b", "clip": ["4", 1]}},
            "3": {"class_type": "KSampler", "inputs": {"model": ["4", 0], "positive": ["6", 0], "negative": ["7", 0]}},
            "8": {"class_type": "VAEDecode", "inputs": {"samples": ["3", 0], "vae": ["4", 2]}}
        })").object();
        QList<LoraRef> loras(2);
        loras[0].name = QStringLiteral("akane_v2");
        loras[0].resolved = QStringLiteral("chars\\akane_v2.safetensors");
        loras[0].weight = 0.7;
        loras[1].name = QStringLiteral("style");
        loras[1].weight = 0.4;
        const QJsonObject out = ComfyUiProvider::insertLoras(graph, loras);
        QCOMPARE(out.size(), graph.size() + 2);
        const QJsonObject first = out.value(QStringLiteral("9")).toObject();
        const QJsonObject second = out.value(QStringLiteral("10")).toObject();
        QCOMPARE(first.value(QStringLiteral("class_type")).toString(), QStringLiteral("LoraLoader"));
        QCOMPARE(first.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("model")).toArray(), (QJsonArray{QStringLiteral("4"), 0}));
        QCOMPARE(first.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("lora_name")).toString(), QStringLiteral("chars\\akane_v2.safetensors"));
        QCOMPARE(second.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("clip")).toArray(), (QJsonArray{QStringLiteral("9"), 1}));
        QCOMPARE(second.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("lora_name")).toString(), QStringLiteral("style"));
        auto input = [&out](const char *node, const char *name) {
            return out.value(QLatin1String(node)).toObject().value(QStringLiteral("inputs")).toObject().value(QLatin1String(name)).toArray();
        };
        QCOMPARE(input("3", "model"), (QJsonArray{QStringLiteral("10"), 0}));
        QCOMPARE(input("6", "clip"), (QJsonArray{QStringLiteral("10"), 1}));
        QCOMPARE(input("7", "clip"), (QJsonArray{QStringLiteral("10"), 1}));
        QCOMPARE(input("8", "vae"), (QJsonArray{QStringLiteral("4"), 2}));
        QCOMPARE(ComfyUiProvider::insertLoras(graph, {}), graph);
    }
}

// Prompts written for the model (plan M18 §18.2): an instruction becomes what should be there for a
// model that fills an area, in its family's dialect; instruction models and new pictures keep the
// words; Use my words as they are sends exactly what was typed.
void TestProviders::promptRules()
{
    using K = Instruction::Kind;
    struct Case
    {
        QString words;
        K kind;
        QString target;
        QString result;
    };
    const QList<Case> cases{
        {QStringLiteral("replace the cup with a candle"), K::Replace, QStringLiteral("the cup"), QStringLiteral("a candle")},
        {QStringLiteral("Please turn the dog into a cat."), K::Replace, QStringLiteral("the dog"), QStringLiteral("a cat")},
        {QStringLiteral("remove the black lines"), K::Remove, QStringLiteral("the black lines"), {}},
        {QStringLiteral("remove it"), K::Remove, {}, {}},
        {QStringLiteral("get rid of the lamp"), K::Remove, QStringLiteral("the lamp"), {}},
        {QStringLiteral("make the car red"), K::Recolor, QStringLiteral("the car"), QStringLiteral("red")},
        {QStringLiteral("change the colour of the dress to dark blue"), K::Recolor, QStringLiteral("the dress"), QStringLiteral("dark blue")},
        {QStringLiteral("paint it green"), K::Recolor, {}, QStringLiteral("green")},
        {QStringLiteral("add a hat"), K::Add, {}, QStringLiteral("a hat")},
        {QStringLiteral("ersetze die Tasse durch eine Kerze"), K::Replace, QStringLiteral("die Tasse"), QStringLiteral("eine Kerze")},
        {QStringLiteral("entferne die Lampe"), K::Remove, QStringLiteral("die Lampe"), {}},
        {QStringLiteral("füge einen Hut hinzu"), K::Add, {}, QStringLiteral("einen Hut")},
        {QStringLiteral("mach das Auto rot"), K::Recolor, QStringLiteral("das Auto"), QStringLiteral("rot")},
        {QStringLiteral("a girl in a red dress"), K::None, {}, {}},
        {QStringLiteral("make it look older"), K::None, {}, {}},
    };
    for (const Case &c : cases) {
        const Instruction in = parseInstruction(c.words);
        QVERIFY2(in.kind == c.kind, qPrintable(c.words));
        QCOMPARE(in.target, c.target);
        QCOMPARE(in.result, c.result);
    }

    PromptStyle tags;
    tags.family = QStringLiteral("illustrious");
    tags.dialect = PromptStyle::Dialect::Tags;
    tags.qualityPrefix = QStringLiteral("masterpiece, best quality, ");
    tags.defaultNegative = QStringLiteral("lowres");
    PromptStyle natural;
    natural.family = QStringLiteral("flux");
    natural.supportsNegative = false;

    // An area inpainted: what should be there, as tags for a tag model.
    FittedPrompt f = fitPrompt(tags, GenMode::Inpaint, true, QStringLiteral("replace the cup with a candle"), {});
    QCOMPARE(f.prompt, QStringLiteral("masterpiece, best quality, candle"));
    QCOMPARE(f.negative, QStringLiteral("lowres"));
    QVERIFY(f.rewritten);
    QCOMPARE(fitPrompt(natural, GenMode::Inpaint, true, QStringLiteral("replace the cup with a candle"), {}).prompt, QStringLiteral("a candle"));
    QCOMPARE(fitPrompt(natural, GenMode::ImageToImage, true, QStringLiteral("make the car red"), {}).prompt, QStringLiteral("red car"));
    // Removed: an empty place, and what was there among the things to leave out.
    f = fitPrompt(tags, GenMode::Inpaint, true, QStringLiteral("remove the lamp"), QStringLiteral("blurry"));
    QCOMPARE(f.prompt, QStringLiteral("masterpiece, best quality, ") + emptyPlaceWords());
    QCOMPARE(f.negative, QStringLiteral("lamp, blurry"));
    QCOMPARE(fitPrompt(natural, GenMode::Inpaint, true, QStringLiteral("remove the lamp"), {}).negative, QString());
    // Words that are no instruction stay; so do instructions for edit models and new pictures.
    QCOMPARE(fitPrompt(tags, GenMode::Inpaint, true, QStringLiteral("1girl, red scarf"), {}).prompt,
             QStringLiteral("masterpiece, best quality, 1girl, red scarf"));
    f = fitPrompt(tags, GenMode::Edit, true, QStringLiteral("replace the cup with a candle"), {});
    QCOMPARE(f.prompt, QStringLiteral("replace the cup with a candle"));
    QVERIFY(!f.rewritten);
    QCOMPARE(fitPrompt(tags, GenMode::TextToImage, false, QStringLiteral("add a hat"), {}).prompt,
             QStringLiteral("masterpiece, best quality, add a hat"));
    QCOMPARE(fitPrompt(tags, GenMode::ImageToImage, false, QStringLiteral("add a hat"), {}).prompt,
             QStringLiteral("masterpiece, best quality, add a hat")); // a variation of the whole picture
    // Use my words as they are.
    f = fitPrompt(tags, GenMode::Inpaint, true, QStringLiteral("replace the cup with a candle"), {}, true);
    QCOMPARE(f.prompt, QStringLiteral("replace the cup with a candle"));
    QCOMPARE(f.negative, QString());
    QVERIFY(!f.rewritten);
}

QTEST_MAIN(TestProviders)
#include "tst_providers.moc"
