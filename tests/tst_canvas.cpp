#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/TransformTool.h"

#include <QPainter>
#include <QtTest>

using namespace lumen;
using namespace lumen::ui;

namespace {

const QColor kRed(220, 30, 30);
const QColor kBlue(30, 30, 220);

// A 40×20 rectangle at (40, 40): left half red, right half blue.
QImage content()
{
    QImage im(200, 200, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.fillRect(QRect(40, 40, 20, 20), kRed);
    p.fillRect(QRect(60, 40, 20, 20), kBlue);
    return im;
}

} // namespace

class TestCanvas : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void transformMoves();
    void transformScalesWithHandles();
    void transformRotatesAndFlips();
    void transformSelectionOnly();
    void transformRevertAndResample();
    void transformLockedLayer();

private:
    ToolEvent at(double x, double y, Qt::KeyboardModifiers mods = Qt::NoModifier) const
    {
        ToolEvent e;
        e.docPos = QPointF(x, y);
        e.widgetPos = m_view->mapFromDoc(e.docPos);
        e.button = Qt::LeftButton;
        e.buttons = Qt::LeftButton;
        e.modifiers = mods;
        return e;
    }
    void drag(TransformTool &tool, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        tool.press(m_view, at(from.x(), from.y(), mods));
        tool.move(m_view, at((from.x() + to.x()) / 2, (from.y() + to.y()) / 2, mods));
        tool.move(m_view, at(to.x(), to.y(), mods));
        tool.release(m_view, at(to.x(), to.y(), mods));
    }
    QColor pixel(int x, int y) const { return m_doc->activeLayer()->image.pixelColor(x, y); }

    EditorState *m_state = nullptr;
    doc::Document *m_doc = nullptr;
    CanvasView *m_view = nullptr;
    TransformTool *m_tool = nullptr;
};

void TestCanvas::init()
{
    QStandardPaths::setTestModeEnabled(true);
    m_state = new EditorState;
    m_doc = new doc::Document(QSize(200, 200));
    m_doc->addLayer(QStringLiteral("Shape"), content());
    m_view = new CanvasView(m_state);
    m_view->resize(400, 400);
    m_view->setDocument(m_doc);
    m_tool = new TransformTool(m_state);
    m_view->setTool(m_tool);
}

void TestCanvas::cleanup()
{
    m_view->setTool(nullptr);
    delete m_tool;
    delete m_view;
    delete m_doc;
    delete m_state;
}

void TestCanvas::transformMoves()
{
    const int steps = m_doc->undoStack()->count();
    drag(*m_tool, QPointF(60, 50), QPointF(100, 90)); // inside the box: move by (40, 40)
    QVERIFY(m_tool->hasSession());
    QCOMPARE(m_tool->sourceRect(), QRect(40, 40, 40, 20));
    QCOMPARE(m_doc->undoStack()->count(), steps + 1);
    QCOMPARE(pixel(85, 90), kRed);
    QCOMPARE(pixel(115, 90), kBlue);
    QCOMPARE(pixel(50, 50).alpha(), 0);
    // A second drag is a second step; undo returns to the first.
    drag(*m_tool, QPointF(100, 90), QPointF(110, 90));
    QCOMPARE(m_doc->undoStack()->count(), steps + 2);
    QCOMPARE(pixel(95, 90), kRed);
    m_doc->undoStack()->undo();
    QCOMPARE(pixel(85, 90), kRed);
    m_doc->undoStack()->undo();
    QCOMPARE(pixel(50, 50), kRed);
    // Arrow keys nudge.
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::ShiftModifier);
    QVERIFY(m_tool->key(m_view, &right));
    QCOMPARE(pixel(52, 50), kRed);
    QCOMPARE(pixel(45, 50).alpha(), 0);
}

void TestCanvas::transformScalesWithHandles()
{
    // Right edge handle (source (80, 50)) dragged 40 px right doubles the width; the left edge stays.
    drag(*m_tool, QPointF(80, 50), QPointF(120, 50));
    QCOMPARE(m_tool->params().sx, 2.0);
    QCOMPARE(m_tool->params().sy, 1.0);
    QCOMPARE(pixel(42, 50), kRed);
    QCOMPARE(pixel(75, 50), kRed);
    QCOMPARE(pixel(85, 50), kBlue);
    QCOMPARE(pixel(118, 50), kBlue);
    QCOMPARE(pixel(122, 50).alpha(), 0);
    // Bottom-right corner with Shift keeps the aspect: (120, 60) → (160, 70) scales by 1.5 from the top left.
    drag(*m_tool, QPointF(120, 60), QPointF(160, 70), Qt::ShiftModifier);
    QVERIFY(std::abs(m_tool->params().sx / m_tool->params().sy - 2.0) < 1e-9);
    const QRectF box = m_tool->transform().mapRect(QRectF(m_tool->sourceRect()));
    QVERIFY(std::abs(box.left() - 40) < 1e-6 && std::abs(box.top() - 40) < 1e-6);
    // Alt scales around the centre.
    const QPointF c = m_tool->params().center;
    const double sy = m_tool->params().sy;
    drag(*m_tool, QPointF(box.center().x(), box.bottom()), QPointF(box.center().x(), box.bottom() + 10), Qt::AltModifier);
    QCOMPARE(m_tool->params().center, c);
    QVERIFY(std::abs(m_tool->params().sy - (sy * 20 + 20) / 20) < 1e-9);
}

