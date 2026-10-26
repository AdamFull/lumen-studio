// Setting up AI (plan M17 §17.5): the setup wizard's three ways and Settings → AI. Everything runs
// against mock servers (Hugging Face, sd-server through the fake server, OpenAI, ComfyUI) and tiny
// model files with real headers; nothing touches the GPU or the network (GitHub's release lookups
// go to a closed port).

#include "MockHttpServer.h"

#include "ai/ModelDefaults.h"
#include "ai/ProviderCatalog.h"
#include "core/ImageOps.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/SecretStore.h"
#include "core/Settings.h"
#include "net/HttpClient.h"
#include "plugins/BuiltInPlugins.h"
#include "runtime/AiSetup.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ModelPresets.h"
#include "runtime/Supervisor.h"
#include "ui/MainWindow.h"
#include "ui/Studio.h"
#include "ui/dialogs/AiSettingsPage.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/dialogs/SetupWizard.h"
#include "ui/ext/Extension.h"
#include "ui/pages/AiPanel.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

using namespace lumen;
using namespace lumen::ui;
using runtime::GpuInfo;
using runtime::HardwareInfo;
using runtime::LocalRecommendation;

namespace {

constexpr quint64 kMiB = 1024ull * 1024;
const QByteArray kPad(2 * 1024 * 1024, '\0'); // the library skips files under 1 MB

HardwareInfo hardware(QList<GpuInfo> gpus)
{
    HardwareInfo hw;
    hw.gpus = gpus;
    hw.systemRam = 32ull * 1024 * kMiB;
    hw.cpuThreads = 16;
    return hw;
}

GpuInfo gpu(quint32 vendor, quint64 mib, const QString &name, bool integrated = false)
{
    GpuInfo g;
    g.vendorId = vendor;
    g.dedicatedVram = mib * kMiB;
    g.name = name;
    g.integrated = integrated;
    return g;
}

// An RTX 4060 laptop GPU as DXGI reports it (8188 MiB).
HardwareInfo rtx4060()
{
    return hardware({gpu(0x8086, 128, QStringLiteral("Intel(R) UHD Graphics"), true), gpu(0x10DE, 8188, QStringLiteral("NVIDIA GeForce RTX 4060 Laptop GPU"))});
}

// A safetensors header with the given tensors (name → shape); no weights are needed to read it.
QByteArray safetensors(const QList<QPair<QString, QList<int>>> &tensors)
{
    QJsonObject header;
    for (const auto &[name, shape] : tensors) {
        QJsonArray dims;
        for (int d : shape)
            dims << d;
        header.insert(name, QJsonObject{{QStringLiteral("dtype"), QStringLiteral("F16")}, {QStringLiteral("shape"), dims}, {QStringLiteral("data_offsets"), QJsonArray{0, 0}}});
    }
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i)
        out.append(char((quint64(json.size()) >> (8 * i)) & 0xFF));
    return out + json + kPad;
}

// FLUX.2: 128 input channels; the text width (7680 = 3 × Qwen3 4B) tells klein 4B.
QByteArray flux2Klein(int textWidth = 7680)
{
    return safetensors({{QStringLiteral("img_in.weight"), {3072, 128}},
                        {QStringLiteral("txt_in.weight"), {3072, textWidth}},
                        {QStringLiteral("double_stream_modulation_img.lin.weight"), {18432, 3072}},
                        {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {9216, 3072}},
                        {QStringLiteral("single_blocks.0.linear1.weight"), {27648, 3072}}});
}

QByteArray flux2Vae()
{
    return safetensors({{QStringLiteral("encoder.down_blocks.0.resnets.0.conv1.weight"), {128, 128, 3, 3}},
                        {QStringLiteral("decoder.conv_in.weight"), {512, 32, 3, 3}},
                        {QStringLiteral("decoder.up_blocks.0.resnets.0.conv1.weight"), {512, 512, 3, 3}}});
}

QByteArray qwen3(int width)
{
    return safetensors({{QStringLiteral("model.embed_tokens.weight"), {151936, width}}, {QStringLiteral("model.layers.0.self_attn.q_norm.weight"), {128}}});
}

QString write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(bytes);
    return QDir::cleanPath(path);
}

QString pngB64(const QColor &c, const QSize &s)
{
    QImage img(s, QImage::Format_ARGB32);
    img.fill(c);
    return QString::fromLatin1(img::toBase64(img));
}

// A mock sd-server: capabilities, one generation job, its result.
void routeSdServer(MockHttpServer &server, int *generations = nullptr)
{
    auto payload = std::make_shared<QJsonObject>();
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model"), QJsonObject{{QStringLiteral("name"), QStringLiteral("flux-2-klein-4b")}}}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [payload, generations](const MockRequest &req) {
        *payload = req.json();
        if (generations)
            ++*generations;
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [payload](const MockRequest &) {
        const QSize size(payload->value(QStringLiteral("width")).toInt(512), payload->value(QStringLiteral("height")).toInt(512));
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngB64(QColor(200, 120, 40), size)}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });
}

template <typename T>
T *named(QWidget *root, const QString &name)
{
    return root->findChild<T *>(name);
}

QPushButton *buttonWithText(QWidget *root, const QString &text)
{
    for (QPushButton *b : root->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

SetupWizard *openWizard()
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *wizard = qobject_cast<SetupWizard *>(w); wizard && wizard->isVisible())
            return wizard;
    }
    return nullptr;
}

} // namespace

class TestSetup : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void recommendation();
    void pinnedDownloads();
    void planFindsFiles();
    void localReusesFiles();
    void localCorrectsServer();
    void localDownloadsMissing();
    void onlineService();
    void runningServer();
    void settingsAi();
    void entryPoints();

private:
    // Leaves the test data folder as it found it: no providers, servers, adopted programs, added
    // folders or model files of Lumen's own folder.
    void reset(Studio &studio);
    // A fake sd-server adopted as the installed stable-diffusion.cpp.
    QString adoptFakeServer(Studio &studio, const QString &folder);
    // FLUX.2 klein 4B as the bundled preset says, with its pinned files replaced by tiny ones served
    // by `hf` and its launch options pointing the fake server at `sd` (the mock sd-server).
    runtime::ModelPreset tinyKlein(MockHttpServer &hf, const MockHttpServer &sd, QHash<QString, QByteArray> *contents);
};

void TestSetup::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    plugins::registerBuiltInPlugins();
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    // Wizards opened from menus neither probe this computer nor read its folders.
    SetupWizard::testHardware = rtx4060();
    SetupWizard::testSearchFolders = QStringList();
}

void TestSetup::init()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
}

void TestSetup::reset(Studio &studio)
{
    studio.runtimes()->feed()->setApiBase(QUrl(QStringLiteral("http://127.0.0.1:9"))); // no GitHub lookups
    runtime::ServerSupervisor *servers = studio.servers();
    for (const runtime::ServerProfile &p : servers->profiles()) {
        servers->stop(p.id);
        servers->removeProfile(p.id);
    }
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    studio.providers()->setEditImageProvider(QString());
    for (const runtime::InstalledRuntime &rt : studio.runtimes()->runtimes())
        studio.runtimes()->remove(rt.id);
    for (const runtime::ModelRoot &r : studio.models()->roots()) {
        if (!r.builtin)
            studio.models()->removeRoot(r.path);
    }
    studio.models()->moveToTrash = [](const QString &path, QString *) { return QFile::remove(path); };
    const QString own = studio.models()->defaultFolder();
    for (const runtime::ModelEntry &e : studio.models()->entries()) {
        if (e.path.startsWith(own))
            studio.models()->trash(e.path);
    }
}

QString TestSetup::adoptFakeServer(Studio &studio, const QString &folder)
{
    const QString exe = QDir(folder).filePath(QStringLiteral("sd/sd-server.exe"));
    QDir().mkpath(QFileInfo(exe).absolutePath());
    if (!QFile::exists(exe) && !QFile::copy(QStringLiteral(FAKE_SERVER_PATH), exe))
        return {};
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/setup-test");
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = exe;
    rt.directory = QFileInfo(exe).absolutePath();
    studio.runtimes()->addInstalled(rt);
    studio.servers()->healthIntervalMs = 100;
    return rt.id;
}

