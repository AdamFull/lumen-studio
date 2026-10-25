#include "MockHttpServer.h"

#include "ai/Agent.h"
#include "ai/CastGeneration.h"
#include "ai/providers/SdCppProvider.h"
#include "ai/CharacterContext.h"
#include "ai/Director.h"
#include "ai/Gate.h"
#include "ai/LoraCaptions.h"
#include "ai/ModelDefaults.h"
#include "ai/GenerationService.h"
#include "ai/ProviderCatalog.h"
#include "ai/StructuredChat.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/JsonSchema.h"
#include "net/HttpClient.h"
#include "pose/Rig.h"

#include <QJsonArray>
#include <QSet>
#include <QPromise>
#include <QTimer>
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
    void regionEditMode();
    void localRegionEditEndToEnd();
    void utilityOnAssets();
    void directorCritique();
    void directorDatasetPlan();
    void loraCaptions();
    void identityChecklist();
    void characterContext();
    void castGeneration();
    void castCheck();
    void cleanShotPrompts();
    void agentNativeTools();
    void agentJsonProtocol();
    void providerGate();

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

// A selection stays an instruction edit of its crop for local edit models; inpainting otherwise.
void TestServices::regionEditMode()
{
    QImage source(400, 300, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white);
    QImage mask = img::createMask(source.size());
    mask.fill(0);
    for (int y = 100; y < 200; ++y)
        for (int x = 150; x < 250; ++x)
            mask.scanLine(y)[x] = 255;
    const RegionInput region{source, mask, 32, 6, 12, false};
    ImageCapabilities local;
    local.inpaint = local.edit = local.regionEdit = true;
    ImageCapabilities cloud;
    cloud.inpaint = cloud.edit = true;
    const RegionPlan plan = planRegion(img::maskBounds(mask), source.size(), 32, local);
    ImageRequest edit;
    edit.mode = GenMode::Edit;
    QCOMPARE(prepareRegionRequest(edit, region, plan, local).mode, GenMode::Edit);
    QCOMPARE(prepareRegionRequest(edit, region, plan, cloud).mode, GenMode::Inpaint);
    ImageRequest fill;
    fill.mode = GenMode::Inpaint;
    QCOMPARE(prepareRegionRequest(fill, region, plan, local).mode, GenMode::Inpaint);
}

// "Colorize the selection" with FLUX.2 klein through sd.cpp: an instruction edit of the crop with
// the preset's steps and CFG, and not one pixel outside the feathered selection changes.
void TestServices::localRegionEditEndToEnd()
{
    MockHttpServer server;
    QJsonObject payload;
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        payload = req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [&](const MockRequest &) {
        const QSize size(payload.value(QStringLiteral("width")).toInt(), payload.value(QStringLiteral("height")).toInt());
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngB64(QColor(255, 128, 0), size)}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });

    QTemporaryDir dir;
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("p.lumen")));
    QTRY_VERIFY(opened.isFinished());
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    ModelDefaults klein;
    klein.id = QStringLiteral("flux2-klein-4b");
    klein.cfg = 1.0;
    klein.steps = 4;
    klein.sampler = QStringLiteral("euler");
    klein.nativeResolution = 1024;
    klein.edit = true;
    klein.references = 4;
    klein.negativePrompt = false;
    auto *sd = qobject_cast<SdCppProvider *>(registry.addProvider(
        QStringLiteral("sdcpp"), QStringLiteral("Klein"),
        {{QStringLiteral("url"), server.baseUrl().toString()}, {QStringLiteral("model"), QStringLiteral("flux-2-klein-4b.safetensors")},
         {QStringLiteral("model_preset"), klein.toVariant()}}));
    QVERIFY(sd);
    sd->pollIntervalMs = 20;
    JobQueue queue;
    GenerationService service(&registry, &queue);
    service.setProject(&project);

    // A photo with detail everywhere, so any change outside the selection would show.
    QImage source(1200, 900, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, QColor((x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256));
    QImage mask = img::createMask(source.size());
    mask.fill(0);
    for (int y = 300; y < 520; ++y)
        for (int x = 400; x < 700; ++x)
            mask.scanLine(y)[x] = 255;
    GenerationSpec spec;
    spec.request.mode = GenMode::Edit;
    spec.request.prompt = QStringLiteral("colorize it, keep everything else");
    spec.region = RegionInput{source, mask, 48, 6, 12, false};
    spec.assetKind = doc::AssetKind::Edit;
    Job *job = service.submit(spec);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));

    // The request: the crop as the edit's source, with the preset's steps, CFG and sampler.
    QCOMPARE(payload.value(QStringLiteral("ref_images")).toArray().size(), 1);
    QVERIFY(!payload.contains(QStringLiteral("init_image")));
    QVERIFY(!payload.contains(QStringLiteral("mask_image")));
    const QJsonObject sample = payload.value(QStringLiteral("sample_params")).toObject();
    QCOMPARE(sample.value(QStringLiteral("sample_steps")).toInt(), 4);
    QCOMPARE(sample.value(QStringLiteral("guidance")).toObject().value(QStringLiteral("txt_cfg")).toDouble(), 1.0);
    QCOMPARE(sample.value(QStringLiteral("sample_method")).toString(), QStringLiteral("euler"));
    QCOMPARE(payload.value(QStringLiteral("width")).toInt() % 64, 0);

    // The result layer: the edit inside the selection, nothing outside it plus its feathered edge.
    // The edge is grown by grow + feather / 2 and blurred by three box passes of feather / 2, so for
    // this rectangle it ends 6 + 6 + 18 = 30 px out.
    const GenerationOutput out = GenerationService::outputOf(job);
    QCOMPARE(out.images.size(), 1);
    const QImage layer = out.images[0];
    QCOMPARE(layer.size(), source.size());
    QCOMPARE(layer.pixelColor(550, 410), QColor(255, 128, 0));
    const QRect reach = QRect(400, 300, 300, 220).adjusted(-30, -30, 30, 30);
    int changed = 0;
    for (int y = 0; y < layer.height(); ++y)
        for (int x = 0; x < layer.width(); ++x)
            if (!reach.contains(x, y) && qAlpha(layer.pixel(x, y)) != 0)
                ++changed;
    QCOMPARE(changed, 0);
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