void TestCanvas::transformRotatesAndFlips()
{
    QVERIFY(m_tool->rotateBy(90)); // clockwise around the centre (60, 50): left half goes to the top
    QCOMPARE(pixel(60, 35), kRed);
    QCOMPARE(pixel(60, 65), kBlue);
    QCOMPARE(pixel(45, 50).alpha(), 0);
    QVERIFY(m_tool->flip(Qt::Vertical));
    QCOMPARE(pixel(60, 35), kBlue);
    QCOMPARE(pixel(60, 65), kRed);
    m_tool->cancel(); // back to the original
    QCOMPARE(pixel(45, 50), kRed);
    QVERIFY(m_tool->flip(Qt::Horizontal));
    QCOMPARE(pixel(45, 50), kBlue);
    QCOMPARE(pixel(75, 50), kRed);
    // Dragging outside the box rotates; Shift snaps to 15°.
    drag(*m_tool, QPointF(100, 50), QPointF(100, 62), Qt::ShiftModifier);
    QCOMPARE(m_tool->params().angle, 15.0);
}

void TestCanvas::transformSelectionOnly()
{
    QImage sel = img::createMask(m_doc->size());
    QPainter(&sel).fillRect(QRect(40, 40, 20, 20), Qt::white); // the red half
    m_doc->applySelection(sel, QStringLiteral("Select"));
    drag(*m_tool, QPointF(50, 50), QPointF(50, 100));
    QCOMPARE(m_tool->sourceRect(), QRect(40, 40, 20, 20));
    QCOMPARE(pixel(50, 100), kRed);
    QCOMPARE(pixel(50, 50).alpha(), 0);
    QCOMPARE(pixel(70, 50), kBlue); // unselected pixels stay
    QCOMPARE(img::maskBounds(m_doc->selection()), QRect(40, 90, 20, 20)); // the selection follows
    m_doc->undoStack()->undo(); // pixels and selection in one step
    QCOMPARE(pixel(50, 50), kRed);
    QCOMPARE(img::maskBounds(m_doc->selection()), QRect(40, 40, 20, 20));
}

void TestCanvas::transformRevertAndResample()
{
    const QImage original = m_doc->activeLayer()->image.copy();
    const int index = m_doc->undoStack()->index();
    // Shrinking then growing back resamples the original pixels, so nothing is lost.
    QVERIFY(m_tool->begin());
    TransformTool::Params p = m_tool->params();
    p.sx = 0.25;
    p.sy = 0.25;
    QVERIFY(m_tool->setParams(p));
    p.sx = 1.0;
    p.sy = 1.0;
    QVERIFY(m_tool->setParams(p));
    QCOMPARE(m_doc->activeLayer()->image, original);
    // Esc reverts every step of the session.
    drag(*m_tool, QPointF(60, 50), QPointF(90, 50));
    QVERIFY(m_doc->undoStack()->index() > index);
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QVERIFY(m_tool->key(m_view, &esc));
    QCOMPARE(m_doc->undoStack()->index(), index);
    QCOMPARE(m_doc->activeLayer()->image, original);
    QVERIFY(!m_tool->hasSession());
    // Enter keeps the result and starts a fresh session next time.
    drag(*m_tool, QPointF(60, 50), QPointF(90, 50));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QVERIFY(m_tool->key(m_view, &enter));
    QVERIFY(!m_tool->hasSession());
    QCOMPARE(pixel(75, 50), kRed);
    // A change from elsewhere (undo) restarts the session from the current pixels.
    drag(*m_tool, QPointF(90, 50), QPointF(90, 80));
    QCOMPARE(m_tool->sourceRect(), QRect(70, 40, 40, 20));
    m_doc->undoStack()->undo();
    drag(*m_tool, QPointF(90, 50), QPointF(90, 60));
    QCOMPARE(m_tool->sourceRect(), QRect(70, 40, 40, 20));
    QCOMPARE(pixel(75, 60), kRed);
}

void TestCanvas::transformLockedLayer()
{
    doc::LayerProps props = m_doc->activeLayer()->props;
    props.locked = true;
    m_doc->setLayerProps(m_doc->activeLayerId(), props);
    QSignalSpy messages(m_tool, &Tool::message);
    const QImage before = m_doc->activeLayer()->image.copy();
    drag(*m_tool, QPointF(60, 50), QPointF(100, 90));
    QCOMPARE(messages.size(), 1);
    QCOMPARE(m_doc->activeLayer()->image, before);
}

QTEST_MAIN(TestCanvas)
#include "tst_canvas.moc"