runtime::ModelPreset TestSetup::tinyKlein(MockHttpServer &hf, const MockHttpServer &sd, QHash<QString, QByteArray> *contents)
{
    runtime::ModelPreset p = *runtime::presetById(QStringLiteral("flux2-klein-4b"));
    const QString revision = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    auto pin = [&](runtime::PresetFile &file, const QString &path, const QByteArray &bytes) {
        file.repo = QStringLiteral("org/tiny-klein");
        file.revision = revision;
        file.path = path;
        file.size = bytes.size();
        file.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        file.license = QStringLiteral("apache-2.0");
        contents->insert(path, bytes);
        hf.route("GET", QStringLiteral("/org/tiny-klein/resolve/%1/%2").arg(revision, path), [bytes](const MockRequest &) {
            MockResponse r;
            r.contentType = "application/octet-stream";
            r.body = bytes;
            return r;
        });
    };
    pin(p.download, QStringLiteral("flux-2-klein-4b-tiny.safetensors"), flux2Klein() + "model");
    for (runtime::PresetSlot &slot : p.components)
        pin(slot.file, slot.option == QLatin1String("vae") ? QStringLiteral("split_files/vae/flux2-vae.safetensors")
                                                            : QStringLiteral("split_files/text_encoders/qwen_3_4b.safetensors"),
            slot.option == QLatin1String("vae") ? flux2Vae() + "vae" : qwen3(2560) + "llm");
    p.serverOptions.insert(QStringLiteral("extra_args"), QStringLiteral("--fake-ready-ms 0 --fake-proxy %1").arg(sd.port()));
    return p;
}

// ---- The recommendation ------------------------------------------------------------------------------

void TestSetup::recommendation()
{
    using Fit = LocalRecommendation::Fit;
    // 8 GB: FLUX.2 klein 4B, which makes and edits images; no language model next to it.
    LocalRecommendation r = runtime::recommendLocalModel(rtx4060());
    QCOMPARE(r.fit, Fit::Full);
    QCOMPARE(r.presetId, QStringLiteral("flux2-klein-4b"));
    QCOMPARE(r.alternativeId, QStringLiteral("sdxl"));
    QCOMPARE(r.tier, runtime::VramTier::Small);
    QVERIFY(!r.languageModel);
    QCOMPARE(runtime::imageGpu(rtx4060())->name, QStringLiteral("NVIDIA GeForce RTX 4060 Laptop GPU")); // not the Intel one listed first
    // More memory: the same model, and a language model fits beside it.
    r = runtime::recommendLocalModel(hardware({gpu(0x10DE, 12282, QStringLiteral("RTX 4070"))}));
    QCOMPARE(r.presetId, QStringLiteral("flux2-klein-4b"));
    QCOMPARE(r.tier, runtime::VramTier::Medium);
    QVERIFY(r.languageModel);
    r = runtime::recommendLocalModel(hardware({gpu(0x10DE, 24564, QStringLiteral("RTX 4090"))}));
    QCOMPARE(r.tier, runtime::VramTier::Large);
    QVERIFY(r.languageModel);
    // AMD runs the Vulkan build: the same rules.
    QCOMPARE(runtime::recommendLocalModel(hardware({gpu(0x1002, 8176, QStringLiteral("Radeon RX 7600"))})).presetId, QStringLiteral("flux2-klein-4b"));
    // An 8 GB card that reports a little less still counts; 6 GB gets the lighter SDXL model.
    QCOMPARE(runtime::recommendLocalModel(hardware({gpu(0x10DE, 7680, QStringLiteral("8 GB card"))})).fit, Fit::Full);
    r = runtime::recommendLocalModel(hardware({gpu(0x10DE, 6144, QStringLiteral("RTX 3060 Laptop"))}));
    QCOMPARE(r.fit, Fit::Light);
    QCOMPARE(r.presetId, QStringLiteral("sdxl"));
    QCOMPARE(r.alternativeId, QStringLiteral("flux2-klein-4b"));
    QVERIFY(!r.languageModel);
    // No NVIDIA or AMD GPU (Intel Arc, an integrated Radeon, nothing): an online service.
    for (const HardwareInfo &hw : {hardware({gpu(0x8086, 8192, QStringLiteral("Intel Arc A770"))}),
                                   hardware({gpu(0x1002, 512, QStringLiteral("AMD Radeon(TM) Graphics"), true)}), hardware({})}) {
        r = runtime::recommendLocalModel(hw);
        QCOMPARE(r.fit, Fit::NoGpu);
        QCOMPARE(r.presetId, QStringLiteral("sdxl")); // if set up anyway, the lighter model
        QVERIFY(!runtime::imageGpu(hw));
    }
}

// ---- The download plan --------------------------------------------------------------------------------

// The recommended models' main files are pinned like their components, and an empty library plans
// them all, with sizes, from the ungated repositories.
void TestSetup::pinnedDownloads()
{
    for (const QString &id : {QStringLiteral("flux2-klein-4b"), QStringLiteral("sdxl")}) {
        const runtime::ModelPreset *p = runtime::presetById(id);
        QVERIFY2(p, qPrintable(id));
        QVERIFY2(p->download.isValid(), qPrintable(id));
        QCOMPARE(p->download.revision.size(), 40);
        QCOMPARE(p->download.sha256.size(), 64);
        QVERIFY(p->download.size > 1000000000);
        QVERIFY(!p->download.license.isEmpty());
        QCOMPARE(p->download.url().toString(),
                 QStringLiteral("https://huggingface.co/%1/resolve/%2/%3").arg(p->download.repo, p->download.revision, p->download.path));
    }
    // klein from the GGUF Q8_0 file that sd.cpp's docs list (leejet's repository).
    const runtime::ModelPreset *klein = runtime::presetById(QStringLiteral("flux2-klein-4b"));
    QCOMPARE(klein->download.repo, QStringLiteral("leejet/FLUX.2-klein-4B-GGUF"));
    QCOMPARE(klein->download.fileName(), QStringLiteral("flux-2-klein-4b-Q8_0.gguf"));
    QCOMPARE(klein->download.size, 4300629440LL);
    // Other presets have none: they are set up from files the user has.
    QVERIFY(!runtime::presetById(QStringLiteral("flux2-klein-base-4b"))->download.isValid());

    net::HttpClient http;
    QTemporaryDir data;
    runtime::ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();
    JobQueue jobs;
    runtime::ModelHub hub(&http, &jobs, &lib);
    const runtime::LocalModelPlan plan = runtime::planLocalModel(&lib, {}, *klein);
    QVERIFY(plan.ok());
    QCOMPARE(plan.files.size(), 3);
    QCOMPARE(plan.model()->option, QStringLiteral("diffusion_model"));
    QCOMPARE(plan.model()->kind, runtime::ModelKind::DiffusionModel);
    QCOMPARE(plan.downloads().size(), 3);
    QCOMPARE(plan.downloadSize(), 4300629440LL + 336211292LL + 8044982048LL);
    QVERIFY(plan.foldersToAdd().isEmpty());
    QStringList destinations;
    for (const runtime::PlannedFile &f : plan.downloads()) {
        const runtime::HubDownload d = runtime::plannedDownload(plan, f);
        QCOMPARE(d.file.size, f.file.size);
        QCOMPARE(d.file.sha256, f.file.sha256);
        QCOMPARE(d.license, f.file.license);
        QVERIFY(d.file.url.toString().startsWith(QStringLiteral("https://huggingface.co/%1/resolve/%2/").arg(f.file.repo, f.file.revision)));
        destinations << QDir(lib.defaultFolder()).relativeFilePath(hub.destinationFor(d));
    }
    QCOMPARE(destinations, (QStringList{QStringLiteral("diffusion_models/flux-2-klein-4b-Q8_0.gguf"), QStringLiteral("vae/flux2-vae.safetensors"),
                                        QStringLiteral("text_encoders/qwen_3_4b.safetensors")}));
    // A test server's address replaces huggingface.co.
    QVERIFY(runtime::plannedDownload(plan, plan.files[1], QStringLiteral("http://127.0.0.1:9")).file.url.toString().startsWith(QStringLiteral("http://127.0.0.1:9/Comfy-Org/")));

    const runtime::LocalModelPlan sdxl = runtime::planLocalModel(&lib, {}, *runtime::presetById(QStringLiteral("sdxl")));
    QCOMPARE(sdxl.files.size(), 1);
    QCOMPARE(sdxl.model()->option, QStringLiteral("model"));
    QCOMPARE(sdxl.downloadSize(), runtime::presetById(QStringLiteral("sdxl"))->download.size);
    QCOMPARE(QDir(lib.defaultFolder()).relativeFilePath(hub.destinationFor(runtime::plannedDownload(sdxl, *sdxl.model()))),
             QStringLiteral("checkpoints/RealVisXL_V5.0_fp16.safetensors"));
}

