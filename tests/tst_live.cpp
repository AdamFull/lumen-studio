// Smoke tests against real servers. Skipped unless the servers are given (the assistant scenario
// takes several minutes: set QTEST_FUNCTION_TIMEOUT=3600000):
//   LUMEN_LIVE_LLM=http://127.0.0.1:8080/v1   (llama.cpp / OpenAI-compatible)
//   LUMEN_LIVE_COMFY=http://127.0.0.1:8188    (ComfyUI)
//   LUMEN_LIVE_CHECKPOINT=<checkpoint name>   (optional; first checkpoint otherwise)
//   LUMEN_LIVE_OUT=<folder>                   (optional; generated images are saved there)
//   LUMEN_LIVE_REFERENCE=<image>              (vision tests: a character image to analyse)
#include "ai/Agent.h"
#include "ai/Director.h"
#include "ai/Gate.h"
#include "ai/LoraCaptions.h"
#include "ai/PromptStyle.h"
#include "ai/ProviderCatalog.h"
#include "ai/ProviderRegistry.h"
#include "ai/providers/ComfyUiProvider.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "ai/providers/SdCppProvider.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Settings.h"
#include "doc/CharacterLibrary.h"
#include "doc/Document.h"
#include "doc/Layer.h"
#include "doc/LoraDataset.h"
#include "doc/Parts.h"
#include "doc/Project.h"
#include "net/HttpClient.h"
#include "pose/Rig.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/CloudTraining.h"
#include "runtime/RunPod.h"
#include "runtime/LoraTraining.h"
#include "runtime/ModelFormats.h"
#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "runtime/Trainer.h"
#include "ui/CharacterTraining.h"
#include "ui/LoraEvaluation.h"
#include "ui/director/DirectorSession.h"
#include "ui/pages/AiPanel.h"
#include "ui/pages/GeneratePage.h"
#include "ui/parts/ExpressionCheck.h"
#include "ui/widgets/CharacterChip.h"
#include "ui/widgets/Controls.h"
#include "ui/dialogs/LoraDatasetDialog.h"
#include "ui/Studio.h"
#include "ui/assistant/AssistantPanel.h"
#include "ui/pages/CreatePage.h"

#include "ai/GenerationService.h"
#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
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
    void managedLlamaServer();
    void installRuntime();
    void managedSdServer();
    void animaServer();
    void managedStudioScenario();
    void managedSdUpscale();
    void managedComfyUi();
    void hubDownload();
    void managedSdLora();
    void managedDatasetShots();
    void managedCaptions();
    void installTrainer();
    void trainLora();
    void evaluateLora();
    void characterLoraEndToEnd();
    void cloudLora();
    void characterEverywhere();

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

namespace {

class SupervisorGate : public ProviderGate
{
public:
    SupervisorGate(runtime::ServerSupervisor *s, QString id) : m_s(s), m_id(std::move(id)) {}
    QFuture<QString> acquire() override { return m_s->acquire(m_id); }
    void release() override { m_s->release(m_id); }

private:
    runtime::ServerSupervisor *m_s;
    QString m_id;
};

template<typename T>
bool waitFor(const QFuture<T> &f, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!f.isFinished() && t.elapsed() < ms)
        QTest::qWait(50);
    return f.isFinished();
}

} // namespace

// LUMEN_LIVE_LLAMA_SERVER=<llama-server.exe> LUMEN_LIVE_LLAMA_MODEL=<.gguf> [LUMEN_LIVE_LLAMA_MMPROJ=<.gguf>]:
// Lumen adopts the install, starts the server on the first request, chats, and stops it.
void TestLive::managedLlamaServer()
{
    const QString exe = qEnvironmentVariable("LUMEN_LIVE_LLAMA_SERVER");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MODEL");
    if (exe.isEmpty() || model.isEmpty())
        QSKIP("LUMEN_LIVE_LLAMA_SERVER and LUMEN_LIVE_LLAMA_MODEL are needed");
    QTemporaryDir root;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root.filePath(QStringLiteral("runtimes")));
    QFuture<runtime::InstalledRuntime> adopted = store.adopt(exe);
    QVERIFY(waitFor(adopted, 30000));
    const runtime::InstalledRuntime rt = adopted.result();
    qInfo() << "adopted" << rt.displayName() << rt.versionInfo;

    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(root.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(root.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt.id, QStringLiteral("Live llama"));
    profile.options.insert(QStringLiteral("model"), model);
    const QString mmproj = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ");
    if (!mmproj.isEmpty())
        profile.options.insert(QStringLiteral("mmproj"), mmproj);
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);
    qInfo().noquote() << "command:" << servers.launchFor(profile.id).commandLine();

    OpenAiChatProvider llm(OpenAiChatProvider::describe(OpenAiChatProvider::Flavor::LlamaCpp), QStringLiteral("live"), &m_http, nullptr);
    llm.configure({{QStringLiteral("url"), runtime::providerUrl(rt.kind, profile.port)}, {QStringLiteral("timeout_s"), 600}});
    llm.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));

    QElapsedTimer t;
    t.start();
    ChatRequest req;
    req.messages = {ChatMessage::system(QStringLiteral("Answer in one short sentence.")),
                    ChatMessage::user(QStringLiteral("Name a primary colour."))};
    req.maxTokens = 400;
    ChatOperation *op = gatedChat(&llm, req);
    op->setAutoDelete(false);
    QStringList statuses;
    connect(op, &Operation::progress, this, [&statuses](double, const QString &s) { statuses << s; });
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 360000);
    if (op->state() != Operation::State::Succeeded)
        qInfo().noquote() << servers.log(profile.id).mid(std::max<qsizetype>(0, servers.log(profile.id).size() - 30)).join(QLatin1Char('\n'));
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    qInfo() << "first answer after" << t.elapsed() << "ms (includes server start):" << op->result().text.left(200);
    QVERIFY(!op->result().text.trimmed().isEmpty());
    QCOMPARE(servers.state(profile.id), runtime::ServerState::Ready);
    QCOMPARE(servers.users(profile.id), 0);
    delete op;

    QFuture<QStringList> models = gatedList(&llm, [&llm] { return llm.listModels(); });
    QVERIFY(waitFor(models, 30000));
    qInfo() << "models:" << models.result();

    t.restart();
    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
    qInfo() << "stopped after" << t.elapsed() << "ms";
    QVERIFY(runtime::portIsFree(profile.port));
}

// LUMEN_LIVE_INSTALL=<runtime>:<backend>, e.g. "sd.cpp:cpu": installs a real release from GitHub
// (downloads it!) and checks that the server runs.
void TestLive::installRuntime()
{
    const QString what = qEnvironmentVariable("LUMEN_LIVE_INSTALL");
    if (what.isEmpty())
        QSKIP("LUMEN_LIVE_INSTALL is not set");
    const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(what.section(QLatin1Char(':'), 0, 0));
    QVERIFY(spec);
    // LUMEN_LIVE_INSTALL_ROOT keeps the install (e.g. the app's own runtimes folder); otherwise a
    // temporary folder is used.
    QTemporaryDir tmp;
    const QString keep = qEnvironmentVariable("LUMEN_LIVE_INSTALL_ROOT");
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(keep.isEmpty() ? tmp.filePath(QStringLiteral("runtimes")) : keep);
    store.load();
    QFuture<QList<runtime::Release>> releases = store.feed()->fetch(*spec);
    QVERIFY(waitFor(releases, 60000));
    const runtime::InstallPlan plan = runtime::planInstall(*spec, what.section(QLatin1Char(':'), 1), releases.result());
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    qInfo() << "installing" << plan.tag << plan.main.name << plan.downloadSize() / 1e6 << "MB";
    QElapsedTimer t;
    t.start();
    Job *job = store.install(plan);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 1800000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    const auto rt = store.runtime(job->result().toString());
    QVERIFY(rt);
    qInfo() << "installed in" << t.elapsed() << "ms:" << rt->executable << rt->versionInfo;
    QVERIFY(!rt->versionInfo.isEmpty());
}

// LUMEN_LIVE_SD_RUNTIMES=<runtimes folder with an installed sd.cpp>, LUMEN_LIVE_SD_MODEL=<checkpoint>,
// optional LUMEN_LIVE_SD_CONTROLNET, LUMEN_LIVE_SD_IPADAPTER + LUMEN_LIVE_SD_CLIPVISION, LUMEN_LIVE_REFERENCE:
// Lumen starts sd-server on the first request (8 GB defaults: q8_0, weights in RAM, VAE tiling) and
// generates a pose-controlled shot with the reference through the native API.
void TestLive::managedSdServer()
{
    const QString root = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    if (root.isEmpty() || model.isEmpty())
        QSKIP("LUMEN_LIVE_SD_RUNTIMES and LUMEN_LIVE_SD_MODEL are needed");
    QTemporaryDir tmp;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root);
    store.load();
    const auto rt = store.latestManaged(QStringLiteral("sd.cpp"));
    QVERIFY2(rt, "no managed sd.cpp install in LUMEN_LIVE_SD_RUNTIMES");

    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(tmp.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(tmp.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt->id, QStringLiteral("Live sd.cpp"));
    profile.options.insert(QStringLiteral("model"), model);
    const QString controlNet = qEnvironmentVariable("LUMEN_LIVE_SD_CONTROLNET");
    const QString ipAdapter = qEnvironmentVariable("LUMEN_LIVE_SD_IPADAPTER");
    const QString clipVision = qEnvironmentVariable("LUMEN_LIVE_SD_CLIPVISION");
    if (!controlNet.isEmpty())
        profile.options.insert(QStringLiteral("control_net"), controlNet);
    if (!ipAdapter.isEmpty() && !clipVision.isEmpty()) {
        profile.options.insert(QStringLiteral("ip_adapter"), ipAdapter);
        profile.options.insert(QStringLiteral("clip_vision"), clipVision);
    }
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);
    qInfo().noquote() << "command:" << servers.launchFor(profile.id).commandLine();

    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("live-sd"), &m_http, nullptr);
    sd.configure({{QStringLiteral("url"), runtime::providerUrl(rt->kind, profile.port)},
                  {QStringLiteral("control_net"), !controlNet.isEmpty()},
                  {QStringLiteral("ip_adapter"), !ipAdapter.isEmpty() && !clipVision.isEmpty()}});
    sd.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));

    QList<ReferenceImage> refs;
    const QString refPath = qEnvironmentVariable("LUMEN_LIVE_REFERENCE");
    if (!refPath.isEmpty() && sd.capabilities().maxReferences > 0) {
        ReferenceImage ref;
        ref.image = QImage(refPath);
        ref.role = ReferenceRole::Character;
        QVERIFY(!ref.image.isNull());
        refs << ref;
    }
    QJsonObject shot{{QStringLiteral("aspect_ratio"), QStringLiteral("2:3")},
                     {QStringLiteral("prompt"), QStringLiteral("1girl, solo, long red hair, red eyes, black robe with gold ornaments, wide sleeves, "
                                                               "standing, arms at sides, full body, front view, plain grey background, "
                                                               "masterpiece, best quality")},
                     {QStringLiteral("negative_prompt"), QStringLiteral("lowres, bad anatomy, bad hands, extra fingers, worst quality, text")},
                     {QStringLiteral("pose"), pose::RigParams().toJson()}};
    const ImageCapabilities caps = sd.capabilities();
    PromptStyle style;
    const QSize size = Director::sizeForAspect(QStringLiteral("2:3"), caps.nativePixels, caps.sizeMultiple);
    const pose::Pose pose = Director::poseForShot(shot, size);
    ImageRequest r = Director::buildShotRequest(shot, refs, pose, style, caps);
    r.seed = 7;
    r.steps = 28;
    r.cfg = 6.0;
    r.sampler = QStringLiteral("euler_a");
    qInfo() << "request:" << r.size << "pose" << !r.controls.isEmpty() << "references" << r.references.size();

    QElapsedTimer t;
    t.start();
    ImageOperation *op = gatedGenerate(&sd, r);
    op->setAutoDelete(false);
    int previews = 0;
    QString lastStatus;
    connect(op, &Operation::preview, this, [&previews] { ++previews; });
    connect(op, &Operation::progress, this, [&lastStatus](double, const QString &s) { lastStatus = s; });
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    if (op->state() != Operation::State::Succeeded)
        qInfo().noquote() << servers.log(profile.id).mid(std::max<qsizetype>(0, servers.log(profile.id).size() - 30)).join(QLatin1Char('\n'));
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    qInfo() << "first image after" << t.elapsed() << "ms (includes server start)," << previews << "previews, last status" << lastStatus;
    save(op->result().images.value(0).image, QStringLiteral("sdcpp_shot.png"));
    if (!r.controls.isEmpty())
        save(r.controls.first().image, QStringLiteral("sdcpp_pose_map.png"));
    delete op;

    // A second request reuses the running server.
    t.restart();
    r.seed = 8;
    op = gatedGenerate(&sd, r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    qInfo() << "second image after" << t.elapsed() << "ms";
    save(op->result().images.value(0).image, QStringLiteral("sdcpp_shot2.png"));
    delete op;

    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
}

