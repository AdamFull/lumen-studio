#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/ContextBar.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/TransformTool.h"
#include "core/Settings.h"
#include "ui/Accessibility.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Shortcuts.h"
#include "ui/Studio.h"
#include "ui/assistant/AssistantPanel.h"
#include "ui/director/DirectorSession.h"
#include "ui/director/ShotCard.h"
#include "ui/pages/CreatePage.h"
#include "pose/Rig.h"
#include "MockHttpServer.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/LoraDatasetDialog.h"
#include "ui/CharacterTraining.h"
#include "runtime/RuntimeStore.h"
#include "core/Async.h"
#include "ai/Types.h"
#include "doc/CharacterLibrary.h"
#include "doc/LoraDataset.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/ModelHubDialog.h"
#include "runtime/ModelLibrary.h"
#include "ui/dialogs/ModelsPage.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/dialogs/ShortcutsPage.h"
#include "ui/widgets/ServerStatus.h"
#include "ai/Gate.h"
#include "ui/widgets/Controls.h"

#include <QComboBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTabWidget>
#include <QTreeWidget>
#include <QAction>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QListWidget>
#include <QSpinBox>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QToolButton>
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
    void contextBarFollowsSelection();
    void studioRestoresSession();
    void shortcutRegistry();
    void shortcutsHaveNoConflicts();
    void themeContrast();
    void accessibleNames();
    void fitLabelWraps();
    void criticRetriesShot();
    void assistantRunsReferenceScenario();
    void localServersWiring();
    void localSetupWizard();
    void characterLoraChecks();
    void loraTrainingSet();
    void loraTrainingRun();

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

void TestCanvas::contextBarFollowsSelection()
{
    m_view->resize(800, 600);
    m_view->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_view));
    ContextBar bar(m_view);
    QTest::qWait(100);
    QVERIFY(!bar.isVisible()); // no selection
    QImage sel = img::createMask(m_doc->size());
    QPainter(&sel).fillRect(QRect(40, 40, 40, 30), Qt::white);
    m_doc->applySelection(sel, QStringLiteral("Select"));
    QTRY_VERIFY(bar.isVisible());
    QCOMPARE(bar.anchorRect(), QRect(40, 40, 40, 30));
    const QRectF selRect = m_view->mapFromDoc(QRectF(40, 40, 40, 30));
    QVERIFY(bar.geometry().top() >= selRect.bottom()); // below the selection
    QVERIFY(m_view->rect().contains(bar.geometry()));

    // Enter in the prompt field asks for a generation with the prompt.
    QSignalSpy generate(&bar, &ContextBar::generateRequested);
    bar.setPrompt(QStringLiteral("a red hat"));
    auto *edit = bar.findChild<QLineEdit *>();
    QVERIFY(edit);
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(generate.size(), 1);
    QCOMPARE(generate.first().first().toString(), QStringLiteral("a red hat"));

    // Hidden while drawing, back afterwards; hidden when the user disables it.
    emit m_view->interactionChanged(true);
    QVERIFY(!bar.isVisible());
    emit m_view->interactionChanged(false);
    QTRY_VERIFY(bar.isVisible());
    bar.setEnabledByUser(false);
    QTRY_VERIFY(!bar.isVisible());
    bar.setEnabledByUser(true);
    QTRY_VERIFY(bar.isVisible());
    // The close button hides it until the selection changes.
    auto *close = bar.findChild<QToolButton *>(QStringLiteral("ContextBarClose"));
    QVERIFY(close);
    close->click();
    QVERIFY(!bar.isVisible());
    m_doc->clearSelection();
    QTest::qWait(100);
    QVERIFY(!bar.isVisible());
    m_doc->applySelection(sel, QStringLiteral("Select"));
    QTRY_VERIFY(bar.isVisible());
}

void TestCanvas::studioRestoresSession()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("session.lumen"));
    QString firstId, secondId;
    {
        Studio studio;
        studio.openProject(path);
        QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
        doc::Document *a = studio.newDocument(QSize(64, 48), QStringLiteral("First"));
        doc::Document *b = studio.newDocument(QSize(32, 32), QStringLiteral("Second"));
        // A change is saved shortly after the user stops editing (no explicit save).
        a->addLayer(QStringLiteral("Paint"));
        QTRY_VERIFY_WITH_TIMEOUT(!a->id().isEmpty() && !a->isModified(), Studio::kIdleSaveMs + 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!b->id().isEmpty() && !b->isModified(), 5000);
        firstId = a->id();
        secondId = b->id();
        studio.setActiveDocument(a);
        auto saved = studio.saveSession();
        QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 5000);
        // Simulated crash: the studio goes away without closing its documents.
    }
    Studio studio;
    QSignalSpy restored(&studio, &Studio::sessionRestored);
    studio.openProject(path);
    QTRY_VERIFY_WITH_TIMEOUT(restored.size() == 1, 10000);
    QCOMPARE(studio.documents().size(), 2);
    QStringList ids;
    for (doc::Document *d : studio.documents())
        ids << d->id();
    QVERIFY(ids.contains(firstId) && ids.contains(secondId));
    QCOMPARE(studio.activeDocument()->id(), firstId);
    QCOMPARE(studio.activeDocument()->layerCount(), 2);
    // Closing a document removes it from the session.
    studio.closeDocument(studio.activeDocument());
    QTest::qWait(700);
    auto records = studio.project()->records(QStringLiteral("session"));
    QTRY_VERIFY(records.isFinished());
    const QJsonArray docs = records.result().first().json.value(QStringLiteral("documents")).toArray();
    QCOMPARE(docs.size(), 1);
    QCOMPARE(docs.first().toString(), secondId);
}

void TestCanvas::shortcutRegistry()
{
    Shortcuts &sc = Shortcuts::instance();
    auto *a = new QAction(QStringLiteral("Alpha"));
    auto *b = new QAction(QStringLiteral("Beta"));
    auto *g = new QAction(QStringLiteral("Global"));
    a->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    sc.reset(QStringLiteral("test.alpha"));
    sc.reset(QStringLiteral("test.beta"));
    sc.bindAction(a, QStringLiteral("test.alpha"), QStringLiteral("Test"), QStringLiteral("test-scope"), {}, QString(), true);
    sc.bindAction(b, QStringLiteral("test.beta"), QStringLiteral("Test"), QStringLiteral("other-scope"), {QKeySequence(QStringLiteral("Ctrl+K"))});
    sc.bindAction(g, QStringLiteral("test.global"), QStringLiteral("Test"), QStringLiteral("global"), {QKeySequence(QStringLiteral("F9"))});
    QCOMPARE(sc.info(QStringLiteral("test.alpha")).name, QStringLiteral("Alpha"));
    QCOMPARE(sc.keys(QStringLiteral("test.alpha")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+K"))});
    QCOMPARE(a->toolTip(), QStringLiteral("Alpha (%1)").arg(QKeySequence(QStringLiteral("Ctrl+K")).toString(QKeySequence::NativeText)));
    // Same key in different page scopes is fine; the global scope conflicts with everything.
    QVERIFY(sc.conflicts(QStringLiteral("test.alpha"), QKeySequence(QStringLiteral("Ctrl+K"))).isEmpty());
    QCOMPARE(sc.conflicts(QStringLiteral("test.alpha"), QKeySequence(QStringLiteral("F9"))), QStringList{QStringLiteral("test.global")});

    // Overrides apply to the action at once and persist; resetting restores the default.
    sc.setKeys(QStringLiteral("test.alpha"), {QKeySequence(QStringLiteral("Ctrl+Alt+K")), QKeySequence(QStringLiteral("F7"))});
    QCOMPARE(a->shortcuts().size(), 2);
    QCOMPARE(a->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));
    QVERIFY(sc.isCustomized(QStringLiteral("test.alpha")));
    QVERIFY(Settings::instance().contains(QStringLiteral("shortcuts/test.alpha")));
    auto *again = new QAction(QStringLiteral("Alpha 2")); // a second window binding the same id gets the override
    sc.bindAction(again, QStringLiteral("test.alpha"), QStringLiteral("Test"), QStringLiteral("test-scope"));
    QCOMPARE(again->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));
    sc.reset(QStringLiteral("test.alpha"));
    QCOMPARE(a->shortcut(), QKeySequence(QStringLiteral("Ctrl+K")));
    QVERIFY(!sc.isCustomized(QStringLiteral("test.alpha")));
    // Setting the defaults explicitly does not count as a customisation; an empty list removes the key.
    sc.setKeys(QStringLiteral("test.alpha"), {QKeySequence(QStringLiteral("Ctrl+K"))});
    QVERIFY(!sc.isCustomized(QStringLiteral("test.alpha")));
    sc.setKeys(QStringLiteral("test.alpha"), {});
    QVERIFY(a->shortcuts().isEmpty());
    QCOMPARE(a->toolTip(), QStringLiteral("Alpha"));

    // The settings page moves a key that is taken in a conflicting scope.
    ShortcutsPage page;
    const QStringList moved = page.assign(QStringLiteral("test.alpha"), QKeySequence(QStringLiteral("F9")), {});
    QCOMPARE(moved, QStringList{QStringLiteral("test.global")});
    QVERIFY(g->shortcuts().isEmpty());
    QCOMPARE(a->shortcut(), QKeySequence(QStringLiteral("F9")));
    sc.reset(QStringLiteral("test.alpha"));
    sc.reset(QStringLiteral("test.global"));
    QCOMPARE(g->shortcut(), QKeySequence(QStringLiteral("F9")));
    delete a;
    delete b;
    delete g;
    delete again;
    // Bindings of deleted actions are dropped.
    sc.setKeys(QStringLiteral("test.alpha"), {QKeySequence(QStringLiteral("F8"))});
    sc.reset(QStringLiteral("test.alpha"));
}

