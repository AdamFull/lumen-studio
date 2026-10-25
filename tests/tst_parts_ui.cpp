#include "FakeCubismCore.h"
#include "core/ImageOps.h"
#include "core/MaskOps.h"
#include "doc/Document.h"
#include "doc/Parts.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "live2d/CubismRenderer.h"
#include "plugins/parts/CubismView.h"
#include "plugins/parts/PartTools.h"
#include "plugins/parts/PuppetPreview.h"

#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace lumen;
using namespace lumen::ui;

namespace {

QImage figure()
{
    QImage im(200, 200, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    QPainter p(&im);
    p.fillRect(QRect(50, 20, 100, 160), QColor(200, 60, 60)); // body
    p.fillRect(QRect(60, 30, 30, 30), QColor(30, 200, 40));   // green patch
    p.end();
    return im;
}

ToolEvent at(double x, double y, Qt::KeyboardModifiers mods = Qt::NoModifier, Qt::MouseButton button = Qt::LeftButton)
{
    ToolEvent e;
    e.docPos = QPointF(x, y);
    e.button = button;
    e.buttons = button;
    e.modifiers = mods;
    return e;
}

int labelAt(const doc::PartSet &set, int x, int y)
{
    return set.labels().constScanLine(y)[x];
}

} // namespace

class TestPartsUi : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void brushPaintsInsideSilhouette();
    void brushRespectsLocks();
    void eraserWithoutActivePart();
    void lassoAndWand();
    void cutLineSplits();
    void hiddenAreaBrush();
    void puppetRenders();
    void cubismViewAnimates();

private:
    EditorState *m_state = nullptr;
    PartEditContext *m_context = nullptr;
    doc::PartSet *m_set = nullptr;
    doc::Document *m_doc = nullptr;
    CanvasView *m_view = nullptr;
};

void TestPartsUi::init()
{
    QStandardPaths::setTestModeEnabled(true);
    m_state = new EditorState;
    m_state->setMaskBrushSize(10);
    m_context = new PartEditContext;
    m_set = new doc::PartSet;
    m_set->setSource(figure(), QStringLiteral("src"), img::silhouetteMask(figure()));
    m_context->setSet(m_set);
    m_doc = new doc::Document(m_set->size());
    m_view = new CanvasView(m_state);
    m_view->resize(400, 400);
    m_view->setDocument(m_doc);
}

void TestPartsUi::cleanup()
{
    delete m_view;
    delete m_doc;
    delete m_context;
    delete m_set;
    delete m_state;
}

void TestPartsUi::brushPaintsInsideSilhouette()
{
    PartBrushTool brush(m_state, m_context, PartBrushTool::Mode::Paint);
    QSignalSpy messages(&brush, &Tool::message);
    brush.press(m_view, at(100, 100));
    QCOMPARE(messages.size(), 1); // no active part yet
    const int part = m_set->addPart(QStringLiteral("topwear"));
    m_context->setActiveLabel(part);
    const int undoBefore = m_set->undoStack()->count();
    brush.press(m_view, at(40, 100)); // starts outside the silhouette
    brush.move(m_view, at(120, 100));
    brush.release(m_view, at(120, 100));
    QCOMPARE(m_set->undoStack()->count(), undoBefore + 1);
    QCOMPARE(labelAt(*m_set, 100, 100), part);
    QCOMPARE(labelAt(*m_set, 42, 100), 0); // background stays unassigned
    QCOMPARE(m_set->part(part)->state, doc::PartState::Masked);
    m_set->undoStack()->undo();
    QCOMPARE(labelAt(*m_set, 100, 100), 0);
    // Without the silhouette restriction the background is painted too.
    m_context->setInsideSilhouette(false);
    m_set->undoStack()->redo();
    brush.press(m_view, at(42, 100));
    brush.release(m_view, at(42, 100));
    QCOMPARE(labelAt(*m_set, 42, 100), part);
}

void TestPartsUi::brushRespectsLocks()
{
    const int a = m_set->addPart(QStringLiteral("face"), {}, {}, img::createMask(m_set->size(), 0));
    QImage maskA = img::createMask(m_set->size(), 0);
    for (int y = 90; y < 110; ++y)
        std::fill(maskA.scanLine(y) + 60, maskA.scanLine(y) + 140, uchar(255));
    m_context->setActiveLabel(a);
    m_context->assign(maskA, false, QStringLiteral("a"));
    doc::Part locked = *m_set->part(a);
    locked.locked = true;
    m_set->updatePart(locked);
    const int b = m_set->addPart(QStringLiteral("topwear"));
    m_context->setActiveLabel(b);
    PartBrushTool brush(m_state, m_context, PartBrushTool::Mode::Paint);
    brush.press(m_view, at(100, 100));
    brush.release(m_view, at(100, 100));
    QCOMPARE(labelAt(*m_set, 100, 100), a); // locked pixels untouched
    QCOMPARE(labelAt(*m_set, 100, 92), a);
    brush.press(m_view, at(100, 80));
    brush.release(m_view, at(100, 80));
    QCOMPARE(labelAt(*m_set, 100, 80), b);
}

