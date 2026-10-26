// The node editor, saved filters and the Filters panel (M17 §17.6, plan 17.8 "Node editor window,
// saved filters, the Filters panel"), and the AI nodes (plan 17.8 "AI nodes"). Synthetic images,
// test node types and mock image servers only; nothing runs on the GPU.

#include "MockHttpServer.h"
#include "ai/FilterNodes.h"
#include "ai/providers/SdCppProvider.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/Paths.h"
#include "doc/Document.h"
#include "filters/Ops.h"
#include "filters/Registry.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/FilterRunner.h"
#include "ui/filters/FiltersPanel.h"
#include "ui/filters/GraphModel.h"
#include "ui/filters/NodeEditorWindow.h"
#include "ui/filters/NodeScene.h"
#include "ui/filters/ParamWidgets.h"
#include "ui/pages/EditorPage.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMutex>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>
#include <QtTest>

#include <atomic>

using namespace lumen;
using namespace lumen::ui;

namespace {

const QColor kRed(220, 30, 30);
const QColor kBlue(30, 30, 220);

// 64 × 48: the left half red, the right half blue.
QImage twoColours()
{
    QImage im(64, 48, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&im);
    p.fillRect(QRect(0, 0, 32, 48), kRed);
    p.fillRect(QRect(32, 0, 32, 48), kBlue);
    return im;
}

// The left 32 × 24 corner.
QImage cornerSelection(const QSize &size)
{
    QImage m = img::createMask(size, 0);
    QPainter p(&m);
    p.fillRect(QRect(0, 0, 32, 24), Qt::white);
    return m;
}

std::atomic<int> g_quickRuns{0};

// An AI step stand-in that waits until the test opens the gate, then inverts its input (or, as a
// cut-out, makes its right half transparent): what the document does meanwhile (a brush stroke,
// another filter chosen) is in the test's hands.
class Gate
{
public:
    QFuture<filters::Values> wait(const QImage &input, bool cutOut)
    {
        QMutexLocker lock(&m_mutex);
        m_promise = std::make_shared<QPromise<filters::Values>>();
        m_promise->start();
        m_input = input;
        m_cutOut = cutOut;
        return m_promise->future();
    }
    bool isWaiting() const
    {
        QMutexLocker lock(&m_mutex);
        return m_promise != nullptr;
    }
    void open()
    {
        QMutexLocker lock(&m_mutex);
        if (!m_promise)
            return;
        QImage result = filters::ops::invert(m_input);
        if (m_cutOut) {
            result = m_input.copy();
            QPainter p(&result);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(QRect(result.width() / 2, 0, result.width() - result.width() / 2, result.height()), Qt::transparent);
        }
        m_promise->addResult(filters::Values{{QStringLiteral("image"), result}});
        m_promise->finish();
        m_promise.reset();
    }
    void cancel()
    {
        QMutexLocker lock(&m_mutex);
        if (!m_promise)
            return;
        m_promise->future().cancel();
        m_promise->finish();
        m_promise.reset();
    }

private:
    mutable QMutex m_mutex;
    std::shared_ptr<QPromise<filters::Values>> m_promise;
    QImage m_input;
    bool m_cutOut = false;
};

Gate &gate()
{
    static Gate g;
    return g;
}

// An AI step stand-in that never finishes by itself: its future ends only when cancelled.
void registerTestNodes()
{
    filters::NodeRegistry &r = filters::NodeRegistry::global();
    if (r.contains(QStringLiteral("test-slow-ai")))
        return;
    filters::NodeTypeInfo slow;
    slow.id = QStringLiteral("test-slow-ai");
    slow.name = QStringLiteral("Slow AI step");
    slow.category = filters::categories::Ai;
    slow.inputs = {{QStringLiteral("image"), QStringLiteral("Image"), filters::PortType::Image}};
    slow.outputs = {{QStringLiteral("image"), QStringLiteral("Image"), filters::PortType::Image}};
    slow.expensive = true;
    r.add(filters::makeNodeType(slow, [](const filters::NodeArgs &, filters::EvalContext &ctx) -> filters::NodeResult {
        auto promise = std::make_shared<QPromise<filters::Values>>();
        promise->start();
        ctx.onCancel([promise] {
            promise->future().cancel();
            promise->finish();
        });
        return filters::NodeResult(promise->future());
    }));

    // An AI step stand-in that inverts on a worker and counts its runs.
    filters::NodeTypeInfo quick = slow;
    quick.id = QStringLiteral("test-quick-ai");
    quick.name = QStringLiteral("Quick AI step");
    r.add(filters::makeNodeType(quick, [](const filters::NodeArgs &a, filters::EvalContext &) -> filters::NodeResult {
        ++g_quickRuns;
        const QImage src = a.image(QStringLiteral("image"));
        return filters::NodeResult(async::run([src] { return filters::Values{{QStringLiteral("image"), filters::ops::invert(src)}}; }));
    }));

    filters::NodeTypeInfo gated = slow;
    gated.id = QStringLiteral("test-gated-ai");
    gated.name = QStringLiteral("Gated AI step");
    r.add(filters::makeNodeType(gated, [](const filters::NodeArgs &a, filters::EvalContext &ctx) -> filters::NodeResult {
        ctx.onCancel([] { gate().cancel(); });
        return filters::NodeResult(gate().wait(a.image(QStringLiteral("image")), false));
    }));
    filters::NodeTypeInfo gatedCut = slow;
    gatedCut.id = QStringLiteral("test-gated-cut");
    gatedCut.name = QStringLiteral("Gated cut-out");
    r.add(filters::makeNodeType(gatedCut, [](const filters::NodeArgs &a, filters::EvalContext &ctx) -> filters::NodeResult {
        ctx.onCancel([] { gate().cancel(); });
        return filters::NodeResult(gate().wait(a.image(QStringLiteral("image")), true));
    }));

    // A node with a colour setting (no classic node has one yet).
    filters::NodeTypeInfo tint;
    tint.id = QStringLiteral("test-tint");
    tint.name = QStringLiteral("Tint");
    tint.category = filters::categories::Color;
    tint.inputs = slow.inputs;
    tint.outputs = slow.outputs;
    filters::ParamSpec colour;
    colour.id = QStringLiteral("colour");
    colour.label = QStringLiteral("Colour");
    colour.type = filters::PortType::Color;
    colour.defaultValue = QColor(255, 128, 0);
    tint.params = {colour};
    r.add(filters::makeNodeType(tint, [](const filters::NodeArgs &a, filters::EvalContext &) -> filters::NodeResult {
        QImage out = a.image(QStringLiteral("image")).copy();
        out.fill(a.color(QStringLiteral("colour")));
        return filters::Values{{QStringLiteral("image"), out}};
    }));
}

// input → <type> → output, as a saved filter.
filters::Graph chain(const QString &type, const QString &name)
{
    filters::Graph g;
    g.name = name;
    const QString in = g.addNode(QStringLiteral("input"), QPointF(0, 0));
    const QString mid = g.addNode(type, QPointF(260, 0));
    const QString out = g.addNode(QStringLiteral("output"), QPointF(520, 0));
    g.connect(in, QStringLiteral("image"), mid, QStringLiteral("image"));
    g.connect(mid, QStringLiteral("image"), out, QStringLiteral("image"));
    return g;
}

QPoint viewPoint(NodeEditorWindow &w, const QPointF &scenePos)
{
    return w.view()->mapFromScene(scenePos);
}

PortItem *portOf(NodeEditorWindow &w, const QString &node, const QString &port, bool output)
{
    NodeItem *item = w.scene()->nodeItem(node);
    return item ? item->port(port, output) : nullptr;
}

void drag(QWidget *viewport, const QPoint &from, const QPoint &to)
{
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, from);
    for (int i = 1; i <= 4; ++i)
        QTest::mouseMove(viewport, from + (to - from) * i / 4);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, to);
}

bool sameOutside(const QImage &a, const QImage &b, const QImage &mask)
{
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (qGray(mask.pixel(x, y)) == 0 && a.pixel(x, y) != b.pixel(x, y))
                return false;
        }
    }
    return true;
}

QString builtIn(const QString &id)
{
    return QStringLiteral(":/filters/builtin/%1.lumenfilter").arg(id);
}

// The built-in filters with AI steps.
const QStringList kAiFilters{QStringLiteral("colorize-with-ai"), QStringLiteral("clean-up-background"),
                             QStringLiteral("remove-background"), QStringLiteral("upscale-2x")};

const QColor kOrange(255, 128, 0);

// A photo with detail everywhere, so a change outside the selection would show.
QImage detailed(const QSize &size)
{
    QImage im(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = qRgb((x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256);
    }
    return im;
}

QImage rectMask(const QSize &size, const QRect &rect)
{
    QImage m = img::createMask(size, 0);
    QPainter p(&m);
    p.fillRect(rect, Qt::white);
    return m;
}

QString pngB64(const QImage &image)
{
    return QString::fromLatin1(img::toBase64(image));
}

// Waits for an evaluation with the event loop running (AI steps finish through it). Gives its
// error in plain words, "cancelled", or empty when it has a result.
QString waitFor(QFuture<filters::EvalResult> future, int ms = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!future.isFinished() && timer.elapsed() < ms)
        QTest::qWait(10);
    if (!future.isFinished())
        return QStringLiteral("timed out");
    try {
        future.waitForFinished();
    } catch (const std::exception &e) {
        return describeException(e);
    }
    return future.resultCount() == 1 ? QString() : QStringLiteral("cancelled");
}

// The servers the AI steps talk to, in one: stable-diffusion.cpp's native API (img_gen answers
// with a job that reports one generating step and then a picture of one colour at the requested
// size; with `hold`, it keeps generating until it is cancelled; the upscaler makes pictures four
// times larger) and Stable Diffusion WebUI's background removal (the right half becomes
// transparent).
class MockImageServer
{
public:
    MockImageServer()
    {
        m_http.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [this](const MockRequest &req) {
            payloads << req.json();
            polls = 0;
            cancelled = false;
            return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
        });
        m_http.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [this](const MockRequest &) {
            ++polls;
            if (cancelled)
                return MockResponse::json({{QStringLiteral("status"), QStringLiteral("cancelled")}});
            if (hold || polls == 1) {
                const QJsonObject preview{{QStringLiteral("step"), 2}, {QStringLiteral("total_steps"), 4}};
                return MockResponse::json({{QStringLiteral("status"), QStringLiteral("generating")}, {QStringLiteral("preview"), preview}});
            }
            const QJsonObject p = payloads.last();
            QImage im(QSize(p.value(QStringLiteral("width")).toInt(), p.value(QStringLiteral("height")).toInt()), QImage::Format_ARGB32);
            im.fill(colour);
            const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), pngB64(im)}}};
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")},
                                       {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
        });
        m_http.route("POST", QStringLiteral("/sdcpp/v1/jobs/job_1/cancel"), [this](const MockRequest &) {
            cancelled = true;
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("cancelled")}});
        });
        m_http.route("POST", QStringLiteral("/sdcpp/v1/upscale"), [this](const MockRequest &req) {
            ++upscales;
            const QImage in = img::fromBase64(req.json().value(QStringLiteral("image")).toString().toLatin1());
            const QImage out = in.scaled(in.size() * 4, Qt::IgnoreAspectRatio, Qt::FastTransformation);
            return MockResponse::json({{QStringLiteral("images"), QJsonArray{QJsonObject{{QStringLiteral("b64_json"), pngB64(out)}}}}});
        });
        m_http.route("POST", QStringLiteral("/rembg"), [this](const MockRequest &req) {
            ++cutouts;
            QImage im = img::fromBase64(req.json().value(QStringLiteral("input_image")).toString().toLatin1())
                            .convertToFormat(QImage::Format_ARGB32);
            QPainter p(&im);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(QRect(im.width() / 2, 0, im.width() - im.width() / 2, im.height()), Qt::transparent);
            p.end();
            return MockResponse::json({{QStringLiteral("image"), pngB64(im)}});
        });
    }

    QString url() const { return m_http.baseUrl().toString(); }
    bool received(const QString &path) const
    {
        for (const MockRequest &r : m_http.requests) {
            if (r.path == path)
                return true;
        }
        return false;
    }

    QList<QJsonObject> payloads; // img_gen requests
    QColor colour = kOrange;
    bool hold = false;
    int polls = 0;
    bool cancelled = false;
    int upscales = 0;
    int cutouts = 0;

private:
    MockHttpServer m_http;
};

// Image engines added to a Studio for one test and removed at its end (their settings are written
// to the test's own data folder).
class TestEngines
{
public:
    explicit TestEngines(Studio &studio)
        : m_studio(studio)
    {
    }
    ~TestEngines()
    {
        for (const QString &id : std::as_const(m_ids))
            m_studio.providers()->removeProvider(id);
        QFile::remove(m_studio.providers()->configPath());
    }

