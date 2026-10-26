// Files as in an image editor (M17): File → Save writes a picture back to the file it came from,
// Save as… and Export a copy…, the unsaved-changes marker and the questions on closing and quitting,
// recent files; the main window's keys in other windows; closing a document stops its prompt-bar
// run; a filter of the whole picture.

#include "MockHttpServer.h"
#include "ai/ModelDefaults.h"
#include "ai/providers/SdCppProvider.h"
#include "core/ImageOps.h"
#include "core/Psd.h"
#include "core/Job.h"
#include "core/Settings.h"
#include "doc/Document.h"
#include "doc/ImageFile.h"
#include "doc/Project.h"
#include "doc/PsdIO.h"
#include "filters/Graph.h"
#include "filters/Registry.h"
#include "plugins/BuiltInPlugins.h"
#include "ui/DocumentFiles.h"
#include "ui/MainWindow.h"
#include "ui/Shortcuts.h"
#include "ui/Studio.h"
#include "ui/dialogs/NewFromPromptDialog.h"
#include "ui/ext/Extension.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/FilterRunner.h"
#include "ui/filters/FiltersPanel.h"
#include "ui/filters/GraphModel.h"
#include "ui/filters/NodeEditorWindow.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/PromptBar.h"
#include "ui/pages/PromptPlan.h"
#include "ui/pages/StartScreen.h"
#include "ui/pages/VariantStrip.h"
#include "ui/widgets/Controls.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QtEndian>
#include <QtTest>

using namespace lumen;
using namespace lumen::ui;

namespace {

// Plugins are off by default: on for one test, back to the default afterwards.
struct PluginsOn
{
    explicit PluginsOn(const QStringList &ids = {})
    {
        ext::Extensions &registry = ext::Extensions::instance();
        for (const ext::Extension *e : registry.all()) {
            if (ids.isEmpty() || ids.contains(e->id()))
                registry.setEnabled(e->id(), true);
        }
        registry.refresh();
    }
    ~PluginsOn()
    {
        for (const ext::Extension *e : ext::Extensions::instance().all())
            Settings::instance().remove(QStringLiteral("plugins/%1/enabled").arg(e->id()));
        ext::Extensions::instance().refresh();
    }
};

// A photo with detail everywhere, opaque.
QImage photo(const QSize &size = QSize(160, 120))
{
    QImage im(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < im.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < im.width(); ++x)
            line[x] = qRgb((x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256);
    }
    return im;
}

QImage filled(const QSize &size, const QColor &color)
{
    QImage im(size, QImage::Format_ARGB32_Premultiplied);
    im.fill(color);
    return im;
}

QImage readImage(const QString &path)
{
    QImageReader reader(path);
    return img::normalized(reader.read());
}

QByteArray bytesOf(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A red square painted onto the active layer, as one undo step.
void paintSquare(doc::Document *doc, const QRect &rect = QRect(10, 10, 20, 20))
{
    doc->applyPixels(doc->activeLayer()->id, rect, filled(rect.size(), QColor(220, 30, 30)), QStringLiteral("Paint"));
}

// The smallest GIF there is (1 × 1): Qt reads GIF but cannot write it.
QByteArray tinyGif()
{
    static const unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xff, 0xff,
                                        0xff, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00,
                                        0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b};
    return QByteArray(reinterpret_cast<const char *>(gif), sizeof(gif));
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(bytes), bytes.size());
}

void addToBigEndian(QByteArray &bytes, qsizetype at, qint64 delta)
{
    const quint32 value = qFromBigEndian<quint32>(bytes.constData() + at);
    qToBigEndian(quint32(value + delta), bytes.data() + at);
}

// Lumen's PSD of `file` (one pixel layer) as Photoshop writes a text, effect or adjustment layer:
// its record also holds additional information `key`, which Lumen's writer never writes. With
// `icc`, the file has that colour profile too (image resource 1039).
QByteArray photoshopPsd(const psd::File &file, const QByteArray &key, const QByteArray &icc = {})
{
    QByteArray bytes = psd::toBytes(file);
    // No colour mode data or image resources: the layer and mask section's length is at 34, the
    // layer information's at 38; the record's extra data length follows its blend mode.
    if (!key.isEmpty()) {
        const QByteArray block = QByteArray("8BIM") + key + QByteArray("\0\0\0\4\0\0\0\0", 8);
        const qsizetype record = bytes.indexOf("8BIMnorm");
        const qsizetype luni = bytes.indexOf("8BIMluni", record);
        bytes.insert(luni, block);
        addToBigEndian(bytes, record + 12, block.size());
        addToBigEndian(bytes, 38, block.size());
        addToBigEndian(bytes, 34, block.size());
    }
    if (!icc.isEmpty()) {
        QByteArray resource = QByteArray("8BIM") + QByteArray("\x04\x0f\0\0", 4); // 1039, no name
        QByteArray length(4, Qt::Uninitialized);
        qToBigEndian(quint32(icc.size()), length.data());
        resource += length + icc;
        if (icc.size() % 2)
            resource += '\0';
        bytes.insert(34, resource);
        addToBigEndian(bytes, 30, resource.size());
    }
    return bytes;
}

// Answers the next message box (the main window's own questions) with `button` once it shows,
// keeping its text in `text`.
void answerNextBox(QMessageBox::StandardButton button, QString *text)
{
    auto *timer = new QTimer;
    auto tries = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, [timer, button, text, tries] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box || ++*tries > 250) {
            timer->stop();
            timer->deleteLater();
        }
        if (box) {
            *text = box->text();
            box->button(button)->click();
        }
    });
    timer->start(20);
}

template<typename T>
T waitFor(QFuture<T> future, int ms = 10000)
{
    if (!QTest::qWaitFor([&future] { return future.isFinished(); }, ms))
        return T();
    return future.result();
}

// A studio with a project, the main window and its file questions answered by the test.
struct Fixture
{
    QTemporaryDir dir;
    Studio studio;
    std::unique_ptr<MainWindow> window;
    QList<SaveOverQuestion> saveOverQuestions;
    SaveOverReply saveOverReply{SaveOverReply::SaveOver, false};
    QList<QStringList> unsavedQuestions;
    QMessageBox::StandardButton unsavedReply = QMessageBox::Cancel;
    QStringList pathQuestions;
    QString pathReply;

    bool setUp()
    {
        Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
        Settings::instance().removeRecentFile(QString());
        studio.openProject(dir.filePath(QStringLiteral("files.lumen")));
        if (!QTest::qWaitFor([this] { return studio.isProjectOpen(); }, 5000))
            return false;
        window = std::make_unique<MainWindow>(&studio);
        window->resize(1300, 800);
        window->show();
        DocumentFiles *f = files();
        f->askSaveOver = [this](const SaveOverQuestion &q) {
            saveOverQuestions << q;
            return saveOverReply;
        };
        f->askUnsaved = [this](const QStringList &names, bool) {
            unsavedQuestions << names;
            return unsavedReply;
        };
        f->askPath = [this](const QString &, const QString &suggested, const QString &, QString *) {
            pathQuestions << suggested;
            return pathReply;
        };
        return QTest::qWaitForWindowExposed(window.get());
    }
    ~Fixture()
    {
        window.reset();
        Settings::instance().removeRecentFile(QString());
    }
    DocumentFiles *files() const { return window->editor()->files(); }
    QString path(const QString &name) const { return dir.filePath(name); }
    QTabBar *tabs() const { return window->editor()->findChild<QTabBar *>(); }
    // Opens a file and waits for its document.
    doc::Document *open(const QString &file)
    {
        const int before = int(studio.documents().size());
        studio.openFile(file);
        if (!QTest::qWaitFor([&] { return studio.documents().size() > before || studio.documentForFile(file); }, 5000))
            return nullptr;
        return studio.documentForFile(file);
    }
};

// The mock stable-diffusion.cpp server of a FLUX.2 [klein] engine: every request is held while
// `hold` is set, then answered with one plain colour per image.
struct MockSdCpp
{
    MockHttpServer server;
    QList<QJsonObject> generations;
    bool hold = false;
    int cancels = 0;

