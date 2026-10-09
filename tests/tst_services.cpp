#include "MockHttpServer.h"

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

QTEST_MAIN(TestServices)
#include "tst_services.moc"