    // A stable-diffusion.cpp server running FLUX.2 klein (instruction edits, from its preset), or,
    // without the preset, a model that cannot edit from a description.
    ai::SdCppProvider *sdcpp(const MockImageServer &server, bool editModel = true, bool upscaler = false)
    {
        ai::ModelDefaults klein;
        klein.id = QStringLiteral("flux2-klein-4b");
        klein.cfg = 1.0;
        klein.steps = 4;
        klein.sampler = QStringLiteral("euler");
        klein.nativeResolution = 1024;
        klein.edit = true;
        klein.references = 4;
        klein.negativePrompt = false;
        QVariantMap settings{{QStringLiteral("url"), server.url()},
                             {QStringLiteral("model"), QStringLiteral("flux-2-klein-4b.safetensors")},
                             {QStringLiteral("upscaler"), upscaler}};
        if (editModel)
            settings.insert(QStringLiteral("model_preset"), klein.toVariant());
        auto *p = qobject_cast<ai::SdCppProvider *>(
            m_studio.providers()->addProvider(QStringLiteral("sdcpp"), editModel ? QStringLiteral("Klein") : QStringLiteral("SDXL"), settings));
        if (p) {
            p->pollIntervalMs = 20;
            m_ids << p->instanceId();
        }
        return p;
    }
    // Stable Diffusion WebUI: removes backgrounds.
    ai::Provider *webUi(const MockImageServer &server)
    {
        ai::Provider *p = m_studio.providers()->addProvider(QStringLiteral("sdwebui"), QStringLiteral("WebUI"),
                                                            {{QStringLiteral("url"), server.url()}});
        if (p)
            m_ids << p->instanceId();
        return p;
    }

private:
    Studio &m_studio;
    QStringList m_ids;
};

QPushButton *buttonNamed(QWidget *in, const QString &text)
{
    for (QPushButton *b : in->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

QComboBox *targetCombo(FiltersPanel &panel)
{
    for (QComboBox *c : panel.findChildren<QComboBox *>()) {
        if (c->accessibleName() == QStringLiteral("Put the result on"))
            return c;
    }
    return nullptr;
}

} // namespace

class TestFiltersUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();
    void filtersFolderDefault();
    void builtInFiltersValidate();
    void catalogSavesAndLists();
    void composeThroughSelection();
    void nodeEditorEditing();
    void nodeEditorDragging();
    void nodeEditorPublishSaveReopen();
    void nodeEditorPreviewAndMenu();
    void nodeEditorAiAndColour();
    void curveEditor();
    void panelListsAndSettings();
    void panelPreviewAndApply();
    void panelApplyOnLayerAndCancel();
    void panelAiFilterRunsOnRequest();
    void panelManagesFilters();
    void editorFiltersTab();
    void aiNodesSayWhatTheyNeed();
    void aiEditFeedsNextNode();
    void aiEditOfSelection();
    void aiStepCancel();
    void aiInpaintGraphOnSelection();
    void builtInAiFiltersEndToEnd();
    // Review fixes (plan 17.8 "Review fixes for the node editor, the Filters panel and the AI nodes").
    void applyKeepsChangesMadeMeanwhile();
    void panelPreviewFollowsTheDocument();
    void applyNamesTheFilterItApplied();
    void cancelWhileTheResultIsPutTogether();
    void aiStepJobEnds();
    void upscaleWithSelection();
    void filterNamesStayApart();
    void editorRunServesApply();
    void panelActionsInView();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FilterCatalog> m_catalog;
};

void TestFiltersUi::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    registerTestNodes();
    // The AI nodes, as a Studio registers them at start-up, so the catalogs of the tests (made
    // before their Studio) load the built-in AI filters.
    ai::registerFilterNodes();
}

void TestFiltersUi::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_catalog = std::make_unique<FilterCatalog>(m_dir->filePath(QStringLiteral("filters")));
}

void TestFiltersUi::cleanup()
{
    for (NodeEditorWindow *w : NodeEditorWindow::openWindows()) {
        w->setSaveQuestion({});
        delete w;
    }
    m_catalog.reset();
    m_dir.reset();
}

void TestFiltersUi::filtersFolderDefault()
{
    // Next to the models folder: under the relocated data root here (LUMEN_DATA_DIR or test mode).
    const QString filters = paths::defaultFilters();
    QVERIFY(filters.endsWith(QStringLiteral("/filters")));
    QCOMPARE(QFileInfo(filters).absolutePath(), QFileInfo(paths::defaultModels()).absolutePath());
    Studio studio;
    FilterCatalog *catalog = FilterCatalog::of(&studio);
    QCOMPARE(catalog->folder(), QDir::cleanPath(filters));
    QCOMPARE(FilterCatalog::of(&studio), catalog); // one per Studio
}

void TestFiltersUi::builtInFiltersValidate()
{
    const QStringList files = QDir(QStringLiteral(":/filters/builtin")).entryList({QStringLiteral("*.lumenfilter")});
    QVERIFY2(files.size() >= 11, qPrintable(files.join(QLatin1Char(' '))));
    for (const QString &file : files) {
        const QString path = QStringLiteral(":/filters/builtin/") + file;
        const bool ai = kAiFilters.contains(QFileInfo(file).completeBaseName());
        QString error;
        const std::optional<filters::Graph> g = filters::Graph::load(path, filters::NodeRegistry::global(), &error);
        QVERIFY2(g, qPrintable(file + QStringLiteral(": ") + error));
        QVERIFY2(!g->name.isEmpty() && !g->description.isEmpty(), qPrintable(file));
        for (const filters::Issue &issue : g->validate())
            QVERIFY2(false, qPrintable(file + QStringLiteral(": ") + issue.message)); // no errors, no warnings
        QCOMPARE(g->needsExplicitRun(), ai);
        filters::Evaluator evaluator;
        if (ai) {
            // An AI step runs only when asked (the tests below run them against mock servers).
            filters::EvalOptions preview;
            preview.runExpensive = false;
            auto f = evaluator.evaluate(*g, {twoColours(), {}}, preview);
            f.waitForFinished();
            QVERIFY2(f.resultCount() == 1 && f.result().needsRun && f.result().evaluated.isEmpty(), qPrintable(file));
            continue;
        }
        QVERIFY2(!g->published().isEmpty(), qPrintable(file + QStringLiteral(" publishes no setting")));
        // Applied to a picture, every one changes it.
        auto f = evaluator.evaluate(*g, {twoColours(), {}});
        f.waitForFinished();
        QVERIFY2(f.resultCount() == 1, qPrintable(file));
        QCOMPARE(f.result().image.size(), QSize(64, 48));
        QVERIFY2(f.result().image != img::normalized(twoColours()), qPrintable(file));
    }
    // The catalog lists them first, in their order (those with AI steps last), with thumbnails.
    const QList<FilterCatalog::Entry> entries = m_catalog->entries();
    QVERIFY(entries.size() >= 11);
    QCOMPARE(entries.first().name, QStringLiteral("Black and white"));
    QCOMPARE(entries.first().path, builtIn(QStringLiteral("black-and-white")));
    QCOMPARE(entries.at(7).name, QStringLiteral("Colorize with AI"));
    bool seenAi = false;
    for (const FilterCatalog::Entry &e : entries) {
        QVERIFY(e.builtIn);
        QVERIFY2(e.error.isEmpty(), qPrintable(e.error));
        QCOMPARE(e.usesAi, kAiFilters.contains(QFileInfo(e.path).completeBaseName()));
        QVERIFY(e.usesAi || !seenAi); // the AI ones after the classic ones
        seenAi = seenAi || e.usesAi;
    }
    QSignalSpy ready(m_catalog.get(), &FilterCatalog::thumbnailReady);
    QVERIFY(m_catalog->thumbnail(entries.first().path).isNull()); // computed in the background
    QTRY_VERIFY(!m_catalog->thumbnail(entries.first().path).isNull());
    QCOMPARE(m_catalog->thumbnail(entries.first().path).size(), FilterCatalog::thumbnailSize());
    QVERIFY(ready.count() >= 1);
    // Black and white: the thumbnail is grey.
    const QImage thumb = m_catalog->thumbnail(entries.first().path);
    const QRgb px = thumb.pixel(thumb.width() / 2, thumb.height() / 4);
    QVERIFY(std::abs(qRed(px) - qBlue(px)) <= 2 && std::abs(qRed(px) - qGreen(px)) <= 2);
}