    MockSdCpp()
    {
        server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) { return MockResponse::json({}); });
        server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [this](const MockRequest &req) {
            generations << req.json();
            return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_%1").arg(generations.size())}}, 202);
        });
        for (int n = 1; n <= 12; ++n) {
            server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_%1").arg(n), [this, n](const MockRequest &) {
                if (hold)
                    return MockResponse::json({{QStringLiteral("status"), QStringLiteral("queued")}});
                const QJsonObject g = generations.value(n - 1);
                const QSize size(g.value(QStringLiteral("width")).toInt(), g.value(QStringLiteral("height")).toInt());
                QJsonArray images;
                for (int i = 0; i < std::max(1, g.value(QStringLiteral("batch_count")).toInt()); ++i) {
                    QImage im(size, QImage::Format_ARGB32);
                    im.fill(QColor(40 * i, 90, 200));
                    images.append(QJsonObject{{QStringLiteral("index"), i}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(im).toBase64())}});
                }
                return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")},
                                           {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
            });
            server.route("POST", QStringLiteral("/sdcpp/v1/jobs/job_%1/cancel").arg(n), [this](const MockRequest &) {
                ++cancels;
                return MockResponse::json({});
            });
        }
    }
};

ai::ModelDefaults kleinPreset()
{
    ai::ModelDefaults klein;
    klein.id = QStringLiteral("flux2-klein-4b");
    klein.name = QStringLiteral("FLUX.2 [klein] 4B");
    klein.cfg = 1.0;
    klein.steps = 4;
    klein.nativeResolution = 1024;
    klein.edit = true;
    klein.references = 4;
    return klein;
}

// A node type for the tests: makes the left half of the picture transparent (a cut-out).
filters::NodeTypePtr cutLeftHalf()
{
    filters::NodeTypeInfo info;
    info.id = QStringLiteral("test-cut-left");
    info.name = QStringLiteral("Cut left half");
    info.category = filters::categories::Color;
    info.inputs = {{QStringLiteral("image"), QStringLiteral("Image"), filters::PortType::Image, false}};
    info.outputs = {{QStringLiteral("image"), QStringLiteral("Image"), filters::PortType::Image, false}};
    return filters::makeNodeType(info, [](const filters::NodeArgs &args, filters::EvalContext &) -> filters::NodeResult {
        QImage im = args.image(QStringLiteral("image")).copy();
        QPainter p(&im);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(QRect(0, 0, im.width() / 2, im.height()), Qt::transparent);
        p.end();
        return filters::Values{{QStringLiteral("image"), im}};
    });
}

// Input → `middle` → Output, the Input reading the layer or the whole picture.
filters::Graph oneStep(const QString &middle, bool whole, const filters::NodeRegistry &registry = filters::NodeRegistry::global())
{
    filters::Graph g;
    g.name = QStringLiteral("One step");
    const QString in = g.addNode(QStringLiteral("input"), QPointF(0, 0), registry);
    const QString mid = g.addNode(middle, QPointF(260, 0), registry);
    const QString out = g.addNode(QStringLiteral("output"), QPointF(520, 0), registry);
    g.connect(in, QStringLiteral("image"), mid, QStringLiteral("image"));
    g.connect(mid, QStringLiteral("image"), out, QStringLiteral("image"));
    if (whole)
        g.setParam(in, QStringLiteral("source"), QStringLiteral("picture"));
    return g;
}

bool applyAndWait(FilterRunner &runner, FilterRunner::Target target)
{
    QSignalSpy finished(&runner, &FilterRunner::finished);
    QString why;
    if (!runner.apply(target, &why)) {
        qWarning() << why;
        return false;
    }
    return finished.wait(10000) && finished.first().at(0).toBool();
}

} // namespace

class TestFiles : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void imageFormats();
    void saveOverOriginal();
    void saveOverJpeg();
    void saveAsFormats();
    void closeAndQuitAsk();
    void saveOverChangedFile();
    void notACopyOfItself();
    void photoshopFiles();
    void coloursAndCameraDetails();
    void recentFiles();
    void shortcutsInOtherWindows();
    void closingStopsItsRun();
    void wholePictureInput();
};

void TestFiles::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    plugins::registerBuiltInPlugins();
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
}

// The formats Lumen writes here, what saving in each changes, and the JPEG quality a file had.
void TestFiles::imageFormats()
{
    const QList<doc::ImageFormat> formats = doc::writableImageFormats();
    QVERIFY(formats.size() >= 4);
    QCOMPARE(formats.first().id, QByteArray("png"));
    QVERIFY(doc::imageFormatById("psd") && doc::imageFormatById("psd")->layers);
    QCOMPARE(doc::imageFormatFor(QStringLiteral("C:/a/Photo.JPG"))->id, QByteArray("jpeg"));
    QVERIFY(!doc::imageFormatFor(QStringLiteral("anim.gif")));
    QVERIFY(!doc::imageFormatFor(QStringLiteral("no-suffix")));
    // WebP and TIFF only where Qt can write them.
    QCOMPARE(bool(doc::imageFormatById("webp")), QImageWriter::supportedImageFormats().contains("webp"));
    QCOMPARE(bool(doc::imageFormatById("tiff")), QImageWriter::supportedImageFormats().contains("tiff"));
    QVERIFY(doc::imageFileFilter(formats).contains(QStringLiteral("JPEG (*.jpg *.jpeg)")));

    // Effects: two layers merge, transparency is lost in JPEG, JPEG is lossy; PSD keeps all.
    doc::Document doc(QSize(40, 30));
    doc.addLayer(QStringLiteral("Bottom"), filled(QSize(40, 30), Qt::white));
    QImage half(40, 30, QImage::Format_ARGB32_Premultiplied);
    half.fill(Qt::transparent);
    doc.addLayer(QStringLiteral("Top"), half);
    const doc::SaveEffects png = doc::saveEffects(doc.snapshot(), doc.size(), *doc::imageFormatById("png"));
    QVERIFY(png.mergesLayers && !png.dropsTransparency && !png.lossy);
    QVERIFY(!doc::saveEffects(doc.snapshot(), doc.size(), *doc::imageFormatById("psd")).any());
    doc::Document clear(QSize(40, 30));
    clear.addLayer(QStringLiteral("Only"), half);
    const doc::SaveEffects jpeg = doc::saveEffects(clear.snapshot(), clear.size(), *doc::imageFormatById("jpeg"));
    QVERIFY(!jpeg.mergesLayers && jpeg.dropsTransparency && jpeg.lossy);

    // The quality of JPEG files Qt wrote, told back from their tables.
    for (const int quality : {30, 55, 70, 85, 92, 100})
        QCOMPARE(doc::estimateJpegQuality(img::encode(photo(), "JPEG", quality)), quality);
    QCOMPARE(doc::estimateJpegQuality(img::encode(photo(), "PNG")), -1);
    QCOMPARE(doc::estimateJpegQuality(QByteArray("not a picture")), -1);

    // A failed write leaves the file as it was.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("kept.png"));
    QVERIFY(img::encode(photo(), "PNG").size() > 0);
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(img::encode(photo(), "PNG"));
    f.close();
    const QByteArray before = bytesOf(path);
    QVERIFY_THROWS_EXCEPTION(lumen::Error, doc::writeImageFile(dir.filePath(QStringLiteral("missing/folder/x.png")),
                                                               *doc::imageFormatById("png"), doc.snapshot(), doc.size()));
    QCOMPARE(bytesOf(path), before);
}