void TestServices::castGeneration()
{
    // A mock stable-diffusion.cpp server: the base image is grey; an inpaint keeps its input
    // outside the mask and paints the mask red for Aki's LoRA, blue for Bo's (so the colour match
    // of the region pipeline, which looks outside the mask, changes nothing).
    MockHttpServer server;
    QList<QJsonObject> requests;
    QHash<QString, QImage> results;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("loras"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("lmn_aki")},
                                                                                    {QStringLiteral("path"), QStringLiteral("lmn_aki.safetensors")}},
                                                                        QJsonObject{{QStringLiteral("name"), QStringLiteral("lmn_bo")},
                                                                                    {QStringLiteral("path"), QStringLiteral("lmn_bo.safetensors")}}}}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        const QJsonObject body = req.json();
        requests << body;
        const QSize size(body.value(QStringLiteral("width")).toInt(), body.value(QStringLiteral("height")).toInt());
        QImage out(size, QImage::Format_ARGB32);
        out.fill(QColor(128, 128, 128));
        if (body.contains(QStringLiteral("mask_image"))) {
            out = img::fromBase64(body.value(QStringLiteral("init_image")).toString().toLatin1()).convertToFormat(QImage::Format_ARGB32);
            const QImage mask = img::fromBase64(body.value(QStringLiteral("mask_image")).toString().toLatin1()).convertToFormat(QImage::Format_Grayscale8);
            const QString lora = body.value(QStringLiteral("lora")).toArray().first().toObject().value(QStringLiteral("path")).toString();
            const QRgb colour = lora.contains(QStringLiteral("aki")) ? qRgb(255, 0, 0) : qRgb(0, 0, 255);
            for (int y = 0; y < out.height(); ++y) {
                auto *px = reinterpret_cast<QRgb *>(out.scanLine(y));
                const uchar *m = mask.constScanLine(y);
                for (int x = 0; x < out.width(); ++x)
                    if (m[x] > 127)
                        px[x] = colour;
            }
        }
        const QString id = QStringLiteral("job_%1").arg(requests.size());
        results.insert(id, out);
        return MockResponse::json({{QStringLiteral("id"), id}}, 202);
    });
    for (int i = 1; i <= 4; ++i) {
        const QString id = QStringLiteral("job_%1").arg(i);
        server.route("GET", QStringLiteral("/sdcpp/v1/jobs/") + id, [&results, id](const MockRequest &) {
            const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(results.value(id)).toBase64())}}};
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")},
                                       {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
        });
    }

    QTemporaryDir dir;
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("cast.lumen")));
    QTRY_VERIFY(opened.isFinished());
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    Provider *sd = registry.addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"),
                                        {{QStringLiteral("url"), server.baseUrl().toString()},
                                         {QStringLiteral("model"), QStringLiteral("waiIllustriousSDXL_v170.safetensors")},
                                         {QStringLiteral("control_net"), true}});
    QVERIFY(sd);
    qobject_cast<SdCppProvider *>(sd)->pollIntervalMs = 20;
    auto cleanup = qScopeGuard([&] { QFile::remove(registry.configPath()); });
    JobQueue queue;
    GenerationService service(&registry, &queue);
    service.setProject(&project);

    auto character = [](const char *id, const char *name, const char *gender, const char *tags, const char *lora) {
        CharacterContext c;
        c.id = QLatin1String(id);
        c.name = QLatin1String(name);
        c.sheet = QJsonObject{{QStringLiteral("gender_presentation"), QLatin1String(gender)}, {QStringLiteral("tags"), QLatin1String(tags)}};
        c.lora.file = QStringLiteral("C:/models/loras/%1.safetensors").arg(QLatin1String(lora));
        c.lora.trigger = QLatin1String(lora);
        c.lora.weight = 0.8;
        c.lora.family = QStringLiteral("SDXL");
        return c;
    };
    CastRequest request;
    request.scene.prompt = QStringLiteral("sharing an umbrella in the rain");
    request.scene.size = QSize(640, 448); // small: PNG coding dominates the test's time
    request.scene.seed = 100;
    request.cast = {character("aki", "Aki", "female", "1girl, solo, red hair, green eyes, school uniform", "lmn_aki"),
                    character("bo", "Bo", "male", "1boy, black hair, blue eyes, hoodie", "lmn_bo")};
    request.modelFamily = QStringLiteral("SDXL");
    QCOMPARE(castCountTags(request.cast), QStringLiteral("1boy, 1girl"));
    QCOMPARE(castSubject(request.cast[0].sheet), QStringLiteral("girl"));
    QCOMPARE(castCountTags({request.cast[0], request.cast[0]}), QStringLiteral("2girls"));

    Job *job = startCastGeneration(&service, request);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 20000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    QCOMPARE(requests.size(), 3);

    // The base: both people's pose, both appearances tied to their sides, the count tags; no LoRA.
    const QJsonObject base = requests[0];
    const QString basePrompt = base.value(QStringLiteral("prompt")).toString();
    QVERIFY2(basePrompt.contains(QStringLiteral("1boy, 1girl, sharing an umbrella in the rain")), qPrintable(basePrompt));
    QVERIFY(basePrompt.contains(QStringLiteral("on the left: red hair, green eyes, school uniform")));
    QVERIFY(basePrompt.contains(QStringLiteral("on the right: black hair, blue eyes, hoodie")));
    QVERIFY(!basePrompt.contains(QStringLiteral("solo")));
    QVERIFY(!basePrompt.contains(QStringLiteral("lmn_")));
    QVERIFY(!base.contains(QStringLiteral("lora")));
    QVERIFY(!base.contains(QStringLiteral("mask_image")));
    QCOMPARE(base.value(QStringLiteral("seed")).toDouble(), 100.0);
    const QImage baseControl = img::fromBase64(base.value(QStringLiteral("control_image")).toString().toLatin1());
    auto skeletonIn = [](const QImage &map, const QRect &area) {
        for (int y = area.top(); y <= area.bottom(); y += 2)
            for (int x = area.left(); x <= area.right(); x += 2)
                if (qGray(map.pixel(x, y)) > 40)
                    return true;
        return false;
    };
    const int w = baseControl.width(), h = baseControl.height();
    QVERIFY(skeletonIn(baseControl, QRect(0, 0, w / 2 - 1, h - 1)));
    QVERIFY(skeletonIn(baseControl, QRect(w / 2, 0, w / 2 - 1, h - 1)));

    // The refine passes: one character each, its LoRA and trigger, its look only, an inpaint with
    // its own person's pose.
    for (int k = 1; k <= 2; ++k) {
        const QJsonObject r = requests[k];
        const bool aki = k == 1; // side by side, same depth: in cast order
        const QString prompt = r.value(QStringLiteral("prompt")).toString();
        QVERIFY2(prompt.startsWith(aki ? QStringLiteral("lmn_aki") : QStringLiteral("lmn_bo")), qPrintable(prompt));
        QVERIFY(prompt.contains(aki ? QStringLiteral("1girl, red hair, green eyes, school uniform, sharing an umbrella")
                                    : QStringLiteral("1boy, black hair, blue eyes, hoodie, sharing an umbrella")));
        QVERIFY(!prompt.contains(aki ? QStringLiteral("black hair") : QStringLiteral("red hair")));
        const QJsonArray loras = r.value(QStringLiteral("lora")).toArray();
        QCOMPARE(loras.size(), 1);
        QCOMPARE(loras[0].toObject().value(QStringLiteral("path")).toString(), aki ? QStringLiteral("lmn_aki.safetensors") : QStringLiteral("lmn_bo.safetensors"));
        QCOMPARE(loras[0].toObject().value(QStringLiteral("multiplier")).toDouble(), 0.8);
        QVERIFY(r.contains(QStringLiteral("mask_image")));
        QCOMPARE(r.value(QStringLiteral("strength")).toDouble(), 0.55);
        QCOMPARE(r.value(QStringLiteral("seed")).toDouble(), aki ? 101.0 : 102.0);
        QVERIFY(r.contains(QStringLiteral("control_image")));
    }

    // The result: each character's colour in its own region, the scene elsewhere.
    const GenerationOutput out = GenerationService::outputOf(job);
    QCOMPARE(out.images.size(), 1);
    const QImage result = out.images.first();
    QCOMPARE(result.size(), QSize(640, 448));
    pose::Pose people = castPose(request, result.size());
    auto colourAt = [&](int person, int joint) {
        return result.pixelColor(people.people[person].body[static_cast<size_t>(joint)].pos.toPoint());
    };
    QCOMPARE(colourAt(0, pose::Neck), QColor(Qt::red));
    QCOMPARE(colourAt(1, pose::Neck), QColor(Qt::blue));
    QCOMPARE(colourAt(0, pose::Nose), QColor(Qt::red));
    QCOMPARE(result.pixelColor(2, 2), QColor(128, 128, 128));

    // Stored once, with the cast for Re-run.
    QCOMPARE(out.assets.size(), 1);
    QCOMPARE(project.assets().size(), 1);
    const QJsonObject meta = project.assets()[0].metadata;
    QCOMPARE(meta.value(QStringLiteral("user_prompt")).toString(), QStringLiteral("sharing an umbrella in the rain"));
    const QJsonObject cast = meta.value(QStringLiteral("cast")).toObject();
    QCOMPARE(cast.value(QStringLiteral("arrangement")).toString(), QStringLiteral("side_by_side"));
    QCOMPARE(cast.value(QStringLiteral("refine_denoise")).toDouble(), 0.55);
    QCOMPARE(cast.value(QStringLiteral("pose")).toObject().value(QStringLiteral("people")).toArray().size(), 2);
    const QJsonArray members = cast.value(QStringLiteral("members")).toArray();
    QCOMPARE(members.size(), 2);
    QCOMPARE(members[0].toObject().value(QStringLiteral("character")).toString(), QStringLiteral("aki"));
    QCOMPARE(members[0].toObject().value(QStringLiteral("applied")).toObject().value(QStringLiteral("lora")).toString(),
             QStringLiteral("C:/models/loras/lmn_aki.safetensors"));
    QCOMPARE(members[1].toObject().value(QStringLiteral("seed")).toDouble(), 102.0);
    QCOMPARE(members[1].toObject().value(QStringLiteral("region")).toArray().size(), 4);

    // One in front: the one behind (Bo) is drawn first, the one in front last.
    requests.clear();
    results.clear();
    request.arrangement = pose::Arrangement::FrontBack;
    request.saveAssets = false;
    job = startCastGeneration(&service, request);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 20000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    QCOMPARE(requests.size(), 3);
    QVERIFY(requests[0].value(QStringLiteral("prompt")).toString().contains(QStringLiteral("in front: red hair")));
    QVERIFY(requests[1].value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("lmn_bo")));
    QVERIFY(requests[2].value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("lmn_aki")));
    QCOMPARE(project.assets().size(), 1); // not stored

    // A back-end that cannot inpaint cannot do this.
    CastRequest none = request;
    none.cast.clear();
    job = startCastGeneration(&service, none);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 5000);
    QCOMPARE(job->state(), Job::State::Failed);
}

