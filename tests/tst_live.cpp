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
#include "ai/PromptStyle.h"
#include "ai/ProviderCatalog.h"
#include "ai/ProviderRegistry.h"
#include "ai/providers/ComfyUiProvider.h"
#include "ai/providers/OpenAiChatProvider.h"
#include "ai/providers/SdCppProvider.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Settings.h"
#include "doc/Project.h"
#include "net/HttpClient.h"
#include "pose/Rig.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/RuntimeStore.h"
#include "runtime/Supervisor.h"
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
    void managedLlamaServer();
    void installRuntime();
    void managedSdServer();
    void managedStudioScenario();
    void managedSdUpscale();
    void managedComfyUi();
    void hubDownload();
    void managedSdLora();

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

QTEST_MAIN(TestLive)
#include "tst_live.moc"