void TestFiltersUi::catalogSavesAndLists()
{
    QSignalSpy changed(m_catalog.get(), &FilterCatalog::changed);
    const int builtIns = int(m_catalog->entries().size());
    QString error;
    const QString path = m_catalog->create(QStringLiteral("My: filter?"), &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    QVERIFY(path.startsWith(m_catalog->folder()));
    QVERIFY(path.endsWith(QStringLiteral(".lumenfilter")));
    QVERIFY(QFile::exists(path));
    QVERIFY(changed.count() >= 1);
    QList<FilterCatalog::Entry> entries = m_catalog->entries();
    QCOMPARE(entries.size(), builtIns + 1);
    QCOMPARE(entries.last().name, QStringLiteral("My: filter?"));
    QVERIFY(!entries.last().builtIn);
    const std::optional<filters::Graph> created = m_catalog->load(path);
    QVERIFY(created && created->isValid());

    // A built-in filter cannot be deleted, but copied.
    QVERIFY(!m_catalog->remove(builtIn(QStringLiteral("sharpen")), &error));
    QVERIFY(!error.isEmpty());
    const QString copy = m_catalog->duplicate(builtIn(QStringLiteral("sharpen")), &error);
    QVERIFY2(!copy.isEmpty(), qPrintable(error));
    QCOMPARE(m_catalog->entry(copy)->name, QStringLiteral("Sharpen copy"));
    QCOMPARE(m_catalog->load(copy)->published().size(), 2);
    // Saving a built-in path makes a new file instead of writing into the resources.
    filters::Graph g = *m_catalog->load(builtIn(QStringLiteral("film-grain")));
    const QString saved = m_catalog->save(g, builtIn(QStringLiteral("film-grain")), &error);
    QVERIFY(!saved.isEmpty() && !FilterCatalog::isBuiltIn(saved));
    QCOMPARE(m_catalog->entries().size(), builtIns + 3);

    // A file put into the folder by hand appears; one that does not load is listed with the reason.
    QFile broken(QDir(m_catalog->folder()).filePath(QStringLiteral("broken.lumenfilter")));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write(R"({"format": "lumen-filter", "version": 1, "name": "Broken", "nodes": [{"id": "a", "type": "no-such-node"}]})");
    broken.close();
    QTRY_VERIFY(m_catalog->entry(broken.fileName()).has_value()); // the folder is watched
    QVERIFY(m_catalog->entry(broken.fileName())->error.contains(QStringLiteral("plugin")));

    QVERIFY(m_catalog->remove(copy, &error));
    QVERIFY(!QFile::exists(copy));
    QVERIFY(!m_catalog->entry(copy));
}

void TestFiltersUi::composeThroughSelection()
{
    const QImage layer = img::normalized(twoColours());
    QImage result(layer.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(QColor(0, 200, 0));
    const QImage where = cornerSelection(layer.size());

    const FilterComposite onNew = composeFilterResult(layer, result, where, FilterRunner::Target::NewLayer, false);
    QCOMPARE(onNew.rect, layer.rect());
    QCOMPARE(QColor(onNew.pixels.pixel(5, 5)), QColor(0, 200, 0));
    QCOMPARE(qAlpha(onNew.pixels.pixel(40, 30)), 0); // transparent outside the selection

    const FilterComposite onLayer = composeFilterResult(layer, result, where, FilterRunner::Target::Layer, false);
    QCOMPARE(onLayer.rect, QRect(0, 0, 32, 24)); // only what changes
    QCOMPARE(QColor(onLayer.pixels.pixel(5, 5)), QColor(0, 200, 0));

    // A half-transparent result keeps the opacity of an alpha-locked layer.
    QImage clear(layer.size(), QImage::Format_ARGB32_Premultiplied);
    clear.fill(QColor(0, 0, 0, 0));
    const FilterComposite locked = composeFilterResult(layer, clear, {}, FilterRunner::Target::Layer, true);
    QCOMPARE(qAlpha(locked.pixels.pixel(5, 5)), 255);

    // A result of another size is stretched to the layer; masks multiply.
    const QImage small = result.scaled(16, 12);
    QCOMPARE(composeFilterResult(layer, small, {}, FilterRunner::Target::NewLayer, false).pixels.size(), layer.size());
    QImage half = img::createMask(layer.size(), 128);
    const QImage both = combinedMask(where, half, layer.size());
    QCOMPARE(qGray(both.pixel(5, 5)), 128);
    QCOMPARE(qGray(both.pixel(40, 30)), 0);
    QVERIFY(combinedMask({}, {}, layer.size()).isNull());
}

void TestFiltersUi::nodeEditorEditing()
{
    NodeEditorWindow w(m_catalog.get());
    w.setAttribute(Qt::WA_DeleteOnClose, false);
    GraphModel *model = w.model();
    QUndoStack *undo = model->undoStack();
    QCOMPARE(model->graph().nodes().size(), 2); // a new filter: input → output
    const QString in = model->graph().nodes().at(0).id;
    const QString out = model->graph().outputNode();
    QVERIFY(model->graph().isValid());

    // Add.
    const QString blur = w.addNode(QStringLiteral("gaussian-blur"));
    QVERIFY(!blur.isEmpty());
    QCOMPARE(model->graph().nodes().size(), 3);
    QCOMPARE(w.selectedNode(), blur);
    QVERIFY(w.scene()->nodeItem(blur));
    QCOMPARE(undo->count(), 1);
    undo->undo();
    QCOMPARE(model->graph().nodes().size(), 2);
    QVERIFY(!w.scene()->nodeItem(blur));
    undo->redo();
    QVERIFY(w.scene()->nodeItem(blur));

    // Connect: input → blur → output (the output's old link is replaced).
    QVERIFY(w.connectPorts(in, QStringLiteral("image"), blur, QStringLiteral("image")));
    QVERIFY(w.connectPorts(blur, QStringLiteral("image"), out, QStringLiteral("image")));
    QCOMPARE(model->graph().links().size(), 2);
    QCOMPARE(model->graph().linkInto(out, QStringLiteral("image"))->fromNode, blur);
    QCOMPARE(w.scene()->linkItems().size(), 2);
    QVERIFY(model->graph().isValid());

    // A wrong type is refused, with the reason, and changes nothing.
    const QString invert = w.addNode(QStringLiteral("invert-mask"));
    const int steps = undo->count();
    const QList<filters::Link> links = model->graph().links();
    QVERIFY(!w.connectPorts(blur, QStringLiteral("image"), invert, QStringLiteral("mask")));
    QVERIFY2(w.message().contains(QStringLiteral("Mask from image")), qPrintable(w.message()));
    QCOMPARE(model->graph().links(), links);
    QCOMPARE(undo->count(), steps);
    // A loop is refused.
    const QString sharpen = w.addNode(QStringLiteral("sharpen"));
    QVERIFY(w.connectPorts(blur, QStringLiteral("image"), sharpen, QStringLiteral("image")));
    QVERIFY(!w.connectPorts(sharpen, QStringLiteral("image"), blur, QStringLiteral("image")));
    QVERIFY2(w.message().contains(QStringLiteral("loop")), qPrintable(w.message()));
    QVERIFY(w.deleteSelection()); // the sharpen node, selected when added
    QVERIFY(!model->graph().node(sharpen));
    // The selection fits the mask input.
    QVERIFY(w.connectPorts(in, QStringLiteral("selection"), invert, QStringLiteral("mask")));

    // An unconnected node is listed as a problem in plain words.
    QVERIFY2(w.problems().join(QLatin1Char('\n')).contains(QStringLiteral("not connected to the output")),
             qPrintable(w.problems().join(QLatin1Char('\n'))));

    // Delete, as one step with its links; undo brings both back.
    w.selectNode(invert);
    const int before = int(model->graph().links().size());
    QVERIFY(w.deleteSelection());
    QVERIFY(!model->graph().node(invert));
    QCOMPARE(model->graph().links().size(), before - 1);
    undo->undo();
    QVERIFY(model->graph().node(invert));
    QCOMPARE(model->graph().links().size(), before);

    // Settings: shown in short on the node; slider steps merge into one undo step.
    const int stepsBefore = undo->index();
    QVERIFY(w.setParam(blur, QStringLiteral("radius"), 10.0));
    QVERIFY(w.setParam(blur, QStringLiteral("radius"), 12.0));
    QCOMPARE(undo->index(), stepsBefore + 1);
    QCOMPARE(model->graph().param(blur, QStringLiteral("radius")).toDouble(), 12.0);
    QVERIFY2(w.scene()->nodeItem(blur)->summary().join(QLatin1Char(' ')).contains(QStringLiteral("12")),
             qPrintable(w.scene()->nodeItem(blur)->summary().join(QLatin1Char(' '))));
    undo->undo();
    QCOMPARE(model->graph().param(blur, QStringLiteral("radius")).toDouble(), 4.0);
    // The side panel follows undo.
    w.selectNode(blur);
    auto *field = w.findChild<ParamField *>(QStringLiteral("param:radius"));
    QVERIFY(field);
    QCOMPARE(field->value().toDouble(), 4.0);
    undo->redo();
    QCOMPARE(field->value().toDouble(), 12.0);
    // A slider dragged back to where it was leaves no step.
    const int stepsNow = undo->index();
    QVERIFY(w.setParam(blur, QStringLiteral("radius"), 20.0));
    QVERIFY(w.setParam(blur, QStringLiteral("radius"), 12.0));
    QCOMPARE(undo->index(), stepsNow);

    // Another filter in the same window: node ids repeat with other kinds of nodes.
    w.setGraph(*m_catalog->load(builtIn(QStringLiteral("soft-focus"))));
    QCOMPARE(w.scene()->nodeItem(QStringLiteral("n2"))->nodeType()->id(), QStringLiteral("gaussian-blur"));
    QCOMPARE(w.scene()->linkItems().size(), 4);
    QCOMPARE(undo->count(), 0);
    QVERIFY(!w.isModified());
    QVERIFY(w.problems().isEmpty());
}

void TestFiltersUi::nodeEditorDragging()
{
    NodeEditorWindow w(m_catalog.get());
    w.setAttribute(Qt::WA_DeleteOnClose, false);
    w.resize(1100, 700);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    GraphModel *model = w.model();
    const QString in = model->graph().nodes().at(0).id;
    const QString out = model->graph().outputNode();
    const QString grow = w.addNode(QStringLiteral("grow-shrink-feather"), QPointF(180, 220));
    w.frameAll();
    QTest::qWait(20);
    QWidget *viewport = w.view()->viewport();

    // Port to port with the mouse: two inputs refuse.
    PortItem *selection = portOf(w, in, QStringLiteral("selection"), true);
    PortItem *maskIn = portOf(w, grow, QStringLiteral("mask"), false);
    PortItem *where = portOf(w, out, QStringLiteral("mask"), false);
    QVERIFY(selection && maskIn && where);
    drag(viewport, viewPoint(w, maskIn->center()), viewPoint(w, where->center()));
    QVERIFY2(w.message().contains(QStringLiteral("Both are inputs")), qPrintable(w.message()));
    QCOMPARE(model->undoStack()->count(), 1);
    // The selection (a mask) into the mask input connects.
    drag(viewport, viewPoint(w, selection->center()), viewPoint(w, maskIn->center()));
    QVERIFY(model->graph().linkInto(grow, QStringLiteral("mask")).has_value());
    QCOMPARE(model->graph().linkInto(grow, QStringLiteral("mask"))->fromPort, QStringLiteral("selection"));

    // An image into a mask input: refused, the reason in the status bar, nothing changes.
    const int steps = model->undoStack()->count();
    PortItem *image = portOf(w, in, QStringLiteral("image"), true);
    drag(viewport, viewPoint(w, image->center()), viewPoint(w, maskIn->center()));
    QCOMPARE(model->graph().linkInto(grow, QStringLiteral("mask"))->fromPort, QStringLiteral("selection"));
    QCOMPARE(model->undoStack()->count(), steps);
    QVERIFY2(w.message().contains(QStringLiteral("takes a mask")), qPrintable(w.message()));

    // Picking a link up from an input and dropping it on nothing removes it.
    drag(viewport, viewPoint(w, maskIn->center()), viewPoint(w, maskIn->center() + QPointF(-60, 120)));
    QVERIFY(!model->graph().linkInto(grow, QStringLiteral("mask")).has_value());
    QCOMPARE(model->undoStack()->count(), steps + 1);
    model->undoStack()->undo();
    QVERIFY(model->graph().linkInto(grow, QStringLiteral("mask")).has_value());

    // Moving a node by its title is one undo step; links follow.
    NodeItem *item = w.scene()->nodeItem(out);
    const QPointF before = model->graph().node(out)->position;
    const QPoint title = viewPoint(w, item->scenePos() + QPointF(60, 12));
    drag(viewport, title, title + QPoint(40, 30));
    const QPointF after = model->graph().node(out)->position;
    QVERIFY2(after != before, "the node did not move");
    QCOMPARE(model->undoStack()->undoText(), QStringLiteral("Move node"));
    model->undoStack()->undo();
    QCOMPARE(w.scene()->nodeItem(out)->pos(), before);

    // Delete with the key, in the view.
    w.selectNode(grow);
    w.view()->setFocus();
    QTest::keyClick(w.view(), Qt::Key_Delete);
    QVERIFY(!model->graph().node(grow));
    QVERIFY(w.view()->zoom() <= 1.0);
}

void TestFiltersUi::nodeEditorPublishSaveReopen()
{
    auto *w = new NodeEditorWindow(m_catalog.get());
    GraphModel *model = w->model();
    const QString in = model->graph().nodes().at(0).id;
    const QString out = model->graph().outputNode();
    const QString levels = w->addNode(QStringLiteral("levels"));
    QVERIFY(w->connectPorts(in, QStringLiteral("image"), levels, QStringLiteral("image")));
    QVERIFY(w->connectPorts(levels, QStringLiteral("image"), out, QStringLiteral("image")));

    // Publish with a label and a range; the side panel's button shows it.
    QVERIFY(w->publish(levels, QStringLiteral("gamma"), QStringLiteral("Midtones"), 0.5, 2.0));
    QCOMPARE(model->graph().published().size(), 1);
    QCOMPARE(model->graph().published().first().label, QStringLiteral("Midtones"));
    w->selectNode(levels);
    auto *button = w->findChild<QToolButton *>(QStringLiteral("publish:gamma"));
    QVERIFY(button && button->isChecked());
    QVERIFY(w->unpublish(levels, QStringLiteral("gamma")));
    QVERIFY(model->graph().published().isEmpty());
    QVERIFY(!button->isChecked());
    model->undoStack()->undo();
    QCOMPARE(model->graph().published().size(), 1);
    QVERIFY(button->isChecked());
    // A range beyond the setting's own is a problem.
    QVERIFY(w->publish(levels, QStringLiteral("inBlack"), QStringLiteral("Black"), -10, 100));
    QVERIFY(!model->graph().isValid());
    model->undoStack()->undo();
    QVERIFY(model->graph().isValid());

    // Save as: a file in the filters folder, the catalog lists it, nothing left to save.
    QVERIFY(w->isModified());
    QSignalSpy saved(w, &NodeEditorWindow::saved);
    QString error;
    QVERIFY2(w->saveAs(QStringLiteral("Brighter"), &error), qPrintable(error));
    const QString path = w->path();
    QVERIFY(path.startsWith(m_catalog->folder()));
    QVERIFY(QFile::exists(path));
    QCOMPARE(saved.count(), 1);
    QVERIFY(!w->isModified());
    QCOMPARE(m_catalog->entry(path)->name, QStringLiteral("Brighter"));
    QVERIFY(w->windowTitle().contains(QStringLiteral("Brighter")));

    // A change, then Save into the same file.
    QVERIFY(w->setParam(levels, QStringLiteral("gamma"), 1.6));
    QVERIFY(w->isModified());
    QVERIFY(w->save(&error));
    QCOMPARE(w->path(), path);
    QCOMPARE(m_catalog->load(path)->param(levels, QStringLiteral("gamma")).toDouble(), 1.6);

    // Opening the same filter raises the window that has it; once closed, it opens again as saved.
    QCOMPARE(NodeEditorWindow::openFilter(m_catalog.get(), path, nullptr), w);
    const filters::Graph savedGraph = model->graph();
    QVERIFY(w->close()); // nothing to save: no question
    QTRY_VERIFY(NodeEditorWindow::openWindows().isEmpty());
    NodeEditorWindow *again = NodeEditorWindow::openFilter(m_catalog.get(), path, nullptr, &error);
    QVERIFY2(again, qPrintable(error));
    QVERIFY(sameGraph(again->model()->graph(), savedGraph));
    QCOMPARE(again->model()->graph().published().first().minimum, 0.5);
    QVERIFY(!again->isModified());
    QCOMPARE(again->model()->undoStack()->count(), 0);

    // Built-in filters are not opened for editing.
    QVERIFY(!NodeEditorWindow::openFilter(m_catalog.get(), builtIn(QStringLiteral("sharpen")), nullptr, &error));
    QVERIFY(!error.isEmpty());

    // Closing with changes asks; Cancel keeps the window open.
    QVERIFY(again->setParam(levels, QStringLiteral("gamma"), 0.8));
    int asked = 0;
    again->setSaveQuestion([&](const QString &) {
        ++asked;
        return QMessageBox::Cancel;
    });
    QVERIFY(!again->close());
    QCOMPARE(asked, 1);
    again->setSaveQuestion([&](const QString &) { return QMessageBox::Discard; });
    QVERIFY(again->close());
}

void TestFiltersUi::nodeEditorPreviewAndMenu()
{
    NodeEditorWindow w(m_catalog.get());
    w.setAttribute(Qt::WA_DeleteOnClose, false);
    w.setSource(img::normalized(twoColours()));
    GraphModel *model = w.model();
    const QString in = model->graph().nodes().at(0).id;
    const QString out = model->graph().outputNode();

    // The menu finds nodes by their plain names and descriptions, grouped by category.
    NodeMenu *menu = w.nodeMenu();
    menu->setFilter(QStringLiteral("blur"));
    QVERIFY(menu->visibleTypes().contains(QStringLiteral("gaussian-blur")));
    QVERIFY(!menu->visibleTypes().contains(QStringLiteral("invert")));
    menu->setFilter(QStringLiteral("negative")); // Invert: "Turns the image into its negative."
    QCOMPARE(menu->visibleTypes(), QStringList{QStringLiteral("invert")});
    menu->setFilter({});
    QVERIFY(menu->visibleTypes().size() >= 25);
    // Enter in the search field adds the first match.
    menu->setFilter(QStringLiteral("invert"));
    QSignalSpy picked(menu, &NodeMenu::picked);
    QTest::keyClick(menu->searchField(), Qt::Key_Return);
    QCOMPARE(picked.count(), 1);
    const QString invert = w.selectedNode();
    QCOMPARE(model->graph().node(invert)->typeId(), QStringLiteral("invert"));

    // Unconnected, the preview says what is missing.
    QTRY_VERIFY(!w.isPreviewPending());
    QVERIFY2(w.previewText().contains(QStringLiteral("nothing connected")), qPrintable(w.previewText()));
    // Connected, it shows the node's result on the picture.
    QVERIFY(w.connectPorts(in, QStringLiteral("image"), invert, QStringLiteral("image")));
    QTRY_VERIFY(!w.isPreviewPending());
    QVERIFY(!w.preview().isNull());
    QCOMPARE(QColor(w.preview().pixel(4, 4)), QColor(255 - kRed.red(), 255 - kRed.green(), 255 - kRed.blue()));
    // With nothing selected, the result of the filter (still the input: Invert is not connected to it).
    w.selectNode({});
    QTRY_VERIFY(!w.isPreviewPending());
    QCOMPARE(QColor(w.preview().pixel(4, 4)), kRed);
    QVERIFY(w.connectPorts(invert, QStringLiteral("image"), out, QStringLiteral("image")));
    QTRY_VERIFY(!w.isPreviewPending());
    QCOMPARE(QColor(w.preview().pixel(4, 4)), QColor(255 - kRed.red(), 255 - kRed.green(), 255 - kRed.blue()));

    // A filter without an output says so.
    w.selectNode(out);
    QVERIFY(w.deleteSelection());
    QVERIFY(w.problems().join(QLatin1Char('\n')).contains(QStringLiteral("no output")));
}

void TestFiltersUi::nodeEditorAiAndColour()
{
    QWidget owner;
    auto *w = new NodeEditorWindow(m_catalog.get(), &owner);
    w->setSource(img::normalized(twoColours()));
    GraphModel *model = w->model();
    QAction *run = nullptr;
    for (QAction *a : w->findChildren<QAction *>()) {
        if (a->text() == QStringLiteral("Run AI steps"))
            run = a;
    }
    QVERIFY(run);
    QVERIFY(!run->isVisible()); // no AI steps yet

    // An AI step previews only when asked; then the preview shows its result.
    const QString path = m_catalog->save(chain(QStringLiteral("test-quick-ai"), QStringLiteral("Quick")));
    QVERIFY(w->open(path));
    QVERIFY(run->isVisible());
    g_quickRuns = 0;
    QTRY_VERIFY(!w->isPreviewPending());
    QVERIFY2(w->previewText().contains(QStringLiteral("AI steps")), qPrintable(w->previewText()));
    QCOMPARE(g_quickRuns.load(), 0);
    run->trigger();
    QTRY_VERIFY(!w->isPreviewPending());
    QCOMPARE(g_quickRuns.load(), 1);
    QCOMPARE(QColor(w->preview().pixel(4, 4)), QColor(255 - kRed.red(), 255 - kRed.green(), 255 - kRed.blue()));

    // A long one can be stopped; the button says so while it runs.
    QVERIFY(w->open(m_catalog->save(chain(QStringLiteral("test-slow-ai"), QStringLiteral("Slow")))));
    QTRY_VERIFY(!w->isPreviewPending());
    run->trigger();
    QTRY_COMPARE(run->text(), QStringLiteral("Stop AI steps"));
    QVERIFY(w->isPreviewPending());
    run->trigger();
    QTRY_VERIFY(!w->isPreviewPending());
    QCOMPARE(run->text(), QStringLiteral("Run AI steps"));
    QVERIFY(w->previewText().contains(QStringLiteral("AI steps")) || w->previewText().contains(QStringLiteral("Stopped")));

    // A colour setting: a swatch, kept through Save and open.
    const QString tint = w->addNode(QStringLiteral("test-tint"));
    auto *swatch = w->findChild<ParamField *>(QStringLiteral("param:colour"))->findChild<ColorSwatch *>();
    QVERIFY(swatch);
    QCOMPARE(swatch->color(), QColor(255, 128, 0));
    swatch->setColor(QColor(10, 20, 30));
    QCOMPARE(model->graph().param(tint, QStringLiteral("colour")).value<QColor>(), QColor(10, 20, 30));
    QString error;
    QVERIFY2(w->saveAs(QStringLiteral("Tinted"), &error), qPrintable(error));
    QCOMPARE(m_catalog->load(w->path())->param(tint, QStringLiteral("colour")).value<QColor>(), QColor(10, 20, 30));

    // The window it belongs to asks about unsaved changes when it closes.
    QVERIFY(w->setParam(tint, QStringLiteral("colour"), QColor(Qt::white)));
    int asked = 0;
    w->setSaveQuestion([&](const QString &) {
        ++asked;
        return QMessageBox::Cancel;
    });
    QVERIFY(!owner.close());
    QCOMPARE(asked, 1);
    w->setSaveQuestion([&](const QString &) {
        ++asked;
        return QMessageBox::Discard;
    });
    QVERIFY(owner.close());
    QCOMPARE(asked, 2);
}

void TestFiltersUi::curveEditor()
{
    filters::ParamSpec spec = filters::NodeRegistry::global().find(QStringLiteral("curves"))->info().params.first();
    QCOMPARE(spec.editor, QStringLiteral("curve"));
    ParamField field(spec, QStringLiteral("0 0, 255 255"));
    field.resize(240, 320);
    field.show();
    QVERIFY(QTest::qWaitForWindowExposed(&field));
    auto *curve = field.findChild<CurveEditor *>();
    QVERIFY(curve);
    QCOMPARE(curve->points().size(), 2);
    QSignalSpy changed(&field, &ParamField::valueChanged);
    // A click in the middle, above the line, adds a point that lifts the midtones.
    const QPoint middle(curve->width() / 2, curve->height() / 3);
    QTest::mouseClick(curve, Qt::LeftButton, Qt::NoModifier, middle);
    QCOMPARE(curve->points().size(), 3);
    QVERIFY(changed.count() >= 1);
    const QList<QPointF> points = filters::ops::parseCurve(field.value().toString());
    QCOMPARE(points.size(), 3);
    QVERIFY(points[1].y() > points[1].x());
    // The text field follows; typing points moves the curve.
    auto *text = field.findChild<QLineEdit *>();
    QCOMPARE(text->text(), field.value().toString());
    text->clear();
    QTest::keyClicks(text, QStringLiteral("0 0, 64 128, 192 200, 255 255"));
    QCOMPARE(curve->points().size(), 4);
    // Right-click on a point removes it (the plot is the square in the middle, 6 px in).
    const double side = std::min(curve->width(), curve->height()) - 12.0;
    const QPointF at((curve->width() - side) / 2.0 + 64.0 / 255.0 * side, 6.0 + side - 128.0 / 255.0 * side);
    QTest::mouseClick(curve, Qt::RightButton, Qt::NoModifier, at.toPoint());
    QCOMPARE(curve->points().size(), 3);
    QCOMPARE(curve->points().at(1), QPointF(192, 200));
}

void TestFiltersUi::panelListsAndSettings()
{
    Studio studio;
    studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());

    // Built-in filters first, under a heading; "Your filters" after them.
    QListWidget *list = panel.list();
    QCOMPARE(list->item(0)->text(), QStringLiteral("Built in"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("Black and white"));
    QStringList texts;
    for (int i = 0; i < list->count(); ++i)
        texts << list->item(i)->text();
    QVERIFY(texts.contains(QStringLiteral("Your filters")));
    QVERIFY(texts.contains(QStringLiteral("Soft focus")));
    // Those with AI steps under a heading of their own, after the others.
    QVERIFY(texts.indexOf(QStringLiteral("Built in, with AI")) > texts.indexOf(QStringLiteral("Old photo")));
    QCOMPARE(texts.indexOf(QStringLiteral("Colorize with AI")), texts.indexOf(QStringLiteral("Built in, with AI")) + 1);

    // A saved filter appears when it is saved.
    filters::Graph g = chain(QStringLiteral("levels"), QStringLiteral("Lift"));
    g.publish({QStringLiteral("n2"), QStringLiteral("gamma"), QStringLiteral("Midtones"), 0.5, 2.0});
    const QString path = m_catalog->save(g);
    QVERIFY(!path.isEmpty());
    QVERIFY(panel.selectFilter(path));
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Lift"));

    // Its published setting as a slider with its label and range.
    QCOMPARE(panel.fields().size(), 1);
    ParamField *midtones = panel.field(QStringLiteral("Midtones"));
    QVERIFY(midtones);
    auto *spin = midtones->findChild<QDoubleSpinBox *>();
    QVERIFY(spin);
    QCOMPARE(spin->minimum(), 0.5);
    QCOMPARE(spin->maximum(), 2.0);
    QCOMPARE(spin->value(), 1.0);

    // A built-in filter with a slider and a list.
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("film-grain"))));
    QCOMPARE(panel.fields().size(), 2);
    QVERIFY(panel.field(QStringLiteral("Amount")));
    auto *grain = panel.field(QStringLiteral("Grain"))->findChild<QComboBox *>();
    QVERIFY(grain);
    QCOMPARE(grain->currentData().toString(), QStringLiteral("mono"));
    // Moving a setting changes the filter that is applied, not the saved one.
    grain->setCurrentIndex(grain->findData(QStringLiteral("color")));
    QCOMPARE(panel.runner()->graph()->param(QStringLiteral("n2"), QStringLiteral("type")).toString(), QStringLiteral("color"));
    QCOMPARE(m_catalog->load(builtIn(QStringLiteral("film-grain")))->param(QStringLiteral("n2"), QStringLiteral("type")).toString(),
             QStringLiteral("mono"));
    // Built-in filters: edited as a copy, never deleted.
    bool deleteEnabled = true;
    for (QPushButton *b : panel.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Delete"))
            deleteEnabled = b->isEnabled();
        if (b->text().startsWith(QStringLiteral("Edit")))
            QCOMPARE(b->text(), QStringLiteral("Edit a copy…"));
    }
    QVERIFY(!deleteEnabled);
}