// Files already on this computer: in a folder the library does not know yet (a ComfyUI install),
// in the library, or only some of them.
void TestSetup::planFindsFiles()
{
    QTemporaryDir comfy;
    const QString models = QDir::cleanPath(comfy.filePath(QStringLiteral("models")));
    const QString klein = write(models + QStringLiteral("/diffusion_models/flux-2-klein-4b-fp8.safetensors"), flux2Klein());
    const QString base = write(models + QStringLiteral("/diffusion_models/flux-2-klein-base-4b.safetensors"), flux2Klein());
    const QString vae = write(models + QStringLiteral("/vae/flux2-vae.safetensors"), flux2Vae());
    const QString qwen4 = write(models + QStringLiteral("/text_encoders/qwen_3_4b.safetensors"), qwen3(2560));
    const QString qwen8 = write(models + QStringLiteral("/text_encoders/qwen_3_8b_fp8mixed.safetensors"), qwen3(4096));
    QVERIFY(!klein.isEmpty() && !base.isEmpty() && !vae.isEmpty() && !qwen4.isEmpty() && !qwen8.isEmpty());
    const runtime::ModelPreset &preset = *runtime::presetById(QStringLiteral("flux2-klein-4b"));

    net::HttpClient http;
    QTemporaryDir data;
    runtime::ModelLibrary lib(&http);
    lib.setDataDir(data.path());
    lib.load();

    // 1. In a suggested folder: everything is there, nothing to download, the folder is added.
    auto read = runtime::readFolders({models, comfy.filePath(QStringLiteral("missing"))});
    QTRY_VERIFY(read.isFinished());
    const QList<runtime::FoundFolder> found = read.result();
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().entries.size(), 5);
    runtime::LocalModelPlan plan = runtime::planLocalModel(&lib, found, preset);
    QVERIFY(plan.ok());
    QCOMPARE(plan.downloadSize(), 0);
    QVERIFY(plan.downloads().isEmpty());
    QCOMPARE(plan.model()->path, klein); // not the base model, which is another preset
    QCOMPARE(plan.model()->option, QStringLiteral("diffusion_model"));
    QCOMPARE(plan.files[1].path, vae);
    QCOMPARE(plan.files[2].path, qwen4); // Qwen3 4B, not 8B
    QCOMPARE(plan.foldersToAdd(), QStringList{models});
    // The server options: the model in its slot, its components, the 8 GB launch options.
    QHash<QString, QString> paths;
    for (const runtime::PlannedFile &f : plan.files)
        paths.insert(f.option, f.path);
    const QVariantMap options = runtime::planServerOptions(&lib, plan, paths, runtime::VramTier::Small);
    QCOMPARE(QDir::fromNativeSeparators(options.value(QStringLiteral("diffusion_model")).toString()), klein);
    QCOMPARE(options.value(QStringLiteral("model")).toString(), QString());
    QCOMPARE(QDir::fromNativeSeparators(options.value(QStringLiteral("vae")).toString()), vae);
    QCOMPARE(QDir::fromNativeSeparators(options.value(QStringLiteral("llm")).toString()), qwen4);
    QCOMPARE(options.value(QStringLiteral("weight_type")).toString(), QStringLiteral("q8_0"));
    QVERIFY(options.value(QStringLiteral("offload_to_cpu")).toBool());

    // 2. In the library already: the same files, no folder to add.
    QSignalSpy scanned(&lib, &runtime::ModelLibrary::scanFinished);
    QVERIFY(lib.addRoot(comfy.path()));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !lib.isScanning(), 20000);
    plan = runtime::planLocalModel(&lib, {}, preset);
    QCOMPARE(plan.model()->path, klein);
    QCOMPARE(plan.downloadSize(), 0);
    QVERIFY(plan.foldersToAdd().isEmpty());

    // 3. Only the VAE: the model and the text encoder are downloaded, the VAE is used.
    QTemporaryDir partial;
    write(partial.filePath(QStringLiteral("vae/ae-flux2.safetensors")), flux2Vae());
    auto readPartial = runtime::readFolders({partial.path()});
    QTRY_VERIFY(readPartial.isFinished());
    runtime::ModelLibrary empty(&http);
    QTemporaryDir data2;
    empty.setDataDir(data2.path());
    empty.load();
    plan = runtime::planLocalModel(&empty, readPartial.result(), preset);
    QVERIFY(plan.model()->missing());
    QCOMPARE(plan.files[1].path, QDir::cleanPath(partial.filePath(QStringLiteral("vae/ae-flux2.safetensors"))));
    QVERIFY(plan.files[2].missing());
    QCOMPARE(plan.downloadSize(), preset.download.size + preset.slot(QStringLiteral("llm"))->file.size);
    QCOMPARE(plan.foldersToAdd(), QStringList{QDir::cleanPath(partial.path())});
}

// ---- Run on this computer --------------------------------------------------------------------------------