void TestPartsUi::eraserWithoutActivePart()
{
    const int a = m_set->addPart(QStringLiteral("face"));
    m_context->setActiveLabel(a);
    m_context->assign(img::thresholdMask(m_set->silhouette(), 128), false, QStringLiteral("all"));
    QCOMPARE(labelAt(*m_set, 100, 100), a);
    m_context->setActiveLabel(0);
    PartBrushTool eraser(m_state, m_context, PartBrushTool::Mode::Erase);
    eraser.press(m_view, at(100, 100));
    eraser.release(m_view, at(100, 100));
    QCOMPARE(labelAt(*m_set, 100, 100), 0);
    QCOMPARE(labelAt(*m_set, 100, 150), a);
    // Ctrl+click picks the part under the cursor.
    eraser.press(m_view, at(100, 150, Qt::ControlModifier));
    QCOMPARE(m_context->activeLabel(), a);
}

void TestPartsUi::lassoAndWand()
{
    const int a = m_set->addPart(QStringLiteral("topwear"));
    m_context->setActiveLabel(a);
    PartLassoTool lasso(m_state, m_context);
    lasso.press(m_view, at(70, 70));
    for (const QPointF &p : {QPointF(130, 70), QPointF(130, 130), QPointF(70, 130)})
        lasso.move(m_view, at(p.x(), p.y()));
    lasso.release(m_view, at(70, 130));
    QCOMPARE(labelAt(*m_set, 100, 100), a);
    QCOMPARE(labelAt(*m_set, 140, 100), 0);
    // Alt removes.
    lasso.press(m_view, at(90, 90, Qt::AltModifier));
    for (const QPointF &p : {QPointF(110, 90), QPointF(110, 110), QPointF(90, 110)})
        lasso.move(m_view, at(p.x(), p.y(), Qt::AltModifier));
    lasso.release(m_view, at(90, 110));
    QCOMPARE(labelAt(*m_set, 100, 100), 0);
    QCOMPARE(labelAt(*m_set, 75, 75), a);

    const int green = m_set->addPart(QStringLiteral("eyewear"));
    m_context->setActiveLabel(green);
    PartWandTool wand(m_state, m_context);
    wand.press(m_view, at(70, 40));
    QCOMPARE(labelAt(*m_set, 85, 55), green);
    QCOMPARE(labelAt(*m_set, 95, 55), 0); // red body is a different colour
    QCOMPARE(m_set->part(green)->pixels, 30 * 30);
}

void TestPartsUi::cutLineSplits()
{
    const int a = m_set->addPart(QStringLiteral("handwear"), QStringLiteral("Arm"));
    m_context->setActiveLabel(a);
    m_context->assign(img::thresholdMask(m_set->silhouette(), 128), false, QStringLiteral("all"));
    PartCutTool cut(m_state, m_context);
    cut.press(m_view, at(100, 95));
    cut.move(m_view, at(100, 105));
    cut.release(m_view, at(100, 105));
    QTRY_COMPARE(m_set->parts().size(), 2);
    QCOMPARE(m_set->parts()[1].name, QStringLiteral("Arm 2"));
    QVERIFY(labelAt(*m_set, 60, 100) != labelAt(*m_set, 140, 100));
}

void TestPartsUi::hiddenAreaBrush()
{
    const int a = m_set->addPart(QStringLiteral("topwear"));
    m_context->setActiveLabel(a);
    QImage left = img::createMask(m_set->size(), 0);
    for (int y = 20; y < 180; ++y)
        std::fill(left.scanLine(y) + 50, left.scanLine(y) + 100, uchar(255));
    m_context->assign(left, false, QStringLiteral("left"));
    QSignalSpy preview(m_context, &PartEditContext::extensionPreview);
    PartBrushTool extend(m_state, m_context, PartBrushTool::Mode::Extend);
    extend.press(m_view, at(130, 100));
    extend.release(m_view, at(130, 100));
    QVERIFY(!preview.isEmpty());
    QVERIFY(!m_set->part(a)->extensionAuto);
    QCOMPARE(qGray(m_set->extensionMask(a).pixel(130, 100)), 255);
    QCOMPARE(labelAt(*m_set, 130, 100), 0); // visible mask unchanged
    m_set->undoStack()->undo();
    QVERIFY(m_set->part(a)->extensionAuto);
}