void TestFiltersUi::panelPreviewAndApply()
{
    Studio studio;
    doc::Document *doc = studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.resize(320, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));

    // A live preview on the canvas, through the selection.
    doc->applySelection(cornerSelection(doc->size()), QStringLiteral("Select"));
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("black-and-white"))));
    QTRY_VERIFY(!canvas.preview().isNull());
    QCOMPARE(canvas.preview().size(), doc->size()); // a small picture previews at its own size
    const QColor grey = QColor(canvas.preview().pixel(4, 4));
    QVERIFY(grey.red() == grey.green() && grey.green() == grey.blue());
    QCOMPARE(QColor(canvas.preview().pixel(4, 40)), kRed); // outside the selection
    QCOMPARE(doc->layerCount(), 1);                         // a preview changes nothing

    // Moving a slider previews again.
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("warm-tones"))));
    QTRY_VERIFY(!canvas.preview().isNull() && QColor(canvas.preview().pixel(4, 4)) != kRed);
    const QImage first = canvas.preview();
    auto *strength = panel.field(QStringLiteral("Strength"))->findChild<QDoubleSpinBox *>();
    strength->setValue(100);
    QTRY_VERIFY(canvas.preview() != first && !canvas.preview().isNull());
    // Hidden (another tab), the panel leaves the canvas alone.
    panel.hide();
    QVERIFY(canvas.preview().isNull());
    panel.show();
    QTRY_VERIFY(!canvas.preview().isNull());

    // Apply: a new layer above, one undo step, nothing outside the selection changes.
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("black-and-white"))));
    const QImage before = doc->flattened();
    const QUuid original = doc->activeLayerId();
    const QImage originalPixels = doc->activeLayer()->image;
    const int steps = doc->undoStack()->count();
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(5000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->undoStack()->count(), steps + 1);
    QCOMPARE(doc->layerAt(1)->props.name, QStringLiteral("Black and white"));
    QCOMPARE(doc->activeLayerId(), doc->layerAt(1)->id);
    QCOMPARE(doc->findLayer(original)->image, originalPixels); // the original stays underneath
    const QImage after = doc->flattened();
    QVERIFY(sameOutside(before, after, doc->selection()));
    const QColor applied(after.pixel(4, 4));
    QVERIFY(applied != kRed && applied.red() == applied.green() && applied.green() == applied.blue());
    QVERIFY(panel.statusText().contains(QStringLiteral("new layer")));
    // After applying, the canvas shows the document again (a preview now would filter twice).
    QVERIFY(canvas.preview().isNull());
    // One undo takes it all back.
    doc->undoStack()->undo();
    QCOMPARE(doc->layerCount(), 1);
    QCOMPARE(doc->flattened(), before);
}