void TestCanvas::shortcutsHaveNoConflicts()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Studio studio;
    MainWindow window(&studio);
    Shortcuts &sc = Shortcuts::instance();
    int checked = 0;
    for (const Shortcuts::Info &info : sc.all()) {
        if (info.id.startsWith(QLatin1String("test.")))
            continue;
        QVERIFY2(!info.name.isEmpty(), qPrintable(info.id));
        for (const QKeySequence &key : sc.keys(info.id)) {
            const QStringList others = sc.conflicts(info.id, key);
            QVERIFY2(others.isEmpty(), qPrintable(QStringLiteral("%1 (%2) conflicts with %3")
                                                       .arg(info.id, key.toString(), others.join(QStringLiteral(", ")))));
            ++checked;
        }
    }
    QVERIFY(checked > 40);
    QVERIFY(sc.contains(QStringLiteral("editor.tool.transform")));
    QVERIFY(sc.contains(QStringLiteral("parts.tool.part-lasso")) || sc.contains(QStringLiteral("parts.tool.lasso")));
    QVERIFY(sc.contains(QStringLiteral("app.settings")));
}

void TestCanvas::themeContrast()
{
    QCOMPARE(qRound(Theme::contrastRatio(Qt::black, Qt::white) * 10), 210);
    QCOMPARE(Theme::contrastRatio(QColor(0x777777), QColor(0x777777)), 1.0);
    QCOMPARE(Theme::resolve(QStringLiteral("light")), QStringLiteral("light"));
    QCOMPARE(Theme::resolve(QStringLiteral("auto")),
             Theme::systemHighContrast() ? QStringLiteral("high-contrast") : QStringLiteral("dark"));
    for (const QString &name : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("high-contrast")}) {
        const Palette p = Theme::paletteFor(name);
        const bool hc = name == QLatin1String("high-contrast");
        auto check = [&](const char *what, const QColor &fg, const QColor &bg, double minimum) {
            const double ratio = Theme::contrastRatio(fg, bg);
            QVERIFY2(ratio >= minimum, qPrintable(QStringLiteral("%1 %2: %3 < %4").arg(name, QLatin1String(what)).arg(ratio, 0, 'f', 2).arg(minimum)));
        };
        // WCAG 2.1: body text AA 4.5:1 (AAA 7:1 for high contrast), UI components 3:1.
        for (const QColor &bg : {p.window, p.panel, p.surface, p.raised}) {
            check("text", p.text, bg, 7.0);
            check("muted text", p.textMuted, bg, hc ? 7.0 : 4.5);
        }
        for (const QColor &bg : {p.window, p.panel, p.surface}) {
            check("danger", p.danger, bg, 4.5);
            check("success", p.success, bg, 4.5);
            check("warning", p.warning, bg, 4.5);
            check("accent", p.accent, bg, 3.0);
        }
        check("text on accent", p.accentText, p.accent, hc ? 7.0 : 4.5);
        check("text on accent hover", p.accentText, p.accentHover, 4.5);
        if (hc)
            check("border", p.border, p.window, 3.0);
    }
}

void TestCanvas::accessibleNames()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Accessibility::install();
    Studio studio;
    MainWindow window(&studio);
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // Every page (and its widgets) gets polished once shown.
    for (Page page : {Page::Create, Page::Generate, Page::Editor, Page::Poses, Page::Parts, Page::Library}) {
        window.showPage(page);
        QTest::qWait(30);
    }
    QStringList missing;
    for (QWidget *w : Accessibility::unnamed(&window)) {
        if (w->isVisibleTo(&window))
        {
            QStringList chain;
            for (QWidget *p = w->parentWidget(); p && p != &window; p = p->parentWidget())
                chain << QLatin1String(p->metaObject()->className()) + (p->objectName().isEmpty() ? QString() : u':' + p->objectName());
            missing << QStringLiteral("%1 '%2' in %3").arg(QLatin1String(w->metaObject()->className()), w->objectName(),
                                                         chain.join(QStringLiteral(" < ")));
        }
    }
    QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral("\n"))));
    // An icon-only button gets its tool tip (without the shortcut suffix) as its name.
    QToolButton icon(&window);
    icon.setToolTip(QStringLiteral("Zoom in (Ctrl+=)"));
    QCOMPARE(Accessibility::ensureName(&icon), QStringLiteral("Zoom in"));
}

void TestCanvas::fitLabelWraps()
{
    double pt = 8.0;
    QCOMPARE(fitLabel(QStringLiteral("Edit"), 68, &pt), QStringLiteral("Edit"));
    QCOMPARE(pt, 8.0);
    const QString two = fitLabel(QStringLiteral("Per Anweisung bearbeiten"), 68, &pt);
    QVERIFY(two.contains(u'\n'));
    pt = 8.0;
    fitLabel(QStringLiteral("Warteschlangenverwaltung"), 40, &pt);
    QVERIFY(pt < 8.0 && pt >= 7.0);
}