void TestServices::castCheck()
{
    // Verdicts from the two scores.
    QCOMPARE(castVerdict(9, 1, 6), QStringLiteral("ok"));
    QCOMPARE(castVerdict(5, 0, 6), QStringLiteral("weak"));
    QCOMPARE(castVerdict(2, 8, 6), QStringLiteral("swap"));
    QCOMPARE(castVerdict(9, 8, 6), QStringLiteral("blend"));
    QCOMPARE(castVerdict(-1, 8, 6), QStringLiteral("unchecked"));
    QCOMPARE(castVerdict(9, -1, 6), QStringLiteral("ok")); // one member: no other
    // What sets a member apart: shared tags (the uniform) and subject tags go.
    const QJsonObject akiSheet{{QStringLiteral("gender_presentation"), QStringLiteral("female")},
                               {QStringLiteral("tags"), QStringLiteral("1girl, red hair, green eyes, school_uniform")}};
    const QJsonObject boSheet{{QStringLiteral("gender_presentation"), QStringLiteral("male")},
                              {QStringLiteral("tags"), QStringLiteral("1boy, solo, black hair, blue eyes, school uniform")}};
    QCOMPARE(castDistinguishingSheet(boSheet, akiSheet).value(QStringLiteral("tags")).toString(), QStringLiteral("black hair, blue eyes"));
    QCOMPARE(castDistinguishingSheet(akiSheet, boSheet).value(QStringLiteral("tags")).toString(), QStringLiteral("red hair, green eyes"));

    // Mock sd.cpp: an inpaint paints the mask in the colour the scenario gives the LoRA.
    MockHttpServer sdServer;
    QList<QJsonObject> sdRequests;
    QHash<QString, QImage> results;
    std::function<QRgb(const QString &lora, int akiPass)> paint;
    int akiPasses = 0;
    sdServer.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("loras"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("lmn_aki")},
                                                                                    {QStringLiteral("path"), QStringLiteral("lmn_aki.safetensors")}},
                                                                        QJsonObject{{QStringLiteral("name"), QStringLiteral("lmn_bo")},
                                                                                    {QStringLiteral("path"), QStringLiteral("lmn_bo.safetensors")}}}}});
    });
    sdServer.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        const QJsonObject body = req.json();
        sdRequests << body;
        const QSize size(body.value(QStringLiteral("width")).toInt(), body.value(QStringLiteral("height")).toInt());
        QImage out(size, QImage::Format_ARGB32);
        out.fill(QColor(128, 128, 128));
        if (body.contains(QStringLiteral("mask_image"))) {
            out = img::fromBase64(body.value(QStringLiteral("init_image")).toString().toLatin1()).convertToFormat(QImage::Format_ARGB32);
            const QImage mask = img::fromBase64(body.value(QStringLiteral("mask_image")).toString().toLatin1()).convertToFormat(QImage::Format_Grayscale8);
            const QString lora = body.value(QStringLiteral("lora")).toArray().first().toObject().value(QStringLiteral("path")).toString();
            const QRgb colour = paint(lora, lora.contains(QStringLiteral("aki")) ? ++akiPasses : 0);
            for (int y = 0; y < out.height(); ++y) {
                auto *px = reinterpret_cast<QRgb *>(out.scanLine(y));
                const uchar *m = mask.constScanLine(y);
                for (int x = 0; x < out.width(); ++x)
                    if (m[x] > 127)
                        px[x] = colour;
            }
        }
        const QString id = QStringLiteral("job_%1").arg(sdRequests.size());
        results.insert(id, out);
        return MockResponse::json({{QStringLiteral("id"), id}}, 202);
    });
    for (int i = 1; i <= 12; ++i) {
        const QString id = QStringLiteral("job_%1").arg(i);
        sdServer.route("GET", QStringLiteral("/sdcpp/v1/jobs/") + id, [&results, id](const MockRequest &) {
            const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(results.value(id)).toBase64())}}};
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")},
                                       {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
        });
    }

    // Mock vision model: a feature matches when the crop's colour is the colour of the character
    // the checklist describes (red: Aki, blue: Bo, magenta: both).
    MockHttpServer llm;
    QStringList checks; // "<checklist owner>:<crop colour>"
    llm.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        const QJsonArray parts = req.json().value(QStringLiteral("messages")).toArray().last().toObject().value(QStringLiteral("content")).toArray();
        QString text;
        QImage crop;
        for (const QJsonValue &p : parts) {
            const QJsonObject o = p.toObject();
            if (o.value(QStringLiteral("type")).toString() == QLatin1String("text"))
                text += o.value(QStringLiteral("text")).toString();
            else if (crop.isNull())
                crop = img::fromBase64(o.value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString().section(QLatin1Char(','), 1).toLatin1());
        }
        const QColor c = crop.pixelColor(crop.width() / 2, crop.height() / 2);
        const QString colour = c.red() > 200 && c.blue() > 200 ? QStringLiteral("both") : c.red() > 200 ? QStringLiteral("aki") : c.blue() > 200 ? QStringLiteral("bo") : QStringLiteral("none");
        const QString owner = text.contains(QStringLiteral("red hair")) ? QStringLiteral("aki") : QStringLiteral("bo");
        checks << owner + QLatin1Char(':') + colour;
        const bool match = colour == owner || colour == QLatin1String("both");
        QJsonObject features;
        static const QRegularExpression line(QStringLiteral("\\nf(\\d+): "));
        auto it = line.globalMatch(text);
        while (it.hasNext())
            features.insert(QStringLiteral("f") + it.next().captured(1), match ? QStringLiteral("match") : QStringLiteral("different"));
        const QJsonObject answer{{QStringLiteral("features"), features}, {QStringLiteral("pose"), 8}, {QStringLiteral("issues"), QJsonArray{}}};
        return MockResponse::json({{QStringLiteral("choices"), QJsonArray{QJsonObject{
                                       {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact))}}},
                                       {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });

    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    QFile::remove(registry.configPath());
    Provider *sd = registry.addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"),
                                        {{QStringLiteral("url"), sdServer.baseUrl().toString()},
                                         {QStringLiteral("model"), QStringLiteral("waiIllustriousSDXL_v170.safetensors")},
                                         {QStringLiteral("control_net"), true}});
    QVERIFY(sd);
    qobject_cast<SdCppProvider *>(sd)->pollIntervalMs = 20;
    Provider *vision = registry.addProvider(QStringLiteral("llamacpp"), QStringLiteral("Vision"),
                                            {{QStringLiteral("url"), llm.baseUrl().toString() + QStringLiteral("/v1")}});
    QVERIFY(vision);
    auto cleanup = qScopeGuard([&] { QFile::remove(registry.configPath()); });
    JobQueue queue;
    GenerationService service(&registry, &queue);

    auto character = [](const char *id, const char *name, const QJsonObject &sheet, const char *lora, QColor colour) {
        CharacterContext c;
        c.id = QLatin1String(id);
        c.name = QLatin1String(name);
        c.sheet = sheet;
        QImage ref(64, 96, QImage::Format_ARGB32);
        ref.fill(colour);
        c.references = {ref};
        c.lora.file = QStringLiteral("C:/models/loras/%1.safetensors").arg(QLatin1String(lora));
        c.lora.trigger = QLatin1String(lora);
        c.lora.weight = 0.8;
        c.lora.family = QStringLiteral("SDXL");
        return c;
    };
    CastRequest request;
    request.scene.prompt = QStringLiteral("walking home after school");
    request.scene.size = QSize(640, 448);
    request.scene.seed = 100;
    request.cast = {character("aki", "Aki", akiSheet, "lmn_aki", Qt::red), character("bo", "Bo", boSheet, "lmn_bo", Qt::blue)};
    request.modelFamily = QStringLiteral("SDXL");
    request.check = true;
    request.saveAssets = false;
    // 1. Clean: each crop is its own character; four checks, no retry.
    QJsonArray m;
    Job *job = nullptr;
    auto start = [&](std::function<QRgb(const QString &, int)> painter) {
        paint = std::move(painter);
        akiPasses = 0;
        sdRequests.clear();
        results.clear();
        checks.clear();
        job = startCastGeneration(&service, request);
        QVERIFY(QTest::qWaitFor([&] { return job->isFinished(); }, 30000));
        QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
        m = job->metadata().value(QStringLiteral("cast_members")).toJsonArray();
    };
    auto byLora = [](const QString &lora, int) { return lora.contains(QStringLiteral("aki")) ? qRgb(255, 0, 0) : qRgb(0, 0, 255); };
    start(byLora);
    QCOMPARE(sdRequests.size(), 3);
    QCOMPARE(checks, (QStringList{QStringLiteral("aki:aki"), QStringLiteral("bo:aki"), QStringLiteral("bo:bo"), QStringLiteral("aki:bo")}));
    QCOMPARE(m.size(), 2);
    for (const QJsonValue &v : m) {
        const QJsonObject check = v.toObject().value(QStringLiteral("check")).toObject();
        QCOMPARE(check.value(QStringLiteral("verdict")).toString(), QStringLiteral("ok"));
        QCOMPARE(check.value(QStringLiteral("own")).toDouble(), 10.0);
        QCOMPARE(check.value(QStringLiteral("other")).toDouble(), 0.0);
        QCOMPARE(check.value(QStringLiteral("attempts")).toInt(), 1);
    }

    // 2. Aki's first pass comes out as Bo: a swap; only Aki is drawn and checked again.
    start([](const QString &lora, int akiPass) {
        if (lora.contains(QStringLiteral("aki")))
            return akiPass == 1 ? qRgb(0, 0, 255) : qRgb(255, 0, 0);
        return qRgb(0, 0, 255);
    });
    QCOMPARE(sdRequests.size(), 4);
    QVERIFY(sdRequests[3].value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("lmn_aki")));
    QCOMPARE(sdRequests[3].value(QStringLiteral("seed")).toDouble(), 1101.0); // a new seed
    QCOMPARE(checks.mid(4), (QStringList{QStringLiteral("aki:aki"), QStringLiteral("bo:aki")}));
    QCOMPARE(m[0].toObject().value(QStringLiteral("check")).toObject().value(QStringLiteral("verdict")).toString(), QStringLiteral("ok"));
    QCOMPARE(m[0].toObject().value(QStringLiteral("check")).toObject().value(QStringLiteral("attempts")).toInt(), 2);
    QCOMPARE(m[1].toObject().value(QStringLiteral("check")).toObject().value(QStringLiteral("attempts")).toInt(), 1);

    // 3. Aki keeps coming out with both looks: a blend, retried once, then kept with its verdict.
    start([](const QString &lora, int) { return lora.contains(QStringLiteral("aki")) ? qRgb(255, 0, 255) : qRgb(0, 0, 255); });
    QCOMPARE(sdRequests.size(), 4);
    const QJsonObject blend = m[0].toObject().value(QStringLiteral("check")).toObject();
    QCOMPARE(blend.value(QStringLiteral("verdict")).toString(), QStringLiteral("blend"));
    QCOMPARE(blend.value(QStringLiteral("attempts")).toInt(), 2);

    // 4. No vision model: the image stands as drawn, marked unchecked.
    registry.removeProvider(vision->instanceId());
    start(byLora);
    QCOMPARE(sdRequests.size(), 3);
    QCOMPARE(m[0].toObject().value(QStringLiteral("check")).toObject().value(QStringLiteral("verdict")).toString(), QStringLiteral("unchecked"));
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

