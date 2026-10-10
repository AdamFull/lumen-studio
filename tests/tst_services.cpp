#include "MockHttpServer.h"

#include "ai/Agent.h"
#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/ProviderCatalog.h"
#include "ai/StructuredChat.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/JsonSchema.h"
#include "net/HttpClient.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>

using namespace lumen;
using namespace lumen::ai;

static QString pngB64(const QColor &c, const QSize &s)
{
    QImage img(s, QImage::Format_ARGB32);
    img.fill(c);
    return QString::fromLatin1(img::toBase64(img));
}

class TestServices : public QObject
{
    Q_OBJECT
private slots:
    void init() { QStandardPaths::setTestModeEnabled(true); }
    void planRegionFlexible();
    void planRegionFixedSizes();
    void regionCompositeOnlyTouchesMask();
    void structuredRepairLoop();
    void directorSchemasAcceptExample();
    void shotRequestBuildsPoseControl();
    void generationServiceRegionEndToEnd();
    void utilityOnAssets();
    void directorCritique();
    void cleanShotPrompts();
    void agentNativeTools();
    void agentJsonProtocol();

private:
    net::HttpClient m_http;
};

void TestServices::planRegionFlexible()
{
    ImageCapabilities caps;
    caps.nativePixels = 1024 * 1024;
    caps.sizeMultiple = 8;
    const RegionPlan plan = planRegion(QRect(1000, 1000, 100, 60), QSize(4096, 4096), 32, caps);
    QVERIFY(plan.rect.contains(QRect(1000, 1000, 100, 60)));
    QVERIFY(plan.rect.width() >= 384 && plan.rect.height() >= 384);
    QCOMPARE(plan.modelSize.width() % 8, 0);
    // Small regions are upscaled for detail (up to 4x).
    QVERIFY(plan.modelSize.width() > plan.rect.width());
    QVERIFY(plan.modelSize.width() <= plan.rect.width() * 4 + 8);
}

void TestServices::planRegionFixedSizes()
{
    ImageCapabilities caps;
    caps.fixedSizes = {QSize(1024, 1024), QSize(1536, 1024), QSize(1024, 1536)};
    const RegionPlan plan = planRegion(QRect(100, 100, 900, 300), QSize(2000, 2000), 20, caps);
    QCOMPARE(plan.modelSize, QSize(1536, 1024));
    QVERIFY(std::abs(double(plan.rect.width()) / plan.rect.height() - 1.5) < 0.02);
}

void TestServices::regionCompositeOnlyTouchesMask()
{
    QImage source(800, 600, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white);
    QImage mask = img::createMask(source.size());
    for (int y = 250; y < 350; ++y)
        for (int x = 350; x < 450; ++x)
            mask.scanLine(y)[x] = 255;
    RegionInput region;
    region.source = source;
    region.mask = mask;
    region.colorMatch = false;
    ImageCapabilities caps;
    const RegionPlan plan = planRegion(img::maskBounds(mask), source.size(), 48, caps);
    ImageRequest base;
    base.prompt = QStringLiteral("x");
    const ImageRequest req = prepareRegionRequest(base, region, plan, caps);
    QCOMPARE(req.mode, GenMode::ImageToImage); // caps without inpaint/edit
    QCOMPARE(req.initImage.size(), plan.modelSize);
    QCOMPARE(req.mask.size(), plan.modelSize);
    QImage generated(plan.modelSize, QImage::Format_ARGB32_Premultiplied);
    generated.fill(Qt::red);
    const QImage layer = compositeRegionResult(generated, region, plan);
    QCOMPARE(layer.size(), source.size());
    QCOMPARE(layer.pixelColor(400, 300), QColor(Qt::red));      // inside mask: replaced
    QCOMPARE(layer.pixelColor(10, 10).alpha(), 0);              // far outside: untouched
    QVERIFY(layer.pixelColor(plan.rect.left() + 2, 300).alpha() == 0); // edge of region: untouched
}

