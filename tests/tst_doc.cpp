#include "core/ImageOps.h"
#include "core/PngText.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "doc/PsdIO.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace lumen;
using namespace lumen::doc;

class TestDoc : public QObject
{
    Q_OBJECT
private slots:
    void psdImportExport();
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
    QVERIFY(imported.warnings.isEmpty());

    // Export the stack again and read it back.
    doc::DocumentSnapshot snap;
    for (const doc::LayerPtr &l : imported.layers)
        snap.append(doc::LayerSnapshot{l->image, l->props});
    const psd::File out = doc::exportPsd(snap, imported.size);
    QCOMPARE(out.layers.size(), 3);
    QCOMPARE(out.layers[0].name, QStringLiteral("Shade")); // front-most first
    QCOMPARE(out.layers[0].blendKey, QByteArray("mul "));
    QCOMPARE(out.layers[0].opacity, 128);
    QVERIFY(!out.layers[1].visible);
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

QTEST_MAIN(TestDoc)
#include "tst_doc.moc"