// A ComfyUI folder has klein and its components: the wizard uses them, downloads nothing, sets up
// the server and the provider from the preset, and the test image comes from the server.
void TestSetup::localReusesFiles()
{
    QTemporaryDir dir;
    const QString models = QDir::cleanPath(dir.filePath(QStringLiteral("ComfyUI/models")));
    const QString klein = write(models + QStringLiteral("/diffusion_models/flux-2-klein-4b.safetensors"), flux2Klein());
    const QString vae = write(models + QStringLiteral("/vae/flux2-vae.safetensors"), flux2Vae());
    const QString qwen = write(models + QStringLiteral("/text_encoders/qwen_3_4b.safetensors"), qwen3(2560));
    MockHttpServer sd;
    int generations = 0;
    routeSdServer(sd, &generations);
    MockHttpServer hf; // must not be asked for anything
    QHash<QString, QByteArray> unused;

    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] { reset(studio); });
    QVERIFY(!adoptFakeServer(studio, dir.path()).isEmpty());
    studio.hub()->setBaseUrls(hf.baseUrl().toString(), hf.baseUrl().toString());

    SetupWizard wizard(&studio);
    wizard.hardwareOverride = rtx4060();
    wizard.searchFolders = QStringList{models};
    // The bundled klein preset, with its launch options pointing the fake server at the mock.
    runtime::ModelPreset preset = *runtime::presetById(QStringLiteral("flux2-klein-4b"));
    preset.serverOptions.insert(QStringLiteral("extra_args"), QStringLiteral("--fake-ready-ms 0 --fake-proxy %1").arg(sd.port()));
    wizard.presets = {preset, *runtime::presetById(QStringLiteral("sdxl"))};
    QSignalSpy planned(&wizard, &SetupWizard::localPlanReady);
    wizard.resize(780, 600);
    wizard.show();
    wizard.showPage(SetupWizard::Page::Local);
    QTRY_VERIFY_WITH_TIMEOUT(planned.size() == 1, 20000);

    // The suggestion for 8 GB, the files found, nothing to download.
    auto *model = named<QComboBox>(&wizard, QStringLiteral("setupModel"));
    QCOMPARE(model->currentData().toString(), QStringLiteral("flux2-klein-4b"));
    QCOMPARE(model->currentText(), QStringLiteral("FLUX.2 [klein] 4B (suggested)"));
    QCOMPARE(model->count(), 2);
    auto *tree = named<QTreeWidget>(&wizard, QStringLiteral("setupPlan"));
    QCOMPARE(tree->topLevelItemCount(), 1 + 3); // the program and three files
    QCOMPARE(tree->topLevelItem(0)->text(2), QStringLiteral("Installed"));
    for (int i = 1; i < 4; ++i)
        QCOMPARE(tree->topLevelItem(i)->text(2), QStringLiteral("On this computer (found)"));
    QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("FLUX.2 [klein] 4B — flux-2-klein-4b.safetensors"));
    QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("FLUX.2 VAE — flux2-vae.safetensors"));
    QCOMPARE(tree->topLevelItem(3)->text(0), QStringLiteral("Qwen3 4B text encoder — qwen_3_4b.safetensors"));
    QVERIFY(named<QLabel>(&wizard, QStringLiteral("setupSummary"))->text().contains(QLatin1String("nothing to download")));
    // Plugins that need a language model are on, but 8 GB gets none by default.
    auto *language = named<QCheckBox>(&wizard, QStringLiteral("setupLanguage"));
    QVERIFY(language->isVisibleTo(&wizard));
    QVERIFY(!language->isChecked());
    auto *start = named<QPushButton>(&wizard, QStringLiteral("setupStart"));
    QCOMPARE(start->text(), QStringLiteral("Set up"));
    QVERIFY(start->isEnabled());
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        wizard.grab().save(shots + QStringLiteral("/setup-local-found.png"));

    QSignalSpy finished(wizard.localSetup(), &LocalSetup::finished);
    QSignalSpy tested(&wizard, &SetupWizard::testImageFinished);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 20000);
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QVERIFY(wizard.localSetup()->jobs().isEmpty()); // nothing downloaded
    QCOMPARE(hf.requests.size(), 0);

    // One server: klein in the diffusion model slot with its VAE and text encoder, 8 GB options.
    const QList<runtime::ServerProfile> profiles = studio.servers()->profiles();
    QCOMPARE(profiles.size(), 1);
    const runtime::ServerProfile &p = profiles.first();
    QCOMPARE(p.name, QStringLiteral("FLUX.2 [klein] 4B"));
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("diffusion_model")).toString()), klein);
    QCOMPARE(p.options.value(QStringLiteral("model")).toString(), QString());
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("vae")).toString()), vae);
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("llm")).toString()), qwen);
    QCOMPARE(p.options.value(QStringLiteral("weight_type")).toString(), QStringLiteral("q8_0"));
    QVERIFY(p.options.value(QStringLiteral("offload_to_cpu")).toBool());
    QVERIFY(runtime::checkSdProfile(studio.models(), p).isEmpty());
    // The ComfyUI folder joined the library (read in place).
    bool rooted = false;
    for (const runtime::ModelRoot &r : studio.models()->roots())
        rooted |= QDir::cleanPath(r.path) == models;
    QVERIFY(rooted);
    // Its provider is the image engine and knows the preset; no language model was set up.
    ai::Provider *provider = studio.providerForServer(p.id);
    QVERIFY(provider);
    QCOMPARE(studio.providers()->defaultImageProvider(), provider);
    QCOMPARE(ai::modelDefaultsOf(provider).name, QStringLiteral("FLUX.2 [klein] 4B"));
    QVERIFY(studio.providers()->llmProviders().isEmpty());

    // The test image: the server starts (the fake one, relaying to the mock sd-server) and answers.
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 1, 30000);
    QVERIFY2(tested.first().at(0).toBool(), qPrintable(named<QLabel>(&wizard, QStringLiteral("resultText"))->text()));
    QCOMPARE(wizard.currentPage(), SetupWizard::Page::Result);
    QCOMPARE(wizard.testedProvider(), provider->instanceId());
    QCOMPARE(generations, 1);
    QVERIFY(!named<QLabel>(&wizard, QStringLiteral("resultImage"))->pixmap().isNull());
    if (!shots.isEmpty())
        wizard.grab().save(shots + QStringLiteral("/setup-result.png"));
    // Run again, the same files: the server is there already.
    wizard.showPage(SetupWizard::Page::Local);
    QTRY_VERIFY(start->isEnabled());
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 2, 20000);
    QCOMPARE(studio.servers()->profiles().size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 2, 30000);
    studio.servers()->stop(p.id);
}

// The profile of §17.1's log: FLUX.2 klein put in the checkpoint slot by hand, without its VAE and
// text encoder (and here with a ControlNet file that is gone), and no provider. The wizard does not
// take it as it is: it corrects it from the preset, keeping its name, port and program, adds its
// provider, and the test image works.
void TestSetup::localCorrectsServer()
{
    QTemporaryDir dir;
    const QString models = QDir::cleanPath(dir.filePath(QStringLiteral("ComfyUI/models")));
    const QString klein = write(models + QStringLiteral("/diffusion_models/flux-2-klein-4b.safetensors"), flux2Klein());
    const QString vae = write(models + QStringLiteral("/vae/flux2-vae.safetensors"), flux2Vae());
    const QString qwen = write(models + QStringLiteral("/text_encoders/qwen_3_4b.safetensors"), qwen3(2560));
    MockHttpServer sd;
    int generations = 0;
    routeSdServer(sd, &generations);

    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] { reset(studio); });
    const QString runtimeId = adoptFakeServer(studio, dir.path());
    QVERIFY(!runtimeId.isEmpty());
    runtime::ServerProfile broken = studio.servers()->createProfile(runtimeId, QStringLiteral("Klein by hand"));
    broken.options.insert(QStringLiteral("model"), QDir::toNativeSeparators(klein));
    broken.options.insert(QStringLiteral("control_net"), QDir::toNativeSeparators(dir.filePath(QStringLiteral("gone/openpose-sdxl.safetensors"))));
    addServer(&studio, broken, false);
    QVERIFY(!runtime::checkSdProfile(studio.models(), broken).isEmpty());

    SetupWizard wizard(&studio);
    wizard.hardwareOverride = rtx4060();
    wizard.searchFolders = QStringList{models};
    runtime::ModelPreset preset = *runtime::presetById(QStringLiteral("flux2-klein-4b"));
    preset.serverOptions.insert(QStringLiteral("extra_args"), QStringLiteral("--fake-ready-ms 0 --fake-proxy %1").arg(sd.port()));
    wizard.presets = {preset, *runtime::presetById(QStringLiteral("sdxl"))};
    QSignalSpy planned(&wizard, &SetupWizard::localPlanReady);
    wizard.resize(780, 600);
    wizard.show();
    wizard.showPage(SetupWizard::Page::Local);
    QTRY_VERIFY_WITH_TIMEOUT(planned.size() == 1, 20000);
    // The plan says so.
    const QString summary = named<QLabel>(&wizard, QStringLiteral("setupSummary"))->text();
    QVERIFY2(summary.contains(QLatin1String("Klein by hand")) && summary.contains(QLatin1String("Lumen corrects them")), qPrintable(summary));

    QSignalSpy finished(wizard.localSetup(), &LocalSetup::finished);
    QSignalSpy tested(&wizard, &SetupWizard::testImageFinished);
    named<QPushButton>(&wizard, QStringLiteral("setupStart"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 20000);
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QVERIFY2(finished.first().at(1).toString().contains(QLatin1String("Corrected the settings of Klein by hand")),
             qPrintable(finished.first().at(1).toString()));

    // The same server, corrected: klein in the diffusion model slot with its VAE and text encoder,
    // the 8 GB options, no ControlNet; its name, port and program kept.
    const QList<runtime::ServerProfile> profiles = studio.servers()->profiles();
    QCOMPARE(profiles.size(), 1);
    const runtime::ServerProfile &p = profiles.first();
    QCOMPARE(p.id, broken.id);
    QCOMPARE(p.name, QStringLiteral("Klein by hand"));
    QCOMPARE(p.port, broken.port);
    QCOMPARE(p.runtimeId, runtimeId);
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("diffusion_model")).toString()), klein);
    QCOMPARE(p.options.value(QStringLiteral("model")).toString(), QString());
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("vae")).toString()), vae);
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("llm")).toString()), qwen);
    QCOMPARE(p.options.value(QStringLiteral("weight_type")).toString(), QStringLiteral("q8_0"));
    QVERIFY(!p.options.contains(QStringLiteral("control_net")));
    QVERIFY(runtime::checkSdProfile(studio.models(), p).isEmpty());
    // It had no provider: now it has one, the image engine.
    ai::Provider *provider = studio.providerForServer(p.id);
    QVERIFY(provider);
    QCOMPARE(studio.providers()->defaultImageProvider(), provider);
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 1, 30000);
    QVERIFY2(tested.first().at(0).toBool(), qPrintable(named<QLabel>(&wizard, QStringLiteral("resultText"))->text()));
    QCOMPARE(generations, 1);

    // Planned again, it runs as it is now.
    wizard.showPage(SetupWizard::Page::Local);
    QTRY_VERIFY(named<QPushButton>(&wizard, QStringLiteral("setupStart"))->isEnabled());
    QVERIFY(named<QLabel>(&wizard, QStringLiteral("setupSummary"))->text().contains(QLatin1String("runs this model already")));
    studio.servers()->stop(p.id);
}