void TestServices::structuredRepairLoop()
{
    MockHttpServer server;
    int calls = 0;
    QJsonArray lastMessages;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        ++calls;
        lastMessages = req.json().value(QStringLiteral("messages")).toArray();
        const QString content = calls == 1 ? QStringLiteral("Here you go: {\"count\": \"three\"}") : QStringLiteral("```json\n{\"count\": 3}\n```");
        return MockResponse::json({{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}},
                                                                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });
    OpenAiChatProvider llm(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("l"), &m_http, nullptr);
    llm.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}});
    ChatRequest req;
    req.stream = false;
    req.messages = {ChatMessage::user(QStringLiteral("count"))};
    req.jsonSchema = QJsonDocument::fromJson(R"({"type":"object","properties":{"count":{"type":"integer"}},"required":["count"]})").object();
    StructuredChatOperation *op = requestJson(&llm, req);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().value(QStringLiteral("count")).toInt(), 3);
    QCOMPARE(calls, 2);
    QCOMPARE(op->attempts(), 2);
    QVERIFY(lastMessages.last().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("/count: expected integer")));
    delete op;
}

void TestServices::directorSchemasAcceptExample()
{
    QJsonObject shot{{QStringLiteral("id"), QStringLiteral("shot1")},
                     {QStringLiteral("title"), QStringLiteral("Front")},
                     {QStringLiteral("description"), QStringLiteral("base")},
                     {QStringLiteral("aspect_ratio"), QStringLiteral("2:3")},
                     {QStringLiteral("background"), QStringLiteral("plain grey")},
                     {QStringLiteral("prompt"), QStringLiteral("1girl, solo")},
                     {QStringLiteral("negative_prompt"), QString()}};
    QJsonObject pose = pose::RigParams().toJson();
    pose.remove(QStringLiteral("include_hands"));
    pose.remove(QStringLiteral("include_face"));
    pose.remove(QStringLiteral("name"));
    shot.insert(QStringLiteral("pose"), pose);
    const QJsonObject plan{{QStringLiteral("title"), QStringLiteral("t")}, {QStringLiteral("summary"), QStringLiteral("s")},
                           {QStringLiteral("shots"), QJsonArray{shot}}};
    json::SchemaValidator v(Director::shotPlanSchema(3));
    QStringList errors;
    QVERIFY2(v.validate(plan, &errors), qPrintable(errors.join(u'\n')));
    // Strict conversion (OpenAI) keeps it valid.
    json::SchemaValidator strict(json::toStrictSchema(Director::shotPlanSchema(3)));
    QVERIFY(strict.validate(plan));
    QVERIFY(Director::characterSheetSchema().value(QStringLiteral("required")).toArray().size() >= 10);
    QVERIFY(purposeProfile(QStringLiteral("live2d")).guidance.contains(QStringLiteral("A-pose")));
}

void TestServices::shotRequestBuildsPoseControl()
{
    QJsonObject shot{{QStringLiteral("aspect_ratio"), QStringLiteral("2:3")},
                     {QStringLiteral("prompt"), QStringLiteral("1girl, a-pose")},
                     {QStringLiteral("negative_prompt"), QStringLiteral("text")},
                     {QStringLiteral("pose"), pose::RigParams().toJson()}};
    ImageCapabilities caps;
    caps.poseControl = true;
    caps.maxReferences = 2;
    caps.nativePixels = 1024 * 1024;
    PromptStyle style;
    style.dialect = PromptStyle::Dialect::Tags;
    style.qualityPrefix = QStringLiteral("masterpiece, ");
    style.defaultNegative = QStringLiteral("lowres");
    const QSize size = Director::sizeForAspect(QStringLiteral("2:3"), caps.nativePixels, 64);
    QCOMPARE(size, QSize(832, 1280));
    const pose::Pose pose = Director::poseForShot(shot, size);
    QList<ReferenceImage> refs(3);
    const ImageRequest r = Director::buildShotRequest(shot, refs, pose, style, caps);
    QCOMPARE(r.prompt, QStringLiteral("masterpiece, 1girl, a-pose"));
    QCOMPARE(r.negativePrompt, QStringLiteral("text, lowres"));
    QCOMPARE(r.references.size(), 2);
    QCOMPARE(r.controls.size(), 1);
    QCOMPARE(r.controls[0].image.size(), r.size);
}

