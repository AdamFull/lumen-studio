#include "core/ImageOps.h"
#include "core/PngText.h"
#include "doc/CharacterLibrary.h"
#include "doc/LoraDataset.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "doc/PsdIO.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QDir>
#include <QtTest>

using namespace lumen;
using namespace lumen::doc;

class TestDoc : public QObject
{
    Q_OBJECT
private slots:
    void psdImportExport();
    void characterLibrary();
    void rotateAndFlip();
    void loraDataset();
    void projectImportsPsd();
    void addRemoveLayerUndo();
    void paintCommitUndo();
    void layerPropsMerge();
    void selectionUndo();
    void resizeCanvasUndo();
    void compositeTilesFollowChanges();
    void compositeTilesPerformance();
    void projectAssetsRoundTrip();
    void projectDocumentRoundTrip();
    void projectRecords();
};

static QImage solid(const QSize &s, const QColor &c)
{
    QImage img(s, QImage::Format_ARGB32_Premultiplied);
    img.fill(c);
    return img;
}

void TestDoc::addRemoveLayerUndo()
{
    Document doc(QSize(64, 64));
    const QUuid a = doc.addLayer(QStringLiteral("A"), solid(QSize(64, 64), Qt::red));
    const QUuid b = doc.addLayer(QStringLiteral("B"));
    QCOMPARE(doc.layerCount(), 2);
    QCOMPARE(doc.activeLayerId(), b);
    doc.removeLayer(b);
    QCOMPARE(doc.layerCount(), 1);
    QCOMPARE(doc.activeLayerId(), a);
    doc.undoStack()->undo();
    QCOMPARE(doc.layerCount(), 2);
    QCOMPARE(doc.activeLayerId(), b);
    doc.undoStack()->undo();
    doc.undoStack()->undo();
    QCOMPARE(doc.layerCount(), 0);
    doc.undoStack()->redo();
    QCOMPARE(doc.layerAt(0)->image.pixelColor(5, 5), QColor(Qt::red));
}

void TestDoc::paintCommitUndo()
{
    Document doc(QSize(64, 64));
    const QUuid id = doc.addLayer(QStringLiteral("A"), solid(QSize(64, 64), Qt::white));
    QImage *img = doc.layerImageForPainting(id);
    const QRect rect(10, 10, 8, 8);
    const QImage before = img->copy(rect);
    {
        QPainter p(img);
        p.fillRect(rect, Qt::blue);
    }
    doc.notifyPixelsChanged(id, rect);
    doc.commitPaint(id, rect, before, QStringLiteral("Brush"));
    QCOMPARE(doc.layerAt(0)->image.pixelColor(12, 12), QColor(Qt::blue));
    doc.undoStack()->undo();
    QCOMPARE(doc.layerAt(0)->image.pixelColor(12, 12), QColor(Qt::white));
    doc.undoStack()->redo();
    QCOMPARE(doc.layerAt(0)->image.pixelColor(12, 12), QColor(Qt::blue));
}

void TestDoc::layerPropsMerge()
{
    Document doc(QSize(16, 16));
    const QUuid id = doc.addLayer(QStringLiteral("A"));
    const int before = doc.undoStack()->count();
    LayerProps p = doc.findLayer(id)->props;
    for (double o : {0.9, 0.8, 0.5}) {
        p.opacity = o;
        doc.setLayerProps(id, p, QStringLiteral("Opacity"));
    }
    QCOMPARE(doc.undoStack()->count(), before + 1);
    doc.undoStack()->undo();
    QCOMPARE(doc.findLayer(id)->props.opacity, 1.0);
}

void TestDoc::selectionUndo()
{
    Document doc(QSize(32, 32));
    QVERIFY(!doc.hasSelection());
    QImage mask = img::createMask(QSize(32, 32));
    mask.scanLine(4)[4] = 255;
    doc.applySelection(mask, QStringLiteral("Select"));
    QVERIFY(doc.hasSelection());
    QCOMPARE(doc.selectionBounds(), QRect(4, 4, 1, 1));
    doc.invertSelection();
    QCOMPARE(int(doc.selection().constScanLine(4)[4]), 0);
    doc.undoStack()->undo();
    doc.undoStack()->undo();
    QVERIFY(!doc.hasSelection());
}