void TestFiltersUi::panelApplyOnLayerAndCancel()
{
    Studio studio;
    doc::Document *doc = studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    doc->applySelection(cornerSelection(doc->size()), QStringLiteral("Select"));

    // On the layer itself: the same layer changes inside the selection only, as one undo step.
    QComboBox *target = nullptr;
    for (QComboBox *c : panel.findChildren<QComboBox *>()) {
        if (c->accessibleName() == QStringLiteral("Put the result on"))
            target = c;
    }
    QVERIFY(target);
    target->setCurrentIndex(1);
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("black-and-white"))));
    const QImage before = doc->activeLayer()->image;
    const int steps = doc->undoStack()->count();
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(5000));
    QVERIFY(finished.first().at(0).toBool());
    QCOMPARE(doc->layerCount(), 1);
    QCOMPARE(doc->undoStack()->count(), steps + 1);
    const QImage after = doc->activeLayer()->image;
    QVERIFY(sameOutside(before, after, doc->selection()));
    QVERIFY(QColor(after.pixel(4, 4)) != kRed);
    doc->undoStack()->undo();
    QCOMPARE(doc->activeLayer()->image, before);
    // A locked layer is not changed; a new layer still can be made.
    doc::LayerProps props = doc->activeLayer()->props;
    props.locked = true;
    doc->setLayerProps(doc->activeLayerId(), props);
    QVERIFY(!panel.applySelected());
    QVERIFY(panel.statusText().contains(QStringLiteral("locked")));
    props.locked = false;
    doc->setLayerProps(doc->activeLayerId(), props);
    target->setCurrentIndex(0);

    // Cancel: an AI step that waits for its job; the panel shows progress; nothing changes.
    const QString slow = m_catalog->save(chain(QStringLiteral("test-slow-ai"), QStringLiteral("Slow")));
    QVERIFY(panel.selectFilter(slow));
    QVERIFY(panel.runner()->needsRun());
    const int layers = doc->layerCount();
    const int stepsBefore = doc->undoStack()->count();
    QSignalSpy progress(panel.runner(), &FilterRunner::progress);
    finished.clear();
    QVERIFY(panel.applySelected());
    QVERIFY(panel.runner()->isBusy());
    QTRY_VERIFY(progress.count() >= 1);
    QCOMPARE(progress.last().at(2).toString(), QStringLiteral("Slow AI step"));
    QVERIFY(!panel.applySelected()); // one at a time
    panel.cancel();
    QTRY_VERIFY(finished.count() == 1);
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("Cancelled")));
    QVERIFY(!panel.runner()->isBusy());
    QCOMPARE(doc->layerCount(), layers);
    QCOMPARE(doc->undoStack()->count(), stepsBefore);
}

void TestFiltersUi::panelAiFilterRunsOnRequest()
{
    Studio studio;
    doc::Document *doc = studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));

    // A filter with an AI step previews only after Run.
    const QString path = m_catalog->save(chain(QStringLiteral("test-quick-ai"), QStringLiteral("Quick")));
    QVERIFY(m_catalog->entry(path)->usesAi);
    g_quickRuns = 0;
    QVERIFY(panel.selectFilter(path));
    QVERIFY(panel.runner()->needsRun());
    QTest::qWait(150);
    QVERIFY(canvas.preview().isNull());
    QCOMPARE(g_quickRuns.load(), 0);
    QPushButton *run = nullptr;
    for (QPushButton *b : panel.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Run AI steps"))
            run = b;
    }
    QVERIFY(run && run->isVisible());
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    run->click();
    QVERIFY(finished.wait(5000));
    QVERIFY(finished.first().at(0).toBool());
    QTRY_VERIFY(!canvas.preview().isNull());
    QCOMPARE(g_quickRuns.load(), 1);
    QVERIFY(!panel.runner()->needsRun());
    // Applying uses what Run computed.
    finished.clear();
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(5000));
    QVERIFY(finished.first().at(0).toBool());
    QCOMPARE(g_quickRuns.load(), 1);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(QColor(doc->flattened().pixel(4, 4)), QColor(255 - kRed.red(), 255 - kRed.green(), 255 - kRed.blue()));
}

void TestFiltersUi::panelManagesFilters()
{
    Studio studio;
    studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());

    // New filter: a file and an editor window for it.
    const QString created = panel.newFilter(QStringLiteral("Mine"));
    QVERIFY(!created.isEmpty());
    QCOMPARE(panel.currentPath(), created);
    QCOMPARE(NodeEditorWindow::openWindows().size(), 1);
    NodeEditorWindow *editor = NodeEditorWindow::openWindows().first();
    QCOMPARE(editor->path(), created);

    // Saving in the editor refreshes the panel, published settings included.
    const QString in = editor->model()->graph().nodes().at(0).id;
    const QString out = editor->model()->graph().outputNode();
    const QString hue = editor->addNode(QStringLiteral("hue-saturation"));
    QVERIFY(editor->connectPorts(in, QStringLiteral("image"), hue, QStringLiteral("image")));
    QVERIFY(editor->connectPorts(hue, QStringLiteral("image"), out, QStringLiteral("image")));
    QVERIFY(editor->publish(hue, QStringLiteral("hue"), QStringLiteral("Colour turn")));
    QVERIFY(panel.fields().isEmpty());
    QVERIFY(editor->save());
    QTRY_COMPARE(panel.fields().size(), 1);
    QVERIFY(panel.field(QStringLiteral("Colour turn")));

    // Edit on a built-in filter edits a copy.
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("old-photo"))));
    NodeEditorWindow *copyEditor = panel.editSelected();
    QVERIFY(copyEditor);
    QVERIFY(!FilterCatalog::isBuiltIn(copyEditor->path()));
    QCOMPARE(panel.currentPath(), copyEditor->path());
    QCOMPARE(copyEditor->model()->graph().name, QStringLiteral("Old photo copy"));

    // Duplicate, then Delete (asked first).
    QVERIFY(panel.selectFilter(created));
    const QString copy = panel.duplicateSelected();
    QVERIFY(!copy.isEmpty());
    QCOMPARE(panel.currentPath(), copy);
    QString asked;
    panel.setConfirm([&](const QString &question) {
        asked = question;
        return false;
    });
    QVERIFY(!panel.deleteSelected());
    QVERIFY(QFile::exists(copy));
    QVERIFY(asked.contains(QStringLiteral("Mine copy")));
    panel.setConfirm([](const QString &) { return true; });
    QVERIFY(panel.deleteSelected());
    QVERIFY(!QFile::exists(copy));
    QVERIFY(!m_catalog->entry(copy));
    QVERIFY(panel.currentPath() != copy);
}

// The tab in the editor's right panel: it previews on the editor's canvas while it is shown.
void TestFiltersUi::editorFiltersTab()
{
    Studio studio;
    QImage picture = filterSampleImage(QSize(800, 600));
    doc::Document *doc = studio.openImageAsDocument(picture, QStringLiteral("Sample"));
    doc->applySelection(cornerSelection(doc->size()).scaled(doc->size()), QStringLiteral("Select"));
    EditorPage page(&studio);
    page.resize(1280, 700);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto *panel = page.findChild<FiltersPanel *>();
    QVERIFY(panel);
    QTabWidget *side = nullptr;
    for (QTabWidget *t : page.findChildren<QTabWidget *>()) {
        if (t->indexOf(panel) >= 0)
            side = t;
    }
    QVERIFY(side);
    QCOMPARE(side->tabText(side->indexOf(panel)), QStringLiteral("Filters"));
    QVERIFY(panel->selectFilter(builtIn(QStringLiteral("old-photo"))));
    QVERIFY(page.canvas()->preview().isNull()); // another tab is shown
    side->setCurrentWidget(panel);
    QTRY_VERIFY(!page.canvas()->preview().isNull());
    // Reduced resolution: at most the preview size, and not more than the picture.
    QVERIFY(page.canvas()->preview().width() <= doc->size().width());
    // It stays while nothing changes (the page itself does not change the document).
    QTest::qWait(150);
    QVERIFY(!page.canvas()->preview().isNull());
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        QTest::qWait(50);
        page.grab().save(QDir(shots).filePath(QStringLiteral("editor-filters-tab.png")));
    }
    side->setCurrentIndex(0);
    QVERIFY(page.canvas()->preview().isNull());
}

// ---- AI nodes (plan 17.8 "AI nodes") -------------------------------------------------------------