// LUMEN_LIVE_SD_RUNTIMES=<runtimes folder with an installed sd.cpp>, LUMEN_LIVE_ANIMA_MODEL=<an Anima
// diffusion model>, LUMEN_LIVE_MODEL_DIRS=<folders with its VAE and text encoder, ';'-separated>:
// the library finds the model's family and companions from the headers, Lumen starts sd-server
// with them and its 8 GB defaults, and draws an Anima-style prompt with "Safe for work".
void TestLive::animaServer()
{
    const QString root = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString model = QDir::cleanPath(qEnvironmentVariable("LUMEN_LIVE_ANIMA_MODEL"));
    if (root.isEmpty() || qEnvironmentVariable("LUMEN_LIVE_ANIMA_MODEL").isEmpty())
        QSKIP("LUMEN_LIVE_SD_RUNTIMES and LUMEN_LIVE_ANIMA_MODEL are needed");
    QTemporaryDir tmp;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root);
    store.load();
    const auto rt = store.latestManaged(QStringLiteral("sd.cpp"));
    QVERIFY2(rt, "no managed sd.cpp install in LUMEN_LIVE_SD_RUNTIMES");

    runtime::ModelLibrary lib(&m_http);
    lib.setDataDir(tmp.filePath(QStringLiteral("library")));
    lib.load();
    QSignalSpy scanned(&lib, &runtime::ModelLibrary::scanFinished);
    lib.addRoot(QFileInfo(model).absolutePath());
    for (const QString &d : qEnvironmentVariable("LUMEN_LIVE_MODEL_DIRS").split(QLatin1Char(';'), Qt::SkipEmptyParts))
        lib.addRoot(d);
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !lib.isScanning(), 120000);
    const auto entry = lib.entry(model);
    QVERIFY(entry);
    QCOMPARE(entry->info.family, QStringLiteral("Anima"));
    for (const runtime::ModelCompanion &c : runtime::companionsOf(&lib, model))
        qInfo().noquote() << "companion" << c.file.fileName() << "->" << (c.missing() ? QStringLiteral("missing") : c.path);
    const QVariantMap companions = runtime::companionServerOptions(&lib, model);
    QVERIFY2(companions.contains(QStringLiteral("vae")) && companions.contains(QStringLiteral("llm")), "VAE or text encoder not found");

    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(tmp.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(tmp.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt->id, QStringLiteral("Live Anima"));
    profile.options.insert(QStringLiteral("diffusion_model"), QDir::toNativeSeparators(model));
    profile.options.insert(companions);
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);
    qInfo().noquote() << "command:" << servers.launchFor(profile.id).commandLine();

    // As Studio configures it: the file name and the family from the header.
    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("live-anima"), &m_http, nullptr);
    sd.configure({{QStringLiteral("url"), runtime::providerUrl(rt->kind, profile.port)},
                  {QStringLiteral("model"), QFileInfo(model).fileName()},
                  {QStringLiteral("model_family"), entry->info.family}});
    sd.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));
    const PromptStyle style = promptStyleFor(&sd);
    QCOMPARE(style.family, QStringLiteral("anima"));

    ImageRequest r; // Lumen's defaults: 28 steps, CFG 6
    r.prompt = style.withSafeTag(style.apply(QStringLiteral("1girl, solo, red hair, long hair, green eyes, school uniform, smile, upper body, "
                                                            "cherry blossoms, outdoors. A cheerful girl stands under blooming cherry trees on a "
                                                            "sunny spring day.")));
    r.negativePrompt = GenerationService::withSafeNegative(style.defaultNegative);
    r.size = QSize(832, 1216);
    r.seed = 42;
    r.sampler = QStringLiteral("euler_a");
    qInfo().noquote() << "prompt:" << r.prompt;
    qInfo().noquote() << "negative:" << r.negativePrompt;

    QElapsedTimer t;
    t.start();
    ImageOperation *op = gatedGenerate(&sd, r);
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
    if (op->state() != Operation::State::Succeeded)
        qInfo().noquote() << servers.log(profile.id).mid(std::max<qsizetype>(0, servers.log(profile.id).size() - 40)).join(QLatin1Char('\n'));
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    qInfo() << "image after" << t.elapsed() << "ms (includes server start)";
    const QStringList log = servers.log(profile.id);
    for (const QString &line : log) {
        if (line.contains(QLatin1String("Version:")) || line.contains(QLatin1String("total params")) || line.contains(QLatin1String("sampling completed"))
            || line.contains(QLatin1String("decode")) || line.contains(QLatin1String("[E]")))
            qInfo().noquote() << "sd:" << line.trimmed();
    }
    save(op->result().images.value(0).image, QStringLiteral("anima_lumen.png"));
    delete op;
    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
}


// The reference scenario with both servers run by Lumen: LUMEN_LIVE_LLAMA_SERVER/_MODEL/_MMPROJ for the
// language model, LUMEN_LIVE_SD_* for sd.cpp, LUMEN_LIVE_REFERENCE. Servers start on first use
// through the providers' gates and share the GPU (llama.cpp sleeps when idle, sd.cpp keeps its
// weights in RAM between images).
void TestLive::managedStudioScenario()
{
    const QString llamaExe = qEnvironmentVariable("LUMEN_LIVE_LLAMA_SERVER");
    const QString llamaModel = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MODEL");
    const QString sdRoot = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString sdModel = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    if (llamaExe.isEmpty() || llamaModel.isEmpty() || sdRoot.isEmpty() || sdModel.isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_LLAMA_*, LUMEN_LIVE_SD_* and LUMEN_LIVE_REFERENCE are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().setValue(QStringLiteral("director/critic"), true);
    Settings::instance().setValue(QStringLiteral("director/criticThreshold"), 6);
    Settings::instance().setValue(QStringLiteral("director/criticRetries"), 1);
    QTemporaryDir dir;
    ui::Studio studio;
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    for (const runtime::ServerProfile &p : studio.servers()->profiles())
        studio.servers()->removeProfile(p.id);
    // Runtimes: a temporary list with the installed sd.cpp and the existing llama.cpp.
    studio.runtimes()->setRoot(dir.filePath(QStringLiteral("runtimes")));
    studio.runtimes()->load();
    {
        JobQueue jobs;
        runtime::RuntimeStore real(&m_http, &jobs);
        real.setRoot(sdRoot);
        real.load();
        const auto sd = real.latestManaged(QStringLiteral("sd.cpp"));
        QVERIFY(sd);
        studio.runtimes()->addInstalled(*sd);
    }
    QFuture<runtime::InstalledRuntime> llamaRt = studio.runtimes()->adopt(llamaExe);
    QTRY_VERIFY_WITH_TIMEOUT(llamaRt.isFinished(), 30000);
    runtime::ServerSupervisor *servers = studio.servers();

    runtime::ServerProfile llm = servers->createProfile(llamaRt.result().id, QStringLiteral("Qwen3-VL"));
    llm.options.insert(QStringLiteral("model"), llamaModel);
    if (!qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ").isEmpty())
        llm.options.insert(QStringLiteral("mmproj"), qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ"));
    // Optional text model next to the vision model (router mode, one loaded at a time).
    const QString textModel = qEnvironmentVariable("LUMEN_LIVE_LLAMA_TEXT_MODEL");
    if (!textModel.isEmpty())
        llm.options.insert(QStringLiteral("more_models"), QVariantList{QVariantMap{{QStringLiteral("model"), textModel}}});
    llm.startupTimeoutSec = 300;
    servers->addProfile(llm);
    runtime::ServerProfile image = servers->createProfile(studio.runtimes()->latestManaged(QStringLiteral("sd.cpp"))->id, QStringLiteral("SDXL"));
    image.options.insert(QStringLiteral("model"), sdModel);
    for (const auto &[key, env] : {std::pair{QStringLiteral("control_net"), "LUMEN_LIVE_SD_CONTROLNET"},
                                   std::pair{QStringLiteral("ip_adapter"), "LUMEN_LIVE_SD_IPADAPTER"},
                                   std::pair{QStringLiteral("clip_vision"), "LUMEN_LIVE_SD_CLIPVISION"}}) {
        if (!qEnvironmentVariable(env).isEmpty())
            image.options.insert(key, qEnvironmentVariable(env));
    }
    image.startupTimeoutSec = 300;
    servers->addProfile(image);

    Provider *sdProvider = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SDXL"),
                                                           {{QStringLiteral("managed_server"), image.id}});
    Provider *llmProvider = studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Qwen3-VL"),
                                                            {{QStringLiteral("managed_server"), llm.id}, {QStringLiteral("timeout_s"), 900}});
    QVERIFY(sdProvider && llmProvider);
    QVERIFY(sdProvider->gate() && llmProvider->gate());
    QVERIFY(sdProvider->setting(QStringLiteral("control_net")).toBool());
    studio.providers()->setDefaultImageProvider(sdProvider->instanceId());
    // Roles as Settings → Local servers assigns them: the vision model analyses references and
    // reviews results, the text model plans and talks.
    QString visionName;
    QString textName;
    for (const runtime::LlamaModel &m : runtime::llamaModels(*servers->profile(llm.id))) {
        if (m.vision() && visionName.isEmpty())
            visionName = m.name;
        if (!m.vision() && textName.isEmpty())
            textName = m.name;
    }
    if (textName.isEmpty())
        textName = visionName;
    for (LlmRole role : allLlmRoles())
        studio.providers()->setRole(role, {llmProvider->instanceId(), role == LlmRole::Vision ? visionName : textName});
    qInfo() << "roles: vision" << visionName << "text" << textName;
    QCOMPARE(llmProvider->text(QStringLiteral("model")), visionName);
    QCOMPARE(servers->state(llm.id), runtime::ServerState::Stopped);
    QCOMPARE(servers->state(image.id), runtime::ServerState::Stopped);

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
    qInfo() << "managed scenario:" << t.elapsed() << "ms";
    for (const ChatMessage &m : panel.history()) {
        if (m.role == ChatRole::Assistant && !m.toolCalls.isEmpty())
            for (const ToolCall &c : m.toolCalls)
                qInfo().noquote() << "  tool:" << c.name << QString::fromUtf8(QJsonDocument(c.arguments).toJson(QJsonDocument::Compact)).left(200);
        if (m.role == ChatRole::Tool)
            qInfo().noquote() << "  result:" << m.text.left(300);
    }
    qInfo().noquote() << "  reply:" << panel.lastReply();
    const QJsonObject state = studio.directorActions()->directorState();
    int images = 0;
    for (const QJsonValue &v : state.value(QStringLiteral("shots")).toArray()) {
        const QJsonObject shot = v.toObject();
        qInfo().noquote() << "  shot" << shot.value(QStringLiteral("id")).toString() << "best critic"
                          << shot.value(QStringLiteral("best_critic_score")).toInt(-1) << "results"
                          << shot.value(QStringLiteral("results")).toArray().size();
        for (const QJsonValue &id : shot.value(QStringLiteral("results")).toArray()) {
            auto img = studio.project()->loadImage(id.toString());
            QTRY_VERIFY_WITH_TIMEOUT(img.isFinished(), 10000);
            save(img.result(), QStringLiteral("managed_%1.png").arg(++images));
        }
    }
    const QStringList llmLog = servers->log(llm.id);
    const QStringList sdLog = servers->log(image.id);
    qInfo() << "  llama log lines" << llmLog.size() << "sd log lines" << sdLog.size();
    QVERIFY(!state.value(QStringLiteral("character")).toObject().isEmpty());
    QVERIFY(images >= 2);
    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    servers->stopAll();
    QTRY_VERIFY_WITH_TIMEOUT(servers->state(llm.id) == runtime::ServerState::Stopped
                                 && servers->state(image.id) == runtime::ServerState::Stopped, 30000);
    servers->removeProfile(llm.id);
    servers->removeProfile(image.id);
}

// LUMEN_LIVE_SD_RUNTIMES, LUMEN_LIVE_SD_MODEL, LUMEN_LIVE_SD_UPSCALERS=<folder with ESRGAN models>, LUMEN_LIVE_REFERENCE:
// 2x upscale of the reference through sd-server's /upscale.
void TestLive::managedSdUpscale()
{
    const QString root = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    const QString upscaler = qEnvironmentVariable("LUMEN_LIVE_SD_UPSCALERS");
    if (root.isEmpty() || model.isEmpty() || upscaler.isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_SD_RUNTIMES, LUMEN_LIVE_SD_MODEL, LUMEN_LIVE_SD_UPSCALERS and LUMEN_LIVE_REFERENCE are needed");
    QTemporaryDir tmp;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root);
    store.load();
    const auto rt = store.latestManaged(QStringLiteral("sd.cpp"));
    QVERIFY(rt);
    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(tmp.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(tmp.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt->id, QStringLiteral("Upscale"));
    profile.options.insert(QStringLiteral("model"), model);
    profile.options.insert(QStringLiteral("upscalers_dir"), upscaler);
    servers.addProfile(profile);
    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("live-up"), &m_http, nullptr);
    sd.configure({{QStringLiteral("url"), runtime::providerUrl(rt->kind, profile.port)}, {QStringLiteral("upscaler"), true}});
    sd.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));
    const QImage input(referencePath());
    QElapsedTimer t;
    t.start();
    ImageOperation *op = gatedUtility(&sd, Utility::Upscale, input, {{QStringLiteral("factor"), 2.0}});
    op->setAutoDelete(false);
    QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 600000);
    if (op->state() != Operation::State::Succeeded)
        qInfo().noquote() << servers.log(profile.id).mid(std::max<qsizetype>(0, servers.log(profile.id).size() - 20)).join(QLatin1Char('\n'));
    QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
    const QImage out = op->result().images.value(0).image;
    qInfo() << "upscaled" << input.size() << "->" << out.size() << "in" << t.elapsed() << "ms (includes server start), with"
            << op->result().info.value(QStringLiteral("upscaler")).toString();
    QCOMPARE(out.size(), input.size() * 2);
    save(out, QStringLiteral("sdcpp_upscaled.png"));
    delete op;
    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
}