namespace {

// Opens after a delay (like a server that is loading), or reports an error.
class TestGate : public ProviderGate
{
public:
    QString error;
    int delayMs = 150;
    int acquired = 0;
    int released = 0;
    QFuture<QString> acquire() override
    {
        ++acquired;
        auto promise = std::make_shared<QPromise<QString>>();
        promise->start();
        QTimer::singleShot(delayMs, [promise, e = error] {
            promise->addResult(e);
            promise->finish();
        });
        return promise->future();
    }
    void release() override { ++released; }
};

} // namespace

void TestServices::providerGate()
{
    MockHttpServer server;
    int requests = 0;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &) {
        ++requests;
        return chatReply(QJsonObject{{QStringLiteral("content"), QStringLiteral("hello")}}, QStringLiteral("stop"));
    });
    server.route("GET", QStringLiteral("/v1/models"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("data"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("qwen")}}}}});
    });
    OpenAiChatProvider llm(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("l"), &m_http, nullptr);
    llm.configure({{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}});
    ChatRequest req;
    req.messages = {ChatMessage::user(QStringLiteral("hi"))};

    // No gate: the provider's own operation.
    ChatOperation *direct = gatedChat(&llm, req);
    QTRY_VERIFY_WITH_TIMEOUT(direct->isFinished(), 5000);

    auto gate = std::make_shared<TestGate>();
    llm.setGate(gate);
    // The request waits for the gate, reports that it is starting, then runs.
    ChatOperation *op = gatedChat(&llm, req);
    op->setAutoDelete(false);
    QSignalSpy progress(op, &Operation::progress);
    QCOMPARE(requests, 1);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().text, QStringLiteral("hello"));
    QCOMPARE(requests, 2);
    QCOMPARE(gate->acquired, 1);
    QCOMPARE(gate->released, 1);
    delete op;

    // Cancelled while the server is still starting: nothing is sent, the gate is released.
    gate->delayMs = 400;
    op = gatedChat(&llm, req);
    op->setAutoDelete(false);
    op->cancel();
    QVERIFY(op->state() == Operation::State::Cancelled);
    QTest::qWait(600);
    QCOMPARE(requests, 2);
    QCOMPARE(gate->released, 2);
    delete op;

    // The server cannot start: the request fails with its error.
    gate->delayMs = 10;
    gate->error = QStringLiteral("CUDA out of memory");
    op = gatedChat(&llm, req);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 5000);
    QCOMPARE(op->errorString(), QStringLiteral("CUDA out of memory"));
    QCOMPARE(requests, 2);
    QCOMPARE(gate->released, 3);
    delete op;

    // One-off calls.
    gate->error.clear();
    QFuture<QStringList> models = gatedList(&llm, [&] { return llm.listModels(); });
    QTRY_VERIFY_WITH_TIMEOUT(models.isFinished(), 5000);
    QCOMPARE(models.result(), QStringList{QStringLiteral("qwen")});
    QCOMPARE(gate->released, 4);
    gate->error = QStringLiteral("no server");
    QFuture<QString> test = gatedCall(&llm, [&] { return llm.testConnection(); });
    QTRY_VERIFY_WITH_TIMEOUT(test.isFinished(), 5000);
    bool threw = false;
    try {
        test.result();
    } catch (const std::exception &e) {
        threw = QString::fromUtf8(e.what()).contains(QLatin1String("no server"));
    }
    QVERIFY(threw);
    QCOMPARE(gate->released, 5);
}