// The AI steps: in the registry, expensive. Each says in plain words what it needs before anything
// runs (the Filters panel and the node editor show it), and fails with that when it runs anyway.
void TestFiltersUi::aiNodesSayWhatTheyNeed()
{
    namespace fn = ai::filter_nodes;
    filters::NodeRegistry &registry = filters::NodeRegistry::global();
    for (const QString &id : {fn::Edit, fn::Inpaint, fn::Upscale, fn::RemoveBackground}) {
        const filters::NodeTypePtr type = registry.find(id);
        QVERIFY2(type, qPrintable(id));
        QVERIFY(type->info().expensive);
        QCOMPARE(type->info().category, filters::categories::Ai);
        QVERIFY(type->info().param(fn::EngineParam));
    }
    QCOMPARE(registry.find(fn::RemoveBackground)->info().outputs.size(), 2); // the cut-out and its mask
    const filters::NodeTypePtr edit = registry.find(fn::Edit);

    Studio studio;
    QCOMPARE(ai::filterNodeService(), studio.generation()); // the newest Studio's engines
    TestEngines engines(studio);
    // No image engine at all.
    QVERIFY2(edit->unavailableReason({}).contains(QStringLiteral("no image engine")), qPrintable(edit->unavailableReason({})));
    const std::optional<filters::Graph> colorize = m_catalog->load(builtIn(QStringLiteral("colorize-with-ai")));
    QVERIFY(colorize);
    QCOMPARE(colorize->unavailable().size(), 1);
    filters::Evaluator evaluator;
    QString error = waitFor(evaluator.evaluate(*colorize, {twoColours(), {}}));
    QVERIFY2(error.startsWith(QStringLiteral("“Edit with AI”: There is no image engine")), qPrintable(error));

    // The Filters panel says so before anything runs; Run gives the same reason.
    doc::Document *doc = studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("colorize-with-ai"))));
    auto *availability = panel.findChild<QLabel *>(QStringLiteral("FilterAvailability"));
    QVERIFY(availability);
    QVERIFY(!availability->isHidden());
    QVERIFY2(availability->text().contains(QStringLiteral("no image engine")), qPrintable(availability->text()));
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QPushButton *run = buttonNamed(&panel, QStringLiteral("Run AI steps"));
    QVERIFY(run && !run->isHidden());
    run->click();
    QVERIFY(finished.wait(5000));
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("no image engine")));
    QVERIFY(panel.statusText().contains(QStringLiteral("no image engine")));

    // A server whose model cannot edit from a description: it can still fill an area, but it has
    // no upscaler and removes no backgrounds. The panel follows when an engine is added.
    MockImageServer server;
    ai::SdCppProvider *sdxl = engines.sdcpp(server, false);
    QVERIFY(sdxl);
    QTRY_VERIFY(availability->text().contains(QStringLiteral("“SDXL” cannot change a picture from a description")));
    QVERIFY(edit->unavailableReason({}).contains(QStringLiteral("cannot change a picture from a description")));
    QVERIFY(registry.find(fn::Inpaint)->unavailableReason({}).isEmpty());
    QVERIFY(registry.find(fn::Upscale)->unavailableReason({}).contains(QStringLiteral("has an upscaler")));
    QVERIFY(registry.find(fn::RemoveBackground)->unavailableReason({}).contains(QStringLiteral("can remove a background")));
    // An engine chosen in the node that is gone.
    QVERIFY(edit->unavailableReason({{fn::EngineParam, QStringLiteral("sdcpp-gone")}}).contains(QStringLiteral("no longer set up")));
    // With an edit model nothing is missing: chosen in the node, or as the engine in use.
    ai::SdCppProvider *klein = engines.sdcpp(server);
    QVERIFY(klein);
    QVERIFY(edit->unavailableReason({{fn::EngineParam, klein->instanceId()}}).isEmpty());
    QVERIFY(!edit->unavailableReason({}).isEmpty()); // the one in use is still the first one
    studio.providers()->setDefaultImageProvider(klein->instanceId());
    QVERIFY(edit->unavailableReason({}).isEmpty());
    QTRY_VERIFY(availability->isHidden());
    // A step changes a picture: the one in use is the engine for changes of Settings → AI (the
    // image engine unless another is chosen for changes), as for the editor's prompt bar.
    studio.providers()->setEditImageProvider(sdxl->instanceId());
    QVERIFY(edit->unavailableReason({}).contains(QStringLiteral("“SDXL” cannot change a picture from a description")));
    QTRY_VERIFY(!availability->isHidden());
    studio.providers()->setDefaultImageProvider(sdxl->instanceId());
    studio.providers()->setEditImageProvider(klein->instanceId());
    QVERIFY(edit->unavailableReason({}).isEmpty());
    QTRY_VERIFY(availability->isHidden());
    studio.providers()->setDefaultImageProvider(klein->instanceId());
    studio.providers()->setEditImageProvider(QString());
    // An empty instruction is refused when the step runs, before anything is sent.
    error = waitFor(evaluator.evaluate(chain(fn::Edit, QStringLiteral("Empty")), {twoColours(), {}}));
    QVERIFY2(error.contains(QStringLiteral("“Instruction” is empty")), qPrintable(error));
    QVERIFY(server.payloads.isEmpty());

    // The node editor: the engine as a list of the engines, the instruction in a field of several
    // lines, and a step that cannot run among the problems, outlined.
    QWidget owner;
    auto *w = new NodeEditorWindow(m_catalog.get(), &owner);
    w->setSaveQuestion({});
    const QString node = w->addNode(fn::Edit);
    auto *engine = w->findChild<ParamField *>(QStringLiteral("param:engine"))->findChild<QComboBox *>();
    QVERIFY(engine);
    QCOMPARE(engine->count(), 3);
    QCOMPARE(engine->itemText(0), QStringLiteral("The one in use"));
    QCOMPARE(engine->itemText(1), QStringLiteral("SDXL"));
    QCOMPARE(engine->itemText(2), QStringLiteral("Klein"));
    auto *prompt = w->findChild<ParamField *>(QStringLiteral("param:prompt"))->findChild<QPlainTextEdit *>();
    QVERIFY(prompt);
    prompt->setPlainText(QStringLiteral("make it night"));
    QCOMPARE(w->model()->graph().param(node, QStringLiteral("prompt")).toString(), QStringLiteral("make it night"));
    QVERIFY(!w->problems().join(QLatin1Char('\n')).contains(QStringLiteral("cannot change")));
    engine->setCurrentIndex(1);
    QCOMPARE(w->model()->graph().param(node, fn::EngineParam).toString(), sdxl->instanceId());
    QVERIFY2(w->problems().join(QLatin1Char('\n')).contains(QStringLiteral("“SDXL” cannot change a picture from a description")),
             qPrintable(w->problems().join(QLatin1Char('\n'))));
    QVERIFY(w->scene()->nodeItem(node)->toolTip().contains(QStringLiteral("cannot change")));
    QVERIFY(w->scene()->nodeItem(node)->summary().contains(QStringLiteral("Image engine: SDXL")));
    // On "The one in use", the problems and the outline follow another engine becoming the one in use.
    engine->setCurrentIndex(0); // before the engines change: the side panel is made again then
    QVERIFY(!w->problems().join(QLatin1Char('\n')).contains(QStringLiteral("cannot change"))); // Klein
    studio.providers()->setDefaultImageProvider(sdxl->instanceId());
    QVERIFY2(w->problems().join(QLatin1Char('\n')).contains(QStringLiteral("“SDXL” cannot change a picture from a description")),
             qPrintable(w->problems().join(QLatin1Char('\n'))));
    QVERIFY(w->scene()->nodeItem(node)->toolTip().contains(QStringLiteral("cannot change")));
    studio.providers()->setDefaultImageProvider(klein->instanceId());
    QVERIFY(!w->problems().join(QLatin1Char('\n')).contains(QStringLiteral("cannot change")));
    QVERIFY(!w->scene()->nodeItem(node)->toolTip().contains(QStringLiteral("cannot change")));
}

// "Edit with AI" sends an instruction edit to the engine as a Queue job, reports its progress, and
// its result feeds the next node; once it has run, it comes from the cache.
void TestFiltersUi::aiEditFeedsNextNode()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    QVERIFY(engines.sdcpp(server));
    filters::Graph g;
    const QString in = g.addNode(QStringLiteral("input"));
    const QString edit = g.addNode(ai::filter_nodes::Edit, QPointF(260, 0));
    const QString invert = g.addNode(QStringLiteral("invert"), QPointF(520, 0));
    const QString out = g.addNode(QStringLiteral("output"), QPointF(780, 0));
    QVERIFY(g.connect(in, QStringLiteral("image"), edit, QStringLiteral("image")));
    QVERIFY(g.connect(edit, QStringLiteral("image"), invert, QStringLiteral("image")));
    QVERIFY(g.connect(invert, QStringLiteral("image"), out, QStringLiteral("image")));
    QVERIFY(g.setParam(edit, QStringLiteral("prompt"), QStringLiteral("colorize it")));
    QVERIFY(g.isValid() && g.needsExplicitRun() && g.unavailable().isEmpty());

    const QImage picture = detailed(QSize(300, 200));
    filters::Evaluator evaluator;
    QMutex mutex;
    QStringList statuses;
    filters::EvalOptions options;
    options.stepProgress = [&](double, const QString &status) {
        QMutexLocker lock(&mutex);
        statuses << status;
    };
    QFuture<filters::EvalResult> f = evaluator.evaluate(g, {picture, {}}, options);
    const QString error = waitFor(f);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // The request: an instruction edit of the whole picture at the model's working size.
    QCOMPARE(server.payloads.size(), 1);
    const QJsonObject payload = server.payloads.first();
    QCOMPARE(payload.value(QStringLiteral("prompt")).toString(), QStringLiteral("colorize it"));
    QCOMPARE(payload.value(QStringLiteral("ref_images")).toArray().size(), 1);
    QVERIFY(!payload.contains(QStringLiteral("init_image")));
    QCOMPARE(QSize(payload.value(QStringLiteral("width")).toInt(), payload.value(QStringLiteral("height")).toInt()),
             img::fitToPixelBudget(picture.size(), 1024 * 1024, 64));
    // Its result, back at the picture's size, went through Invert.
    const filters::EvalResult r = f.result();
    QCOMPARE(r.image.size(), picture.size());
    QCOMPARE(QColor(r.image.pixel(150, 100)), QColor(255 - kOrange.red(), 255 - kOrange.green(), 255 - kOrange.blue()));
    QCOMPARE(r.evaluated, (QStringList{in, edit, invert, out}));
    // The step was a job in the Queue, which reported how far it was.
    {
        QMutexLocker lock(&mutex);
        QVERIFY2(statuses.join(QLatin1Char('\n')).contains(QStringLiteral("2 / 4")), qPrintable(statuses.join(QLatin1Char('\n'))));
    }
    Job *job = nullptr;
    for (Job *j : studio.jobs()->jobs()) {
        if (j->title().startsWith(QStringLiteral("Edit with AI: colorize it")))
            job = j;
    }
    QVERIFY(job);
    QCOMPARE(job->state(), Job::State::Succeeded);
    // Evaluated again, everything comes from the cache: no second request.
    f = evaluator.evaluate(g, {picture, {}}, options);
    QVERIFY(waitFor(f).isEmpty());
    QVERIFY(f.result().evaluated.isEmpty());
    QCOMPARE(server.payloads.size(), 1);
}

// A selection limits the change: the edit is of a crop around it (the region pipeline) and every
// pixel outside the selection stays as it was. Run previews it; Apply reuses it, as one undo step.
void TestFiltersUi::aiEditOfSelection()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    ai::SdCppProvider *klein = engines.sdcpp(server);
    QVERIFY(klein);
    const QSize size(900, 600);
    doc::Document *doc = studio.openImageAsDocument(detailed(size), QStringLiteral("Photo"));
    const QRect area(300, 200, 200, 150);
    doc->applySelection(rectMask(size, area), QStringLiteral("Select"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.resize(320, 640);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("colorize-with-ai"))));
    QVERIFY(panel.runner()->needsRun());
    QVERIFY(panel.findChild<QLabel *>(QStringLiteral("FilterAvailability"))->isHidden());
    // The instruction can be changed in the panel.
    QVERIFY(panel.field(QStringLiteral("Instruction")));
    auto *instruction = panel.field(QStringLiteral("Instruction"))->findChild<QPlainTextEdit *>();
    QVERIFY(instruction && instruction->toPlainText().startsWith(QStringLiteral("Colorize this black-and-white photo")));

    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QSignalSpy step(panel.runner(), &FilterRunner::stepProgress);
    QPushButton *run = buttonNamed(&panel, QStringLiteral("Run AI steps"));
    QVERIFY(run && !run->isHidden());
    run->click();
    QVERIFY(finished.wait(10000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QTRY_VERIFY(!canvas.preview().isNull());
    QVERIFY(step.count() >= 1);
    // The request: a crop around the selection at the model's working size.
    QCOMPARE(server.payloads.size(), 1);
    const QJsonObject payload = server.payloads.first();
    ai::ImageRequest probe;
    probe.mode = ai::GenMode::Edit;
    const ai::RegionPlan plan = ai::planRegion(img::maskBounds(doc->selection(), 8), size, 48, klein->capabilitiesFor(probe));
    QVERIFY(plan.rect != QRect(QPoint(0, 0), size));
    QCOMPARE(QSize(payload.value(QStringLiteral("width")).toInt(), payload.value(QStringLiteral("height")).toInt()), plan.modelSize);
    QCOMPARE(payload.value(QStringLiteral("ref_images")).toArray().size(), 1);

    // Apply: what Run computed, onto a new layer, one undo step; nothing outside the selection changes.
    const QImage before = doc->flattened();
    const int steps = doc->undoStack()->count();
    finished.clear();
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(10000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(server.payloads.size(), 1);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->undoStack()->count(), steps + 1);
    const QImage after = doc->flattened();
    QVERIFY(sameOutside(before, after, doc->selection()));
    QCOMPARE(QColor(after.pixel(area.center())), kOrange); // the colours as the model made them
    doc->undoStack()->undo();
    QCOMPARE(doc->flattened(), before);
}

// Cancel stops the step's job (the server is asked to cancel it) and nothing changes.
void TestFiltersUi::aiStepCancel()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    server.hold = true;
    QVERIFY(engines.sdcpp(server));
    doc::Document *doc = studio.openImageAsDocument(detailed(QSize(320, 240)), QStringLiteral("Photo"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("colorize-with-ai"))));
    const int layers = doc->layerCount();
    const int steps = doc->undoStack()->count();
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QTRY_VERIFY_WITH_TIMEOUT(server.polls >= 2, 10000); // the job runs on the server
    QPointer<Job> job;
    for (Job *j : studio.jobs()->jobs()) {
        if (j->title().startsWith(QStringLiteral("Edit with AI")))
            job = j;
    }
    QVERIFY(job);
    QCOMPARE(job->state(), Job::State::Running);
    panel.cancel();
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("Cancelled")));
    QTRY_VERIFY(server.cancelled);
    QVERIFY(server.received(QStringLiteral("/sdcpp/v1/jobs/job_1/cancel")));
    QTRY_COMPARE(job->state(), Job::State::Cancelled);
    QVERIFY(!panel.runner()->isBusy());
    QCOMPARE(doc->layerCount(), layers);
    QCOMPARE(doc->undoStack()->count(), steps);
}