void TestCanvas::criticRetriesShot()
{
    MockHttpServer server;
    QStringList prompts;
    server.route("POST", QStringLiteral("/sdapi/v1/txt2img"), [&](const MockRequest &req) {
        prompts << req.json().value(QStringLiteral("prompt")).toString();
        QImage im(64, 64, QImage::Format_ARGB32);
        im.fill(prompts.size() == 1 ? Qt::red : Qt::green);
        return MockResponse::json({{QStringLiteral("images"), QJsonArray{QString::fromLatin1(img::encode(im).toBase64())}},
                                   {QStringLiteral("info"), QStringLiteral("{\"seed\": 1, \"all_seeds\": [1]}")}});
    });
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [](const MockRequest &) { return MockResponse::json({}); });
    server.route("GET", QStringLiteral("/controlnet/model_list"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model_list"), QJsonArray{QStringLiteral("control_openpose [abc]")}}});
    });
    int critiques = 0;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &) {
        ++critiques;
        const int score = critiques == 1 ? 3 : 9;
        const QJsonObject verdict{{QStringLiteral("overall"), score},
                                  {QStringLiteral("identity"), score},
                                  {QStringLiteral("pose"), score},
                                  {QStringLiteral("issues"), QJsonArray{QStringLiteral("arms too low")}},
                                  {QStringLiteral("revised_prompt"), QStringLiteral("1girl, a-pose, arms raised 35 degrees")},
                                  {QStringLiteral("negative_additions"), QStringLiteral("extra fingers")}};
        return MockResponse::json(
            {{QStringLiteral("choices"),
              QJsonArray{QJsonObject{{QStringLiteral("message"),
                                      QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(verdict).toJson(QJsonDocument::Compact))}}},
                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });

    Settings::instance().setValue(QStringLiteral("director/critic"), true);
    Settings::instance().setValue(QStringLiteral("director/criticThreshold"), 7);
    Settings::instance().setValue(QStringLiteral("director/criticRetries"), 2);
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdwebui"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Vision"),
                                            {{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}}));
    studio.openProject(dir.filePath(QStringLiteral("critic.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    CreatePage page(&studio);
    QTRY_VERIFY(page.findChild<DirectorSession *>());
    DirectorSession *session = page.findChild<DirectorSession *>();
    const QJsonObject shot{{QStringLiteral("id"), QStringLiteral("front")},
                           {QStringLiteral("title"), QStringLiteral("Front")},
                           {QStringLiteral("description"), QStringLiteral("A-pose")},
                           {QStringLiteral("aspect_ratio"), QStringLiteral("1:1")},
                           {QStringLiteral("background"), QStringLiteral("plain grey")},
                           {QStringLiteral("prompt"), QStringLiteral("1girl, a-pose")},
                           {QStringLiteral("negative_prompt"), QStringLiteral("text")},
                           {QStringLiteral("pose"), pose::RigParams().toJson()}};
    session->setPlan(QJsonObject{{QStringLiteral("title"), QStringLiteral("Test")}, {QStringLiteral("shots"), QJsonArray{shot}}});
    for (QSpinBox *spin : page.findChildren<QSpinBox *>()) {
        if (spin->suffix().contains(QLatin1String("per shot")))
            spin->setValue(1);
    }
    QTRY_VERIFY(page.findChild<ShotCard *>());
    emit page.findChild<ShotCard *>()->generateRequested(QStringLiteral("front"));

    // First result scores 3 → one retry with the revised prompt → 9 is accepted, no further retry.
    QTRY_VERIFY_WITH_TIMEOUT(critiques == 2 && session->shot(QStringLiteral("front"))->critiques.size() == 2, 20000);
    QTest::qWait(300);
    QCOMPARE(prompts.size(), 2);
    QCOMPARE(critiques, 2);
    QVERIFY(prompts[1].contains(QLatin1String("arms raised 35 degrees")));
    QVERIFY(!prompts[0].contains(QLatin1String("arms raised")));
    const ShotState *state = session->shot(QStringLiteral("front"));
    QCOMPARE(state->results.size(), 2);
    QCOMPARE(state->critiques.value(state->results[0]).toObject().value(QStringLiteral("score")).toInt(), 3);
    QCOMPARE(state->critiques.value(state->results[1]).toObject().value(QStringLiteral("score")).toInt(), 9);
    // The verdict is also stored with the library asset.
    QTRY_COMPARE(studio.project()->asset(state->results[1])->metadata.value(QStringLiteral("critic")).toObject().value(QStringLiteral("score")).toInt(), 9);
    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
}

void TestCanvas::assistantRunsReferenceScenario()
{
    // Reference scenario through the tool-calling assistant: plan Live2D shots, generate them.
    MockHttpServer server;
    int txt2img = 0;
    server.route("POST", QStringLiteral("/sdapi/v1/txt2img"), [&](const MockRequest &) {
        ++txt2img;
        QImage im(64, 64, QImage::Format_ARGB32);
        im.fill(Qt::magenta);
        return MockResponse::json({{QStringLiteral("images"), QJsonArray{QString::fromLatin1(img::encode(im).toBase64())}},
                                   {QStringLiteral("info"), QStringLiteral("{\"seed\": 1, \"all_seeds\": [1]}")}});
    });
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [](const MockRequest &) { return MockResponse::json({}); });
    server.route("GET", QStringLiteral("/controlnet/model_list"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model_list"), QJsonArray{QStringLiteral("control_openpose [abc]")}}});
    });

    QJsonObject shot{{QStringLiteral("id"), QStringLiteral("front")},
                     {QStringLiteral("title"), QStringLiteral("Front A-pose")},
                     {QStringLiteral("description"), QStringLiteral("rig-ready front view")},
                     {QStringLiteral("aspect_ratio"), QStringLiteral("2:3")},
                     {QStringLiteral("background"), QStringLiteral("plain grey")},
                     {QStringLiteral("prompt"), QStringLiteral("1girl, a-pose, front view")},
                     {QStringLiteral("negative_prompt"), QString()}};
    QJsonObject pose = pose::RigParams().toJson();
    pose.remove(QStringLiteral("include_hands"));
    pose.remove(QStringLiteral("include_face"));
    pose.remove(QStringLiteral("name"));
    shot.insert(QStringLiteral("pose"), pose);
    const QJsonObject plan{{QStringLiteral("title"), QStringLiteral("Live2D source")},
                           {QStringLiteral("summary"), QStringLiteral("one front shot")},
                           {QStringLiteral("shots"), QJsonArray{shot}}};
    int agentSteps = 0;
    QList<QJsonObject> agentRequests;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        const QJsonObject body = req.json();
        auto reply = [](const QJsonObject &message, const QString &finish) {
            return MockResponse::json({{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), message},
                                                                                         {QStringLiteral("finish_reason"), finish}}}}});
        };
        if (!body.contains(QStringLiteral("tools"))) // the director's structured planning call
            return reply(QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(plan).toJson(QJsonDocument::Compact))}},
                         QStringLiteral("stop"));
        agentRequests << body;
        ++agentSteps;
        auto call = [&](const QString &name, const QJsonObject &args) {
            const QJsonObject c{{QStringLiteral("id"), QStringLiteral("call_%1").arg(agentSteps)},
                                {QStringLiteral("type"), QStringLiteral("function")},
                                {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name},
                                                                         {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
            return reply(QJsonObject{{QStringLiteral("content"), QJsonValue()}, {QStringLiteral("tool_calls"), QJsonArray{c}}},
                         QStringLiteral("tool_calls"));
        };
        switch (agentSteps) {
        case 1: return call(QStringLiteral("get_state"), {});
        case 2: return call(QStringLiteral("plan_shots"), {{QStringLiteral("request"), QStringLiteral("three Live2D source images")},
                                                            {QStringLiteral("purpose"), QStringLiteral("live2d")},
                                                            {QStringLiteral("count"), 1}});
        case 3: return call(QStringLiteral("generate_shots"), {});
        default:
            return reply(QJsonObject{{QStringLiteral("content"), QStringLiteral("Done — the front shot is ready.")}}, QStringLiteral("stop"));
        }
    });

    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdwebui"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("LLM"),
                                            {{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}}));
    studio.openProject(dir.filePath(QStringLiteral("assistant.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    CreatePage create(&studio);
    QTRY_VERIFY(studio.directorActions());
    for (QSpinBox *spin : create.findChildren<QSpinBox *>()) {
        if (spin->suffix().contains(QLatin1String("per shot")))
            spin->setValue(1);
    }
    AssistantPanel panel(&studio);
    QSignalSpy finished(&panel, &AssistantPanel::runFinished);
    panel.send(QStringLiteral("I need three images of my character for a Live2D avatar."));
    QVERIFY(panel.isRunning());
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 20000);
    QCOMPARE(panel.lastReply(), QStringLiteral("Done — the front shot is ready."));
    QCOMPARE(agentSteps, 4);
    QCOMPARE(txt2img, 1);
    // The director state the model saw, the plan, and the generated asset reported back.
    QVERIFY(QJsonDocument(agentRequests[1]).toJson().contains("purposes"));
    // plan_shots reports the stored plan (title and shot ids) to the model.
    QVERIFY(QJsonDocument(agentRequests[2]).toJson().contains("Front A-pose"));
    const QJsonObject state = studio.directorActions()->directorState();
    QCOMPARE(state.value(QStringLiteral("purpose")).toString(), QStringLiteral("live2d"));
    const QJsonArray shots = state.value(QStringLiteral("shots")).toArray();
    QCOMPARE(shots.size(), 1);
    const QString asset = shots[0].toObject().value(QStringLiteral("results")).toArray().first().toString();
    QVERIFY(!asset.isEmpty());
    QVERIFY(QJsonDocument(agentRequests[3]).toJson().contains(asset.toUtf8()));
    // The conversation keeps the tool exchange for the next turn.
    int toolMessages = 0;
    for (const ai::ChatMessage &m : panel.history())
        toolMessages += m.role == ai::ChatRole::Tool ? 1 : 0;
    QCOMPARE(toolMessages, 3);
    // A new chat starts over with only the system prompt.
    panel.newChat();
    QCOMPARE(panel.history().size(), 1);
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
}

void TestCanvas::localServersWiring()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    // A fake llama-server (see tests/fake_server.cpp), adopted as an existing install.
    const QString exe = dir.filePath(QStringLiteral("llama/llama-server.exe"));
    QDir().mkpath(QFileInfo(exe).absolutePath());
    QVERIFY(QFile::copy(QStringLiteral(FAKE_SERVER_PATH), exe));
    const QString model = dir.filePath(QStringLiteral("model.gguf"));
    {
        QFile f(model);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("gguf");
    }
    Studio studio;
    runtime::ServerSupervisor *servers = studio.servers();
    servers->healthIntervalMs = 100;
    for (const runtime::ServerProfile &old : servers->profiles())
        servers->removeProfile(old.id);
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/test-fake");
    rt.kind = QStringLiteral("llama.cpp");
    rt.executable = exe;
    rt.directory = QFileInfo(exe).absolutePath();
    studio.runtimes()->addInstalled(rt);
    runtime::ServerProfile profile = servers->createProfile(rt.id, QStringLiteral("Local Qwen"));
    profile.options = {{QStringLiteral("model"), model}, {QStringLiteral("extra_args"), QStringLiteral("--fake-ready-ms 100")}};
    servers->addProfile(profile);

    // A provider bound to the server gets a gate and the server's URL.
    ai::Provider *provider = studio.providers()->addProvider(
        QStringLiteral("llamacpp"), QStringLiteral("Local Qwen"),
        {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1/v1")}, {QStringLiteral("managed_server"), profile.id}});
    QVERIFY(provider);
    QVERIFY(provider->gate());
    QCOMPARE(provider->text(QStringLiteral("url")), runtime::providerUrl(QStringLiteral("llama.cpp"), profile.port));
    QCOMPARE(studio.providerForServer(profile.id), provider);

    MainWindow window(&studio);
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *chip = window.findChild<ServerStatusButton *>();
    QVERIFY(chip && chip->isVisible());
    QCOMPARE(chip->text(), QStringLiteral("Servers stopped"));

    // The first request starts the server.
    QFuture<QString> ready = provider->gate()->acquire();
    QTRY_VERIFY_WITH_TIMEOUT(ready.isFinished(), 20000);
    QCOMPARE(ready.result(), QString());
    QCOMPARE(servers->state(profile.id), runtime::ServerState::Ready);
    QCOMPARE(chip->text(), QStringLiteral("1 server(s) running"));
    provider->gate()->release();

    // Settings → Local servers lists the program and the server; the provider form shows the binding.
    {
        SettingsDialog dlg(&studio, &window);
        dlg.showTab(QStringLiteral("servers"));
        dlg.show();
        auto *page = dlg.findChild<LocalServersPage *>();
        QVERIFY(page && page->isVisible());
        const QList<QTreeWidget *> trees = page->findChildren<QTreeWidget *>();
        QCOMPARE(trees.size(), 2);
        QCOMPARE(trees[0]->topLevelItemCount(), 1);
        QCOMPARE(trees[1]->topLevelItemCount(), 1);
        QCOMPARE(trees[1]->topLevelItem(0)->text(0), QStringLiteral("Local Qwen"));
        QCOMPARE(trees[1]->topLevelItem(0)->text(4), QStringLiteral("Running"));
        const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
        if (!shots.isEmpty()) {
            QTest::qWait(300);
            dlg.grab().save(shots + QStringLiteral("/local-servers.png"));
            ServerProfileDialog profileDlg(&studio, profile, false, &dlg);
            profileDlg.show();
            QTest::qWait(200);
            profileDlg.grab().save(shots + QStringLiteral("/server-profile.png"));
            InstallRuntimeDialog install(&studio, std::nullopt, QString(), &dlg);
            install.show();
            QTRY_VERIFY_WITH_TIMEOUT(install.findChildren<QPushButton *>().size() > 1, 1000);
            QTest::qWait(4000); // hardware probe + GitHub
            install.grab().save(shots + QStringLiteral("/install.png"));
            window.grab().save(shots + QStringLiteral("/window.png"));
            // The model library with real folders (LUMEN_SCREENSHOT_MODEL_DIRS=<dir>;<dir>).
            const QStringList modelDirs = qEnvironmentVariable("LUMEN_SCREENSHOT_MODEL_DIRS").split(QLatin1Char(';'), Qt::SkipEmptyParts);
            if (!modelDirs.isEmpty()) {
                QSignalSpy scanned(studio.models(), &runtime::ModelLibrary::scanFinished);
                for (const QString &d : modelDirs)
                    studio.models()->addRoot(d);
                QTRY_VERIFY_WITH_TIMEOUT(!studio.models()->isScanning() && scanned.size() >= 1, 60000);
                dlg.showTab(QStringLiteral("models"));
                QTest::qWait(1500); // hardware probe
                auto *list = dlg.findChild<ModelsPage *>()->findChild<QTreeWidget *>();
                for (int i = 0; i < list->topLevelItemCount(); ++i) {
                    if (list->topLevelItem(i)->text(0).startsWith(QLatin1String("Qwen3VL")))
                        list->setCurrentItem(list->topLevelItem(i));
                }
                QTest::qWait(300);
                dlg.resize(1200, 760);
                QTest::qWait(300);
                dlg.grab().save(shots + QStringLiteral("/models.png"));
                for (const runtime::ModelRoot &r : studio.models()->roots())
                    studio.models()->removeRoot(r.path);
            }
        }
        dlg.showTab(QStringLiteral("providers"));
        QTest::qWait(50);
        auto *form = dlg.findChild<ProviderForm *>();
        QVERIFY(form);
        QComboBox *serverCombo = nullptr;
        for (QComboBox *c : form->findChildren<QComboBox *>()) {
            if (c->findData(profile.id) >= 0)
                serverCombo = c;
        }
        // The list opens on the first provider; select ours if needed.
        if (!serverCombo || serverCombo->currentData().toString() != profile.id) {
            auto *list = dlg.findChild<QListWidget *>();
            for (int i = 0; list && i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == provider->instanceId())
                    list->setCurrentRow(i);
            }
            QTest::qWait(50);
            form = dlg.findChild<ProviderForm *>();
            for (QComboBox *c : form->findChildren<QComboBox *>()) {
                if (c->findData(profile.id) >= 0)
                    serverCombo = c;
            }
        }
        QVERIFY(serverCombo);
        QCOMPARE(serverCombo->currentData().toString(), profile.id);
        // Saving the form keeps the binding.
        for (QPushButton *b : form->findChildren<QPushButton *>()) {
            if (b->text() == QLatin1String("Save"))
                b->click();
        }
        QCOMPARE(studio.providers()->provider(provider->instanceId())->text(QStringLiteral("managed_server")), profile.id);
    }

    // Profile dialog: options form and command preview.
    {
        ServerProfileDialog dlg(&studio, profile, false, &window);
        const runtime::ServerProfile edited = dlg.profile();
        QCOMPARE(edited.options.value(QStringLiteral("model")).toString(), model);
        QCOMPARE(edited.options.value(QStringLiteral("extra_args")).toString(), QStringLiteral("--fake-ready-ms 100"));
        QVERIFY(!edited.options.contains(QStringLiteral("context"))); // defaults are not stored
        auto *preview = dlg.findChild<QPlainTextEdit *>();
        QVERIFY(preview && preview->toPlainText().contains(QStringLiteral("--port %1").arg(profile.port)));
    }

    servers->stop(profile.id);
    QTRY_COMPARE_WITH_TIMEOUT(servers->state(profile.id), runtime::ServerState::Stopped, 10000);
    studio.providers()->removeProvider(provider->instanceId());
    servers->removeProfile(profile.id);
    studio.runtimes()->remove(rt.id);
}

void TestCanvas::localSetupWizard()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    runtime::ServerSupervisor *servers = studio.servers();
    const QString own = studio.models()->defaultFolder();
    // Leaves the (persistent) test data folder as it found it, also when a check fails.
    auto cleanup = [&studio, servers, own, &dir] {
        for (const runtime::ServerProfile &p : servers->profiles()) {
            if (ai::Provider *provider = studio.providerForServer(p.id))
                studio.providers()->removeProvider(provider->instanceId());
            servers->removeProfile(p.id);
        }
        studio.models()->moveToTrash = [](const QString &path, QString *) { return QFile::remove(path); };
        for (const runtime::ModelEntry &e : studio.models()->entries()) {
            if (e.path.startsWith(own))
                studio.models()->trash(e.path);
        }
        studio.models()->removeRoot(QDir::cleanPath(dir.filePath(QStringLiteral("comfy"))));
        for (const QString &kind : {QStringLiteral("llama.cpp"), QStringLiteral("sd.cpp")})
            studio.runtimes()->remove(QStringLiteral("adopted/setup-") + kind);
    };
    cleanup();
    auto guard = qScopeGuard(cleanup);
    // Installed programs are reused: fake servers adopted for both kinds.
    for (const QString &kind : {QStringLiteral("llama.cpp"), QStringLiteral("sd.cpp")}) {
        const QString exe = dir.filePath(kind + QStringLiteral("/server.exe"));
        QDir().mkpath(QFileInfo(exe).absolutePath());
        QVERIFY(QFile::copy(QStringLiteral(FAKE_SERVER_PATH), exe));
        runtime::InstalledRuntime rt;
        rt.id = QStringLiteral("adopted/setup-") + kind;
        rt.kind = kind;
        rt.executable = exe;
        rt.directory = QFileInfo(exe).absolutePath();
        studio.runtimes()->addInstalled(rt);
    }
    // A starter set of small files served by a mock Hugging Face; content differs per run.
    MockHttpServer hf;
    studio.hub()->setBaseUrls(hf.baseUrl().toString(), hf.baseUrl().toString());
    const QString revision = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    const QByteArray stamp = QByteArray::number(QDateTime::currentMSecsSinceEpoch());
    struct Item
    {
        const char *id, *role, *kind, *path;
    };
    const QList<Item> items{{"lm", "language", "llm", "Tiny-Q4_K_M.gguf"},
                            {"proj", "projector", "projector", "mmproj-Tiny-Q8_0.gguf"},
                            {"ckpt", "checkpoint", "checkpoint", "tiny-xl.safetensors"},
                            {"cn", "control_net", "controlnet", "cn/diffusion_pytorch_model.safetensors"},
                            {"ipa", "ip_adapter", "ipadapter", "ip-adapter_tiny.safetensors"},
                            {"clip", "clip_vision", "clip_vision", "image_encoder/model.safetensors"},
                            {"up", "upscaler", "upscaler", "TinyESRGAN_x4.safetensors"}};
    QJsonArray files;
    QJsonArray ids;
    QMap<QString, QByteArray> contents;
    for (const Item &it : items) {
        const QByteArray data = QByteArray(2 * 1024 * 1024, 'x') + it.id + stamp;
        contents.insert(QLatin1String(it.role), data);
        files.append(QJsonObject{{QStringLiteral("id"), QLatin1String(it.id)},
                                 {QStringLiteral("role"), QLatin1String(it.role)},
                                 {QStringLiteral("kind"), QLatin1String(it.kind)},
                                 {QStringLiteral("name"), QStringLiteral("Tiny %1").arg(QLatin1String(it.id))},
                                 {QStringLiteral("repo"), QStringLiteral("org/tiny")},
                                 {QStringLiteral("revision"), revision},
                                 {QStringLiteral("path"), QLatin1String(it.path)},
                                 {QStringLiteral("saveAs"), QLatin1String(it.id) == QLatin1String("cn") ? QStringLiteral("tiny-controlnet.safetensors") : QString()},
                                 {QStringLiteral("size"), data.size()},
                                 {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())},
                                 {QStringLiteral("license"), QStringLiteral("apache-2.0")}});
        ids.append(QLatin1String(it.id));
        if (QLatin1String(it.role) == QLatin1String("upscaler"))
            continue; // already on this computer (below)
        hf.route("GET", QStringLiteral("/org/tiny/resolve/%1/%2").arg(revision, QLatin1String(it.path)), [data](const MockRequest &) {
            MockResponse r;
            r.contentType = "application/octet-stream";
            r.body = data;
            return r;
        });
    }
    const QByteArray starters = QJsonDocument(QJsonObject{
        {QStringLiteral("files"), files},
        {QStringLiteral("sets"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("standard")}, {QStringLiteral("minVramGb"), 0}, {QStringLiteral("files"), ids}}}}}).toJson();
    // The upscaler is in a folder the library scans already.
    const QString existing = dir.filePath(QStringLiteral("comfy/upscale_models/TinyESRGAN_x4.safetensors"));
    QDir().mkpath(QFileInfo(existing).absolutePath());
    {
        QFile f(existing);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(contents.value(QStringLiteral("upscaler")));
    }
    QSignalSpy scanned(studio.models(), &runtime::ModelLibrary::scanFinished);
    QVERIFY(studio.models()->addRoot(dir.filePath(QStringLiteral("comfy"))));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !studio.models()->isScanning(), 20000);

    LocalSetupDialog dlg(&studio);
    dlg.setup()->setSets(runtime::starterSets(starters));
    dlg.resize(760, 620);
    dlg.show();
    QPushButton *start = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Set up"))
            start = b;
    }
    QVERIFY(start);
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 20000); // hardware probe
    auto *plan = dlg.findChild<QTreeWidget *>(QStringLiteral("setupPlan"));
    QVERIFY(plan);
    QCOMPARE(plan->topLevelItemCount(), 2 + 7); // two programs, seven files
    QCOMPARE(plan->topLevelItem(0)->text(2), QStringLiteral("Installed"));
    QCOMPARE(plan->topLevelItem(8)->text(2), QStringLiteral("Already on this computer"));
    qint64 expected = 0;
    for (auto it = contents.cbegin(); it != contents.cend(); ++it)
        expected += it.key() == QLatin1String("upscaler") ? 0 : it.value().size();
    QCOMPARE(dlg.setup()->downloadSize({QStringLiteral("standard"), true, true}), expected);
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        dlg.grab().save(shots + QStringLiteral("/local-setup.png"));

    QSignalSpy finished(dlg.setup(), &LocalSetup::finished);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() == 1, 30000);
    QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
    const QList<runtime::ServerProfile> profiles = servers->profiles();
    QCOMPARE(profiles.size(), 2);
    const auto llama = servers->profile(dlg.setup()->languageServer());
    const auto sd = servers->profile(dlg.setup()->imageServer());
    QVERIFY(llama && sd);
    QCOMPARE(QDir::fromNativeSeparators(llama->options.value(QStringLiteral("model")).toString()), own + QStringLiteral("/llm/Tiny-Q4_K_M.gguf"));
    QCOMPARE(QDir::fromNativeSeparators(llama->options.value(QStringLiteral("mmproj")).toString()), own + QStringLiteral("/llm/mmproj-Tiny-Q8_0.gguf"));
    QVERIFY(llama->options.contains(QStringLiteral("context"))); // GPU preset
    QCOMPARE(llama->name, QStringLiteral("Tiny lm"));
    QCOMPARE(QDir::fromNativeSeparators(sd->options.value(QStringLiteral("model")).toString()), own + QStringLiteral("/checkpoints/tiny-xl.safetensors"));
    QCOMPARE(QDir::fromNativeSeparators(sd->options.value(QStringLiteral("control_net")).toString()), own + QStringLiteral("/controlnet/tiny-controlnet.safetensors"));
    QCOMPARE(QDir::fromNativeSeparators(sd->options.value(QStringLiteral("clip_vision")).toString()), own + QStringLiteral("/clip_vision/model.safetensors"));
    QCOMPARE(QDir::fromNativeSeparators(sd->options.value(QStringLiteral("upscalers_dir")).toString()), QFileInfo(existing).absolutePath());
    QVERIFY(sd->options.contains(QStringLiteral("weight_type")));
    ai::Provider *sdProvider = studio.providerForServer(sd->id);
    QVERIFY(sdProvider && studio.providerForServer(llama->id));
    QCOMPARE(studio.providers()->defaultImageProvider(), sdProvider);
    QVERIFY(QFileInfo::exists(runtime::sidecarPath(own + QStringLiteral("/llm/Tiny-Q4_K_M.gguf"))));
    if (!shots.isEmpty()) {
        dlg.grab().save(shots + QStringLiteral("/local-setup-done.png"));
        // The download dialog against the real hubs (read-only public APIs): LUMEN_SCREENSHOT_HUB=1.
        if (qEnvironmentVariableIntValue("LUMEN_SCREENSHOT_HUB")) {
            studio.hub()->setBaseUrls(QStringLiteral("https://huggingface.co"), QStringLiteral("https://civitai.com"));
            ModelHubDialog hub(&studio);
            hub.show();
            hub.setSearchText(QStringLiteral("Qwen3-VL-8B-Instruct"));
            hub.search();
            QTest::qWait(6000);
            hub.grab().save(shots + QStringLiteral("/hub-huggingface.png"));
            hub.setSource(runtime::HubSource::Civitai);
            hub.setCategory(QStringLiteral("lora"));
            hub.setSearchText(QStringLiteral("watercolor"));
            hub.search();
            QTest::qWait(8000);
            hub.grab().save(shots + QStringLiteral("/hub-civitai.png"));
        }
    }

    // Running it again changes nothing and downloads nothing.
    {
        LocalSetup again(&studio);
        again.setSets(runtime::starterSets(starters));
        QFuture<void> prepared = again.prepare();
        QTRY_VERIFY_WITH_TIMEOUT(prepared.isFinished(), 20000);
        const LocalSetup::Options o{QStringLiteral("standard"), true, true};
        QCOMPARE(again.downloadSize(o), 0);
        QSignalSpy done(&again, &LocalSetup::finished);
        again.run(o);
        QTRY_VERIFY_WITH_TIMEOUT(done.size() == 1, 10000);
        QCOMPARE(done.first().at(1).toString(), QStringLiteral("Everything was already set up."));
        QCOMPARE(servers->profiles().size(), 2);
    }

    dlg.close();
}