// LUMEN_LIVE_COMFY_APP=<ComfyUI main.py or folder>: Lumen adopts the installation (e.g. ComfyUI
// Desktop), starts it, talks to it through the ComfyUI provider and stops it again.
void TestLive::managedComfyUi()
{
    const QString app = qEnvironmentVariable("LUMEN_LIVE_COMFY_APP");
    if (app.isEmpty())
        QSKIP("LUMEN_LIVE_COMFY_APP is not set");
    QTemporaryDir tmp;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(tmp.filePath(QStringLiteral("runtimes")));
    QFuture<runtime::InstalledRuntime> adopted = store.adopt(app);
    QTRY_VERIFY_WITH_TIMEOUT(adopted.isFinished(), 10000);
    const runtime::InstalledRuntime rt = adopted.result();
    qInfo() << "adopted" << rt.displayName() << rt.executable << rt.settings;
    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(tmp.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(tmp.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt.id, QStringLiteral("ComfyUI"));
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);
    qInfo().noquote() << "command:" << servers.launchFor(profile.id).commandLine();
    ComfyUiProvider comfy(ComfyUiProvider::describe(), QStringLiteral("live-comfy"), &m_http, nullptr);
    comfy.configure({{QStringLiteral("url"), runtime::providerUrl(rt.kind, profile.port)}});
    comfy.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));
    QElapsedTimer t;
    t.start();
    QFuture<QString> test = gatedCall(&comfy, [&comfy] { return comfy.testConnection(); });
    QTRY_VERIFY_WITH_TIMEOUT(test.isFinished(), 300000);
    if (servers.state(profile.id) != runtime::ServerState::Ready)
        qInfo().noquote() << servers.log(profile.id).mid(std::max<qsizetype>(0, servers.log(profile.id).size() - 30)).join(QLatin1Char('\n'));
    qInfo() << "ComfyUI answered after" << t.elapsed() << "ms:" << test.result();
    QCOMPARE(servers.state(profile.id), runtime::ServerState::Ready);
    QFuture<QStringList> models = gatedList(&comfy, [&comfy] { return comfy.listModels(); });
    QTRY_VERIFY_WITH_TIMEOUT(models.isFinished(), 60000);
    qInfo() << "checkpoints:" << models.result();
    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
    QVERIFY(runtime::portIsFree(profile.port));
}

// LUMEN_LIVE_HUB=1: searches both hubs and downloads the smallest starter file (Real-ESRGAN x4plus,
// 64 MB) from Hugging Face through its CDN redirect, checked against the pinned SHA-256.
void TestLive::hubDownload()
{
    if (!qEnvironmentVariableIntValue("LUMEN_LIVE_HUB"))
        QSKIP("set LUMEN_LIVE_HUB=1");
    net::HttpClient http;
    JobQueue jobs;
    QTemporaryDir data;
    runtime::ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();
    runtime::ModelHub hub(&http, &jobs, &lib);
    hub.tokenProvider = [](runtime::HubSource) { return QString(); };

    runtime::HubQuery q;
    q.text = QStringLiteral("Qwen3-VL-8B-Instruct-GGUF");
    q.category = QStringLiteral("llm");
    QFuture<runtime::HubPage> page = hub.search(runtime::HubSource::HuggingFace, q);
    QTRY_VERIFY_WITH_TIMEOUT(page.isFinished(), 30000);
    QVERIFY(!page.result().models.isEmpty());
    QVERIFY(!page.result().nextCursor.isEmpty() || page.result().models.size() < q.limit);
    runtime::HubModel qwen;
    for (const runtime::HubModel &m : page.result().models) {
        if (m.id == QLatin1String("Qwen/Qwen3-VL-8B-Instruct-GGUF"))
            qwen = m;
    }
    QVERIFY(!qwen.id.isEmpty());
    QFuture<runtime::HubModel> details = hub.details(qwen);
    QTRY_VERIFY_WITH_TIMEOUT(details.isFinished(), 30000);
    const QList<runtime::HubFile> files = details.result().versions.value(0).files;
    runtime::HubFile q4;
    for (const runtime::HubFile &f : files) {
        if (f.path == QLatin1String("Qwen3VL-8B-Instruct-Q4_K_M.gguf"))
            q4 = f;
    }
    QCOMPARE(q4.sha256, QStringLiteral("67d1659bfe71b89d50b45a4ad1a9e5b997e5bb16ce5da66a6a6167abd569e9e2"));
    QCOMPARE(runtime::projectorFor(files, q4)->path, QStringLiteral("mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf"));

    q.text = QStringLiteral("openpose");
    q.category = QStringLiteral("controlnet");
    QFuture<runtime::HubPage> civ = hub.search(runtime::HubSource::Civitai, q);
    QTRY_VERIFY_WITH_TIMEOUT(civ.isFinished(), 30000);
    QVERIFY(!civ.result().models.isEmpty());
    for (const runtime::HubModel &m : civ.result().models)
        QVERIFY(!m.nsfw);

    const QList<runtime::StarterSet> sets = runtime::starterSets();
    const runtime::StarterFile *up = sets.first().file(QStringLiteral("upscaler"));
    QVERIFY(up);
    QElapsedTimer t;
    t.start();
    Job *job = hub.download(runtime::starterDownload(*up));
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 600000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    const auto entry = lib.entry(job->result().toString());
    QVERIFY(entry);
    QCOMPARE(entry->info.kind, runtime::ModelKind::Upscaler);
    QCOMPARE(entry->sha256, up->sha256);
    qInfo() << "downloaded" << entry->path << entry->size << "bytes in" << t.elapsed() << "ms";
}

// LUMEN_LIVE_SD_LORA=<file.safetensors> (with LUMEN_LIVE_SD_RUNTIMES / _MODEL): the same seed with and
// without the LoRA through a Lumen-run sd-server whose LoRA folder holds the file, and a request
// for a LoRA the server does not have.
void TestLive::managedSdLora()
{
    const QString root = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    const QString loraFile = qEnvironmentVariable("LUMEN_LIVE_SD_LORA");
    if (root.isEmpty() || model.isEmpty() || loraFile.isEmpty())
        QSKIP("LUMEN_LIVE_SD_RUNTIMES, LUMEN_LIVE_SD_MODEL and LUMEN_LIVE_SD_LORA are needed");
    QTemporaryDir tmp;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root);
    store.load();
    const auto rt = store.latestManaged(QStringLiteral("sd.cpp"));
    QVERIFY(rt);
    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(tmp.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(tmp.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt->id, QStringLiteral("Live sd.cpp LoRA"));
    profile.options = runtime::presetOptions(QStringLiteral("sd.cpp"), runtime::VramTier::Small);
    profile.options.insert(QStringLiteral("model"), model);
    profile.options.insert(QStringLiteral("lora_dir"), QFileInfo(loraFile).absolutePath());
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);
    qInfo().noquote() << "command:" << servers.launchFor(profile.id).commandLine();
    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("live-sd"), &m_http, nullptr);
    sd.configure({{QStringLiteral("url"), runtime::providerUrl(rt->kind, profile.port)}});
    sd.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));

    ImageRequest r;
    r.prompt = QStringLiteral("1girl, solo, red hair, black robe, upper body, smile, simple background, masterpiece, best quality");
    r.negativePrompt = QStringLiteral("lowres, bad anatomy, worst quality");
    r.size = QSize(1024, 1024);
    r.seed = 11;
    r.steps = 20;
    r.cfg = 6.0;
    r.sampler = QStringLiteral("euler_a");
    auto run = [&](const ImageRequest &req, const QString &name) {
        QElapsedTimer t;
        t.start();
        ImageOperation *op = gatedGenerate(&sd, req);
        op->setAutoDelete(false);
        if (!QTest::qWaitFor([op] { return op->isFinished(); }, 900000))
            return std::make_pair(QImage(), QStringLiteral("timeout"));
        const QString error = op->state() == Operation::State::Succeeded ? QString() : op->errorString();
        const QImage image = op->result().images.value(0).image;
        qInfo() << name << "after" << t.elapsed() << "ms" << error;
        if (!image.isNull())
            save(image, name);
        delete op;
        return std::make_pair(image, error);
    };
    const auto plain = run(r, QStringLiteral("sdcpp_lora_off.png"));
    QVERIFY2(plain.second.isEmpty(), qPrintable(plain.second));

    CharacterLora lora;
    lora.file = loraFile;
    lora.trigger = qEnvironmentVariable("LUMEN_LIVE_SD_LORA_TRIGGER", QStringLiteral("pixel art"));
    lora.weight = 1.0;
    ImageRequest withLora = r;
    QVERIFY(Director::applyCharacterLora(withLora, lora, sd.capabilities()));
    const auto styled = run(withLora, QStringLiteral("sdcpp_lora_on.png"));
    QVERIFY2(styled.second.isEmpty(), qPrintable(styled.second));
    // The same seed renders a clearly different image with the LoRA.
    const QImage a = plain.first.convertToFormat(QImage::Format_RGB32).scaled(128, 128);
    const QImage b = styled.first.convertToFormat(QImage::Format_RGB32).scaled(128, 128);
    double diff = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            diff += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    }
    diff /= 128.0 * 128 * 3;
    qInfo() << "mean pixel difference with the LoRA:" << diff;
    QVERIFY(diff > 8.0);
    const QStringList log = servers.log(profile.id);
    qInfo().noquote() << "server log (LoRA lines):" << log.filter(QStringLiteral("lora"), Qt::CaseInsensitive).mid(0, 6).join(QLatin1Char('\n'));

    // A LoRA that is not in the server's folder fails before anything is generated.
    ImageRequest missing = r;
    LoraRef ghost;
    ghost.name = QStringLiteral("no_such_lora");
    missing.loras << ghost;
    const auto failed = run(missing, QStringLiteral("sdcpp_lora_missing.png"));
    QVERIFY(failed.second.contains(QStringLiteral("no_such_lora")));

    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
}