// The plan's example of a saved filter (acceptance 5): Levels → Mask from image → Fill area with AI,
// applied from the Filters panel on a selection. The bright part is inpainted (an inpainting
// request of a crop around it, with its mask), as one undo step, and nothing outside the selection
// changes.
void TestFiltersUi::aiInpaintGraphOnSelection()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    QVERIFY(engines.sdcpp(server));
    filters::Graph g;
    g.name = QStringLiteral("Fill the sky");
    const QString in = g.addNode(QStringLiteral("input"));
    const QString levels = g.addNode(QStringLiteral("levels"), QPointF(260, 0));
    const QString bright = g.addNode(QStringLiteral("mask-from-image"), QPointF(520, 0));
    const QString fill = g.addNode(ai::filter_nodes::Inpaint, QPointF(780, 0));
    const QString out = g.addNode(QStringLiteral("output"), QPointF(1040, 0));
    QVERIFY(g.connect(in, QStringLiteral("image"), levels, QStringLiteral("image")));
    QVERIFY(g.connect(levels, QStringLiteral("image"), bright, QStringLiteral("image")));
    QVERIFY(g.connect(in, QStringLiteral("image"), fill, QStringLiteral("image")));
    QVERIFY(g.connect(bright, QStringLiteral("mask"), fill, QStringLiteral("mask")));
    QVERIFY(g.connect(fill, QStringLiteral("image"), out, QStringLiteral("image")));
    QVERIFY(g.setParam(levels, QStringLiteral("inBlack"), 128));
    QVERIFY(g.setParam(fill, QStringLiteral("prompt"), QStringLiteral("a sky full of stars")));
    g.publish({fill, QStringLiteral("prompt"), QStringLiteral("What to paint")});
    QVERIFY(g.isValid());
    const QString path = m_catalog->save(g);
    QVERIFY(!path.isEmpty());

    // Bright above, dark below, with detail in both.
    QImage photo(400, 300, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < photo.height(); ++y) {
        for (int x = 0; x < photo.width(); ++x)
            photo.setPixel(x, y, y < 150 ? qRgb(230 + x % 26, 230 + y % 26, 240) : qRgb(x % 40, y % 40, 20));
    }
    doc::Document *doc = studio.openImageAsDocument(photo, QStringLiteral("Sky"));
    doc->applySelection(rectMask(photo.size(), QRect(0, 0, 200, 300)), QStringLiteral("Select"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(path));
    QVERIFY(panel.field(QStringLiteral("What to paint")));
    const QImage before = doc->flattened();
    const int steps = doc->undoStack()->index();
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(10000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));

    // An inpainting request: the crop, its mask and the strength, no reference images.
    QCOMPARE(server.payloads.size(), 1);
    const QJsonObject payload = server.payloads.first();
    QVERIFY(payload.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("a sky full of stars")));
    QVERIFY(payload.contains(QStringLiteral("init_image")));
    QVERIFY(payload.contains(QStringLiteral("mask_image")));
    QCOMPARE(payload.value(QStringLiteral("strength")).toDouble(), 0.9);
    QVERIFY(!payload.contains(QStringLiteral("ref_images")));
    // One undo step; the bright part of the selection changed, the rest did not.
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    const QImage after = doc->flattened();
    QVERIFY(sameOutside(before, after, doc->selection()));
    QVERIFY(after.pixel(100, 60) != before.pixel(100, 60));
    QCOMPARE(after.pixel(100, 260), before.pixel(100, 260)); // dark: not in the area
    doc->undoStack()->undo();
    QCOMPARE(doc->flattened(), before);
}

// Every built-in AI filter from the Filters panel against the mock servers: Colorize with AI and
// Clean up background (instruction edits), Remove background (the original layer hidden under the
// cut-out) and Upscale ×2 (the picture becomes twice as large), each as one undo step.
void TestFiltersUi::builtInAiFiltersEndToEnd()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    QVERIFY(engines.sdcpp(server, true, true)); // FLUX.2 klein, and an upscaler
    QVERIFY(engines.webUi(server));             // removes backgrounds
    const QSize size(240, 160);
    const QImage photo = img::normalized(detailed(size));
    doc::Document *doc = studio.openImageAsDocument(photo, QStringLiteral("Photo"));
    const QUuid original = doc->activeLayerId();
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    targetCombo(panel)->setCurrentIndex(0);
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    auto apply = [&](const QString &id) -> QString {
        doc->setActiveLayer(original);
        if (!panel.selectFilter(builtIn(id)))
            return QStringLiteral("not listed");
        if (!panel.findChild<QLabel *>(QStringLiteral("FilterAvailability"))->isHidden())
            return panel.findChild<QLabel *>(QStringLiteral("FilterAvailability"))->text();
        finished.clear();
        if (!panel.applySelected())
            return panel.statusText();
        if (!finished.wait(10000))
            return QStringLiteral("timed out");
        return finished.first().at(0).toBool() ? QString() : finished.first().at(1).toString();
    };
    // Undo steps by the stack's index: an undone step stays in its count until the next one.
    int steps = doc->undoStack()->index();

    // Colorize with AI, nothing selected: the whole picture is edited.
    QString error = apply(QStringLiteral("colorize-with-ai"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(server.payloads.size(), 1);
    QVERIFY(server.payloads.last().value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("Colorize this black-and-white photo")));
    QCOMPARE(QSize(server.payloads.last().value(QStringLiteral("width")).toInt(), server.payloads.last().value(QStringLiteral("height")).toInt()),
             img::fitToPixelBudget(size, 1024 * 1024, 64));
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    QCOMPARE(QColor(doc->flattened().pixel(10, 10)), kOrange);
    doc->undoStack()->undo();
    QCOMPARE(doc->layerCount(), 1);

    // Clean up background, on the selected background only.
    const QRect background(0, 0, size.width(), 50);
    doc->applySelection(rectMask(size, background), QStringLiteral("Select"));
    const QImage before = doc->flattened();
    error = apply(QStringLiteral("clean-up-background"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(server.payloads.size(), 2);
    QVERIFY(server.payloads.last().value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("Clean up the background")));
    const QImage cleaned = doc->flattened();
    QVERIFY(sameOutside(before, cleaned, doc->selection()));
    int changed = 0;
    for (int y = background.top(); y <= background.bottom(); ++y) {
        for (int x = background.left(); x <= background.right(); ++x)
            changed += cleaned.pixel(x, y) != before.pixel(x, y);
    }
    QVERIFY2(changed * 2 > background.width() * background.height(), qPrintable(QString::number(changed)));
    doc->undoStack()->undo();
    doc->clearSelection();

    // Remove background: the cut-out on a new layer, the original hidden (and unchanged) under it.
    steps = doc->undoStack()->index();
    error = apply(QStringLiteral("remove-background"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(server.cutouts, 1);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    QVERIFY(!doc->findLayer(original)->props.visible);
    QCOMPARE(doc->findLayer(original)->image, photo);
    const QImage cut = doc->activeLayer()->image;
    QCOMPARE(qAlpha(cut.pixel(200, 80)), 0);           // the right half was background
    QCOMPARE(cut.pixel(40, 80), photo.pixel(40, 80)); // the subject stays as it was
    QVERIFY2(panel.statusText().contains(QStringLiteral("hidden")), qPrintable(panel.statusText()));
    doc->undoStack()->undo();
    QCOMPARE(doc->layerCount(), 1);
    QVERIFY(doc->findLayer(original)->props.visible);

    // Upscale ×2: the picture becomes twice as large, with the upscaled picture on a new layer.
    steps = doc->undoStack()->index();
    error = apply(QStringLiteral("upscale-2x"));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(server.upscales, 1);
    QCOMPARE(doc->size(), size * 2);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->activeLayer()->image.size(), size * 2);
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    QVERIFY2(panel.statusText().contains(QStringLiteral("480 × 320")), qPrintable(panel.statusText()));
    doc->undoStack()->undo();
    QCOMPARE(doc->size(), size);
    QCOMPARE(doc->layerCount(), 1);
    QCOMPARE(doc->activeLayer()->image, photo);
}

// ---- Review fixes --------------------------------------------------------------------------------

// The layer is painted on while Apply waits for an AI step: the stroke outside the (round)
// selection stays, inside the filter's result replaces it. On the layer itself and with a cut-out
// onto a new layer (the hidden original keeps the stroke, the new layer shows it outside the area).
void TestFiltersUi::applyKeepsChangesMadeMeanwhile()
{
    Studio studio;
    const QSize size(64, 48);
    const QImage photo = img::normalized(detailed(size));
    doc::Document *doc = studio.openImageAsDocument(photo, QStringLiteral("Photo"));
    QImage ellipse = img::createMask(size, 0);
    {
        QPainter p(&ellipse);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(QRect(8, 8, 48, 32));
    }
    doc->applySelection(ellipse, QStringLiteral("Select"));
    QCOMPARE(qGray(doc->selection().pixel(10, 10)), 0); // a corner of its bounds, not selected
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    targetCombo(panel)->setCurrentIndex(1); // this layer
    const QString path = m_catalog->save(chain(QStringLiteral("test-gated-ai"), QStringLiteral("Gate")));
    QVERIFY(panel.selectFilter(path));
    const int steps = doc->undoStack()->index();
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QTRY_VERIFY(gate().isWaiting());
    // A stroke while the step waits: inside the selection's bounds, outside the selection, and at
    // the centre (selected).
    QImage green(4, 4, QImage::Format_ARGB32_Premultiplied);
    green.fill(Qt::green);
    doc->applyPixels(doc->activeLayerId(), QRect(9, 9, 4, 4), green, QStringLiteral("Paint"));
    doc->applyPixels(doc->activeLayerId(), QRect(30, 22, 4, 4), green, QStringLiteral("Paint"));
    const QImage painted = doc->activeLayer()->image;
    gate().open();
    QVERIFY(finished.wait(5000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    const QImage after = doc->activeLayer()->image;
    QCOMPARE(QColor(after.pixel(10, 10)), QColor(Qt::green)); // the stroke outside the selection stays
    QVERIFY(sameOutside(painted, after, doc->selection()));
    // Inside, the filter's result of the layer as it was when Apply started.
    QCOMPARE(after.pixel(32, 24), filters::ops::invert(photo).pixel(32, 24));
    QVERIFY2(panel.statusText().contains(QStringLiteral("changed while the filter ran")), qPrintable(panel.statusText()));
    QCOMPARE(doc->undoStack()->index(), steps + 3); // two strokes and the filter
    doc->undoStack()->undo();
    QCOMPARE(doc->activeLayer()->image, painted);
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    QCOMPARE(doc->activeLayer()->image, photo);

    // A cut-out onto a new layer, which holds the layer as it is now outside the area; the original
    // is hidden under it, with the stroke.
    targetCombo(panel)->setCurrentIndex(0);
    const QUuid original = doc->activeLayerId();
    QVERIFY(panel.selectFilter(m_catalog->save(chain(QStringLiteral("test-gated-cut"), QStringLiteral("Cut")))));
    finished.clear();
    QVERIFY(panel.applySelected());
    QTRY_VERIFY(gate().isWaiting());
    doc->applyPixels(original, QRect(9, 9, 4, 4), green, QStringLiteral("Paint"));
    gate().open();
    QVERIFY(finished.wait(5000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(doc->layerCount(), 2);
    QVERIFY(!doc->findLayer(original)->props.visible);
    QCOMPARE(QColor(doc->findLayer(original)->image.pixel(10, 10)), QColor(Qt::green));
    const QImage cutLayer = doc->activeLayer()->image;
    QCOMPARE(QColor(cutLayer.pixel(10, 10)), QColor(Qt::green)); // outside the area: as the layer is now
    QCOMPARE(qAlpha(cutLayer.pixel(44, 24)), 0);                 // the cut-out, inside the area
    QCOMPARE(QColor(doc->flattened().pixel(10, 10)), QColor(Qt::green));
}

// The canvas follows the document while the Filters tab shows a preview: a change shows at once
// (the preview goes until the next one is ready). After Apply, the preview stays off when the tab
// is shown again; the check box brings it back.
void TestFiltersUi::panelPreviewFollowsTheDocument()
{
    Studio studio;
    doc::Document *doc = studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.resize(340, 540);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("warm-tones"))));
    QTRY_VERIFY(!canvas.preview().isNull());

    // A brush stroke: the canvas shows the document at once, then the preview of the new pixels.
    QImage green(8, 8, QImage::Format_ARGB32_Premultiplied);
    green.fill(Qt::green);
    doc->applyPixels(doc->activeLayerId(), QRect(0, 0, 8, 8), green, QStringLiteral("Paint"));
    QVERIFY(canvas.preview().isNull());
    QTRY_VERIFY(!canvas.preview().isNull());
    const QColor warmGreen(canvas.preview().pixel(2, 2)); // was red before the stroke
    QVERIFY2(warmGreen.green() > warmGreen.red(), qPrintable(warmGreen.name()));

    // Apply; the tab hidden and shown again (another tab, a minimised window) does not preview
    // the filter on top of its own result.
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(5000));
    QVERIFY(finished.first().at(0).toBool());
    QVERIFY(canvas.preview().isNull());
    panel.hide();
    panel.show();
    QTest::qWait(150);
    QVERIFY(canvas.preview().isNull());
    // Asked for with the check box, it previews again.
    QCheckBox *check = nullptr;
    for (QCheckBox *c : panel.findChildren<QCheckBox *>()) {
        if (c->text() == QStringLiteral("Preview on the canvas"))
            check = c;
    }
    QVERIFY(check && check->isChecked());
    check->setChecked(false);
    check->setChecked(true);
    QTRY_VERIFY(!canvas.preview().isNull());
}

// Another filter chosen while Apply waits for an AI step: the message names the filter that was
// applied, and the one chosen meanwhile previews.
void TestFiltersUi::applyNamesTheFilterItApplied()
{
    Studio studio;
    doc::Document *doc = studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(m_catalog->save(chain(QStringLiteral("test-gated-ai"), QStringLiteral("Gate")))));
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QTRY_VERIFY(gate().isWaiting());
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("sharpen"))));
    gate().open();
    QVERIFY(finished.wait(5000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QVERIFY2(panel.statusText().startsWith(QStringLiteral("Applied “Gate”")), qPrintable(panel.statusText()));
    QCOMPARE(doc->undoStack()->text(doc->undoStack()->index() - 1), QStringLiteral("Filter: Gate"));
    QCOMPARE(doc->activeLayer()->props.name, QStringLiteral("Gate"));
    QTRY_VERIFY(!canvas.preview().isNull()); // Sharpen, on the new layer
}

// Cancel once every step is done, while the result is being put together: nothing changes.
void TestFiltersUi::cancelWhileTheResultIsPutTogether()
{
    Studio studio;
    doc::Document *doc = studio.openImageAsDocument(detailed(QSize(800, 600)), QStringLiteral("Photo"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("black-and-white"))));
    const int layers = doc->layerCount();
    const int steps = doc->undoStack()->index();
    // The last progress report comes when the evaluation has ended, before the result is applied.
    bool cancelledLate = false;
    connect(panel.runner(), &FilterRunner::progress, &panel, [&](int done, int total, const QString &) {
        if (done == total && !cancelledLate) {
            cancelledLate = true;
            panel.cancel();
        }
    });
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(5000));
    QVERIFY(cancelledLate);
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY2(finished.first().at(1).toString().contains(QStringLiteral("Cancelled")), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(doc->layerCount(), layers);
    QCOMPARE(doc->undoStack()->index(), steps);
    QVERIFY(!panel.runner()->isBusy());
}