// Nothing on this computer: the plan lists the downloads with their sizes, the setup downloads them
// (checked against the pinned size and checksum) into the models folder, and sets the server up.
void TestSetup::localDownloadsMissing()
{
    QTemporaryDir dir;
    MockHttpServer hf;
    MockHttpServer sd;
    routeSdServer(sd);
    QHash<QString, QByteArray> contents;
    const runtime::ModelPreset preset = tinyKlein(hf, sd, &contents);

    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] { reset(studio); });
    QVERIFY(!adoptFakeServer(studio, dir.path()).isEmpty());
    studio.hub()->setBaseUrls(hf.baseUrl().toString(), hf.baseUrl().toString());
    const QString own = studio.models()->defaultFolder();

    SetupWizard wizard(&studio);
    wizard.hardwareOverride = rtx4060();
    wizard.searchFolders = QStringList();
    wizard.presets = {preset, *runtime::presetById(QStringLiteral("sdxl"))};
    QSignalSpy planned(&wizard, &SetupWizard::localPlanReady);
    wizard.resize(780, 600);
    wizard.show();
    wizard.showPage(SetupWizard::Page::Local);
    QTRY_VERIFY_WITH_TIMEOUT(planned.size() == 1, 20000);

    auto *tree = named<QTreeWidget>(&wizard, QStringLiteral("setupPlan"));
    QCOMPARE(tree->topLevelItemCount(), 1 + 3);
    qint64 total = 0;
    for (int i = 1; i < 4; ++i) {
        QCOMPARE(tree->topLevelItem(i)->text(2), QStringLiteral("Download from Hugging Face"));
        QVERIFY(!tree->topLevelItem(i)->text(1).isEmpty()); // the size
        QCOMPARE(tree->topLevelItem(i)->text(3), QStringLiteral("apache-2.0"));
    }
    for (const QByteArray &b : std::as_const(contents))
        total += b.size();
    QCOMPARE(wizard.localPlan().downloadSize(), total);
    const QString summary = named<QLabel>(&wizard, QStringLiteral("setupSummary"))->text();
    QVERIFY2(summary.contains(runtime::formatBytes(total)), qPrintable(summary));
    QVERIFY(summary.contains(QLatin1String("100 Mbit/s")));
    auto *start = named<QPushButton>(&wizard, QStringLiteral("setupStart"));
    QCOMPARE(start->text(), QStringLiteral("Download and set up"));
    // The lighter choice: one file, from the SDXL preset's pinned repository.
    auto *model = named<QComboBox>(&wizard, QStringLiteral("setupModel"));
    model->setCurrentIndex(1);
    QCOMPARE(wizard.localPlan().preset.id, QStringLiteral("sdxl"));
    QCOMPARE(tree->topLevelItemCount(), 1 + 1);
    QCOMPARE(tree->topLevelItem(1)->text(1), runtime::formatBytes(runtime::presetById(QStringLiteral("sdxl"))->download.size));
    model->setCurrentIndex(0);
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        wizard.grab().save(shots + QStringLiteral("/setup-local-download.png"));

    // Use models from a folder…: one Lumen did not look in, with the VAE. It is used where it is;
    // two downloads are left.
    QTemporaryDir other;
    const QString foundVae = write(other.filePath(QStringLiteral("vae/ae-flux2.safetensors")), flux2Vae());
    QSignalSpy read(&wizard, &SetupWizard::folderRead);
    wizard.useModelsFolder(QDir::toNativeSeparators(other.path()));
    QTRY_VERIFY_WITH_TIMEOUT(read.size() == 1, 10000);
    QCOMPARE(tree->topLevelItem(2)->text(2), QStringLiteral("On this computer (found)"));
    QCOMPARE(wizard.localPlan().downloads().size(), 2);
    QCOMPARE(wizard.localPlan().foldersToAdd(), QStringList{QDir::cleanPath(other.path())});
    QVERIFY(named<QLabel>(&wizard, QStringLiteral("setupSummary"))->text().contains(QLatin1String("found in")));
    QCOMPARE(start->text(), QStringLiteral("Download and set up"));

    QSignalSpy finished(wizard.localSetup(), &LocalSetup::finished);
    QSignalSpy tested(&wizard, &SetupWizard::testImageFinished);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 30000);
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(wizard.localSetup()->jobs().size(), 2);
    const QList<runtime::ServerProfile> profiles = studio.servers()->profiles();
    QCOMPARE(profiles.size(), 1);
    const runtime::ServerProfile &p = profiles.first();
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("diffusion_model")).toString()),
             own + QStringLiteral("/diffusion_models/flux-2-klein-4b-tiny.safetensors"));
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("vae")).toString()), foundVae);
    QCOMPARE(QDir::fromNativeSeparators(p.options.value(QStringLiteral("llm")).toString()), own + QStringLiteral("/text_encoders/qwen_3_4b.safetensors"));
    QVERIFY(QFileInfo::exists(runtime::sidecarPath(own + QStringLiteral("/diffusion_models/flux-2-klein-4b-tiny.safetensors"))));
    QVERIFY(runtime::checkSdProfile(studio.models(), p).isEmpty());
    bool rooted = false;
    for (const runtime::ModelRoot &r : studio.models()->roots())
        rooted |= QDir::cleanPath(r.path) == QDir::cleanPath(other.path());
    QVERIFY(rooted);
    ai::Provider *provider = studio.providerForServer(p.id);
    QVERIFY(provider);
    QCOMPARE(studio.providers()->defaultImageProvider(), provider);
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 1, 30000);
    QVERIFY2(tested.first().at(0).toBool(), qPrintable(named<QLabel>(&wizard, QStringLiteral("resultText"))->text()));
    // Done, the test image shows no progress any more.
    QVERIFY(!named<QWidget>(&wizard, QStringLiteral("testProgress"))->isVisibleTo(&wizard));
    studio.servers()->stop(p.id);
}

// ---- Use an online service -------------------------------------------------------------------------------