// LUMEN_LIVE_DATASET_SHOTS=<n> with LUMEN_LIVE_SD_* (ControlNet + IP-Adapter) and LUMEN_LIVE_REFERENCE:
// the first n shots of a LoRA training plan through a Lumen-run sd-server, saved with a contact sheet.
// LUMEN_LIVE_DATASET_TAGS overrides the character's appearance tags.
void TestLive::managedDatasetShots()
{
    const QString root = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    const int count = qEnvironmentVariableIntValue("LUMEN_LIVE_DATASET_SHOTS");
    if (root.isEmpty() || model.isEmpty() || count <= 0 || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_DATASET_SHOTS, LUMEN_LIVE_SD_* and LUMEN_LIVE_REFERENCE are needed");
    QTemporaryDir tmp;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root);
    store.load();
    const auto rt = store.latestManaged(QStringLiteral("sd.cpp"));
    QVERIFY(rt);
    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(tmp.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(tmp.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(rt->id, QStringLiteral("Live dataset"));
    profile.options = runtime::presetOptions(QStringLiteral("sd.cpp"), runtime::VramTier::Small);
    profile.options.insert(QStringLiteral("model"), model);
    profile.options.insert(QStringLiteral("control_net"), qEnvironmentVariable("LUMEN_LIVE_SD_CONTROLNET"));
    profile.options.insert(QStringLiteral("ip_adapter"), qEnvironmentVariable("LUMEN_LIVE_SD_IPADAPTER"));
    profile.options.insert(QStringLiteral("clip_vision"), qEnvironmentVariable("LUMEN_LIVE_SD_CLIPVISION"));
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);
    SdCppProvider sd(SdCppProvider::describe(), QStringLiteral("live-sd"), &m_http, nullptr);
    sd.configure({{QStringLiteral("url"), runtime::providerUrl(rt->kind, profile.port)},
                  {QStringLiteral("control_net"), true},
                  {QStringLiteral("ip_adapter"), true}});
    sd.setGate(std::make_shared<SupervisorGate>(&servers, profile.id));

    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Live character")},
                            {QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), qEnvironmentVariable("LUMEN_LIVE_DATASET_TAGS",
                                                                          QStringLiteral("long red hair, yellow eyes, black hooded cloak, black choker"))}};
    const QJsonArray shots = Director::datasetPlan(sheet, count).value(QStringLiteral("shots")).toArray();
    ReferenceImage ref;
    ref.image = QImage(referencePath());
    ref.role = ReferenceRole::Character;
    QVERIFY(!ref.image.isNull());
    const ImageCapabilities caps = sd.capabilities();
    PromptStyle style;
    style.qualityPrefix = QStringLiteral("masterpiece, best quality");
    style.defaultNegative = QStringLiteral("lowres, bad anatomy, bad hands, extra fingers, worst quality");
    QList<QImage> results;
    QElapsedTimer total;
    total.start();
    for (const QJsonValue &v : shots) {
        const QJsonObject shot = v.toObject();
        const QSize size = Director::sizeForAspect(shot.value(QStringLiteral("aspect_ratio")).toString(), caps.nativePixels, caps.sizeMultiple);
        ImageRequest r = Director::buildShotRequest(shot, {ref}, Director::poseForShot(shot, size), style, caps);
        r.seed = 21;
        r.steps = 24;
        r.sampler = QStringLiteral("euler_a");
        QElapsedTimer t;
        t.start();
        ImageOperation *op = gatedGenerate(&sd, r);
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 900000);
        QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
        const QImage image = op->result().images.value(0).image;
        qInfo().noquote() << shot.value(QStringLiteral("id")).toString() << shot.value(QStringLiteral("title")).toString() << r.size << t.elapsed() << "ms";
        save(image, QStringLiteral("dataset_%1.png").arg(shot.value(QStringLiteral("id")).toString()));
        if (!r.controls.isEmpty())
            save(r.controls.first().image, QStringLiteral("dataset_%1_pose.png").arg(shot.value(QStringLiteral("id")).toString()));
        results << image;
        delete op;
    }
    qInfo() << shots.size() << "shots in" << total.elapsed() / 1000 << "s";
    // Contact sheet: 256 px tiles.
    const int columns = 4;
    QImage sheetImage(columns * 256, ((int(results.size()) + columns - 1) / columns) * 256, QImage::Format_RGB32);
    sheetImage.fill(Qt::white);
    QPainter p(&sheetImage);
    for (int i = 0; i < results.size(); ++i) {
        const QImage tile = results[i].scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((i % columns) * 256 + (256 - tile.width()) / 2, (i / columns) * 256 + (256 - tile.height()) / 2, tile);
    }
    p.end();
    save(sheetImage, QStringLiteral("dataset_contact_sheet.png"));
    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
}

// LUMEN_LIVE_CAPTION_DIR=<folder with dataset_ds*.png> with LUMEN_LIVE_LLAMA_SERVER / _MODEL / _MMPROJ:
// the vision model tags each training image through a Lumen-run llama-server, and the captions are
// built with the character's look pruned (LUMEN_LIVE_DATASET_TAGS = the character's tags).
void TestLive::managedCaptions()
{
    const QString folder = qEnvironmentVariable("LUMEN_LIVE_CAPTION_DIR");
    const QString exe = qEnvironmentVariable("LUMEN_LIVE_LLAMA_SERVER");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MODEL");
    const QString mmproj = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ");
    if (folder.isEmpty() || exe.isEmpty() || model.isEmpty() || mmproj.isEmpty())
        QSKIP("LUMEN_LIVE_CAPTION_DIR and LUMEN_LIVE_LLAMA_SERVER / _MODEL / _MMPROJ are needed");
    QTemporaryDir root;
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root.filePath(QStringLiteral("runtimes")));
    QFuture<runtime::InstalledRuntime> adopted = store.adopt(exe);
    QVERIFY(waitFor(adopted, 30000));
    runtime::ServerSupervisor servers(&store, &m_http);
    servers.setConfigPath(root.filePath(QStringLiteral("servers.json")));
    servers.setLogDir(root.filePath(QStringLiteral("logs")));
    runtime::ServerProfile profile = servers.createProfile(adopted.result().id, QStringLiteral("Live tagger"));
    profile.options = runtime::presetOptions(QStringLiteral("llama.cpp"), runtime::VramTier::Small);
    profile.options.insert(QStringLiteral("model"), model);
    profile.options.insert(QStringLiteral("mmproj"), mmproj);
    profile.startupTimeoutSec = 300;
    servers.addProfile(profile);

    std::unique_ptr<ProviderRegistry> registry(new ProviderRegistry(&m_http));
    registerBuiltInProviders(*registry);
    for (Provider *p : registry->providers())
        registry->removeProvider(p->instanceId());
    Provider *llm = registry->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Tagger"),
                                          {{QStringLiteral("url"), runtime::providerUrl(QStringLiteral("llama.cpp"), profile.port)},
                                           {QStringLiteral("timeout_s"), 600}});
    QVERIFY(llm);
    llm->setGate(std::make_shared<SupervisorGate>(&servers, profile.id));
    registry->setRole(LlmRole::Vision, {llm->instanceId(), runtime::llamaModelName(model)});
    Director director(registry.get());

    const QJsonObject sheet{{QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), qEnvironmentVariable("LUMEN_LIVE_DATASET_TAGS",
                                                                          QStringLiteral("long red hair, yellow eyes, black hooded cloak, black choker"))}};
    const QStringList files = QDir(folder).entryList({QStringLiteral("dataset_ds*.png")}, QDir::Files, QDir::Name).filter(QRegularExpression(QStringLiteral("ds\\d+\\.png$")));
    QVERIFY(!files.isEmpty());
    int pruned = 0;
    QElapsedTimer total;
    total.start();
    for (const QString &f : files) {
        QElapsedTimer t;
        t.start();
        StructuredChatOperation *op = director.tagImage(QImage(QDir(folder).filePath(f)));
        op->setAutoDelete(false);
        QTRY_VERIFY_WITH_TIMEOUT(op->isFinished(), 600000);
        QVERIFY2(op->state() == Operation::State::Succeeded, qPrintable(op->errorString()));
        QStringList tags;
        for (const QJsonValue &v : op->result().value(QStringLiteral("tags")).toArray())
            tags << modelTag(v.toString());
        const QString caption = buildCaption(tags, sheet, QStringLiteral("lmn_live"), true);
        pruned += int(tags.size()) - int(splitTags(caption).size()) + 1;
        qInfo().noquote() << f << t.elapsed() << "ms\n  tags:   " << tags.join(QStringLiteral(", ")) << "\n  caption:" << caption;
        QVERIFY(caption.startsWith(QStringLiteral("lmn_live, ")));
        QVERIFY(!caption.contains(QStringLiteral("red hair")));
        QVERIFY(!caption.contains(QStringLiteral("yellow eyes")));
        delete op;
    }
    qInfo() << files.size() << "images tagged in" << total.elapsed() / 1000 << "s;" << pruned << "identity tags pruned";
    servers.stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers.state(profile.id), runtime::ServerState::Stopped, 30000);
}

// LUMEN_LIVE_TRAINER=<trainer>:<variant>, e.g. "kohya:cu128": installs a LoRA trainer for real (downloads
// uv, Python and PyTorch, several GB!). LUMEN_LIVE_INSTALL_ROOT keeps it (e.g. the app's runtimes folder).
void TestLive::installTrainer()
{
    const QString what = qEnvironmentVariable("LUMEN_LIVE_TRAINER");
    if (what.isEmpty())
        QSKIP("LUMEN_LIVE_TRAINER is not set");
    QTemporaryDir tmp;
    const QString keep = qEnvironmentVariable("LUMEN_LIVE_INSTALL_ROOT");
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(keep.isEmpty() ? tmp.filePath(QStringLiteral("runtimes")) : keep);
    store.load();
    QElapsedTimer t;
    t.start();
    Job *job = store.installTrainer(what.section(QLatin1Char(':'), 0, 0), what.section(QLatin1Char(':'), 1));
    QString last;
    connect(job, &Job::progressChanged, this, [&last](double p, const QString &status) {
        if (status.section(QStringLiteral(" — "), 0, 0) != last.section(QStringLiteral(" — "), 0, 0))
            qInfo().noquote() << int(p * 100) << "%" << status.left(160);
        last = status;
    });
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 3 * 3600 * 1000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    const auto rt = store.runtime(job->result().toString());
    QVERIFY(rt);
    qint64 bytes = 0;
    QDirIterator it(rt->directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        bytes += it.fileInfo().size();
    }
    qInfo().noquote() << "installed in" << t.elapsed() / 1000 << "s:" << rt->versionInfo << "·" << runtime::formatBytes(bytes) << "on disk";
}

// LUMEN_LIVE_TRAIN_IMAGES=<folder with dataset_ds*.png> with LUMEN_LIVE_SD_MODEL (SDXL checkpoint) and an
// installed trainer in LUMEN_LIVE_SD_RUNTIMES: trains a LoRA for LUMEN_LIVE_TRAIN_EPOCHS (1) epochs with
// LUMEN_LIVE_TRAIN_REPEATS (2) repeats and reports speed and peak GPU memory (nvidia-smi).
// LUMEN_LIVE_TRAIN_WORK keeps the run folder.
void TestLive::trainLora()
{
    const QString images = qEnvironmentVariable("LUMEN_LIVE_TRAIN_IMAGES");
    const QString model = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    const QString root = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    if (images.isEmpty() || model.isEmpty() || root.isEmpty())
        QSKIP("LUMEN_LIVE_TRAIN_IMAGES, LUMEN_LIVE_SD_MODEL and LUMEN_LIVE_SD_RUNTIMES are needed");
    const int epochs = std::max(1, qEnvironmentVariableIntValue("LUMEN_LIVE_TRAIN_EPOCHS"));
    const int repeats = std::max(1, qEnvironmentVariable("LUMEN_LIVE_TRAIN_REPEATS", QStringLiteral("2")).toInt());
    QTemporaryDir tmp;
    const QString work = qEnvironmentVariable("LUMEN_LIVE_TRAIN_WORK", tmp.path());
    JobQueue jobs;
    runtime::RuntimeStore store(&m_http, &jobs);
    store.setRoot(root);
    store.load();
    const QList<runtime::InstalledRuntime> trainers = store.runtimes(QStringLiteral("kohya"));
    QVERIFY2(!trainers.isEmpty(), "no trainer installed");

    // Training folder: "<repeats>_<trigger>" with the images and a caption each.
    const QString trigger = QStringLiteral("lmn_live");
    const QString folder = QDir(work).filePath(QStringLiteral("img/%1_%2").arg(repeats).arg(trigger));
    QDir(folder).removeRecursively();
    QDir().mkpath(folder);
    int n = 0;
    const QStringList files = QDir(images).entryList({QStringLiteral("dataset_ds*.png")}, QDir::Files, QDir::Name)
                                  .filter(QRegularExpression(QStringLiteral("ds\\d+\\.png$")));
    for (const QString &f : files) {
        const QString base = QDir(folder).filePath(QStringLiteral("%1").arg(++n, 3, 10, QLatin1Char('0')));
        QVERIFY(QFile::copy(QDir(images).filePath(f), base + QStringLiteral(".png")));
        QFile caption(base + QStringLiteral(".txt"));
        QVERIFY(caption.open(QIODevice::WriteOnly));
        caption.write("lmn_live, 1girl, solo, simple background");
    }
    QVERIFY(n > 0);

    runtime::TrainingOptions o;
    o.runtimeId = trainers.first().id;
    o.checkpoint = model;
    o.family = QStringLiteral("SDXL");
    o.imagesDir = QDir(work).filePath(QStringLiteral("img"));
    o.workDir = work;
    o.outputName = trigger;
    o.trigger = trigger;
    o.epochs = epochs;
    o.tier = runtime::VramTier::Small;
    o.title = QStringLiteral("Lumen live test");
    qInfo().noquote() << "args:" << runtime::trainingArguments(o, trainers.first()).join(QLatin1Char(' '));

    // Peak GPU memory while the trainer runs.
    int peakMiB = 0;
    QTimer sampler;
    sampler.setInterval(3000);
    connect(&sampler, &QTimer::timeout, this, [&peakMiB] {
        QProcess smi;
        smi.start(QStringLiteral("nvidia-smi"), {QStringLiteral("--query-gpu=memory.used"), QStringLiteral("--format=csv,noheader,nounits")});
        if (smi.waitForFinished(3000))
            peakMiB = std::max(peakMiB, QString::fromLatin1(smi.readAllStandardOutput()).trimmed().toInt());
    });
    sampler.start();
    QElapsedTimer t;
    t.start();
    Job *job = runtime::startTraining(&store, nullptr, &jobs, o);
    QString lastPhase;
    connect(job, &Job::progressChanged, this, [&lastPhase](double, const QString &status) {
        const QString phase = status.section(QStringLiteral(" · "), 0, 0);
        if (phase != lastPhase)
            qInfo().noquote() << "  " << status;
        lastPhase = phase;
    });
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 4 * 3600 * 1000);
    sampler.stop();
    if (job->state() != Job::State::Succeeded) {
        QFile log(QDir(work).filePath(QStringLiteral("train.log")));
        if (log.open(QIODevice::ReadOnly))
            qInfo().noquote() << QString::fromUtf8(log.readAll()).right(4000);
    }
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    const QVariantMap result = job->result().toMap();
    qInfo().noquote() << n << "images x" << repeats << "repeats x" << epochs << "epochs in" << t.elapsed() / 1000 << "s;"
                      << job->metadata().value(QStringLiteral("secondsPerStep")).toDouble() << "s/step; loss"
                      << job->metadata().value(QStringLiteral("loss")).toDouble() << "; peak GPU memory" << peakMiB << "MiB";
    qInfo().noquote() << "files:" << result.value(QStringLiteral("files")).toStringList().join(QStringLiteral(", "));
    const runtime::ModelInfo info = runtime::inspectModel(result.value(QStringLiteral("final")).toString());
    QCOMPARE(info.kind, runtime::ModelKind::Lora);
    QCOMPARE(info.family, QStringLiteral("SDXL"));
    qInfo() << "LoRA:" << QFileInfo(result.value(QStringLiteral("final")).toString()).size() / 1e6 << "MB";
}