namespace {

// A safetensors header without weights (enough for Lumen's header inspection).
QByteArray fakeSafetensors(const QStringList &tensors, const QJsonObject &meta = {})
{
    QJsonObject header;
    for (const QString &t : tensors)
        header.insert(t, QJsonObject{{QStringLiteral("dtype"), QStringLiteral("F16")}, {QStringLiteral("shape"), QJsonArray{8, 8}},
                                     {QStringLiteral("data_offsets"), QJsonArray{0, 0}}});
    if (!meta.isEmpty())
        header.insert(QStringLiteral("__metadata__"), meta);
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i)
        out.append(char((quint64(json.size()) >> (8 * i)) & 0xFF));
    return out + json + QByteArray(2 * 1024 * 1024, '\0'); // the library skips files under 1 MB
}

} // namespace

void TestCanvas::characterLoraChecks()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    auto write = [&dir](const QString &rel, const QByteArray &data) {
        const QString path = dir.filePath(rel);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() ? QDir::cleanPath(path) : QString();
    };
    const QString checkpoint = write(QStringLiteral("models/checkpoints/xl.safetensors"),
                                     fakeSafetensors({QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"),
                                                      QStringLiteral("model.diffusion_model.label_emb.0.0.weight"),
                                                      QStringLiteral("conditioner.embedders.1.model.ln_final.weight")}));
    const QString xlLora = write(QStringLiteral("models/loras/mira_xl.safetensors"),
                                 fakeSafetensors({QStringLiteral("lora_unet_a.lora_down.weight")},
                                                 {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")},
                                                  {QStringLiteral("modelspec.trigger_phrase"), QStringLiteral("lmn_mira")}}));
    const QString sd15Lora = write(QStringLiteral("models/old/mira_15.safetensors"),
                                   fakeSafetensors({QStringLiteral("lora_unet_a.lora_down.weight")}, {{QStringLiteral("ss_base_model_version"), QStringLiteral("sd_v1")}}));
    QVERIFY(!checkpoint.isEmpty() && !xlLora.isEmpty() && !sd15Lora.isEmpty());

    Studio studio;
    runtime::ServerSupervisor *servers = studio.servers();
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/lora-test");
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = dir.filePath(QStringLiteral("sd-server.exe"));
    rt.directory = dir.path();
    studio.runtimes()->addInstalled(rt);
    runtime::ServerProfile profile = servers->createProfile(rt.id, QStringLiteral("Images"));
    profile.options = {{QStringLiteral("model"), QDir::toNativeSeparators(checkpoint)}};
    servers->addProfile(profile);
    ai::Provider *sd = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Images"),
                                                      {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")}, {QStringLiteral("managed_server"), profile.id}});
    ai::Provider *cloud = studio.providers()->addProvider(QStringLiteral("openai_image"), QStringLiteral("Cloud"), {});
    QVERIFY(sd && cloud);
    auto cleanup = qScopeGuard([&] {
        studio.providers()->removeProvider(sd->instanceId());
        studio.providers()->removeProvider(cloud->instanceId());
        servers->removeProfile(profile.id);
        studio.runtimes()->remove(rt.id);
        studio.models()->removeRoot(QDir::cleanPath(dir.filePath(QStringLiteral("models"))));
    });
    QSignalSpy scanned(studio.models(), &runtime::ModelLibrary::scanFinished);
    QVERIFY(studio.models()->addRoot(dir.filePath(QStringLiteral("models"))));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !studio.models()->isScanning(), 20000);

    auto *image = qobject_cast<ai::ImageProvider *>(sd);
    QCOMPARE(checkpointFamily(&studio, image), QStringLiteral("SDXL"));
    QCOMPARE(modelFamily(&studio, xlLora), QStringLiteral("SDXL"));
    QCOMPARE(suggestedTrigger(&studio, xlLora), QStringLiteral("lmn_mira"));

    ai::CharacterLora lora;
    lora.file = xlLora;
    lora.trigger = QStringLiteral("lmn_mira");
    // The server has no LoRA folder yet: fixable in one click.
    LoraCheck check = checkCharacterLora(&studio, image, lora);
    QVERIFY(check.fixableFolder);
    QVERIFY(useLoraFolder(&studio, image, xlLora));
    QCOMPARE(QDir::fromNativeSeparators(servers->profile(profile.id)->options.value(QStringLiteral("lora_dir")).toString()), QFileInfo(xlLora).absolutePath());
    QVERIFY2(checkCharacterLora(&studio, image, lora).problem.isEmpty(), qPrintable(checkCharacterLora(&studio, image, lora).problem));
    // An SD 1.5 LoRA does not fit an SDXL checkpoint.
    ai::CharacterLora old = lora;
    old.file = sd15Lora;
    check = checkCharacterLora(&studio, image, old);
    QVERIFY(check.problem.contains(QStringLiteral("SD 1.5")));
    QVERIFY(!check.fixableFolder);
    // Cloud image APIs cannot load local LoRAs.
    QVERIFY(checkCharacterLora(&studio, qobject_cast<ai::ImageProvider *>(cloud), lora).problem.contains(QStringLiteral("cannot use LoRAs")));

    // The dialog lists library LoRAs and fills in the trigger word.
    CharacterLoraDialog dlg(&studio, image, {}, nullptr);
    auto *files = dlg.findChild<QComboBox *>();
    QVERIFY(files);
    QVERIFY(files->findData(QDir::toNativeSeparators(xlLora)) >= 0 && files->findData(QDir::toNativeSeparators(sd15Lora)) >= 0);
    files->setCurrentIndex(files->findData(QDir::toNativeSeparators(xlLora)));
    QCOMPARE(dlg.lora().trigger, QStringLiteral("lmn_mira"));
    QCOMPARE(dlg.lora().family, QStringLiteral("SDXL"));
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        dlg.show();
        QTest::qWait(200);
        dlg.grab().save(shots + QStringLiteral("/character-lora.png"));
    }
}