void TestServices::generationServiceRegionEndToEnd()
{
    MockHttpServer server;
    QJsonObject payload;
    server.route("POST", QStringLiteral("/sdapi/v1/img2img"), [&](const MockRequest &req) {
        payload = req.json();
        const int w = payload.value(QStringLiteral("width")).toInt();
        const int h = payload.value(QStringLiteral("height")).toInt();
        return MockResponse::json({{QStringLiteral("images"), QJsonArray{pngB64(Qt::green, QSize(w, h))}},
                                   {QStringLiteral("info"), QStringLiteral("{\"seed\": 5, \"all_seeds\": [5]}")}});
    });
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [](const MockRequest &) { return MockResponse::json({}); });

    QTemporaryDir dir;
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("p.lumen")));
    QTRY_VERIFY(opened.isFinished());
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    Provider *sd = registry.addProvider(QStringLiteral("sdwebui"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}});
    QVERIFY(sd);
    JobQueue queue;
    GenerationService service(&registry, &queue);
    service.setProject(&project);

    QImage source(1200, 900, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white);
    QImage mask = img::createMask(source.size());
    for (int y = 400; y < 500; ++y)
        for (int x = 500; x < 600; ++x)
            mask.scanLine(y)[x] = 255;
    GenerationSpec spec;
    spec.request.prompt = QStringLiteral("a green square");
    spec.request.denoise = 0.8;
    spec.region = RegionInput{source, mask, 48, 6, 12, false};
    spec.assetKind = doc::AssetKind::Edit;
    Job *job = service.submit(spec);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    const GenerationOutput out = GenerationService::outputOf(job);
    QCOMPARE(out.images.size(), 1);
    QCOMPARE(out.images[0].size(), source.size());
    QCOMPARE(out.images[0].pixelColor(550, 450), QColor(Qt::green));
    QCOMPARE(out.images[0].pixelColor(50, 50).alpha(), 0);
    QCOMPARE(out.assets.size(), 1);
    QCOMPARE(project.assets().size(), 1);
    QVERIFY(project.assets()[0].metadata.value(QStringLiteral("parameters")).toString().contains(QStringLiteral("Denoising strength: 0.8")));
    // Inpaint request sent at model resolution with a mask.
    QVERIFY(payload.contains(QStringLiteral("mask")));
    QCOMPARE(payload.value(QStringLiteral("width")).toInt() % 8, 0);
    QFile::remove(registry.configPath());
}