// LUMEN_LIVE_EVAL_RUN=<a trainLora work folder with output/lmn_live-*.safetensors>, LUMEN_LIVE_EVAL_EPOCHS
// (its epoch count), LUMEN_LIVE_LLAMA_* for the Critic, LUMEN_LIVE_SD_* and LUMEN_LIVE_REFERENCE: the LoRA
// test as the Train tab runs it, with both servers run by Lumen. Compares the epochs with the
// baseline (IP-Adapter and appearance tags, no LoRA).
void TestLive::evaluateLora()
{
    const QString trained = qEnvironmentVariable("LUMEN_LIVE_EVAL_RUN");
    const int epochs = qEnvironmentVariableIntValue("LUMEN_LIVE_EVAL_EPOCHS");
    const QString llamaExe = qEnvironmentVariable("LUMEN_LIVE_LLAMA_SERVER");
    const QString llamaModel = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MODEL");
    const QString sdRoot = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString sdModel = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    if (trained.isEmpty() || epochs <= 0 || llamaExe.isEmpty() || llamaModel.isEmpty() || sdRoot.isEmpty() || sdModel.isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_EVAL_RUN / _EPOCHS, LUMEN_LIVE_LLAMA_*, LUMEN_LIVE_SD_* and LUMEN_LIVE_REFERENCE are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    ui::Studio studio;
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    for (const runtime::ServerProfile &p : studio.servers()->profiles())
        studio.servers()->removeProfile(p.id);
    studio.runtimes()->setRoot(dir.filePath(QStringLiteral("runtimes")));
    studio.runtimes()->load();
    {
        JobQueue jobs;
        runtime::RuntimeStore real(&m_http, &jobs);
        real.setRoot(sdRoot);
        real.load();
        const auto sd = real.latestManaged(QStringLiteral("sd.cpp"));
        QVERIFY(sd);
        studio.runtimes()->addInstalled(*sd);
    }
    QFuture<runtime::InstalledRuntime> llamaRt = studio.runtimes()->adopt(llamaExe);
    QTRY_VERIFY_WITH_TIMEOUT(llamaRt.isFinished(), 30000);
    runtime::ServerSupervisor *servers = studio.servers();
    runtime::ServerProfile llm = servers->createProfile(llamaRt.result().id, QStringLiteral("Qwen3-VL"));
    llm.options.insert(QStringLiteral("model"), llamaModel);
    if (!qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ").isEmpty())
        llm.options.insert(QStringLiteral("mmproj"), qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ"));
    llm.startupTimeoutSec = 300;
    servers->addProfile(llm);
    runtime::ServerProfile image = servers->createProfile(studio.runtimes()->latestManaged(QStringLiteral("sd.cpp"))->id, QStringLiteral("SDXL"));
    image.options.insert(QStringLiteral("model"), sdModel);
    for (const auto &[key, env] : {std::pair{QStringLiteral("control_net"), "LUMEN_LIVE_SD_CONTROLNET"},
                                   std::pair{QStringLiteral("ip_adapter"), "LUMEN_LIVE_SD_IPADAPTER"},
                                   std::pair{QStringLiteral("clip_vision"), "LUMEN_LIVE_SD_CLIPVISION"}}) {
        if (!qEnvironmentVariable(env).isEmpty())
            image.options.insert(key, qEnvironmentVariable(env));
    }
    image.startupTimeoutSec = 300;
    servers->addProfile(image);
    Provider *sdProvider = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SDXL"), {{QStringLiteral("managed_server"), image.id}});
    Provider *llmProvider = studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Qwen3-VL"),
                                                            {{QStringLiteral("managed_server"), llm.id}, {QStringLiteral("timeout_s"), 900}});
    QVERIFY(sdProvider && llmProvider);
    studio.providers()->setDefaultImageProvider(sdProvider->instanceId());
    QString visionName;
    for (const runtime::LlamaModel &m : runtime::llamaModels(*servers->profile(llm.id))) {
        if (m.vision() && visionName.isEmpty())
            visionName = m.name;
    }
    for (LlmRole role : allLlmRoles())
        studio.providers()->setRole(role, {llmProvider->instanceId(), visionName});

    // The character and a run folder with the trained epochs, as CharacterTraining leaves them.
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Live character")},
                            {QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), qEnvironmentVariable("LUMEN_LIVE_DATASET_TAGS",
                                                                          QStringLiteral("long red hair, yellow eyes, black hooded cloak, black choker"))}};
    auto saved = studio.characters()->save(QStringLiteral("Live LoRA evaluation"), sheet, {QImage(referencePath())});
    QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 10000);
    const QString characterId = saved.result().id;
    auto cleanup = qScopeGuard([&] {
        studio.characters()->remove(characterId);
        for (Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        servers->stopAll();
    });
    const QString runDir = QDir(studio.characters()->datasetDirectory(characterId)).filePath(QStringLiteral("training/run-live"));
    QDir().mkpath(QDir(runDir).filePath(QStringLiteral("output")));
    for (const QString &f : QDir(QDir(trained).filePath(QStringLiteral("output"))).entryList({QStringLiteral("lmn_live*.safetensors")}, QDir::Files))
        QVERIFY(QFile::copy(QDir(trained).filePath(QStringLiteral("output/") + f), QDir(runDir).filePath(QStringLiteral("output/") + f)));
    {
        QFile f(QDir(runDir).filePath(QStringLiteral("run.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("checkpoint"), sdModel},
                                          {QStringLiteral("family"), QStringLiteral("SDXL")},
                                          {QStringLiteral("epochs"), epochs},
                                          {QStringLiteral("trigger"), QStringLiteral("lmn_live")},
                                          {QStringLiteral("outputName"), QStringLiteral("lmn_live")}})
                    .toJson());
    }
    qInfo() << "epochs:" << ui::LoraEvaluation::epochFiles(runDir).size();

    QElapsedTimer t;
    t.start();
    QString error;
    ui::LoraEvaluation *e = ui::LoraEvaluation::start(&studio, characterId, runDir, &error);
    QVERIFY2(e, qPrintable(error));
    bool ok = false;
    QString message;
    connect(e, &ui::LoraEvaluation::finished, this, [&](bool success, const QString &m) {
        ok = success;
        message = m;
    });
    QString lastStatus;
    connect(e, &ui::LoraEvaluation::changed, this, [&, e] {
        if (e->status() != lastStatus)
            qInfo().noquote() << "  " << (t.elapsed() / 1000) << "s" << e->status();
        lastStatus = e->status();
    });
    QTRY_VERIFY_WITH_TIMEOUT(!message.isEmpty(), 3 * 3600 * 1000);
    qInfo().noquote() << message << "in" << t.elapsed() / 1000 << "s";
    QVERIFY2(ok, qPrintable(message));

    // The grid as numbers and as a contact sheet (rows: candidates, columns: shots).
    const QJsonObject result = ui::LoraEvaluation::saved(runDir);
    QList<QPair<int, double>> rows;
    for (const QJsonValue &v : result.value(QStringLiteral("cells")).toArray()) {
        const QPair<int, double> key{v[QStringLiteral("epoch")].toInt(), v[QStringLiteral("weight")].toDouble()};
        if (!rows.contains(key))
            rows << key;
    }
    QImage grid(4 * 256, int(rows.size()) * 256, QImage::Format_RGB32);
    grid.fill(Qt::white);
    QPainter p(&grid);
    QFont f = p.font();
    f.setPixelSize(18);
    p.setFont(f);
    QMap<QPair<int, double>, QList<double>> scores;
    for (const QJsonValue &v : result.value(QStringLiteral("cells")).toArray()) {
        const QPair<int, double> key{v[QStringLiteral("epoch")].toInt(), v[QStringLiteral("weight")].toDouble()};
        const int row = int(rows.indexOf(key));
        const int column = v[QStringLiteral("shot")].toString().mid(1).toInt() - 1;
        const QImage tile = QImage(v[QStringLiteral("image")].toString()).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage(column * 256 + (256 - tile.width()) / 2, row * 256 + (256 - tile.height()) / 2, tile);
        const QJsonObject c = v[QStringLiteral("critique")].toObject();
        const QString label = QStringLiteral("%1 e%2 w%3  id %4 pose %5")
                                  .arg(v[QStringLiteral("score")].toDouble(), 0, 'f', 1)
                                  .arg(key.first)
                                  .arg(key.second)
                                  .arg(c.value(QStringLiteral("identity")).toInt())
                                  .arg(c.value(QStringLiteral("pose")).toInt());
        p.fillRect(QRect(column * 256, row * 256, 256, 24), QColor(0, 0, 0, 160));
        p.setPen(Qt::white);
        p.drawText(QRect(column * 256 + 4, row * 256, 252, 24), Qt::AlignVCenter, label);
        scores[key] << v[QStringLiteral("score")].toDouble();
        qInfo().noquote() << "  " << label << v[QStringLiteral("shot")].toString()
                          << c.value(QStringLiteral("issues")).toVariant().toStringList().join(QStringLiteral("; ")).left(200);
    }
    p.end();
    save(grid, QStringLiteral("lora_evaluation.png"));
    for (const auto &key : rows) {
        double sum = 0;
        for (double s : scores[key])
            sum += s;
        qInfo().noquote() << (key.first == 0 ? QStringLiteral("baseline") : QStringLiteral("epoch %1 at %2").arg(key.first).arg(key.second))
                          << "mean" << sum / scores[key].size();
    }
    const int best = result.value(QStringLiteral("bestEpoch")).toInt();
    QVERIFY(best > 0);
}

