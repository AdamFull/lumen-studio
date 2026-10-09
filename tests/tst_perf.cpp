// Performance budgets of the heavy document operations on a large image (4096², 8 layers).
// Timings are printed; budgets are enforced in optimised builds only.
#include "core/ImageOps.h"
#include "core/Psd.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "doc/PsdIO.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/TransformTool.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace lumen;

namespace {

constexpr int kSide = 4096;
constexpr int kLayers = 8;

QImage layerImage(int i)
{
    QImage im(kSide, kSide, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor::fromHsv((i * 45) % 360, 200, 220, 200));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRect(200 + i * 300, 300 + i * 250, 1800, 1500));
    return im;
}

void check(const char *what, qint64 ms, qint64 budget)
{
    qInfo().noquote() << QStringLiteral("%1: %2 ms (budget %3 ms)").arg(QLatin1String(what)).arg(ms).arg(budget);
#ifdef NDEBUG
    QVERIFY2(ms <= budget, what);
#else
    Q_UNUSED(budget);
#endif
}

} // namespace

class TestPerf : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void composite();
    void selectionBounds();
    void transformPreview();
    void projectSaveLoad();
    void psdExport();

private:
    doc::Document *m_doc = nullptr;
};

void TestPerf::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_doc = new doc::Document(QSize(kSide, kSide));
    for (int i = 0; i < kLayers; ++i)
        m_doc->addLayer(QStringLiteral("Layer %1").arg(i), layerImage(i));
}

void TestPerf::cleanupTestCase()
{
    delete m_doc;
}

void TestPerf::composite()
{
    QElapsedTimer t;
    t.start();
    m_doc->updateComposite(m_doc->bounds(), 0);
    check("full composite", t.elapsed(), 1500);
    t.restart();
    const QImage flat = m_doc->flattened();
    check("flatten", t.elapsed(), 1500);
    QVERIFY(!flat.isNull());
}

void TestPerf::selectionBounds()
{
    QImage mask = img::createMask(m_doc->size());
    QPainter(&mask).fillRect(QRect(1000, 1200, 800, 600), Qt::white);
    m_doc->applySelection(mask, QStringLiteral("Select"));
    QElapsedTimer t;
    t.start();
    const QRect r = m_doc->selectionBounds();
    check("selection bounds", t.elapsed(), 10);
    QCOMPARE(r, QRect(1000, 1200, 800, 600));
    m_doc->clearSelection();
}

void TestPerf::transformPreview()
{
    ui::EditorState state;
    ui::CanvasView view(&state);
    view.resize(800, 600);
    view.setDocument(m_doc);
    ui::TransformTool tool(&state);
    view.setTool(&tool);
    QElapsedTimer t;
    t.start();
    QVERIFY(tool.begin());
    check("transform: float layer", t.elapsed(), 300);
    ui::ToolEvent e;
    e.button = Qt::LeftButton;
    e.buttons = Qt::LeftButton;
    const QRectF box = tool.transform().mapRect(QRectF(tool.sourceRect()));
    e.docPos = box.center();
    e.widgetPos = view.mapFromDoc(e.docPos);
    tool.press(&view, e);
    t.restart();
    for (int i = 1; i <= 10; ++i) {
        e.docPos = box.center() + QPointF(i * 7, i * 3);
        e.widgetPos = view.mapFromDoc(e.docPos);
        tool.move(&view, e);
    }
    check("transform: 10 drag steps", t.elapsed(), 600);
    t.restart();
    tool.release(&view, e);
    check("transform: commit", t.elapsed(), 400);
    m_doc->undoStack()->undo();
    view.setTool(nullptr);
}

void TestPerf::projectSaveLoad()
{
    QTemporaryDir dir;
    doc::Project project;
    auto opened = project.open(dir.filePath(QStringLiteral("perf.lumen")));
    QTRY_VERIFY(opened.isFinished());
    QElapsedTimer t;
    t.start();
    auto saved = project.saveDocument(m_doc);
    // The GUI thread only takes a snapshot; encoding runs on workers.
    check("save: GUI thread", t.elapsed(), 50);
    QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 60000);
    check("save: total", t.elapsed(), 8000);
    t.restart();
    auto loaded = project.loadDocument(m_doc->id());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.isFinished(), 60000);
    check("load", t.elapsed(), 5000);
    QCOMPARE(loaded.result()->layers.size(), kLayers);
    // Saving again without changes re-encodes nothing.
    t.restart();
    m_doc->setModified(true);
    auto again = project.saveDocument(m_doc);
    QTRY_VERIFY_WITH_TIMEOUT(again.isFinished(), 60000);
    check("save unchanged", t.elapsed(), 1200);
}

void TestPerf::psdExport()
{
    QElapsedTimer t;
    t.start();
    const psd::File file = doc::exportPsd(m_doc->snapshot(), m_doc->size());
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(psd::write(&buffer, file));
    check("PSD export", t.elapsed(), 6000);
    QVERIFY(buffer.size() > 1000);
}

QTEST_MAIN(TestPerf)
#include "tst_perf.moc"