void TestDoc::resizeCanvasUndo()
{
    Document doc(QSize(40, 30));
    doc.addLayer(QStringLiteral("A"), solid(QSize(40, 30), Qt::green));
    doc.resizeCanvas(QRect(-10, -5, 60, 40), QStringLiteral("Extend"));
    QCOMPARE(doc.size(), QSize(60, 40));
    QCOMPARE(doc.layerAt(0)->image.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(doc.layerAt(0)->image.pixelColor(10, 5), QColor(Qt::green));
    doc.undoStack()->undo();
    QCOMPARE(doc.size(), QSize(40, 30));
    QCOMPARE(doc.layerAt(0)->image.size(), QSize(40, 30));
}

void TestDoc::compositeTilesFollowChanges()
{
    Document doc(QSize(1000, 600));
    const QUuid a = doc.addLayer(QStringLiteral("A"), solid(QSize(1000, 600), Qt::red));
    const QUuid b = doc.addLayer(QStringLiteral("B"));
    doc.updateComposite(doc.bounds(), 0);
    QCOMPARE(doc.compositeTiles().tile(1, 1, 0).pixelColor(10, 10), QColor(Qt::red));
    doc.applyPixels(b, QRect(520, 520, 20, 20), solid(QSize(20, 20), Qt::blue), QStringLiteral("Fill"));
    doc.updateComposite(doc.bounds(), 0);
    QCOMPARE(doc.compositeTiles().tile(1, 1, 0).pixelColor(10, 10), QColor(Qt::blue));
    LayerProps p = doc.findLayer(b)->props;
    p.visible = false;
    doc.setLayerProps(b, p);
    doc.updateComposite(doc.bounds(), 1);
    QCOMPARE(doc.compositeTiles().tile(1, 1, 0).pixelColor(10, 10), QColor(Qt::red));
    QCOMPARE(doc.compositeTiles().tile(0, 0, 1).size(), QSize(256, 256));
    Q_UNUSED(a);
}

void TestDoc::compositeTilesPerformance()
{
    Document doc(QSize(4096, 4096));
    for (int i = 0; i < 4; ++i)
        doc.addLayer(QStringLiteral("L%1").arg(i), solid(QSize(4096, 4096), QColor(10 * i, 20, 30, 128)));
    QElapsedTimer t;
    t.start();
    doc.updateComposite(doc.bounds(), 0);
    const qint64 full = t.elapsed();
    QImage *img = doc.layerImageForPainting(doc.activeLayerId());
    t.restart();
    {
        QPainter p(img);
        p.fillRect(2000, 2000, 64, 64, Qt::black);
    }
    doc.notifyPixelsChanged(doc.activeLayerId(), QRect(2000, 2000, 64, 64));
    doc.updateComposite(doc.bounds(), 0);
    const qint64 dab = t.elapsed();
    qInfo() << "full 4096^2 x4 composite:" << full << "ms; incremental dab:" << dab << "ms";
    QVERIFY(dab < 50);
}

void TestDoc::projectAssetsRoundTrip()
{
    QTemporaryDir dir;
    Project project;
    auto open = project.open(dir.filePath(QStringLiteral("test.lumen")));
    QTRY_VERIFY(open.isFinished());
    QVERIFY(project.isOpen());
    QImage image = solid(QSize(300, 200), QColor(1, 2, 3));
    QJsonObject meta{{QStringLiteral("prompt"), QStringLiteral("a cat")}, {QStringLiteral("parameters"), QStringLiteral("a cat\nSteps: 20")}};
    auto added = project.addAsset(image, AssetKind::Generation, QStringLiteral("cat"), meta);
    QTRY_VERIFY(added.isFinished());
    const AssetInfo info = added.result();
    QCOMPARE(project.assets().size(), 1);
    auto loaded = project.loadImage(info.id);
    QTRY_VERIFY(loaded.isFinished());
    QCOMPARE(loaded.result().size(), QSize(300, 200));
    QCOMPARE(loaded.result().pixelColor(0, 0), QColor(1, 2, 3));
    auto thumb = project.loadThumbnail(info.id);
    QTRY_VERIFY(thumb.isFinished());
    QVERIFY(thumb.result().width() <= Project::kThumbnailSize);
    // Re-open: metadata persisted.
    auto closed = project.close();
    QTRY_VERIFY(closed.isFinished());
    Project again;
    auto open2 = again.open(dir.filePath(QStringLiteral("test.lumen")));
    QTRY_VERIFY(open2.isFinished());
    QCOMPARE(again.assets().size(), 1);
    QCOMPARE(again.assets()[0].prompt(), QStringLiteral("a cat"));
    // Exported PNG keeps A1111 parameters.
    auto exported = again.exportAsset(info.id, dir.filePath(QStringLiteral("out.png")));
    QTRY_VERIFY(exported.isFinished());
    QImageReader reader(dir.filePath(QStringLiteral("out.png")));
    // Qt simplifies whitespace when reading, but must find the chunk (it precedes IDAT).
    QCOMPARE(reader.text(QStringLiteral("parameters")), QStringLiteral("a cat Steps: 20"));
    again.removeAssets({info.id});
    QVERIFY(again.assets().isEmpty());
}

void TestDoc::projectDocumentRoundTrip()
{
    QTemporaryDir dir;
    Project project;
    auto open = project.open(dir.filePath(QStringLiteral("doc.lumen")));
    QTRY_VERIFY(open.isFinished());
    Document doc(QSize(128, 96));
    doc.setName(QStringLiteral("My doc"));
    doc.addLayer(QStringLiteral("Base"), solid(QSize(128, 96), Qt::yellow));
    const QUuid top = doc.addLayer(QStringLiteral("Top"));
    doc.applyPixels(top, QRect(0, 0, 10, 10), solid(QSize(10, 10), Qt::black), QStringLiteral("x"));
    QImage mask = img::createMask(doc.size());
    mask.scanLine(50)[60] = 255;
    doc.applySelection(mask, QStringLiteral("sel"));
    auto saved = project.saveDocument(&doc);
    QTRY_VERIFY(saved.isFinished());
    QTRY_VERIFY(!project.isSaving(doc.id()));
    QVERIFY(!doc.isModified());
    QCOMPARE(project.documents().size(), 1);
    // Second save after removing a layer deletes it from the store.
    doc.removeLayer(top);
    project.saveDocument(&doc);
    QTRY_VERIFY(!project.isSaving(doc.id()));
    auto loaded = project.loadDocument(doc.id());
    QTRY_VERIFY(loaded.isFinished());
    auto ld = loaded.result();
    QCOMPARE(ld->name, QStringLiteral("My doc"));
    QCOMPARE(ld->layers.size(), 1);
    QCOMPARE(ld->layers[0]->image.pixelColor(5, 5), QColor(Qt::yellow));
    QCOMPARE(int(ld->selection.constScanLine(50)[60]), 255);
}

void TestDoc::projectRecords()
{
    QTemporaryDir dir;
    Project project;
    auto open = project.open(dir.filePath(QStringLiteral("rec.lumen")));
    QTRY_VERIFY(open.isFinished());
    Record r;
    r.type = QStringLiteral("pose");
    r.name = QStringLiteral("Hero");
    r.json = QJsonObject{{QStringLiteral("x"), 1}};
    auto saved = project.saveRecord(r);
    QTRY_VERIFY(saved.isFinished());
    const QString id = saved.result().id;
    auto list = project.records(QStringLiteral("pose"));
    QTRY_VERIFY(list.isFinished());
    QCOMPARE(list.result().size(), 1);
    QCOMPARE(list.result()[0].json.value(QStringLiteral("x")).toInt(), 1);
    project.removeRecord(id, QStringLiteral("pose"));
    auto list2 = project.records(QStringLiteral("pose"));
    QTRY_VERIFY(list2.isFinished());
    QCOMPARE(list2.result().size(), 0);
}

void TestDoc::psdImportExport()
{
    psd::File f;
    f.size = QSize(40, 30);
    QImage a(10, 10, QImage::Format_ARGB32);
    a.fill(Qt::red);
    QImage b(5, 5, QImage::Format_ARGB32);
    b.fill(Qt::blue);
    psd::Layer shade = psd::Layer::pixels(QStringLiteral("Shade"), a, QPoint(2, 3));
    shade.blendKey = "mul ";
    shade.opacity = 128;
    psd::Layer hidden = psd::Layer::groupBegin(QStringLiteral("Extras"));
    hidden.visible = false;
    f.layers = {shade, hidden, psd::Layer::pixels(QStringLiteral("Star"), b, QPoint(30, 20)), psd::Layer::groupEnd(),
                psd::Layer::pixels(QStringLiteral("Base"), a, QPoint(0, 0))};
    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite);
    QVERIFY(psd::write(&buffer, f));
    buffer.seek(0);
    const auto back = psd::read(&buffer);
    QVERIFY(back.has_value());
    QCOMPARE(back->layers[0].blendKey, QByteArray("mul "));

    const doc::PsdImport imported = doc::importPsd(*back);
    QCOMPARE(imported.size, QSize(40, 30));
    QCOMPARE(imported.layers.size(), 3);
    // Bottom first: Base, Star (inside the hidden group), Shade.
    QCOMPARE(imported.layers[0]->props.name, QStringLiteral("Base"));
    QCOMPARE(imported.layers[1]->props.name, QStringLiteral("Star"));
    QVERIFY(!imported.layers[1]->props.visible);
    QCOMPARE(imported.layers[1]->meta.value(QStringLiteral("psd_group")).toString(), QStringLiteral("Extras"));
    QCOMPARE(imported.layers[1]->image.pixelColor(31, 21), QColor(Qt::blue)); // placed at its offset
    QCOMPARE(imported.layers[2]->props.blend, doc::BlendMode::Multiply);
    QVERIFY(std::abs(imported.layers[2]->props.opacity - 128 / 255.0) < 1e-6);
    // The group's hiding went onto its layer: said, as saving over the file keeps it that way.
    QCOMPARE(imported.warnings, QStringList{QStringLiteral("the opacity or hiding of 1 group(s) went onto their layers")});

    // Export the stack again, with the group the layer came from.
    doc::DocumentSnapshot snap;
    for (const doc::LayerPtr &l : imported.layers)
        snap.append(doc::LayerSnapshot{l->image, l->props, l->meta.value(QStringLiteral("psd_group")).toString()});
    const psd::File out = doc::exportPsd(snap, imported.size);
    QCOMPARE(out.layers.size(), 5);
    QCOMPARE(out.layers[0].name, QStringLiteral("Shade")); // front-most first
    QCOMPARE(out.layers[0].blendKey, QByteArray("mul "));
    QCOMPARE(out.layers[0].opacity, 128);
    QCOMPARE(out.layers[1].kind, psd::Layer::Kind::GroupBegin);
    QCOMPARE(out.layers[1].name, QStringLiteral("Extras"));
    QCOMPARE(out.layers[2].name, QStringLiteral("Star"));
    QVERIFY(!out.layers[2].visible);
    QCOMPARE(out.layers[3].kind, psd::Layer::Kind::GroupEnd);
    QCOMPARE(out.layers[4].name, QStringLiteral("Base"));
    // Nested groups open and close as the layers go down.
    doc::DocumentSnapshot nested{{a, {}, QStringLiteral("Head")}, {a, {}, QStringLiteral("Head/Eyes")}, {a, {}, QStringLiteral("Head/Eyes")}, {a, {}, {}}};
    QList<psd::Layer::Kind> kinds;
    QStringList names;
    for (const psd::Layer &l : doc::exportPsd(nested, QSize(40, 30)).layers) {
        kinds << l.kind;
        names << l.name;
    }
    using K = psd::Layer::Kind;
    QCOMPARE(kinds, (QList<K>{K::Pixels, K::GroupBegin, K::GroupBegin, K::Pixels, K::Pixels, K::GroupEnd, K::Pixels, K::GroupEnd}));
    QCOMPARE(names.mid(1, 2), (QStringList{QStringLiteral("Head"), QStringLiteral("Eyes")}));
    QCOMPARE(doc::blendModeFromPsdKey("lddg"), doc::BlendMode::Add);
    bool known = true;
    doc::blendModeFromPsdKey("vLit", &known);
    QVERIFY(!known);
}