void TestServices::utilityOnAssets()
{
    MockHttpServer server;
    int calls = 0;
    server.route("POST", QStringLiteral("/rembg"), [&](const MockRequest &req) {
        ++calls;
        const QImage in = img::fromBase64(req.json().value(QStringLiteral("input_image")).toString().toLatin1());
        QImage out(in.size(), QImage::Format_ARGB32);
        out.fill(Qt::transparent);
        return MockResponse::json({{QStringLiteral("image"), QString::fromLatin1(img::encode(out).toBase64())}});
    });

    QTemporaryDir dir;
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("p.lumen")));
    QTRY_VERIFY(opened.isFinished());
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    JobQueue queue;
    GenerationService service(&registry, &queue);
    service.setProject(&project);

    QImage white(40, 30, QImage::Format_ARGB32_Premultiplied);
    white.fill(Qt::white);
    auto added = project.addAsset(white, doc::AssetKind::Import, QStringLiteral("photo"));
    QTRY_VERIFY(added.isFinished());
    const QString source = added.result().id;

    // Without a capable provider the job fails with a clear message.
    Job *none = service.submitUtilityForAsset(QString(), Utility::RemoveBackground, source, {}, QStringLiteral("bg"));
    QTRY_VERIFY_WITH_TIMEOUT(none->isFinished(), 5000);
    QCOMPARE(none->state(), Job::State::Failed);

    QVERIFY(registry.addProvider(QStringLiteral("sdwebui"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    Job *job = service.submitUtilityForAsset(QString(), Utility::RemoveBackground, source, {}, QStringLiteral("Remove background: photo"));
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    QCOMPARE(calls, 1);
    const GenerationOutput out = GenerationService::outputOf(job);
    QCOMPARE(out.assets.size(), 1);
    QCOMPARE(out.assets[0].parentId, source);
    QCOMPARE(out.assets[0].metadata.value(QStringLiteral("source")).toString(), source);
    QCOMPARE(out.assets[0].size, QSize(40, 30));
    QCOMPARE(project.assets().size(), 2);

    // A missing asset fails the job instead of calling the provider.
    Job *missing = service.submitUtilityForAsset(QString(), Utility::RemoveBackground, QStringLiteral("nope"), {}, QStringLiteral("bg"));
    QTRY_VERIFY_WITH_TIMEOUT(missing->isFinished(), 5000);
    QCOMPARE(missing->state(), Job::State::Failed);
    QCOMPARE(calls, 1);
    QFile::remove(registry.configPath());
}

void TestServices::directorCritique()
{
    MockHttpServer server;
    QJsonObject request;
    const QJsonObject verdict{{QStringLiteral("overall"), 5},
                              {QStringLiteral("identity"), 8},
                              {QStringLiteral("pose"), 4},
                              {QStringLiteral("issues"), QJsonArray{QStringLiteral("left hand has six fingers"), QStringLiteral("arms too low")}},
                              {QStringLiteral("revised_prompt"), QStringLiteral("1girl, a-pose, arms raised 35 degrees, detailed hands")},
                              {QStringLiteral("negative_additions"), QStringLiteral("extra fingers, Text")}};
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        request = req.json();
        const QString content = QString::fromUtf8(QJsonDocument(verdict).toJson(QJsonDocument::Compact));
        return MockResponse::json({{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}},
                                                                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    QVERIFY(registry.addProvider(QStringLiteral("llamacpp"), QStringLiteral("Vision"),
                                 {{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}}));
    Director director(&registry);
    const QJsonObject shot{{QStringLiteral("id"), QStringLiteral("front")},
                           {QStringLiteral("title"), QStringLiteral("Front A-pose")},
                           {QStringLiteral("prompt"), QStringLiteral("1girl, a-pose")},
                           {QStringLiteral("negative_prompt"), QStringLiteral("text, watermark")}};
    const QJsonObject character{{QStringLiteral("name"), QStringLiteral("Mira")}, {QStringLiteral("summary"), QStringLiteral("red-haired mage")}};
    QImage result(300, 400, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::red);
    QList<QImage> refs{result, result, result};
    StructuredChatOperation *op = director.critiqueShot(result, shot, character, refs);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result(), verdict);
    delete op;
    // The request carries the result and at most two references, with the shot and the sheet as text.
    const QJsonArray messages = request.value(QStringLiteral("messages")).toArray();
    const QJsonArray parts = messages.last().toObject().value(QStringLiteral("content")).toArray();
    int images = 0;
    QString text;
    for (const QJsonValue &p : parts) {
        if (p.toObject().value(QStringLiteral("type")).toString() == QLatin1String("image_url"))
            ++images;
        else
            text += p.toObject().value(QStringLiteral("text")).toString();
    }
    QCOMPARE(images, 3);
    QVERIFY(text.contains(QStringLiteral("Front A-pose")));
    QVERIFY(text.contains(QStringLiteral("Mira")));

    // Scoring and the retry shot.
    json::SchemaValidator validator(Director::critiqueSchema());
    QVERIFY(validator.validate(verdict));
    QCOMPARE(Director::critiqueScore(verdict), 4); // the weakest of overall, identity and pose
    QVERIFY(!Director::critiqueAccepts(verdict, 7));
    QVERIFY(Director::critiqueAccepts(verdict, 4));
    const QJsonObject retry = Director::applyCritique(shot, verdict);
    QCOMPARE(retry.value(QStringLiteral("prompt")).toString(), verdict.value(QStringLiteral("revised_prompt")).toString());
    QCOMPARE(retry.value(QStringLiteral("negative_prompt")).toString(), QStringLiteral("text, watermark, extra fingers"));
    QCOMPARE(retry.value(QStringLiteral("id")).toString(), QStringLiteral("front"));
    // Negations and repeats from a looping model are dropped.
    QJsonObject looping = verdict;
    looping.insert(QStringLiteral("revised_prompt"), QStringLiteral("1girl, a-pose, no artifacts, red hair, no background, a-pose, without hat, no"));
    looping.insert(QStringLiteral("negative_additions"), QStringLiteral("a, b, c, d, e, f, g, h, i, j, orn\uFFFD"));
    const QJsonObject retried = Director::applyCritique(shot, looping);
    QCOMPARE(retried.value(QStringLiteral("prompt")).toString(), QStringLiteral("1girl, a-pose, red hair"));
    QCOMPARE(retried.value(QStringLiteral("negative_prompt")).toString(), QStringLiteral("text, watermark, a, b, c, d, e, f, g, h"));
    // An empty revision keeps the prompt.
    QCOMPARE(Director::applyCritique(shot, QJsonObject{}).value(QStringLiteral("prompt")).toString(), QStringLiteral("1girl, a-pose"));
    QFile::remove(registry.configPath());
}

namespace {

ToolSet testTools(int *calls)
{
    ToolSet tools;
    tools.add(ToolDefinition{QStringLiteral("add_numbers"), QStringLiteral("Adds two numbers."),
                             QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                         {QStringLiteral("properties"), QJsonObject{{QStringLiteral("a"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
                                                                                    {QStringLiteral("b"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}}}}},
              [calls](const QJsonObject &args) {
                  ++*calls;
                  ToolResult r;
                  r.data.insert(QStringLiteral("sum"), args.value(QStringLiteral("a")).toDouble() + args.value(QStringLiteral("b")).toDouble());
                  return QtFuture::makeReadyValueFuture(r);
              });
    tools.add(ToolDefinition{QStringLiteral("make_image"), QStringLiteral("Makes an image."), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
              [calls](const QJsonObject &) {
                  ++*calls;
                  ToolResult r;
                  r.assetIds << QStringLiteral("asset1");
                  return QtFuture::makeReadyValueFuture(r);
              });
    return tools;
}

MockResponse chatReply(const QJsonObject &message, const QString &finish)
{
    return MockResponse::json({{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), message},
                                                                                 {QStringLiteral("finish_reason"), finish}}}}});
}

} // namespace

void TestServices::agentNativeTools()
{
    MockHttpServer server;
    QList<QJsonObject> requests;
    int mode = 0; // 0: tools then reply, 1: always call a tool, 2: unknown tool then reply
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        requests << req.json();
        const int n = int(requests.size());
        auto call = [](const QString &id, const QString &name, const QString &args) {
            return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
                               {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), args}}}};
        };
        const bool callTools = mode == 1 || n == 1;
        if (callTools) {
            const QJsonArray calls = mode == 2 ? QJsonArray{call(QStringLiteral("c1"), QStringLiteral("fly"), QStringLiteral("{}"))}
                                               : QJsonArray{call(QStringLiteral("c1"), QStringLiteral("add_numbers"), QStringLiteral("{\"a\":2,\"b\":3}")),
                                                            call(QStringLiteral("c2"), QStringLiteral("make_image"), QStringLiteral("{}"))};
            return chatReply(QJsonObject{{QStringLiteral("content"), QJsonValue()}, {QStringLiteral("tool_calls"), calls}}, QStringLiteral("tool_calls"));
        }
        return chatReply(QJsonObject{{QStringLiteral("content"), QStringLiteral("Done: 5")}}, QStringLiteral("stop"));
    });
    OpenAiChatProvider llm(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("l"), &m_http, nullptr);
    llm.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}});
    QVERIFY(llm.capabilities().tools);
    int calls = 0;
    const QList<ChatMessage> history{ChatMessage::system(QStringLiteral("You are a test.")), ChatMessage::user(QStringLiteral("add 2 and 3"))};

    auto *run = new AgentRun(&llm, QString(), testTools(&calls), history);
    run->setAutoDelete(false);
    QSignalSpy started(run, &AgentRun::toolStarted);
    QSignalSpy finished(run, &AgentRun::toolFinished);
    run->start();
    QTRY_VERIFY_WITH_TIMEOUT(run->isFinished(), 5000);
    QVERIFY2(run->state() == Operation::State::Succeeded, qPrintable(run->errorString()));
    QVERIFY(run->usesNativeTools());
    QCOMPARE(run->reply(), QStringLiteral("Done: 5"));
    QCOMPARE(run->producedAssets(), QStringList{QStringLiteral("asset1")});
    QCOMPARE(calls, 2);
    QCOMPARE(started.size(), 2);
    QCOMPARE(finished.size(), 2);
    QCOMPARE(run->steps(), 2);
    // Tools are declared, and the second request carries both tool results.
    QCOMPARE(requests[0].value(QStringLiteral("tools")).toArray().size(), 2);
    const QJsonArray second = requests[1].value(QStringLiteral("messages")).toArray();
    QStringList toolContents;
    for (const QJsonValue &m : second) {
        if (m.toObject().value(QStringLiteral("role")).toString() == QLatin1String("tool"))
            toolContents << m.toObject().value(QStringLiteral("content")).toString();
    }
    QCOMPARE(toolContents.size(), 2);
    QVERIFY(toolContents[0].contains(QLatin1String("\"sum\":5")));
    QVERIFY(toolContents[1].contains(QLatin1String("asset1")));
    QCOMPARE(run->messages().size(), history.size() + 4); // assistant (calls), 2 results, final answer
    delete run;

    // The step limit ends a run that never stops calling tools.
    requests.clear();
    mode = 1;
    run = new AgentRun(&llm, QString(), testTools(&calls), history, 3);
    run->setAutoDelete(false);
    run->start();
    QTRY_VERIFY_WITH_TIMEOUT(run->isFinished(), 5000);
    QCOMPARE(run->state(), Operation::State::Succeeded);
    QCOMPARE(int(requests.size()), 3);
    QVERIFY(run->reply().contains(QLatin1String("3")));
    delete run;

    // An unknown tool is reported back to the model, which then answers.
    requests.clear();
    mode = 2;
    run = new AgentRun(&llm, QString(), testTools(&calls), history);
    run->setAutoDelete(false);
    run->start();
    QTRY_VERIFY_WITH_TIMEOUT(run->isFinished(), 5000);
    QCOMPARE(run->state(), Operation::State::Succeeded);
    QVERIFY(QJsonDocument(requests[1]).toJson().contains("unknown tool"));
    delete run;

    // Cancelling stops the run.
    mode = 1;
    run = new AgentRun(&llm, QString(), testTools(&calls), history, 50);
    run->setAutoDelete(false);
    run->start();
    QTRY_VERIFY_WITH_TIMEOUT(requests.size() >= 4, 5000);
    run->cancel();
    QCOMPARE(run->state(), Operation::State::Cancelled);
    delete run;

    // Without a provider the run fails with a clear message.
    run = new AgentRun(nullptr, QString(), testTools(&calls), history);
    run->setAutoDelete(false);
    run->start();
    QTRY_VERIFY(run->isFinished());
    QCOMPARE(run->state(), Operation::State::Failed);
    delete run;
}