void TestServices::directorDatasetPlan()
{
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")},
                            {QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), QStringLiteral("red hair, long hair, red eyes, black robe, gold trim")}};
    const QJsonObject plan = Director::datasetPlan(sheet);
    const QJsonArray shots = plan.value(QStringLiteral("shots")).toArray();
    QCOMPARE(shots.size(), 24);
    QCOMPARE(Director::datasetPlan(sheet, 100).value(QStringLiteral("shots")).toArray().size(), Director::kMaxDatasetShots);
    QCOMPARE(Director::datasetPlan(sheet, 5).value(QStringLiteral("shots")).toArray().size(), 5);
    QSet<QString> ids;
    QSet<QString> framings;
    QSet<int> yaws;
    for (const QJsonValue &value : shots) {
        const QJsonObject s = value.toObject();
        ids << s.value(QStringLiteral("id")).toString();
        const QString prompt = s.value(QStringLiteral("prompt")).toString();
        QVERIFY2(prompt.startsWith(QStringLiteral("1girl, solo, red hair, long hair, red eyes, black robe, gold trim")), qPrintable(prompt));
        QVERIFY(prompt.contains(QStringLiteral("background")));
        const pose::RigParams rig = pose::RigParams::fromJson(s.value(QStringLiteral("pose")).toObject());
        framings << rig.framing;
        yaws << int(rig.yaw);
        if (rig.yaw >= 170) // a back view shows no face: no expression tags
            QVERIFY(!prompt.contains(QStringLiteral("smile")) && !prompt.contains(QStringLiteral("closed mouth")));
    }
    QCOMPARE(ids.size(), shots.size());
    QCOMPARE(framings, (QSet<QString>{QStringLiteral("full_body"), QStringLiteral("cowboy_shot"), QStringLiteral("upper_body"), QStringLiteral("headshot")}));
    QVERIFY(yaws.contains(0) && yaws.contains(45) && yaws.contains(-45) && yaws.contains(90) && yaws.contains(180));
    QCOMPARE(purposeProfile(QStringLiteral("lora_dataset")).defaultCount, 24);

    // Without tags the appearance comes from the sheet's fields.
    const QJsonObject fields{{QStringLiteral("hair"), QJsonObject{{QStringLiteral("color"), QStringLiteral("Silver")}, {QStringLiteral("length"), QStringLiteral("short")}}},
                             {QStringLiteral("eyes"), QJsonObject{{QStringLiteral("color"), QStringLiteral("green")}}},
                             {QStringLiteral("outfit"), QJsonArray{QJsonObject{{QStringLiteral("item"), QStringLiteral("hoodie")}, {QStringLiteral("color"), QStringLiteral("yellow")}}}}};
    QCOMPARE(Director::appearanceTags(fields), QStringLiteral("silver hair, short hair, green eyes, yellow hoodie"));
}