void TestDoc::projectImportsPsd()
{
    QTemporaryDir dir;
    psd::File f;
    f.size = QSize(16, 16);
    QImage a(16, 16, QImage::Format_ARGB32);
    a.fill(Qt::green);
    f.layers = {psd::Layer::pixels(QStringLiteral("Fill"), a)};
    const QString path = dir.filePath(QStringLiteral("layers.psd"));
    QVERIFY(psd::write(path, f));
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("p.lumen")));
    QTRY_VERIFY(opened.isFinished());
    auto imported = project.importFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(imported.isFinished(), 5000);
    QCOMPARE(imported.result().size, QSize(16, 16));
    auto image = project.loadImage(imported.result().id);
    QTRY_VERIFY(image.isFinished());
    QCOMPARE(image.result().pixelColor(5, 5), QColor(Qt::green));
}

// Image → Rotate and Flip: every layer and the selection, pixel for pixel, one undo step each.
void TestDoc::rotateAndFlip()
{
    Document doc(QSize(4, 2));
    QImage pixels(4, 2, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            pixels.setPixel(x, y, qRgb(x * 60, y * 120, 7));
    const QUuid layer = doc.addLayer(QStringLiteral("Pixels"), pixels);
    QImage mask = img::createMask(doc.size());
    mask.setPixel(3, 0, 255); // the top right pixel, a value of a grey image
    doc.applySelection(mask, QStringLiteral("Select"));
    const int steps = doc.undoStack()->count();
    auto at = [&](int x, int y) { return doc.findLayer(layer)->image.pixel(x, y); };

    doc.rotateImage(90);
    QCOMPARE(doc.size(), QSize(2, 4));
    QCOMPARE(doc.undoStack()->count(), steps + 1);
    // Clockwise: the top left goes to the top right, the top right to the bottom right.
    QCOMPARE(at(1, 0), pixels.pixel(0, 0));
    QCOMPARE(at(1, 3), pixels.pixel(3, 0));
    QCOMPARE(at(0, 0), pixels.pixel(0, 1));
    QCOMPARE(img::maskBounds(doc.selection()), QRect(1, 3, 1, 1));
    doc.undoStack()->undo();
    QCOMPARE(doc.size(), QSize(4, 2));
    QCOMPARE(doc.findLayer(layer)->image.convertToFormat(pixels.format()), pixels);

    doc.rotateImage(270);
    QCOMPARE(doc.size(), QSize(2, 4));
    QCOMPARE(at(0, 3), pixels.pixel(0, 0)); // counter-clockwise: the top left to the bottom left
    doc.undoStack()->undo();
    doc.rotateImage(180);
    QCOMPARE(doc.size(), QSize(4, 2));
    QCOMPARE(at(3, 1), pixels.pixel(0, 0));
    doc.undoStack()->undo();

    doc.flipImage(Qt::Horizontal);
    QCOMPARE(at(3, 0), pixels.pixel(0, 0));
    QCOMPARE(at(0, 1), pixels.pixel(3, 1));
    QCOMPARE(img::maskBounds(doc.selection()), QRect(0, 0, 1, 1));
    doc.undoStack()->undo();
    doc.flipImage(Qt::Vertical);
    QCOMPARE(at(0, 1), pixels.pixel(0, 0));
    QCOMPARE(img::maskBounds(doc.selection()), QRect(3, 1, 1, 1));
    QCOMPARE(doc.undoStack()->undoText(), QStringLiteral("Flip vertically"));
    // Anything else than a quarter turn does nothing.
    const int before = doc.undoStack()->count();
    doc.rotateImage(45);
    QCOMPARE(doc.undoStack()->count(), before);
}

void TestDoc::characterLibrary()
{
    QTemporaryDir dir;
    CharacterLibrary library(dir.filePath(QStringLiteral("characters")));
    QVERIFY(library.entries().isEmpty());
    // No folder until the first character: a start with the Characters plugin off leaves none.
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("characters"))));
    QSignalSpy changed(&library, &CharacterLibrary::changed);
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")}, {QStringLiteral("hair"), QStringLiteral("red")}};
    auto saved = library.save(QStringLiteral("Mira"), sheet, {solid(QSize(64, 80), Qt::red), QImage(), solid(QSize(32, 32), Qt::blue)},
                              QStringLiteral("mage"));
    QTRY_VERIFY(saved.isFinished());
    const CharacterEntry e = saved.result();
    QCOMPARE(e.referenceCount, 2); // null images are skipped
    QCOMPARE(changed.size(), 1);
    QCOMPARE(library.entries().size(), 1);
    const auto read = library.entry(e.id);
    QVERIFY(read.has_value());
    QCOMPARE(read->name, QStringLiteral("Mira"));
    QCOMPARE(read->sheet, sheet);
    QCOMPARE(read->notes, QStringLiteral("mage"));
    QVERIFY(!read->thumbnail.isNull());
    auto refs = library.loadReferences(e.id);
    QTRY_VERIFY(refs.isFinished());
    QCOMPARE(refs.result().size(), 2);
    QCOMPARE(refs.result()[0].size(), QSize(64, 80));
    QCOMPARE(refs.result()[1].pixelColor(5, 5), QColor(Qt::blue));

    // Replacing keeps the id and the creation date and drops old references.
    QTest::qWait(5);
    auto replaced = library.save(QStringLiteral("Mira"), sheet, {solid(QSize(16, 16), Qt::green)}, QString(), e.id);
    QTRY_VERIFY(replaced.isFinished());
    QCOMPARE(replaced.result().id, e.id);
    QCOMPARE(replaced.result().created, read->created);
    QVERIFY(replaced.result().modified >= read->modified);
    auto refs2 = library.loadReferences(e.id);
    QTRY_VERIFY(refs2.isFinished());
    QCOMPARE(refs2.result().size(), 1);
    QCOMPARE(QDir(dir.filePath(QStringLiteral("characters/") + e.id)).entryList({QStringLiteral("ref_*.png")}).size(), 1);

    // A LoRA attached to the character survives a save without one; an explicit empty one detaches it.
    const QJsonObject lora{{QStringLiteral("file"), QStringLiteral("C:/loras/mira.safetensors")}, {QStringLiteral("trigger"), QStringLiteral("lmn_mira")}};
    QVERIFY(library.setLora(e.id, lora));
    QCOMPARE(library.entry(e.id)->lora, lora);
    auto again = library.save(QStringLiteral("Mira"), sheet, {solid(QSize(16, 16), Qt::green)}, QString(), e.id);
    QTRY_VERIFY(again.isFinished());
    QCOMPARE(again.result().lora, lora);
    QCOMPARE(library.entry(e.id)->lora, lora);
    auto cleared = library.save(QStringLiteral("Mira"), sheet, {solid(QSize(16, 16), Qt::green)}, QString(), e.id, QJsonObject());
    QTRY_VERIFY(cleared.isFinished());
    QVERIFY(library.entry(e.id)->lora.isEmpty());

    // A second library on the same folder (another project / session) sees the character.
    CharacterLibrary other(dir.filePath(QStringLiteral("characters")));
    QCOMPARE(other.entries().size(), 1);
    QVERIFY(other.rename(e.id, QStringLiteral("Mira the Mage")));
    QCOMPARE(library.entry(e.id)->name, QStringLiteral("Mira the Mage"));
    QVERIFY(!other.rename(e.id, QStringLiteral("  ")));
    QVERIFY(!library.remove(QStringLiteral("../escape")));
    QVERIFY(library.remove(e.id));
    QVERIFY(library.entries().isEmpty());
    QVERIFY(!library.remove(e.id));
}