// LUMEN_LIVE_FULL_LORA=<shots> with LUMEN_LIVE_LLAMA_*, LUMEN_LIVE_SD_*, LUMEN_LIVE_REFERENCE and an
// installed trainer in LUMEN_LIVE_SD_RUNTIMES: M13's acceptance run through the UI, with both servers
// run by Lumen. Create → Training set → plan and generate the shots → Critic scores → captions →
// Train tab (LUMEN_LIVE_FULL_EPOCHS, default 10) → Test the LoRA → attach the recommended epoch.
// LUMEN_LIVE_FULL_CHARACTER=<library id> starts at the Train tab with that character's training set
// (from an earlier run of this test).
// M15 step 5: a character LoRA trained on a rented RunPod GPU through CharacterTraining (as the
// Train tab does). It costs money: LUMEN_LIVE_RUNPOD=1 and a RunPod key in the credential store.
// LUMEN_LIVE_CLOUD_CHARACTER=<character id with a training set>, LUMEN_LIVE_CLOUD_CHECKPOINT=<base
// model>, optional LUMEN_LIVE_CLOUD_CHECKPOINT_URL + _SHA256 (a Hugging Face copy the pod fetches),
// LUMEN_LIVE_CLOUD_GPU (default: an RTX 4090 in Secure cloud), LUMEN_LIVE_CLOUD_EPOCHS (10).
void TestLive::cloudLora()
{
    if (qEnvironmentVariableIntValue("LUMEN_LIVE_RUNPOD") != 1)
        QSKIP("LUMEN_LIVE_RUNPOD=1 is needed (rents a GPU)");
    const QString characterId = qEnvironmentVariable("LUMEN_LIVE_CLOUD_CHARACTER");
    const QString checkpoint = QDir::cleanPath(qEnvironmentVariable("LUMEN_LIVE_CLOUD_CHECKPOINT"));
    if (characterId.isEmpty() || qEnvironmentVariable("LUMEN_LIVE_CLOUD_CHECKPOINT").isEmpty())
        QSKIP("LUMEN_LIVE_CLOUD_CHARACTER and LUMEN_LIVE_CLOUD_CHECKPOINT are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    ui::Studio studio;
    QVERIFY2(studio.runPod()->hasKey(), "no RunPod API key in the credential store");
    QVERIFY(studio.characters()->entry(characterId));

    // The GPU: the one asked for, else an RTX 4090 in Secure cloud, else the cheapest with 24 GB.
    auto gpus = studio.runPod()->gpus();
    QTRY_VERIFY_WITH_TIMEOUT(gpus.isFinished(), 60000);
    const QString wanted = qEnvironmentVariable("LUMEN_LIVE_CLOUD_GPU");
    std::optional<runtime::RunPodGpu> gpu;
    for (const runtime::RunPodGpu &g : gpus.result()) {
        if (g.memoryGb < runtime::kCloudTrainingMinGpuGb || !g.offered(false))
            continue;
        const bool match = wanted.isEmpty() ? g.name.contains(QLatin1String("4090")) : (g.id == wanted || g.name == wanted);
        if (match || (!gpu && wanted.isEmpty()) || (gpu && wanted.isEmpty() && !gpu->name.contains(QLatin1String("4090")) && g.price(false) < gpu->price(false)))
            gpu = g;
        if (match)
            break;
    }
    QVERIFY2(gpu, "no fitting GPU offered");
    qInfo().noquote() << "GPU:" << gpu->name << gpu->memoryGb << "GB, $" << gpu->price(false) << "/h (Secure)";

    ui::CharacterTraining::Settings s;
    s.characterId = characterId;
    s.checkpoint = checkpoint;
    s.epochs = qEnvironmentVariableIntValue("LUMEN_LIVE_CLOUD_EPOCHS") > 0 ? qEnvironmentVariableIntValue("LUMEN_LIVE_CLOUD_EPOCHS") : 10;
    s.cloud = true;
    s.gpuId = gpu->id;
    s.checkpointUrl = qEnvironmentVariable("LUMEN_LIVE_CLOUD_CHECKPOINT_URL");
    s.checkpointSha256 = qEnvironmentVariable("LUMEN_LIVE_CLOUD_CHECKPOINT_SHA256");
    QString error;
    ui::CharacterTraining *t = ui::CharacterTraining::start(&studio, s, &error);
    QVERIFY2(t, qPrintable(error));

    // Every change of stage with its time, for the plan's numbers.
    QElapsedTimer clock;
    clock.start();
    QPointer<Job> job;
    QString lastStage;
    QVariantMap result;
    connect(t, &ui::CharacterTraining::started, this, [&](Job *j) {
        job = j;
        connect(j, &Job::progressChanged, this, [&, j] {
            const QString stage = j->statusText().section(QLatin1Char(':'), 0, 0).section(QStringLiteral(" · "), 0, 0).left(60);
            if (stage != lastStage) {
                lastStage = stage;
                qInfo().noquote() << QStringLiteral("%1 s").arg(clock.elapsed() / 1000, 5) << j->statusText().left(140);
            }
        });
        connect(j, &Job::stateChanged, this, [&, j] {
            if (j->isFinished())
                result = j->result().toMap();
        });
    });
    bool ok = false;
    QString message;
    QString lora;
    bool done = false;
    connect(t, &ui::CharacterTraining::finished, this, [&](bool success, const QString &m, const QString &file) {
        ok = success;
        message = m;
        lora = file;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3 * 3600 * 1000);
    const double hours = clock.elapsed() / 3600000.0;
    const double perHour = result.value(QStringLiteral("costPerHour")).toDouble();
    qInfo().noquote() << QStringLiteral("finished after %1 min: %2").arg(clock.elapsed() / 60000.0, 0, 'f', 1).arg(message);
    qInfo().noquote() << QStringLiteral("pod %1, $%2/h, about $%3 in all").arg(result.value(QStringLiteral("pod")).toString()).arg(perHour, 0, 'f', 2).arg(hours * perHour, 0, 'f', 2);
    if (job && !ok)
        qInfo().noquote() << "job error:" << job->errorString();
    QVERIFY2(ok, qPrintable(message));
    QVERIFY(QFileInfo::exists(lora));
    QCOMPARE(QDir::cleanPath(ai::CharacterLora::fromJson(studio.characters()->entry(characterId)->lora).file), QDir::cleanPath(lora));
    QCOMPARE(studio.models()->entry(lora)->source.value(QStringLiteral("where")).toString(), QStringLiteral("cloud"));
    const QString run = ui::CharacterTraining::lastRun(studio.characters()->datasetDirectory(characterId));
    qInfo().noquote() << "epochs:" << QDir(run + QStringLiteral("/output")).entryList({QStringLiteral("*.safetensors")}).join(QStringLiteral(", "));
    // No pod is left behind.
    auto leftovers = runtime::leftoverCloudPods(studio.runPod());
    QTRY_VERIFY_WITH_TIMEOUT(leftovers.isFinished(), 60000);
    QVERIFY2(leftovers.result().isEmpty(), "a pod is still rented");
}

void TestLive::characterLoraEndToEnd()
{
    const int shotCount = qEnvironmentVariableIntValue("LUMEN_LIVE_FULL_LORA");
    const int epochs = qEnvironmentVariableIntValue("LUMEN_LIVE_FULL_EPOCHS") > 0 ? qEnvironmentVariableIntValue("LUMEN_LIVE_FULL_EPOCHS") : 10;
    const QString llamaExe = qEnvironmentVariable("LUMEN_LIVE_LLAMA_SERVER");
    const QString llamaModel = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MODEL");
    const QString sdRoot = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString sdModel = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    if (shotCount <= 0 || llamaExe.isEmpty() || llamaModel.isEmpty() || sdRoot.isEmpty() || sdModel.isEmpty() || referencePath().isEmpty())
        QSKIP("LUMEN_LIVE_FULL_LORA, LUMEN_LIVE_LLAMA_*, LUMEN_LIVE_SD_* and LUMEN_LIVE_REFERENCE are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    QTemporaryDir dir;
    ui::Studio studio;
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    for (const runtime::ServerProfile &p : studio.servers()->profiles())
        studio.servers()->removeProfile(p.id);
    studio.runtimes()->setRoot(dir.filePath(QStringLiteral("runtimes")));
    studio.runtimes()->load();
    {
        JobQueue jobs;
        runtime::RuntimeStore real(&m_http, &jobs);
        real.setRoot(sdRoot);
        real.load();
        const auto sd = real.latestManaged(QStringLiteral("sd.cpp"));
        QVERIFY(sd);
        studio.runtimes()->addInstalled(*sd);
        const QList<runtime::InstalledRuntime> trainers = real.runtimes(QStringLiteral("kohya"));
        QVERIFY2(!trainers.isEmpty(), "no trainer installed");
        studio.runtimes()->addInstalled(trainers.last());
    }
    QFuture<runtime::InstalledRuntime> llamaRt = studio.runtimes()->adopt(llamaExe);
    QTRY_VERIFY_WITH_TIMEOUT(llamaRt.isFinished(), 30000);
    runtime::ServerSupervisor *servers = studio.servers();
    runtime::ServerProfile llm = servers->createProfile(llamaRt.result().id, QStringLiteral("Qwen3-VL"));
    llm.options.insert(QStringLiteral("model"), llamaModel);
    if (!qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ").isEmpty())
        llm.options.insert(QStringLiteral("mmproj"), qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ"));
    llm.startupTimeoutSec = 300;
    servers->addProfile(llm);
    runtime::ServerProfile image = servers->createProfile(studio.runtimes()->latestManaged(QStringLiteral("sd.cpp"))->id, QStringLiteral("SDXL"));
    image.options.insert(QStringLiteral("model"), sdModel);
    for (const auto &[key, env] : {std::pair{QStringLiteral("control_net"), "LUMEN_LIVE_SD_CONTROLNET"},
                                   std::pair{QStringLiteral("ip_adapter"), "LUMEN_LIVE_SD_IPADAPTER"},
                                   std::pair{QStringLiteral("clip_vision"), "LUMEN_LIVE_SD_CLIPVISION"}}) {
        if (!qEnvironmentVariable(env).isEmpty())
            image.options.insert(key, qEnvironmentVariable(env));
    }
    image.startupTimeoutSec = 300;
    servers->addProfile(image);
    Provider *sdProvider = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SDXL"), {{QStringLiteral("managed_server"), image.id}});
    Provider *llmProvider = studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Qwen3-VL"),
                                                            {{QStringLiteral("managed_server"), llm.id}, {QStringLiteral("timeout_s"), 900}});
    QVERIFY(sdProvider && llmProvider);
    studio.providers()->setDefaultImageProvider(sdProvider->instanceId());
    QString visionName;
    for (const runtime::LlamaModel &m : runtime::llamaModels(*servers->profile(llm.id))) {
        if (m.vision() && visionName.isEmpty())
            visionName = m.name;
    }
    for (LlmRole role : allLlmRoles())
        studio.providers()->setRole(role, {llmProvider->instanceId(), visionName});

    // The character: one reference image and its sheet, saved to the character library.
    studio.openProject(dir.filePath(QStringLiteral("lora.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 10000);
    auto imported = studio.project()->importFile(referencePath(), doc::AssetKind::Reference);
    QTRY_VERIFY_WITH_TIMEOUT(imported.isFinished(), 10000);
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Akane")},
                            {QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), qEnvironmentVariable("LUMEN_LIVE_DATASET_TAGS",
                                                                          QStringLiteral("long red hair, yellow eyes, black hooded cloak, black choker"))}};
    const QString reuse = qEnvironmentVariable("LUMEN_LIVE_FULL_CHARACTER");
    QString characterId = reuse;
    if (reuse.isEmpty()) {
        auto saved = studio.characters()->save(QStringLiteral("Akane (M13 acceptance)"), sheet, {QImage(referencePath())});
        QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 10000);
        characterId = saved.result().id;
    }
    QVERIFY2(studio.characters()->entry(characterId), "no such library character");
    qInfo().noquote() << "character" << characterId;
    auto cleanup = qScopeGuard([&] {
        Settings::instance().setValue(QStringLiteral("director/critic"), false);
        for (Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        servers->stopAll();
    });
    ui::CreatePage create(&studio);
    QTRY_VERIFY(create.findChild<ui::DirectorSession *>());
    ui::DirectorSession *session = create.findChild<ui::DirectorSession *>();
    session->addReferences({imported.result().id});
    session->setCharacter(sheet);
    session->setLibraryCharacter(characterId);
    for (QSpinBox *spin : create.findChildren<QSpinBox *>()) {
        if (spin->suffix().contains(QLatin1String("per shot")))
            spin->setValue(1);
        if (spin->prefix().startsWith(QLatin1String("Shots")))
            spin->setValue(shotCount);
    }
    QAction *trainingSet = nullptr;
    for (QAction *a : create.findChildren<QAction *>()) {
        if (a->text() == QStringLiteral("Training set…"))
            trainingSet = a;
    }
    QVERIFY(trainingSet);
    trainingSet->trigger();
    QTRY_VERIFY(create.findChild<ui::LoraDatasetDialog *>());
    ui::LoraDatasetDialog *dlg = create.findChild<ui::LoraDatasetDialog *>();
    auto button = [dlg](const QString &text) -> QPushButton * {
        for (QPushButton *b : dlg->findChildren<QPushButton *>()) {
            if (b->text() == text)
                return b;
        }
        return nullptr;
    };
    QElapsedTimer total;
    total.start();
    auto stamp = [&total](const QString &what) { qInfo().noquote() << QStringLiteral("[%1 min]").arg(total.elapsed() / 60000.0, 0, 'f', 1) << what; };

    doc::LoraDataset *ds = dlg->dataset();
    if (!reuse.isEmpty()) {
        stamp(QStringLiteral("training set of an earlier run: %1 images, %2 kept, trigger %3")
                  .arg(ds->items().size())
                  .arg(ds->keptCount())
                  .arg(ds->settings().trigger));
    } else {
        // 1. Plan and generate the training shots.
        QVERIFY(button(QStringLiteral("Plan training shots")));
        button(QStringLiteral("Plan training shots"))->click();
        QTRY_COMPARE_WITH_TIMEOUT(int(session->shots().size()), shotCount, 10000);
        // As "Generate all" does, through the director actions (they report when every shot settled).
        QStringList ids;
        for (const ui::ShotState &s : session->shots())
            ids << s.id();
        bool generated = false;
        QString generationErrors;
        create.generateShots(ids, [&](const QStringList &, const QString &error) {
            generationErrors = error;
            generated = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(generated, 4 * 3600 * 1000);
        int failed = 0;
        for (const ui::ShotState &s : session->shots())
            failed += s.results.isEmpty() ? 1 : 0;
        if (!generationErrors.isEmpty())
            qInfo().noquote() << "   errors:" << generationErrors.left(1000);
        stamp(QStringLiteral("%1 shots generated, %2 failed").arg(shotCount).arg(failed));

        // 2. Into the training set; the Critic scores; near-duplicates out; captions.
        int added = -1;
        dlg->addSessionResults([&](int n) { added = n; });
        QTRY_VERIFY_WITH_TIMEOUT(added >= 0, 120000);
        dlg->scoreAll();
        QTRY_VERIFY_WITH_TIMEOUT(!dlg->isScoring(), 3600 * 1000);
        const int duplicates = ds->markDuplicates();
        QList<QImage> kept;
        QStringList scores;
        for (const doc::DatasetItem &i : ds->items()) {
            scores << QStringLiteral("%1%2").arg(i.score).arg(i.kept ? QString() : QStringLiteral("✗"));
            if (i.kept)
                kept << QImage(ds->imagePath(i.id));
        }
        stamp(QStringLiteral("%1 added, %2 kept, %3 near-duplicates; scores %4").arg(added).arg(ds->keptCount()).arg(duplicates).arg(scores.join(QLatin1Char(' '))));
        dlg->captionAll();
        QTRY_VERIFY_WITH_TIMEOUT(!dlg->isCaptioning(), 3600 * 1000);
        for (const doc::DatasetItem &i : ds->items()) {
            if (i.kept)
                qInfo().noquote() << "   " << i.caption;
        }
        stamp(QStringLiteral("captions done; trigger %1").arg(ds->settings().trigger));
        {
            const int columns = 6;
            QImage contact(columns * 200, ((int(kept.size()) + columns - 1) / columns) * 200, QImage::Format_RGB32);
            contact.fill(Qt::white);
            QPainter p(&contact);
            for (int i = 0; i < kept.size(); ++i) {
                const QImage tile = kept[i].scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                p.drawImage((i % columns) * 200 + (200 - tile.width()) / 2, (i / columns) * 200 + (200 - tile.height()) / 2, tile);
            }
            p.end();
            save(contact, QStringLiteral("full_dataset.png"));
        }
    }

    // 3. Train tab: the generator's model and the GPU's preset are preselected.
    QSpinBox *epochSpin = nullptr;
    for (QSpinBox *s : dlg->findChildren<QSpinBox *>()) {
        if (s->toolTip().startsWith(QLatin1String("Each epoch")))
            epochSpin = s;
    }
    QVERIFY(epochSpin);
    epochSpin->setValue(epochs);
    QPushButton *start = button(QStringLiteral("Start training"));
    QVERIFY(start);
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 30000);
    const QStringList running = [&] {
        QStringList ids;
        for (const QString &id : {llm.id, image.id}) {
            if (servers->state(id) != runtime::ServerState::Stopped)
                ids << id;
        }
        return ids;
    }();
    stamp(QStringLiteral("training starts; servers running before: %1").arg(running.size()));
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!servers->gpuReservation().isEmpty(), 60000);
    QTRY_VERIFY_WITH_TIMEOUT(servers->state(llm.id) == runtime::ServerState::Stopped && servers->state(image.id) == runtime::ServerState::Stopped, 120000);
    // A request while training gets the reason instead of starting a server.
    QFuture<QString> refused = servers->start(image.id);
    QTRY_VERIFY(refused.isFinished());
    QVERIFY(!refused.result().isEmpty());
    qInfo().noquote() << "   while training:" << refused.result();
    QTRY_VERIFY_WITH_TIMEOUT(!ui::CharacterTraining::running(characterId), 8 * 3600 * 1000);
    const ai::CharacterLora trained = ai::CharacterLora::fromJson(studio.characters()->entry(characterId)->lora);
    QVERIFY2(trained.isValid(), "training failed");
    QVERIFY(servers->gpuReservation().isEmpty());
    stamp(QStringLiteral("trained: %1 (%2 MB), trigger %3; GPU released")
              .arg(QDir::toNativeSeparators(trained.file))
              .arg(QFileInfo(trained.file).size() / 1e6, 0, 'f', 1)
              .arg(trained.trigger));
    QFile log(QDir(ui::CharacterTraining::lastRun(ds->directory())).filePath(QStringLiteral("train.log")));
    if (log.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(log.readAll()).split(QLatin1Char('\n'));
        for (const QString &l : lines) {
            if (l.contains(QLatin1String("avr_loss")) && l.contains(QLatin1String("100%")))
                qInfo().noquote() << "   " << l.trimmed().right(120);
        }
    }

    // 4. Test the LoRA: the servers start again on demand.
    QPushButton *test = button(QStringLiteral("Test the LoRA"));
    QVERIFY(test);
    QTRY_VERIFY(test->isEnabled());
    test->click();
    QVERIFY(ui::LoraEvaluation::running(characterId));
    QTRY_VERIFY_WITH_TIMEOUT(!ui::LoraEvaluation::running(characterId), 4 * 3600 * 1000);
    const QString runDir = ui::CharacterTraining::lastRun(ds->directory());
    const QJsonObject result = ui::LoraEvaluation::saved(runDir);
    stamp(result.value(QStringLiteral("status")).toString());
    QVERIFY(result.value(QStringLiteral("finished")).toBool());
    QMap<QPair<int, double>, QList<QJsonObject>> cells;
    QList<QPair<int, double>> order;
    for (const QJsonValue &v : result.value(QStringLiteral("cells")).toArray()) {
        const QPair<int, double> key{v[QStringLiteral("epoch")].toInt(), v[QStringLiteral("weight")].toDouble()};
        if (!order.contains(key))
            order << key;
        cells[key] << v.toObject();
    }
    QImage grid(4 * 256, int(order.size()) * 256, QImage::Format_RGB32);
    grid.fill(Qt::white);
    QPainter p(&grid);
    for (int row = 0; row < order.size(); ++row) {
        double score = 0;
        double identity = 0;
        for (const QJsonObject &c : cells[order[row]]) {
            score += c.value(QStringLiteral("score")).toDouble();
            identity += c.value(QStringLiteral("critique")).toObject().value(QStringLiteral("identity")).toDouble();
            const int column = c.value(QStringLiteral("shot")).toString().mid(1).toInt() - 1;
            const QImage tile = QImage(c.value(QStringLiteral("image")).toString()).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p.drawImage(column * 256 + (256 - tile.width()) / 2, row * 256 + (256 - tile.height()) / 2, tile);
        }
        const int n = int(cells[order[row]].size());
        qInfo().noquote() << "   " << (order[row].first == 0 ? QStringLiteral("without LoRA") : QStringLiteral("epoch %1 at %2").arg(order[row].first).arg(order[row].second))
                          << "score" << QString::number(score / n, 'f', 2) << "identity" << QString::number(identity / n, 'f', 2);
    }
    p.end();
    save(grid, QStringLiteral("full_evaluation.png"));

    // 5. Attach the recommended epoch.
    QPushButton *apply = nullptr;
    for (QPushButton *b : dlg->findChildren<QPushButton *>()) {
        if (b->text().endsWith(QLatin1String("(recommended)")))
            apply = b;
    }
    QVERIFY(apply);
    apply->click();
    const ai::CharacterLora chosen = ai::CharacterLora::fromJson(studio.characters()->entry(characterId)->lora);
    QTRY_COMPARE(ai::CharacterLora::fromJson(session->characterLora()).file, chosen.file);
    stamp(QStringLiteral("attached %1 at %2").arg(QFileInfo(chosen.file).fileName()).arg(chosen.weight));
}

// LUMEN_LIVE_EVERYWHERE=<library character with a trained LoRA and a LoRA test run> with
// LUMEN_LIVE_LLAMA_*, LUMEN_LIVE_SD_* (M14 step 7): the character on the Generate page (8 prompts that
// never name it, with and without the character, same seeds), 4 fixes with Fill selection in the
// editor and the 5 expression variants, each scored by the identity checklist.
void TestLive::characterEverywhere()
{
    const QString characterId = qEnvironmentVariable("LUMEN_LIVE_EVERYWHERE");
    const QString llamaExe = qEnvironmentVariable("LUMEN_LIVE_LLAMA_SERVER");
    const QString llamaModel = qEnvironmentVariable("LUMEN_LIVE_LLAMA_MODEL");
    const QString sdRoot = qEnvironmentVariable("LUMEN_LIVE_SD_RUNTIMES");
    const QString sdModel = qEnvironmentVariable("LUMEN_LIVE_SD_MODEL");
    if (characterId.isEmpty() || llamaExe.isEmpty() || llamaModel.isEmpty() || sdRoot.isEmpty() || sdModel.isEmpty())
        QSKIP("LUMEN_LIVE_EVERYWHERE, LUMEN_LIVE_LLAMA_* and LUMEN_LIVE_SD_* are needed");
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    ui::Studio studio;
    const auto entry = studio.characters()->entry(characterId);
    QVERIFY2(entry, "no such library character");
    const ai::CharacterLora lora = ai::CharacterLora::fromJson(entry->lora);
    QVERIFY2(lora.isValid(), "the character has no LoRA");
    qInfo().noquote() << "character" << entry->name << "LoRA" << QFileInfo(lora.file).fileName() << "weight" << lora.weight;
    for (Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    for (const runtime::ServerProfile &p : studio.servers()->profiles())
        studio.servers()->removeProfile(p.id);
    studio.runtimes()->setRoot(dir.filePath(QStringLiteral("runtimes")));
    studio.runtimes()->load();
    {
        JobQueue jobs;
        runtime::RuntimeStore real(&m_http, &jobs);
        real.setRoot(sdRoot);
        real.load();
        const auto sd = real.latestManaged(QStringLiteral("sd.cpp"));
        QVERIFY(sd);
        studio.runtimes()->addInstalled(*sd);
    }
    QFuture<runtime::InstalledRuntime> llamaRt = studio.runtimes()->adopt(llamaExe);
    QTRY_VERIFY_WITH_TIMEOUT(llamaRt.isFinished(), 30000);
    runtime::ServerSupervisor *servers = studio.servers();
    runtime::ServerProfile llm = servers->createProfile(llamaRt.result().id, QStringLiteral("Qwen3-VL"));
    llm.options.insert(QStringLiteral("model"), llamaModel);
    if (!qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ").isEmpty())
        llm.options.insert(QStringLiteral("mmproj"), qEnvironmentVariable("LUMEN_LIVE_LLAMA_MMPROJ"));
    llm.startupTimeoutSec = 300;
    servers->addProfile(llm);
    runtime::ServerProfile image = servers->createProfile(studio.runtimes()->latestManaged(QStringLiteral("sd.cpp"))->id, QStringLiteral("SDXL"));
    image.options.insert(QStringLiteral("model"), sdModel);
    image.options.insert(QStringLiteral("lora_dir"), QDir::toNativeSeparators(QFileInfo(lora.file).absolutePath()));
    image.startupTimeoutSec = 300;
    servers->addProfile(image);
    Provider *sdProvider = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SDXL"), {{QStringLiteral("managed_server"), image.id}});
    Provider *llmProvider = studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Qwen3-VL"),
                                                            {{QStringLiteral("managed_server"), llm.id}, {QStringLiteral("timeout_s"), 900}});
    QVERIFY(sdProvider && llmProvider);
    studio.providers()->setDefaultImageProvider(sdProvider->instanceId());
    QString visionName;
    for (const runtime::LlamaModel &m : runtime::llamaModels(*servers->profile(llm.id))) {
        if (m.vision() && visionName.isEmpty())
            visionName = m.name;
    }
    for (LlmRole role : allLlmRoles())
        studio.providers()->setRole(role, {llmProvider->instanceId(), visionName});
    studio.openProject(dir.filePath(QStringLiteral("everywhere.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 10000);
    auto cleanup = qScopeGuard([&] {
        for (Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        servers->stopAll();
    });
    studio.setActiveCharacter(characterId);
    auto contextFuture = studio.characterContext(characterId);
    QTRY_VERIFY(contextFuture.isFinished());
    const ai::CharacterContext character = contextFuture.result();
    const QStringList checklist = Director::identityChecklist(character.sheet);
    qInfo().noquote() << "checklist:" << checklist.join(QStringLiteral(" | "));

    // The identity checklist of an image against the character's references.
    struct Score
    {
        double identity = -1;
        int pose = 0;
        QStringList different;
    };
    auto score = [&](const QImage &img, const QString &description, const QList<QImage> &refs) {
        const QJsonObject shot{{QStringLiteral("title"), QStringLiteral("check")}, {QStringLiteral("description"), description}};
        StructuredChatOperation *op = studio.director()->checkIdentity(img, shot, character.sheet, refs);
        op->setAutoDelete(false);
        Score s;
        if (!QTest::qWaitFor([op] { return op->isFinished(); }, 900000) || op->state() != Operation::State::Succeeded) {
            qInfo().noquote() << "   check failed:" << op->errorString();
            delete op;
            return s;
        }
        s.identity = Director::checklistIdentity(op->result());
        s.pose = op->result().value(QStringLiteral("pose")).toInt();
        const QJsonObject f = op->result().value(QStringLiteral("features")).toObject();
        for (int i = 0; i < checklist.size(); ++i) {
            if (f.value(QStringLiteral("f%1").arg(i + 1)).toString() == QLatin1String("different"))
                s.different << checklist[i];
        }
        delete op;
        return s;
    };
    QElapsedTimer total;
    total.start();

    // 1. Generate page: 8 prompts that never name the character, with and without it, same seeds.
    const QStringList prompts{QStringLiteral("reading in a library, evening light"), QStringLiteral("walking in the rain with an umbrella"),
                              QStringLiteral("sitting at a cafe table, smiling"), QStringLiteral("on a rooftop at night, city lights"),
                              QStringLiteral("standing in a snowy forest"), QStringLiteral("at the beach, sunset"),
                              QStringLiteral("playing the violin on a stage"), QStringLiteral("in a classroom, looking out of the window")};
    {
        ui::GeneratePage page(&studio);
        auto *chip = page.findChild<ui::CharacterChip *>();
        ui::AutoTextEdit *promptEdit = nullptr;
        for (ui::AutoTextEdit *e : page.findChildren<ui::AutoTextEdit *>()) {
            if (e->placeholderText().startsWith(QLatin1String("A portrait")))
                promptEdit = e;
        }
        QPushButton *go = nullptr;
        for (QPushButton *b : page.findChildren<QPushButton *>()) {
            if (b->text() == QLatin1String("Generate"))
                go = b;
        }
        QCheckBox *random = nullptr;
        for (QCheckBox *c : page.findChildren<QCheckBox *>()) {
            if (c->text() == QLatin1String("Random"))
                random = c;
        }
        QSpinBox *seed = nullptr;
        for (QSpinBox *sp : page.findChildren<QSpinBox *>()) {
            if (sp->maximum() >= 2147483646)
                seed = sp;
        }
        QVERIFY(chip && promptEdit && go && random && seed);
        random->setChecked(false);
        double sumWith = 0, sumWithout = 0;
        for (int i = 0; i < prompts.size(); ++i) {
            QImage images[2];
            for (int with = 1; with >= 0; --with) {
                chip->setUsedHere(with == 1);
                seed->setValue(1000 + i);
                promptEdit->setPlainText(prompts[i]);
                go->click();
                // The image of this prompt, with the character or without (as the metadata says).
                auto find = [&]() -> std::optional<doc::AssetInfo> {
                    for (const doc::AssetInfo &a : studio.project()->assets()) {
                        if (a.metadata.value(QStringLiteral("user_prompt")).toString() == prompts[i]
                            && a.metadata.contains(QStringLiteral("character")) == (with == 1))
                            return a;
                    }
                    return std::nullopt;
                };
                QVERIFY(QTest::qWaitFor([&] { return find().has_value(); }, 900000));
                const doc::AssetInfo a = *find();
                if (with == 1)
                    qInfo().noquote() << "   sent:" << a.metadata.value(QStringLiteral("prompt")).toString().left(160);
                auto loaded = studio.project()->loadImage(a.id);
                QTRY_VERIFY_WITH_TIMEOUT(loaded.isFinished(), 10000);
                images[with] = loaded.result();
                save(images[with], QStringLiteral("m14_generate_%1_%2.png").arg(i).arg(with ? QStringLiteral("with") : QStringLiteral("without")));
            }
            const Score w = score(images[1], prompts[i], character.references);
            const Score wo = score(images[0], prompts[i], character.references);
            sumWith += w.identity;
            sumWithout += wo.identity;
            qInfo().noquote() << QStringLiteral("  [%1 min] generate \"%2\": with %3 (pose %4) · without %5; different with: %6")
                                     .arg(total.elapsed() / 60000.0, 0, 'f', 1)
                                     .arg(prompts[i])
                                     .arg(w.identity, 0, 'f', 1)
                                     .arg(w.pose)
                                     .arg(wo.identity, 0, 'f', 1)
                                     .arg(w.different.join(QStringLiteral(", ")));
        }
        qInfo().noquote() << QStringLiteral("GENERATE identity mean: with %1, without %2")
                                 .arg(sumWith / prompts.size(), 0, 'f', 2)
                                 .arg(sumWithout / prompts.size(), 0, 'f', 2);
        chip->setUsedHere(true);
    }

    // The character's LoRA test images have known poses: their keypoints place the fixes.
    const QString runDir = ui::CharacterTraining::lastRun(studio.characters()->datasetDirectory(characterId));
    const QJsonObject eval = ui::LoraEvaluation::saved(runDir);
    const int bestEpoch = eval.value(QStringLiteral("bestEpoch")).toInt();
    QString fullBody, portrait;
    for (const QJsonValue &v : eval.value(QStringLiteral("cells")).toArray()) {
        if (v[QStringLiteral("epoch")].toInt() == bestEpoch && qFuzzyCompare(v[QStringLiteral("weight")].toDouble(), 0.8)) {
            if (v[QStringLiteral("shot")].toString() == QLatin1String("t2"))
                fullBody = v[QStringLiteral("image")].toString();
            if (v[QStringLiteral("shot")].toString() == QLatin1String("t4"))
                portrait = v[QStringLiteral("image")].toString();
        }
    }
    QVERIFY2(!fullBody.isEmpty() && !portrait.isEmpty(), "no LoRA test images of the best epoch");
    const QList<QJsonObject> shots = Director::loraTestShots(character.sheet, lora.trigger);
    auto keypoints = [&shots](const QString &id, const QSize &size) {
        for (const QJsonObject &s : shots) {
            if (s.value(QStringLiteral("id")).toString() == id)
                return Director::poseForShot(s, size).people.value(0);
        }
        return pose::Person{};
    };
    auto around = [](const QPointF &c, double w, double h, const QSize &bounds) {
        return QRectF(c.x() - w / 2, c.y() - h / 2, w, h).toAlignedRect() & QRect(QPoint(0, 0), bounds);
    };

    // 2. Editor: Fill selection fixes on the three-quarter image, with and without the character.
    {
        const QImage base(fullBody);
        QVERIFY(!base.isNull());
        const pose::Person p = keypoints(QStringLiteral("t2"), base.size());
        const double unit = QLineF(p.body[1].pos, p.body[0].pos).length(); // neck to nose
        const QList<std::tuple<QString, QRect, QString>> fixes{
            {QStringLiteral("hand"), around(p.body[4].pos, unit * 1.6, unit * 1.6, base.size()), QStringLiteral("a hand resting on the chest, five fingers")},
            {QStringLiteral("face"), around(p.body[0].pos, unit * 2.0, unit * 2.2, base.size()), QStringLiteral("a calm face looking at the viewer")},
            {QStringLiteral("sleeve"), around((p.body[6].pos + p.body[7].pos) / 2, unit * 1.4, unit * 2.2, base.size()), QStringLiteral("a sleeve")},
            {QStringLiteral("hair"), around(p.body[0].pos - QPointF(0, unit * 1.2), unit * 2.4, unit * 1.4, base.size()), QStringLiteral("hair")}};
        const Score original = score(base, QStringLiteral("three-quarter view on a city street"), character.references);
        qInfo().noquote() << QStringLiteral("  editor base: identity %1, different: %2")
                                 .arg(original.identity, 0, 'f', 1)
                                 .arg(original.different.join(QStringLiteral(", ")));
        ui::AiPanel panel(&studio);
        auto *chip = panel.findChild<ui::CharacterChip *>();
        QVERIFY(chip);
        for (const auto &[name, rect, prompt] : fixes) {
            for (int with = 1; with >= 0; --with) {
                chip->setUsedHere(with == 1);
                doc::Document *doc = studio.openImageAsDocument(base, QStringLiteral("fix %1").arg(name));
                panel.setDocument(doc);
                QImage mask = img::createMask(base.size(), 0);
                {
                    QPainter painter(&mask);
                    painter.fillRect(rect, Qt::white);
                }
                doc->setSelectionRaw(mask);
                const int layers = doc->layerCount();
                panel.runFromCanvas(ui::AiPanel::Mode::Inpaint, prompt);
                QVERIFY(QTest::qWaitFor([&] { return doc->layerCount() > layers; }, 900000));
                auto flat = async::run([snap = doc->snapshot(), size = doc->size()] { return doc::flatten(snap, size); });
                QTRY_VERIFY_WITH_TIMEOUT(flat.isFinished(), 10000);
                const QImage result = img::flatten(flat.result(), Qt::white);
                save(result, QStringLiteral("m14_fix_%1_%2.png").arg(name, with ? QStringLiteral("with") : QStringLiteral("without")));
                const Score s = score(result, QStringLiteral("three-quarter view on a city street"), character.references);
                QStringList added;
                for (const QString &d : s.different) {
                    if (!original.different.contains(d))
                        added << d;
                }
                qInfo().noquote() << QStringLiteral("  [%1 min] fix %2 %3: identity %4, new differences: %5")
                                         .arg(total.elapsed() / 60000.0, 0, 'f', 1)
                                         .arg(name, with ? QStringLiteral("with") : QStringLiteral("without"))
                                         .arg(s.identity, 0, 'f', 1)
                                         .arg(added.isEmpty() ? QStringLiteral("none") : added.join(QStringLiteral(", ")));
                studio.closeDocument(doc);
            }
        }
    }

    // 3. Expression variants on the portrait, with and without the character.
    {
        const QImage base = img::normalized(QImage(portrait));
        QVERIFY(!base.isNull());
        const pose::Person p = keypoints(QStringLiteral("t4"), base.size());
        const double eyes = QLineF(p.body[14].pos, p.body[15].pos).length();
        for (int with = 1; with >= 0; --with) {
            doc::PartSet set;
            set.bind(studio.project());
            set.setSource(base, QStringLiteral("portrait"), img::createMask(base.size(), 255));
            QImage eyeMask = img::createMask(base.size(), 0);
            QImage mouthMask = img::createMask(base.size(), 0);
            {
                QPainter e(&eyeMask);
                e.fillRect(around(p.body[14].pos, eyes * 0.75, eyes * 0.5, base.size()), Qt::white);
                e.fillRect(around(p.body[15].pos, eyes * 0.75, eyes * 0.5, base.size()), Qt::white);
                QPainter m(&mouthMask);
                m.fillRect(around(p.body[0].pos + QPointF(0, eyes * 0.55), eyes * 0.7, eyes * 0.35, base.size()), Qt::white);
            }
            QVERIFY(set.addPart(QStringLiteral("irides"), {}, {}, eyeMask));
            QVERIFY(set.addPart(QStringLiteral("mouth"), {}, {}, mouthMask));
            for (const ai::parts::ExpressionKind &kind : ai::parts::expressionKinds()) {
                ExpressionOptions options;
                options.kind = kind.id;
                options.providerId = sdProvider->instanceId();
                options.characterSummary = Director::appearanceTags(character.sheet);
                if (with)
                    options.character = character;
                Job *job = studio.parts()->generateExpression(&set, {}, options);
                QVERIFY(QTest::qWaitFor([job] { return job->isFinished(); }, 900000));
                QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
                const doc::Part *variant = nullptr;
                for (const doc::Part &part : set.parts()) {
                    if (part.tag == kind.tag)
                        variant = &part;
                }
                QVERIFY(variant && variant->hasResult());
                QTRY_VERIFY_WITH_TIMEOUT(!set.resultImage(variant->label).isNull(), 10000);
                const auto [before, after] = ui::expressionCrops(base, set.resultImage(variant->label), variant->chosenRect());
                save(after, QStringLiteral("m14_expr_%1_%2.png").arg(kind.id, with ? QStringLiteral("with") : QStringLiteral("without")));
                QList<QImage> refs{before};
                refs << character.references;
                const Score s = score(after, QStringLiteral("Close-up of the character's face. Planned expression: ") + kind.prompt, refs);
                qInfo().noquote() << QStringLiteral("  [%1 min] expression %2 %3: identity %4, expression %5, different: %6")
                                         .arg(total.elapsed() / 60000.0, 0, 'f', 1)
                                         .arg(kind.id, with ? QStringLiteral("with") : QStringLiteral("without"))
                                         .arg(s.identity, 0, 'f', 1)
                                         .arg(s.pose)
                                         .arg(s.different.isEmpty() ? QStringLiteral("none") : s.different.join(QStringLiteral(", ")));
            }
        }
    }
    qInfo() << "total" << total.elapsed() / 60000.0 << "min";
}

QTEST_MAIN(TestLive)
#include "tst_live.moc"