void TestCanvas::loraTrainingSet()
{
    // Three generated images: two different pictures, the third repeats the first.
    MockHttpServer server;
    int generated = 0;
    server.route("POST", QStringLiteral("/sdapi/v1/txt2img"), [&](const MockRequest &) {
        QImage im(256, 384, QImage::Format_ARGB32);
        im.fill(Qt::white);
        {
            QPainter p(&im);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(200, 40, 40));
            if (generated % 2 == 0)
                p.drawEllipse(QRect(20, 40, 120, 160));
            else
                p.drawRect(QRect(120, 180, 120, 180));
        }
        ++generated;
        return MockResponse::json({{QStringLiteral("images"), QJsonArray{QString::fromLatin1(img::encode(im).toBase64())}},
                                   {QStringLiteral("info"), QStringLiteral("{\"seed\": 1, \"all_seeds\": [1]}")}});
    });
    server.route("GET", QStringLiteral("/sdapi/v1/progress"), [](const MockRequest &) { return MockResponse::json({}); });
    server.route("GET", QStringLiteral("/controlnet/model_list"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("model_list"), QJsonArray{QStringLiteral("control_openpose [abc]")}}});
    });
    QList<int> scores{8, 4, 9};
    int critiques = 0;
    int taggings = 0;
    auto answer = [](const QJsonObject &content) {
        return MockResponse::json(
            {{QStringLiteral("choices"),
              QJsonArray{QJsonObject{{QStringLiteral("message"),
                                      QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Compact))}}},
                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    };
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        if (req.body.contains("image_tags")) {
            ++taggings;
            return answer({{QStringLiteral("tags"), QJsonArray{QStringLiteral("1girl"), QStringLiteral("solo"), QStringLiteral("red_hair"),
                                                               QStringLiteral("red eyes"), QStringLiteral("black robe"), QStringLiteral("smile"),
                                                               QStringLiteral("full body"), QStringLiteral("white background")}}});
        }
        const int score = scores.value(critiques++, 7);
        const QJsonObject verdict{{QStringLiteral("overall"), 9},
                                  {QStringLiteral("identity"), score},
                                  {QStringLiteral("pose"), 9},
                                  {QStringLiteral("issues"), QJsonArray{QStringLiteral("hair colour differs")}},
                                  {QStringLiteral("revised_prompt"), QStringLiteral("1girl")},
                                  {QStringLiteral("negative_additions"), QString()}};
        return MockResponse::json(
            {{QStringLiteral("choices"),
              QJsonArray{QJsonObject{{QStringLiteral("message"),
                                      QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(verdict).toJson(QJsonDocument::Compact))}}},
                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });
    Settings::instance().setValue(QStringLiteral("director/critic"), false);
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdwebui"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Vision"),
                                            {{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}}));
    studio.openProject(dir.filePath(QStringLiteral("dataset.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);

    // A library character with a reference.
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")},
                            {QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), QStringLiteral("red hair, red eyes, black robe")}};
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Mira (test)"), sheet, {ref});
    QTRY_VERIFY(saved.isFinished());
    const QString characterId = saved.result().id;
    auto cleanup = qScopeGuard([&] {
        studio.characters()->remove(characterId);
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });

    CreatePage page(&studio);
    QTRY_VERIFY(page.findChild<DirectorSession *>());
    DirectorSession *session = page.findChild<DirectorSession *>();
    session->setCharacter(sheet);
    session->setLibraryCharacter(characterId);
    for (QSpinBox *spin : page.findChildren<QSpinBox *>()) {
        if (spin->suffix().contains(QLatin1String("per shot")))
            spin->setValue(1);
    }
    LoraDatasetDialog dlg(&studio, characterId, session, [&] { session->setPlan(ai::Director::datasetPlan(sheet, 3)); });
    dlg.resize(1080, 720);
    dlg.show();
    QPushButton *planButton = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Plan training shots"))
            planButton = b;
    }
    QVERIFY(planButton);
    planButton->click();
    QCOMPARE(session->shots().size(), 3);
    QTRY_COMPARE(page.findChildren<ShotCard *>().size(), 3);
    for (const ShotState &s : session->shots())
        emit page.findChild<ShotCard *>()->generateRequested(s.id());
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(session->shots().begin(), session->shots().end(), [](const ShotState &s) { return s.results.size() == 1; }), 20000);

    // Import, then importing again adds nothing.
    int added = -1;
    dlg.addSessionResults([&](int n) { added = n; });
    QTRY_COMPARE_WITH_TIMEOUT(added, 3, 10000);
    added = -1;
    dlg.addSessionResults([&](int n) { added = n; });
    QTRY_COMPARE(added, 0);
    doc::LoraDataset *ds = dlg.dataset();
    QCOMPARE(ds->items().size(), 3);
    QVERIFY(!ds->items()[0].shot.value(QStringLiteral("prompt")).toString().isEmpty());

    // The Critic scores each image; the one below the minimum (6) is rejected.
    dlg.scoreAll();
    QTRY_VERIFY_WITH_TIMEOUT(!dlg.isScoring() && critiques == 3, 20000);
    int rejected = 0;
    for (const doc::DatasetItem &i : ds->items()) {
        QVERIFY(i.score >= 0);
        if (!i.kept) {
            QCOMPARE(i.score, 4);
            ++rejected;
        }
    }
    QCOMPARE(rejected, 1);
    // The repeated picture is a near-duplicate of the better-scored one.
    QCOMPARE(ds->markDuplicates(), 1);
    for (const doc::DatasetItem &i : ds->items()) {
        if (i.duplicateOf.isEmpty())
            continue;
        const auto better = ds->item(i.duplicateOf);
        QVERIFY(better && better->score >= i.score);
        QVERIFY(!i.kept);
        QVERIFY(doc::hashDistance(i.hash, better->hash) <= 6);
    }
    auto *grid = dlg.findChild<QListWidget *>();
    QVERIFY(grid);
    QCOMPARE(grid->count(), 3);
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        grid->setCurrentRow(0);
        QTest::qWait(500);
        dlg.grab().save(shots + QStringLiteral("/lora-training-set.png"));
    }
    // Captions: the vision model tags the kept images; the character's own look is pruned.
    QCOMPARE(ds->settings().trigger, QStringLiteral("lmn_mira"));
    const int kept = ds->keptCount();
    dlg.captionAll();
    QTRY_VERIFY_WITH_TIMEOUT(!dlg.isCaptioning(), 20000);
    QCOMPARE(taggings, kept);
    auto *table = dlg.findChild<QTableWidget *>();
    QVERIFY(table);
    QCOMPARE(table->rowCount(), kept);
    QString firstKept;
    for (const doc::DatasetItem &i : ds->items()) {
        if (!i.kept) {
            QVERIFY(i.tags.isEmpty());
            continue;
        }
        if (firstKept.isEmpty())
            firstKept = i.id;
        QCOMPARE(i.tags.size(), 8);
        QCOMPARE(i.caption, QStringLiteral("lmn_mira, 1girl, solo, smile, full body, white background"));
        QVERIFY(!i.captionEdited);
    }
    // A new trigger word and a changeable outfit rebuild the captions.
    QLineEdit *trigger = nullptr;
    QCheckBox *outfit = nullptr;
    for (QLineEdit *e : dlg.findChildren<QLineEdit *>()) {
        if (e->text() == QLatin1String("lmn_mira"))
            trigger = e;
    }
    for (QCheckBox *c : dlg.findChildren<QCheckBox *>()) {
        if (c->text().startsWith(QLatin1String("Outfit")))
            outfit = c;
    }
    QVERIFY(trigger && outfit && outfit->isChecked());
    trigger->setText(QStringLiteral("Mira V2"));
    emit trigger->editingFinished();
    QCOMPARE(ds->settings().trigger, QStringLiteral("mira_v2"));
    QCOMPARE(ds->item(firstKept)->caption, QStringLiteral("mira_v2, 1girl, solo, smile, full body, white background"));
    // A caption edited by hand keeps its words when the outfit setting changes.
    QTableWidgetItem *cell = table->item(0, 1);
    const QString edited = cell->data(Qt::UserRole).toString();
    cell->setText(QStringLiteral("mira_v2, 1girl, solo, grin, from side"));
    QTRY_VERIFY(ds->item(edited)->captionEdited);
    outfit->setChecked(false);
    QCOMPARE(ds->item(edited)->caption, QStringLiteral("mira_v2, 1girl, solo, grin, from side"));
    for (const doc::DatasetItem &i : ds->items()) {
        if (i.kept && i.id != edited)
            QCOMPARE(i.caption, QStringLiteral("mira_v2, 1girl, solo, black robe, smile, full body, white background"));
    }
    if (!shots.isEmpty()) {
        dlg.findChild<QTabWidget *>()->setCurrentIndex(1);
        QTest::qWait(500);
        dlg.grab().save(shots + QStringLiteral("/lora-captions.png"));
    }

    // The set lives with the library character.
    doc::LoraDataset reloaded(studio.characters()->datasetDirectory(characterId));
    reloaded.load();
    QCOMPARE(reloaded.items().size(), 3);
    QCOMPARE(reloaded.settings().trigger, QStringLiteral("mira_v2"));
}