void TestServices::loraCaptions()
{
    QCOMPARE(modelTag(QStringLiteral("  Long_Red_Hair ")), QStringLiteral("long red hair"));
    QCOMPARE(normalizeTag(QStringLiteral(" LMN_Mira ")), QStringLiteral("lmn_mira")); // trigger words keep their underscores
    QCOMPARE(splitTags(QStringLiteral("lmn_mira, 1girl")).first(), QStringLiteral("lmn_mira"));
    QCOMPARE(normalizeTag(QStringLiteral("hood \\(clothing\\)")), QStringLiteral("hood (clothing)"));
    QCOMPARE(splitTags(QStringLiteral("1girl, solo,1girl , , Smile")), (QStringList{QStringLiteral("1girl"), QStringLiteral("solo"), QStringLiteral("smile")}));

    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")},
                            {QStringLiteral("tags"), QStringLiteral("long red hair, yellow eyes, black hooded cloak, black choker")},
                            {QStringLiteral("distinctive_features"), QJsonArray{QStringLiteral("mole under eye")}}};
    // Hair, eyes and body features belong to the character; expressions and gazes do not.
    for (const char *tag : {"red hair", "long hair", "very long hair", "yellow eyes", "ahoge", "twintails", "mole under eye", "pointy ears"})
        QVERIFY2(isIdentityTag(QLatin1String(tag), sheet, false), tag);
    for (const char *tag : {"closed eyes", "looking at viewer", "smile", "1girl", "solo", "from side", "full body", "white background",
                            "hand on hip", "hair ornament", "holding sword"})
        QVERIFY2(!isIdentityTag(QLatin1String(tag), sheet, true), tag);
    // The outfit: the sheet's tags and the model's own words for the same clothes.
    QVERIFY(isIdentityTag(QStringLiteral("black choker"), sheet, true));
    QVERIFY(isIdentityTag(QStringLiteral("cloak"), sheet, true));
    QVERIFY(isIdentityTag(QStringLiteral("black cloak"), sheet, true));
    for (const char *tag : {"hood", "black hood", "hooded", "black hoodie", "hood up"})
        QVERIFY2(isIdentityTag(QLatin1String(tag), sheet, true), tag);
    for (const char *tag : {"black boots", "holding", "hand on hip", "blush"})
        QVERIFY2(!isIdentityTag(QLatin1String(tag), sheet, true), tag);
    QVERIFY(!isIdentityTag(QStringLiteral("black choker"), sheet, false));
    QVERIFY(!isIdentityTag(QStringLiteral("cloak"), sheet, false));

    const QStringList tags{QStringLiteral("1girl"), QStringLiteral("solo"), QStringLiteral("long red hair"), QStringLiteral("yellow eyes"),
                           QStringLiteral("black cloak"), QStringLiteral("choker"), QStringLiteral("smile"), QStringLiteral("from side"),
                           QStringLiteral("full body"), QStringLiteral("standing"), QStringLiteral("white background")};
    QCOMPARE(buildCaption(tags, sheet, QStringLiteral("lmn_mira"), true),
             QStringLiteral("lmn_mira, 1girl, solo, smile, from side, full body, standing, white background"));
    QCOMPARE(buildCaption(tags, sheet, QStringLiteral("lmn_mira"), false),
             QStringLiteral("lmn_mira, 1girl, solo, black cloak, choker, smile, from side, full body, standing, white background"));
    QCOMPARE(suggestTrigger(QStringLiteral("Mira the Mage")), QStringLiteral("lmn_mira"));
    QCOMPARE(suggestTrigger(QStringLiteral("Аня")), QStringLiteral("lmn_char"));
    QVERIFY(imageTagsSchema().value(QStringLiteral("properties")).toObject().contains(QStringLiteral("tags")));
}