void TestDoc::loraDataset()
{
    // A character-like picture, a slightly changed copy (near duplicate) and a different one.
    auto picture = [](const QColor &body, int shift) {
        QImage img(512, 768, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        QPainter p(&img);
        p.setPen(Qt::NoPen);
        p.setBrush(body);
        p.drawEllipse(QRect(180 + shift, 60, 150, 150));
        p.drawRect(QRect(160 + shift, 220, 190, 420));
        p.setBrush(Qt::black);
        p.drawRect(QRect(60, 600, 80, 120));
        return img;
    };
    const QImage a = picture(QColor(200, 40, 40), 0);
    const QImage nearA = picture(QColor(205, 45, 40), 3);
    QImage b(512, 768, QImage::Format_ARGB32_Premultiplied);
    b.fill(Qt::white);
    {
        QPainter p(&b);
        p.setBrush(QColor(40, 60, 200));
        p.drawRect(QRect(20, 300, 470, 200));
    }
    QVERIFY(hashDistance(differenceHash(a), differenceHash(nearA)) <= 6);
    QVERIFY(hashDistance(differenceHash(a), differenceHash(b)) > 12);

    // SDXL buckets: about one megapixel in multiples of 64; SD 1.5 about 512².
    const QList<QSize> xl = trainingBuckets(QStringLiteral("SDXL"));
    QVERIFY(xl.contains(QSize(1024, 1024)));
    QVERIFY(xl.contains(QSize(832, 1216)));
    QVERIFY(xl.contains(QSize(1216, 832)));
    for (const QSize &s : xl) {
        QCOMPARE(s.width() % 64, 0);
        QCOMPARE(s.height() % 64, 0);
        QVERIFY(qint64(s.width()) * s.height() <= 1024 * 1024 * 1.06);
        QVERIFY(qint64(s.width()) * s.height() >= 1024 * 1024 * 0.85);
    }
    QVERIFY(trainingBuckets(QStringLiteral("SD 1.5")).contains(QSize(512, 512)));
    QCOMPARE(bucketFor(QSize(832, 1216), xl), QSize(832, 1216));
    QCOMPARE(bucketFor(QSize(1000, 1000), xl), QSize(1024, 1024));
    QCOMPARE(bucketFor(QSize(512, 768), xl), QSize(832, 1216));
    const QImage cropped = cropToBucket(a, QSize(832, 1216));
    QCOMPARE(cropped.size(), QSize(832, 1216));

    QTemporaryDir dir;
    LoraDataset ds(dir.filePath(QStringLiteral("dataset")));
    QSignalSpy changed(&ds, &LoraDataset::changed);
    QStringList ids;
    for (const QImage &img : {a, nearA, b}) {
        auto f = ds.add(img, {{QStringLiteral("asset"), QStringLiteral("asset-%1").arg(ids.size())}},
                        {{QStringLiteral("id"), QStringLiteral("ds%1").arg(ids.size() + 1)}});
        QTRY_VERIFY(f.isFinished());
        ids << f.result().id;
    }
    QCOMPARE(ds.items().size(), 3);
    QCOMPARE(ds.keptCount(), 3);
    QVERIFY(ds.hasAsset(QStringLiteral("asset-1")));
    QVERIFY(!ds.hasAsset(QStringLiteral("asset-9")));
    QVERIFY(QFile::exists(ds.imagePath(ids[0])));
    // The near duplicate with the lower score is flagged and dropped from training.
    ds.setCritique(ids[0], 6, {{QStringLiteral("identity"), 6}});
    ds.setCritique(ids[1], 8, {{QStringLiteral("identity"), 8}});
    QCOMPARE(ds.markDuplicates(), 1);
    QCOMPARE(ds.item(ids[0])->duplicateOf, ids[1]);
    QVERIFY(!ds.item(ids[0])->kept);
    QCOMPARE(ds.keptCount(), 2);
    ds.setKept(ids[0], true); // the user decides
    QCOMPARE(ds.keptCount(), 3);
    ds.setCaption(ids[1], QStringLiteral("lmn_mira, 1girl, standing"));
    ds.setTags(ids[2], {QStringLiteral("1girl"), QStringLiteral("red hair")});
    ds.setCaption(ids[2], QStringLiteral("lmn_mira, 1girl"), false);
    ds.setSettings({QStringLiteral("lmn_mira"), false});

    // Reloading keeps everything.
    LoraDataset again(dir.filePath(QStringLiteral("dataset")));
    again.load();
    QCOMPARE(again.items().size(), 3);
    QCOMPARE(again.item(ids[1])->score, 8);
    QCOMPARE(again.item(ids[1])->caption, QStringLiteral("lmn_mira, 1girl, standing"));
    QCOMPARE(again.item(ids[0])->hash, ds.item(ids[0])->hash);
    QVERIFY(again.item(ids[1])->captionEdited);
    QVERIFY(!again.item(ids[2])->captionEdited);
    QCOMPARE(again.item(ids[2])->tags, (QStringList{QStringLiteral("1girl"), QStringLiteral("red hair")}));
    QCOMPARE(again.settings().trigger, QStringLiteral("lmn_mira"));
    QVERIFY(!again.settings().outfitIsIdentity);
    // One change applied to every caption.
    again.updateCaptions([](const DatasetItem &i) { return i.caption.isEmpty() ? i.caption : i.caption + QStringLiteral(", solo"); });
    QCOMPARE(again.item(ids[2])->caption, QStringLiteral("lmn_mira, 1girl, solo"));
    QVERIFY(again.item(ids[2])->captionEdited);
    QCOMPARE(again.item(ids[0])->caption, QString());
    QCOMPARE(again.item(ids[2])->shot.value(QStringLiteral("id")).toString(), QStringLiteral("ds3"));

    // Training export: kept images at bucket sizes with captions.
    again.setKept(ids[2], false);
    auto exported = again.exportForTraining(dir.filePath(QStringLiteral("train/10_lmn_mira")), QStringLiteral("SDXL"));
    QTRY_VERIFY(exported.isFinished());
    QCOMPARE(exported.result(), 2);
    const QDir train(dir.filePath(QStringLiteral("train/10_lmn_mira")));
    QCOMPARE(train.entryList({QStringLiteral("*.png")}).size(), 2);
    QCOMPARE(QImage(train.filePath(QStringLiteral("001.png"))).size(), QSize(832, 1216));
    QFile caption(train.filePath(QStringLiteral("002.txt")));
    QVERIFY(caption.open(QIODevice::ReadOnly));
    QCOMPARE(caption.readAll(), QByteArray("lmn_mira, 1girl, standing, solo"));
    QFile bare(train.filePath(QStringLiteral("001.txt"))); // no caption yet: the trigger word alone
    QVERIFY(bare.open(QIODevice::ReadOnly));
    QCOMPARE(bare.readAll(), QByteArray("lmn_mira"));

    QVERIFY(again.remove(ids[0]));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("dataset/images/")) + ids[0] + QStringLiteral(".png")));
    QCOMPARE(again.items().size(), 2);
}

QTEST_MAIN(TestDoc)
#include "tst_doc.moc"