void TestCanvas::loraTrainingRun()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    auto write = [&dir](const QString &rel, const QByteArray &data) {
        const QString path = QDir::cleanPath(dir.filePath(rel));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() ? path : QString();
    };
    const QString checkpoint = write(QStringLiteral("models/checkpoints/xl.safetensors"),
                                     fakeSafetensors({QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"),
                                                      QStringLiteral("model.diffusion_model.label_emb.0.0.weight"),
                                                      QStringLiteral("conditioner.embedders.1.model.ln_final.weight")}));
    QVERIFY(!write(QStringLiteral("kohya/sd-scripts/sdxl_train_network.py"), "#").isEmpty());
    QVERIFY(!write(QStringLiteral("kohya/venv/Scripts/python.exe"), "MZ").isEmpty());

    Studio studio;
    QFuture<runtime::InstalledRuntime> adopted = studio.runtimes()->adopt(dir.filePath(QStringLiteral("kohya")));
    QTRY_VERIFY(adopted.isFinished());
    const QString trainerId = adopted.result().id;
    QSignalSpy scanned(studio.models(), &runtime::ModelLibrary::scanFinished);
    QVERIFY(studio.models()->addRoot(dir.filePath(QStringLiteral("models"))));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !studio.models()->isScanning(), 20000);

    // A library character with a small training set.
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")}, {QStringLiteral("tags"), QStringLiteral("red hair")}};
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Mira (training test)"), sheet, {ref});
    QTRY_VERIFY(saved.isFinished());
    const QString characterId = saved.result().id;
    QString trainedLora;
    auto cleanup = qScopeGuard([&] {
        studio.characters()->remove(characterId);
        studio.runtimes()->remove(trainerId);
        studio.models()->moveToTrash = [](const QString &path, QString *) { return QFile::remove(path); };
        if (!trainedLora.isEmpty())
            studio.models()->trash(trainedLora);
        studio.models()->removeRoot(QDir::cleanPath(dir.filePath(QStringLiteral("models"))));
    });
    {
        doc::LoraDataset ds(studio.characters()->datasetDirectory(characterId));
        ds.load();
        for (int i = 0; i < 3; ++i) {
            QImage img(512, 768, QImage::Format_ARGB32);
            img.fill(QColor(200, 40 * i, 40));
            auto added = ds.add(img, {{QStringLiteral("file"), QStringLiteral("x")}});
            QTRY_VERIFY(added.isFinished());
            ds.setCaption(added.result().id, QStringLiteral("lmn_mira, 1girl, smile"), false);
        }
        ds.setSettings({QStringLiteral("lmn_mira"), true});
    }

    // The trainer is simulated: it checks the folder it gets and writes a LoRA per epoch.
    QStringList trainerArgs;
    QString imagesFolder;
    studio.runtimes()->processRunner = [&](const QString &, const QStringList &args, const QProcessEnvironment &, const QString &,
                                           std::function<void(const QString &)> line, const async::CancelToken &) {
        trainerArgs = args;
        const QString out = args.value(args.indexOf(QStringLiteral("--output_dir")) + 1);
        const QString name = args.value(args.indexOf(QStringLiteral("--output_name")) + 1);
        const QString images = args.value(args.indexOf(QStringLiteral("--train_data_dir")) + 1);
        const QStringList folders = QDir(images).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        imagesFolder = folders.value(0);
        const QByteArray lora = fakeSafetensors({QStringLiteral("lora_unet_a.lora_down.weight")},
                                                {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")}});
        for (const QString &f : {name + QStringLiteral("-000001.safetensors"), name + QStringLiteral(".safetensors")}) {
            QFile file(QDir(out).filePath(f));
            if (file.open(QIODevice::WriteOnly))
                file.write(lora);
        }
        line(QStringLiteral("epoch 1/1"));
        line(QStringLiteral("steps: 100%|##########| 75/75 [00:03<00:00,  3.10s/it, avr_loss=0.0850]"));
        line(QStringLiteral("saving checkpoint: %1").arg(QDir(out).filePath(name + QStringLiteral(".safetensors"))));
        runtime::ProcessResult r;
        r.started = true;
        r.exitCode = 0;
        return QtFuture::makeReadyValueFuture(r);
    };

    LoraDatasetDialog dlg(&studio, characterId, nullptr, {});
    dlg.resize(1080, 720);
    dlg.show();
    dlg.findChild<QTabWidget *>()->setCurrentIndex(2);
    QComboBox *base = nullptr;
    for (QComboBox *c : dlg.findChildren<QComboBox *>()) {
        if (c->findData(checkpoint) >= 0)
            base = c;
    }
    QVERIFY(base);
    base->setCurrentIndex(base->findData(checkpoint));
    QPushButton *start = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Start training"))
            start = b;
    }
    QVERIFY(start);
    QTRY_VERIFY(start->isEnabled());
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!studio.characters()->entry(characterId)->lora.isEmpty(), 20000);
    QVERIFY(!CharacterTraining::running(characterId));

    // 3 kept images × 40 repeats (the most) in "<repeats>_<trigger>", the chosen base model, one epoch file each.
    QCOMPARE(imagesFolder, QStringLiteral("40_lmn_mira"));
    QCOMPARE(QDir::fromNativeSeparators(trainerArgs.value(trainerArgs.indexOf(QStringLiteral("--pretrained_model_name_or_path")) + 1)), checkpoint);
    const ai::CharacterLora lora = ai::CharacterLora::fromJson(studio.characters()->entry(characterId)->lora);
    trainedLora = lora.file;
    QCOMPARE(lora.trigger, QStringLiteral("lmn_mira"));
    QCOMPARE(lora.family, QStringLiteral("SDXL"));
    QCOMPARE(QFileInfo(lora.file).fileName().left(8), QStringLiteral("lmn_mira"));
    QCOMPARE(QFileInfo(lora.file).absolutePath(), QDir(studio.models()->defaultFolder()).filePath(QStringLiteral("loras")));
    QTRY_VERIFY(studio.models()->entry(lora.file).has_value());
    const auto entry = studio.models()->entry(lora.file);
    QCOMPARE(entry->info.kind, runtime::ModelKind::Lora);
    QCOMPARE(entry->source.value(QStringLiteral("source")).toString(), QStringLiteral("training"));
    QCOMPARE(entry->info.triggerWords, QStringList{QStringLiteral("lmn_mira")});
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        QTest::qWait(300);
        dlg.grab().save(shots + QStringLiteral("/lora-train.png"));
    }
}

QTEST_MAIN(TestCanvas)
#include "tst_canvas.moc"