void TestServices::identityChecklist()
{
    // The face first, then the sheet's look; subject and count tags are not features.
    const QJsonObject sheet{{QStringLiteral("tags"), QStringLiteral("1girl, solo, long_red_hair, yellow eyes, black hooded cloak, black choker")}};
    const QStringList list = Director::identityChecklist(sheet);
    QCOMPARE(list.size(), 5);
    QVERIFY(list[0].contains(QStringLiteral("face")));
    QCOMPARE(list.mid(1), (QStringList{QStringLiteral("long red hair"), QStringLiteral("yellow eyes"), QStringLiteral("black hooded cloak"),
                                       QStringLiteral("black choker")}));
    QCOMPARE(Director::identityChecklist({{QStringLiteral("tags"), QStringLiteral("2girls, multiple girls, red hair")}}).size(), 2);
    // From the structured sheet fields when there are no tags.
    const QJsonObject structured{{QStringLiteral("hair"), QJsonObject{{QStringLiteral("color"), QStringLiteral("silver")}}},
                                 {QStringLiteral("eyes"), QJsonObject{{QStringLiteral("color"), QStringLiteral("blue")}}}};
    QCOMPARE(Director::identityChecklist(structured).mid(1), (QStringList{QStringLiteral("silver hair"), QStringLiteral("blue eyes")}));

    // The schema asks for every feature with a fixed verdict.
    const QJsonObject schema = Director::identityChecklistSchema(3);
    const QJsonObject features = schema[QStringLiteral("properties")][QStringLiteral("features")].toObject();
    QCOMPARE(features[QStringLiteral("required")].toArray().size(), 3);
    QCOMPARE(features[QStringLiteral("properties")][QStringLiteral("f3")][QStringLiteral("enum")].toArray().size(), 4);
    const QJsonObject answer{{QStringLiteral("features"), QJsonObject{{QStringLiteral("f1"), QStringLiteral("match")},
                                                                      {QStringLiteral("f2"), QStringLiteral("close")},
                                                                      {QStringLiteral("f3"), QStringLiteral("different")}}},
                             {QStringLiteral("pose"), 9},
                             {QStringLiteral("issues"), QJsonArray{}}};
    const json::SchemaValidator validator(schema);
    QStringList errors;
    QVERIFY2(validator.validate(answer, &errors), qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(!validator.validate(QJsonObject{{QStringLiteral("features"), QJsonObject{{QStringLiteral("f1"), QStringLiteral("yes")}}},
                                            {QStringLiteral("pose"), 9},
                                            {QStringLiteral("issues"), QJsonArray{}}}));

    // Identity: 1 + ½ of 3 visible features = 5; a hidden feature does not count.
    QCOMPARE(Director::checklistIdentity(answer), 5.0);
    QJsonObject hidden = answer;
    hidden[QStringLiteral("features")] = QJsonObject{{QStringLiteral("f1"), QStringLiteral("match")}, {QStringLiteral("f2"), QStringLiteral("not_visible")}};
    QCOMPARE(Director::checklistIdentity(hidden), 10.0);
    hidden[QStringLiteral("features")] = QJsonObject{{QStringLiteral("f1"), QStringLiteral("not_visible")}};
    QCOMPARE(Director::checklistIdentity(hidden), -1.0);
    // The test score: identity twice, the pose once; the pose alone when nothing is visible.
    QCOMPARE(Director::loraTestScore(answer), (2 * 5.0 + 9) / 3);
    QCOMPARE(Director::loraTestScore(hidden), 9.0);
    // A critiqueShot() verdict still works.
    QCOMPARE(Director::loraTestScore({{QStringLiteral("identity"), 9}, {QStringLiteral("pose"), 6}}), 8.0);
}

void TestServices::characterContext()
{
    CharacterContext akane;
    akane.id = QStringLiteral("c1");
    akane.name = QStringLiteral("Akane");
    akane.sheet = {{QStringLiteral("tags"), QStringLiteral("long red hair, yellow eyes, black hooded cloak")},
                   {QStringLiteral("description"), QStringLiteral("A girl with long red hair and yellow eyes in a black hooded cloak.")}};
    QImage ref(32, 48, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    akane.references = {ref, ref};
    akane.lora.file = QStringLiteral("D:/loras/lmn_akane.safetensors");
    akane.lora.trigger = QStringLiteral("lmn_akane");
    akane.lora.weight = 0.7;
    akane.lora.family = QStringLiteral("SDXL");
    PromptStyle tags;
    tags.dialect = PromptStyle::Dialect::Tags;
    PromptStyle prose;
    ImageCapabilities local;   // sd.cpp: LoRAs and one IP-Adapter reference
    local.loras = true;
    local.maxReferences = 1;
    ImageCapabilities cloud;   // a multi-reference API without LoRAs
    cloud.maxReferences = 4;
    cloud.edit = true;
    ImageCapabilities bare;    // neither
    auto request = [](const QString &prompt) {
        ImageRequest r;
        r.prompt = prompt;
        return r;
    };

    // Full, LoRA back-end: trigger first, the tags after the user's words; no reference (its pose
    // would leak in).
    ImageRequest r = request(QStringLiteral("reading in a library"));
    CharacterApplied a = applyCharacter(r, akane, local, CharacterUse::Full, tags, QStringLiteral("SDXL"));
    QVERIFY(a.lora && a.description && !a.reference);
    QCOMPARE(r.prompt, QStringLiteral("lmn_akane, reading in a library, long red hair, yellow eyes, black hooded cloak"));
    QCOMPARE(r.loras.size(), 1);
    QCOMPARE(r.loras[0].weight, 0.7);
    QVERIFY(r.references.isEmpty());
    QVERIFY(a.fallbackNote(akane, QStringLiteral("sd.cpp")).isEmpty());
    // Applying twice adds nothing.
    a = applyCharacter(r, akane, local, CharacterUse::Full, tags, QStringLiteral("SDXL"));
    QCOMPARE(r.loras.size(), 1);
    QCOMPARE(r.prompt.count(QStringLiteral("long red hair")), 1);
    QCOMPARE(r.prompt.count(QStringLiteral("lmn_akane")), 1);

    // Full, cloud: one reference and the description in prose; the note says what replaced the LoRA.
    r = request(QStringLiteral("Reading in a library."));
    a = applyCharacter(r, akane, cloud, CharacterUse::Full, prose);
    QVERIFY(!a.lora && a.reference && a.description);
    QCOMPARE(a.loraSkipped, QStringLiteral("no_lora_support"));
    QCOMPARE(r.references.size(), 1);
    QVERIFY(r.references[0].role == ReferenceRole::Character);
    QVERIFY(r.prompt.endsWith(QStringLiteral("in a black hooded cloak.")));
    QCOMPARE(a.fallbackNote(akane, QStringLiteral("Gemini")), QStringLiteral("Gemini cannot load LoRAs: using the reference image of Akane."));
    // Neither LoRAs nor references: the description alone.
    r = request(QString());
    a = applyCharacter(r, akane, bare, CharacterUse::Full, tags);
    QVERIFY(!a.lora && !a.reference && a.description);
    QCOMPARE(r.prompt, QStringLiteral("long red hair, yellow eyes, black hooded cloak"));
    QCOMPARE(a.fallbackNote(akane, QStringLiteral("Stability")), QStringLiteral("Stability cannot load LoRAs: describing Akane in the prompt."));

    // A LoRA of another family is left out, with a note.
    r = request(QStringLiteral("smile"));
    a = applyCharacter(r, akane, local, CharacterUse::Full, tags, QStringLiteral("SD 1.5"));
    QVERIFY(!a.lora && a.reference);
    QCOMPARE(a.loraSkipped, QStringLiteral("family"));
    QVERIFY(r.loras.isEmpty() && !r.prompt.contains(QStringLiteral("lmn_akane")));
    QVERIFY(a.fallbackNote(akane, QStringLiteral("sd.cpp")).contains(QStringLiteral("for SDXL models")));
    // An unknown family does not block the LoRA.
    r = request(QStringLiteral("smile"));
    QVERIFY(applyCharacter(r, akane, local, CharacterUse::Full, tags).lora);

    // Region (inpaint, expand): the LoRA, never the appearance tags; else a reference.
    r = request(QStringLiteral("a hand holding a cup"));
    a = applyCharacter(r, akane, local, CharacterUse::Region, tags, QStringLiteral("SDXL"));
    QVERIFY(a.lora && !a.description && !a.reference);
    QCOMPARE(r.prompt, QStringLiteral("lmn_akane, a hand holding a cup"));
    r = request(QStringLiteral("a hand holding a cup"));
    a = applyCharacter(r, akane, cloud, CharacterUse::Region, prose);
    QVERIFY(!a.lora && a.reference && !a.description);
    QCOMPARE(r.prompt, QStringLiteral("a hand holding a cup"));
    r = request(QStringLiteral("a hand"));
    QVERIFY(!applyCharacter(r, akane, bare, CharacterUse::Region, tags).any());

    // Edit: never the LoRA (and no note about it); the reference, else the description.
    r = request(QStringLiteral("make it night"));
    a = applyCharacter(r, akane, local, CharacterUse::Edit, tags, QStringLiteral("SDXL"));
    QVERIFY(!a.lora && a.reference && !a.description);
    QCOMPARE(a.loraSkipped, QStringLiteral("use"));
    QVERIFY(a.fallbackNote(akane, QStringLiteral("sd.cpp")).isEmpty());
    r = request(QStringLiteral("make it night"));
    a = applyCharacter(r, akane, bare, CharacterUse::Edit, tags);
    QVERIFY(!a.reference && a.description);

    // Shot (Create): LoRA and every reference that fits, as M13 did; the planned prompt stays.
    r = request(QStringLiteral("1girl, long red hair, standing"));
    ImageCapabilities two = local;
    two.maxReferences = 2;
    a = applyCharacter(r, akane, two, CharacterUse::Shot, tags);
    QVERIFY(a.lora && a.reference && !a.description);
    QCOMPARE(r.references.size(), 2);
    QCOMPARE(r.prompt, QStringLiteral("lmn_akane, 1girl, long red hair, standing"));
    // References already in the request count against the limit.
    r = request(QStringLiteral("x"));
    r.references << ReferenceImage{};
    applyCharacter(r, akane, two, CharacterUse::Shot, tags);
    QCOMPARE(r.references.size(), 2);

    // Metadata.
    r = request(QStringLiteral("smile"));
    a = applyCharacter(r, akane, local, CharacterUse::Full, tags, QStringLiteral("SDXL"));
    const QJsonObject meta = a.toJson(akane);
    QCOMPARE(meta.value(QStringLiteral("character")).toString(), QStringLiteral("c1"));
    QCOMPARE(meta.value(QStringLiteral("lora")).toString(), akane.lora.file);
    QCOMPARE(meta.value(QStringLiteral("lora_weight")).toDouble(), 0.7);
    QVERIFY(meta.value(QStringLiteral("description")).toBool());
    QVERIFY(!meta.contains(QStringLiteral("lora_skipped")));
    // No character: nothing happens.
    r = request(QStringLiteral("smile"));
    QVERIFY(!applyCharacter(r, CharacterContext{}, local, CharacterUse::Full, tags).any());
    QCOMPARE(r.prompt, QStringLiteral("smile"));
}

QTEST_MAIN(TestServices)
#include "tst_services.moc"