// An AI step's job: cancelled, the Queue offers no Retry (its result would go nowhere); when its
// engine is removed while it runs, it fails in plain words instead of waiting for ever.
void TestFiltersUi::aiStepJobEnds()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    server.hold = true;
    ai::SdCppProvider *klein = engines.sdcpp(server);
    QVERIFY(klein);
    doc::Document *doc = studio.openImageAsDocument(detailed(QSize(320, 240)), QStringLiteral("Photo"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("colorize-with-ai"))));
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    auto stepJob = [&]() -> Job * {
        Job *found = nullptr;
        for (Job *j : studio.jobs()->jobs()) {
            if (j->title().startsWith(QStringLiteral("Edit with AI")) && !j->isFinished())
                found = j;
        }
        return found;
    };

    QVERIFY(panel.applySelected());
    QTRY_VERIFY_WITH_TIMEOUT(server.polls >= 2, 10000);
    QPointer<Job> job = stepJob();
    QVERIFY(job);
    panel.cancel();
    QTRY_COMPARE(finished.count(), 1);
    QTRY_COMPARE(job->state(), Job::State::Cancelled);
    QVERIFY(!job->canRetry());
    studio.jobs()->retry(job);
    QTest::qWait(100);
    QCOMPARE(job->state(), Job::State::Cancelled);
    QCOMPARE(server.payloads.size(), 1); // nothing sent again

    // The engine is removed while the step runs.
    finished.clear();
    QVERIFY(panel.applySelected());
    QTRY_VERIFY_WITH_TIMEOUT(server.payloads.size() == 2 && server.polls >= 2, 10000);
    job = stepJob();
    QVERIFY(job);
    studio.providers()->removeProvider(klein->instanceId());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY2(finished.first().at(1).toString().contains(QStringLiteral("removed")), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(job->state(), Job::State::Failed);
    QVERIFY(!panel.runner()->isBusy());
    QCOMPARE(doc->layerCount(), 1);
}

// Upscale ×2 with a selection: the whole picture becomes twice as large (the panel says so once the
// AI step has run), and only the selected area gets the upscaler's result.
void TestFiltersUi::upscaleWithSelection()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    QVERIFY(engines.sdcpp(server, true, true));
    const QSize size(240, 160);
    doc::Document *doc = studio.openImageAsDocument(img::normalized(detailed(size)), QStringLiteral("Photo"));
    const QRect area(40, 40, 80, 60);
    doc->applySelection(rectMask(size, area), QStringLiteral("Select"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("upscale-2x"))));
    auto *hint = panel.findChild<QLabel *>(QStringLiteral("FilterSelectionHint"));
    QVERIFY(hint);
    QCOMPARE(hint->text(), QStringLiteral("Only the selected area changes."));
    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    buttonNamed(&panel, QStringLiteral("Run AI steps"))->click();
    QVERIFY(finished.wait(10000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QTRY_VERIFY2(hint->text().startsWith(QStringLiteral("The picture becomes 480 × 320 pixels.")), qPrintable(hint->text()));
    QVERIFY(hint->text().contains(QStringLiteral("only resized")));

    const int steps = doc->undoStack()->index();
    finished.clear();
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(10000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(server.upscales, 1); // Apply used what Run made
    QCOMPARE(doc->size(), size * 2);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    const QImage upscaled = doc->activeLayer()->image;
    QCOMPARE(upscaled.size(), size * 2);
    QCOMPARE(qAlpha(upscaled.pixel(area.center() * 2)), 255); // the selected area, upscaled
    QCOMPARE(qAlpha(upscaled.pixel(400, 280)), 0);           // outside: the picture only resized
    QVERIFY2(panel.statusText().contains(QStringLiteral("480 × 320")), qPrintable(panel.statusText()));
    QVERIFY2(panel.statusText().contains(QStringLiteral("only resized")), qPrintable(panel.statusText()));
    doc->undoStack()->undo();
    QCOMPARE(doc->size(), size);
    QCOMPARE(doc->layerCount(), 1);
}

// Copies and new filters get names of their own, so the list can tell them apart; Edit a copy… on
// a built-in filter opens the copy made before while it is unchanged.
void TestFiltersUi::filterNamesStayApart()
{
    const QString sharpen = builtIn(QStringLiteral("sharpen"));
    const QString first = m_catalog->duplicate(sharpen);
    const QString second = m_catalog->duplicate(sharpen);
    QVERIFY(!first.isEmpty() && !second.isEmpty() && first != second);
    QCOMPARE(m_catalog->entry(first)->name, QStringLiteral("Sharpen copy"));
    QCOMPARE(m_catalog->entry(second)->name, QStringLiteral("Sharpen copy 2"));
    const QString mine = m_catalog->create(QStringLiteral("Mine"));
    QCOMPARE(m_catalog->entry(mine)->name, QStringLiteral("Mine"));
    QCOMPARE(m_catalog->entry(m_catalog->create(QStringLiteral("mine")))->name, QStringLiteral("mine 2")); // case does not count
    QCOMPARE(m_catalog->uniqueName(QStringLiteral("Mine")), QStringLiteral("Mine 3"));
    QCOMPARE(m_catalog->uniqueName(QStringLiteral("Mine"), mine), QStringLiteral("Mine")); // its own name

    // Edit a copy… opens the unchanged copy again rather than making a third.
    Studio studio;
    studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    QVERIFY(panel.selectFilter(sharpen));
    const qsizetype filters = m_catalog->entries().size();
    NodeEditorWindow *editor = panel.editSelected();
    QVERIFY(editor);
    QCOMPARE(editor->path(), first);
    QCOMPARE(m_catalog->entries().size(), filters);
    // Changed and saved, it is no longer a plain copy: the next Edit a copy… makes a new one.
    QVERIFY(editor->setParam(QStringLiteral("n2"), QStringLiteral("amount"), 120));
    QVERIFY(editor->save());
    QVERIFY(panel.selectFilter(sharpen));
    NodeEditorWindow *another = panel.editSelected();
    QVERIFY(another && another != editor);
    QCOMPARE(another->path(), second); // the other copy, still unchanged
    delete another;
    QVERIFY(m_catalog->remove(second));
    QVERIFY(panel.selectFilter(sharpen));
    NodeEditorWindow *third = panel.editSelected();
    QVERIFY(third);
    QCOMPARE(third->model()->graph().name, QStringLiteral("Sharpen copy 2"));

    // Save as… with a name a saved filter has: a number, in the file and in the list.
    QString error;
    QVERIFY2(third->saveAs(QStringLiteral("Mine"), &error), qPrintable(error));
    QCOMPARE(third->model()->graph().name, QStringLiteral("Mine 3"));
    QCOMPARE(m_catalog->entry(third->path())->name, QStringLiteral("Mine 3"));
}

// Run AI steps in the filter editor, then Apply of the saved filter from the Filters panel on the
// same layer: the step is not sent to the engine again.
void TestFiltersUi::editorRunServesApply()
{
    Studio studio;
    TestEngines engines(studio);
    MockImageServer server;
    QVERIFY(engines.sdcpp(server));
    doc::Document *doc = studio.openImageAsDocument(detailed(QSize(320, 240)), QStringLiteral("Photo"));
    EditorState state;
    CanvasView canvas(&state);
    canvas.setDocument(doc);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    targetCombo(panel)->setCurrentIndex(0);
    QVERIFY(panel.selectFilter(builtIn(QStringLiteral("colorize-with-ai"))));
    NodeEditorWindow *editor = panel.editSelected(); // a copy, with the active layer as its picture
    QVERIFY(editor);
    QAction *run = nullptr;
    for (QAction *a : editor->findChildren<QAction *>()) {
        if (a->text() == QStringLiteral("Run AI steps"))
            run = a;
    }
    QVERIFY(run);
    QTRY_VERIFY(!editor->isPreviewPending());
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(server.payloads.size() == 1 && !editor->isPreviewPending(), 10000);
    QVERIFY2(!editor->preview().isNull(), qPrintable(editor->previewText()));

    QSignalSpy finished(panel.runner(), &FilterRunner::finished);
    QCOMPARE(panel.currentPath(), editor->path());
    QVERIFY(panel.applySelected());
    QVERIFY(finished.wait(10000));
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(server.payloads.size(), 1);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(QColor(doc->flattened().pixel(160, 120)), kOrange);
}

// At about its size in a 1280 × 720 window, the panel shows Apply and why a filter cannot run
// without scrolling, also for an AI filter with its instruction field.
void TestFiltersUi::panelActionsInView()
{
    Studio studio;
    studio.openImageAsDocument(twoColours(), QStringLiteral("Two colours"));
    EditorState state;
    CanvasView canvas(&state);
    FiltersPanel panel(&studio, &canvas, nullptr, m_catalog.get());
    panel.resize(340, 540);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    auto inView = [&](QWidget *w) {
        if (!w->isVisible())
            return false;
        const QRect r(w->mapTo(&panel, QPoint(0, 0)), w->size());
        return panel.rect().contains(r);
    };
    for (const QString &id : {QStringLiteral("soft-focus"), QStringLiteral("colorize-with-ai")}) {
        QVERIFY(panel.selectFilter(builtIn(id)));
        QTest::qWait(20);
        QVERIFY2(inView(panel.applyButton()), qPrintable(id));
    }
    // No image engine here: the reason is in view, with Run AI steps.
    auto *availability = panel.findChild<QLabel *>(QStringLiteral("FilterAvailability"));
    QVERIFY(inView(availability));
    QVERIFY(availability->text().contains(QStringLiteral("no image engine")));
    QVERIFY(inView(buttonNamed(&panel, QStringLiteral("Run AI steps"))));
}

QTEST_MAIN(TestFiltersUi)

#include "tst_filters_ui.moc"