void TestServices::agentJsonProtocol()
{
    MockHttpServer server;
    QList<QJsonObject> requests;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        requests << req.json();
        const QString content = requests.size() == 1
                                    ? QStringLiteral("{\"action\": \"add_numbers\", \"arguments\": {\"a\": 1, \"b\": 1}}")
                                    : QStringLiteral("{\"action\": \"reply\", \"reply\": \"It is 2.\"}");
        return chatReply(QJsonObject{{QStringLiteral("content"), content}}, QStringLiteral("stop"));
    });
    OpenAiChatProvider llm(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("l"), &m_http, nullptr);
    llm.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}, {QStringLiteral("tools"), false}});
    QVERIFY(!llm.capabilities().tools);
    int calls = 0;
    auto *run = new AgentRun(&llm, QString(), testTools(&calls), {ChatMessage::user(QStringLiteral("1+1?"))});
    run->setAutoDelete(false);
    QSignalSpy text(run, &AgentRun::textDelta);
    run->start();
    QTRY_VERIFY_WITH_TIMEOUT(run->isFinished(), 5000);
    QVERIFY2(run->state() == Operation::State::Succeeded, qPrintable(run->errorString()));
    QVERIFY(!run->usesNativeTools());
    QCOMPARE(run->reply(), QStringLiteral("It is 2."));
    QCOMPARE(calls, 1);
    QCOMPARE(text.size(), 1);
    // The protocol is described in a system message; no native tools are sent.
    const QJsonArray first = requests[0].value(QStringLiteral("messages")).toArray();
    QCOMPARE(first[0].toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
    QVERIFY(first[0].toObject().value(QStringLiteral("content")).toString().contains(QLatin1String("add_numbers")));
    QVERIFY(!requests[0].contains(QStringLiteral("tools")));
    QVERIFY(QJsonDocument(requests[1]).toJson().contains("Result of add_numbers"));
    // The transcript ends with the plain reply.
    QCOMPARE(run->messages().last().text, QStringLiteral("It is 2."));
    delete run;
}