void TestSetup::onlineService()
{
    MockHttpServer openai;
    QJsonObject generation;
    int checks = 0;
    int generations = 0;
    int delayMs = 0;
    auto authorised = [](const MockRequest &req) { return req.headers.value("authorization") == "Bearer good-key"; };
    openai.route("GET", QStringLiteral("/v1/models"), [authorised, &checks](const MockRequest &req) {
        ++checks;
        if (!authorised(req))
            return MockResponse::json({{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("Incorrect API key provided")}}}}, 401);
        return MockResponse::json({{QStringLiteral("data"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-image-1")}}}}});
    });
    openai.route("POST", QStringLiteral("/v1/images/generations"), [&](const MockRequest &req) {
        generation = req.json();
        ++generations;
        MockResponse r = MockResponse::json({{QStringLiteral("data"), QJsonArray{QJsonObject{{QStringLiteral("b64_json"), pngB64(Qt::darkCyan, QSize(64, 64))}}}}});
        r.delayMs = delayMs;
        return r;
    });

    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] { reset(studio); }); // also removes the stored key
    SetupWizard wizard(&studio);
    wizard.hardwareOverride = rtx4060();
    wizard.searchFolders = QStringList();
    wizard.serviceUrls.insert(QStringLiteral("openai_image"), openai.baseUrl().toString() + QStringLiteral("/v1"));
    studio.providers()->serverProbes.clear();
    wizard.resize(780, 600);
    wizard.show();
    wizard.showPage(SetupWizard::Page::Online);

    // fal.ai first, with where to get a key.
    auto *status = named<QLabel>(&wizard, QStringLiteral("serviceStatus"));
    QVERIFY(named<QRadioButton>(&wizard, QStringLiteral("service-fal"))->isChecked());
    QLabel *link = nullptr;
    for (QLabel *l : wizard.findChildren<QLabel *>()) {
        if (l->text().contains(QLatin1String("fal.ai/dashboard/keys")))
            link = l;
    }
    QVERIFY(link);
    named<QRadioButton>(&wizard, QStringLiteral("service-openai_image"))->click();
    QVERIFY(link->text().contains(QLatin1String("platform.openai.com/api-keys")));
    auto *connectButton = named<QPushButton>(&wizard, QStringLiteral("connectService"));
    connectButton->click();
    QCOMPARE(status->text(), QStringLiteral("Paste the key first."));
    QVERIFY(studio.providers()->providers().isEmpty());

    // A wrong key: said in plain words, no test image, and nothing added: no engine for a key the
    // service refused.
    auto *key = named<QLineEdit>(&wizard, QStringLiteral("serviceKey"));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->setText(QStringLiteral("bad-key"));
    connectButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QLatin1String("The service did not accept the key")), 10000);
    QCOMPARE(wizard.currentPage(), SetupWizard::Page::Online);
    QVERIFY(generation.isEmpty());
    QVERIFY(studio.providers()->providers().isEmpty());
    QVERIFY(!studio.providers()->defaultImageProvider());

    // Another engine is the image engine, and the editor's AI panel shows it.
    ai::Provider *other = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Other sd-server"),
                                                          {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}});
    QVERIFY(other);
    AiPanel panel(&studio);
    auto *panelEngine = panel.findChild<ProviderCombo *>();
    QVERIFY(panelEngine);
    QCOMPARE(panelEngine->providerId(), other->instanceId());

    // The right one, with Enter pressed twice while it is checked: one check, one test image. The
    // service gets a provider and becomes the image engine, the panel follows, and the test is cheap.
    QSignalSpy tested(&wizard, &SetupWizard::testImageFinished);
    key->setText(QStringLiteral("good-key"));
    checks = 0;
    emit key->returnPressed();
    emit key->returnPressed();
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 1, 10000);
    QVERIFY2(tested.first().at(0).toBool(), qPrintable(named<QLabel>(&wizard, QStringLiteral("resultText"))->text()));
    QCOMPARE(checks, 1);
    QCOMPARE(generations, 1);
    QCOMPARE(studio.providers()->providers().size(), 2);
    ai::ImageProvider *provider = studio.providers()->defaultImageProvider();
    QVERIFY(provider);
    QCOMPARE(provider->type().id, QStringLiteral("openai_image"));
    QCOMPARE(provider->displayName(), QStringLiteral("OpenAI"));
    const QString id = provider->instanceId();
    QCOMPARE(wizard.testedProvider(), id);
    QCOMPARE(panelEngine->providerId(), id);
    QCOMPARE(generation.value(QStringLiteral("quality")).toString(), QStringLiteral("low"));
    QVERIFY(!generation.value(QStringLiteral("prompt")).toString().isEmpty());
    QCOMPARE(wizard.currentPage(), SetupWizard::Page::Result);
    QVERIFY(!named<QLabel>(&wizard, QStringLiteral("resultImage"))->pixmap().isNull());
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        wizard.grab().save(shots + QStringLiteral("/setup-online-result.png"));

    // A mistyped key later, for the service that works (turned off meanwhile): refused before it
    // replaces anything, so the old key stays stored and the engine stays off.
    studio.providers()->setEnabled(id, false);
    wizard.showPage(SetupWizard::Page::Online);
    key->setText(QStringLiteral("good-key-typo"));
    connectButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QLatin1String("The service did not accept the key")), 10000);
    QCOMPARE(studio.providers()->settingsFor(id).value(QStringLiteral("api_key")).toString(), QStringLiteral("good-key"));
    QCOMPARE(SecretStore::read(QStringLiteral("provider/%1/api_key").arg(id)), QStringLiteral("good-key"));
    QVERIFY(!studio.providers()->isEnabled(id));
    QCOMPARE(studio.providers()->providers().size(), 2);
    studio.providers()->setEnabled(id, true);

    // Picked by hand in the panel, a provider stays when the engine changes; picking the engine of
    // Settings → AI again follows it. The panel changes images, so it follows the engine for changes.
    panelEngine->setProviderId(other->instanceId());
    studio.providers()->setDefaultImageProvider(id);
    QCOMPARE(panelEngine->providerId(), other->instanceId());
    panelEngine->setProviderId(id);
    QVERIFY(panelEngine->followsSettings());
    studio.providers()->setEditImageProvider(other->instanceId());
    QCOMPARE(panelEngine->providerId(), other->instanceId());
    studio.providers()->setEditImageProvider(QString());
    QCOMPARE(panelEngine->providerId(), id);

    // The test image shows its progress and can be stopped; the engine stays set up.
    delayMs = 5000;
    wizard.runTestImage(id, QStringLiteral("OpenAI is connected"), QStringLiteral("Another test."));
    auto *stop = named<QPushButton>(&wizard, QStringLiteral("stopTest"));
    QVERIFY(stop->isVisibleTo(&wizard));
    QVERIFY(named<QWidget>(&wizard, QStringLiteral("testProgress"))->isVisibleTo(&wizard));
    QTRY_VERIFY_WITH_TIMEOUT(generations == 2, 10000);
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 2, 10000);
    QVERIFY(!tested.last().at(0).toBool());
    QVERIFY(named<QLabel>(&wizard, QStringLiteral("resultText"))->text().startsWith(QLatin1String("The test image was stopped")));
    QVERIFY(!stop->isVisibleTo(&wizard));
    QCOMPARE(studio.providers()->defaultImageProvider()->instanceId(), id);
}

// ---- Connect a running server ------------------------------------------------------------------------------