// A picture opened from disk remembers its file: the tab is marked while changes are not in it,
// Save writes it back in its own format (asking first when that merges layers), Undo back to the
// saved state clears the mark, and the link survives the project.
void TestFiles::saveOverOriginal()
{
    Fixture f;
    QVERIFY(f.setUp());
    const QString path = f.path(QStringLiteral("photo.png"));
    QVERIFY(photo().save(path));
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    QCOMPARE(doc->file().path, QDir::cleanPath(path));
    QCOMPARE(doc->file().format, QByteArray("png"));
    QVERIFY(!doc->isFileModified());
    QCOMPARE(f.tabs()->tabText(0), QStringLiteral("photo"));
    QVERIFY(f.tabs()->tabToolTip(0).contains(QDir::toNativeSeparators(QDir::cleanPath(path))));

    // A change: the tab says so.
    paintSquare(doc);
    QVERIFY(doc->isFileModified());
    QCOMPARE(f.tabs()->tabText(0), QStringLiteral("photo*"));
    QVERIFY(f.tabs()->tabToolTip(0).contains(QStringLiteral("File → Save")));
    // One layer in a PNG: nothing to ask, the file is the picture now.
    QAction *save = f.window->findChild<QAction *>(QStringLiteral("app-save"));
    QVERIFY(save && save->isEnabled());
    QCOMPARE(save->shortcut(), QKeySequence(QKeySequence::Save));
    save->trigger();
    QTRY_VERIFY(!doc->isFileModified());
    QVERIFY(f.saveOverQuestions.isEmpty());
    QCOMPARE(readImage(path), doc->flattened());
    QCOMPARE(f.tabs()->tabText(0), QStringLiteral("photo"));
    // Undo is a change again; Redo back to what the file has clears it.
    doc->undoStack()->undo();
    QVERIFY(doc->isFileModified());
    doc->undoStack()->redo();
    QVERIFY(!doc->isFileModified());

    // A second layer: the file would get the layers merged, so the first Save asks. Cancel
    // changes nothing; Save over writes the merged picture.
    doc->addLayer(QStringLiteral("Note"), filled(QSize(20, 10), QColor(0, 200, 0)));
    const QByteArray before = bytesOf(path);
    f.saveOverReply = {SaveOverReply::Cancel, false};
    QVERIFY(!waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 1);
    QCOMPARE(f.saveOverQuestions.first().fileName, QStringLiteral("photo.png"));
    QCOMPARE(f.saveOverQuestions.first().reasons, QStringList{QStringLiteral("The layers are merged in the file; your project keeps them.")});
    QCOMPARE(bytesOf(path), before);
    QVERIFY(doc->isFileModified());
    f.saveOverReply = {SaveOverReply::SaveOver, false};
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 2);
    QCOMPARE(readImage(path), doc->flattened());
    QCOMPARE(doc->layerCount(), 2); // the document keeps them
    QVERIFY(!doc->isFileModified());
    // Asked once per document: the next Save writes at once.
    paintSquare(doc, QRect(50, 50, 10, 10));
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 2);
    QCOMPARE(readImage(path), doc->flattened());

    // The question's own box: Save over, Save as…, Cancel and "Don't ask again for this picture".
    {
        std::unique_ptr<QMessageBox> box(saveOverBox({QStringLiteral("photo.jpg"), {QStringLiteral("The layers are merged in the file; your project keeps them.")}},
                                                     f.window.get()));
        QCOMPARE(box->text(), QStringLiteral("Save over photo.jpg?"));
        QVERIFY(box->informativeText().contains(QStringLiteral("your project keeps them")));
        QVERIFY(box->findChild<QPushButton *>(QStringLiteral("saveOver")));
        QCOMPARE(box->findChild<QPushButton *>(QStringLiteral("saveAs"))->text(), QStringLiteral("Save as…"));
        QVERIFY(box->button(QMessageBox::Cancel));
        QCOMPARE(box->checkBox()->text(), QStringLiteral("Don't ask again for this picture"));
    }

    // The project keeps the link: closed and opened again from the project, the document saves to
    // the same file and knows whether it has changes that are not in it.
    paintSquare(doc, QRect(80, 80, 10, 10));
    const QString id = doc->id();
    QVERIFY(waitFor(f.studio.saveAll().then([] { return true; })));
    const QString docId = doc->id();
    QVERIFY(!docId.isEmpty());
    f.unsavedReply = QMessageBox::Discard;
    QVERIFY(waitFor(f.files()->close(doc)));
    QCOMPARE(f.unsavedQuestions.size(), 1);
    QTRY_VERIFY(f.studio.documents().isEmpty());
    const auto info = std::find_if(f.studio.project()->documents().cbegin(), f.studio.project()->documents().cend(),
                                   [&](const doc::DocumentInfo &d) { return d.id == docId; });
    QVERIFY(info != f.studio.project()->documents().cend());
    QCOMPARE(info->file.path, QDir::cleanPath(path));
    QVERIFY(info->fileModified);
    f.studio.openStoredDocument(docId);
    QTRY_COMPARE(f.studio.documents().size(), 1);
    doc::Document *again = f.studio.documents().first();
    QCOMPARE(again->file().path, QDir::cleanPath(path));
    QVERIFY(again->isFileModified());
    QCOMPARE(f.tabs()->tabText(0), QStringLiteral("photo*"));
    Q_UNUSED(id);
}

// A JPEG keeps the quality it had; transparency is said to go; "Don't ask again for this picture"
// holds in a later session too.
void TestFiles::saveOverJpeg()
{
    Fixture f;
    QVERIFY(f.setUp());
    const QString path = f.path(QStringLiteral("holiday.jpg"));
    {
        // Closed again before Lumen writes over it (a writer keeps its file open).
        QImageWriter writer(path, "jpeg");
        writer.setQuality(70);
        QVERIFY(writer.write(photo(QSize(320, 240)).convertToFormat(QImage::Format_RGB32)));
    }
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    QCOMPARE(doc->file().format, QByteArray("jpeg"));
    QCOMPARE(doc->file().quality, 70);
    paintSquare(doc);
    f.saveOverReply = {SaveOverReply::SaveOver, true};
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 1);
    QCOMPARE(f.saveOverQuestions.first().reasons,
             QStringList{QStringLiteral("JPEG loses a little quality each time it is saved; Lumen keeps the quality the file had (70 of 100).")});
    QCOMPARE(doc::estimateJpegQuality(path), 70);
    QVERIFY(doc->file().dontAsk);
    QVERIFY(!doc->isFileModified());

    // Erased to transparent: JPEG has none, the question would say so, but it is not asked again.
    doc->applyPixels(doc->activeLayer()->id, QRect(0, 0, 40, 40), filled(QSize(40, 40), Qt::transparent), QStringLiteral("Erase"));
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 1);
    QCOMPARE(QColor(readImage(path).pixel(20, 20)).lightness() > 240, true); // white, as on a page
    QCOMPARE(doc::estimateJpegQuality(path), 70);

    // Another session (a new DocumentFiles) still does not ask: the document remembers.
    DocumentFiles later(&f.studio, f.window.get());
    int asked = 0;
    later.askSaveOver = [&asked](const SaveOverQuestion &) {
        ++asked;
        return SaveOverReply{SaveOverReply::Cancel, false};
    };
    paintSquare(doc, QRect(100, 100, 20, 20));
    QVERIFY(waitFor(later.save(doc)));
    QCOMPARE(asked, 0);
    // Without "don't ask", a new session asks the first time again, and says what goes.
    doc::FileLink link = doc->file();
    link.dontAsk = false;
    doc->updateFile(link);
    DocumentFiles third(&f.studio, f.window.get());
    QStringList reasons;
    third.askSaveOver = [&reasons](const SaveOverQuestion &q) {
        reasons = q.reasons;
        return SaveOverReply{SaveOverReply::Cancel, false};
    };
    paintSquare(doc, QRect(120, 100, 20, 20));
    QVERIFY(!waitFor(third.save(doc)));
    QVERIFY2(reasons.contains(QStringLiteral("JPEG has no transparency: the transparent parts become white.")), qPrintable(reasons.join(u'|')));
}