void TestServices::cleanShotPrompts()
{
    // A real llama.cpp plan copied rig parameters into the prompt.
    const QString dirty = QStringLiteral("1girl, red hair, blue robe, full body, A-pose, arms raise 35 direction 90 bend 0 bend_twist 0, "
                                         "left_arm raise: 40, yaw=-15 degrees, hands open,, eyes open");
    QCOMPARE(Director::cleanPrompt(dirty),
             QStringLiteral("1girl, red hair, blue robe, full body, A-pose, arms, hands open, eyes open"));
    // Ordinary text and numbers are kept.
    QCOMPARE(Director::cleanPrompt(QStringLiteral("2girls, 1boy, holding 3 apples, raised hand, bending forward")),
             QStringLiteral("2girls, 1boy, holding 3 apples, raised hand, bending forward"));
    const QJsonObject plan{{QStringLiteral("title"), QStringLiteral("t")},
                           {QStringLiteral("shots"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("a")},
                                                                            {QStringLiteral("prompt"), dirty},
                                                                            {QStringLiteral("description"), QStringLiteral("front, yaw 0")},
                                                                            {QStringLiteral("pose"), QJsonObject{{QStringLiteral("yaw"), 0}}}}}}};
    const QJsonObject clean = Director::sanitizePlan(plan);
    const QJsonObject shot = clean.value(QStringLiteral("shots")).toArray().first().toObject();
    QVERIFY(!shot.value(QStringLiteral("prompt")).toString().contains(QLatin1String("raise")));
    QCOMPARE(shot.value(QStringLiteral("description")).toString(), QStringLiteral("front"));
    QCOMPARE(shot.value(QStringLiteral("pose")).toObject().value(QStringLiteral("yaw")).toInt(), 0); // the rig itself is untouched
    QCOMPARE(clean.value(QStringLiteral("title")).toString(), QStringLiteral("t"));
}

QTEST_MAIN(TestServices)
#include "tst_services.moc"
