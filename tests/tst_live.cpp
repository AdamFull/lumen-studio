// Smoke tests against real servers. Skipped unless the servers are given (the assistant scenario
// takes several minutes: set QTEST_FUNCTION_TIMEOUT=3600000):
//   LUMEN_LIVE_LLM=http://127.0.0.1:8080/v1   (llama.cpp / OpenAI-compatible)
//   LUMEN_LIVE_COMFY=http://127.0.0.1:8188    (ComfyUI)
//   LUMEN_LIVE_CHECKPOINT=<checkpoint name>   (optional; first checkpoint otherwise)
//   LUMEN_LIVE_OUT=<folder>                   (optional; generated images are saved there)
//   LUMEN_LIVE_REFERENCE=<image>              (vision tests: a character image to analyse)
#include "ai/Agent.h"
#include "ai/Director.h"
#include "ai/PromptStyle.h"
#include "ai/ProviderCatalog.h"
#include "ai/ProviderRegistry.h"
#include "ai/providers/ComfyUiProvider.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "core/Settings.h"
#include "doc/Project.h"
#include "net/HttpClient.h"
#include "pose/Rig.h"
#include "ui/Studio.h"
#include "ui/assistant/AssistantPanel.h"
#include "ui/pages/CreatePage.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

using namespace lumen;
using namespace lumen::ai;

namespace {

QString llmUrl()
{
    return qEnvironmentVariable("LUMEN_LIVE_LLM");
}

QString comfyUrl()
{
    return qEnvironmentVariable("LUMEN_LIVE_COMFY");
}

void save(const QImage &image, const QString &name)
{
    const QString dir = qEnvironmentVariable("LUMEN_LIVE_OUT");
    if (dir.isEmpty() || image.isNull())
        return;
    QDir().mkpath(dir);
    image.save(dir + u'/' + name);
}

} // namespace

class TestLive : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void llmToolCalling();
    void llmPlansShots();
    void comfyTextToImage();
    void comfyPoseControl();
    void assistantScenario();
    void visionCharacterSheet();
    void visionCritique();
    void assistantWithReferenceAndCritic();

private:
    net::HttpClient m_http;
};