// Save as… into every format Lumen writes here: the document saves there from then on, takes the
// file's name and lists it in File → Open recent; a new picture's Save asks for a file; Export a
// copy… leaves the document's file alone; a GIF goes to Save as….
void TestFiles::saveAsFormats()
{
    Fixture f;
    QVERIFY(f.setUp());
    doc::Document *doc = f.studio.newDocument(QSize(64, 48), QStringLiteral("New: a picture?"), Qt::white);
    doc->addLayer(QStringLiteral("Corner"), filled(QSize(16, 16), QColor(0, 0, 200)));
    QVERIFY(!doc->hasFile());
    QVERIFY(!doc->isFileModified()); // no file: no mark
    QCOMPARE(f.tabs()->tabText(0), QStringLiteral("New: a picture?"));
    // Save without a file is Save as…; the suggested name leaves out what a file name cannot have.
    f.pathReply.clear();
    QVERIFY(!waitFor(f.files()->save(doc)));
    QCOMPARE(f.pathQuestions.size(), 1);
    QCOMPARE(QFileInfo(f.pathQuestions.first()).fileName(), QStringLiteral("New a picture.png"));

    for (const doc::ImageFormat &format : doc::writableImageFormats()) {
        const QString path = f.path(QStringLiteral("saved-%1.%2").arg(QString::fromLatin1(format.id), format.suffixes.first()));
        QVERIFY2(waitFor(f.files()->saveAs(doc, path)), format.id.constData());
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(doc->file().path, QDir::cleanPath(path));
        QCOMPARE(doc->file().format, format.id);
        QCOMPARE(doc->name(), QFileInfo(path).completeBaseName());
        QCOMPARE(Settings::instance().recentFiles().first(), QDir::cleanPath(path));
        QVERIFY(!doc->isFileModified());
        if (format.layers) {
            const std::optional<psd::File> psd = psd::read(path);
            QVERIFY(psd);
            QCOMPARE(int(psd->layers.size()), 2);
        } else {
            const QImage read = readImage(path);
            QCOMPARE(read.size(), doc->size());
            QCOMPARE(QColor(read.pixel(8, 8)).blue() > 150, true); // the top layer, merged
            QCOMPARE(QColor(read.pixel(40, 40)).red() > 240, true);
        }
    }
    QVERIFY(f.saveOverQuestions.isEmpty()); // a format chosen in Save as… is not asked about
    // Save as… with the dialog: a name without a suffix gets the chosen format's, at first the one
    // the picture has.
    QVERIFY(waitFor(f.files()->saveAs(doc, f.path(QStringLiteral("again.psd")))));
    f.pathReply = f.path(QStringLiteral("from-dialog"));
    QVERIFY(waitFor(f.files()->saveAs(doc)));
    QCOMPARE(QFileInfo(f.pathQuestions.last()).fileName(), QStringLiteral("again.psd"));
    const QString fromDialog = QDir::cleanPath(f.path(QStringLiteral("from-dialog.psd")));
    QCOMPARE(doc->file().path, fromDialog);
    // A copy: the document keeps saving to its own file.
    const QString copy = f.path(QStringLiteral("copy.png"));
    QVERIFY(waitFor(f.files()->exportCopy(doc, copy)));
    QVERIFY(QFileInfo::exists(copy));
    QCOMPARE(doc->file().path, fromDialog);
    QCOMPARE(readImage(copy), doc->flattened());
    // One set of commands: Save, Save as… (Ctrl+Shift+S), Export a copy…; Save a copy to project
    // images lost Ctrl+Shift+S but kept its shortcut id.
    QCOMPARE(Shortcuts::instance().keys(QStringLiteral("app.save-as")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+S"))});
    QVERIFY(Shortcuts::instance().contains(QStringLiteral("app.save-to-library")));
    QVERIFY(Shortcuts::instance().keys(QStringLiteral("app.save-to-library")).isEmpty());
    QVERIFY(Shortcuts::instance().keys(QStringLiteral("app.save-all")).isEmpty());
    // Quit has a key on Windows too (QKeySequence::Quit has none there).
    QCOMPARE(Shortcuts::instance().keys(QStringLiteral("app.quit")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Q"))});
    QCOMPARE(f.window->findChild<QAction *>(QStringLiteral("app-export-image"))->text(), QStringLiteral("Export a copy…"));

    // A GIF opens, but Lumen cannot write it: Save asks for another file.
    const QString gif = f.path(QStringLiteral("tiny.gif"));
    QFile g(gif);
    QVERIFY(g.open(QIODevice::WriteOnly));
    g.write(tinyGif());
    g.close();
    doc::Document *tiny = f.open(gif);
    QVERIFY(tiny);
    QCOMPARE(tiny->file().format, QByteArray("gif"));
    paintSquare(tiny, QRect(0, 0, 1, 1));
    const qsizetype asked = f.pathQuestions.size();
    f.pathReply = f.path(QStringLiteral("tiny.png"));
    QVERIFY(waitFor(f.files()->save(tiny)));
    QCOMPARE(f.pathQuestions.size(), asked + 1);
    QCOMPARE(tiny->file().format, QByteArray("png"));
    QCOMPARE(bytesOf(gif), tinyGif());
}

// Closing a picture with changes not saved to its file asks Save / Don't save / Cancel (also its
// tab's ✕); quitting asks once for all of them. Pictures without a file close as before.
void TestFiles::closeAndQuitAsk()
{
    Fixture f;
    QVERIFY(f.setUp());
    const QString path = f.path(QStringLiteral("cat.png"));
    QVERIFY(photo().save(path));
    const QByteArray original = bytesOf(path);
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    // Not changed: closes without a question.
    QVERIFY(waitFor(f.files()->close(doc)));
    QVERIFY(f.unsavedQuestions.isEmpty());
    QTRY_VERIFY(f.studio.documents().isEmpty());

    doc = f.open(path);
    QVERIFY(doc);
    paintSquare(doc);
    // The tab's ✕: Cancel keeps it open.
    f.unsavedReply = QMessageBox::Cancel;
    emit f.tabs()->tabCloseRequested(0);
    QCOMPARE(f.unsavedQuestions.size(), 1);
    QCOMPARE(f.unsavedQuestions.first(), QStringList{QStringLiteral("cat.png")});
    QCOMPARE(f.studio.documents().size(), 1);
    // Don't save: closed, the file as it was.
    f.unsavedReply = QMessageBox::Discard;
    QAction *close = f.window->findChild<QAction *>(QStringLiteral("app-close-document"));
    QVERIFY(close);
    close->trigger();
    QTRY_VERIFY(f.studio.documents().isEmpty());
    QCOMPARE(bytesOf(path), original);
    // Opened again, it is the file (the project's copy has the changes that were not saved).
    doc = f.open(path);
    QVERIFY(doc);
    QCOMPARE(doc->flattened(), readImage(path));
    QVERIFY(!doc->isFileModified());
    // Save: written, then closed.
    paintSquare(doc);
    f.unsavedReply = QMessageBox::Save;
    QVERIFY(waitFor(f.files()->close(doc)));
    QTRY_VERIFY(f.studio.documents().isEmpty());
    QVERIFY(bytesOf(path) != original);

    // A picture without a file closes as before: the project keeps it.
    const qsizetype questions = f.unsavedQuestions.size();
    doc::Document *blank = f.studio.newDocument(QSize(32, 32), QStringLiteral("Blank"));
    paintSquare(blank, QRect(0, 0, 8, 8));
    QVERIFY(waitFor(f.files()->close(blank)));
    QCOMPARE(f.unsavedQuestions.size(), questions);

    // Quitting with two changed pictures: one question; Cancel stays, Save writes both and quits.
    const QString other = f.path(QStringLiteral("dog.png"));
    QVERIFY(photo(QSize(90, 60)).save(other));
    doc::Document *a = f.open(path);
    doc::Document *b = f.open(other);
    QVERIFY(a && b);
    paintSquare(a, QRect(30, 30, 10, 10));
    paintSquare(b, QRect(30, 30, 10, 10));
    f.unsavedReply = QMessageBox::Cancel;
    f.window->close();
    QTest::qWait(100);
    QVERIFY(f.window->isVisible());
    QCOMPARE(f.unsavedQuestions.last(), (QStringList{QStringLiteral("cat.png"), QStringLiteral("dog.png")}));
    f.unsavedReply = QMessageBox::Save;
    f.window->close();
    QTRY_VERIFY_WITH_TIMEOUT(!f.window->isVisible(), 10000);
    QCOMPARE(readImage(path), a->flattened());
    QCOMPARE(readImage(other), b->flattened());
    // The boxes say it in plain words.
    std::unique_ptr<QMessageBox> box(unsavedChangesBox({QStringLiteral("cat.png"), QStringLiteral("dog.png")}, true, nullptr));
    QVERIFY(box->text().startsWith(QStringLiteral("Save the changes to 2 picture"))); // the plural form comes with the app's translation
    QVERIFY(box->informativeText().contains(QStringLiteral("dog.png")));
    QCOMPARE(box->button(QMessageBox::Discard)->text(), QStringLiteral("Don't save"));
}

// A file another program changed since Lumen read or wrote it is never saved over without asking,
// also with "Don't ask again" and after the first question; a Save that finishes after a later
// Save as… leaves the document saving to the file chosen last.
void TestFiles::saveOverChangedFile()
{
    Fixture f;
    QVERIFY(f.setUp());
    const QString path = f.path(QStringLiteral("shared.png"));
    QVERIFY(photo().save(path));
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    paintSquare(doc);
    QVERIFY(waitFor(f.files()->save(doc))); // one layer in a PNG: not asked
    QVERIFY(f.saveOverQuestions.isEmpty());
    doc::FileLink link = doc->file();
    link.dontAsk = true;
    doc->updateFile(link);

    // Another program writes the file; then Lumen's Save asks, and Cancel keeps their picture.
    QTest::qWait(20);
    QVERIFY(filled(QSize(30, 30), Qt::green).save(path));
    const QByteArray theirs = bytesOf(path);
    paintSquare(doc, QRect(40, 40, 10, 10));
    f.saveOverReply = {SaveOverReply::Cancel, false};
    QVERIFY(!waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 1);
    QCOMPARE(f.saveOverQuestions.first().reasons.value(0),
             QStringLiteral("Another program changed shared.png since Lumen opened or saved it; saving over it loses those changes."));
    QCOMPARE(bytesOf(path), theirs);
    // Save over: written, and the next Save does not ask again (the file is Lumen's again).
    f.saveOverReply = {SaveOverReply::SaveOver, false};
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(readImage(path), doc->flattened());
    paintSquare(doc, QRect(60, 60, 10, 10));
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 2);

    // Ctrl+S and at once Save as… another file: whichever finishes last, the document saves to the
    // file chosen last, and that one is not marked as changed.
    doc->addLayer(QStringLiteral("Big"), photo(doc->size()));
    paintSquare(doc, QRect(0, 0, 5, 5));
    const QString other = f.path(QStringLiteral("other.jpg"));
    QFuture<bool> saving = f.files()->save(doc);
    QFuture<bool> savingAs = f.files()->saveAs(doc, other);
    QVERIFY(waitFor(savingAs));
    waitFor(saving);
    QCOMPARE(doc->file().path, QDir::cleanPath(other));
    QCOMPARE(doc->file().format, QByteArray("jpeg"));
    QVERIFY(!doc->isFileModified());
}

// Export a copy… does not suggest the picture's own file, and Save as… or Export a copy… onto it is
// File → Save: asked about what saving over it changes.
void TestFiles::notACopyOfItself()
{
    Fixture f;
    QVERIFY(f.setUp());
    const QString path = f.path(QStringLiteral("photo.png"));
    QVERIFY(photo().save(path));
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    doc->addLayer(QStringLiteral("Note"), filled(QSize(20, 10), QColor(0, 200, 0)));
    f.pathReply.clear();
    QVERIFY(!waitFor(f.files()->exportCopy(doc)));
    QCOMPARE(QFileInfo(f.pathQuestions.last()).fileName(), QStringLiteral("photo copy.png"));
    QVERIFY(!waitFor(f.files()->saveAs(doc)));
    QCOMPARE(QFileInfo(f.pathQuestions.last()).fileName(), QStringLiteral("photo.png")); // as other editors do

    const QByteArray before = bytesOf(path);
    f.saveOverReply = {SaveOverReply::Cancel, false};
    QVERIFY(!waitFor(f.files()->exportCopy(doc, path)));
    QCOMPARE(f.saveOverQuestions.size(), 1);
    QVERIFY(f.saveOverQuestions.last().reasons.contains(QStringLiteral("The layers are merged in the file; your project keeps them.")));
    f.pathReply = path; // chosen in the dialog
    QVERIFY(!waitFor(f.files()->saveAs(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 2);
    QCOMPARE(bytesOf(path), before);
    f.saveOverReply = {SaveOverReply::SaveOver, false};
    QVERIFY(waitFor(f.files()->exportCopy(doc, path)));
    QCOMPARE(readImage(path), doc->flattened());
    QVERIFY(!doc->isFileModified()); // the document's own file, so it is saved
    QVERIFY(doc->file().isUnchangedOnDisk());
}

// A layered Photoshop file is saved over with its groups; what Lumen does not keep of it (text,
// effects and adjustment layers, the groups' own opacity, a colour profile) is said first.
void TestFiles::photoshopFiles()
{
    Fixture f;
    QVERIFY(f.setUp());
    psd::File file;
    file.size = QSize(40, 30);
    QImage base(40, 30, QImage::Format_ARGB32);
    base.fill(Qt::white);
    QImage star(8, 8, QImage::Format_ARGB32);
    star.fill(Qt::blue);
    file.layers = {psd::Layer::groupBegin(QStringLiteral("Folder")), psd::Layer::pixels(QStringLiteral("Star"), star, QPoint(4, 4)),
                   psd::Layer::groupEnd(), psd::Layer::pixels(QStringLiteral("Base"), base)};
    const QString path = f.path(QStringLiteral("layers.psd"));
    QVERIFY(psd::write(path, file));
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    QCOMPARE(doc->layerCount(), 2);
    QVERIFY(doc->file().losses.isEmpty());
    paintSquare(doc, QRect(20, 20, 5, 5));
    QVERIFY(waitFor(f.files()->save(doc)));
    QVERIFY(f.saveOverQuestions.isEmpty()); // nothing is lost
    const std::optional<psd::File> back = psd::read(path);
    QVERIFY(back);
    QStringList kinds;
    for (const psd::Layer &l : back->layers)
        kinds << (l.kind == psd::Layer::Kind::GroupBegin ? QStringLiteral("[") + l.name
                  : l.kind == psd::Layer::Kind::GroupEnd ? QStringLiteral("]")
                                                         : l.name);
    QCOMPARE(kinds, (QStringList{QStringLiteral("[Folder"), QStringLiteral("Star"), QStringLiteral("]"), QStringLiteral("Base")}));

    // Photoshop's text layer, in a file with a wide-gamut colour profile: Lumen reads both, and
    // Save says what goes before it writes over the file.
    psd::File single;
    single.size = QSize(40, 30);
    single.layers = {psd::Layer::pixels(QStringLiteral("Title"), star, QPoint(2, 2))};
    const QString text = f.path(QStringLiteral("text.psd"));
    writeBytes(text, photoshopPsd(single, "TySh", QColorSpace(QColorSpace::DisplayP3).iccProfile()));
    const std::optional<psd::File> read = psd::read(text);
    QVERIFY(read);
    QCOMPARE(read->layers.first().otherInfo, QList<QByteArray>{"TySh"});
    QCOMPARE(read->layers.first().name, QStringLiteral("Title"));
    QVERIFY(!read->iccProfile.isEmpty());
    doc::Document *titled = f.open(text);
    QVERIFY(titled);
    paintSquare(titled, QRect(20, 20, 5, 5));
    const QByteArray before = bytesOf(text);
    f.saveOverReply = {SaveOverReply::Cancel, false};
    QVERIFY(!waitFor(f.files()->save(titled)));
    QCOMPARE(f.saveOverQuestions.size(), 1);
    const QString reasons = f.saveOverQuestions.first().reasons.join(u'|');
    QVERIFY2(reasons.contains(QStringLiteral("1 text layer(s) became pictures of their text")), qPrintable(reasons));
    QVERIFY2(reasons.contains(QStringLiteral("its colour profile is not kept")), qPrintable(reasons));
    QCOMPARE(bytesOf(text), before);

    // The rest of what Lumen does not keep, told from the file; an sRGB profile changes nothing.
    psd::File other = single;
    other.layers.first().otherInfo = {"lfx2", "levl", "SoLd"};
    psd::Layer half = psd::Layer::groupBegin(QStringLiteral("Faded"));
    half.opacity = 128;
    other.layers.prepend(half);
    other.layers.append(psd::Layer::groupEnd());
    other.iccProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
    QCOMPARE(doc::importPsd(other).warnings,
             (QStringList{QStringLiteral("the effects of 1 layer(s) (shadows, strokes, glows) are not kept"),
                          QStringLiteral("1 adjustment or fill layer(s) are empty layers here"),
                          QStringLiteral("1 smart object(s) became plain layers"),
                          QStringLiteral("the opacity or hiding of 1 group(s) went onto their layers")}));
}

// A photo in colours of its own (Display P3) is turned into standard colours when it opens, and
// saving over it says the profile goes; so do the camera's details of a JPEG.
void TestFiles::coloursAndCameraDetails()
{
    Fixture f;
    QVERIFY(f.setUp());
    QImage wide = photo().convertToFormat(QImage::Format_RGB32);
    wide.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QString path = f.path(QStringLiteral("phone.png"));
    QVERIFY(wide.save(path));
    QVERIFY(QImageReader(path).read().colorSpace().isValid());
    doc::Document *doc = f.open(path);
    QVERIFY(doc);
    // A P3 red is redder than sRGB's: in sRGB numbers it has no green or blue left.
    const QImage shown = doc->flattened();
    const QColor converted = shown.pixelColor(30, 0);
    QVERIFY2(converted != QColor(wide.pixel(30, 0)), qPrintable(converted.name()));
    QCOMPARE(doc->file().losses.size(), 1);
    QVERIFY(doc->file().losses.first().contains(QStringLiteral("standard colours (sRGB)")));
    paintSquare(doc);
    f.saveOverReply = {SaveOverReply::SaveOver, false};
    QVERIFY(waitFor(f.files()->save(doc)));
    QCOMPARE(f.saveOverQuestions.size(), 1); // one layer in a PNG, still asked: the profile goes
    QVERIFY(f.saveOverQuestions.first().reasons.first().contains(QStringLiteral("standard colours")));
    QVERIFY(!QImageReader(path).read().colorSpace().isValid() || QImageReader(path).read().colorSpace() == QColorSpace(QColorSpace::SRgb));
    QVERIFY(doc->file().losses.isEmpty()); // the file is the picture now

    // An sRGB picture is left alone.
    QImage plain = photo().convertToFormat(QImage::Format_RGB32);
    plain.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const QString srgb = f.path(QStringLiteral("plain.png"));
    QVERIFY(plain.save(srgb));
    doc::Document *same = f.open(srgb);
    QVERIFY(same);
    QVERIFY(same->file().losses.isEmpty());
    QCOMPARE(same->flattened().pixel(30, 0), photo().pixel(30, 0));

    // A JPEG from a camera: its Exif segment right after the start.
    QByteArray jpeg = img::encode(photo(), "JPEG", 90);
    QVERIFY(!doc::hasCameraDetails(jpeg));
    const QByteArray tiff("II*\0\x08\0\0\0\0\0", 10); // an empty Exif directory
    QByteArray app1("\xFF\xE1", 2);
    QByteArray length(2, Qt::Uninitialized);
    qToBigEndian(quint16(2 + 6 + tiff.size()), length.data());
    app1 += length + QByteArray("Exif\0\0", 6) + tiff;
    jpeg.insert(2, app1);
    QVERIFY(doc::hasCameraDetails(jpeg));
    QVERIFY(!doc::hasCameraDetails(img::encode(photo(), "PNG")));
    const QString camera = f.path(QStringLiteral("camera.jpg"));
    writeBytes(camera, jpeg);
    doc::Document *shot = f.open(camera);
    QVERIFY(shot);
    QVERIFY(shot->file().losses.contains(QStringLiteral("The photo's details from the camera (date, place, camera) are not kept.")));
}

// File → Open recent lists the pictures opened or saved as, a missing one greyed; the start screen
// shows those that are there, with their thumbnails; a picture the project keeps unchanged opens
// with its layers, one changed by another program from the file.
void TestFiles::recentFiles()
{
    Fixture f;
    QVERIFY(f.setUp());
    const QString one = f.path(QStringLiteral("one.png"));
    const QString two = f.path(QStringLiteral("two.png"));
    QVERIFY(photo().save(one));
    QVERIFY(photo(QSize(80, 80)).save(two));
    doc::Document *first = f.open(one);
    QVERIFY(first && f.open(two));
    QCOMPARE(Settings::instance().recentFiles(), (QStringList{QDir::cleanPath(two), QDir::cleanPath(one)}));
    // Opened again: shown, not opened twice.
    f.studio.openFile(one);
    QCOMPARE(f.studio.documents().size(), 2);
    QCOMPARE(f.studio.activeDocument(), first);
    QCOMPARE(Settings::instance().recentFiles().first(), QDir::cleanPath(one));

    // A layer, saved over: the project keeps the document with it.
    first->addLayer(QStringLiteral("Sticker"), filled(QSize(10, 10), QColor(0, 160, 0)));
    QVERIFY(waitFor(f.files()->save(first)));
    QVERIFY(waitFor(f.studio.saveAll().then([] { return true; })));
    const QString firstId = first->id();
    QVERIFY(QFile::remove(two));
    QVERIFY(waitFor(f.files()->close(first)));
    QVERIFY(waitFor(f.files()->close(f.studio.documents().first())));
    QTRY_VERIFY(f.studio.documents().isEmpty());

    // The menu, with the missing file greyed.
    QMenu *recent = f.window->findChild<QMenu *>(QStringLiteral("openRecent"));
    QVERIFY(recent);
    emit recent->aboutToShow();
    QList<QAction *> items;
    for (QAction *a : recent->actions()) {
        if (!a->isSeparator())
            items << a;
    }
    QCOMPARE(items.size(), 3); // one, two, Clear the list
    QCOMPARE(items[0]->text(), QStringLiteral("one.png"));
    QVERIFY(items[0]->isEnabled());
    QCOMPARE(items[1]->text(), QStringLiteral("two.png"));
    QVERIFY(!items[1]->isEnabled());
    QVERIFY(items[1]->toolTip().contains(QStringLiteral("not there any more")));
    QCOMPARE(items[2]->text(), QStringLiteral("Clear the list"));

    // The start screen: only the file that is there, with its thumbnail; the document that is just
    // that file is not listed twice.
    StartScreen *start = f.window->editor()->startScreen();
    QTRY_VERIFY(start->isVisible());
    QTRY_COMPARE(start->recentFileTiles().size(), 1);
    QToolButton *tile = start->recentFileTiles().first();
    QCOMPARE(tile->accessibleName(), QStringLiteral("one.png"));
    QTRY_VERIFY(start->recentFileTiles().first()->icon().pixmap(QSize(112, 112)).toImage().pixelColor(56, 56).alpha() > 0);
    for (QToolButton *t : start->recentTiles())
        QVERIFY(t->property("documentId").toString() != firstId);
    // A click opens the project's document of that file, with its layer.
    start->recentFileTiles().first()->click();
    QTRY_COMPARE(f.studio.documents().size(), 1);
    doc::Document *reopened = f.studio.documents().first();
    QCOMPARE(reopened->id(), firstId);
    QCOMPARE(reopened->layerCount(), 2);
    QVERIFY(waitFor(f.files()->close(reopened)));
    QTRY_VERIFY(f.studio.documents().isEmpty());
    // Changed by another program: the file, not the old document.
    QTest::qWait(20);
    QVERIFY(filled(QSize(50, 40), Qt::black).save(one));
    f.studio.openFile(one);
    QTRY_COMPARE(f.studio.documents().size(), 1);
    QVERIFY(f.studio.documents().first()->id() != firstId);
    QCOMPARE(f.studio.documents().first()->layerCount(), 1);
    QCOMPARE(f.studio.documents().first()->size(), QSize(50, 40));
    QVERIFY(waitFor(f.files()->close(f.studio.documents().first())));
    QTRY_VERIFY(start->isVisible());
    // A file deleted while the start screen shows it: a click says so and takes it off the list.
    const QString three = f.path(QStringLiteral("three.png"));
    QVERIFY(photo(QSize(40, 40)).save(three));
    Settings::instance().addRecentFile(three);
    QTRY_COMPARE(start->recentFileTiles().size(), 2);
    QVERIFY(QFile::remove(three));
    start->recentFileTiles().first()->click();
    QVERIFY(!Settings::instance().recentFiles().contains(QDir::cleanPath(three)));
    QTRY_COMPARE(start->recentFileTiles().size(), 1);
    QVERIFY(f.studio.documents().isEmpty());

    // A long name keeps its end and its extension on the tile (how much of it depends on the fonts).
    const QString longName = f.path(QStringLiteral("A picture from disk with a long name that goes on 7.png"));
    QVERIFY(photo(QSize(40, 40)).save(longName));
    Settings::instance().addRecentFile(longName);
    QTRY_COMPARE(start->recentFileTiles().size(), 2);
    const QString caption = start->recentFileTiles().first()->text();
    QVERIFY2(caption.endsWith(QStringLiteral("png")) && caption.contains(QChar(0x2026)), qPrintable(caption));

    // Clear the list.
    emit recent->aboutToShow();
    for (QAction *a : recent->actions()) {
        if (a->text() == QLatin1String("Clear the list"))
            a->trigger();
    }
    QVERIFY(Settings::instance().recentFiles().isEmpty());
}

// The main window's app-wide keys work while a plugin window or the filter editor is active; the
// filter editor's own Ctrl+S and Ctrl+Z stay its own, also when a customised key collides.
void TestFiles::shortcutsInOtherWindows()
{
    PluginsOn plugins({QStringLiteral("poses")});
    Fixture f;
    QVERIFY(f.setUp());
    auto cleanup = qScopeGuard([] { Shortcuts::instance().reset(QStringLiteral("app.job-queue")); });
    MainWindow &window = *f.window;
    QAction *queue = window.findChild<QAction *>(QStringLiteral("jobQueue"));
    QVERIFY(queue);
    window.showPage(QStringLiteral("poses"));
    QWidget *poses = window.pageWindow(QStringLiteral("poses"));
    QVERIFY(poses);
    QVERIFY(QTest::qWaitForWindowExposed(poses));
    poses->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), poses);
    QTest::keyClick(poses, Qt::Key_Q, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(queue->isChecked());
    QTest::keyClick(poses, Qt::Key_Q, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(!queue->isChecked());
    // New from prompt (Alt+2) from the plugin window.
    QVERIFY(!window.newFromPromptDialog());
    poses->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), poses);
    QTest::keyClick(poses, Qt::Key_2, Qt::AltModifier);
    QTRY_VERIFY(window.newFromPromptDialog());
    window.newFromPromptDialog()->close();
    // A customised key works there too.
    Shortcuts::instance().setKeys(QStringLiteral("app.job-queue"), {QKeySequence(QStringLiteral("Ctrl+Alt+J"))});
    poses->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), poses);
    QTest::keyClick(poses, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    QTRY_VERIFY(queue->isChecked());
    queue->setChecked(false);

    // The filter editor: the Queue's key works; its own Ctrl+Z undoes its step and Ctrl+S saves the
    // filter, not the picture.
    doc::Document *doc = f.studio.newDocument(QSize(64, 48), QStringLiteral("Picture"));
    paintSquare(doc);
    const QString filter = window.editor()->filtersPanel()->newFilter(QStringLiteral("Keys test"));
    QVERIFY(!filter.isEmpty());
    QTRY_VERIFY(!NodeEditorWindow::openWindows().isEmpty());
    NodeEditorWindow *editor = NodeEditorWindow::openWindows().last();
    QVERIFY(QTest::qWaitForWindowExposed(editor));
    editor->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), editor);
    QTest::keyClick(editor, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    QTRY_VERIFY(queue->isChecked());
    queue->setChecked(false);
    const QString node = editor->addNode(QStringLiteral("invert"));
    QVERIFY(!node.isEmpty());
    QVERIFY(editor->isModified());
    const int steps = editor->model()->undoStack()->index();
    // A key the editor uses itself, given to the Queue: the editor keeps it.
    Shortcuts::instance().setKeys(QStringLiteral("app.job-queue"), {QKeySequence(QStringLiteral("Ctrl+Z"))});
    editor->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), editor);
    QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE(editor->model()->undoStack()->index(), steps - 1);
    QVERIFY(!queue->isChecked());
    QTest::keyClick(editor, Qt::Key_S, Qt::ControlModifier);
    QTRY_VERIFY(!editor->isModified()); // the filter was saved
    QVERIFY(f.pathQuestions.isEmpty());  // and the picture was not
    QVERIFY(doc->isModified());
    // In the main window the customised key is the Queue's (the editor's undo has its own there).
    editor->close();
}