void TestPartsUi::puppetRenders()
{
    PuppetPreview preview;
    QImage face(40, 40, QImage::Format_ARGB32_Premultiplied);
    face.fill(QColor(250, 220, 200));
    QImage hair(60, 30, QImage::Format_ARGB32_Premultiplied);
    hair.fill(QColor(40, 60, 160));
    QImage closed(40, 8, QImage::Format_ARGB32_Premultiplied);
    closed.fill(Qt::black);
    preview.setLayers({PuppetLayer{QStringLiteral("Face"), QStringLiteral("face"), QStringLiteral("head"), {}, face, QRect(80, 40, 40, 40), 0.6, true},
                       PuppetLayer{QStringLiteral("Bangs"), QStringLiteral("front hair"), QStringLiteral("head"), {}, hair, QRect(70, 30, 60, 30), 0.8, true},
                       PuppetLayer{QStringLiteral("Eyes closed"), QStringLiteral("eyes closed"), QStringLiteral("head"), {}, closed, QRect(80, 55, 40, 8), 0.67, false}},
                      QSize(200, 200));
    PuppetPreview::Params p;
    p.angleX = 25;
    p.hair = 1.0;
    p.eyeOpen = 1.0;
    preview.setParams(p);
    const QImage open = preview.renderFrame(QSize(200, 200));
    p.eyeOpen = 0.0;
    preview.setParams(p);
    const QImage shut = preview.renderFrame(QSize(200, 200));
    QVERIFY(open != shut); // the closed-eye drawing replaces the open eyes
    p.angleX = -25;
    preview.setParams(p);
    QVERIFY(preview.renderFrame(QSize(200, 200)) != shut); // head parallax moves the layers
}

void TestPartsUi::cubismViewAnimates()
{
    QTemporaryDir dir;
    const QString model3 = fake::writeModel(dir.path());
    QVERIFY(!model3.isEmpty());
    lumen::live2d::CubismCore::instance().setApiForTesting(fake::api(), QStringLiteral("fake"));
    CubismView view;
    QString error;
    QVERIFY(!view.loadModel(dir.filePath(QStringLiteral("missing.model3.json")), &error));
    QVERIFY(!error.isEmpty());
    QSignalSpy changed(&view, &CubismView::modelChanged);
    QVERIFY2(view.loadModel(model3, &error), qPrintable(error));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(view.modelName(), QStringLiteral("fake"));
    QVERIFY(view.hasIdleMotion());

    // Manual values override the animation; the idle motion drives the parameter otherwise.
    view.setAutoIdle(false);
    view.setManual(0, 30.0f);
    QCOMPARE(view.value(0), 30.0f);
    const QImage manual = view.renderNow(QSize(200, 200));
    QCOMPARE(manual.pixelColor(150, 90), QColor(0, 0, 255)); // eye moved right
    view.clearManual();
    view.setPlayMotion(false);
    QCOMPARE(view.value(0), 0.0f);
    view.setPlayMotion(true);
    view.resize(320, 320);
    view.show();
    QSignalSpy frames(&view, &CubismView::framePresented);
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 3, 5000);
    QVERIFY(view.value(0) != 0.0f); // the Idle curve moves ParamAngleX
    // Motion groups, played motions and expressions.
    QCOMPARE(view.motionGroups(), (QStringList{QStringLiteral("Idle"), QStringLiteral("TapBody")}));
    QCOMPARE(view.motionCount(QStringLiteral("Idle")), 2);
    QCOMPARE(view.motionLabel(QStringLiteral("TapBody"), 0), QStringLiteral("tap"));
    QCOMPARE(view.expressionNames(), QStringList{QStringLiteral("smile")});
    QVERIFY(view.hasPose());
    QSignalSpy motions(&view, &CubismView::motionChanged);
    QVERIFY(!view.playMotion(QStringLiteral("TapBody"), 5, &error));
    QVERIFY(view.playMotion(QStringLiteral("TapBody"), 0, &error));
    QTRY_VERIFY_WITH_TIMEOUT(view.currentMotionGroup() == QLatin1String("TapBody") && std::abs(view.value(0) - 20.0f) < 0.5f, 3000);
    QVERIFY(!motions.isEmpty());
    // After the one-shot motion an idle motion takes over again.
    QTRY_VERIFY_WITH_TIMEOUT(view.currentMotionGroup() == QLatin1String("Idle"), 4000);
    QVERIFY(view.setExpression(QStringLiteral("smile"), &error));
    QCOMPARE(view.currentExpression(), QStringLiteral("smile"));
    QVERIFY(!view.setExpression(QStringLiteral("angry"), &error));
    QVERIFY(view.setExpression(QString()));
    QCOMPARE(view.currentExpression(), QString());

    // The fake model's physics3.json swings ParamHairFront while ParamAngleX moves.
    QVERIFY(view.hasPhysics());
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(view.value(1)) > 0.02f, 5000);
    view.setPhysicsEnabled(false);
    view.setPlayMotion(false);
    view.setAutoIdle(false);
    QTRY_COMPARE(view.value(1), 0.0f);
    view.clear();
    QVERIFY(!view.hasModel());
    lumen::live2d::CubismCore::instance().unload();
}

QTEST_MAIN(TestPartsUi)
#include "tst_parts_ui.moc"