void TestSetup::runningServer()
{
    MockHttpServer comfy;
    comfy.route("GET", QStringLiteral("/system_stats"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("system"), QJsonObject{{QStringLiteral("os"), QStringLiteral("nt")}}}, {QStringLiteral("devices"), QJsonArray{}}});
    });
    comfy.route("GET", QStringLiteral("/object_info/CheckpointLoaderSimple"), [](const MockRequest &) {
        const QJsonObject required{{QStringLiteral("ckpt_name"), QJsonArray{QJsonArray{QStringLiteral("realvisxlV50.safetensors"), QStringLiteral("other.safetensors")}}}};
        return MockResponse::json({{QStringLiteral("CheckpointLoaderSimple"),
                                    QJsonObject{{QStringLiteral("input"), QJsonObject{{QStringLiteral("required"), required}}}}}});
    });
    MockHttpServer sd;
    int generations = 0;
    routeSdServer(sd, &generations);
    quint16 deadPort = 0;
    {
        MockHttpServer gone;
        deadPort = gone.port();
    }
    const QString dead = QStringLiteral("http://127.0.0.1:%1").arg(deadPort);

    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] { reset(studio); });
    // The usual ports, here: ComfyUI answers, WebUI does not; language model servers are not listed.
    studio.providers()->serverProbes = {{QStringLiteral("comfyui"), QStringLiteral("ComfyUI"), comfy.baseUrl().toString(), QStringLiteral("/system_stats")},
                                        {QStringLiteral("sdwebui"), QStringLiteral("Stable Diffusion WebUI"), dead, QStringLiteral("/sdapi/v1/sd-models")},
                                        {QStringLiteral("llamacpp"), QStringLiteral("llama.cpp server"), comfy.baseUrl().toString(), QStringLiteral("/system_stats")}};
    SetupWizard wizard(&studio);
    wizard.hardwareOverride = rtx4060();
    wizard.searchFolders = QStringList();
    QSignalSpy found(&wizard, &SetupWizard::serversFound);
    wizard.resize(780, 600);
    wizard.show();
    QTRY_VERIFY_WITH_TIMEOUT(found.size() == 1, 10000);
    // The first page says what was found.
    bool hinted = false;
    for (QLabel *l : wizard.findChildren<QLabel *>())
        hinted |= l->text() == QStringLiteral("Found: ComfyUI (%1).").arg(comfy.baseUrl().authority());
    QVERIFY(hinted);
    wizard.showPage(SetupWizard::Page::Server);
    QList<QPushButton *> uses = wizard.findChildren<QPushButton *>(QStringLiteral("useServer"));
    QCOMPARE(uses.size(), 1);
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        wizard.grab().save(shots + QStringLiteral("/setup-server.png"));

    // Using it adds ComfyUI as the image engine with its first checkpoint, and asks it for a test
    // image (this mock makes none).
    QSignalSpy tested(&wizard, &SetupWizard::testImageFinished);
    uses.first()->click();
    ai::ImageProvider *provider = studio.providers()->defaultImageProvider();
    QVERIFY(provider);
    QCOMPARE(provider->type().id, QStringLiteral("comfyui"));
    QCOMPARE(provider->text(QStringLiteral("url")), comfy.baseUrl().toString());
    QTRY_COMPARE_WITH_TIMEOUT(wizard.currentPage(), SetupWizard::Page::Result, 10000);
    QCOMPARE(provider->text(QStringLiteral("model")), QStringLiteral("realvisxlV50.safetensors"));
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 1, 20000);
    QVERIFY(!tested.first().at(0).toBool());
    QVERIFY(named<QLabel>(&wizard, QStringLiteral("resultText"))->text().startsWith(QLatin1String("The test image did not work")));
    // Found again, it is not added twice. Two searches at once (Search again while the first
    // still waits) list it once.
    wizard.showPage(SetupWizard::Page::Server);
    QPushButton *again = buttonWithText(&wizard, QStringLiteral("Search again"));
    QVERIFY(again);
    again->click();
    again->click();
    QTRY_VERIFY_WITH_TIMEOUT(found.size() == 2, 10000);
    QTest::qWait(300); // the first search's answer too
    QCOMPARE(found.size(), 2);
    uses = wizard.findChildren<QPushButton *>(QStringLiteral("useServer"));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.first()->text(), QStringLiteral("Use it again"));
    uses.first()->click();
    QCOMPARE(studio.providers()->providers().size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 2, 20000);

    // By address: nothing answers, then an sd-server does, which makes the test image.
    wizard.showPage(SetupWizard::Page::Server);
    auto *address = named<QLineEdit>(&wizard, QStringLiteral("serverAddress"));
    auto *status = named<QLabel>(&wizard, QStringLiteral("serverStatus"));
    address->setText(dead);
    named<QPushButton>(&wizard, QStringLiteral("connectAddress"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith(QLatin1String("No ComfyUI, WebUI / Forge or sd-server answers")), 10000);
    address->setText(sd.baseUrl().authority()); // "127.0.0.1:port", without http://
    named<QPushButton>(&wizard, QStringLiteral("connectAddress"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(tested.size() == 3, 20000);
    QVERIFY2(tested.last().at(0).toBool(), qPrintable(named<QLabel>(&wizard, QStringLiteral("resultText"))->text()));
    provider = studio.providers()->defaultImageProvider();
    QCOMPARE(provider->type().id, QStringLiteral("sdcpp"));
    QCOMPARE(provider->text(QStringLiteral("url")), sd.baseUrl().toString());
    QCOMPARE(generations, 1);
    QCOMPARE(studio.providers()->providers().size(), 2);

    // An sd-server also answers WebUI's API: on a port both could use it is listed once, as sd-server.
    sd.route("GET", QStringLiteral("/sdapi/v1/sd-models"), [](const MockRequest &) { return MockResponse::json(QJsonObject{}); });
    studio.providers()->serverProbes = {{QStringLiteral("sdwebui"), QStringLiteral("Stable Diffusion WebUI (Forge)"), sd.baseUrl().toString(), QStringLiteral("/sdapi/v1/sd-models")},
                                        {QStringLiteral("sdcpp"), QStringLiteral("stable-diffusion.cpp (sd-server)"), sd.baseUrl().toString(), QStringLiteral("/sdcpp/v1/capabilities")}};
    wizard.showPage(SetupWizard::Page::Server);
    again->click();
    QTRY_VERIFY_WITH_TIMEOUT(found.size() == 3, 10000);
    uses = wizard.findChildren<QPushButton *>(QStringLiteral("useServer"));
    QCOMPARE(uses.size(), 1);
    QCOMPARE(uses.first()->text(), QStringLiteral("Use it again")); // the sd-server added above
}

// ---- Settings → AI ---------------------------------------------------------------------------------------

void TestSetup::settingsAi()
{
    QTemporaryDir dir;
    const QString klein = write(dir.filePath(QStringLiteral("models/flux-2-klein-4b.safetensors")), flux2Klein());
    const QString vae = write(dir.filePath(QStringLiteral("models/flux2-vae.safetensors")), flux2Vae());
    const QString qwen = write(dir.filePath(QStringLiteral("models/qwen_3_4b.safetensors")), qwen3(2560));
    quint16 deadPort = 0;
    {
        MockHttpServer gone;
        deadPort = gone.port();
    }

    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] {
        reset(studio);
        ext::Extensions::instance().setEnabled(QStringLiteral("characters"), true);
        ext::Extensions::instance().refresh();
    });
    {
        // Nothing set up: the rows say so and Change… sets one up.
        SettingsDialog dlg(&studio);
        dlg.showTab(QStringLiteral("ai"));
        auto *tabs = dlg.findChild<QTabWidget *>();
        QCOMPARE(tabs->currentIndex(), 0); // the first tab
        QCOMPARE(tabs->tabText(0), QStringLiteral("AI"));
        QVERIFY(qobject_cast<AiSettingsPage *>(tabs->currentWidget()));
        QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiImageEngine"))->fullText(), QStringLiteral("No image engine yet"));
        QCOMPARE(named<QLabel>(&dlg, QStringLiteral("aiImageStatus"))->text(), QStringLiteral("Not set up"));
        QCOMPARE(named<QPushButton>(&dlg, QStringLiteral("aiImageChange"))->text(), QStringLiteral("Set up…"));
        QVERIFY(!named<QPushButton>(&dlg, QStringLiteral("aiImageChange"))->menu()); // nothing to pick: it opens the wizard
    }

    // A server Lumen runs with FLUX.2 klein from the preset, and a second, unmanaged engine.
    const QString runtimeId = adoptFakeServer(studio, dir.path());
    QVERIFY(!runtimeId.isEmpty());
    runtime::ServerProfile profile = studio.servers()->createProfile(runtimeId, QStringLiteral("Klein here"));
    profile.options = runtime::modelServerOptions(studio.models(), klein, runtime::VramTier::Small);
    profile.options.insert(QStringLiteral("vae"), QDir::toNativeSeparators(vae));
    profile.options.insert(QStringLiteral("llm"), QDir::toNativeSeparators(qwen));
    addServer(&studio, profile, true);
    ai::Provider *managed = studio.providerForServer(profile.id);
    QVERIFY(managed);
    studio.providers()->setDefaultImageProvider(managed->instanceId());
    ai::Provider *other = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Other sd-server"),
                                                          {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:%1").arg(deadPort)}});
    QVERIFY(other);

    SettingsDialog dlg(&studio);
    dlg.resize(1020, 640);
    dlg.show();
    dlg.showTab(QStringLiteral("ai"));
    // The image engine: its name, the preset's name, and that it starts when needed.
    QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiImageEngine"))->fullText(), QStringLiteral("Klein here"));
    QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiImageModel"))->fullText(), QStringLiteral("Model: FLUX.2 [klein] 4B"));
    QCOMPARE(named<QLabel>(&dlg, QStringLiteral("aiImageStatus"))->text(), QStringLiteral("On this computer; starts when it is needed"));
    QCOMPARE(named<QPushButton>(&dlg, QStringLiteral("aiImageChange"))->text(), QStringLiteral("Change…"));
    QVERIFY(named<QPushButton>(&dlg, QStringLiteral("aiImageChange"))->menu());
    // Changes use the image engine until another is chosen.
    QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiEditEngine"))->fullText(), QStringLiteral("The image engine (Klein here)"));
    ai::GenerationSpec edit;
    edit.request.mode = ai::GenMode::Edit;
    ai::GenerationSpec fresh;
    QCOMPARE(studio.generation()->providerFor(edit), managed);
    QCOMPARE(studio.generation()->providerFor(fresh), managed);
    // Change… lists the engines: the other one for changes.
    QMenu *menu = named<QPushButton>(&dlg, QStringLiteral("aiEditChange"))->menu();
    QVERIFY(menu);
    QMetaObject::invokeMethod(menu, "aboutToShow");
    QStringList entries;
    QAction *pick = nullptr;
    for (QAction *a : menu->actions()) {
        entries << a->text();
        if (a->text().startsWith(QLatin1String("Other sd-server")))
            pick = a;
    }
    QVERIFY2(entries.contains(QStringLiteral("The image engine")), qPrintable(entries.join(QLatin1Char('|'))));
    QVERIFY(entries.contains(QStringLiteral("Klein here — FLUX.2 [klein] 4B")));
    QVERIFY(entries.contains(QStringLiteral("Set up a new engine…")));
    QVERIFY(pick);
    pick->trigger();
    QCOMPARE(studio.providers()->editImageProviderId(), other->instanceId());
    QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiEditEngine"))->fullText(), QStringLiteral("Other sd-server"));
    QCOMPARE(studio.generation()->providerFor(edit), other);
    QCOMPARE(studio.generation()->providerFor(fresh), managed);
    ai::GenerationSpec region;
    region.region = ai::RegionInput{};
    QCOMPARE(studio.generation()->providerFor(region), other);
    // A submitted edit without a provider goes there (it fails: nothing answers there).
    edit.request.prompt = QStringLiteral("colorize it");
    edit.saveAssets = false;
    Job *job = studio.generation()->submit(edit);
    QCOMPARE(job->metadata().value(QStringLiteral("provider")).toString(), QStringLiteral("Other sd-server"));
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    // An engine Lumen does not run is checked once: this one does not answer.
    QTRY_VERIFY_WITH_TIMEOUT(named<QLabel>(&dlg, QStringLiteral("aiEditStatus"))->text().startsWith(QLatin1String("Not reachable")), 10000);
    // The choice is kept with the providers, and goes when the engine is removed.
    studio.providers()->save();
    {
        ai::ProviderRegistry reloaded(studio.http());
        ai::registerBuiltInProviders(reloaded);
        reloaded.load();
        QCOMPARE(reloaded.editImageProviderId(), other->instanceId());
    }
    // The language model: listed while the Characters plugin is on.
    auto *languageRow = named<QFrame>(&dlg, QStringLiteral("aiLanguage"));
    QVERIFY(languageRow->isVisibleTo(&dlg));
    QVERIFY(named<QLabel>(&dlg, QStringLiteral("aiLanguageText"))->text().contains(QLatin1String("Characters")));
    QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiLanguageEngine"))->fullText(), QStringLiteral("No language model"));
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        dlg.grab().save(shots + QStringLiteral("/settings-ai.png"));
    studio.providers()->removeProvider(other->instanceId());
    QCOMPARE(studio.providers()->editImageProviderId(), QString());
    QCOMPARE(named<ElidedLabel>(&dlg, QStringLiteral("aiEditEngine"))->fullText(), QStringLiteral("The image engine (Klein here)"));

    // With every plugin that needs one off, no language model row.
    ext::Extensions::instance().setEnabled(QStringLiteral("characters"), false);
    ext::Extensions::instance().refresh();
    SettingsDialog without(&studio);
    without.show();
    without.showTab(QStringLiteral("ai"));
    QVERIFY(!named<QFrame>(&without, QStringLiteral("aiLanguage"))->isVisibleTo(&without));
}

// ---- Where the wizard opens -------------------------------------------------------------------------------

void TestSetup::entryPoints()
{
    Studio studio;
    reset(studio);
    auto guard = qScopeGuard([&] {
        if (SetupWizard *w = openWizard())
            w->close();
        Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
        reset(studio);
    });
    studio.providers()->serverProbes.clear();
    studio.runtimes()->feed()->setApiBase(QUrl(QStringLiteral("http://127.0.0.1:9"))); // no GitHub lookups

    // The first start (no engine, not asked before) opens it on its first page, and only once.
    Settings::instance().setValue(QStringLiteral("app/onboarded"), false);
    {
        MainWindow window(&studio);
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(openWizard(), 5000);
        QCOMPARE(openWizard()->currentPage(), SetupWizard::Page::Choose);
        QVERIFY(Settings::instance().value(QStringLiteral("app/onboarded")).toBool());
        openWizard()->close();
        QTRY_VERIFY(!openWizard());

        // Help → Set up AI…
        QAction *help = window.findChild<QAction *>(QStringLiteral("setUpAi"));
        QVERIFY(help);
        help->trigger();
        QVERIFY(openWizard());
        QCOMPARE(openWizard()->currentPage(), SetupWizard::Page::Choose);
        // A second call brings the same one forward, on the page asked for.
        SetupWizard *first = openWizard();
        QCOMPARE(openSetupWizard(&studio, &window, SetupWizard::Page::Online), first);
        QCOMPARE(first->currentPage(), SetupWizard::Page::Online);
        QCOMPARE(first->parentWidget(), &window);

        // Asked for from Settings (modal, so it would block a wizard of the main window): the open
        // wizard moves over Settings, and back to the main window when Settings closes.
        {
            SettingsDialog settings(&studio, &window);
            settings.setWindowModality(Qt::ApplicationModal);
            settings.show();
            QCOMPARE(QApplication::activeModalWidget(), &settings);
            settings.showTab(QStringLiteral("ai"));
            named<QPushButton>(&settings, QStringLiteral("aiSetUp"))->click();
            QCOMPARE(openWizard(), first);
            QCOMPARE(first->parentWidget(), &settings);
            QVERIFY(first->isVisible());
            settings.reject();
            QCOMPARE(first->parentWidget(), &window);
            QVERIFY(first->isVisible());
        }
        first->close();
        QTRY_VERIFY(!openWizard());

        // Opened from Settings, it does not close with it either.
        {
            SettingsDialog settings(&studio, &window);
            settings.setWindowModality(Qt::ApplicationModal);
            settings.show();
            settings.showTab(QStringLiteral("ai"));
            named<QPushButton>(&settings, QStringLiteral("aiImageChange"))->click(); // no engine: Set up… opens the wizard
            SetupWizard *fromSettings = openWizard();
            QVERIFY(fromSettings);
            QCOMPARE(fromSettings->parentWidget(), &settings);
            settings.accept();
        }
        QVERIFY(openWizard());
        QCOMPARE(openWizard()->parentWidget(), &window);
        openWizard()->close();
        QTRY_VERIFY(!openWizard());
    }
    {
        // Not again on the next start.
        MainWindow window(&studio);
        window.show();
        QTest::qWait(700);
        QVERIFY(!openWizard());
    }
    // The Local servers page's button opens it on "Run on this computer".
    LocalServersPage page(&studio);
    page.show();
    QPushButton *local = buttonWithText(&page, QStringLiteral("Set up local AI…"));
    QVERIFY(local);
    local->click();
    QTRY_VERIFY(openWizard());
    QCOMPARE(openWizard()->currentPage(), SetupWizard::Page::Local);
}

QTEST_MAIN(TestSetup)
#include "tst_setup.moc"