// Closing a document while its prompt-bar run goes cancels that run's jobs; their results go
// nowhere, and the next document's run and results are not touched.
void TestFiles::closingStopsItsRun()
{
    MockSdCpp sd;
    Fixture f;
    for (ai::Provider *p : f.studio.providers()->providers())
        f.studio.providers()->removeProvider(p->instanceId());
    auto cleanup = qScopeGuard([&] {
        for (ai::Provider *p : f.studio.providers()->providers())
            f.studio.providers()->removeProvider(p->instanceId());
        Settings::instance().remove(QStringLiteral("editor/ai"));
    });
    Settings::instance().remove(QStringLiteral("editor/ai"));
    QVariantMap settings{{QStringLiteral("url"), sd.server.baseUrl().toString()},
                         {QStringLiteral("model"), QStringLiteral("flux-2-klein-4b.safetensors")},
                         {QStringLiteral("model_preset"), kleinPreset().toVariant()}};
    auto *provider = qobject_cast<ai::SdCppProvider *>(f.studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Klein"), settings));
    QVERIFY(provider);
    provider->pollIntervalMs = 20;
    QVERIFY(f.setUp());
    EditorPage *editor = f.window->editor();
    PromptBar *bar = editor->promptBar();
    VariantStrip *strip = editor->variantStrip();
    auto *words = bar->findChild<AutoTextEdit *>(QStringLiteral("promptText"));
    QVERIFY(words);
    auto generationJobs = [&f] {
        QList<Job *> jobs;
        for (Job *j : f.studio.jobs()->jobs()) {
            if (j->kind() == QLatin1String("generation"))
                jobs << j;
        }
        return jobs;
    };

    // B runs first and keeps running; then A.
    sd.hold = true;
    doc::Document *b = f.studio.openImageAsDocument(photo(QSize(320, 240)), QStringLiteral("B"));
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
    words->setPlainText(QStringLiteral("make it night"));
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(sd.generations.size(), 1, 10000);
    Job *bJob = generationJobs().value(0);
    QVERIFY(bJob);
    // A's job waits in the Queue behind B's (one at a time per engine).
    doc::Document *a = f.studio.openImageAsDocument(photo(QSize(300, 200)), QStringLiteral("A"));
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
    words->setPlainText(QStringLiteral("make it snow"));
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(generationJobs().size(), 2, 10000);
    Job *aJob = generationJobs().value(1);
    QVERIFY(aJob && aJob != bJob);
    QVERIFY(strip->isRunning());

    // A closes: its job is cancelled, B's goes on and B is shown.
    QVERIFY(waitFor(f.files()->close(a)));
    QTRY_COMPARE(aJob->state(), Job::State::Cancelled);
    QCOMPARE(f.studio.activeDocument(), b);
    QVERIFY(!bJob->isFinished());
    QVERIFY(strip->isRunning()); // B's run, on show
    sd.hold = false;
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(strip->currentName(), QStringLiteral("make it night"));
    QCOMPARE(int(sd.generations.size()), 1); // A's never went out

    // Closed while the run is still being prepared (before its jobs exist): they are cancelled as
    // they come.
    strip->discard();
    sd.hold = true;
    doc::Document *c = f.studio.openImageAsDocument(photo(QSize(200, 200)), QStringLiteral("C"));
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
    words->setPlainText(QStringLiteral("make it pink"));
    const qsizetype before = generationJobs().size();
    QTest::keyClick(words, Qt::Key_Return);
    QVERIFY(waitFor(f.files()->close(c)));
    QTRY_VERIFY_WITH_TIMEOUT(generationJobs().size() > before, 10000);
    Job *cJob = generationJobs().last();
    QTRY_COMPARE(cJob->state(), Job::State::Cancelled);
    QCOMPARE(f.studio.activeDocument(), b);
    QCOMPARE(strip->count(), 0);
    QVERIFY(!strip->isRunning());

    // Quitting while a picture with unsaved changes to its file has its run going: every question
    // comes before anything is done, so No to the running jobs leaves the picture open and its
    // run going, and nothing was asked about the file yet.
    const QString path = f.path(QStringLiteral("running.png"));
    QVERIFY(photo(QSize(240, 160)).save(path));
    doc::Document *d = f.open(path);
    QVERIFY(d);
    paintSquare(d);
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
    words->setPlainText(QStringLiteral("make it rain"));
    const qsizetype queued = generationJobs().size();
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(generationJobs().size() > queued, 10000);
    Job *dJob = generationJobs().last();
    const qsizetype asked = f.unsavedQuestions.size();
    f.unsavedReply = QMessageBox::Discard;
    QString question;
    answerNextBox(QMessageBox::No, &question);
    f.window->close();
    QVERIFY2(question.contains(QStringLiteral("still running")), qPrintable(question));
    QVERIFY(f.window->isVisible());
    QCOMPARE(f.unsavedQuestions.size(), asked);
    QVERIFY(f.studio.documents().contains(d));
    QVERIFY(!dJob->isFinished());
    // Yes, then Don't save: the picture closes, its run is cancelled and Lumen quits.
    question.clear();
    answerNextBox(QMessageBox::Yes, &question);
    f.window->close();
    QTRY_VERIFY_WITH_TIMEOUT(!f.window->isVisible(), 10000);
    QCOMPARE(f.unsavedQuestions.size(), asked + 1);
    QTRY_COMPARE(dJob->state(), Job::State::Cancelled);
    sd.hold = false;
}

// The filters' Input node reads the active layer or the whole picture (every visible layer merged);
// with the whole picture the result goes on a new layer above all the others, as one undo step,
// and a cut-out hides every layer under it. The built-in AI filters read the whole picture.
void TestFiles::wholePictureInput()
{
    // The node's setting and the built-in filters.
    const filters::NodeTypePtr input = filters::NodeRegistry::global().find(QStringLiteral("input"));
    QVERIFY(input);
    const filters::ParamSpec *source = input->info().param(QStringLiteral("source"));
    QVERIFY(source);
    QCOMPARE(source->defaultValue.toString(), QStringLiteral("layer"));
    QCOMPARE(source->choices.size(), 2);
    QCOMPARE(source->choices.at(0).label, QStringLiteral("Active layer"));
    QCOMPARE(source->choices.at(1).label, QStringLiteral("Whole picture"));
    Studio studio;
    FilterCatalog *catalog = FilterCatalog::of(&studio);
    for (const QString &name : {QStringLiteral("colorize-with-ai"), QStringLiteral("clean-up-background"), QStringLiteral("remove-background"),
                                QStringLiteral("upscale-2x"), QStringLiteral("black-and-white"), QStringLiteral("old-photo")}) {
        const std::optional<filters::Graph> g = catalog->load(QStringLiteral(":/filters/builtin/%1.lumenfilter").arg(name));
        QVERIFY2(g, qPrintable(name));
        const bool ai = !name.contains(QStringLiteral("black")) && !name.contains(QStringLiteral("old"));
        QCOMPARE(g->readsWholePicture(), ai);
    }

    // Two layers: a red background and a blue square on the upper layer; the lower one is active.
    const QSize size(80, 60);
    doc::Document *doc = studio.newDocument(size, QStringLiteral("Layers"), QColor(200, 0, 0));
    const QUuid background = doc->activeLayerId();
    QImage square(size, QImage::Format_ARGB32_Premultiplied);
    square.fill(Qt::transparent);
    QPainter(&square).fillRect(QRect(40, 0, 40, 60), QColor(0, 0, 200));
    const QUuid top = doc->addLayer(QStringLiteral("Square"), square);
    doc->setActiveLayer(background);
    doc->undoStack()->clear();

    FilterRunner runner;
    runner.setPreviewEnabled(false);
    runner.setDocument(doc);
    // The active layer: the result goes above it, the square stays over it.
    runner.setGraph(oneStep(QStringLiteral("invert"), false), QStringLiteral("Invert"));
    QVERIFY(!runner.readsWholePicture());
    QVERIFY(applyAndWait(runner, FilterRunner::Target::NewLayer));
    QCOMPARE(doc->layerCount(), 3);
    QCOMPARE(doc->indexOf(top), 2);
    QCOMPARE(QColor(doc->layerAt(1)->image.pixel(60, 30)), QColor(55, 255, 255)); // the red layer inverted
    doc->undoStack()->undo();
    QCOMPARE(doc->layerCount(), 2);

    // The whole picture: one new layer on top with the picture inverted, the others as they were.
    runner.setGraph(oneStep(QStringLiteral("invert"), true), QStringLiteral("Invert all"));
    QVERIFY(runner.readsWholePicture());
    QCOMPARE(runner.wholePicture(), doc->flattened());
    const QImage picture = runner.wholePicture();
    QVERIFY(runner.wholePicture().cacheKey() == picture.cacheKey()); // kept while nothing changes
    const int steps = doc->undoStack()->index();
    QVERIFY(applyAndWait(runner, FilterRunner::Target::NewLayer));
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    QCOMPARE(doc->layerCount(), 3);
    const doc::LayerPtr result = doc->layerAt(2);
    QCOMPARE(result->props.name, QStringLiteral("Invert all"));
    QCOMPARE(QColor(result->image.pixel(10, 30)), QColor(55, 255, 255));  // red inverted
    QCOMPARE(QColor(result->image.pixel(60, 30)), QColor(255, 255, 55));  // blue inverted
    QCOMPARE(doc->indexOf(top), 1);
    QCOMPARE(QColor(doc->findLayer(top)->image.pixel(60, 30)), QColor(0, 0, 200));
    QCOMPARE(QColor(doc->findLayer(background)->image.pixel(10, 30)), QColor(200, 0, 0));
    doc->undoStack()->undo();
    QCOMPARE(doc->layerCount(), 2);

    // Through a selection: only there.
    QImage left = img::createMask(size);
    QPainter(&left).fillRect(QRect(0, 0, 40, 60), Qt::white);
    doc->applySelection(left, QStringLiteral("Select"));
    QVERIFY(applyAndWait(runner, FilterRunner::Target::NewLayer));
    QCOMPARE(qAlpha(doc->layerAt(2)->image.pixel(10, 30)), 255);
    QCOMPARE(qAlpha(doc->layerAt(2)->image.pixel(60, 30)), 0);
    doc->undoStack()->undo();
    doc->undoStack()->undo(); // the selection
    QVERIFY(!doc->hasSelection());

    // The preview shows the same: the inverted picture over the layers.
    runner.setPreviewEnabled(true);
    runner.resumePreview(); // off since the last Apply
    runner.setPreviewSize(80);
    QTRY_VERIFY(!runner.previewImage().isNull());
    QCOMPARE(QColor(runner.previewImage().pixel(60, 30)), QColor(255, 255, 55));
    runner.setPreviewEnabled(false);

    // Into the active layer: the picture's result replaces the layer's pixels.
    QVERIFY(applyAndWait(runner, FilterRunner::Target::Layer));
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(QColor(doc->findLayer(background)->image.pixel(60, 30)), QColor(255, 255, 55));
    doc->undoStack()->undo();

    // A cut-out of the whole picture hides every layer under its new layer, in the same step.
    filters::NodeRegistry registry = filters::NodeRegistry::global();
    registry.add(cutLeftHalf());
    runner.setGraph(oneStep(QStringLiteral("test-cut-left"), true, registry), QStringLiteral("Cut"));
    const int before = doc->undoStack()->index();
    QVERIFY(applyAndWait(runner, FilterRunner::Target::NewLayer));
    QCOMPARE(doc->undoStack()->index(), before + 1);
    QCOMPARE(doc->layerCount(), 3);
    QVERIFY(!doc->findLayer(background)->props.visible);
    QVERIFY(!doc->findLayer(top)->props.visible);
    QCOMPARE(qAlpha(doc->flattened().pixel(10, 30)), 0);
    QCOMPARE(QColor(doc->flattened().pixel(60, 30)), QColor(0, 0, 200));
    QVERIFY(runner.appliedNote().contains(QStringLiteral("layers are hidden")));
    doc->undoStack()->undo();
    QVERIFY(doc->findLayer(background)->props.visible && doc->findLayer(top)->props.visible);
    QCOMPARE(doc->layerCount(), 2);

    // The Filters panel says what it reads and where the result goes.
    FiltersPanel panel(&studio, nullptr, nullptr, catalog);
    QVERIFY(panel.selectFilter(QStringLiteral(":/filters/builtin/colorize-with-ai.lumenfilter")));
    auto *hint = panel.findChild<QLabel *>(QStringLiteral("FilterSelectionHint"));
    QVERIFY(hint);
    QVERIFY2(hint->text().startsWith(QStringLiteral("Reads the whole picture")), qPrintable(hint->text()));
    QVERIFY(panel.selectFilter(QStringLiteral(":/filters/builtin/black-and-white.lumenfilter")));
    QCOMPARE(hint->text(), QStringLiteral("Nothing is selected, so the whole layer changes."));
}

QTEST_MAIN(TestFiles)
#include "tst_files.moc"