void TestLive::llmToolCalling()
{
    if (llmUrl().isEmpty())
        QSKIP("LUMEN_LIVE_LLM is not set");
    OpenAiChatProvider llm(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("live"), &m_http, nullptr);
    llm.configure({{QStringLiteral("url"), llmUrl()}, {QStringLiteral("timeout_s"), 600}});
    ToolSet tools;
    int calls = 0;
    tools.add(ToolDefinition{QStringLiteral("add_numbers"), QStringLiteral("Adds two numbers and returns the sum."),
                             QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                         {QStringLiteral("properties"), QJsonObject{{QStringLiteral("a"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
                                                                                    {QStringLiteral("b"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}}},
                                         {QStringLiteral("required"), QJsonArray{QStringLiteral("a"), QStringLiteral("b")}}}},
              [&calls](const QJsonObject &args) {
                  ++calls;
                  ToolResult r;
                  r.data.insert(QStringLiteral("sum"), args.value(QStringLiteral("a")).toDouble() + args.value(QStringLiteral("b")).toDouble());
                  return QtFuture::makeReadyValueFuture(r);
              });
    QElapsedTimer t;
    t.start();
    auto *run = new AgentRun(&llm, QString(), tools,
                             {ChatMessage::system(QStringLiteral("Use the tools to compute. Answer with the number only.")),
                              ChatMessage::user(QStringLiteral("What is 1234 + 4321? Use add_numbers."))});
    run->setAutoDelete(false);
    run->start();
    QTRY_VERIFY_WITH_TIMEOUT(run->isFinished(), 300000);
    qInfo() << "tool call turn:" << t.elapsed() << "ms, steps" << run->steps() << "reply" << run->reply();
    QVERIFY2(run->state() == Operation::State::Succeeded, qPrintable(run->errorString()));
    QCOMPARE(calls, 1);
    QVERIFY(run->reply().contains(QLatin1String("5555")));
    delete run;
}

void TestLive::llmPlansShots()
{
    if (llmUrl().isEmpty())
        QSKIP("LUMEN_LIVE_LLM is not set");
    ProviderRegistry registry(&m_http);
    registerBuiltInProviders(registry);
    for (Provider *p : registry.providers())
        registry.removeProvider(p->instanceId());
    QVERIFY(registry.addProvider(QStringLiteral("llamacpp"), QStringLiteral("LLM"),
                                 {{QStringLiteral("url"), llmUrl()}, {QStringLiteral("timeout_s"), 900}}));
    Director director(&registry);
    PlanInput in;
    in.request = QStringLiteral("Two source images of a red-haired mage girl for a Live2D avatar.");
    in.purposeId = QStringLiteral("live2d");
    in.count = 2;
    QElapsedTimer t;
    t.start();
    StructuredChatOperation *op = director.planShots(in);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    qInfo() << "plan:" << t.elapsed() << "ms, attempts" << op->attempts();
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    const QJsonArray shots = op->result().value(QStringLiteral("shots")).toArray();
    QCOMPARE(shots.size(), 2);
    for (const QJsonValue &v : shots)
        qInfo().noquote() << " -" << v.toObject().value(QStringLiteral("title")).toString() << "|"
                          << v.toObject().value(QStringLiteral("prompt")).toString().left(160);
    delete op;
    for (Provider *p : registry.providers())
        registry.removeProvider(p->instanceId());
}

void TestLive::comfyTextToImage()
{
    if (comfyUrl().isEmpty())
        QSKIP("LUMEN_LIVE_COMFY is not set");
    ComfyUiProvider comfy(ComfyUiProvider::describe(), QStringLiteral("comfy"), &m_http, nullptr);
    comfy.configure({{QStringLiteral("url"), comfyUrl()}, {QStringLiteral("model"), qEnvironmentVariable("LUMEN_LIVE_CHECKPOINT")}});
    ImageRequest r;
    r.prompt = QStringLiteral("1girl, solo, red hair, mage robe, smile, upper body, simple background, masterpiece, best quality");
    r.negativePrompt = QStringLiteral("lowres, bad anatomy, bad hands, text, watermark");
    r.size = QSize(832, 1216);
    r.steps = 24;
    r.cfg = 5.0;
    r.seed = 42;
    QElapsedTimer t;
    t.start();
    ImageOperation *op = comfy.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    qInfo() << "txt2img:" << t.elapsed() << "ms";
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    QCOMPARE(op->result().images.size(), 1);
    QCOMPARE(op->result().images[0].image.size(), QSize(832, 1216));
    save(op->result().images[0].image, QStringLiteral("comfy_txt2img.png"));
    delete op;
}

void TestLive::comfyPoseControl()
{
    if (comfyUrl().isEmpty())
        QSKIP("LUMEN_LIVE_COMFY is not set");
    ComfyUiProvider comfy(ComfyUiProvider::describe(), QStringLiteral("comfy"), &m_http, nullptr);
    comfy.configure({{QStringLiteral("url"), comfyUrl()}, {QStringLiteral("model"), qEnvironmentVariable("LUMEN_LIVE_CHECKPOINT")}});
    QJsonObject shot{{QStringLiteral("aspect_ratio"), QStringLiteral("2:3")},
                     {QStringLiteral("prompt"), QStringLiteral("1girl, solo, red hair, mage robe, a-pose, arms away from body, full body, "
                                                               "front view, plain grey background, masterpiece, best quality")},
                     {QStringLiteral("negative_prompt"), QStringLiteral("lowres, bad anatomy, bad hands, text")},
                     {QStringLiteral("pose"), pose::RigParams().toJson()}};
    const ImageCapabilities caps = comfy.capabilities();
    PromptStyle style;
    const QSize size = Director::sizeForAspect(QStringLiteral("2:3"), caps.nativePixels, 64);
    const pose::Pose pose = Director::poseForShot(shot, size);
    ImageRequest r = Director::buildShotRequest(shot, {}, pose, style, caps);
    r.seed = 7;
    r.steps = 24;
    r.cfg = 5.0;
    QVERIFY(!r.controls.isEmpty());
    save(r.controls.first().image, QStringLiteral("comfy_pose_map.png"));
    QElapsedTimer t;
    t.start();
    ImageOperation *op = comfy.generate(r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    qInfo() << "pose-controlled generation:" << t.elapsed() << "ms at" << r.size;
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    save(op->result().images.value(0).image, QStringLiteral("comfy_pose.png"));
    delete op;
}

void TestLive::assistantScenario()
{
    if (llmUrl().isEmpty() || comfyUrl().isEmpty())
        QSKIP("LUMEN_LIVE_LLM and LUMEN_LIVE_COMFY are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    QTemporaryDir dir;
    ui::Studio studio;
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    // The LLM and SDXL share the GPU: ComfyUI unloads its models when idle.
    QVERIFY(studio.providers()->addProvider(QStringLiteral("comfyui"), QStringLiteral("ComfyUI"),
                                            {{QStringLiteral("url"), comfyUrl()},
                                             {QStringLiteral("model"), qEnvironmentVariable("LUMEN_LIVE_CHECKPOINT")},
                                             {QStringLiteral("free_vram"), true}}));
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("llama.cpp"),
                                            {{QStringLiteral("url"), llmUrl()}, {QStringLiteral("timeout_s"), 900}}));
    studio.openProject(dir.filePath(QStringLiteral("live.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 10000);
    ui::CreatePage create(&studio);
    for (QSpinBox *spin : create.findChildren<QSpinBox *>()) {
        if (spin->suffix().contains(QLatin1String("per shot")))
            spin->setValue(1);
    }
    ui::AssistantPanel panel(&studio);
    QSignalSpy finished(&panel, &ui::AssistantPanel::runFinished);
    QElapsedTimer t;
    t.start();
    panel.send(QStringLiteral("I need two images of my character, a red-haired mage girl in a blue robe, for a Live2D avatar. "
                              "There are no reference images. Plan the shots and generate them."));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 1800000);
    qInfo() << "assistant scenario:" << t.elapsed() << "ms";
    for (const ChatMessage &m : panel.history()) {
        if (m.role == ChatRole::Assistant && !m.toolCalls.isEmpty())
            for (const ToolCall &c : m.toolCalls)
                qInfo().noquote() << "  tool:" << c.name << QString::fromUtf8(QJsonDocument(c.arguments).toJson(QJsonDocument::Compact)).left(200);
        if (m.role == ChatRole::Tool)
            qInfo().noquote() << "  result:" << m.text.left(400);
    }
    qInfo().noquote() << "  reply:" << panel.lastReply();
    QVERIFY(!panel.lastReply().isEmpty());
    const QJsonArray shots = studio.directorActions()->directorState().value(QStringLiteral("shots")).toArray();
    QVERIFY(!shots.isEmpty());
    int images = 0;
    for (const QJsonValue &v : shots) {
        for (const QJsonValue &id : v.toObject().value(QStringLiteral("results")).toArray()) {
            auto img = studio.project()->loadImage(id.toString());
            QTRY_VERIFY_WITH_TIMEOUT(img.isFinished(), 10000);
            save(img.result(), QStringLiteral("assistant_%1.png").arg(++images));
        }
    }
    QVERIFY(images >= 1);
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
}

namespace {

QString referencePath()
{
    return qEnvironmentVariable("LUMEN_LIVE_REFERENCE");
}

ProviderRegistry *liveRegistry(net::HttpClient *http)
{
    auto *registry = new ProviderRegistry(http);
    registerBuiltInProviders(*registry);
    for (Provider *p : registry->providers())
        registry->removeProvider(p->instanceId());
    registry->addProvider(QStringLiteral("llamacpp"), QStringLiteral("LLM"), {{QStringLiteral("url"), llmUrl()}, {QStringLiteral("timeout_s"), 900}});
    return registry;
}

} // namespace

void TestLive::visionCharacterSheet()
{
    if (llmUrl().isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_LLM and LUMEN_LIVE_REFERENCE are needed");
    std::unique_ptr<ProviderRegistry> registry(liveRegistry(&m_http));
    Director director(registry.get());
    const QImage reference(referencePath());
    QVERIFY(!reference.isNull());
    QElapsedTimer t;
    t.start();
    StructuredChatOperation *op = director.analyzeCharacter({reference}, QString());
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    qInfo() << "character sheet:" << t.elapsed() << "ms, attempts" << op->attempts();
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    const QJsonObject sheet = op->result();
    qInfo().noquote() << QJsonDocument(sheet).toJson(QJsonDocument::Indented).left(1500);
    QVERIFY(QJsonDocument(sheet).toJson().toLower().contains("red"));
    delete op;
    for (Provider *p : registry->providers())
        registry->removeProvider(p->instanceId());
}

void TestLive::visionCritique()
{
    if (llmUrl().isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_LLM and LUMEN_LIVE_REFERENCE are needed");
    const QString candidatePath = qEnvironmentVariable("LUMEN_LIVE_CANDIDATE");
    if (candidatePath.isEmpty())
        QSKIP("LUMEN_LIVE_CANDIDATE (a generated shot to review) is needed");
    std::unique_ptr<ProviderRegistry> registry(liveRegistry(&m_http));
    Director director(registry.get());
    const QJsonObject shot{{QStringLiteral("title"), QStringLiteral("Front A-pose")},
                           {QStringLiteral("description"), QStringLiteral("full body, front view, arms slightly away from the body")},
                           {QStringLiteral("prompt"), QStringLiteral("1girl, red hair, mage robe, full body, front view, a-pose")}};
    const QJsonObject character{{QStringLiteral("name"), QStringLiteral("Mage")},
                                {QStringLiteral("hair"), QJsonObject{{QStringLiteral("color"), QStringLiteral("red")}}}};
    QElapsedTimer t;
    t.start();
    StructuredChatOperation *op = director.critiqueShot(QImage(candidatePath), shot, character, {QImage(referencePath())});
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    qInfo() << "critique:" << t.elapsed() << "ms";
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    qInfo().noquote() << QJsonDocument(op->result()).toJson(QJsonDocument::Indented);
    const int score = Director::critiqueScore(op->result());
    QVERIFY(score >= 0 && score <= 10);
    QVERIFY(!op->result().value(QStringLiteral("revised_prompt")).toString().isEmpty());
    delete op;
    for (Provider *p : registry->providers())
        registry->removeProvider(p->instanceId());
}

void TestLive::assistantWithReferenceAndCritic()
{
    if (llmUrl().isEmpty() || comfyUrl().isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_LLM, LUMEN_LIVE_COMFY and LUMEN_LIVE_REFERENCE are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().setValue(QStringLiteral("director/critic"), true);
    Settings::instance().setValue(QStringLiteral("director/criticThreshold"), 6);
    Settings::instance().setValue(QStringLiteral("director/criticRetries"), 1);
    QTemporaryDir dir;
    ui::Studio studio;
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("comfyui"), QStringLiteral("ComfyUI"),
                                            {{QStringLiteral("url"), comfyUrl()},
                                             {QStringLiteral("model"), qEnvironmentVariable("LUMEN_LIVE_CHECKPOINT")},
                                             {QStringLiteral("free_vram"), true}}));
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("llama.cpp"),
                                            {{QStringLiteral("url"), llmUrl()}, {QStringLiteral("timeout_s"), 900}}));
    studio.openProject(dir.filePath(QStringLiteral("live.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 10000);
    auto imported = studio.project()->importFile(referencePath(), doc::AssetKind::Reference);
    QTRY_VERIFY_WITH_TIMEOUT(imported.isFinished(), 10000);
    const QString refId = imported.result().id;
    ui::CreatePage create(&studio);
    for (QSpinBox *spin : create.findChildren<QSpinBox *>()) {
        if (spin->suffix().contains(QLatin1String("per shot")))
            spin->setValue(1);
    }
    ui::AssistantPanel panel(&studio);
    QSignalSpy finished(&panel, &ui::AssistantPanel::runFinished);
    QElapsedTimer t;
    t.start();
    panel.send(QStringLiteral("This is my character: reference image %1. Analyse it, then plan two source images for a Live2D avatar "
                              "(purpose live2d, 2 shots) and generate them.")
                   .arg(refId));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 3000000);
    qInfo() << "assistant with reference and critic:" << t.elapsed() << "ms";
    for (const ChatMessage &m : panel.history()) {
        if (m.role == ChatRole::Assistant && !m.toolCalls.isEmpty())
            for (const ToolCall &c : m.toolCalls)
                qInfo().noquote() << "  tool:" << c.name << QString::fromUtf8(QJsonDocument(c.arguments).toJson(QJsonDocument::Compact)).left(200);
        if (m.role == ChatRole::Tool)
            qInfo().noquote() << "  result:" << m.text.left(300);
    }
    qInfo().noquote() << "  reply:" << panel.lastReply();
    const QJsonObject state = studio.directorActions()->directorState();
    qInfo().noquote() << "  character:" << QJsonDocument(state.value(QStringLiteral("character")).toObject()).toJson(QJsonDocument::Compact);
    int images = 0;
    for (const QJsonValue &v : state.value(QStringLiteral("shots")).toArray()) {
        const QJsonObject shot = v.toObject();
        qInfo().noquote() << "  shot" << shot.value(QStringLiteral("id")).toString() << "best critic"
                          << shot.value(QStringLiteral("best_critic_score")).toInt(-1) << "results"
                          << shot.value(QStringLiteral("results")).toArray().size();
        for (const QJsonValue &id : shot.value(QStringLiteral("results")).toArray()) {
            auto img = studio.project()->loadImage(id.toString());
            QTRY_VERIFY_WITH_TIMEOUT(img.isFinished(), 10000);
            save(img.result(), QStringLiteral("reference_%1.png").arg(++images));
        }
    }
    QVERIFY(!state.value(QStringLiteral("character")).toObject().isEmpty());
    QVERIFY(images >= 2);
    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
}

QTEST_MAIN(TestLive)
#include "tst_live.moc"
