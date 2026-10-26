#include "core/ImageOps.h"
#include "core/Paths.h"
#include "doc/Document.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/TransformTool.h"
#include "core/Settings.h"
#include "ui/Accessibility.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Shortcuts.h"
#include "ui/Studio.h"
#include "plugins/BuiltInPlugins.h"
#include "plugins/characters/assistant/AssistantPanel.h"
#include "plugins/characters/assistant/DirectorActions.h"
#include "plugins/characters/director/DirectorSession.h"
#include "plugins/characters/director/ShotCard.h"
#include "plugins/characters/CreatePage.h"
#include "ui/dialogs/NewFromPromptDialog.h"
#include "ui/dialogs/NewCanvasDialog.h"
#include "ui/filters/FilterRunner.h"
#include "ui/filters/FiltersPanel.h"
#include "ui/filters/NodeEditorWindow.h"
#include "ui/filters/ParamWidgets.h"
#include "ui/pages/AiOptions.h"
#include "ui/pages/AiPanel.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/LibraryPage.h"
#include "ui/pages/PromptBar.h"
#include "ui/pages/PromptPlan.h"
#include "ui/pages/StartScreen.h"
#include "ui/pages/VariantStrip.h"
#include "ui/widgets/JobCard.h"
#include "ui/widgets/LoraPicker.h"
#include "ui/dialogs/SelectByDescriptionDialog.h"
#include "ui/SmartSelect.h"
#include "ui/canvas/SmartSelectTool.h"
#include "ai/Segmenter.h"
#include "runtime/Components.h"
#include "filters/Evaluator.h"
#include "filters/Registry.h"
#include "core/Finders.h"
#include "ui/widgets/RunSettingsEditor.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"
#include "ai/ModelDefaults.h"
#include "ai/providers/SdCppProvider.h"
#include "pose/PosePresets.h"
#include "plugins/parts/PartsPage.h"
#include "plugins/parts/ExpressionCheck.h"
#include "doc/Parts.h"
#include "pose/Rig.h"
#include "MockHttpServer.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "plugins/characters/training/CloudTrainingPage.h"
#include "runtime/Download.h"
#include "runtime/ModelPresets.h"
#include "ui/dialogs/LocalServersPage.h"
#include "plugins/characters/training/LoraDatasetDialog.h"
#include "plugins/characters/training/CharacterTraining.h"
#include "plugins/characters/training/LoraEvaluation.h"
#include "plugins/characters/assistant/AssistantTools.h"
#include "ui/widgets/CharacterChip.h"
#include "ai/Agent.h"
#include "runtime/Supervisor.h"
#include "runtime/RuntimeStore.h"
#include "core/Async.h"
#include "ai/Types.h"
#include "doc/CharacterLibrary.h"
#include "doc/LoraDataset.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/dialogs/ImageSizeDialog.h"
#include "ui/dialogs/ModelHubDialog.h"
#include "ui/dialogs/UserGuideDialog.h"
#include "runtime/ModelLibrary.h"
#include "ui/dialogs/ModelsPage.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/dialogs/SetupWizard.h"
#include "ui/dialogs/ShortcutsPage.h"
#include "ui/widgets/ServerStatus.h"
#include "ai/Gate.h"
#include "ai/PromptStyle.h"
#include "ui/widgets/Controls.h"
#include "ui/ext/Extension.h"

#include <QClipboard>
#include <QtConcurrent/QtConcurrentRun>
#include <QRadioButton>
#include <QCryptographicHash>
#include <QDragEnterEvent>
#include <QMenuBar>
#include <QMimeData>
#include <QStatusBar>
#include <QTreeWidgetItemIterator>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QDirIterator>
#include <QInputDialog>
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
#include <QMenu>
#include <QMessageBox>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextBrowser>
#include <QScopeGuard>
#include <QSlider>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QTimer>
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

// Plugins are off by default: switches them on for one test (the main window reads which are on
// when it is built) and back to the default afterwards.
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
    void loraEvaluation();
    void activeCharacter();
    void newFromPrompt();
    void editorCharacter();
    void partsPageCharacter();
    void expressionCheck();
    void assistantCharacter();
    void safeContent();
    void cloudTrainingPage();
    void cloudTrainingTab();
    void hubCompanionFiles();
    void activeCast();
    void startScreen();
    void shellStatusBar();
    void pluginWindows();
    void pluginsOff();
    void legacyPageSetting();
    void presetServerDialog();
    void serverDialogModelTypes();
    void modelsFolderSettings();
    void promptModes();
    void promptBarSelectionEdit();
    void promptBarNewImage();
    void loraPickerElsewhere();
    void loraPickerFolder();
    void runSettingsPerModel();
    void selectByDescription();
    void smartSelection();
    void selectObjectsByName();
    void promptBarWithoutEdits();
    void aiMenuAndPanel();
    void promptBarKeepsTyping();
    void promptBarRuns();
    void promptBarEngine();
    void wholeImageKeepsDetail();
    void editorMenus();
    void imageSizeDialogs();
    void selectionOptions();
    void newFromPromptEngines();
    void recentAndDetails();
    void engineRouting();
    void filtersInTheShell();

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
    plugins::registerBuiltInPlugins();
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
    PluginsOn plugins;
    Studio studio;
    MainWindow window(&studio);
    // Plugin pages bind their shortcuts when their window is first opened.
    for (const QString &page : window.pageIds())
        window.showPage(page);
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
    // The keys of the old rail pages open what replaced them.
    QCOMPARE(sc.keys(QStringLiteral("nav.sparkles")), QList<QKeySequence>{QKeySequence(QStringLiteral("Alt+2"))});
    QCOMPARE(sc.keys(QStringLiteral("nav.gallery")), QList<QKeySequence>{QKeySequence(QStringLiteral("Alt+6"))});
    QCOMPARE(sc.keys(QStringLiteral("app.assistant")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+A"))});
    // The Generate page's Ctrl+Enter went with it; in the editor it runs the prompt bar.
    QCOMPARE(sc.keys(QStringLiteral("editor.ai-run")), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Return"))});
    QVERIFY(!sc.contains(QStringLiteral("app.context-bar"))); // the floating toolbar went with the prompt bar
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
    PluginsOn plugins;
    Studio studio;
    MainWindow window(&studio);
    window.resize(1400, 900);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QStringList missing;
    QSet<QWidget *> reported;
    auto check = [&missing, &reported](QWidget *root) {
        QTest::qWait(30); // widgets get polished (and named) once shown
        for (QWidget *w : Accessibility::unnamed(root)) {
            if (!w->isVisibleTo(root) || reported.contains(w))
                continue;
            reported.insert(w);
            QStringList chain;
            for (QWidget *p = w->parentWidget(); p && p != root; p = p->parentWidget())
                chain << QLatin1String(p->metaObject()->className()) + (p->objectName().isEmpty() ? QString() : u':' + p->objectName());
            missing << QStringLiteral("%1 '%2' in %3").arg(QLatin1String(w->metaObject()->className()), w->objectName(),
                                                         chain.join(QStringLiteral(" < ")));
        }
    };
    // The start screen, the editor with a document, every plugin page's window, the dialogs.
    QVERIFY(window.editor()->showsStartScreen());
    check(&window);
    doc::Document *named = studio.openImageAsDocument(content(), QStringLiteral("Named"));
    check(&window);
    // The prompt bar's ⋯ menu, results to choose from, and the AI panel.
    window.editor()->promptBar()->showOptions();
    check(window.editor()->promptBar()->optionsPopup());
    window.editor()->promptBar()->optionsPopup()->hide();
    window.editor()->variantStrip()->addVariants(named, {content(), content()}, QStringLiteral("Two results"));
    window.editor()->showAiPanel();
    check(&window);
    window.editor()->variantStrip()->discard();
    // The Filters tab with a filter's settings, and the filter editor (a window of its own).
    window.editor()->hideAiPanel();
    window.editor()->showFiltersPanel();
    QVERIFY(window.editor()->filtersPanel()->selectFilter(QStringLiteral(":/filters/builtin/old-photo.lumenfilter")));
    check(&window);
    NodeEditorWindow *filterEditor = window.editor()->filtersPanel()->editSelected();
    QVERIFY(filterEditor);
    filterEditor->selectNode(QStringLiteral("n3"));
    check(filterEditor);
    filterEditor->close();
    for (const QString &page : window.pageIds().mid(1)) {
        window.showPage(page);
        check(window.pageWindow(page));
    }
    check(window.browseProjectImages());
    check(window.newFromPrompt());
    window.newFromPromptDialog()->reject();
    // The setup wizard's pages (no probing of this computer, its ports or GitHub) and Settings → AI.
    studio.providers()->serverProbes.clear();
    studio.runtimes()->feed()->setApiBase(QUrl(QStringLiteral("http://127.0.0.1:9")));
    SetupWizard::testHardware = runtime::HardwareInfo();
    SetupWizard::testSearchFolders = QStringList();
    auto restore = qScopeGuard([] {
        SetupWizard::testHardware.reset();
        SetupWizard::testSearchFolders.reset();
    });
    SetupWizard *wizard = window.setUpAi();
    QVERIFY(wizard);
    for (const SetupWizard::Page page : {SetupWizard::Page::Choose, SetupWizard::Page::Local, SetupWizard::Page::Online, SetupWizard::Page::Server}) {
        wizard->showPage(page);
        if (page == SetupWizard::Page::Local)
            QTRY_VERIFY_WITH_TIMEOUT(wizard->recommendation().has_value(), 20000);
        check(wizard);
    }
    wizard->close();
    {
        SettingsDialog settings(&studio, &window);
        settings.showTab(QStringLiteral("ai"));
        settings.show();
        check(&settings);
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
    QTRY_VERIFY(directorActions(&studio));
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
    const QJsonObject state = directorActions(&studio)->directorState();
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
    // Runtimes another test left behind when it was aborted (the store persists between runs).
    for (const runtime::InstalledRuntime &old : studio.runtimes()->runtimes()) {
        if (old.id.startsWith(QLatin1String("adopted/")))
            studio.runtimes()->remove(old.id);
    }
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
            auto *list = dlg.findChild<QListWidget *>(QStringLiteral("providerList"));
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
    // The provider knows the server's checkpoint, so the prompt style can match it.
    QCOMPARE(studio.providers()->imageProvider(sd->instanceId())->text(QStringLiteral("model")), QStringLiteral("xl.safetensors"));
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
    // The Train tab follows the set (the images came after the dialog opened).
    bool summaryFollows = false;
    for (QLabel *l : dlg.findChildren<QLabel *>())
        summaryFollows = summaryFollows || l->text().startsWith(QStringLiteral("%1 images × ").arg(ds->keptCount()));
    QVERIFY(summaryFollows);
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
    auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("captions"));
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
        for (const QString &f : {name + QStringLiteral("-000001.safetensors"), name + QStringLiteral(".safetensors"),
                                 name + QStringLiteral("-000001-state/optimizer.bin")}) {
            QDir().mkpath(QFileInfo(QDir(out).filePath(f)).absolutePath());
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

    // An earlier training of this character.
    const QString datasetDir = studio.characters()->datasetDirectory(characterId);
    {
        const QString old = QDir(datasetDir).filePath(QStringLiteral("training/run-20200101-000000/output/lmn_mira.safetensors"));
        QDir().mkpath(QFileInfo(old).absolutePath());
        QFile f(old);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(2048, 'x'));
    }

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
    // The finished run keeps its epochs but not the trainer's state or the exported images.
    const QString run = CharacterTraining::lastRun(datasetDir);
    QCOMPARE(CharacterTraining::runs(datasetDir).size(), 2);
    QVERIFY(QFileInfo::exists(run + QStringLiteral("/output/lmn_mira-000001.safetensors")));
    QVERIFY(!QFileInfo::exists(run + QStringLiteral("/output/lmn_mira-000001-state")));
    QVERIFY(!QFileInfo::exists(run + QStringLiteral("/img")));
    QVERIFY(QFileInfo::exists(run + QStringLiteral("/run.json")));
    // The earlier run can be removed (after a confirmation); the newest stays.
    QPushButton *removeRuns = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Remove earlier trainings…"))
            removeRuns = b;
    }
    QVERIFY(removeRuns);
    QTRY_VERIFY(removeRuns->isVisible());
    QTimer::singleShot(200, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    removeRuns->click();
    QCOMPARE(CharacterTraining::runs(datasetDir), QStringList{run});
    QVERIFY(!removeRuns->isVisible());
    QVERIFY(QFileInfo::exists(lora.file));
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

void TestCanvas::loraEvaluation()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    auto write = [&dir](const QString &rel, const QByteArray &data) {
        const QString path = QDir::cleanPath(dir.filePath(rel));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() ? path : QString();
    };
    const QString checkpoint = write(QStringLiteral("models/xl.safetensors"),
                                     fakeSafetensors({QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"),
                                                      QStringLiteral("model.diffusion_model.label_emb.0.0.weight"),
                                                      QStringLiteral("conditioner.embedders.1.model.ln_final.weight")}));
    const QString serverLoras = QDir::cleanPath(dir.filePath(QStringLiteral("server-loras")));
    QDir().mkpath(serverLoras);

    // A stable-diffusion.cpp server (simulated: sd.cpp and the Critic answer from the mock) that
    // lists the LoRA files in its folder, as the real one rescans it per request.
    MockHttpServer server;
    QList<QJsonObject> generations;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [&](const MockRequest &) {
        QJsonArray loras;
        QDirIterator it(serverLoras, {QStringLiteral("*.safetensors")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString rel = QDir(serverLoras).relativeFilePath(it.next());
            loras.append(QJsonObject{{QStringLiteral("name"), QFileInfo(rel).completeBaseName()}, {QStringLiteral("path"), rel}});
        }
        return MockResponse::json({{QStringLiteral("loras"), loras}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        generations << req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    // The sixth image fails once (the server lost the job); the test generates it again.
    int failedGeneration = -1;
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [&](const MockRequest &) {
        if (failedGeneration < 0 && generations.size() == 6) {
            failedGeneration = 5;
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("failed")},
                                       {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("job lost")}}}});
        }
        QImage im(64, 96, QImage::Format_ARGB32);
        im.fill(QColor::fromHsv(int(generations.size() * 15) % 360, 200, 200));
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(im).toBase64())}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });
    // Identity checklists (face, red hair, red eyes, black robe; m = match, c = close, d = different)
    // in the order the Critic sees the images: the baseline, epochs 2, 3, 4 at weight 0.8, then the
    // best epoch (3; epoch 4 only ties) at 0.6 and 1.0.
    QStringList checklists;
    for (const char *rows : {"mmdd", "mmmd", "mmmm", "mmmm", "mmmc", "mmmm"}) {
        for (int i = 0; i < 4; ++i)
            checklists << QLatin1String(rows);
    }
    int critiques = 0;
    int criticCalls = 0;
    QJsonObject checklistRequest;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        // The tenth check fails once (the language server was reloading); the test asks again.
        if (++criticCalls == 10)
            return MockResponse::json({{QStringLiteral("error"), QStringLiteral("proxy error")}}, 500);
        checklistRequest = req.json();
        const QString marks = checklists.value(critiques++, QStringLiteral("dddd"));
        QJsonObject features;
        for (int i = 0; i < marks.size(); ++i) {
            features.insert(QStringLiteral("f%1").arg(i + 1), marks[i] == QLatin1Char('m')   ? QStringLiteral("match")
                                                              : marks[i] == QLatin1Char('c') ? QStringLiteral("close")
                                                                                             : QStringLiteral("different"));
        }
        const QJsonObject verdict{{QStringLiteral("features"), features},
                                  {QStringLiteral("pose"), 9},
                                  {QStringLiteral("issues"), QJsonArray{QStringLiteral("hair a little darker")}}};
        return MockResponse::json(
            {{QStringLiteral("choices"),
              QJsonArray{QJsonObject{{QStringLiteral("message"),
                                      QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(verdict).toJson(QJsonDocument::Compact))}}},
                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });
    const QString exe = dir.filePath(QStringLiteral("sd/sd-server.exe"));
    QDir().mkpath(QFileInfo(exe).absolutePath());
    QVERIFY(QFile::copy(QStringLiteral(FAKE_SERVER_PATH), exe));

    Studio studio;
    runtime::ServerSupervisor *servers = studio.servers();
    servers->healthIntervalMs = 100;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/eval-test");
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = exe;
    rt.directory = QFileInfo(exe).absolutePath();
    studio.runtimes()->addInstalled(rt);
    runtime::ServerProfile profile = servers->createProfile(rt.id, QStringLiteral("Images"));
    profile.options = {{QStringLiteral("model"), QDir::toNativeSeparators(checkpoint)},
                       {QStringLiteral("lora_dir"), QDir::toNativeSeparators(serverLoras)},
                       {QStringLiteral("extra_args"), QStringLiteral("--fake-ready-ms 0 --fake-proxy %1").arg(server.baseUrl().port())}};
    servers->addProfile(profile);
    ai::Provider *sd = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Images"),
                                                      {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")}, {QStringLiteral("managed_server"), profile.id}});
    QVERIFY(sd);
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Vision"),
                                            {{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}}));

    // A trained character: four epochs in its last run.
    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")},
                            {QStringLiteral("gender_presentation"), QStringLiteral("female")},
                            {QStringLiteral("tags"), QStringLiteral("red hair, red eyes, black robe")}};
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Mira (evaluation test)"), sheet, {ref});
    QTRY_VERIFY(saved.isFinished());
    const QString characterId = saved.result().id;
    QString appliedLora;
    auto cleanup = qScopeGuard([&] {
        servers->stop(profile.id);
        studio.characters()->remove(characterId);
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        servers->removeProfile(profile.id);
        studio.runtimes()->remove(rt.id);
        studio.models()->moveToTrash = [](const QString &path, QString *) { return QFile::remove(path); };
        if (!appliedLora.isEmpty())
            studio.models()->trash(appliedLora);
    });
    const QString runDir = QDir(studio.characters()->datasetDirectory(characterId)).filePath(QStringLiteral("training/run-20261010-120000"));
    QDir().mkpath(QDir(runDir).filePath(QStringLiteral("output")));
    {
        QFile f(QDir(runDir).filePath(QStringLiteral("run.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("checkpoint"), checkpoint},
                                          {QStringLiteral("family"), QStringLiteral("SDXL")},
                                          {QStringLiteral("epochs"), 4},
                                          {QStringLiteral("trigger"), QStringLiteral("lmn_mira")},
                                          {QStringLiteral("outputName"), QStringLiteral("lmn_mira")}})
                    .toJson());
    }
    const QByteArray loraBytes = fakeSafetensors({QStringLiteral("lora_unet_a.lora_down.weight")}, {{QStringLiteral("ss_base_model_version"), QStringLiteral("sdxl_base_v1-0")}});
    for (const QString &name : {QStringLiteral("lmn_mira-000001"), QStringLiteral("lmn_mira-000002"), QStringLiteral("lmn_mira-000003"), QStringLiteral("lmn_mira")}) {
        QFile f(QDir(runDir).filePath(QStringLiteral("output/%1.safetensors").arg(name)));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(loraBytes);
    }
    const QList<QPair<int, QString>> epochs = LoraEvaluation::epochFiles(runDir);
    QCOMPARE(epochs.size(), 4);
    QCOMPARE(epochs.last().first, 4);
    QCOMPARE(QFileInfo(epochs.last().second).fileName(), QStringLiteral("lmn_mira.safetensors"));

    // The test shots: new scenes with the character's description; LoRA candidates add the trigger word.
    const QList<QJsonObject> withTrigger = ai::Director::loraTestShots(sheet, QStringLiteral("lmn_mira"));
    const QList<QJsonObject> baseline = ai::Director::loraTestShots(sheet, QString());
    QCOMPARE(withTrigger.size(), 4);
    QVERIFY(withTrigger[0].value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("lmn_mira, 1girl, solo, red hair")));
    QCOMPARE(QStringLiteral("lmn_mira, ") + baseline[0].value(QStringLiteral("prompt")).toString(), withTrigger[0].value(QStringLiteral("prompt")).toString());
    QCOMPARE(ai::Director::loraTestScore({{QStringLiteral("identity"), 9}, {QStringLiteral("pose"), 6}}), 8.0);

    LoraDatasetDialog dlg(&studio, characterId, nullptr, {});
    dlg.resize(1080, 820);
    dlg.show();
    dlg.findChild<QTabWidget *>()->setCurrentIndex(2);
    QPushButton *test = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Test the LoRA"))
            test = b;
    }
    QVERIFY(test);
    QTRY_VERIFY(test->isEnabled());
    test->click();
    QVERIFY(LoraEvaluation::running(characterId));
    // The candidates are copied into the server's LoRA folder for the test.
    QTRY_VERIFY(QFileInfo::exists(serverLoras + QStringLiteral("/lumen-eval/lmn_mira-e04.safetensors")));
    QVERIFY(!QFileInfo::exists(serverLoras + QStringLiteral("/lumen-eval/lmn_mira-e01.safetensors")));
    QTRY_VERIFY_WITH_TIMEOUT(!LoraEvaluation::running(characterId), 120000);

    // 4 shots × (baseline + 3 epochs) + 4 shots × 2 more weights of the best epoch, plus the one
    // generation and the one check that were repeated.
    QCOMPARE(generations.size(), 25);
    QCOMPARE(failedGeneration, 5);
    QCOMPARE(generations[5], generations[6]);
    generations.removeAt(5);
    QCOMPARE(critiques, 24);
    QCOMPARE(criticCalls, 25);
    for (int i = 0; i < 4; ++i) {
        QVERIFY(generations[i].value(QStringLiteral("lora")).toArray().isEmpty());
        QVERIFY(generations[i].value(QStringLiteral("prompt")).toString().contains(QStringLiteral("red hair")));
    }
    for (int i = 4; i < 24; ++i) {
        const QJsonArray lora = generations[i].value(QStringLiteral("lora")).toArray();
        QCOMPARE(lora.size(), 1);
        QVERIFY(lora[0].toObject().value(QStringLiteral("path")).toString().startsWith(QStringLiteral("lumen-eval/lmn_mira-e0")));
        // The trigger follows the model's quality prefix (the checkpoint "xl" is a generic SDXL model).
        QVERIFY(generations[i].value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("high quality, detailed, lmn_mira, ")));
        // The same seed per shot for every candidate.
        QCOMPARE(generations[i].value(QStringLiteral("seed")), generations[i % 4].value(QStringLiteral("seed")));
    }
    QCOMPARE(generations[8].value(QStringLiteral("lora")).toArray()[0].toObject().value(QStringLiteral("path")).toString(),
             QStringLiteral("lumen-eval/lmn_mira-e03.safetensors"));
    QCOMPARE(generations[16].value(QStringLiteral("lora")).toArray()[0].toObject().value(QStringLiteral("multiplier")).toDouble(), 0.6);
    QCOMPARE(generations[20].value(QStringLiteral("lora")).toArray()[0].toObject().value(QStringLiteral("multiplier")).toDouble(), 1.0);
    QVERIFY(!QFileInfo::exists(serverLoras + QStringLiteral("/lumen-eval")));

    // Epoch 3 at 0.8 wins (epoch 4 and weight 1.0 only tie with it).
    const QJsonObject result = LoraEvaluation::saved(runDir);
    QVERIFY(result.value(QStringLiteral("finished")).toBool());
    QCOMPARE(result.value(QStringLiteral("bestEpoch")).toInt(), 3);
    QCOMPARE(result.value(QStringLiteral("bestWeight")).toDouble(), 0.8);
    QCOMPARE(result.value(QStringLiteral("cells")).toArray().size(), 24);
    for (const QJsonValue &c : result.value(QStringLiteral("cells")).toArray())
        QVERIFY(QFileInfo::exists(c[QStringLiteral("image")].toString()));
    auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("loraTest"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 6);
    QCOMPARE(table->verticalHeaderItem(0)->text(), QStringLiteral("Without LoRA"));
    // Baseline: 2 of 4 features → identity 5, score (2·5 + 9) / 3; epoch 3: all → (2·10 + 9) / 3.
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("6.3"));
    QCOMPARE(table->item(2, 4)->text(), QStringLiteral("9.7"));
    QVERIFY(table->item(0, 0)->toolTip().contains(QStringLiteral("✗ black robe")));
    QVERIFY(table->item(0, 0)->toolTip().contains(QStringLiteral("✓ red hair")));
    // The Critic got the checklist, numbered like the schema.
    QVERIFY(QJsonDocument(checklistRequest).toJson().contains("identity_checklist"));
    QVERIFY(QJsonDocument(checklistRequest).toJson().contains("f4: black robe"));

    QPushButton *apply = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>()) {
        if (b->text().startsWith(QLatin1String("Use epoch")))
            apply = b;
    }
    QVERIFY(apply && apply->isVisible());
    QCOMPARE(apply->text(), QStringLiteral("Use epoch 3 at weight 0.8 (recommended)"));
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        QTest::qWait(300);
        dlg.grab().save(shots + QStringLiteral("/lora-test.png"));
    }
    // Another row can be chosen instead.
    table->selectRow(4);
    QCOMPARE(apply->text(), QStringLiteral("Use epoch 3 at weight 0.6"));
    table->selectRow(1);
    QCOMPARE(apply->text(), QStringLiteral("Use epoch 2 at weight 0.8"));
    table->selectRow(2);
    apply->click();
    const ai::CharacterLora lora = ai::CharacterLora::fromJson(studio.characters()->entry(characterId)->lora);
    appliedLora = lora.file;
    QCOMPARE(QFileInfo(lora.file).fileName(), QStringLiteral("lmn_mira-e03.safetensors"));
    QCOMPARE(lora.weight, 0.8);
    QCOMPARE(lora.trigger, QStringLiteral("lmn_mira"));
    QCOMPARE(lora.family, QStringLiteral("SDXL"));
    QTRY_VERIFY(studio.models()->entry(lora.file).has_value());
    QCOMPARE(studio.models()->entry(lora.file)->source.value(QStringLiteral("epoch")).toInt(), 3);

    // A stopped test is not "finished": no recommendation to apply.
    table->clearSelection();
    QTRY_VERIFY(test->isEnabled());
    test->click();
    QVERIFY(LoraEvaluation::running(characterId));
    test->click(); // "Stop testing"
    QTRY_VERIFY_WITH_TIMEOUT(!LoraEvaluation::running(characterId), 30000);
    QVERIFY(!LoraEvaluation::saved(runDir).value(QStringLiteral("finished")).toBool());
    QTRY_VERIFY(!apply->isVisible());
}

void TestCanvas::activeCharacter()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    studio.openProject(dir.filePath(QStringLiteral("characters.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    const QJsonObject loraJson{{QStringLiteral("file"), dir.filePath(QStringLiteral("lmn_aki.safetensors"))},
                               {QStringLiteral("trigger"), QStringLiteral("lmn_aki")},
                               {QStringLiteral("weight"), 0.7},
                               {QStringLiteral("family"), QStringLiteral("SDXL")}};
    auto a = studio.characters()->save(QStringLiteral("Aki (active test)"), {{QStringLiteral("tags"), QStringLiteral("red hair")}}, {ref},
                                       QString(), QString(), loraJson);
    auto b = studio.characters()->save(QStringLiteral("Bo (active test)"), {{QStringLiteral("tags"), QStringLiteral("blue hair")}}, {ref});
    QTRY_VERIFY(a.isFinished() && b.isFinished());
    const QString aki = a.result().id;
    const QString bo = b.result().id;
    auto cleanup = qScopeGuard([&] {
        Settings::instance().setValue(QStringLiteral("character/use/test"), true);
        studio.characters()->remove(aki);
        studio.characters()->remove(bo);
    });

    // A chip follows the active character; its LoRA shows.
    CharacterChip chip(&studio, QStringLiteral("test"));
    chip.setUsedHere(true);
    QSignalSpy chipChanged(&chip, &CharacterChip::characterChanged);
    QCOMPARE(chip.characterId(), QString());
    QCOMPARE(chip.text(), QStringLiteral("No character"));
    QSignalSpy changed(&studio, &Studio::activeCharacterChanged);
    studio.setActiveCharacter(aki);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(chip.characterId(), aki);
    QCOMPARE(chip.text(), QStringLiteral("Aki (active test) · LoRA"));
    QVERIFY(chipChanged.size() >= 1);

    // The character as requests use it.
    auto context = studio.characterContext(aki);
    QTRY_VERIFY(context.isFinished());
    QCOMPARE(context.result().name, QStringLiteral("Aki (active test)"));
    QCOMPARE(context.result().references.size(), 1);
    QCOMPARE(context.result().lora.trigger, QStringLiteral("lmn_aki"));
    auto missing = studio.characterContext(QStringLiteral("nope"));
    QTRY_VERIFY(missing.isFinished());
    QVERIFY(missing.result().isEmpty());

    // Switched off on this page only: the project keeps its character; the switch is remembered.
    chip.setUsedHere(false);
    QCOMPARE(chip.characterId(), QString());
    QCOMPARE(studio.activeCharacter(), aki);
    QVERIFY(chip.font().strikeOut());
    QVERIFY(!CharacterChip(&studio, QStringLiteral("test")).isUsedHere());
    QVERIFY(CharacterChip(&studio, QStringLiteral("other")).isUsedHere());
    chip.setUsedHere(true);

    // The menu lists the library and makes another character active.
    emit chip.menu()->aboutToShow();
    QAction *pickBo = nullptr;
    for (QAction *act : chip.menu()->actions()) {
        if (act->text() == QStringLiteral("Bo (active test)"))
            pickBo = act;
    }
    QVERIFY(pickBo);
    pickBo->trigger();
    QCOMPARE(studio.activeCharacter(), bo);
    QCOMPARE(chip.text(), QStringLiteral("Bo (active test)"));

    // Stored in the project: reopening it restores the character.
    studio.setActiveCharacter(aki);
    QTest::qWait(200);
    studio.openProject(dir.filePath(QStringLiteral("other.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("other.lumen")), 5000);
    QTRY_COMPARE(studio.activeCharacter(), QString());
    studio.openProject(dir.filePath(QStringLiteral("characters.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("characters.lumen")), 5000);
    QTRY_COMPARE(studio.activeCharacter(), aki);

    // The Create page makes its session's library character active when it changes.
    CreatePage page(&studio);
    QTRY_VERIFY(page.findChild<DirectorSession *>());
    page.findChild<DirectorSession *>()->setLibraryCharacter(bo);
    QCOMPARE(studio.activeCharacter(), bo);

    // The assistant lists the characters and switches the active one.
    const ai::ToolSet tools = assistantTools(&studio);
    auto listed = tools.run(QStringLiteral("list_characters"), {});
    QTRY_VERIFY(listed.isFinished());
    QCOMPARE(listed.result().data.value(QStringLiteral("active")).toString(), bo);
    bool akiHasLora = false;
    for (const QJsonValue &v : listed.result().data.value(QStringLiteral("characters")).toArray()) {
        if (v[QStringLiteral("id")].toString() == aki)
            akiHasLora = v[QStringLiteral("has_lora")].toBool();
    }
    QVERIFY(akiHasLora);
    auto set = tools.run(QStringLiteral("set_active_character"), {{QStringLiteral("character_id"), aki}});
    QTRY_VERIFY(set.isFinished());
    QCOMPARE(studio.activeCharacter(), aki);
    auto state = tools.run(QStringLiteral("get_state"), {});
    QTRY_VERIFY(state.isFinished());
    QCOMPARE(state.result().data.value(QStringLiteral("active_character")).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("Aki (active test)"));
    auto wrong = tools.run(QStringLiteral("set_active_character"), {{QStringLiteral("character_id"), QStringLiteral("nope")}});
    QTRY_VERIFY(wrong.isFinished());
    QVERIFY(!wrong.result().error.isEmpty());
    QCOMPARE(studio.activeCharacter(), aki);

    // A character removed from the library stops being active.
    studio.characters()->remove(aki);
    QTRY_COMPARE(studio.activeCharacter(), QString());
    QCOMPARE(chip.text(), QStringLiteral("No character"));
}

// File → New from prompt, which replaces the Generate page: a new document of the chosen size, the
// request (prompt, size, variants, seed), the results as layers; and the ways in that went to the
// Generate page: Re-run, Reuse prompt, Use as reference and the Poses plugin's pose.
void TestCanvas::newFromPrompt()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    MockHttpServer server;
    QList<QJsonObject> generations;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) { return MockResponse::json({}); });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        generations << req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_%1").arg(generations.size())}}, 202);
    });
    // Two variants, smaller than the document (the editor scales them to its size).
    for (int job = 1; job <= 3; ++job) {
        server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_%1").arg(job), [](const MockRequest &) {
            QJsonArray images;
            for (const QColor &c : {QColor(Qt::darkCyan), QColor(Qt::darkRed)}) {
                QImage im(96, 64, QImage::Format_ARGB32);
                im.fill(c);
                images.append(QJsonObject{{QStringLiteral("index"), images.size()}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(im).toBase64())}});
            }
            return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
        });
    }
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"),
                                            {{QStringLiteral("url"), server.baseUrl().toString()}, {QStringLiteral("control_net"), true}}));
    auto cleanup = qScopeGuard([&] {
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        Settings::instance().remove(QStringLiteral("newFromPrompt"));
    });
    studio.openProject(dir.filePath(QStringLiteral("new.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.editor()->showsStartScreen());

    // File → New from prompt…: nothing to create without words.
    auto *action = window.findChild<QAction *>(QStringLiteral("newFromPrompt"));
    QVERIFY(action);
    action->trigger();
    QPointer<NewFromPromptDialog> dialog = window.newFromPromptDialog();
    QVERIFY(dialog && dialog->isVisible());
    QPushButton *create = nullptr;
    for (QPushButton *b : dialog->findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Create"))
            create = b;
    }
    AutoTextEdit *prompt = nullptr;
    for (AutoTextEdit *e : dialog->findChildren<AutoTextEdit *>()) {
        if (e->accessibleName() == QLatin1String("Prompt"))
            prompt = e;
    }
    QSpinBox *variants = nullptr;
    for (QSpinBox *s : dialog->findChildren<QSpinBox *>()) {
        if (s->maximum() == 4)
            variants = s;
    }
    QToolButton *landscape = nullptr;
    for (QToolButton *b : dialog->findChildren<QToolButton *>()) {
        if (b->text() == QLatin1String("3:2"))
            landscape = b;
    }
    QVERIFY(create && prompt && variants && landscape);
    QVERIFY(!create->isEnabled());
    auto *engine = dialog->findChild<ProviderCombo *>(QStringLiteral("imageEngine"));
    QVERIFY(engine);
    QCOMPARE(engine->currentText(), QStringLiteral("SD"));

    // A landscape picture, two variants: a new document of that size with both as layers.
    prompt->setPlainText(QStringLiteral("a lighthouse at dusk"));
    QVERIFY(create->isEnabled());
    landscape->click();
    variants->setValue(2);
    const QSize size = dialog->requestedSize();
    QCOMPARE(size, ai::Director::sizeForAspect(QStringLiteral("3:2"), 1024 * 1024, 64)); // at the model's pixels
    QVERIFY(size.width() > size.height());
    create->click();
    QTRY_VERIFY(!dialog);
    doc::Document *doc = studio.activeDocument();
    QVERIFY(doc);
    QCOMPARE(doc->size(), size);
    QCOMPARE(doc->name(), QStringLiteral("a lighthouse at dusk"));
    QVERIFY(!window.editor()->showsStartScreen());
    // The two variants are shown in the results strip over the new document; nothing is added yet.
    VariantStrip *strip = window.editor()->variantStrip();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(doc->layerCount(), 1);
    QCOMPARE(window.editor()->canvas()->preview().pixelColor(600, 400), QColor(Qt::darkCyan));
    QCOMPARE(generations.size(), 1);
    const QJsonObject sent = generations.first();
    QVERIFY2(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("a lighthouse at dusk")), qPrintable(sent.value(QStringLiteral("prompt")).toString()));
    QCOMPARE(sent.value(QStringLiteral("width")).toInt(), size.width());
    QCOMPARE(sent.value(QStringLiteral("height")).toInt(), size.height());
    QCOMPARE(sent.value(QStringLiteral("batch_count")).toInt(), 2);
    QVERIFY(sent.value(QStringLiteral("seed")).toDouble() >= 0);
    QVERIFY(!sent.contains(QStringLiteral("control_image"))); // no pose
    // The second one is kept: a layer on the background, scaled to the document, one step to undo;
    // both images are in the project with the words the user wrote.
    QTest::keyClick(strip, Qt::Key_Right);
    QCOMPARE(window.editor()->canvas()->preview().pixelColor(600, 400), QColor(Qt::darkRed));
    QTest::keyClick(strip, Qt::Key_Return);
    QCOMPARE(doc->layerCount(), 2);
    QCOMPARE(strip->count(), 0);
    QVERIFY(window.editor()->canvas()->preview().isNull());
    const doc::LayerPtr kept = doc->layers().at(1);
    QCOMPARE(kept->props.name, QStringLiteral("a lighthouse at dusk"));
    QCOMPARE(kept->image.size(), size);
    QCOMPARE(kept->image.pixelColor(600, 400), QColor(Qt::darkRed));
    QVERIFY(!kept->meta.value(QStringLiteral("asset")).toString().isEmpty());
    QCOMPARE(doc->undoStack()->count(), 1);
    std::optional<doc::AssetInfo> made;
    for (const doc::AssetInfo &a : studio.project()->assets()) {
        if (a.metadata.value(QStringLiteral("user_prompt")).toString() == QLatin1String("a lighthouse at dusk"))
            made = a;
    }
    QVERIFY(made);
    QCOMPARE(made->metadata.value(QStringLiteral("source")).toString(), QStringLiteral("new_from_prompt"));
    // The choices are kept for the next time.
    QCOMPARE(Settings::instance().value(QStringLiteral("newFromPrompt/variants")).toInt(), 2);

    // Re-run (Browse project images): the image's words, negative prompt, size and seed.
    doc::AssetInfo rerun = *made;
    rerun.metadata.insert(QStringLiteral("negative_prompt"), QStringLiteral("text, watermark"));
    rerun.metadata.insert(QStringLiteral("width"), 640);
    rerun.metadata.insert(QStringLiteral("height"), 960);
    rerun.metadata.insert(QStringLiteral("seed"), 4242);
    studio.project()->updateAsset(rerun);
    studio.rerunAsset(made->id);
    dialog = window.newFromPromptDialog();
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(dialog->requestedSize(), QSize(640, 960));
    QString negative;
    QSpinBox *seed = nullptr;
    for (AutoTextEdit *e : dialog->findChildren<AutoTextEdit *>()) {
        if (e->accessibleName() == QLatin1String("Prompt"))
            QCOMPARE(e->toPlainText(), QStringLiteral("a lighthouse at dusk"));
        if (e->accessibleName() == QLatin1String("Negative prompt"))
            negative = e->toPlainText();
    }
    for (QSpinBox *s : dialog->findChildren<QSpinBox *>()) {
        if (s->accessibleName() == QLatin1String("Seed"))
            seed = s;
    }
    QCOMPARE(negative, QStringLiteral("text, watermark"));
    QVERIFY(seed && seed->isEnabled());
    QCOMPARE(seed->value(), 4242);
    doc::Document *again = dialog->create();
    QVERIFY(again && again != doc);
    QCOMPARE(again->size(), QSize(640, 960));
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 2, 10000);
    QCOMPARE(generations[1].value(QStringLiteral("seed")).toDouble(), 4242.0);
    QCOMPARE(generations[1].value(QStringLiteral("negative_prompt")).toString(), QStringLiteral("text, watermark"));
    dialog->close();
    QTRY_VERIFY(!dialog);

    // "New image with this pose" (the Poses plugin): the pose controls the image and stays with
    // the new document; the shape follows the pose's canvas.
    const pose::Pose standing = pose::findBuiltInPreset(QStringLiteral("standing"))->toPose(QSize(832, 1216));
    studio.usePoseForGeneration(standing);
    dialog = window.newFromPromptDialog();
    QVERIFY(dialog && dialog->isVisible());
    const QSize portrait = dialog->requestedSize();
    QVERIFY2(portrait.height() > portrait.width(), qPrintable(QStringLiteral("%1×%2").arg(portrait.width()).arg(portrait.height())));
    dialog->setPrompt(QStringLiteral("a dancer"));
    doc::Document *posed = dialog->create();
    QVERIFY(posed);
    QVERIFY(posed->controlPose());
    QCOMPARE(posed->controlPose()->canvas, portrait);
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 3, 10000);
    QVERIFY(generations[2].contains(QStringLiteral("control_image")));
    dialog->close();
    QTRY_VERIFY(!dialog);

    // Reuse prompt lands in the prompt bar, Use as reference in the AI panel (which opens).
    AiPanel *ai = window.editor()->aiPanel();
    studio.usePrompt(QStringLiteral("a red umbrella"), QString());
    QCOMPARE(window.editor()->promptBar()->prompt(), QStringLiteral("a red umbrella"));
    QCOMPARE(window.editor()->aiOptions()->prompt(), QStringLiteral("a red umbrella"));
    QCOMPARE(ai->references()->references().size(), 0);
    QVERIFY(!window.editor()->isAiPanelShown());
    studio.useAsReference(made->id);
    QTRY_COMPARE(ai->references()->references().size(), 1);
    QVERIFY(window.editor()->isAiPanelShown());
    QVERIFY(!window.newFromPromptDialog());
    // With no document open, a reused prompt is one for a new image.
    const auto open = studio.documents();
    for (doc::Document *d : open)
        studio.closeDocument(d);
    studio.usePrompt(QStringLiteral("a quiet harbour"), QString());
    dialog = window.newFromPromptDialog();
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(dialog->findChild<AutoTextEdit *>()->toPlainText(), QStringLiteral("a quiet harbour"));
    dialog->close();
}

void TestCanvas::editorCharacter()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    // Characters are made by the Characters plugin; the AI panel draws one in while it is on.
    PluginsOn plugins({QStringLiteral("characters")});
    MockHttpServer server;
    QList<QJsonObject> generations;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("loras"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("lmn_aki")},
                                                                                    {QStringLiteral("path"), QStringLiteral("lmn_aki.safetensors")}}}}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        generations << req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [](const MockRequest &) {
        QImage im(128, 128, QImage::Format_ARGB32);
        im.fill(Qt::darkCyan);
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(im).toBase64())}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    studio.openProject(dir.filePath(QStringLiteral("editor.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Aki (editor test)"), {{QStringLiteral("tags"), QStringLiteral("red hair, green eyes")}}, {ref},
                                           QString(), QString(),
                                           QJsonObject{{QStringLiteral("file"), dir.filePath(QStringLiteral("lmn_aki.safetensors"))},
                                                       {QStringLiteral("trigger"), QStringLiteral("lmn_aki")},
                                                       {QStringLiteral("weight"), 0.7}});
    QTRY_VERIFY(saved.isFinished());
    const QString aki = saved.result().id;
    auto cleanup = qScopeGuard([&] {
        Settings::instance().setValue(QStringLiteral("character/use/editor"), true);
        studio.characters()->remove(aki);
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    studio.setActiveCharacter(aki);

    AiOptions options;
    AiPanel panel(&studio, &options);
    auto *chip = panel.findChild<CharacterChip *>();
    QVERIFY(chip);
    chip->setUsedHere(true);
    doc::Document *doc = studio.newDocument(QSize(128, 128), QStringLiteral("Editor character test"));
    // What the prompt bar sends for a selection with a model without instruction edits.
    auto fill = [&panel](const QString &words, bool character = true) {
        panel.submitPrompt({.mode = ai::GenMode::Inpaint, .region = true, .prompt = words, .withCharacter = character, .name = words});
    };
    panel.setDocument(doc);
    QImage mask = img::createMask(doc->size(), 0);
    {
        QPainter p(&mask);
        p.fillRect(QRect(32, 32, 64, 64), Qt::white);
    }
    auto loraOf = [](const QJsonObject &g) { return g.value(QStringLiteral("lora")).toArray(); };
    auto promptOf = [](const QJsonObject &g) { return g.value(QStringLiteral("prompt")).toString(); };

    // Fill selection: the LoRA and its trigger, but not the hair and eye tags (they would repaint
    // the region).
    doc->setSelectionRaw(mask);
    QVERIFY(doc->hasSelection());
    fill(QStringLiteral("a hand holding a cup"));
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 1, 10000);
    QVERIFY2(promptOf(generations[0]).startsWith(QStringLiteral("lmn_aki, ")), qPrintable(promptOf(generations[0])));
    QVERIFY(promptOf(generations[0]).contains(QStringLiteral("a hand holding a cup")));
    QVERIFY(!promptOf(generations[0]).contains(QStringLiteral("red hair")));
    QCOMPARE(loraOf(generations[0]).size(), 1);
    QCOMPARE(loraOf(generations[0])[0][QStringLiteral("multiplier")].toDouble(), 0.7);

    // Removing an object never draws the character.
    fill(QStringLiteral("empty background, seamless continuation of the surroundings"), false);
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 2, 10000);
    QVERIFY(!promptOf(generations[1]).contains(QStringLiteral("lmn_aki")));
    QVERIFY(loraOf(generations[1]).isEmpty());

    // A variation of the whole image: the LoRA and the description.
    doc->setSelectionRaw(QImage());
    QVERIFY(!doc->hasSelection());
    options.setPrompt(QStringLiteral("evening light"));
    panel.run(AiPanel::Mode::Variation);
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 3, 10000);
    QVERIFY(promptOf(generations[2]).startsWith(QStringLiteral("lmn_aki, ")));
    QVERIFY(promptOf(generations[2]).contains(QStringLiteral("red hair, green eyes")));
    QCOMPARE(loraOf(generations[2]).size(), 1);

    // Expanding the canvas: a region, so the LoRA without the tags.
    options.setPrompt(QStringLiteral("forest"));
    panel.run(AiPanel::Mode::Expand);
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 4, 10000);
    QVERIFY(promptOf(generations[3]).startsWith(QStringLiteral("lmn_aki, ")));
    QVERIFY(!promptOf(generations[3]).contains(QStringLiteral("red hair")));
    QCOMPARE(loraOf(generations[3]).size(), 1);

    // Switched off in the editor: nothing of the character.
    chip->setUsedHere(false);
    doc->setSelectionRaw(img::createMask(doc->size(), 255));
    fill(QStringLiteral("a hat"));
    QTRY_COMPARE_WITH_TIMEOUT(generations.size(), 5, 10000);
    QVERIFY(!promptOf(generations[4]).contains(QStringLiteral("lmn_aki")));
    QVERIFY(loraOf(generations[4]).isEmpty());
    // The edits record the character they used.
    bool recorded = false;
    for (const doc::AssetInfo &a : studio.project()->assets())
        recorded = recorded || a.metadata.value(QStringLiteral("character")).toObject().value(QStringLiteral("character")).toString() == aki;
    QVERIFY(recorded);
}

void TestCanvas::partsPageCharacter()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    studio.openProject(dir.filePath(QStringLiteral("parts.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto a = studio.characters()->save(QStringLiteral("Aki (parts test)"), {{QStringLiteral("tags"), QStringLiteral("red hair, green eyes")}}, {ref},
                                       QString(), QString(),
                                       QJsonObject{{QStringLiteral("file"), dir.filePath(QStringLiteral("lmn_aki.safetensors"))},
                                                   {QStringLiteral("trigger"), QStringLiteral("lmn_aki")}});
    auto b = studio.characters()->save(QStringLiteral("Bo (parts test)"), {{QStringLiteral("tags"), QStringLiteral("blue hair")}}, {ref});
    QTRY_VERIFY(a.isFinished() && b.isFinished());
    const QString aki = a.result().id;
    const QString bo = b.result().id;
    auto cleanup = qScopeGuard([&] {
        studio.characters()->remove(aki);
        studio.characters()->remove(bo);
    });
    PartsPage page(&studio);
    auto *chip = page.findChild<CharacterChip *>();
    QVERIFY(chip);
    chip->setUsedHere(true);

    // An image that names its character (Create, Generate and the editor record it) makes that
    // character the project's one when it is opened for slicing.
    QImage figure(128, 192, QImage::Format_ARGB32);
    figure.fill(Qt::transparent);
    {
        QPainter p(&figure);
        p.fillRect(QRect(40, 20, 48, 160), Qt::red);
    }
    auto withCharacter = studio.project()->addAsset(figure, doc::AssetKind::Generation, QStringLiteral("Aki shot"),
                                                    {{QStringLiteral("character"), QJsonObject{{QStringLiteral("character"), aki}}}});
    QTRY_VERIFY(withCharacter.isFinished());
    studio.sliceAsset(withCharacter.result().id);
    QTRY_COMPARE(studio.activeCharacter(), aki);
    QCOMPARE(chip->characterId(), aki);
    // The empty description box says what is used instead.
    bool hint = false;
    for (AutoTextEdit *e : page.findChildren<AutoTextEdit *>())
        hint = hint || e->placeholderText().contains(QStringLiteral("Aki (parts test)'s LoRA and description are used (red hair, green eyes)"));
    QVERIFY(hint);

    // An older Create image names only its director session; the session names the character.
    doc::Record session;
    session.id = QStringLiteral("session-bo");
    session.type = QStringLiteral("director");
    session.name = QStringLiteral("Bo session");
    session.json = QJsonObject{{QStringLiteral("library_character"), bo}};
    auto savedSession = studio.project()->saveRecord(session);
    QTRY_VERIFY(savedSession.isFinished());
    auto fromSession = studio.project()->addAsset(figure, doc::AssetKind::Generation, QStringLiteral("Bo shot"),
                                                  {{QStringLiteral("director_session"), QStringLiteral("session-bo")}});
    QTRY_VERIFY(fromSession.isFinished());
    QTimer::singleShot(0, [] {
        // Opening a second image asks whether to start a new session.
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    studio.sliceAsset(fromSession.result().id);
    QTRY_COMPARE(studio.activeCharacter(), bo);
    // An image without a character leaves the active one alone.
    auto plain = studio.project()->addAsset(figure, doc::AssetKind::Import, QStringLiteral("plain"), {});
    QTRY_VERIFY(plain.isFinished());
    studio.sliceAsset(plain.result().id);
    QTest::qWait(100);
    QCOMPARE(studio.activeCharacter(), bo);
}

void TestCanvas::expressionCheck()
{
    // The crops: the face area around the variant, before and after, at least 256 px.
    QImage source(400, 400, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(240, 200, 180));
    QImage layer(40, 20, QImage::Format_ARGB32_Premultiplied);
    layer.fill(QColor(20, 20, 20));
    const QRect rect(180, 180, 40, 20);
    const auto [before, after] = expressionCrops(source, layer, rect);
    QCOMPARE(before.size(), QSize(256, 256));
    QCOMPARE(after.size(), before.size());
    QVERIFY(before.pixelColor(128, 127) != after.pixelColor(128, 127)); // the variant sits at the centre

    // A mock vision model answers the identity checklist (face, red hair, green eyes).
    MockHttpServer server;
    QString verdicts = QStringLiteral("mmd");
    int pose = 9;
    QByteArray lastRequest;
    server.route("POST", QStringLiteral("/v1/chat/completions"), [&](const MockRequest &req) {
        lastRequest = req.body;
        QJsonObject features;
        for (int i = 0; i < verdicts.size(); ++i)
            features.insert(QStringLiteral("f%1").arg(i + 1), verdicts[i] == QLatin1Char('m') ? QStringLiteral("match") : QStringLiteral("different"));
        const QJsonObject answer{{QStringLiteral("features"), features}, {QStringLiteral("pose"), pose}, {QStringLiteral("issues"), QJsonArray{}}};
        return MockResponse::json(
            {{QStringLiteral("choices"),
              QJsonArray{QJsonObject{{QStringLiteral("message"),
                                      QJsonObject{{QStringLiteral("content"), QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact))}}},
                                     {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}});
    });
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("llamacpp"), QStringLiteral("Vision"),
                                            {{QStringLiteral("url"), server.baseUrl().toString() + QStringLiteral("/v1")}}));
    auto cleanup = qScopeGuard([&] {
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    doc::PartSet set;
    set.setSource(source, QStringLiteral("src"), img::createMask(source.size(), 255));
    const int variant = set.addPart(QStringLiteral("eyes closed"), QStringLiteral("Eyes closed"));
    QVERIFY(variant);
    set.addVariant(variant, QStringLiteral("asset-1"), rect, true);
    set.setResultImage(QStringLiteral("asset-1"), layer);
    ai::CharacterContext aki;
    aki.name = QStringLiteral("Aki");
    aki.sheet = {{QStringLiteral("tags"), QStringLiteral("red hair, green eyes")}};
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    aki.references = {ref};

    // Another eye colour: a warning on the variant.
    checkExpressionVariant(&studio, &set, variant, aki, QString(), QStringLiteral("both eyes gently closed"));
    QTRY_VERIFY_WITH_TIMEOUT(!set.part(variant)->warning.isEmpty(), 10000);
    QCOMPARE(set.part(variant)->warning, QStringLiteral("the Critic sees a change: green eyes"));
    QVERIFY(lastRequest.contains("identity_checklist"));
    QVERIFY(lastRequest.contains("Planned expression: both eyes gently closed"));
    QCOMPARE(lastRequest.count("data:image/"), 3); // the result, the face before, the character's reference
    // A clean check clears it.
    verdicts = QStringLiteral("mmm");
    checkExpressionVariant(&studio, &set, variant, aki, QString(), QStringLiteral("both eyes gently closed"));
    QTRY_VERIFY_WITH_TIMEOUT(set.part(variant)->warning.isEmpty(), 10000);
    // An expression that barely shows; without a character the description box gives the features.
    pose = 3;
    verdicts = QStringLiteral("mm");
    checkExpressionVariant(&studio, &set, variant, ai::CharacterContext{}, QStringLiteral("silver hair"), QStringLiteral("a wide smile"));
    QTRY_COMPARE_WITH_TIMEOUT(set.part(variant)->warning, QStringLiteral("the expression barely shows"), 10000);
    QVERIFY(lastRequest.contains("silver hair"));
    // Choosing or adding another result drops the warning (it was about the old one).
    set.addVariant(variant, QStringLiteral("asset-2"), rect, true);
    QVERIFY(set.part(variant)->warning.isEmpty());
}

void TestCanvas::assistantCharacter()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    MockHttpServer server;
    QList<QJsonObject> generations;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("loras"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("lmn_aki")},
                                                                                    {QStringLiteral("path"), QStringLiteral("lmn_aki.safetensors")}}}}});
    });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        generations << req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [](const MockRequest &) {
        QImage im(128, 128, QImage::Format_ARGB32);
        im.fill(Qt::darkCyan);
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(im).toBase64())}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    studio.openProject(dir.filePath(QStringLiteral("assistant.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Aki (assistant test)"), {{QStringLiteral("tags"), QStringLiteral("red hair, green eyes")}},
                                           {ref}, QString(), QString(),
                                           QJsonObject{{QStringLiteral("file"), dir.filePath(QStringLiteral("lmn_aki.safetensors"))},
                                                       {QStringLiteral("trigger"), QStringLiteral("lmn_aki")},
                                                       {QStringLiteral("weight"), 0.7}});
    QTRY_VERIFY(saved.isFinished());
    const QString aki = saved.result().id;
    auto cleanup = qScopeGuard([&] {
        studio.characters()->remove(aki);
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    studio.setActiveCharacter(aki);
    const ai::ToolSet tools = assistantTools(&studio);
    auto run = [&tools](const char *name, const QJsonObject &args) {
        auto f = tools.run(QLatin1String(name), args);
        return f;
    };
    auto promptOf = [&generations](int i) { return generations.value(i).value(QStringLiteral("prompt")).toString(); };
    auto lorasOf = [&generations](int i) { return generations.value(i).value(QStringLiteral("lora")).toArray(); };

    // "Draw Aki at the beach": the scene only; the LoRA and the description come with the character.
    auto drawn = run("generate_image", {{QStringLiteral("prompt"), QStringLiteral("at the beach, sunset")}});
    QTRY_VERIFY_WITH_TIMEOUT(drawn.isFinished(), 10000);
    QVERIFY2(drawn.result().error.isEmpty(), qPrintable(drawn.result().error));
    QVERIFY2(promptOf(0).startsWith(QStringLiteral("lmn_aki, at the beach, sunset")), qPrintable(promptOf(0)));
    QVERIFY(promptOf(0).contains(QStringLiteral("red hair, green eyes")));
    QCOMPARE(lorasOf(0).size(), 1);
    const QJsonObject info = drawn.result().data.value(QStringLiteral("character")).toObject();
    QCOMPARE(info.value(QStringLiteral("name")).toString(), QStringLiteral("Aki (assistant test)"));
    QCOMPARE(info.value(QStringLiteral("used")).toString(), QStringLiteral("LoRA, description"));
    QCOMPARE(drawn.result().assetIds.size(), 1);
    const QString image = drawn.result().assetIds.first();
    QCOMPARE(studio.project()->asset(image)->metadata.value(QStringLiteral("character")).toObject().value(QStringLiteral("character")).toString(), aki);

    // character: false leaves it out.
    auto plain = run("generate_image", {{QStringLiteral("prompt"), QStringLiteral("an empty beach")}, {QStringLiteral("character"), false}});
    QTRY_VERIFY_WITH_TIMEOUT(plain.isFinished(), 10000);
    QCOMPARE(promptOf(1), QStringLiteral("an empty beach"));
    QVERIFY(lorasOf(1).isEmpty());
    QVERIFY(!plain.result().data.contains(QStringLiteral("character")));

    // inpaint_region: the LoRA for the region, without the appearance words.
    auto fixed = run("inpaint_region", {{QStringLiteral("asset_id"), image}, {QStringLiteral("x"), 0.25}, {QStringLiteral("y"), 0.25},
                                        {QStringLiteral("width"), 0.5}, {QStringLiteral("height"), 0.5},
                                        {QStringLiteral("prompt"), QStringLiteral("a hand holding a shell")}});
    QTRY_VERIFY_WITH_TIMEOUT(fixed.isFinished(), 10000);
    QVERIFY2(fixed.result().error.isEmpty(), qPrintable(fixed.result().error));
    QVERIFY(promptOf(2).startsWith(QStringLiteral("lmn_aki, a hand holding a shell")));
    QVERIFY(!promptOf(2).contains(QStringLiteral("red hair")));
    QCOMPARE(lorasOf(2).size(), 1);

    // edit_image: no LoRA (edits take none); this back-end takes no reference either, so the description.
    auto edited = run("edit_image", {{QStringLiteral("asset_id"), image}, {QStringLiteral("instruction"), QStringLiteral("make it night")}});
    QTRY_VERIFY_WITH_TIMEOUT(edited.isFinished(), 10000);
    QVERIFY2(edited.result().error.isEmpty(), qPrintable(edited.result().error));
    QVERIFY(promptOf(3).startsWith(QStringLiteral("make it night")));
    QVERIFY(promptOf(3).contains(QStringLiteral("red hair, green eyes")));
    QVERIFY(lorasOf(3).isEmpty());
    QCOMPARE(edited.result().data.value(QStringLiteral("character")).toObject().value(QStringLiteral("used")).toString(), QStringLiteral("description"));

    // No active character: the tools behave as before.
    studio.setActiveCharacter(QString());
    auto none = run("generate_image", {{QStringLiteral("prompt"), QStringLiteral("a lighthouse")}});
    QTRY_VERIFY_WITH_TIMEOUT(none.isFinished(), 10000);
    QCOMPARE(promptOf(4), QStringLiteral("a lighthouse"));
}

void TestCanvas::safeContent()
{
    // The terms join the user's negative prompt once, whatever its case and separators.
    QCOMPARE(ai::GenerationService::withSafeNegative(QString()), ai::GenerationService::safeNegativeTerms());
    QCOMPARE(ai::GenerationService::withSafeNegative(QStringLiteral("lowres, NSFW, bad hands")),
             QStringLiteral("lowres, NSFW, bad hands, nude, nudity, explicit, cleavage, underwear, lingerie"));
    const QString twice = ai::GenerationService::withSafeNegative(ai::GenerationService::withSafeNegative(QStringLiteral("blurry")));
    QCOMPARE(twice.count(QStringLiteral("nsfw")), 1);

    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().setValue(QStringLiteral("generation/safeContent"), false);
    MockHttpServer server;
    QList<QJsonObject> generations;
    server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"), [](const MockRequest &) { return MockResponse::json({}); });
    server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [&](const MockRequest &req) {
        generations << req.json();
        return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_1")}}, 202);
    });
    server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_1"), [](const MockRequest &) {
        QImage im(64, 64, QImage::Format_ARGB32);
        im.fill(Qt::darkCyan);
        const QJsonArray images{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("b64_json"), QString::fromLatin1(img::encode(im).toBase64())}}};
        return MockResponse::json({{QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("images"), images}}}});
    });
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    QVERIFY(studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"), {{QStringLiteral("url"), server.baseUrl().toString()}}));
    auto cleanup = qScopeGuard([&] {
        Settings::instance().setValue(QStringLiteral("generation/safeContent"), false);
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    auto generate = [&](double chosenCfg = 0) -> QString {
        ai::GenerationSpec spec;
        if (chosenCfg > 0) {
            spec.request.cfg = chosenCfg;
            spec.request.cfgChosen = true;
        }
        spec.request.prompt = QStringLiteral("1girl, beach");
        spec.request.negativePrompt = QStringLiteral("lowres");
        spec.request.size = QSize(64, 64);
        spec.saveAssets = false;
        Job *job = studio.generation()->submit(spec);
        if (!QTest::qWaitFor([job] { return job->isFinished(); }, 10000))
            return QStringLiteral("timeout");
        return generations.last().value(QStringLiteral("negative_prompt")).toString();
    };
    QCOMPARE(generate(), QStringLiteral("lowres"));
    auto cfgSent = [&] {
        return generations.last().value(QStringLiteral("sample_params")).toObject().value(QStringLiteral("guidance")).toObject()
            .value(QStringLiteral("txt_cfg")).toDouble();
    };
    QCOMPARE(cfgSent(), 6.0); // Lumen's usual guidance

    // The status bar button and the setting follow each other.
    MainWindow window(&studio);
    auto *shield = window.findChild<QToolButton *>(QStringLiteral("safeContent"));
    QVERIFY(shield && !shield->isChecked());
    shield->click();
    QVERIFY(Settings::instance().value(QStringLiteral("generation/safeContent")).toBool());
    QVERIFY(studio.generation()->safeContent());
    QCOMPARE(generate(), QStringLiteral("lowres, ") + ai::GenerationService::safeNegativeTerms());
    Settings::instance().setValue(QStringLiteral("generation/safeContent"), false);
    QVERIFY(!shield->isChecked());
    QCOMPARE(generate(), QStringLiteral("lowres"));
    QCOMPARE(generations.last().value(QStringLiteral("prompt")).toString(), QStringLiteral("1girl, beach"));

    // An Anima model also gets its "safe" rating tag.
    ai::Provider *sd = studio.providers()->providers().first();
    QVariantMap settings = studio.providers()->settingsFor(sd->instanceId());
    settings.insert(QStringLiteral("model_family"), QStringLiteral("Anima"));
    studio.providers()->updateProvider(sd->instanceId(), sd->displayName(), settings);
    shield->click();
    QCOMPARE(generate(), QStringLiteral("lowres, ") + ai::GenerationService::safeNegativeTerms());
    QCOMPARE(generations.last().value(QStringLiteral("prompt")).toString(), QStringLiteral("safe, 1girl, beach"));
    Settings::instance().setValue(QStringLiteral("generation/safeContent"), false);
    QCOMPARE(generate(), QStringLiteral("lowres"));
    QCOMPARE(generations.last().value(QStringLiteral("prompt")).toString(), QStringLiteral("1girl, beach"));
    // Anima's recommended guidance, unless the user chose one.
    QCOMPARE(cfgSent(), 4.5);
    generate(7.0);
    QCOMPARE(cfgSent(), 7.0);
}

void TestCanvas::cloudTrainingPage()
{
    MockHttpServer server;
    bool refuse = false;
    server.route("GET", QStringLiteral("/v2/catalog/gpus"), [&](const MockRequest &) {
        if (refuse)
            return MockResponse::json({{QStringLiteral("title"), QStringLiteral("Unauthorized")}, {QStringLiteral("status"), 401}}, 401);
        auto gpu = [](const char *id, int memory, double secure, double community) {
            return QJsonObject{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("name"), QLatin1String(id)},
                               {QStringLiteral("memory"), memory},
                               {QStringLiteral("price"), QJsonObject{{QStringLiteral("secure"), secure}, {QStringLiteral("community"), community}}},
                               {QStringLiteral("maxCount"), QJsonObject{{QStringLiteral("secure"), secure > 0 ? 4 : 0},
                                                                        {QStringLiteral("community"), community > 0 ? 4 : 0}}}};
        };
        return MockResponse::json({{QStringLiteral("gpus"), QJsonArray{gpu("RTX A4000", 16, 0.17, 0.09), gpu("RTX 4090", 24, 0.69, 0.34),
                                                                       gpu("A100 80GB", 80, 1.64, 0.0), gpu("RTX 6000 Ada", 48, 0.0, 0.0),
                                                                       gpu("AMD Instinct MI300X OAM", 192, 2.39, 0.5)}}});
    });
    Settings::instance().setValue(QStringLiteral("cloud/runpod/gpu"), QString());
    Settings::instance().setValue(QStringLiteral("cloud/runpod/community"), false);
    Studio studio;
    studio.runPod()->setBaseUrl(server.baseUrl().toString() + QStringLiteral("/v2"));
    studio.runPod()->keyProvider = [] { return QStringLiteral("rp_test"); };
    auto cleanup = qScopeGuard([] {
        Settings::instance().setValue(QStringLiteral("cloud/runpod/gpu"), QString());
        Settings::instance().setValue(QStringLiteral("cloud/runpod/community"), false);
    });

    // With a key the page loads the catalogue: GPUs with 24 GB or more that the cloud offers.
    CloudTrainingPage page(&studio);
    QComboBox *gpus = nullptr;
    QComboBox *cloud = nullptr;
    for (QComboBox *c : page.findChildren<QComboBox *>()) {
        if (c->findData(true) >= 0)
            cloud = c;
        else
            gpus = c;
    }
    QVERIFY(gpus && cloud);
    QTRY_COMPARE(gpus->count(), 2);
    QCOMPARE(gpus->itemText(0), QStringLiteral("RTX 4090 · 24 GB · $0.69/h"));
    QCOMPARE(gpus->itemData(1).toString(), QStringLiteral("A100 80GB"));
    // The first one becomes the preferred GPU until the user picks another.
    QCOMPARE(Settings::instance().value(QStringLiteral("cloud/runpod/gpu")).toString(), QStringLiteral("RTX 4090"));
    gpus->setCurrentIndex(1);
    emit gpus->activated(1);
    QCOMPARE(Settings::instance().value(QStringLiteral("cloud/runpod/gpu")).toString(), QStringLiteral("A100 80GB"));
    // Community cloud: its prices; the A100 is not offered there, so the choice falls back.
    cloud->setCurrentIndex(1);
    QVERIFY(Settings::instance().value(QStringLiteral("cloud/runpod/community")).toBool());
    QTRY_COMPARE(gpus->count(), 1);
    QCOMPARE(gpus->itemText(0), QStringLiteral("RTX 4090 · 24 GB · $0.34/h"));
    // A refused key shows RunPod's answer.
    refuse = true;
    page.refreshGpus();
    auto refusedShown = [&page] {
        const QList<QLabel *> labels = page.findChildren<QLabel *>();
        return std::any_of(labels.begin(), labels.end(), [](QLabel *l) { return l->text().contains(QStringLiteral("refused the API key")); });
    };
    QTRY_VERIFY(refusedShown());
}

void TestCanvas::cloudTrainingTab()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    auto resetSettings = [] {
        Settings::instance().setValue(QStringLiteral("cloud/runpod/gpu"), QString());
        Settings::instance().setValue(QStringLiteral("cloud/runpod/community"), false);
        Settings::instance().setValue(QStringLiteral("lora/train_cloud"), false);
    };
    resetSettings();
    Settings::instance().setValue(QStringLiteral("cloud/runpod/gpu"), QStringLiteral("A100 80GB"));
    QTemporaryDir dir;
    // A base model downloaded from Hugging Face: the pod can fetch it itself.
    const QString checkpoint = QDir::cleanPath(dir.filePath(QStringLiteral("models/checkpoints/xl.safetensors")));
    QDir().mkpath(QFileInfo(checkpoint).absolutePath());
    {
        QFile f(checkpoint);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(fakeSafetensors({QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"),
                                 QStringLiteral("model.diffusion_model.label_emb.0.0.weight"),
                                 QStringLiteral("conditioner.embedders.1.model.ln_final.weight")}));
        QFile side(runtime::sidecarPath(checkpoint));
        QVERIFY(side.open(QIODevice::WriteOnly));
        side.write(QJsonDocument(QJsonObject{{QStringLiteral("source"), QStringLiteral("huggingface")},
                                             {QStringLiteral("url"), QStringLiteral("https://huggingface.co/acme/xl/resolve/main/xl.safetensors")},
                                             {QStringLiteral("sha256"), QString(64, u'a')}})
                       .toJson());
    }

    // RunPod: the catalogue, no capacity for the new pod, and a pod left by an earlier session.
    MockHttpServer server;
    server.route("GET", QStringLiteral("/v2/catalog/gpus"), [](const MockRequest &) {
        auto gpu = [](const char *id, int memory, double secure) {
            return QJsonObject{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("name"), QLatin1String(id)},
                               {QStringLiteral("memory"), memory},
                               {QStringLiteral("price"), QJsonObject{{QStringLiteral("secure"), secure}, {QStringLiteral("community"), 0.0}}},
                               {QStringLiteral("maxCount"), QJsonObject{{QStringLiteral("secure"), 4}, {QStringLiteral("community"), 0}}}};
        };
        return MockResponse::json({{QStringLiteral("gpus"), QJsonArray{gpu("RTX A4000", 16, 0.17), gpu("RTX 4090", 24, 0.69),
                                                                       gpu("A100 80GB", 80, 1.64)}}});
    });
    QJsonObject created;
    server.route("POST", QStringLiteral("/v2/pods"), [&](const MockRequest &req) {
        created = req.json();
        return MockResponse::json({{QStringLiteral("title"), QStringLiteral("Bad Request")},
                                   {QStringLiteral("detail"), QStringLiteral("There are no instances available with the requested specifications.")},
                                   {QStringLiteral("status"), 400}},
                                  400);
    });
    server.route("GET", QStringLiteral("/v2/pods"), [](const MockRequest &) {
        return MockResponse::json({{QStringLiteral("pods"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("old1")},
                                                                                   {QStringLiteral("name"), QStringLiteral("lumen-train-mira-20261010-2200")},
                                                                                   {QStringLiteral("status"), QStringLiteral("RUNNING")},
                                                                                   {QStringLiteral("cost"), 0.69}}}}});
    });
    QStringList deleted;
    server.route("DELETE", QStringLiteral("/v2/pods/old1"), [&](const MockRequest &) {
        deleted << QStringLiteral("old1");
        MockResponse r;
        r.status = 204;
        return r;
    });

    Studio studio;
    studio.runPod()->setBaseUrl(server.baseUrl().toString() + QStringLiteral("/v2"));
    studio.runPod()->keyProvider = [] { return QStringLiteral("rp_test"); };
    QSignalSpy scanned(studio.models(), &runtime::ModelLibrary::scanFinished);
    QVERIFY(studio.models()->addRoot(dir.filePath(QStringLiteral("models"))));
    QTRY_VERIFY_WITH_TIMEOUT(scanned.size() >= 1 && !studio.models()->isScanning(), 20000);
    QCOMPARE(CharacterTraining::checkpointSource(&studio, checkpoint).first,
             QStringLiteral("https://huggingface.co/acme/xl/resolve/main/xl.safetensors"));

    const QJsonObject sheet{{QStringLiteral("name"), QStringLiteral("Mira")}};
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Mira (cloud test)"), sheet, {ref});
    QTRY_VERIFY(saved.isFinished());
    const QString characterId = saved.result().id;
    auto cleanup = qScopeGuard([&] {
        studio.characters()->remove(characterId);
        studio.models()->removeRoot(QDir::cleanPath(dir.filePath(QStringLiteral("models"))));
        resetSettings();
    });
    {
        doc::LoraDataset ds(studio.characters()->datasetDirectory(characterId));
        ds.load();
        for (int i = 0; i < 3; ++i) {
            QImage img(512, 768, QImage::Format_ARGB32);
            img.fill(QColor(40 * i, 200, 40));
            auto added = ds.add(img, {{QStringLiteral("file"), QStringLiteral("x")}});
            QTRY_VERIFY(added.isFinished());
            ds.setCaption(added.result().id, QStringLiteral("lmn_mira, 1girl"), false);
        }
        ds.setSettings({QStringLiteral("lmn_mira"), true});
    }

    LoraDatasetDialog dlg(&studio, characterId, nullptr, {});
    dlg.resize(1080, 720);
    dlg.show();
    dlg.findChild<QTabWidget *>()->setCurrentIndex(2);
    auto *where = dlg.findChild<QComboBox *>(QStringLiteral("trainWhere"));
    auto *gpu = dlg.findChild<QComboBox *>(QStringLiteral("cloudGpu"));
    QVERIFY(where && gpu);
    QVERIFY(!gpu->isVisibleTo(&dlg));
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
    auto labelWith = [&dlg](const QString &text) {
        const QList<QLabel *> labels = dlg.findChildren<QLabel *>();
        for (QLabel *l : labels) {
            if (l->text().contains(text))
                return l;
        }
        return static_cast<QLabel *>(nullptr);
    };

    // In the cloud: GPUs with 24 GB or more, the saved choice, and the price of the training.
    where->setCurrentIndex(1);
    QVERIFY(Settings::instance().value(QStringLiteral("lora/train_cloud")).toBool());
    QTRY_COMPARE(gpu->count(), 2);
    QVERIFY(gpu->isVisibleTo(&dlg));
    QCOMPARE(gpu->currentData().toString(), QStringLiteral("A100 80GB"));
    gpu->setCurrentIndex(gpu->findData(QStringLiteral("RTX 4090")));
    QCOMPARE(Settings::instance().value(QStringLiteral("cloud/runpod/gpu")).toString(), QStringLiteral("RTX 4090"));
    QVERIFY(labelWith(QStringLiteral("at $0.69/h")));
    QTRY_VERIFY(start->isEnabled());

    // Every start asks first, with the cost; "Cancel" rents nothing.
    QString question;
    auto answer = [&question](QMessageBox::StandardButton button) {
        QTimer::singleShot(200, [&question, button] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                question = box->objectName() + u'|' + box->text() + u'|' + box->informativeText();
                box->button(button)->click();
            }
        });
    };
    answer(QMessageBox::Cancel);
    start->click();
    QVERIFY(question.startsWith(QStringLiteral("confirmCloudTraining|Rent a RTX 4090")));
    QVERIFY(question.contains(QStringLiteral("$0.69 per hour")));
    QVERIFY(question.contains(QStringLiteral("from Hugging Face")));
    QTest::qWait(200);
    QVERIFY(created.isEmpty());
    QVERIFY(!CharacterTraining::running(characterId));

    // "Rent and train": the pod Lumen asks for; RunPod's refusal ends the training with its reason.
    answer(QMessageBox::Yes);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!created.isEmpty(), 10000);
    QCOMPARE(created.value(QStringLiteral("gpu")).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("RTX 4090"));
    QCOMPARE(created.value(QStringLiteral("cloud")).toString(), QStringLiteral("SECURE"));
    QVERIFY(created.value(QStringLiteral("name")).toString().startsWith(QLatin1String(runtime::RunPodClient::kTrainingPrefix)));
    QTRY_VERIFY_WITH_TIMEOUT(labelWith(QStringLiteral("no instances available")), 10000);
    QTRY_VERIFY(!CharacterTraining::running(characterId));
    const QString run = CharacterTraining::lastRun(studio.characters()->datasetDirectory(characterId));
    QFile runFile(run + QStringLiteral("/run.json"));
    QVERIFY(runFile.open(QIODevice::ReadOnly));
    const QJsonObject runJson = QJsonDocument::fromJson(runFile.readAll()).object();
    QCOMPARE(runJson.value(QStringLiteral("where")).toString(), QStringLiteral("cloud"));
    QCOMPARE(runJson.value(QStringLiteral("gpu")).toString(), QStringLiteral("RTX 4090"));
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty())
        dlg.grab().save(shots + QStringLiteral("/lora-train-cloud.png"));
    dlg.close();

    // At start-up Lumen offers to shut down its pods nothing uses any more.
    MainWindow window(&studio);
    QString offer;
    QTimer poll;
    poll.setInterval(50);
    connect(&poll, &QTimer::timeout, &poll, [&] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box || box->objectName() != QLatin1String("leftoverPods"))
            return;
        offer = box->text() + u'|' + box->informativeText();
        poll.stop();
        box->button(QMessageBox::Yes)->click();
    });
    poll.start();
    window.checkLeftoverCloudPods();
    QTRY_COMPARE(deleted, QStringList{QStringLiteral("old1")});
    QVERIFY(offer.startsWith(QStringLiteral("1 cloud GPU")));
    QVERIFY(offer.contains(QStringLiteral("$0.69 per hour")));
    QVERIFY(offer.contains(QStringLiteral("lumen-train-mira-20261010-2200")));
}

void TestCanvas::hubCompanionFiles()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    // A Civitai checkpoint whose version carries its text encoder (marked required) and VAE.
    const QByteArray model = fakeSafetensors({QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"),
                                              QStringLiteral("model.diffusion_model.label_emb.0.0.weight")});
    const QByteArray encoder = fakeSafetensors({QStringLiteral("model.layers.0.self_attn.q_proj.weight")}) + "te";
    const QByteArray vae = fakeSafetensors({QStringLiteral("decoder.conv_in.weight"), QStringLiteral("encoder.down.0.block.0.conv1.weight")}) + "vae";
    auto hex = [](const QByteArray &b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); };
    MockHttpServer civ;
    QStringList fetched;
    auto serve = [&](const QString &path, const QByteArray &bytes) {
        civ.route("GET", path, [&fetched, path, bytes](const MockRequest &) {
            fetched << path;
            MockResponse r;
            r.body = bytes;
            return r;
        });
    };
    serve(QStringLiteral("/dl/model"), model);
    serve(QStringLiteral("/dl/te"), encoder);
    serve(QStringLiteral("/dl/vae"), vae);
    auto file = [&](const char *name, const char *type, const QByteArray &bytes, const char *url, bool required) {
        QJsonObject meta{{QStringLiteral("format"), QStringLiteral("SafeTensor")}, {QStringLiteral("fp"), QStringLiteral("bf16")}};
        if (required)
            meta.insert(QStringLiteral("isRequired"), true);
        return QJsonObject{{QStringLiteral("name"), QLatin1String(name)}, {QStringLiteral("type"), QLatin1String(type)},
                           {QStringLiteral("sizeKB"), bytes.size() / 1024.0}, {QStringLiteral("metadata"), meta},
                           {QStringLiteral("primary"), QLatin1String(type) == QLatin1String("Model")},
                           {QStringLiteral("hashes"), QJsonObject{{QStringLiteral("SHA256"), hex(bytes).toUpper()}}},
                           {QStringLiteral("downloadUrl"), civ.baseUrl().toString() + QLatin1String(url)}};
    };
    civ.route("GET", QStringLiteral("/api/v1/models"), [&](const MockRequest &) {
        const QJsonObject version{{QStringLiteral("id"), 30}, {QStringLiteral("name"), QStringLiteral("v1")}, {QStringLiteral("baseModel"), QStringLiteral("Anima")},
                                  {QStringLiteral("files"), QJsonArray{file("harem_txt.safetensors", "Text Encoder", encoder, "/dl/te", true),
                                                                       file("harem.safetensors", "Model", model, "/dl/model", false),
                                                                       file("qwen_image_vae.safetensors", "VAE", vae, "/dl/vae", false)}}};
        const QJsonObject m{{QStringLiteral("id"), 3}, {QStringLiteral("name"), QStringLiteral("Harem")}, {QStringLiteral("type"), QStringLiteral("Checkpoint")},
                            {QStringLiteral("modelVersions"), QJsonArray{version}}};
        return MockResponse::json({{QStringLiteral("items"), QJsonArray{m}}, {QStringLiteral("metadata"), QJsonObject{}}});
    });

    Studio studio;
    studio.hub()->setBaseUrls(civ.baseUrl().toString(), civ.baseUrl().toString());
    studio.hub()->tokenProvider = [](runtime::HubSource) { return QStringLiteral("civ_test"); };
    studio.models()->moveToTrash = [](const QString &path, QString *) { return QFile::remove(path); };
    auto cleanup = qScopeGuard([&] {
        for (const QByteArray &b : {model, encoder, vae}) {
            if (const auto e = studio.models()->find(hex(b)))
                studio.models()->trash(e->path);
        }
    });
    for (const QByteArray &b : {model, encoder, vae})
        QVERIFY2(!studio.models()->find(hex(b)), "left over from an earlier run");

    ModelHubDialog hub(&studio);
    hub.resize(1100, 700);
    hub.show();
    // It opens on image models (language models are only for plugins), also on Hugging Face.
    auto *category = hub.findChild<QComboBox *>(QStringLiteral("hubCategory"));
    QVERIFY(category);
    QCOMPARE(category->currentData().toString(), QStringLiteral("checkpoint"));
    hub.setSource(runtime::HubSource::Civitai);
    hub.setSearchText(QStringLiteral("harem"));
    hub.search();
    QTreeWidget *results = nullptr;
    QTreeWidget *files = nullptr;
    for (QTreeWidget *t : hub.findChildren<QTreeWidget *>())
        (t->headerItem()->text(0) == QLatin1String("Model") ? results : files) = t;
    QVERIFY(results && files);
    QTRY_COMPARE(results->topLevelItemCount(), 1);
    results->setCurrentItem(results->topLevelItem(0));
    QTRY_COMPARE(files->topLevelItemCount(), 3);
    // The model file is preselected (primary); its companions come along, the required one
    // without a choice.
    QCOMPARE(files->currentItem()->text(0), QStringLiteral("harem.safetensors"));
    auto *companions = hub.findChild<QCheckBox *>(QStringLiteral("companions"));
    QVERIFY(companions);
    QVERIFY(companions->isVisibleTo(&hub));
    QVERIFY(companions->isChecked());
    QVERIFY(!companions->isEnabled());
    QVERIFY(companions->text().contains(QStringLiteral("harem_txt.safetensors (Text encoder)")));
    QVERIFY(companions->text().contains(QStringLiteral("qwen_image_vae.safetensors (VAE)")));
    // A companion's own row has none.
    files->setCurrentItem(files->topLevelItem(2));
    QVERIFY(!companions->isVisibleTo(&hub));
    files->setCurrentItem(files->topLevelItem(1));
    QPushButton *download = nullptr;
    for (QPushButton *b : hub.findChildren<QPushButton *>()) {
        if (b->text() == QLatin1String("Download"))
            download = b;
    }
    QVERIFY(download);
    download->click();
    QTRY_VERIFY_WITH_TIMEOUT(studio.models()->find(hex(model)) && studio.models()->find(hex(encoder)) && studio.models()->find(hex(vae)), 20000);
    const QString modelPath = studio.models()->find(hex(model))->path;
    QCOMPARE(runtime::companionsOf(studio.models(), modelPath).size(), 2);
    QCOMPARE(runtime::companionServerOptions(studio.models(), modelPath).value(QStringLiteral("vae")).toString(),
             QDir::toNativeSeparators(studio.models()->find(hex(vae))->path));
    // Everything is there now.
    QTRY_VERIFY(companions->text().startsWith(QStringLiteral("The files it needs are in the library")));
    hub.close();

    // The VAE goes missing: the Models page says so and fetches it again.
    studio.models()->trash(studio.models()->find(hex(vae))->path);
    QVERIFY(!studio.models()->find(hex(vae)));
    ModelsPage page(&studio);
    page.resize(1000, 700);
    page.show();
    auto *list = page.findChild<QTreeWidget *>();
    QVERIFY(list);
    QTreeWidgetItem *row = nullptr;
    QTRY_VERIFY([&] {
        for (QTreeWidgetItemIterator it(list); *it; ++it) {
            if (QDir::cleanPath((*it)->data(0, Qt::UserRole).toString()) == modelPath)
                row = *it;
        }
        return row != nullptr;
    }());
    list->setCurrentItem(row);
    QLabel *needs = nullptr;
    QTRY_VERIFY((needs = page.findChild<QLabel *>(QStringLiteral("companions"))) != nullptr);
    QVERIFY(needs->text().contains(QStringLiteral("missing")));
    QVERIFY(needs->text().contains(QStringLiteral("required")));
    auto *get = page.findChild<QPushButton *>(QStringLiteral("downloadCompanions"));
    QVERIFY(get);
    fetched.clear();
    get->click();
    QTRY_VERIFY_WITH_TIMEOUT(studio.models()->find(hex(vae)).has_value(), 20000);
    QCOMPARE(fetched, QStringList{QStringLiteral("/dl/vae")});
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        list->setCurrentItem(nullptr);
        list->setCurrentItem(row);
        QTest::qWait(300);
        page.grab().save(shots + QStringLiteral("/models-companions.png"));
    }
}

void TestCanvas::activeCast()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    studio.openProject(dir.filePath(QStringLiteral("cast.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    QImage ref(64, 96, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    const QJsonObject loraJson{{QStringLiteral("file"), dir.filePath(QStringLiteral("lmn_aki.safetensors"))},
                               {QStringLiteral("trigger"), QStringLiteral("lmn_aki")},
                               {QStringLiteral("family"), QStringLiteral("SDXL")}};
    auto a = studio.characters()->save(QStringLiteral("Aki (cast test)"), {}, {ref}, QString(), QString(), loraJson);
    auto b = studio.characters()->save(QStringLiteral("Bo (cast test)"), {}, {ref});
    auto c = studio.characters()->save(QStringLiteral("Cy (cast test)"), {}, {ref});
    QTRY_VERIFY(a.isFinished() && b.isFinished() && c.isFinished());
    const QString aki = a.result().id, bo = b.result().id, cy = c.result().id;
    auto cleanup = qScopeGuard([&] {
        Settings::instance().setValue(QStringLiteral("character/use/cast"), true);
        for (const QString &id : {aki, bo, cy})
            studio.characters()->remove(id);
    });

    // Up to two, in order; duplicates, unknown ids and a third are dropped.
    QSignalSpy changed(&studio, &Studio::activeCharacterChanged);
    studio.setActiveCast({aki, aki, QStringLiteral("nope"), bo, cy});
    QCOMPARE(studio.activeCast(), (QStringList{aki, bo}));
    QCOMPARE(studio.activeCharacter(), aki);
    QCOMPARE(changed.size(), 1);
    studio.setActiveCast({aki, bo});
    QCOMPARE(changed.size(), 1); // no change, no signal
    // One character for the pages that draw one (Create page, Re-run, Parts): a cast of one.
    studio.setActiveCharacter(bo);
    QCOMPARE(studio.activeCast(), QStringList{bo});

    // The chip: both names, the first on the left; the menu adds, swaps and removes.
    CharacterChip chip(&studio, QStringLiteral("cast"));
    chip.setUsedHere(true);
    auto menuAction = [&chip](const QString &text) -> QAction * {
        emit chip.menu()->aboutToShow();
        for (QAction *act : chip.menu()->actions()) {
            if (act->text() == text)
                return act;
            if (act->menu()) {
                for (QAction *sub : act->menu()->actions()) {
                    if (act->text() + QStringLiteral(" > ") + sub->text() == text)
                        return sub;
                }
            }
        }
        return nullptr;
    };
    QAction *add = menuAction(QStringLiteral("Add a second character > Aki (cast test) · LoRA"));
    QVERIFY(add);
    QVERIFY(!menuAction(QStringLiteral("Add a second character > Bo (cast test)"))); // already in the cast
    add->trigger();
    QCOMPARE(studio.activeCast(), (QStringList{bo, aki}));
    QCOMPARE(chip.text(), QStringLiteral("Bo (cast test) & Aki (cast test)"));
    QCOMPARE(chip.characterId(), bo);
    QCOMPARE(chip.castIds(), (QStringList{bo, aki}));
    QVERIFY(chip.toolTip().contains(QStringLiteral("Aki (cast test) (LoRA) on the right")));
    // Both members are ticked in the list.
    emit chip.menu()->aboutToShow();
    int ticked = 0;
    for (QAction *act : chip.menu()->actions())
        ticked += act->isChecked() && act->text().contains(QStringLiteral("(cast test)")) ? 1 : 0;
    QCOMPARE(ticked, 2);
    QVERIFY(menuAction(QStringLiteral("Use the characters on this page")));
    menuAction(QStringLiteral("Swap left and right"))->trigger();
    QCOMPARE(studio.activeCast(), (QStringList{aki, bo}));
    QVERIFY(menuAction(QStringLiteral("Replace the second character > Cy (cast test)")));
    menuAction(QStringLiteral("Replace the second character > Cy (cast test)"))->trigger();
    QCOMPARE(studio.activeCast(), (QStringList{aki, cy}));
    menuAction(QStringLiteral("Remove Aki (cast test)"))->trigger();
    QCOMPARE(studio.activeCast(), QStringList{cy});
    QCOMPARE(chip.text(), QStringLiteral("Cy (cast test)"));
    // Switched off on this page: no cast here, the project keeps it.
    studio.setActiveCast({aki, bo});
    chip.setUsedHere(false);
    QVERIFY(chip.castIds().isEmpty());
    QCOMPARE(studio.activeCast().size(), 2);
    chip.setUsedHere(true);

    // Stored in the project: reopening it restores both, in order.
    QTest::qWait(200);
    studio.openProject(dir.filePath(QStringLiteral("other.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("other.lumen")), 5000);
    QTRY_VERIFY(studio.activeCast().isEmpty());
    studio.openProject(dir.filePath(QStringLiteral("cast.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("cast.lumen")), 5000);
    QTRY_COMPARE(studio.activeCast(), (QStringList{aki, bo}));

    // A project from before M16 has an "active_character" record: read as a cast of one.
    studio.openProject(dir.filePath(QStringLiteral("old.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("old.lumen")), 5000);
    {
        doc::Record record;
        record.id = QStringLiteral("active_character");
        record.type = QStringLiteral("active_character");
        record.json = QJsonObject{{QStringLiteral("character"), bo}};
        auto saved = studio.project()->saveRecord(record);
        QTRY_VERIFY(saved.isFinished());
    }
    studio.openProject(dir.filePath(QStringLiteral("other.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("other.lumen")), 5000);
    studio.openProject(dir.filePath(QStringLiteral("old.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen() && studio.project()->path().endsWith(QStringLiteral("old.lumen")), 5000);
    QTRY_COMPARE(studio.activeCast(), QStringList{bo});
    // Saving the cast replaces the old record.
    studio.setActiveCast({bo, aki});
    auto old = studio.project()->records(QStringLiteral("active_character"));
    QTRY_VERIFY(old.isFinished());
    QVERIFY(old.result().isEmpty());

    // A character removed from the library leaves the cast.
    studio.characters()->remove(aki);
    QTRY_COMPARE(studio.activeCast(), QStringList{bo});

    // The assistant: the cast in the state, set with two ids, at most two.
    studio.setActiveCast({bo, cy});
    const ai::ToolSet tools = assistantTools(&studio);
    auto state = tools.run(QStringLiteral("get_state"), {});
    QTRY_VERIFY(state.isFinished());
    const QJsonArray cast = state.result().data.value(QStringLiteral("active_cast")).toArray();
    QCOMPARE(cast.size(), 2);
    QCOMPARE(cast[1].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Cy (cast test)"));
    auto set = tools.run(QStringLiteral("set_active_character"), {{QStringLiteral("character_ids"), QJsonArray{cy, bo}}});
    QTRY_VERIFY(set.isFinished());
    QVERIFY(set.result().error.isEmpty());
    QCOMPARE(studio.activeCast(), (QStringList{cy, bo}));
    auto tooMany = tools.run(QStringLiteral("set_active_character"), {{QStringLiteral("character_ids"), QJsonArray{cy, bo, QStringLiteral("x")}}});
    QTRY_VERIFY(tooMany.isFinished());
    QVERIFY(tooMany.result().error.contains(QStringLiteral("at most 2")));
    auto one = tools.run(QStringLiteral("set_active_character"), {{QStringLiteral("character_id"), bo}});
    QTRY_VERIFY(one.isFinished());
    QCOMPARE(studio.activeCast(), QStringList{bo});
    auto listed = tools.run(QStringLiteral("list_characters"), {});
    QTRY_VERIFY(listed.isFinished());
    QCOMPARE(listed.result().data.value(QStringLiteral("cast")).toArray(), QJsonArray{bo});
    const QString shots = qEnvironmentVariable("LUMEN_SCREENSHOT_DIR");
    if (!shots.isEmpty()) {
        studio.setActiveCast({bo, cy});
        chip.resize(chip.sizeHint());
        chip.grab().save(shots + QStringLiteral("/cast-chip.png"));
    }
}

namespace {

QAbstractButton *buttonNamed(QWidget *root, const QString &name)
{
    for (QAbstractButton *b : root->findChildren<QAbstractButton *>()) {
        if (b->accessibleName() == name || b->text() == name)
            return b;
    }
    return nullptr;
}

} // namespace

// The start screen while no document is open: the ways to start, the project's recent documents and
// images (a click opens them), "Set up AI" while no image engine is configured, and an image file
// dropped on it opens.
void TestCanvas::startScreen()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    studio.openProject(dir.filePath(QStringLiteral("start.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    // A saved document and an image in the project.
    doc::Document *saved = studio.newDocument(QSize(64, 48), QStringLiteral("Saved"));
    auto done = studio.project()->saveDocument(saved);
    QTRY_VERIFY_WITH_TIMEOUT(done.isFinished(), 5000);
    const QString savedId = saved->id();
    studio.closeDocument(saved);
    QImage red(40, 30, QImage::Format_ARGB32);
    red.fill(kRed);
    auto added = studio.project()->addAsset(red, doc::AssetKind::Import, QStringLiteral("Red square"));
    QTRY_VERIFY_WITH_TIMEOUT(added.isFinished(), 5000);
    const QString redId = added.result().id;

    MainWindow window(&studio);
    window.resize(1280, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    EditorPage *editor = window.editor();
    QCOMPARE(window.centralWidget()->findChild<EditorPage *>(), editor);
    QTRY_VERIFY(editor->showsStartScreen());
    StartScreen *start = editor->startScreen();
    QVERIFY(start->isVisible());

    // "Set up AI" while nothing can draw or change images.
    auto *card = start->findChild<QFrame *>(QStringLiteral("setUpAiCard"));
    QVERIFY(card && card->isVisible());
    ai::Provider *sd = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("SD"), {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")}});
    QVERIFY(sd);
    QVERIFY(!card->isVisible());
    studio.providers()->removeProvider(sd->instanceId());
    QVERIFY(card->isVisible());

    // The documents (newest first), then the images; a click opens one.
    auto tile = [start](const char *property, const QString &id) -> QToolButton * {
        for (QToolButton *t : start->recentTiles()) {
            if (t->property(property).toString() == id)
                return t;
        }
        return nullptr;
    };
    QTRY_COMPARE(start->recentTiles().size(), 2);
    QCOMPARE(start->recentTiles().first(), tile("documentId", savedId));
    QToolButton *redTile = tile("assetId", redId);
    QVERIFY(redTile);
    QCOMPARE(redTile->accessibleName(), QStringLiteral("Red square"));
    redTile->click();
    QTRY_VERIFY(!editor->showsStartScreen());
    QCOMPARE(studio.activeDocument()->size(), QSize(40, 30));
    // Closing the last document brings the start screen back.
    studio.closeDocument(studio.activeDocument());
    QVERIFY(editor->showsStartScreen());
    QTRY_VERIFY(tile("documentId", savedId));
    tile("documentId", savedId)->click();
    QTRY_VERIFY(!editor->showsStartScreen());
    QCOMPARE(studio.activeDocument()->id(), savedId);
    studio.closeDocument(studio.activeDocument());

    // Paste from clipboard: the copied picture as a new document.
    QImage blue(20, 10, QImage::Format_ARGB32);
    blue.fill(kBlue);
    QApplication::clipboard()->setImage(blue);
    QAbstractButton *paste = buttonNamed(start, QStringLiteral("Paste from clipboard"));
    QVERIFY(paste);
    paste->click();
    QVERIFY(!editor->showsStartScreen());
    QCOMPARE(studio.activeDocument()->size(), QSize(20, 10));
    QCOMPARE(studio.activeDocument()->name(), QStringLiteral("Pasted image"));
    studio.closeDocument(studio.activeDocument());

    // An image file dropped on it opens.
    const QString file = dir.filePath(QStringLiteral("dropped.png"));
    QVERIFY(red.scaled(30, 20).save(file));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(file)});
    QDragEnterEvent enter(QPoint(200, 200), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(200, 200), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor, &drop);
    QTRY_VERIFY(!editor->showsStartScreen());
    QCOMPARE(studio.activeDocument()->name(), QStringLiteral("dropped"));
    QCOMPARE(studio.activeDocument()->size(), QSize(30, 20));
    studio.closeDocument(studio.activeDocument());

    // New from prompt… and Browse project images… open their windows.
    QVERIFY(editor->showsStartScreen());
    QAbstractButton *fromPrompt = buttonNamed(start, QStringLiteral("New from prompt…"));
    QAbstractButton *browse = buttonNamed(start, QStringLiteral("Browse project images…"));
    QVERIFY(fromPrompt && browse);
    fromPrompt->click();
    QPointer<NewFromPromptDialog> dialog = window.newFromPromptDialog();
    QVERIFY(dialog && dialog->isVisible());
    dialog->close();
    QTRY_VERIFY(!dialog);
    browse->click();
    QDialog *images = window.projectImagesDialog();
    QVERIFY(images && images->isVisible() && images->isWindow());
    QVERIFY(images->findChild<LibraryPage *>());
}

// The status bar: the project, the image engine the prompt bar uses in plain words (a click opens
// Settings → AI; without an engine the setup wizard, tst_setup entryPoints), and the jobs, whose
// button opens the Queue popup as View → Job queue does.
void TestCanvas::shellStatusBar()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    auto cleanup = qScopeGuard([&] {
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    studio.openProject(dir.filePath(QStringLiteral("Status test.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    MainWindow window(&studio);
    window.resize(1280, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *project = window.statusBar()->findChild<ElidedLabel *>();
    QVERIFY(project);
    QCOMPARE(project->fullText(), QStringLiteral("Project: Status test"));
    QVERIFY(project->toolTip().endsWith(QStringLiteral("Status test.lumen")));

    auto *engine = window.findChild<QToolButton *>(QStringLiteral("imageEngine"));
    QVERIFY(engine && engine->isVisible());
    QCOMPARE(engine->text(), QStringLiteral("No image engine – Set up AI"));
    // A provider by its name.
    const QVariantMap url{{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")}};
    ai::Provider *workstation = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Workstation"), url);
    QVERIFY(workstation);
    QCOMPARE(engine->text(), QStringLiteral("Workstation"));
    // A server Lumen runs, with a model it has a preset for: the model, and where it runs.
    ai::ModelDefaults klein;
    klein.id = QStringLiteral("flux2-klein-4b");
    klein.name = QStringLiteral("FLUX.2 [klein] 4B");
    QVariantMap managed = url;
    managed.insert(QStringLiteral("managed_server"), QStringLiteral("not-running"));
    managed.insert(QStringLiteral("model_preset"), klein.toVariant());
    ai::Provider *local = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Local"), managed);
    QVERIFY(local);
    QCOMPARE(engine->text(), QStringLiteral("Workstation")); // the default one
    studio.providers()->setDefaultImageProvider(local->instanceId());
    QCOMPARE(engine->text(), QStringLiteral("FLUX.2 [klein] 4B on this computer"));
    // Switched off: the next one that is on.
    studio.providers()->setEnabled(local->instanceId(), false);
    QCOMPARE(engine->text(), QStringLiteral("Workstation"));
    studio.providers()->setEnabled(local->instanceId(), true);
    // A click opens Settings → AI, where the engines for new images and for changes are chosen.
    QString tab;
    QTimer::singleShot(200, &window, [&tab] {
        if (auto *settings = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget())) {
            auto *tabs = settings->findChild<QTabWidget *>();
            tab = tabs->tabText(tabs->currentIndex()).remove(u'&');
            settings->reject();
        }
    });
    engine->click();
    QCOMPARE(tab, QStringLiteral("AI"));

    // The jobs: a summary, and the Queue above the button.
    auto *jobs = window.findChild<QToolButton *>(QStringLiteral("jobs"));
    QAction *queue = window.findChild<QAction *>(QStringLiteral("jobQueue"));
    QVERIFY(jobs && queue);
    QCOMPARE(jobs->text(), QStringLiteral("No jobs running"));
    // An empty Queue says so, and its buttons have nothing to act on.
    jobs->click();
    QVERIFY(window.queuePanel() && window.queuePanel()->isVisible());
    auto *empty = window.queuePanel()->findChild<QLabel *>(QStringLiteral("queueEmpty"));
    QVERIFY(empty && empty->isVisible());
    QVERIFY(!buttonNamed(window.queuePanel(), QStringLiteral("Cancel all"))->isEnabled());
    QVERIFY(!buttonNamed(window.queuePanel(), QStringLiteral("Clear finished"))->isEnabled());
    QVERIFY(window.queuePanel()->height() < 300); // as tall as what it shows
    jobs->click();
    QPointer<Job> job = studio.jobs()->submit(QStringLiteral("Waiting job"), QStringLiteral("status-test"), [](Job *) {});
    QTRY_COMPARE(jobs->text(), QStringLiteral("1 running · 0 queued"));
    jobs->click();
    QWidget *panel = window.queuePanel();
    QVERIFY(panel && panel->isVisible());
    QVERIFY(queue->isChecked() && jobs->isChecked());
    QVERIFY(window.centralWidget()->rect().contains(panel->geometry()));
    QVERIFY(panel->geometry().right() > window.centralWidget()->width() / 2); // on the side of the button
    QCOMPARE(panel->findChildren<JobCard *>().size(), 1);
    QVERIFY(!panel->findChild<QLabel *>(QStringLiteral("queueEmpty"))->isVisible());
    QVERIFY(buttonNamed(panel, QStringLiteral("Cancel all"))->isEnabled());
    queue->trigger(); // View → Job queue (Ctrl+Shift+Q) closes it again
    QVERIFY(!panel->isVisible() && !jobs->isChecked());
    queue->trigger();
    QVERIFY(panel->isVisible() && jobs->isChecked());
    job->succeed();
    QTRY_COMPARE(jobs->text(), QStringLiteral("No jobs running"));
}

// Plugin pages open in windows of their own from the Tools menu: built on first use, raised
// afterwards, their place remembered. Studio::showPage reaches each of them and what replaced the
// Generate and Library pages; plugin panels are switched from the View menu.
void TestCanvas::pluginWindows()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    PluginsOn plugins;
    auto cleanup = qScopeGuard([] { Settings::instance().remove(QStringLiteral("ui/windows")); });
    Studio studio;
    QPoint place;
    {
        MainWindow window(&studio);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.pageIds(), (QStringList{QStringLiteral("editor"), QStringLiteral("create"), QStringLiteral("poses"), QStringLiteral("parts")}));
        // Tools: the filter editor, one action per page, and the way to the plugins.
        QMenu *tools = nullptr;
        for (QAction *a : window.menuBar()->actions()) {
            if (a->text() == QLatin1String("&Tools"))
                tools = a->menu();
        }
        QVERIFY(tools);
        QStringList names;
        for (QAction *a : tools->actions()) {
            if (!a->isSeparator())
                names << a->text();
        }
        QCOMPARE(names, (QStringList{QStringLiteral("New filter…"), QStringLiteral("Filter editor…"), QStringLiteral("Create"),
                                     QStringLiteral("Poses"), QStringLiteral("Parts"), QStringLiteral("Plugins…")}));
        QCOMPARE(Shortcuts::instance().keys(QStringLiteral("nav.person")), QList<QKeySequence>{QKeySequence(QStringLiteral("Alt+4"))});

        QVERIFY(!window.pageWindow(QStringLiteral("poses")) && !window.pageWidget(QStringLiteral("poses")));
        window.findChild<QAction *>(QStringLiteral("tool-poses"))->trigger();
        QWidget *poses = window.pageWindow(QStringLiteral("poses"));
        QVERIFY(poses && poses->isWindow() && poses->isVisible());
        QVERIFY(QTest::qWaitForWindowExposed(poses));
        QCOMPARE(poses->windowTitle(), QStringLiteral("Poses — Lumen Studio"));
        QVERIFY(window.pageWidget(QStringLiteral("poses")) && window.pageWidget(QStringLiteral("poses"))->window() == poses);
        QVERIFY(!window.pageWindow(QStringLiteral("parts"))); // only what was opened
        // Closed and opened again: the same window.
        poses->move(30, 40);
        place = poses->pos();
        poses->close();
        QVERIFY(!poses->isVisible());
        studio.showPage(QStringLiteral("poses"));
        QCOMPARE(window.pageWindow(QStringLiteral("poses")), poses);
        QVERIFY(poses->isVisible());
        QCOMPARE(poses->pos(), place);
        poses->close();
        QVERIFY(Settings::instance().contains(QStringLiteral("ui/windows/poses/geometry")));
    }
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // A pose to edit (the editor's Detect pose) opens the Poses window, built for it, where it was.
    QVERIFY(!window.pageWindow(QStringLiteral("poses")));
    studio.editPose(pose::findBuiltInPreset(QStringLiteral("standing"))->toPose(QSize(512, 768)));
    QWidget *poses = window.pageWindow(QStringLiteral("poses"));
    QVERIFY(poses && poses->isVisible());
    // (The test screen is narrower than the page, so the restored window is moved left to fit.)
    QCOMPARE(poses->y(), place.y());
    // "editor" brings the main window back; "generate" and "library" open what replaced them.
    studio.showPage(pages::Editor);
    studio.showPage(pages::Generate);
    QPointer<NewFromPromptDialog> dialog = window.newFromPromptDialog();
    QVERIFY(dialog && dialog->isVisible() && dialog->parentWidget() == &window);
    dialog->close();
    QTRY_VERIFY(!dialog);
    studio.showPage(pages::Library);
    QVERIFY(window.projectImagesDialog() && window.projectImagesDialog()->isVisible());
    QVERIFY(!window.projectImagesDialog()->parentWidget()); // a window of its own: the editor can come in front

    // View → Assistant shows the panel over the editor; its close button unchecks the action.
    QAction *assistant = window.findChild<QAction *>(QStringLiteral("panel-assistant"));
    QVERIFY(assistant && assistant->isCheckable());
    QCOMPARE(assistant->text(), QStringLiteral("Assistant"));
    assistant->setChecked(true);
    auto *panel = window.findChild<AssistantPanel *>();
    QVERIFY(panel && panel->isVisible());
    QCOMPARE(panel->parentWidget(), window.centralWidget());
    // With a document open it ends above the prompt bar and the results, which it would hide.
    doc::Document *doc = studio.openImageAsDocument(content(), QStringLiteral("Under the assistant"));
    window.editor()->variantStrip()->addVariants(doc, {content(), content()}, QStringLiteral("Two"));
    QTRY_VERIFY(window.editor()->variantStrip()->isVisible());
    const int strip = window.editor()->variantStrip()->mapTo(window.centralWidget(), QPoint(0, 0)).y();
    QTRY_VERIFY2(panel->geometry().bottom() < strip, qPrintable(QStringLiteral("%1 >= %2").arg(panel->geometry().bottom()).arg(strip)));
    window.editor()->variantStrip()->discard();
    const int bar = window.editor()->promptBar()->mapTo(window.centralWidget(), QPoint(0, 0)).y();
    QTRY_VERIFY(panel->geometry().bottom() < bar && panel->geometry().bottom() > bar - 40);
    studio.closeDocument(doc);
    QTRY_VERIFY(panel->geometry().bottom() > window.centralWidget()->height() - 40); // the start screen: the whole height
    emit panel->closeRequested();
    QVERIFY(!assistant->isChecked());
    QVERIFY(!panel->isVisible());

    // Quitting closes the windows of their own with the main window.
    window.close();
    QTRY_VERIFY(!window.isVisible());
    QVERIFY(!poses->isVisible());
    QVERIFY(!window.projectImagesDialog()->isVisible());
}

// The core starts, edits and keeps plugin data with every plugin off (plan M17 §17.7); they are off
// by default.
void TestCanvas::pluginsOff()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    ext::Extensions &registry = ext::Extensions::instance();
    QCOMPARE(registry.all().size(), 3);
    // Off by default.
    for (const ext::Extension *e : registry.all()) {
        Settings::instance().remove(QStringLiteral("plugins/%1/enabled").arg(e->id()));
        QVERIFY2(!registry.isEnabled(e->id()), qPrintable(e->id()));
    }
    registry.refresh();
    QVERIFY(registry.active().isEmpty());
    auto restore = qScopeGuard([&registry] {
        for (const ext::Extension *e : registry.all())
            Settings::instance().remove(QStringLiteral("plugins/%1/enabled").arg(e->id()));
        registry.refresh();
    });

    // A record of the Characters plugin, written while it was on.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("plugins.lumen"));
    {
        Studio studio;
        studio.openProject(path);
        QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
        doc::Record r;
        r.type = QStringLiteral("director");
        r.name = QStringLiteral("Akane shots");
        r.json = QJsonObject{{QStringLiteral("title"), QStringLiteral("Akane")}};
        auto saved = studio.project()->saveRecord(r);
        QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 5000);
    }

    Studio studio;
    QVERIFY(studio.hasPage(pages::Editor));
    for (const QString &page : {QStringLiteral("create"), QStringLiteral("poses"), QStringLiteral("parts")})
        QVERIFY2(!studio.hasPage(page), qPrintable(page));
    MainWindow window(&studio);
    QCOMPARE(window.pageIds(), QStringList{QStringLiteral("editor")});
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // No tools but the way to the plugins, no panels in the View menu.
    QVERIFY(!window.findChild<QAction *>(QStringLiteral("panel-assistant")));
    for (QAction *a : window.findChildren<QAction *>())
        QVERIFY2(!a->objectName().startsWith(QLatin1String("tool-")), qPrintable(a->objectName()));
    QVERIFY(!window.findChild<AssistantPanel *>());
    // Characters and poses are the plugins': no character in the AI panel, no Detect pose in it or in
    // the AI menu, no Poses or Parts in Browse project images, no Live2D sheet for a new canvas.
    QVERIFY(!window.findChild<QAction *>(QStringLiteral("ai-detect-pose")));
    QVERIFY(!window.editor()->aiPanel()->findChild<CharacterChip *>()->isVisibleTo(window.editor()->aiPanel()));
    for (QToolButton *b : window.editor()->aiPanel()->findChildren<QToolButton *>())
        QVERIFY2(b->accessibleName() != QLatin1String("Detect pose") || !b->isVisibleTo(window.editor()->aiPanel()), "Detect pose");
    {
        LibraryPage library(&studio);
        for (QToolButton *b : library.findChildren<QToolButton *>())
            QVERIFY2(b->text() != QLatin1String("Poses") && b->text() != QLatin1String("Parts"), qPrintable(b->text()));
        NewCanvasDialog canvas(&studio);
        for (QComboBox *c : canvas.findChildren<QComboBox *>())
            QVERIFY(c->findText(QStringLiteral("Live2D sheet 2048 × 3072")) < 0);
    }
    {
        PluginsOn on;
        Studio withPlugins;
        LibraryPage library(&withPlugins);
        QStringList chips;
        for (QToolButton *b : library.findChildren<QToolButton *>())
            chips << b->text();
        QVERIFY(chips.contains(QStringLiteral("Poses")) && chips.contains(QStringLiteral("Parts")));
        NewCanvasDialog canvas(&withPlugins);
        QVERIFY(canvas.findChild<QComboBox *>()->findText(QStringLiteral("Live2D sheet 2048 × 3072")) >= 0);
    }
    // A plugin page that is off opens nothing.
    studio.showPage(QStringLiteral("poses"));
    QVERIFY(!window.pageWindow(QStringLiteral("poses")));

    // Editing works.
    studio.openProject(path);
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    doc::Document *d = studio.newDocument(QSize(64, 48), QStringLiteral("Plain"));
    QVERIFY(!window.editor()->showsStartScreen());
    QCOMPARE(studio.activeDocument(), d);
    d->addLayer(QStringLiteral("Paint"));
    QCOMPARE(d->layerCount(), 2);

    // Settings: no Cloud training tab; the Plugins tab switches a plugin on for the next start.
    SettingsDialog settings(&studio);
    auto *tabs = settings.findChild<QTabWidget *>();
    QStringList names;
    for (int i = 0; i < tabs->count(); ++i)
        names << tabs->tabText(i);
    QVERIFY(!names.contains(QStringLiteral("Cloud training")));
    QCOMPARE(names.last(), QStringLiteral("Plugins"));
    settings.showTab(QStringLiteral("plugins"));
    QCOMPARE(tabs->currentIndex(), tabs->count() - 1);
    settings.showTab(QStringLiteral("cloud")); // a tab of a plugin that is off: stays where it is
    QCOMPARE(tabs->currentIndex(), tabs->count() - 1);
    auto *poses = settings.findChild<QCheckBox *>(QStringLiteral("plugin-poses"));
    QVERIFY(poses);
    QVERIFY(!poses->isChecked());
    poses->setChecked(true);
    QVERIFY(registry.isEnabled(QStringLiteral("poses")));
    QVERIFY(!registry.isActive(QStringLiteral("poses")));
    QVERIFY(!studio.hasPage(QStringLiteral("poses")));

    // The plugin's record is still there after a session without the plugin, and after reopening.
    auto saved = studio.saveSession();
    QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 5000);
    Studio again;
    again.openProject(path);
    QTRY_VERIFY_WITH_TIMEOUT(again.isProjectOpen(), 5000);
    auto records = again.project()->records(QStringLiteral("director"));
    QTRY_VERIFY_WITH_TIMEOUT(records.isFinished(), 5000);
    QCOMPARE(records.result().size(), 1);
    QCOMPARE(records.result().first().json.value(QStringLiteral("title")).toString(), QStringLiteral("Akane"));
}

// Before the editor became the main window, the page the rail showed last was remembered ("ui/page",
// before plugins an index into a fixed list: 3 was Poses). Now the main window always shows the
// editor; the setting goes, and no plugin window opens by itself.
void TestCanvas::legacyPageSetting()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    PluginsOn plugins;
    for (const QVariant &old : {QVariant(3), QVariant(QStringLiteral("poses"))}) {
        Settings::instance().setValue(QStringLiteral("ui/page"), old);
        Studio studio;
        MainWindow window(&studio);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.editor()->isVisible());
        QVERIFY(!window.pageWindow(QStringLiteral("poses")));
        QVERIFY(!Settings::instance().contains(QStringLiteral("ui/page")));
    }
}

namespace {

// A safetensors header with tensor shapes, padded past the library's 1 MB minimum.
QByteArray shapedSafetensors(const QList<QPair<QString, QList<int>>> &tensors)
{
    QJsonObject header;
    for (const auto &[name, shape] : tensors) {
        QJsonArray dims;
        for (int d : shape)
            dims.append(d);
        header.insert(name, QJsonObject{{QStringLiteral("dtype"), QStringLiteral("BF16")}, {QStringLiteral("shape"), dims},
                                        {QStringLiteral("data_offsets"), QJsonArray{0, 0}}});
    }
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    QByteArray out;
    for (int i = 0; i < 8; ++i)
        out.append(char((quint64(json.size()) >> (8 * i)) & 0xFF));
    return out + json + QByteArray(2 * 1024 * 1024, '\0');
}

// The headers of the model files the presets know, as the library reads them.
using Tensors = QList<QPair<QString, QList<int>>>;
Tensors kleinTensors(int textWidth)
{
    return {{QStringLiteral("img_in.weight"), {3072, 128}},
            {QStringLiteral("txt_in.weight"), {3072, textWidth}},
            {QStringLiteral("double_stream_modulation_img.lin.weight"), {18432, 3072}},
            {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {9216, 3072}}};
}
Tensors flux1DevTensors()
{
    return {{QStringLiteral("img_in.weight"), {3072, 64}},
            {QStringLiteral("txt_in.weight"), {3072, 4096}},
            {QStringLiteral("double_blocks.0.img_attn.qkv.weight"), {9216, 3072}},
            {QStringLiteral("guidance_in.in_layer.weight"), {3072, 256}}};
}
Tensors vaeTensors(int channels)
{
    return {{QStringLiteral("encoder.down_blocks.0.resnets.0.conv1.weight"), {128, 128, 3, 3}},
            {QStringLiteral("decoder.conv_in.weight"), {512, channels, 3, 3}},
            {QStringLiteral("decoder.up_blocks.0.resnets.0.conv1.weight"), {512, 512, 3, 3}}};
}
Tensors qwen3Tensors(int width)
{
    return {{QStringLiteral("model.embed_tokens.weight"), {151936, width}}, {QStringLiteral("model.layers.0.self_attn.q_norm.weight"), {128}}};
}
Tensors clipTensors(int width)
{
    return {{QStringLiteral("text_model.embeddings.token_embedding.weight"), {49408, width}},
            {QStringLiteral("text_model.encoder.layers.0.mlp.fc1.weight"), {width * 4, width}}};
}
Tensors t5Tensors()
{
    return {{QStringLiteral("encoder.block.0.layer.0.SelfAttention.q.weight"), {4096, 4096}}, {QStringLiteral("shared.weight"), {32128, 4096}}};
}
Tensors sdxlTensors()
{
    return {{QStringLiteral("model.diffusion_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
            {QStringLiteral("model.diffusion_model.label_emb.0.0.weight"), {1280, 2816}},
            {QStringLiteral("conditioner.embedders.1.model.ln_final.weight"), {1280}}};
}
Tensors sdxlControlNetTensors()
{
    return {{QStringLiteral("control_model.input_blocks.0.0.weight"), {320, 4, 3, 3}},
            {QStringLiteral("control_model.label_emb.0.0.weight"), {1280, 2816}},
            {QStringLiteral("control_model.input_hint_block.0.weight"), {16, 3, 3, 3}}};
}

// The server dialog as a test drives it: a field by its option, its status line, typing a path.
struct ServerDialogDriver
{
    ServerProfileDialog *dlg;

    QLineEdit *field(const QString &key) const
    {
        for (QLineEdit *e : dlg->findChildren<QLineEdit *>()) {
            if (e->property("optionKey").toString() == key)
                return e;
        }
        return nullptr;
    }
    QString status(const QString &key) const
    {
        auto *l = dlg->findChild<QLabel *>(QStringLiteral("status-%1").arg(key));
        return l && l->isVisible() ? QTextDocumentFragment::fromHtml(l->text()).toPlainText().replace(QChar(0xA0), u' ') : QString();
    }
    // The file fields shown, in their order.
    QStringList shown() const
    {
        QStringList out;
        for (const QString &key : runtime::sdFileOptions()) {
            if (field(key) && field(key)->isVisible())
                out << key;
        }
        return out;
    }
    // A path typed or pasted in, then the field left.
    void type(const QString &key, const QString &path) const
    {
        QLineEdit *e = field(key);
        e->setText(QDir::toNativeSeparators(path));
        e->setModified(true);
        emit e->editingFinished();
    }
    void chooseType(const QString &id) const
    {
        auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("serverModelType"));
        const int index = combo->findData(id);
        combo->setCurrentIndex(index);
        emit combo->activated(index);
    }
    QComboBox *typeCombo() const { return dlg->findChild<QComboBox *>(QStringLiteral("serverModelType")); }
    QPushButton *ok() const { return dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok); }
    QLabel *problems() const { return dlg->findChild<QLabel *>(QStringLiteral("problems")); }
    // The library menu of a field, as it opens.
    QList<QAction *> library(const QString &key) const
    {
        auto *menu = dlg->findChild<QMenu *>(QStringLiteral("library-%1").arg(key));
        emit menu->aboutToShow();
        return menu->actions();
    }
};

} // namespace

// The server dialog of plan §17.5 with FLUX.2 [klein] 4B, from the checkpoints folder as in the
// session of plan §17.1: a new server starts on its type; the file gets its VAE and text encoder, each
// with a status line; the launch settings and the command line are under a folded Advanced; the
// saved server's provider gets the preset; a text encoder that does not fit keeps OK off; a missing
// one shows with its size and a download.
void TestCanvas::presetServerDialog()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir comfy;
    auto put = [&](const QString &rel, const Tensors &tensors) {
        const QString path = QDir::cleanPath(comfy.filePath(rel));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(shapedSafetensors(tensors));
        return path;
    };
    const QString klein = put(QStringLiteral("checkpoints/flux-2-klein-4b-fp8.safetensors"), kleinTensors(7680));
    const QString vae = put(QStringLiteral("vae/flux2-vae.safetensors"), vaeTensors(32));
    const QString qwen = put(QStringLiteral("text_encoders/qwen_3_4b.safetensors"), qwen3Tensors(2560));
    const QString t5 = put(QStringLiteral("text_encoders/t5xxl_fp16.safetensors"), t5Tensors());

    Studio studio;
    QVERIFY(studio.models()->addRoot(comfy.path()));
    QTRY_VERIFY_WITH_TIMEOUT(studio.models()->entry(klein) && studio.models()->entry(qwen) && studio.models()->entry(t5) && !studio.models()->isScanning(),
                             20000);
    auto removeRoot = qScopeGuard([&] { studio.models()->removeRoot(comfy.path()); });
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/test-sdcpp");
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = comfy.filePath(QStringLiteral("sd-server.exe"));
    studio.runtimes()->addInstalled(rt);
    auto removeRuntime = qScopeGuard([&] { studio.runtimes()->remove(rt.id); });
    const runtime::ServerProfile profile = studio.servers()->createProfile(rt.id, QStringLiteral("Images"));

    ServerProfileDialog dlg(&studio, profile, true);
    dlg.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dlg));
    const ServerDialogDriver d{&dlg};
    QPushButton *ok = d.ok();
    QLabel *problems = d.problems();
    QVERIFY(ok && problems);

    // A new server without a model starts on the type of the library's model, FLUX.2 [klein] 4B, with
    // only that type's fields (not every field of Other); its model field asks for the file, there
    // rather than in a line below the form. The memory and the port; the launch settings and the
    // command line are under Advanced, folded.
    auto *memory = dlg.findChild<QComboBox *>(QStringLiteral("serverMemory"));
    auto *advanced = dlg.findChild<QToolButton *>(QStringLiteral("serverAdvanced"));
    auto *advancedOptions = dlg.findChild<QWidget *>(QStringLiteral("serverAdvancedOptions"));
    QVERIFY(memory && advanced && advancedOptions && d.typeCombo());
    QCOMPARE(dlg.modelType(), QStringLiteral("flux2-klein-4b"));
    QCOMPARE(d.shown(), (QStringList{QStringLiteral("diffusion_model"), QStringLiteral("vae"), QStringLiteral("llm"), QStringLiteral("lora_dir"),
                                     QStringLiteral("upscalers_dir")}));
    QVERIFY2(d.status(QStringLiteral("diffusion_model")).startsWith(QStringLiteral("• Choose the diffusion model file")),
             qPrintable(d.status(QStringLiteral("diffusion_model"))));
    QVERIFY(!problems->isVisible());
    QVERIFY(memory->isVisible() && advanced->isVisible());
    QVERIFY(!advanced->isChecked() && !advancedOptions->isVisible());
    QVERIFY(!dlg.findChild<QPlainTextEdit *>()->isVisible()); // the command line

    // The klein file (in the checkpoints folder, as in the session of plan §17.1) typed in: its type,
    // its slot, its VAE and its text encoder, found.
    d.type(QStringLiteral("diffusion_model"), klein);
    QCOMPARE(dlg.modelType(), QStringLiteral("flux2-klein-4b"));
    QCOMPARE(d.typeCombo()->currentText(), QStringLiteral("FLUX.2 [klein] 4B"));
    runtime::ServerProfile chosen = dlg.profile();
    QCOMPARE(chosen.options.value(QStringLiteral("diffusion_model")).toString(), QDir::toNativeSeparators(klein));
    QVERIFY(!chosen.options.contains(QStringLiteral("model")));
    QCOMPARE(chosen.options.value(QStringLiteral("vae")).toString(), QDir::toNativeSeparators(vae));
    QCOMPARE(chosen.options.value(QStringLiteral("llm")).toString(), QDir::toNativeSeparators(qwen));
    QCOMPARE(d.shown(), (QStringList{QStringLiteral("diffusion_model"), QStringLiteral("vae"), QStringLiteral("llm"), QStringLiteral("lora_dir"),
                                     QStringLiteral("upscalers_dir")}));
    QCOMPARE(d.field(QStringLiteral("llm"))->accessibleName(), QStringLiteral("Text encoder (Qwen3 4B)"));
    QCOMPARE(d.field(QStringLiteral("vae"))->accessibleName(), QStringLiteral("VAE (FLUX.2)"));
    QVERIFY2(d.status(QStringLiteral("diffusion_model")).startsWith(QStringLiteral("✓ FLUX.2 [klein] 4B: Makes and changes pictures")),
             qPrintable(d.status(QStringLiteral("diffusion_model"))));
    QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✓ Found"));
    QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✓ Found"));
    QVERIFY(!problems->isVisible());
    QVERIFY(ok->isEnabled());
    QVERIFY(!advancedOptions->isVisible()); // nothing unfolds by itself
    const runtime::LaunchCommand cmd = runtime::buildLaunch(chosen, rt);
    QVERIFY(cmd.arguments.contains(QStringLiteral("--diffusion-model")) && !cmd.arguments.contains(QStringLiteral("-m")));
    advanced->click();
    QVERIFY(advancedOptions->isVisible() && dlg.findChild<QPlainTextEdit *>()->isVisible());
    QVERIFY(dlg.findChild<QPlainTextEdit *>()->toPlainText().contains(QLatin1String("--llm")));
    advanced->click();
    QVERIFY(!advancedOptions->isVisible());

    // Saved as a server with a provider: the provider knows the preset, so it offers instruction
    // edits and the prompt style takes klein's 4 steps at CFG 1.
    studio.servers()->addProfile(chosen);
    ai::Provider *bound = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Klein"),
                                                          {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")},
                                                           {QStringLiteral("managed_server"), chosen.id}});
    QVERIFY(bound);
    const QString boundId = bound->instanceId();
    auto removeServer = qScopeGuard([&] {
        studio.providers()->removeProvider(boundId);
        studio.servers()->removeProfile(chosen.id);
    });
    QTRY_COMPARE(studio.providers()->provider(boundId)->setting(QStringLiteral("model_preset")).toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("flux2-klein-4b"));
    ai::ImageProvider *kleinProvider = studio.providers()->imageProvider(boundId);
    QVERIFY(kleinProvider && kleinProvider->capabilities().edit && kleinProvider->capabilities().regionEdit);
    const ai::PromptStyle kleinStyle = ai::promptStyleFor(kleinProvider);
    QCOMPARE(kleinStyle.steps, 4);
    QCOMPARE(kleinStyle.cfg, 1.0);

    // A T5 encoder chosen for klein's text encoder does not fit: said next to the field (once), and
    // the server cannot be saved like that. The file stays as chosen.
    d.type(QStringLiteral("llm"), t5);
    QCOMPARE(d.field(QStringLiteral("llm"))->text(), QDir::toNativeSeparators(t5));
    QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✗ This is a T5 text encoder; FLUX.2 [klein] 4B needs a Qwen3 4B text encoder."));
    QVERIFY(!problems->isVisible());
    QVERIFY(!ok->isEnabled());
    d.type(QStringLiteral("llm"), qwen);
    QVERIFY(ok->isEnabled());

    // Without a text encoder in the library: missing with its size, a download is offered, and saving
    // stays possible.
    QVERIFY(QFile::remove(qwen));
    studio.models()->rescan();
    QTRY_VERIFY_WITH_TIMEOUT(!studio.models()->entry(qwen) && !studio.models()->isScanning(), 20000);
    d.field(QStringLiteral("llm"))->clear();
    QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✗ Missing (qwen_3_4b.safetensors, 7.5 GB)"));
    QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✓ Found"));
    QVERIFY(!problems->isVisible());
    QVERIFY(ok->isEnabled());
    auto *download = dlg.findChild<QPushButton *>(QStringLiteral("downloadMissing"));
    QVERIFY(download && download->isVisible());
    QCOMPARE(download->text(), QStringLiteral("Download missing files (7.5 GB)"));

    // Downloading it (from a server that never answers): the line says it will be downloaded, until
    // the download stops.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));
    const QString silentUrl = QStringLiteral("http://127.0.0.1:%1").arg(silent.serverPort());
    studio.hub()->setBaseUrls(silentUrl, silentUrl);
    QStringList parts;
    for (const runtime::ModelCompanion &c : runtime::companionsOf(studio.models(), klein)) {
        if (c.missing())
            parts << runtime::FileDownload::partPath(studio.hub()->destinationFor(runtime::companionDownload(*studio.models()->entry(klein), c.file)));
    }
    auto removeParts = qScopeGuard([&] {
        for (const QString &part : std::as_const(parts))
            QFile::remove(part);
    });
    download->click();
    QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("↓ Will be downloaded (qwen_3_4b.safetensors, 7.5 GB)"));
    QVERIFY(!download->isVisible());
    QVERIFY(ok->isEnabled());
    studio.jobs()->cancelAll();
    QTRY_COMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✗ Missing (qwen_3_4b.safetensors, 7.5 GB)"));
    QVERIFY(download->isVisible());
}

// The user's request (plan §17.5): the sd.cpp server dialog shows the fields of the model's type.
// Each type shows exactly its files; a model file chooses its type; the type can be changed by hand;
// a text encoder chosen by hand is kept, saved and on the command line; a field the type does not
// have is not saved; the library lists the files that fit first; an existing profile opens with its
// type; a file can be dropped on a field.
void TestCanvas::serverDialogModelTypes()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir comfy;
    auto put = [&](const QString &rel, const Tensors &tensors) {
        const QString path = QDir::cleanPath(comfy.filePath(rel));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(shapedSafetensors(tensors));
        return path;
    };
    const QString klein = put(QStringLiteral("diffusion_models/flux-2-klein-4b.safetensors"), kleinTensors(7680));
    const QString flux1 = put(QStringLiteral("diffusion_models/flux1-dev.safetensors"), flux1DevTensors());
    const QString sdxl = put(QStringLiteral("checkpoints/juggernautXL.safetensors"), sdxlTensors());
    const QString flux2Vae = put(QStringLiteral("vae/flux2-vae.safetensors"), vaeTensors(32));
    const QString flux1Vae = put(QStringLiteral("vae/ae.safetensors"), vaeTensors(16));
    const QString qwen4b = put(QStringLiteral("text_encoders/qwen_3_4b.safetensors"), qwen3Tensors(2560));
    const QString qwen8b = put(QStringLiteral("text_encoders/qwen_3_8b.safetensors"), qwen3Tensors(4096));
    // Text encoders of the user's own, chosen by hand.
    const QString myQwen = put(QStringLiteral("text_encoders/my-qwen3-4b-q8.safetensors"), qwen3Tensors(2560));
    const QString myT5 = put(QStringLiteral("text_encoders/my-t5-encoder.safetensors"), t5Tensors());
    const QString t5 = put(QStringLiteral("text_encoders/t5xxl_fp16.safetensors"), t5Tensors());
    const QString clipL = put(QStringLiteral("clip/clip_l.safetensors"), clipTensors(768));
    put(QStringLiteral("clip/clip_g.safetensors"), clipTensors(1280));
    const QString controlNet = put(QStringLiteral("controlnet/xinsir-controlnet-openpose-sdxl-1.0.safetensors"), sdxlControlNetTensors());

    Studio studio;
    QVERIFY(studio.models()->addRoot(comfy.path()));
    QTRY_VERIFY_WITH_TIMEOUT(studio.models()->entry(klein) && studio.models()->entry(controlNet) && studio.models()->entry(myT5)
                                 && !studio.models()->isScanning(),
                             20000);
    auto removeRoot = qScopeGuard([&] { studio.models()->removeRoot(comfy.path()); });
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/test-sdcpp");
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = comfy.filePath(QStringLiteral("sd-server.exe"));
    studio.runtimes()->addInstalled(rt);
    auto removeRuntime = qScopeGuard([&] { studio.runtimes()->remove(rt.id); });
    const runtime::ServerProfile empty = studio.servers()->createProfile(rt.id, QStringLiteral("Images"));
    auto native = [](const QString &path) { return QDir::toNativeSeparators(path); };
    auto argument = [](const runtime::LaunchCommand &cmd, const QString &flag) {
        const int i = int(cmd.arguments.indexOf(flag));
        return i >= 0 ? cmd.arguments.value(i + 1) : QString();
    };
    const QStringList kleinFields{QStringLiteral("diffusion_model"), QStringLiteral("vae"), QStringLiteral("llm"), QStringLiteral("lora_dir"),
                                  QStringLiteral("upscalers_dir")};
    const QStringList flux1Fields{QStringLiteral("diffusion_model"), QStringLiteral("vae"),     QStringLiteral("clip_l"),
                                  QStringLiteral("t5xxl"),           QStringLiteral("lora_dir"), QStringLiteral("upscalers_dir")};
    const QStringList sdxlFields{QStringLiteral("model"),       QStringLiteral("vae"),         QStringLiteral("control_net"), QStringLiteral("ip_adapter"),
                                 QStringLiteral("clip_vision"), QStringLiteral("lora_dir"), QStringLiteral("upscalers_dir")};

    {
        ServerProfileDialog dlg(&studio, empty, true);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const ServerDialogDriver d{&dlg};
        // Every type of the presets, by name, and Other.
        QStringList types;
        for (int i = 0; i < d.typeCombo()->count(); ++i)
            types << d.typeCombo()->itemText(i);
        QCOMPARE(types.size(), int(runtime::modelTypes().size()) + 1);
        QCOMPARE(types.last(), QStringLiteral("Other (every field)"));
        QVERIFY(types.contains(QStringLiteral("FLUX.1 (dev, schnell, Kontext; separate files)")) && types.contains(QStringLiteral("FLUX.2 [klein] 4B"))
                && types.contains(QStringLiteral("SDXL (also Illustrious, NoobAI, Pony)")));

        // Each type by hand: exactly its fields, labelled with what they expect.
        d.chooseType(QStringLiteral("flux1"));
        QCOMPARE(d.shown(), flux1Fields);
        QCOMPARE(d.field(QStringLiteral("clip_l"))->accessibleName(), QStringLiteral("Text encoder (CLIP-L)"));
        QCOMPARE(d.field(QStringLiteral("t5xxl"))->accessibleName(), QStringLiteral("Text encoder (T5-XXL)"));
        d.chooseType(QStringLiteral("flux2-klein-4b"));
        QCOMPARE(d.shown(), kleinFields);
        QCOMPARE(d.field(QStringLiteral("llm"))->accessibleName(), QStringLiteral("Text encoder (Qwen3 4B)"));
        d.chooseType(QStringLiteral("flux2-klein-9b"));
        QCOMPARE(d.field(QStringLiteral("llm"))->accessibleName(), QStringLiteral("Text encoder (Qwen3 8B)"));
        d.chooseType(QStringLiteral("sdxl"));
        QCOMPARE(d.shown(), sdxlFields);
        QCOMPARE(d.field(QStringLiteral("vae"))->accessibleName(), QStringLiteral("VAE (optional)"));
        d.chooseType(QString());
        QCOMPARE(d.shown(), runtime::sdFileOptions());

        // Choosing the klein file from the library (every diffusion model is listed while the type is
        // Other) selects its type and fills its files.
        QAction *kleinAction = nullptr;
        for (QAction *a : d.library(QStringLiteral("diffusion_model"))) {
            if (a->toolTip() == native(klein))
                kleinAction = a;
        }
        QVERIFY(kleinAction);
        kleinAction->trigger();
        QCOMPARE(dlg.modelType(), QStringLiteral("flux2-klein-4b"));
        QCOMPARE(d.shown(), kleinFields);
        QCOMPARE(d.field(QStringLiteral("vae"))->text(), native(flux2Vae));
        QCOMPARE(d.field(QStringLiteral("llm"))->text(), native(qwen4b));

        // The text encoder's library menu: the Qwen3 4B files first, then the other text encoders
        // under a line that says they may not fit.
        const QList<QAction *> encoders = d.library(QStringLiteral("llm"));
        QStringList order;
        int heading = -1;
        for (int i = 0; i < encoders.size(); ++i) {
            if (encoders[i]->isSeparator())
                continue;
            if (!encoders[i]->isEnabled()) {
                QCOMPARE(encoders[i]->text(), QStringLiteral("Other files of this kind (they may not fit):"));
                heading = int(order.size());
                continue;
            }
            order << encoders[i]->toolTip();
        }
        QCOMPARE(heading, 2);
        QCOMPARE(QSet<QString>(order.cbegin(), order.cbegin() + 2), (QSet<QString>{native(qwen4b), native(myQwen)}));
        QVERIFY(order.mid(2).contains(native(qwen8b)) && order.mid(2).contains(native(t5)) && order.mid(2).contains(native(clipL)));
        // One that does not fit can still be chosen; it is named, and OK stays off.
        for (QAction *a : encoders) {
            if (a->toolTip() == native(qwen8b))
                a->trigger();
        }
        QCOMPARE(d.field(QStringLiteral("llm"))->text(), native(qwen8b));
        QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✗ This is a Qwen3 8B text encoder; FLUX.2 [klein] 4B needs a Qwen3 4B text encoder."));
        QVERIFY(!d.ok()->isEnabled());
        QVERIFY(!d.problems()->isVisible()); // said once, next to its field

        // A text encoder of the user's own for FLUX.2, typed in: kept, saved and on the command line.
        d.type(QStringLiteral("llm"), myQwen);
        QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✓ Found"));
        QVERIFY(d.ok()->isEnabled());
        runtime::ServerProfile p = dlg.profile();
        QCOMPARE(p.options.value(QStringLiteral("llm")).toString(), native(myQwen));
        QCOMPARE(argument(runtime::buildLaunch(p, rt), QStringLiteral("--llm")), native(myQwen));

        // The type changed by hand to FLUX.1: its fields; the klein file is not a FLUX.1 model, which
        // its line says, and the FLUX.2 text encoder is no longer saved.
        d.chooseType(QStringLiteral("flux1"));
        QCOMPARE(d.shown(), flux1Fields);
        QCOMPARE(d.status(QStringLiteral("diffusion_model")),
                 QStringLiteral("✗ This is a FLUX.2 [klein] 4B model: choose its type, FLUX.2 [klein] 4B."));
        QVERIFY(!d.ok()->isEnabled());
        QVERIFY(!dlg.profile().options.contains(QStringLiteral("llm")));
        // A FLUX.1 model typed in: its VAE (the FLUX.2 one does not fit), CLIP-L and T5-XXL.
        d.type(QStringLiteral("diffusion_model"), flux1);
        QCOMPARE(dlg.modelType(), QStringLiteral("flux1"));
        QCOMPARE(d.field(QStringLiteral("vae"))->text(), native(flux1Vae));
        QCOMPARE(d.field(QStringLiteral("clip_l"))->text(), native(clipL));
        QVERIFY(!d.field(QStringLiteral("t5xxl"))->text().isEmpty());
        QVERIFY(d.ok()->isEnabled());
        // A T5 encoder of the user's own for FLUX.1: kept, saved and on the command line; the FLUX.2
        // text encoder typed before is not.
        d.type(QStringLiteral("t5xxl"), myT5);
        p = dlg.profile();
        QCOMPARE(p.options.value(QStringLiteral("t5xxl")).toString(), native(myT5));
        QVERIFY(!p.options.contains(QStringLiteral("llm")));
        runtime::LaunchCommand cmd = runtime::buildLaunch(p, rt);
        QCOMPARE(argument(cmd, QStringLiteral("--t5xxl")), native(myT5));
        QCOMPARE(argument(cmd, QStringLiteral("--clip_l")), native(clipL));
        QCOMPARE(argument(cmd, QStringLiteral("--diffusion-model")), native(flux1));
        QVERIFY(!cmd.arguments.contains(QStringLiteral("--llm")));
        // A new model of the same type keeps the encoder chosen by hand.
        d.type(QStringLiteral("diffusion_model"), flux1);
        QCOMPARE(d.field(QStringLiteral("t5xxl"))->text(), native(myT5));
        // Back to FLUX.2 [klein] 4B by hand: what was typed there is back.
        d.chooseType(QStringLiteral("flux2-klein-4b"));
        QCOMPARE(d.field(QStringLiteral("llm"))->text(), native(myQwen));
        d.chooseType(QStringLiteral("flux1"));

        // An SDXL checkpoint typed into the Diffusion model field: its type, and it moves to the
        // Checkpoint field. The FLUX.1 VAE does not fit in place of its own; the FLUX.1 encoders are
        // not saved.
        d.type(QStringLiteral("diffusion_model"), sdxl);
        QCOMPARE(dlg.modelType(), QStringLiteral("sdxl"));
        QCOMPARE(d.shown(), sdxlFields);
        QCOMPARE(d.field(QStringLiteral("model"))->text(), native(sdxl));
        QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✗ This is a FLUX / SD 3 VAE; SDXL takes only a VAE for SD / SDXL."));
        QVERIFY(!d.ok()->isEnabled());
        d.field(QStringLiteral("vae"))->clear();
        QVERIFY(d.ok()->isEnabled());
        p = dlg.profile();
        QVERIFY(!p.options.contains(QStringLiteral("t5xxl")) && !p.options.contains(QStringLiteral("clip_l")) && !p.options.contains(QStringLiteral("diffusion_model")));
        cmd = runtime::buildLaunch(p, rt);
        QCOMPARE(argument(cmd, QStringLiteral("-m")), native(sdxl));
        QVERIFY(!cmd.arguments.contains(QStringLiteral("--t5xxl")));
        // Its ControlNet menu: the SDXL ControlNet fits.
        const QList<QAction *> controlNets = d.library(QStringLiteral("control_net"));
        QVERIFY(!controlNets.isEmpty() && controlNets.first()->toolTip() == native(controlNet));
        // Other: every field again, and every one saved.
        d.chooseType(QString());
        QCOMPARE(d.shown(), runtime::sdFileOptions());
        QCOMPARE(dlg.profile().options.value(QStringLiteral("t5xxl")).toString(), native(myT5));
    }

    // Existing profiles open with their model's type; Other when the model is in the other slot (the
    // profile of plan §17.1) or a file has no place in its type (an SDXL ControlNet with FLUX.2).
    auto openedAs = [&](const QVariantMap &options) {
        runtime::ServerProfile p = empty;
        p.options = options;
        ServerProfileDialog dlg(&studio, p, false);
        return dlg.modelType();
    };
    QCOMPARE(openedAs({{QStringLiteral("diffusion_model"), native(flux1)},
                       {QStringLiteral("vae"), native(flux1Vae)},
                       {QStringLiteral("clip_l"), native(clipL)},
                       {QStringLiteral("t5xxl"), native(myT5)}}),
             QStringLiteral("flux1"));
    QCOMPARE(openedAs({{QStringLiteral("diffusion_model"), native(klein)}, {QStringLiteral("llm"), native(myQwen)}}), QStringLiteral("flux2-klein-4b"));
    QCOMPARE(openedAs({{QStringLiteral("model"), native(sdxl)}, {QStringLiteral("control_net"), native(controlNet)}}), QStringLiteral("sdxl"));
    QCOMPARE(openedAs({{QStringLiteral("model"), native(klein)}}), QString());
    QCOMPARE(openedAs({{QStringLiteral("diffusion_model"), native(klein)}, {QStringLiteral("control_net"), native(controlNet)}}), QString());
    {
        // The profile of plan §17.1 in Other: the Checkpoint field says where the file belongs, and
        // the type moves it there.
        runtime::ServerProfile p = empty;
        p.options = {{QStringLiteral("model"), native(klein)}};
        ServerProfileDialog dlg(&studio, p, false);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const ServerDialogDriver d{&dlg};
        QCOMPARE(d.status(QStringLiteral("model")),
                 QStringLiteral("✗ This is a diffusion model (FLUX.2 Klein 4B), not a complete checkpoint: it belongs under \"Diffusion model\"."));
        QVERIFY(!d.ok()->isEnabled());
        d.chooseType(QStringLiteral("flux2-klein-4b"));
        QCOMPARE(d.field(QStringLiteral("diffusion_model"))->text(), native(klein));
        QVERIFY(d.field(QStringLiteral("model"))->text().isEmpty());
        QCOMPARE(d.field(QStringLiteral("vae"))->text(), native(flux2Vae));
        QCOMPARE(d.field(QStringLiteral("llm"))->text(), native(qwen4b));
        QVERIFY(d.ok()->isEnabled());
    }
    {
        // An existing FLUX.1 server whose type is changed by hand to one with no place for its model
        // (SDXL, FLUX.1 as one file, SD 1.5): the model stays in sight in its own field, saying what
        // is wrong, and OK is off, so the server is never saved without its model and its encoders.
        // Nothing about the FLUX.1 encoders the type does not show is said below the form.
        runtime::ServerProfile p = empty;
        p.options = {{QStringLiteral("diffusion_model"), native(flux1)},
                     {QStringLiteral("vae"), native(flux1Vae)},
                     {QStringLiteral("clip_l"), native(clipL)},
                     {QStringLiteral("t5xxl"), native(myT5)}};
        ServerProfileDialog dlg(&studio, p, false);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const ServerDialogDriver d{&dlg};
        QCOMPARE(dlg.modelType(), QStringLiteral("flux1"));
        QCOMPARE(d.field(QStringLiteral("vae"))->accessibleName(), QStringLiteral("VAE (FLUX.1)"));
        QVERIFY(d.ok()->isEnabled());
        for (const QString &other : {QStringLiteral("sdxl"), QStringLiteral("flux1-checkpoint"), QStringLiteral("sd15")}) {
            d.chooseType(other);
            QVERIFY2(d.shown().contains(QStringLiteral("diffusion_model")) && !d.shown().contains(QStringLiteral("model")), qPrintable(other));
            QCOMPARE(d.status(QStringLiteral("diffusion_model")),
                     QStringLiteral("✗ This is a FLUX.1 [dev] model: choose its type, FLUX.1 (dev, schnell, Kontext; separate files)."));
            QVERIFY2(!d.ok()->isEnabled(), qPrintable(other));
            QVERIFY2(!d.problems()->isVisible(), qPrintable(d.problems()->text()));
            QCOMPARE(dlg.profile().options.value(QStringLiteral("diffusion_model")).toString(), native(flux1));
        }
        // The FLUX.1 VAE is judged by the type shown: SDXL takes only an SD / SDXL one.
        d.chooseType(QStringLiteral("sdxl"));
        QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✗ This is a FLUX / SD 3 VAE; this model type takes only a VAE for SD / SDXL."));
        // Turning the mouse wheel over the type, to scroll the form, does not change it.
        QComboBox *combo = d.typeCombo();
        const int before = combo->currentIndex();
        QWheelEvent wheel(QPointF(5, 5), combo->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(combo, &wheel);
        QCOMPARE(combo->currentIndex(), before);
        // Back to FLUX.1: as it was, and it can be saved with its encoders.
        d.chooseType(QStringLiteral("flux1"));
        QVERIFY(d.ok()->isEnabled());
        runtime::ServerProfile back = dlg.profile();
        QCOMPARE(back.options.value(QStringLiteral("diffusion_model")).toString(), native(flux1));
        QCOMPARE(back.options.value(QStringLiteral("clip_l")).toString(), native(clipL));
        QCOMPARE(back.options.value(QStringLiteral("t5xxl")).toString(), native(myT5));
        // The model cleared under SDXL: the Checkpoint field takes its place and asks for the file
        // there, not in a line below the form that names a field SDXL does not have.
        d.chooseType(QStringLiteral("sdxl"));
        d.field(QStringLiteral("diffusion_model"))->clear();
        QCOMPARE(d.shown(), sdxlFields);
        QVERIFY2(d.status(QStringLiteral("model")).startsWith(QStringLiteral("• Choose the checkpoint file")), qPrintable(d.status(QStringLiteral("model"))));
        QVERIFY2(!d.problems()->isVisible(), qPrintable(d.problems()->text()));
        // With no model, the VAE left over from FLUX.1 still does not fit SDXL, and keeps OK off.
        QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✗ This is a FLUX / SD 3 VAE; this model type takes only a VAE for SD / SDXL."));
        QVERIFY(!d.ok()->isEnabled());
        d.field(QStringLiteral("vae"))->clear();
        QVERIFY(d.ok()->isEnabled());
    }
    {
        // An SDXL server changed by hand to FLUX.1: its checkpoint stays in sight, and OK is off.
        runtime::ServerProfile p = empty;
        p.options = {{QStringLiteral("model"), native(sdxl)}};
        ServerProfileDialog dlg(&studio, p, false);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const ServerDialogDriver d{&dlg};
        QCOMPARE(dlg.modelType(), QStringLiteral("sdxl"));
        d.chooseType(QStringLiteral("flux1"));
        QVERIFY(d.shown().contains(QStringLiteral("model")) && !d.shown().contains(QStringLiteral("diffusion_model")));
        QVERIFY2(d.status(QStringLiteral("model")).endsWith(QStringLiteral("choose its type, SDXL (also Illustrious, NoobAI, Pony).")),
                 qPrintable(d.status(QStringLiteral("model"))));
        QVERIFY(!d.ok()->isEnabled());
        QCOMPARE(dlg.profile().options.value(QStringLiteral("model")).toString(), native(sdxl));
    }
    {
        // A new server of a type chosen by hand, before any model: a component of another type is
        // named next to its field, and OK is off (FLUX.2's VAE with SDXL, a T5 encoder for klein).
        ServerProfileDialog dlg(&studio, empty, true);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const ServerDialogDriver d{&dlg};
        d.chooseType(QStringLiteral("sdxl"));
        d.type(QStringLiteral("vae"), flux2Vae);
        QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✗ This is a FLUX.2 VAE; this model type takes only a VAE for SD / SDXL."));
        QVERIFY(!d.ok()->isEnabled());
        d.chooseType(QStringLiteral("flux2-klein-4b"));
        QCOMPARE(d.status(QStringLiteral("vae")), QStringLiteral("✓ Found"));
        d.type(QStringLiteral("llm"), t5);
        QCOMPARE(d.status(QStringLiteral("llm")), QStringLiteral("✗ This is a T5 text encoder; this model type needs a Qwen3 4B text encoder."));
        QVERIFY(!d.ok()->isEnabled());
        d.type(QStringLiteral("llm"), myQwen);
        QVERIFY(d.ok()->isEnabled());
    }
    {
        // A model file dropped on its field chooses the type too.
        ServerProfileDialog dlg(&studio, empty, true);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const ServerDialogDriver d{&dlg};
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(klein)});
        QLineEdit *target = d.field(QStringLiteral("diffusion_model"));
        QDragEnterEvent enter(target->rect().center(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(target->rect().center()), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &drop);
        QCOMPARE(dlg.modelType(), QStringLiteral("flux2-klein-4b"));
        QCOMPARE(target->text(), native(klein));
        QCOMPARE(d.field(QStringLiteral("llm"))->text(), native(qwen4b));
    }
}

void TestCanvas::modelsFolderSettings()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().remove(QStringLiteral("models/folder"));
    QTemporaryDir dir;
    Studio studio;
    runtime::ModelLibrary *lib = studio.models();
    const QString standard = lib->defaultFolder();
    // Relocated test data keeps the standard folder next to it, so nothing is asked at start-up.
    QCOMPARE(standard, QDir::cleanPath(paths::defaultModels()));
    QVERIFY(studio.legacyModelsFolder().isEmpty());
    const QString model = standard + QStringLiteral("/checkpoints/folder-test.safetensors");
    const QString lora = standard + QStringLiteral("/loras/folder-test-lora.safetensors");
    for (const QString &path : {model, lora}) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(fakeSafetensors({QStringLiteral("model.diffusion_model.input_blocks.0.0.weight")}));
    }
    runtime::ServerProfile profile = studio.servers()->createProfile(QStringLiteral("adopted/none"), QStringLiteral("Folder test"));
    profile.options = {{QStringLiteral("model"), QDir::toNativeSeparators(model)},
                       {QStringLiteral("lora_dir"), QDir::toNativeSeparators(standard + QStringLiteral("/loras"))}};
    studio.servers()->addProfile(profile);
    QImage ref(32, 32, QImage::Format_ARGB32);
    ref.fill(Qt::red);
    auto saved = studio.characters()->save(QStringLiteral("Mira (folder test)"), {{QStringLiteral("tags"), QStringLiteral("red hair")}}, {ref}, QString(),
                                           QString(), QJsonObject{{QStringLiteral("file"), lora}, {QStringLiteral("trigger"), QStringLiteral("lmn_mira")}});
    QTRY_VERIFY(saved.isFinished());
    const QString characterId = saved.result().id;
    const QString target = QDir::cleanPath(dir.filePath(QStringLiteral("Big drive/Models")));
    // Leaves the shared test data as it found it, also when a check fails.
    auto cleanup = qScopeGuard([&] {
        studio.servers()->removeProfile(profile.id);
        studio.characters()->remove(characterId);
        for (const QString &path : {model, lora})
            QFile::remove(path);
        lib->setFolder(QString());
        Settings::instance().remove(QStringLiteral("models/folder"));
    });
    auto lastMove = [&studio]() -> Job * {
        Job *found = nullptr;
        for (Job *j : studio.jobs()->jobs()) {
            if (j->kind() == QLatin1String("models-folder"))
                found = j;
        }
        return found;
    };

    // Help → Open models folder; no question at start-up without models in an old folder.
    MainWindow window(&studio);
    QAction *open = window.findChild<QAction *>(QStringLiteral("openModelsFolder"));
    QVERIFY(open && open->isEnabled());
    QVERIFY(!window.offerModelsFolderMove([](const runtime::FolderMovePlan &) {
        [] { QFAIL("asked without models in an old folder"); }();
        return true;
    }));

    // Settings → Models shows the folder.
    SettingsDialog dlg(&studio, &window);
    dlg.showTab(QStringLiteral("models"));
    dlg.show();
    auto *page = dlg.findChild<ModelsPage *>();
    QVERIFY(page);
    auto *shown = page->findChild<ElidedLabel *>(QStringLiteral("modelsFolder"));
    QVERIFY(shown);
    QCOMPARE(shown->fullText(), QDir::toNativeSeparators(standard));
    QVERIFY(page->findChild<QPushButton *>(QStringLiteral("moveModelsFolder")));

    QString message;
    auto answer = [&message](bool accept) {
        QTimer::singleShot(200, [&message, accept] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                message = box->text() + u'|' + box->informativeText();
                if (accept)
                    box->defaultButton()->click();
                else
                    box->reject();
            }
        });
    };
    // Into itself: refused with the reason.
    answer(true);
    page->moveFolder(standard + QStringLiteral("/loras/sub"));
    QVERIFY2(message.contains(QStringLiteral("inside the current models folder")), qPrintable(message));
    QVERIFY(!lastMove());
    // Cancelled: nothing moves.
    const runtime::FolderMovePlan plan = runtime::planModelsFolderMove(standard, target);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    message.clear();
    answer(false);
    page->moveFolder(target);
    QVERIFY2(message.startsWith(QStringLiteral("Move %1 model(s)").arg(plan.models)), qPrintable(message));
    QVERIFY(!lastMove());
    QVERIFY(QFileInfo::exists(model));

    // Moved: a Queue job; the setting, the page, the server and the character's LoRA follow.
    answer(true);
    page->moveFolder(target);
    Job *job = lastMove();
    QVERIFY(job);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 20000);
    QVERIFY2(job->state() == Job::State::Succeeded, qPrintable(job->errorString()));
    QCOMPARE(lib->defaultFolder(), target);
    QCOMPARE(Settings::instance().value(QStringLiteral("models/folder")).toString(), target);
    QCOMPARE(shown->fullText(), QDir::toNativeSeparators(target));
    const QString movedModel = target + QStringLiteral("/checkpoints/folder-test.safetensors");
    QVERIFY(QFileInfo::exists(movedModel));
    QVERIFY(!QFileInfo::exists(model));
    QCOMPARE(studio.servers()->profile(profile.id)->options.value(QStringLiteral("model")).toString(), QDir::toNativeSeparators(movedModel));
    QCOMPARE(studio.servers()->profile(profile.id)->options.value(QStringLiteral("lora_dir")).toString(),
             QDir::toNativeSeparators(target + QStringLiteral("/loras")));
    QCOMPARE(studio.characters()->entry(characterId)->lora.value(QStringLiteral("file")).toString(),
             target + QStringLiteral("/loras/folder-test-lora.safetensors"));
    // Downloads go there now.
    runtime::HubDownload d;
    d.file.path = QStringLiteral("next.safetensors");
    d.file.kind = runtime::ModelKind::Lora;
    QCOMPARE(studio.hub()->destinationFor(d), target + QStringLiteral("/loras/next.safetensors"));
    // A stopped training continues with its base model where it is now.
    QFuture<runtime::ModelEntry> added = lib->addFile(movedModel);
    QTRY_VERIFY(added.isFinished() && lib->entry(movedModel));
    QCOMPARE(CharacterTraining::findCheckpoint(&studio, model), movedModel);
    QCOMPARE(CharacterTraining::findCheckpoint(&studio, movedModel), movedModel);
    const QString elsewhere = dir.filePath(QStringLiteral("other.safetensors"));
    QCOMPARE(CharacterTraining::findCheckpoint(&studio, elsewhere), elsewhere); // unknown: as it was
    // While a move runs, no training starts (its LoRA is copied into the folder at the end).
    lib->beginMove();
    CharacterTraining::Settings training;
    training.characterId = characterId;
    QString why;
    QVERIFY(!CharacterTraining::start(&studio, training, &why));
    QVERIFY2(why.contains(QStringLiteral("being moved")), qPrintable(why));
    lib->endMove();

    // And back, so the shared test data keeps its files.
    Job *back = studio.moveModelsFolder(standard);
    QTRY_VERIFY_WITH_TIMEOUT(back->isFinished(), 20000);
    QVERIFY2(back->state() == Job::State::Succeeded, qPrintable(back->errorString()));
    QVERIFY(QFileInfo::exists(model));
    QCOMPARE(studio.characters()->entry(characterId)->lora.value(QStringLiteral("file")).toString(), lora);
    QCOMPARE(lib->defaultFolder(), standard);
    // The standard folder is no choice: the setting is gone, not a path to it.
    QVERIFY(lib->chosenFolder().isEmpty());
    QVERIFY(!Settings::instance().contains(QStringLiteral("models/folder")));
    dlg.close();
}

// What the prompt bar's Generate does, from the selection, a blank canvas and what the engine can
// do; what Remove sends; when a canvas is blank; the size a new image is drawn at.
void TestCanvas::promptModes()
{
    ai::ImageCapabilities klein; // a local instruction edit model
    klein.imageToImage = klein.inpaint = klein.edit = klein.regionEdit = true;
    ai::ImageCapabilities cloudEdit; // an online editing service: edits with a mask
    cloudEdit.imageToImage = cloudEdit.inpaint = cloudEdit.edit = true;
    ai::ImageCapabilities inpaint; // a regular model on a server
    inpaint.imageToImage = inpaint.inpaint = true;
    ai::ImageCapabilities img2img;
    img2img.imageToImage = true;
    ai::ImageCapabilities newOnly; // text to image only
    ai::ImageCapabilities noText;
    noText.textToImage = false;
    noText.imageToImage = noText.inpaint = true;
    using A = PromptAction;
    using M = ai::GenMode;
    struct Row
    {
        bool selection, blank;
        const ai::ImageCapabilities *caps;
        A action;
        M mode;
        bool region, strength, variation;
    };
    const QList<Row> rows = {
        {true, false, &klein, A::EditArea, M::Edit, true, false, false},
        {true, true, &klein, A::EditArea, M::Edit, true, false, false}, // a selection wins over a blank canvas
        {false, false, &klein, A::EditImage, M::Edit, false, false, false},
        {false, true, &klein, A::NewImage, M::TextToImage, false, false, false},
        {true, false, &cloudEdit, A::InpaintArea, M::Inpaint, true, true, false},
        {false, false, &cloudEdit, A::EditImage, M::Edit, false, false, false},
        {true, false, &inpaint, A::InpaintArea, M::Inpaint, true, true, false},
        {false, false, &inpaint, A::CannotEditImage, M::ImageToImage, false, true, true},
        {false, true, &inpaint, A::NewImage, M::TextToImage, false, false, false},
        {true, false, &img2img, A::RedrawArea, M::ImageToImage, true, true, false},
        {false, false, &img2img, A::CannotEditImage, M::ImageToImage, false, true, true},
        {true, false, &newOnly, A::CannotChangeImages, M::TextToImage, false, false, false},
        {false, true, &newOnly, A::NewImage, M::TextToImage, false, false, false},
        {false, true, &noText, A::CannotCreate, M::TextToImage, false, false, false},
        {true, false, nullptr, A::NoEngine, M::TextToImage, false, false, false},
        {false, true, nullptr, A::NoEngine, M::TextToImage, false, false, false},
    };
    for (int i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        const PromptPlan p = promptPlan(r.selection, r.blank, r.caps);
        const QByteArray row = QByteArray::number(i);
        QVERIFY2(p.action == r.action, row.constData());
        QVERIFY2(p.mode == r.mode, row.constData());
        QVERIFY2(p.region == r.region, row.constData());
        QVERIFY2(p.strength == r.strength, row.constData());
        QVERIFY2(p.variation == r.variation, row.constData());
        const bool runs = r.action == A::EditArea || r.action == A::InpaintArea || r.action == A::RedrawArea || r.action == A::EditImage
                          || r.action == A::NewImage;
        QVERIFY2(p.canRun() == runs, row.constData());
    }
    // An image engine that cannot do img2img offers no variation.
    ai::ImageCapabilities editOnlyMask;
    editOnlyMask.inpaint = true;
    QVERIFY(!promptPlan(false, false, &editOnlyMask).variation);

    // Remove: an instruction for edit models, else the surroundings painted in, at full strength.
    const RemovalRequest edit = removalRequest(promptPlan(true, false, &klein));
    QCOMPARE(edit.mode, M::Edit);
    QVERIFY(edit.prompt.startsWith(QStringLiteral("Remove the object")));
    const RemovalRequest fill = removalRequest(promptPlan(true, false, &inpaint));
    QCOMPARE(fill.mode, M::Inpaint);
    QCOMPARE(fill.prompt, QStringLiteral("empty background, seamless continuation of the surroundings"));
    QCOMPARE(fill.strength, 1.0);
    QCOMPARE(removalRequest(promptPlan(true, false, &img2img)).mode, M::ImageToImage);

    // Blank: every layer empty or one flat colour.
    doc::Document blank(QSize(64, 48));
    QVERIFY(isBlankCanvas(blank));
    QImage white(blank.size(), QImage::Format_ARGB32_Premultiplied);
    white.fill(Qt::white);
    blank.addLayer(QStringLiteral("Background"), white);
    blank.addLayer(QStringLiteral("Empty"));
    QVERIFY(isBlankCanvas(blank));
    QImage dot = white;
    dot.setPixelColor(63, 47, Qt::black);
    blank.addLayer(QStringLiteral("Dot"), dot);
    QVERIFY(!isBlankCanvas(blank));
    QVERIFY(!isBlankCanvas(*m_doc));

    // A new image: the canvas's shape at the engine's pixels, or its closest fixed size.
    ai::ImageCapabilities local;
    local.nativePixels = 1024 * 1024;
    local.sizeMultiple = 64;
    QCOMPARE(workingSize(QSize(768, 512), local), img::fitToPixelBudget(QSize(768, 512), 1024 * 1024, 64));
    QCOMPARE(workingSize(QSize(1024, 1024), local), QSize(1024, 1024));
    // An engine that takes any size (a ComfyUI workflow) still gets multiples of 8, as from New from
    // prompt; a change of a whole picture leaves that to the engine.
    ai::ImageCapabilities anySize;
    anySize.nativePixels = 1024 * 1024;
    anySize.sizeMultiple = 1;
    const QSize any = workingSize(QSize(1000, 700), anySize);
    QCOMPARE(any, img::fitToPixelBudget(QSize(1000, 700), 1024 * 1024, 8));
    QVERIFY(any.width() % 8 == 0 && any.height() % 8 == 0);
    ai::ImageCapabilities fixed;
    fixed.fixedSizes = {QSize(1024, 1024), QSize(1536, 1024), QSize(1024, 1536)};
    QCOMPARE(workingSize(QSize(1600, 1000), fixed), QSize(1536, 1024));
    QCOMPARE(workingSize(QSize(600, 900), fixed), QSize(1024, 1536));
}

namespace {

// A stable-diffusion.cpp server: each generation answers with `batch_count` images of the size asked
// for, one colour each (with a dark frame, so that no result is a flat colour). While `hold` is on,
// jobs stay queued. With `echo`, an edit answers with the picture it was sent, tinted warm and with
// its brightness kept (a colorization that changes nothing else).
struct MockSdCpp
{
    MockHttpServer server;
    QList<QJsonObject> generations;
    QList<QColor> colors{QColor(255, 128, 0), QColor(0, 90, 200), QColor(40, 170, 60), QColor(200, 40, 160)};
    bool hold = false;
    bool echo = false;
    int cancels = 0;
    QJsonArray loras; // what its capabilities list in its LoRA folder

    static QImage tinted(const QImage &source)
    {
        QImage im = source.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < im.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(im.scanLine(y));
            for (int x = 0; x < im.width(); ++x) {
                const int l = qGray(line[x]);
                line[x] = qRgb(std::min(255, l * 11 / 10), l, l * 8 / 10);
            }
        }
        return im;
    }

    MockSdCpp()
    {
        server.route("GET", QStringLiteral("/sdcpp/v1/capabilities"),
                     [this](const MockRequest &) { return MockResponse::json({{QStringLiteral("loras"), loras}}); });
        server.route("POST", QStringLiteral("/sdcpp/v1/img_gen"), [this](const MockRequest &req) {
            generations << req.json();
            return MockResponse::json({{QStringLiteral("id"), QStringLiteral("job_%1").arg(generations.size())}}, 202);
        });
        for (int n = 1; n <= 24; ++n) {
            server.route("GET", QStringLiteral("/sdcpp/v1/jobs/job_%1").arg(n), [this, n](const MockRequest &) {
                if (hold)
                    return MockResponse::json({{QStringLiteral("status"), QStringLiteral("queued")}});
                const QJsonObject g = generations.value(n - 1);
                const QSize size(g.value(QStringLiteral("width")).toInt(), g.value(QStringLiteral("height")).toInt());
                QJsonArray images;
                const QJsonArray refs = g.value(QStringLiteral("ref_images")).toArray();
                for (int i = 0; i < std::max(1, g.value(QStringLiteral("batch_count")).toInt()); ++i) {
                    QImage im(size, QImage::Format_ARGB32);
                    im.fill(colors[i % colors.size()]);
                    QPainter p(&im);
                    p.setPen(QColor(20, 20, 20));
                    p.drawRect(im.rect().adjusted(0, 0, -1, -1));
                    p.end();
                    if (echo && !refs.isEmpty())
                        im = tinted(img::fromBase64(refs.first().toString().toLatin1()).scaled(size));
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
    QJsonObject last() const { return generations.value(generations.size() - 1); }
};

// FLUX.2 [klein] 4B as the provider of a server Lumen runs sees it: instruction edits, references,
// 4 steps at CFG 1, no negative prompt.
ai::ModelDefaults kleinPreset()
{
    ai::ModelDefaults klein;
    klein.id = QStringLiteral("flux2-klein-4b");
    klein.name = QStringLiteral("FLUX.2 [klein] 4B");
    klein.cfg = 1.0;
    klein.steps = 4;
    klein.sampler = QStringLiteral("euler");
    klein.nativeResolution = 1024;
    klein.edit = true;
    klein.references = 4;
    klein.negativePrompt = false;
    return klein;
}

// A photo with detail everywhere, so that any change outside a selection would show.
QImage detailedPhoto(const QSize &size = QSize(1200, 900))
{
    QImage im(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < im.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < im.width(); ++x)
            line[x] = qRgb((x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256);
    }
    return im;
}

// A studio whose only image engine is the mock sd.cpp server (with the klein preset or none), a
// project for the results, and the main window.
struct PromptBarFixture
{
    MockSdCpp sd;
    QTemporaryDir dir;
    Studio studio;
    ai::SdCppProvider *provider = nullptr;
    std::unique_ptr<MainWindow> window;

    bool setUp(bool editModel)
    {
        Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
        Settings::instance().remove(QStringLiteral("editor/ai"));
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        QVariantMap settings{{QStringLiteral("url"), sd.server.baseUrl().toString()}};
        if (editModel) {
            settings.insert(QStringLiteral("model"), QStringLiteral("flux-2-klein-4b.safetensors"));
            settings.insert(QStringLiteral("model_preset"), kleinPreset().toVariant());
        }
        provider = qobject_cast<ai::SdCppProvider *>(
            studio.providers()->addProvider(QStringLiteral("sdcpp"), editModel ? QStringLiteral("Klein") : QStringLiteral("SD"), settings));
        if (!provider)
            return false;
        provider->pollIntervalMs = 20;
        studio.openProject(dir.filePath(QStringLiteral("bar.lumen")));
        if (!QTest::qWaitFor([this] { return studio.isProjectOpen(); }, 5000))
            return false;
        window = std::make_unique<MainWindow>(&studio);
        window->resize(1400, 900);
        window->show();
        return QTest::qWaitForWindowExposed(window.get());
    }
    ~PromptBarFixture()
    {
        window.reset();
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        Settings::instance().remove(QStringLiteral("editor/ai"));
    }
    EditorPage *editor() const { return window->editor(); }
    PromptBar *bar() const { return window->editor()->promptBar(); }
    VariantStrip *strip() const { return window->editor()->variantStrip(); }
    AutoTextEdit *words() const { return bar()->findChild<AutoTextEdit *>(QStringLiteral("promptText")); }
    QPushButton *button(const QString &name) const { return bar()->findChild<QPushButton *>(name); }
};

// The selection's feathered edge: grown by 6 + feather / 2 px and blurred by three box passes of
// feather / 2 px (16 px by default: 38 px out).
QRect featherReach(const QRect &area, int feather = 16)
{
    const int out = 6 + feather / 2 + 3 * (feather / 2);
    return area.adjusted(-out, -out, out, out);
}

// Pixels of `layer` that are not clear outside `reach`, and pixels of the picture that changed
// where the layer is clear.
std::pair<int, int> changesOutside(const QImage &layer, const QRect &reach, const QImage &before, const QImage &after)
{
    int outside = 0, changed = 0;
    for (int y = 0; y < layer.height(); ++y) {
        for (int x = 0; x < layer.width(); ++x) {
            const bool clear = qAlpha(layer.pixel(x, y)) == 0;
            if (!clear && !reach.contains(x, y))
                ++outside;
            if (clear && after.pixel(x, y) != before.pixel(x, y))
                ++changed;
        }
    }
    return {outside, changed};
}

} // namespace

// The prompt bar with an instruction edit model (FLUX.2 [klein] on sd.cpp): what Generate does
// follows the selection. A selection edit shows its results over the document, ←/→ compares them,
// Enter keeps one as a layer (one step; every pixel outside the feathered selection as it was),
// Esc drops them, Cancel stops a run, and Remove sends a removal instruction.
void TestCanvas::promptBarSelectionEdit()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    EditorPage *editor = f.editor();
    PromptBar *bar = f.bar();
    VariantStrip *strip = f.strip();
    AutoTextEdit *words = f.words();
    QPushButton *generate = f.button(QStringLiteral("promptGenerate"));
    QPushButton *remove = f.button(QStringLiteral("promptRemove"));
    QVERIFY(words && generate && remove);

    doc::Document *doc = f.studio.openImageAsDocument(detailedPhoto(), QStringLiteral("Photo"));
    QVERIFY(bar->isVisible());
    // The right panel shows Layers and History; the AI panel waits until it is asked for.
    QVERIFY(!editor->isAiPanelShown());
    // Without a selection, the words change the whole image.
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
    QTRY_VERIFY2(bar->hint().startsWith(QStringLiteral("Changes the whole image")), qPrintable(bar->hint()));
    QVERIFY(!remove->isVisible());

    // A selection: an instruction edit of the area.
    const QRect area(400, 300, 300, 220);
    QImage mask = img::createMask(doc->size());
    QPainter(&mask).fillRect(area, Qt::white);
    doc->applySelection(mask, QStringLiteral("Select"));
    QTRY_VERIFY2(bar->hint().startsWith(QStringLiteral("Changes the selected area")), qPrintable(bar->hint()));
    QVERIFY(bar->plan().action == PromptAction::EditArea);
    QVERIFY(remove->isVisible());
    QVERIFY(!generate->isEnabled()); // no words yet
    words->setPlainText(QStringLiteral("colorize it, keep everything else"));
    QTRY_VERIFY(generate->isEnabled());
    const QImage before = doc->flattened();
    const int layers = doc->layerCount();
    const int steps = doc->undoStack()->index();

    // Enter runs it: two results over the document, which does not change.
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(f.sd.generations.size(), 1);
    const QJsonObject sent = f.sd.last();
    QCOMPARE(sent.value(QStringLiteral("prompt")).toString(), QStringLiteral("colorize it, keep everything else"));
    QCOMPARE(sent.value(QStringLiteral("ref_images")).toArray().size(), 1); // the area as the edit's source
    QVERIFY(!sent.contains(QStringLiteral("init_image")) && !sent.contains(QStringLiteral("mask_image")));
    QCOMPARE(sent.value(QStringLiteral("batch_count")).toInt(), 2);
    QCOMPARE(sent.value(QStringLiteral("sample_params")).toObject().value(QStringLiteral("sample_steps")).toInt(), 4);
    QVERIFY(strip->isVisible());
    QCOMPARE(doc->layerCount(), layers);
    QCOMPARE(doc->undoStack()->index(), steps);
    // The strip takes some of the canvas's height; the fitted picture stays whole.
    CanvasView *canvas = editor->canvas();
    QVERIFY(canvas->isFitted());
    QTRY_VERIFY(QRectF(canvas->focusProxy()->rect()).contains(canvas->mapFromDoc(QRectF(doc->bounds()))));
    // Enter ran it from the words, but the user did not type on: the strip has the keyboard.
    QTRY_COMPARE(QApplication::focusWidget(), strip);
    const QImage first = strip->currentImage();
    QCOMPARE(first.size(), doc->size());
    QCOMPARE(editor->canvas()->preview(), first);
    QCOMPARE(generate->text(), QStringLiteral("Generate"));

    // ←/→ compare them on the canvas, also while the canvas has the keyboard.
    QTest::keyClick(strip, Qt::Key_Right);
    QCOMPARE(strip->current(), 1);
    QVERIFY(editor->canvas()->preview() != first);
    QTest::keyClick(editor->canvas()->focusProxy(), Qt::Key_Left);
    QCOMPARE(strip->current(), 0);
    QCOMPARE(editor->canvas()->preview(), first);
    QTest::keyClick(strip, Qt::Key_Right);
    const QImage second = strip->currentImage();

    // Enter keeps it: one new layer on top, named after the words, one step in the history.
    QTest::keyClick(strip, Qt::Key_Return);
    QCOMPARE(strip->count(), 0);
    QVERIFY(!strip->isVisible());
    QVERIFY(editor->canvas()->preview().isNull());
    QCOMPARE(doc->layerCount(), layers + 1);
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    const doc::LayerPtr kept = doc->layers().last();
    QCOMPARE(kept->props.name, QStringLiteral("colorize it, keep everything else"));
    QCOMPARE(kept->image, second);
    QCOMPARE(doc->activeLayerId(), kept->id);
    // Masked to the selection: nothing outside it and its feathered edge, and the picture is the
    // same wherever the layer is clear.
    const QImage after = doc->flattened();
    const auto [outside, changed] = changesOutside(kept->image, featherReach(area), before, after);
    QCOMPARE(outside, 0);
    QCOMPARE(changed, 0);
    QVERIFY(after.pixel(550, 410) != before.pixel(550, 410)); // the area itself changed
    // One undo takes it away again.
    doc->undoStack()->undo();
    QCOMPARE(doc->layerCount(), layers);
    QCOMPARE(doc->flattened(), before);

    // Esc (here on the canvas) drops the results: no layer, no step.
    generate->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    const int index = doc->undoStack()->index();
    QTest::keyClick(editor->canvas()->focusProxy(), Qt::Key_Escape);
    QCOMPARE(strip->count(), 0);
    QVERIFY(editor->canvas()->preview().isNull());
    QCOMPARE(doc->layerCount(), layers);
    QCOMPARE(doc->undoStack()->index(), index);

    // While a run goes, the bar says so and Generate cancels it.
    f.sd.hold = true;
    generate->click();
    QTRY_VERIFY_WITH_TIMEOUT(strip->isRunning(), 10000);
    QTRY_COMPARE(generate->text(), QStringLiteral("Cancel"));
    QTRY_VERIFY2(bar->hint().startsWith(QStringLiteral("Working on 2 results")), qPrintable(bar->hint()));
    QVERIFY(!remove->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(f.sd.generations.size(), 3, 10000); // the job reached the server
    generate->click();
    QTRY_VERIFY_WITH_TIMEOUT(!strip->isRunning(), 10000);
    QTRY_COMPARE(generate->text(), QStringLiteral("Generate"));
    QCOMPARE(strip->count(), 0);
    QCOMPARE(doc->layerCount(), layers);
    f.sd.hold = false;

    // Remove: an instruction to fill the area from its surroundings.
    remove->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(f.sd.generations.size(), 4);
    QVERIFY(f.sd.last().value(QStringLiteral("prompt")).toString().startsWith(QStringLiteral("Remove the object")));
    QCOMPARE(f.sd.last().value(QStringLiteral("ref_images")).toArray().size(), 1);
    QCOMPARE(strip->currentName(), QStringLiteral("Removed"));
    // Results belong to their document: another one shows none, and back they are.
    doc::Document *other = f.studio.openImageAsDocument(detailedPhoto(QSize(300, 200)), QStringLiteral("Other"));
    QCOMPARE(strip->count(), 0);
    QVERIFY(editor->canvas()->preview().isNull());
    f.studio.setActiveDocument(doc);
    QCOMPARE(strip->count(), 2);
    QVERIFY(!editor->canvas()->preview().isNull());
    strip->discard();
    f.studio.closeDocument(other);
}

// A blank canvas: Generate makes a new image of the canvas's shape at the engine's working size, and
// the kept result fills the canvas.
void TestCanvas::promptBarNewImage()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    PromptBar *bar = f.bar();
    VariantStrip *strip = f.strip();
    QPushButton *generate = f.button(QStringLiteral("promptGenerate"));
    QVERIFY(generate);
    doc::Document *doc = f.studio.newDocument(QSize(768, 512), QStringLiteral("Blank"));
    QTRY_VERIFY(bar->plan().action == PromptAction::NewImage);
    QVERIFY2(bar->hint().startsWith(QStringLiteral("Creates an image of 768 × 512 px")), qPrintable(bar->hint()));
    QVERIFY(!f.button(QStringLiteral("promptRemove"))->isVisible());
    f.words()->setPlainText(QStringLiteral("a lighthouse on a cliff at dusk"));
    QTRY_VERIFY(generate->isEnabled());
    generate->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    const QJsonObject sent = f.sd.last();
    const QSize work = workingSize(doc->size(), f.provider->capabilities());
    QCOMPARE(work, img::fitToPixelBudget(QSize(768, 512), 1024 * 1024, 64)); // the canvas's shape at the model's pixels
    QCOMPARE(QSize(sent.value(QStringLiteral("width")).toInt(), sent.value(QStringLiteral("height")).toInt()), work);
    QVERIFY(!sent.contains(QStringLiteral("init_image")) && !sent.contains(QStringLiteral("ref_images")) && !sent.contains(QStringLiteral("mask_image")));
    QVERIFY(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("a lighthouse on a cliff at dusk")));
    QCOMPARE(strip->currentImage().size(), QSize(768, 512));
    QTest::keyClick(strip, Qt::Key_Return);
    QCOMPARE(doc->layerCount(), 2);
    const doc::LayerPtr image = doc->layers().last();
    QCOMPARE(image->image.size(), QSize(768, 512));
    QCOMPARE(image->image.pixelColor(384, 256), QColor(255, 128, 0));
    QCOMPARE(image->props.name, QStringLiteral("a lighthouse on a cliff at dusk"));
    // With a picture on it, the words change the whole image.
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
}

// LoRAs chosen in the prompt bar's ⋯ instead of typed (plan M18 §18.3), from a server elsewhere that
// lists them: the chosen one goes with the request at its weight with its trigger words, an unticked
// one does not, and the choice is kept for the engine in the next session.
void TestCanvas::loraPickerElsewhere()
{
    PromptBarFixture f;
    f.sd.loras = QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("ink")}, {QStringLiteral("path"), QStringLiteral("styles/ink.safetensors")}},
                            QJsonObject{{QStringLiteral("name"), QStringLiteral("pixel")}, {QStringLiteral("path"), QStringLiteral("pixel.safetensors")}}};
    QVERIFY(f.setUp(true));
    Settings::instance().remove(QStringLiteral("editor/ai/loras"));
    auto cleanup = qScopeGuard([] { Settings::instance().remove(QStringLiteral("editor/ai/loras")); });
    doc::Document *doc = f.studio.newDocument(QSize(512, 512), QStringLiteral("Blank"));
    QVERIFY(doc);
    QTRY_VERIFY(f.bar()->plan().action == PromptAction::NewImage);
    LoraPicker *picker = f.bar()->loraPicker();
    QVERIFY(picker);
    QSignalSpy listed(picker, &LoraPicker::listed);
    f.bar()->findChild<QToolButton *>(QStringLiteral("promptMore"))->click();
    QTRY_VERIFY(f.bar()->optionsPopup()->isVisible());
    QCOMPARE(picker->engine(), f.provider);
    QTRY_VERIFY(listed.size() >= 1);
    QCOMPARE(picker->listing().loras.size(), 2);
    QVERIFY(picker->listing().problem.isEmpty());
    // The menu: both, as the server names them (what they were made for is not known here).
    QMenu *menu = picker->addButton()->menu();
    emit menu->aboutToShow();
    QAction *ink = nullptr;
    for (QAction *a : menu->actions()) {
        if (a->text() == QLatin1String("ink"))
            ink = a;
    }
    QVERIFY(ink && ink->isEnabled());
    ink->trigger();
    AiOptions *options = f.editor()->aiOptions();
    QList<ChosenLora> chosen = options->loras(f.provider->instanceId());
    QCOMPARE(chosen.size(), 1);
    QCOMPARE(chosen.first().path, QStringLiteral("styles/ink.safetensors"));
    // Its weight in the row, and words that call it up.
    chosen.first().triggerWords = {QStringLiteral("inkstyle")};
    options->setLoras(f.provider->instanceId(), chosen);
    QTRY_VERIFY(picker->findChild<QDoubleSpinBox *>(QStringLiteral("loraWeight")));
    auto *weight = picker->findChild<QDoubleSpinBox *>(QStringLiteral("loraWeight"));
    weight->setValue(0.6);
    emit weight->editingFinished();
    QCOMPARE(options->loras(f.provider->instanceId()).first().weight, 0.6);
    f.bar()->optionsPopup()->hide();

    f.words()->setPlainText(QStringLiteral("a lighthouse"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(f.strip()->count(), 2, 10000);
    QJsonObject sent = f.sd.last();
    const QJsonArray loras = sent.value(QStringLiteral("lora")).toArray();
    QCOMPARE(loras.size(), 1);
    QCOMPARE(loras.first().toObject().value(QStringLiteral("path")).toString(), QStringLiteral("styles/ink.safetensors"));
    QCOMPARE(loras.first().toObject().value(QStringLiteral("multiplier")).toDouble(), 0.6);
    QVERIFY2(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("inkstyle")), qPrintable(sent.value(QStringLiteral("prompt")).toString()));
    f.strip()->discard();

    // Unticked: not sent.
    auto *on = picker->findChild<QCheckBox *>(QStringLiteral("loraOn"));
    QVERIFY(on && on->isChecked());
    on->setChecked(false);
    QVERIFY(!options->loras(f.provider->instanceId()).first().on);
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(f.strip()->count(), 2, 10000);
    sent = f.sd.last();
    QVERIFY(!sent.contains(QStringLiteral("lora")));
    QVERIFY(!sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("inkstyle")));
    f.strip()->discard();

    // Kept for the engine in the next session.
    AiOptions next;
    QCOMPARE(next.loras(f.provider->instanceId()).size(), 1);
    QCOMPARE(next.loras(f.provider->instanceId()).first().weight, 0.6);
    QVERIFY(!next.loras(f.provider->instanceId()).first().on);
}

// A server Lumen runs: its LoRA folder is read without starting it, and the LoRAs are told apart by
// what they were made for. Only those for its model (FLUX.2 [klein] 4B) can be chosen; one for klein
// 9B chosen before is turned off with the reason; a server without a LoRA folder says so.
void TestCanvas::loraPickerFolder()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().remove(QStringLiteral("editor/ai/loras"));
    auto cleanup = qScopeGuard([] { Settings::instance().remove(QStringLiteral("editor/ai/loras")); });
    QTemporaryDir dir;
    auto put = [&](const QString &rel, const QByteArray &bytes) {
        const QString path = QDir::cleanPath(dir.filePath(rel));
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly))
            f.write(bytes);
        return path;
    };
    // FLUX blocks of a LoRA: `doubles` double and `singles` single blocks of `width`.
    auto fluxLora = [](int width, int doubles, int singles) {
        Tensors t;
        for (int b = 0; b < doubles; ++b) {
            t.append({QStringLiteral("diffusion_model.double_blocks.%1.img_attn.proj.lora_A.weight").arg(b), {16, width}});
            t.append({QStringLiteral("diffusion_model.double_blocks.%1.img_attn.proj.lora_B.weight").arg(b), {width, 16}});
        }
        for (int b = 0; b < singles; ++b)
            t.append({QStringLiteral("diffusion_model.single_blocks.%1.linear2.lora_A.weight").arg(b), {16, width * 4}});
        return shapedSafetensors(t);
    };
    const QString klein = put(QStringLiteral("models/flux-2-klein-4b.safetensors"), shapedSafetensors(kleinTensors(7680)));
    put(QStringLiteral("loras/ink_klein4b.safetensors"), fluxLora(3072, 5, 20));
    put(QStringLiteral("loras/realism_klein9b.safetensors"), fluxLora(4096, 8, 24));
    put(QStringLiteral("loras/sdxl/pixel.safetensors"),
        shapedSafetensors({{QStringLiteral("lora_unet_input_blocks_4_1_transformer_blocks_1_attn1_to_q.lora_down.weight"), {32, 640}}}));
    // Not a LoRA: left out.
    put(QStringLiteral("loras/stray-vae.safetensors"), shapedSafetensors(vaeTensors(32)));

    // lorasInFolder: the folder as sd-server lists it, with each file's model.
    const QList<LoraChoice> found = lorasInFolder(dir.filePath(QStringLiteral("loras")), QStringLiteral("FLUX.2"), QStringLiteral("Klein 4B"));
    QCOMPARE(found.size(), 3);
    QCOMPARE(found.at(0).name, QStringLiteral("ink_klein4b"));
    QVERIFY(found.at(0).mismatch.isEmpty());
    // Then the others by name.
    QCOMPARE(found.at(1).path, QStringLiteral("sdxl/pixel.safetensors"));
    QCOMPARE(found.at(1).mismatch, QStringLiteral("Made for SDXL; this engine runs FLUX.2 [klein] 4B."));
    QCOMPARE(found.at(2).path, QStringLiteral("realism_klein9b.safetensors"));
    QCOMPARE(found.at(2).mismatch, QStringLiteral("Made for FLUX.2 [klein] 9B; this engine runs FLUX.2 [klein] 4B."));

    Studio studio;
    runtime::InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/lora-sdcpp");
    rt.kind = QStringLiteral("sd.cpp");
    rt.executable = dir.filePath(QStringLiteral("sd-server.exe"));
    studio.runtimes()->addInstalled(rt);
    auto removeRuntime = qScopeGuard([&] { studio.runtimes()->remove(rt.id); });
    runtime::ServerProfile profile = studio.servers()->createProfile(rt.id, QStringLiteral("Klein"));
    profile.options.insert(QStringLiteral("diffusion_model"), QDir::toNativeSeparators(klein));
    studio.servers()->addProfile(profile);
    auto removeProfile = qScopeGuard([&] { studio.servers()->removeProfile(profile.id); });
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    auto *provider = qobject_cast<ai::ImageProvider *>(
        studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Klein"), {{QStringLiteral("managed_server"), profile.id}}));
    QVERIFY(provider);
    auto removeProvider = qScopeGuard([&] { studio.providers()->removeProvider(provider->instanceId()); });

    // No LoRA folder yet: the reason, and the way to Settings → Local servers.
    AiOptions options;
    LoraPicker picker(&studio, &options);
    QSignalSpy listed(&picker, &LoraPicker::listed);
    picker.setEngine(provider);
    QTRY_COMPARE(listed.size(), 1);
    QCOMPARE(picker.listing().problem, QStringLiteral("This server has no LoRA folder. Choose one in Settings → Local servers."));
    QCOMPARE(picker.listing().model, QStringLiteral("FLUX.2 [klein] 4B"));
    QVERIFY(!picker.addButton()->isEnabled());
    QSignalSpy settings(&studio, &Studio::settingsRequested);
    auto *serverSettings = picker.findChild<QPushButton *>(QStringLiteral("loraServerSettings"));
    QVERIFY(serverSettings && !serverSettings->isHidden());
    serverSettings->click();
    QCOMPARE(settings.size(), 1);
    QCOMPARE(settings.first().first().toString(), QStringLiteral("servers"));

    // With the folder, and a 9B LoRA chosen before: it is turned off and says why.
    profile.options.insert(QStringLiteral("lora_dir"), QDir::toNativeSeparators(dir.filePath(QStringLiteral("loras"))));
    studio.servers()->updateProfile(profile);
    ChosenLora old;
    old.name = QStringLiteral("realism_klein9b");
    old.path = QStringLiteral("realism_klein9b.safetensors");
    options.setLoras(provider->instanceId(), {old});
    picker.refresh();
    QTRY_COMPARE(listed.size(), 2);
    QCOMPARE(picker.listing().loras.size(), 3);
    QVERIFY(picker.listing().problem.isEmpty());
    QVERIFY(!options.loras(provider->instanceId()).first().on);
    QVERIFY(options.loraRefs(provider->instanceId()).isEmpty());
    auto *on = picker.findChild<QCheckBox *>(QStringLiteral("loraOn"));
    QVERIFY(on && !on->isChecked() && !on->isEnabled());
    auto *reason = picker.findChild<QLabel *>(QStringLiteral("loraReason"));
    QVERIFY(reason);
    QCOMPARE(reason->text(), QStringLiteral("Made for FLUX.2 [klein] 9B; this engine runs FLUX.2 [klein] 4B."));
    // The menu: the 4B one to choose, the others under "Made for another model", greyed out.
    QMenu *menu = picker.addButton()->menu();
    emit menu->aboutToShow();
    QStringList enabled;
    QStringList greyed;
    bool section = false;
    for (QAction *a : menu->actions()) {
        if (a->isSeparator()) {
            section = section || a->text() == QLatin1String("Made for another model");
            continue;
        }
        (a->isEnabled() ? enabled : greyed) << a->text();
    }
    QVERIFY(section);
    QCOMPARE(enabled, QStringList{QStringLiteral("ink_klein4b")});
    QCOMPARE(greyed, (QStringList{QStringLiteral("pixel  ·  SDXL"), QStringLiteral("realism_klein9b  ·  FLUX.2 [klein] 9B")}));
    for (QAction *a : menu->actions()) {
        if (a->text() == QLatin1String("ink_klein4b"))
            a->trigger();
    }
    const QList<ai::LoraRef> refs = options.loraRefs(provider->instanceId());
    QCOMPARE(refs.size(), 1);
    QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(refs.first().file)), QDir::cleanPath(dir.filePath(QStringLiteral("loras/ink_klein4b.safetensors"))));
    // The server was never started to list them.
    QCOMPARE(studio.servers()->state(profile.id), runtime::ServerState::Stopped);
}

// The settings of a run follow the model type (plan M18 §18.4): FLUX.2 [klein] 4B (distilled) offers
// its steps and sampler and says why CFG is not set; an SDXL-class engine offers CFG and CLIP skip;
// FLUX.1 dev its guidance. What is set goes with the request, is kept per model type, and the AI tab
// shows the same values.
void TestCanvas::runSettingsPerModel()
{
    Settings::instance().remove(QStringLiteral("editor/ai/settings"));
    auto cleanup = qScopeGuard([] { Settings::instance().remove(QStringLiteral("editor/ai/settings")); });
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    doc::Document *doc = f.studio.newDocument(QSize(512, 512), QStringLiteral("Blank"));
    QVERIFY(doc);
    QTRY_VERIFY(f.bar()->plan().action == PromptAction::NewImage);
    QWidget *popup = f.bar()->optionsPopup();
    f.bar()->findChild<QToolButton *>(QStringLiteral("promptMore"))->click();
    QTRY_VERIFY(popup->isVisible());
    RunSettingsEditor *settings = f.bar()->runSettings();
    QVERIFY(settings && !settings->isVisibleTo(popup)); // under Advanced
    popup->findChild<QToolButton *>(QStringLiteral("promptAdvanced"))->click();
    QVERIFY(settings->isVisibleTo(popup));
    auto byName = [](QWidget *root, const QString &name) -> QWidget * {
        for (QWidget *w : root->findChildren<QWidget *>()) {
            if (w->accessibleName() == name)
                return w;
        }
        return nullptr;
    };
    QCOMPARE(settings->spec().key, QStringLiteral("preset:flux2-klein-4b"));
    auto *steps = qobject_cast<QSpinBox *>(byName(settings, QStringLiteral("Steps")));
    QVERIFY(steps && steps->isVisibleTo(popup));
    QCOMPARE(steps->specialValueText(), QStringLiteral("Model's choice (4)"));
    // Distilled: no CFG to set, and a note that says why.
    QVERIFY(!byName(settings, QStringLiteral("Follow the words"))->isVisibleTo(popup));
    QVERIFY(settings->findChild<QLabel *>(QStringLiteral("runSettingsDistilled"))->isVisibleTo(popup));
    QVERIFY(!byName(settings, QStringLiteral("CLIP skip"))->isVisibleTo(popup));
    QVERIFY(!byName(settings, QStringLiteral("Guidance"))->isVisibleTo(popup));
    // The sampler list, though the server lists none (a server Lumen runs is not started for it).
    auto *sampler = qobject_cast<QComboBox *>(byName(settings, QStringLiteral("Sampler")));
    QVERIFY(sampler && sampler->isVisibleTo(popup));
    QCOMPARE(sampler->itemText(0), QStringLiteral("Model's choice (euler)"));
    QTRY_VERIFY(sampler->findData(QStringLiteral("euler_a")) > 0);
    steps->setValue(6);
    sampler->setCurrentIndex(sampler->findData(QStringLiteral("euler_a")));
    emit sampler->activated(sampler->currentIndex());
    AiOptions *options = f.editor()->aiOptions();
    QCOMPARE(options->runSettings(QStringLiteral("preset:flux2-klein-4b")).steps, 6);
    QCOMPARE(options->runSettings(QStringLiteral("preset:flux2-klein-4b")).sampler, QStringLiteral("euler_a"));
    // The AI tab shows the same.
    auto *panelSteps = qobject_cast<QSpinBox *>(byName(f.editor()->aiPanel(), QStringLiteral("Steps")));
    QVERIFY(panelSteps);
    QCOMPARE(panelSteps->value(), 6);
    popup->hide();

    f.words()->setPlainText(QStringLiteral("a lighthouse"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(f.strip()->count(), 2, 10000);
    QJsonObject sample = f.sd.last().value(QStringLiteral("sample_params")).toObject();
    QCOMPARE(sample.value(QStringLiteral("sample_steps")).toInt(), 6);
    QCOMPARE(sample.value(QStringLiteral("sample_method")).toString(), QStringLiteral("euler_a"));
    QCOMPARE(sample.value(QStringLiteral("guidance")).toObject().value(QStringLiteral("txt_cfg")).toDouble(), 1.0);
    f.strip()->discard();
    // Kept for the model type in the next session; back to the model's choice.
    {
        AiOptions next;
        QCOMPARE(next.runSettings(QStringLiteral("preset:flux2-klein-4b")).steps, 6);
    }
    settings->findChild<QPushButton *>(QStringLiteral("runSettingsReset"))->click();
    QVERIFY(options->runSettings(QStringLiteral("preset:flux2-klein-4b")).isEmpty());
    QCOMPARE(steps->value(), 0);

    // An SDXL-class engine without a preset: CFG and CLIP skip, kept apart from klein's.
    auto *sdxl = qobject_cast<ai::SdCppProvider *>(f.studio.providers()->addProvider(
        QStringLiteral("sdcpp"), QStringLiteral("SDXL"), {{QStringLiteral("url"), f.sd.server.baseUrl().toString()}}));
    QVERIFY(sdxl);
    auto removeSdxl = qScopeGuard([&] { f.studio.providers()->removeProvider(sdxl->instanceId()); });
    RunSettingsEditor editor(options);
    editor.setEngine(sdxl);
    QCOMPARE(editor.spec().key, QStringLiteral("engine:") + sdxl->instanceId());
    QVERIFY(editor.spec().cfg && editor.spec().clipSkip && !editor.spec().distilled);
    auto *cfg = qobject_cast<QDoubleSpinBox *>(byName(&editor, QStringLiteral("Follow the words")));
    auto *clipSkip = qobject_cast<QSpinBox *>(byName(&editor, QStringLiteral("CLIP skip")));
    QVERIFY(cfg && clipSkip);
    QCOMPARE(cfg->specialValueText(), QStringLiteral("Model's choice (6)"));
    cfg->setValue(4.5);
    clipSkip->setValue(2);
    ai::ImageRequest r;
    const ai::RunSettingsSpec spec = ai::runSettingsFor(sdxl);
    ai::applyRunSettings(r, options->runSettings(spec.key), spec);
    QVERIFY(r.cfgChosen);
    QCOMPARE(r.cfg, 4.5);
    QCOMPARE(sdxl->buildPayload(r).value(QStringLiteral("clip_skip")).toInt(), 2);
    QVERIFY(options->runSettings(QStringLiteral("preset:flux2-klein-4b")).isEmpty());
    // A value set for a model type does not reach one that does not take it (klein: no CFG).
    ai::RunSettings forced;
    forced.cfg = 7;
    ai::ImageRequest kr;
    ai::applyRunSettings(kr, forced, ai::runSettingsFor(f.provider));
    QVERIFY(!kr.cfgChosen);

    // FLUX.1 dev: its guidance, the preset's value unless one is set.
    ai::ModelDefaults dev;
    dev.id = QStringLiteral("flux1-dev");
    dev.name = QStringLiteral("FLUX.1 [dev]");
    dev.cfg = 1.0;
    dev.guidance = 3.5;
    dev.steps = 20;
    auto *flux1 = qobject_cast<ai::SdCppProvider *>(f.studio.providers()->addProvider(
        QStringLiteral("sdcpp"), QStringLiteral("Dev"),
        {{QStringLiteral("url"), f.sd.server.baseUrl().toString()}, {QStringLiteral("model_preset"), dev.toVariant()}}));
    QVERIFY(flux1);
    auto removeFlux1 = qScopeGuard([&] { f.studio.providers()->removeProvider(flux1->instanceId()); });
    const ai::RunSettingsSpec devSpec = ai::runSettingsFor(flux1);
    QVERIFY(devSpec.guidance && devSpec.distilled && !devSpec.cfg && !devSpec.clipSkip);
    QCOMPARE(devSpec.modelGuidance, 3.5);
    ai::ImageRequest dr;
    QCOMPARE(flux1->buildPayload(dr).value(QStringLiteral("sample_params")).toObject().value(QStringLiteral("guidance")).toObject()
                 .value(QStringLiteral("distilled_guidance")).toDouble(),
             3.5);
    ai::RunSettings devSettings;
    devSettings.guidance = 2.5;
    ai::applyRunSettings(dr, devSettings, devSpec);
    QCOMPARE(flux1->buildPayload(dr).value(QStringLiteral("sample_params")).toObject().value(QStringLiteral("guidance")).toObject()
                 .value(QStringLiteral("distilled_guidance")).toDouble(),
             2.5);
    QCOMPARE(dr.describe().value(QStringLiteral("guidance")).toDouble(), 2.5);
}

// Select → By description and "select: …" in the prompt bar (plan M18 §18.1): the black lines are
// found but not a large dark block, Select is one undo step and can add to a selection, words only a
// model could find say so, and the prompt bar selects without asking the engine.
void TestCanvas::selectByDescription()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    QImage page(400, 300, QImage::Format_ARGB32_Premultiplied);
    page.fill(QColor(235, 235, 230));
    {
        QPainter p(&page);
        p.fillRect(QRect(20, 40, 360, 3), QColor(20, 20, 20));    // a pen line
        p.fillRect(QRect(240, 150, 120, 120), QColor(20, 20, 20)); // a dark block
    }
    doc::Document *doc = f.studio.openImageAsDocument(page, QStringLiteral("Scan"));
    QVERIFY(doc);
    QVERIFY(f.window->findChild<QAction *>(QStringLiteral("editor-select-by-description")));

    SelectByDescriptionDialog dlg(doc, f.window.get());
    QSignalSpy found(&dlg, &SelectByDescriptionDialog::foundChanged);
    dlg.setWords(QStringLiteral("the black lines"));
    QTRY_VERIFY(found.size() >= 1 && !dlg.found().isNull());
    QCOMPARE(dlg.request().kind, img::FinderRequest::Kind::Strokes);
    QCOMPARE(dlg.findChild<QLabel *>(QStringLiteral("selectUnderstood"))->text(),
             QStringLiteral("Lines darker than what is around them, up to 6 px wide."));
    QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("selectResult"))->text().startsWith(QStringLiteral("Found ")));
    const int undoBefore = doc->undoStack()->count();
    QVERIFY(dlg.apply());
    QVERIFY(doc->hasSelection());
    QCOMPARE(qGray(doc->selection().pixel(200, 41)), 255);
    QCOMPARE(qGray(doc->selection().pixel(300, 210)), 0);
    QCOMPARE(doc->undoStack()->count(), undoBefore + 1);
    doc->undoStack()->undo();
    QVERIFY(!doc->hasSelection());

    // Add: what is found joins a rectangle that was selected.
    QImage rect = img::createMask(doc->size());
    {
        QPainter p(&rect);
        p.fillRect(QRect(10, 200, 40, 40), Qt::white);
    }
    doc->applySelection(rect, QStringLiteral("Rectangle"));
    for (QRadioButton *b : dlg.findChildren<QRadioButton *>()) {
        if (b->text() == QLatin1String("Add"))
            b->setChecked(true);
    }
    QVERIFY(dlg.apply());
    QCOMPARE(qGray(doc->selection().pixel(200, 41)), 255);
    QCOMPARE(qGray(doc->selection().pixel(30, 220)), 255);
    doc->clearSelection();

    // Words only a model could find: what can be found instead, and nothing to select.
    dlg.setWords(QStringLiteral("the cup"));
    QTRY_COMPARE(dlg.request().kind, img::FinderRequest::Kind::None);
    QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("selectUnderstood"))->text().contains(QStringLiteral("detection model")));
    QVERIFY(!dlg.apply());

    // The prompt bar: "select: …" selects, clears the words and sends nothing to the engine.
    QCOMPARE(selectionWords(QStringLiteral("Select: the black lines")), QStringLiteral("the black lines"));
    QCOMPARE(selectionWords(QStringLiteral("auswählen: Staub")), QStringLiteral("Staub"));
    QVERIFY(selectionWords(QStringLiteral("selected areas look nice")).isEmpty());
    f.words()->setPlainText(QStringLiteral("select: the black lines"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_VERIFY(doc->hasSelection());
    QCOMPARE(qGray(doc->selection().pixel(200, 41)), 255);
    QCOMPARE(qGray(doc->selection().pixel(300, 210)), 0);
    QTRY_VERIFY(f.words()->toPlainText().isEmpty());
    QVERIFY(f.sd.generations.isEmpty());
}

namespace {

// Stands in for SAM 2: a disk of radius 30 around each point on the object, less a disk of 15
// around each point off it; a box is filled.
struct FakeSegmenter : ai::Segmenter
{
    int calls = 0;
    QImage segment(const QImage &picture, const ai::SegmentPrompt &prompt, QString *) override
    {
        ++calls;
        QImage mask = img::createMask(picture.size());
        QPainter p(&mask);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        if (!prompt.box.isNull())
            p.fillRect(prompt.box, Qt::white);
        for (int i = 0; i < prompt.points.size(); ++i) {
            p.setBrush(prompt.positive.value(i) ? Qt::white : Qt::black);
            const double r = prompt.positive.value(i) ? 30 : 15;
            p.drawEllipse(prompt.points.at(i), r, r);
        }
        return mask;
    }
};

} // namespace

// Smart selection (plan M18 §18.1): the first use asks before downloading; then a click selects the
// object, Shift+click and Alt+click refine it, a drag selects what is in the box, Add keeps the
// selection there was, and each is one undo step.
void TestCanvas::smartSelection()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    doc::Document *doc = f.studio.openImageAsDocument(detailedPhoto(QSize(400, 300)), QStringLiteral("Photo"));
    QVERIFY(doc);
    EditorPage *editor = f.editor();
    CanvasView *canvas = editor->canvas();
    editor->selectTool(QStringLiteral("smart-select"));
    auto *tool = qobject_cast<SmartSelectTool *>(canvas->tool());
    QVERIFY(tool);
    QTRY_VERIFY(canvas->isFitted());
    QWidget *surface = canvas->focusProxy();
    auto click = [&](const QPointF &at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QPoint p = canvas->mapFromDoc(at).toPoint();
        QTest::mousePress(surface, Qt::LeftButton, mods, p);
        QTest::mouseRelease(surface, Qt::LeftButton, mods, p);
    };
    auto drag = [&](const QRectF &r) {
        const QPoint from = canvas->mapFromDoc(r.topLeft()).toPoint();
        const QPoint to = canvas->mapFromDoc(r.bottomRight()).toPoint();
        QTest::mousePress(surface, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(surface, (from + to) / 2);
        QTest::mouseMove(surface, to);
        QTest::mouseRelease(surface, Qt::LeftButton, Qt::NoModifier, to);
    };
    auto at = [doc](int x, int y) { return qGray(doc->selection().pixel(x, y)); };

    // Not downloaded: it asks, says how much, and Cancel downloads nothing.
    runtime::ComponentSpec fakeRuntime;
    fakeRuntime.id = QStringLiteral("onnxruntime");
    fakeRuntime.name = QStringLiteral("ONNX Runtime");
    fakeRuntime.files = {runtime::ComponentFile{QUrl(QStringLiteral("http://127.0.0.1:9/ort.zip")), {}, 80000000, QStringLiteral("onnxruntime.dll"), {}}};
    runtime::ComponentSpec fakeSam = fakeRuntime;
    fakeSam.id = QStringLiteral("sam2.1-tiny");
    fakeSam.files.first().size = 62000000;
    fakeSam.files.first().path = QStringLiteral("model.onnx");
    f.studio.components()->setSpecs({fakeRuntime, fakeSam});
    QVERIFY(!f.studio.smartSelect()->isReady());
    click(QPointF(100, 100));
    QMessageBox *ask = nullptr;
    QTRY_VERIFY((ask = f.window->findChild<QMessageBox *>(QStringLiteral("smartSelectDownload"))));
    QVERIFY(ask->text().contains(QStringLiteral("142 MB")));
    ask->button(QMessageBox::Cancel)->click();
    QVERIFY(!f.studio.smartSelect()->isInstalling());
    QVERIFY(!doc->hasSelection());

    // Ready: a click selects the object.
    auto fake = std::make_shared<FakeSegmenter>();
    f.studio.smartSelect()->setSegmenter(fake);
    QSignalSpy selected(tool, &SmartSelectTool::selected);
    const int undoBefore = doc->undoStack()->count();
    click(QPointF(100, 100));
    QTRY_COMPARE(selected.size(), 1);
    QCOMPARE(at(100, 100), 255);
    QCOMPARE(at(200, 100), 0);
    QCOMPARE(doc->undoStack()->count(), undoBefore + 1);
    // Shift+click: another point on the same object.
    click(QPointF(150, 100), Qt::ShiftModifier);
    QTRY_COMPARE(selected.size(), 2);
    QCOMPARE(tool->prompt().points.size(), 2);
    QCOMPARE(at(175, 100), 255);
    QCOMPARE(at(100, 100), 255);
    // Alt+click: a point that is not part of it.
    click(QPointF(120, 100), Qt::AltModifier);
    QTRY_COMPARE(selected.size(), 3);
    QCOMPARE(tool->prompt().positive.last(), false);
    QCOMPARE(at(120, 100), 0);
    // A drag: what is in the box, as a new object.
    drag(QRectF(250, 150, 100, 80));
    QTRY_COMPARE(selected.size(), 4);
    QCOMPARE(tool->prompt().points.size(), 0);
    QCOMPARE(at(300, 190), 255);
    QCOMPARE(at(100, 100), 0);
    // Add (the options bar): the box stays and a click's object joins it.
    auto *add = editor->findChild<QToolButton *>(QStringLiteral("selectionAdd"));
    QVERIFY(add);
    add->click();
    click(QPointF(60, 60));
    QTRY_COMPARE(selected.size(), 5);
    QCOMPARE(at(60, 60), 255);
    QCOMPARE(at(300, 190), 255);
    editor->findChild<QToolButton *>(QStringLiteral("selectionNew"))->click();
    doc->undoStack()->undo();
    QCOMPARE(at(60, 60), 0);
    QCOMPARE(at(300, 190), 255);
}

// Objects by name (plan M18 §18.1): the dialog offers the detection models' download while they are
// missing; with them, "the ball" is found and selected, "a giraffe" is not; the prompt bar's
// "select: the ball" and the filter step "Find area" do the same.
void TestCanvas::selectObjectsByName()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    QImage page(400, 300, QImage::Format_ARGB32_Premultiplied);
    page.fill(QColor(150, 150, 145));
    {
        QPainter p(&page);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 40, 40));
        p.drawEllipse(QPoint(120, 150), 50, 50);
    }
    doc::Document *doc = f.studio.openImageAsDocument(page, QStringLiteral("Ball"));
    QVERIFY(doc);
    SmartSelect *finder = f.studio.smartSelect();

    // Missing: the dialog says so, with the size, and offers the download.
    runtime::ComponentSpec part;
    part.files = {runtime::ComponentFile{QUrl(QStringLiteral("http://127.0.0.1:9/x")), {}, 50000000, QStringLiteral("x"), {}}};
    QList<runtime::ComponentSpec> specs;
    for (const QString &id : SmartSelect::objectComponentIds()) {
        runtime::ComponentSpec c = part;
        c.id = id;
        c.name = id;
        specs << c;
    }
    f.studio.components()->setSpecs(specs);
    QVERIFY(!finder->objectsReady());
    {
        SelectByDescriptionDialog dlg(doc, f.window.get());
        dlg.setObjectFinder(finder);
        dlg.setWords(QStringLiteral("the ball"));
        QVERIFY(dlg.findsObjects());
        auto *download = dlg.findChild<QPushButton *>(QStringLiteral("selectDownload"));
        QVERIFY(download && !download->isHidden());
        QCOMPARE(download->text(), QStringLiteral("Download the detection models (about 150 MB, once)"));
        QVERIFY(!dlg.apply());
    }
    filters::NodeTypePtr node = filters::NodeRegistry::global().find(QStringLiteral("find-area"));
    QVERIFY(node && node->info().expensive);
    QVERIFY(node->unavailableReason({}).contains(QStringLiteral("150 MB")));

    // Without the models, an image engine that finds objects does it (Gemini, against a mock).
    {
        MockHttpServer gemini;
        int asked = 0;
        gemini.route("POST", QStringLiteral("/v1beta/models/gemini-2.5-flash:generateContent"), [&](const MockRequest &) {
            ++asked;
            // The ball's box, 70-170 across 400 and 100-200 down 300, in thousandths.
            const QString list = QStringLiteral("[{\"box_2d\": [333, 175, 667, 425], \"label\": \"ball\"}]");
            return MockResponse::json({{QStringLiteral("candidates"), QJsonArray{QJsonObject{{QStringLiteral("content"), QJsonObject{
                {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("text"), list}}}}}}}}}});
        });
        ai::Provider *engine = f.studio.providers()->addProvider(
            QStringLiteral("gemini_image"), QStringLiteral("Gemini"),
            {{QStringLiteral("url"), gemini.baseUrl().toString() + QStringLiteral("/v1beta")}, {QStringLiteral("api_key"), QStringLiteral("k")}});
        QVERIFY(engine);
        QVERIFY(!finder->objectsReady());
        QVERIFY(finder->canFindObjects());
        QVERIFY(node->unavailableReason({}).isEmpty());
        {
            SelectByDescriptionDialog dlg(doc, f.window.get());
            dlg.setObjectFinder(finder);
            dlg.setWords(QStringLiteral("the ball"));
            QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("selectUnderstood"))->text().contains(QStringLiteral("found by Gemini")));
            QVERIFY(!dlg.findChild<QPushButton *>(QStringLiteral("selectDownload"))->isHidden()); // still offered, to find them here
            QTRY_VERIFY_WITH_TIMEOUT(!dlg.found().isNull(), 10000);
            QCOMPARE(dlg.findChild<QLabel *>(QStringLiteral("selectResult"))->text(), QStringLiteral("Found by Gemini (tinted)."));
            QVERIFY(dlg.apply());
            QCOMPARE(qGray(doc->selection().pixel(120, 150)), 255);
            QCOMPARE(qGray(doc->selection().pixel(350, 50)), 0);
            doc->clearSelection();
        }
        QCOMPARE(asked, 1);
        f.studio.providers()->removeProvider(engine->instanceId());
        QVERIFY(!finder->canFindObjects());
    }

    // With the models (stand-ins): a box around what the words name, cut out as a disk.
    auto segmenter = std::make_shared<FakeSegmenter>();
    finder->setSegmenter(segmenter);
    finder->setDetector([](const QImage &, const QString &words) {
        QList<ai::Detection> found;
        if (words.contains(QStringLiteral("ball")))
            found << ai::Detection{QRectF(70, 100, 100, 100), 0.9f};
        return found;
    });
    QVERIFY(finder->objectsReady());
    QVERIFY(node->unavailableReason({}).isEmpty());
    {
        SelectByDescriptionDialog dlg(doc, f.window.get());
        dlg.setObjectFinder(finder);
        QSignalSpy found(&dlg, &SelectByDescriptionDialog::foundChanged);
        dlg.setWords(QStringLiteral("the ball"));
        QTRY_VERIFY(found.size() >= 1 && !dlg.found().isNull());
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("selectResult"))->text().startsWith(QStringLiteral("Found 1 object")));
        QVERIFY(dlg.apply());
        QCOMPARE(qGray(doc->selection().pixel(120, 150)), 255);
        QCOMPARE(qGray(doc->selection().pixel(350, 50)), 0);
        doc->clearSelection();
        dlg.setWords(QStringLiteral("a giraffe"));
        QTRY_VERIFY2(dlg.findChild<QLabel *>(QStringLiteral("selectResult"))->text().contains(QStringLiteral("Nothing that looks like")),
                     qPrintable(dlg.findChild<QLabel *>(QStringLiteral("selectResult"))->text()));
        QVERIFY(!dlg.apply());
    }

    // The prompt bar.
    f.words()->setPlainText(QStringLiteral("select: the ball"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_VERIFY(doc->hasSelection());
    QCOMPARE(qGray(doc->selection().pixel(120, 150)), 255);
    QTRY_VERIFY(f.words()->toPlainText().isEmpty());
    QVERIFY(f.sd.generations.isEmpty());

    // The filter step, evaluated as a filter's worker thread does.
    QFuture<filters::Values> out = QtConcurrent::run([node, page] {
        return filters::evaluateNode(*node, {{QStringLiteral("image"), page}}, {{QStringLiteral("what"), QStringLiteral("the ball")}});
    });
    QTRY_VERIFY_WITH_TIMEOUT(out.isFinished(), 10000);
    const QImage mask = out.result().value(QStringLiteral("mask")).value<QImage>();
    QCOMPARE(mask.size(), page.size());
    QCOMPARE(qGray(mask.pixel(120, 150)), 255);
    QCOMPARE(qGray(mask.pixel(350, 50)), 0);
}

// An engine without instruction edits (sd.cpp with a model it has no preset for): without a
// selection the bar says it cannot change the whole image from words and offers a variation (img2img
// at the variation strength) or selecting an area; with a selection it inpaints the area at the area
// strength, and Remove paints it in from the surroundings at full strength.
void TestCanvas::promptBarWithoutEdits()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(false));
    EditorPage *editor = f.editor();
    PromptBar *bar = f.bar();
    VariantStrip *strip = f.strip();
    QPushButton *generate = f.button(QStringLiteral("promptGenerate"));
    QPushButton *remove = f.button(QStringLiteral("promptRemove"));
    QPushButton *selectArea = f.button(QStringLiteral("promptSelectArea"));
    QVERIFY(generate && remove && selectArea);
    doc::Document *doc = f.studio.openImageAsDocument(detailedPhoto(QSize(800, 600)), QStringLiteral("Photo"));
    QTRY_VERIFY(bar->plan().action == PromptAction::CannotEditImage);
    QTRY_VERIFY2(bar->hint().contains(QStringLiteral("cannot change the whole picture from words")), qPrintable(bar->hint()));
    QVERIFY(selectArea->isVisible());
    // The main button says what it does instead, and needs no words.
    QCOMPARE(generate->text(), QStringLiteral("Make a variation"));
    QVERIFY(generate->isEnabled());
    f.words()->setPlainText(QStringLiteral("evening light"));
    QTRY_VERIFY(generate->isEnabled());

    // How much a variation may change is in the ⋯ menu, in per cent.
    bar->showOptions();
    QWidget *popup = bar->optionsPopup();
    QVERIFY(popup->isVisible());
    auto *strength = popup->findChild<SliderField *>();
    QVERIFY(strength && strength->isVisibleTo(popup));
    QCOMPARE(strength->value(), 45.0);
    strength->findChild<QDoubleSpinBox *>()->setValue(30);
    QCOMPARE(editor->aiOptions()->variationStrength(), 0.3);
    QCOMPARE(editor->aiOptions()->regionStrength(), 0.75);
    // This model takes a negative prompt.
    AutoTextEdit *negative = nullptr;
    for (AutoTextEdit *e : popup->findChildren<AutoTextEdit *>()) {
        if (e->accessibleName() == QLatin1String("Negative prompt"))
            negative = e;
    }
    QVERIFY(negative && negative->isVisibleTo(popup));
    popup->hide();

    // Enter in the words makes it, as the button does.
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QJsonObject sent = f.sd.last();
    QVERIFY(sent.contains(QStringLiteral("init_image")) && !sent.contains(QStringLiteral("mask_image")));
    QCOMPARE(sent.value(QStringLiteral("strength")).toDouble(), 0.3);
    QCOMPARE(QSize(sent.value(QStringLiteral("width")).toInt(), sent.value(QStringLiteral("height")).toInt()),
             workingSize(doc->size(), f.provider->capabilities()));
    QVERIFY(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("evening light")));
    QCOMPARE(strip->currentName(), QStringLiteral("evening light"));
    strip->discard();

    // "Select an area" picks the rectangle selection tool.
    selectArea->click();
    QCOMPARE(editor->canvas()->tool()->id(), QStringLiteral("select-rect"));

    // With a selection: inpainting of the area at the area strength, masked to it.
    const QRect area(250, 200, 200, 160);
    QImage mask = img::createMask(doc->size());
    QPainter(&mask).fillRect(area, Qt::white);
    doc->applySelection(mask, QStringLiteral("Select"));
    QTRY_VERIFY(bar->plan().action == PromptAction::InpaintArea);
    QTRY_VERIFY2(bar->hint().contains(QStringLiteral("Describe what should be there")), qPrintable(bar->hint()));
    QCOMPARE(generate->text(), QStringLiteral("Generate"));
    QVERIFY(!selectArea->isVisible());
    f.words()->setPlainText(QStringLiteral("a red scarf"));
    QTRY_VERIFY(generate->isEnabled());
    const QImage before = doc->flattened();
    generate->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    sent = f.sd.last();
    QVERIFY(sent.contains(QStringLiteral("init_image")) && sent.contains(QStringLiteral("mask_image")));
    QCOMPARE(sent.value(QStringLiteral("strength")).toDouble(), 0.75);
    QVERIFY(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("a red scarf")));
    QTest::keyClick(strip, Qt::Key_Return);
    const doc::LayerPtr scarf = doc->layers().last();
    QCOMPARE(scarf->props.name, QStringLiteral("a red scarf"));
    const auto [outside, changed] = changesOutside(scarf->image, featherReach(area), before, doc->flattened());
    QCOMPARE(outside, 0);
    QCOMPARE(changed, 0);

    // An instruction for the area: this model fills areas, so it gets what should be there (plan
    // M18 §18.2), and each result's tooltip says what it got; Use my words as they are sends them.
    f.words()->setPlainText(QStringLiteral("replace the scarf with a blue hat"));
    QTRY_VERIFY(generate->isEnabled());
    generate->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    sent = f.sd.last();
    const QString fitted = sent.value(QStringLiteral("prompt")).toString();
    QVERIFY2(fitted.endsWith(QStringLiteral("a blue hat")) && !fitted.contains(QStringLiteral("replace")), qPrintable(fitted));
    QToolButton *first = nullptr;
    for (QToolButton *b : strip->findChildren<QToolButton *>()) {
        if (b->accessibleName() == QLatin1String("Result 1") && b->isVisibleTo(strip))
            first = b;
    }
    QVERIFY(first);
    QVERIFY2(first->toolTip().contains(QStringLiteral("The model got: ") + fitted), qPrintable(first->toolTip()));
    strip->discard();
    bar->showOptions();
    auto *literal = bar->optionsPopup()->findChild<QCheckBox *>(QStringLiteral("promptLiteralWords"));
    QVERIFY(literal && !literal->isChecked());
    literal->setChecked(true);
    QVERIFY(editor->aiOptions()->literalWords());
    bar->optionsPopup()->hide();
    generate->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    sent = f.sd.last();
    QCOMPARE(sent.value(QStringLiteral("prompt")).toString(), QStringLiteral("replace the scarf with a blue hat"));
    strip->discard();
    editor->aiOptions()->setLiteralWords(false);

    // Remove: the area painted in from its surroundings, at full strength.
    const int layers = doc->layerCount();
    remove->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    sent = f.sd.last();
    QVERIFY(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("empty background, seamless continuation of the surroundings")));
    QVERIFY(sent.contains(QStringLiteral("mask_image")));
    QCOMPARE(sent.value(QStringLiteral("strength")).toDouble(), 1.0);
    QTest::keyClick(strip, Qt::Key_Return);
    QCOMPARE(doc->layerCount(), layers + 1);
    QCOMPARE(doc->layers().last()->props.name, QStringLiteral("Removed"));
}

// The AI menu has what is not "describe a change" and needs a document. The AI panel is hidden until
// asked for (All AI options…, Expand canvas…); it and the prompt bar's ⋯ menu share their options,
// and the menu's Variation goes to the results strip.
void TestCanvas::aiMenuAndPanel()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    EditorPage *editor = f.editor();
    PromptBar *bar = f.bar();
    const QStringList names{QStringLiteral("ai-upscale-2"), QStringLiteral("ai-upscale-4"), QStringLiteral("ai-remove-background"),
                            QStringLiteral("ai-expand"), QStringLiteral("ai-variation"), QStringLiteral("ai-all-options"),
                            QStringLiteral("editor-undo"), QStringLiteral("editor-select-all"), QStringLiteral("editor-zoom-in")};
    auto action = [&f](const QString &name) { return f.window->findChild<QAction *>(name); };
    for (const QString &name : names) {
        QVERIFY2(action(name), qPrintable(name));
        QVERIFY2(!action(name)->isEnabled(), qPrintable(name));
    }
    f.studio.openImageAsDocument(detailedPhoto(QSize(640, 480)), QStringLiteral("Photo"));
    // Upscale and Remove background also need an engine that can do them, which this sd.cpp server
    // (no upscaler) is not (editorMenus has the rest).
    const QStringList utilities{QStringLiteral("ai-upscale-2"), QStringLiteral("ai-upscale-4"), QStringLiteral("ai-remove-background")};
    for (const QString &name : names)
        QVERIFY2(action(name)->isEnabled() != utilities.contains(name), qPrintable(name));

    QVERIFY(!editor->isAiPanelShown());
    action(QStringLiteral("ai-all-options"))->trigger();
    QVERIFY(editor->isAiPanelShown());
    editor->hideAiPanel();
    QVERIFY(!editor->isAiPanelShown());
    action(QStringLiteral("ai-expand"))->trigger();
    QVERIFY(editor->isAiPanelShown());
    QCOMPARE(editor->aiPanel()->mode(), AiPanel::Mode::Expand);

    // The ⋯ menu and the AI panel show the same options.
    auto spin = [](QWidget *root, const QString &name) -> QSpinBox * {
        for (QSpinBox *s : root->findChildren<QSpinBox *>()) {
            if (s->accessibleName() == name)
                return s;
        }
        return nullptr;
    };
    bar->showOptions();
    QWidget *popup = bar->optionsPopup();
    QVERIFY(spin(popup, QStringLiteral("Results per run")) && spin(editor->aiPanel(), QStringLiteral("Results per run")));
    spin(popup, QStringLiteral("Results per run"))->setValue(3);
    QCOMPARE(editor->aiOptions()->variants(), 3);
    QCOMPARE(spin(editor->aiPanel(), QStringLiteral("Results per run"))->value(), 3);
    spin(editor->aiPanel(), QStringLiteral("Steps"))->setValue(8);
    QCOMPARE(spin(popup, QStringLiteral("Steps"))->value(), 8);
    // By the same names, in per cent as in ⋯: how much a variation and a selected area may change.
    auto percent = [&editor](const QString &name) -> QDoubleSpinBox * {
        for (QDoubleSpinBox *d : editor->aiPanel()->findChildren<QDoubleSpinBox *>()) {
            if (d->accessibleName() == name)
                return d;
        }
        return nullptr;
    };
    QVERIFY(percent(QStringLiteral("How much may change")) && percent(QStringLiteral("Area may change")));
    QCOMPARE(percent(QStringLiteral("How much may change"))->suffix(), QStringLiteral(" %"));
    QCOMPARE(percent(QStringLiteral("How much may change"))->value(), std::round(editor->aiOptions()->variationStrength() * 100));
    percent(QStringLiteral("Area may change"))->setValue(60);
    QCOMPARE(editor->aiOptions()->regionStrength(), 0.6);
    // Four tabs in the narrow right panel: whole names, or only icons (named in their tooltips),
    // never "Lay…".
    QTabWidget *side = nullptr;
    for (QTabWidget *t : editor->findChildren<QTabWidget *>()) {
        if (t->indexOf(editor->aiPanel()) >= 0)
            side = t;
    }
    QVERIFY(side);
    QTRY_VERIFY2(side->tabBar()->sizeHint().width() <= side->width(),
                 qPrintable(QStringLiteral("%1 > %2").arg(side->tabBar()->sizeHint().width()).arg(side->width())));
    for (int i = 0; i < side->count(); ++i) {
        if (side->isTabVisible(i) && side->widget(i) != editor->aiPanel())
            QVERIFY(!side->tabText(i).isEmpty() || !side->tabToolTip(i).isEmpty());
    }
    // Steps and the seed wait under Advanced.
    QVERIFY(!spin(popup, QStringLiteral("Steps"))->isVisibleTo(popup) && !spin(popup, QStringLiteral("Seed"))->isVisibleTo(popup));
    popup->findChild<QToolButton *>(QStringLiteral("promptAdvanced"))->click();
    QVERIFY(spin(popup, QStringLiteral("Steps"))->isVisibleTo(popup) && spin(popup, QStringLiteral("Seed"))->isVisibleTo(popup));
    auto *engine = popup->findChild<ProviderCombo *>(QStringLiteral("promptEngine"));
    QVERIFY(engine && engine->currentText() == QLatin1String("Klein"));
    // Only what applies: FLUX.2 [klein] takes no negative prompt and an edit uses no strength; it
    // takes reference images.
    for (AutoTextEdit *e : popup->findChildren<AutoTextEdit *>())
        QVERIFY(!e->isVisibleTo(popup));
    QVERIFY(!popup->findChild<SliderField *>()->isVisibleTo(popup));
    QPushButton *references = nullptr;
    for (QPushButton *b : popup->findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Add in the AI tab…"))
            references = b;
    }
    QVERIFY(references && references->isVisibleTo(popup));
    references->click(); // to the AI panel, where the references are
    QVERIFY(!popup->isVisible());
    QVERIFY(editor->isAiPanelShown());

    // The menu's Variation: the whole image redrawn at the variation strength with the bar's words,
    // three results at 8 steps.
    f.words()->setPlainText(QStringLiteral("in autumn colours"));
    action(QStringLiteral("ai-variation"))->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(f.strip()->count(), 3, 10000);
    const QJsonObject sent = f.sd.last();
    QVERIFY(sent.contains(QStringLiteral("init_image")));
    QCOMPARE(sent.value(QStringLiteral("strength")).toDouble(), 0.45);
    QCOMPARE(sent.value(QStringLiteral("batch_count")).toInt(), 3);
    QCOMPARE(sent.value(QStringLiteral("sample_params")).toObject().value(QStringLiteral("sample_steps")).toInt(), 8);
    QVERIFY(sent.value(QStringLiteral("prompt")).toString().contains(QStringLiteral("in autumn colours")));
    f.strip()->discard();
}

// Results that arrive while the user types the next words leave the keyboard in the words: what they
// type stays there, Enter is theirs (it runs the new words), and the strip's line says to click a
// result. From the canvas, the active tool has the first go at a key.
void TestCanvas::promptBarKeepsTyping()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    EditorPage *editor = f.editor();
    VariantStrip *strip = f.strip();
    AutoTextEdit *words = f.words();
    doc::Document *doc = f.studio.openImageAsDocument(detailedPhoto(), QStringLiteral("Photo"));
    QTRY_VERIFY(f.bar()->plan().action == PromptAction::EditImage);

    f.sd.hold = true;
    words->setPlainText(QStringLiteral("colorize it"));
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(strip->isRunning(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(f.sd.generations.size(), 1, 10000);
    // The next words while it runs.
    words->setFocus();
    words->moveCursor(QTextCursor::End);
    QTest::keyClicks(words, QStringLiteral(" and"));
    const QString tool = editor->canvas()->tool()->id();
    f.sd.hold = false;
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(QApplication::focusWidget(), words);
    QTest::keyClicks(words, QStringLiteral(" blue"));
    QCOMPARE(words->toPlainText(), QStringLiteral("colorize it and blue"));
    QCOMPARE(editor->canvas()->tool()->id(), tool); // no tool key went off
    QVERIFY2(strip->label().contains(QStringLiteral("click one to compare")), qPrintable(strip->label()));
    // Enter runs the new words (Generate again replaces the results), it does not keep one.
    const int layers = doc->layerCount();
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(f.sd.generations.size(), 2, 10000);
    QCOMPARE(f.sd.last().value(QStringLiteral("prompt")).toString(), QStringLiteral("colorize it and blue"));
    QCOMPARE(doc->layerCount(), layers);
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    // Clicking a result hands the keyboard to the strip, whose line then names the keys.
    strip->findChildren<QToolButton *>().first()->click();
    QCOMPARE(QApplication::focusWidget(), strip);
    QVERIFY2(strip->label().contains(QStringLiteral("←/→ to compare")), qPrintable(strip->label()));
    strip->discard();

    // On the canvas the tool comes first: the move tool's arrows nudge the layer, the selection tool
    // leaves them to the results.
    strip->addVariants(doc, {detailedPhoto(), detailedPhoto()}, QStringLiteral("Two"));
    QWidget *surface = editor->canvas()->focusProxy();
    editor->selectTool(QStringLiteral("move"));
    surface->setFocus();
    const QImage before = doc->layers().first()->image;
    QTest::keyClick(surface, Qt::Key_Right);
    QCOMPARE(strip->current(), 0);
    QVERIFY(doc->layers().first()->image != before);
    editor->selectTool(QStringLiteral("select-rect"));
    QTest::keyClick(surface, Qt::Key_Right);
    QCOMPARE(strip->current(), 1);
    // One result: no keys to compare with.
    strip->discard();
    strip->addVariants(doc, {detailedPhoto()}, QStringLiteral("One"));
    surface->setFocus();
    QCOMPARE(strip->label(), QStringLiteral("Keep it (Enter) or discard it (Esc)"));
    strip->discard();
}

// A run counts as running from the click: a second Enter does not start another. A run cancelled and
// retried from the Queue brings its results to the editor; an engine removed while it works ends the
// run with a message instead of keeping the bar busy.
void TestCanvas::promptBarRuns()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    VariantStrip *strip = f.strip();
    PromptBar *bar = f.bar();
    AutoTextEdit *words = f.words();
    QPushButton *generate = f.button(QStringLiteral("promptGenerate"));
    f.studio.openImageAsDocument(detailedPhoto(), QStringLiteral("Photo"));
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);

    f.sd.hold = true;
    words->setPlainText(QStringLiteral("make it night"));
    QTest::keyClick(words, Qt::Key_Return);
    QVERIFY(strip->isRunning()); // at once, before the jobs exist
    QTest::keyClick(words, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(f.sd.generations.size(), 1, 10000);
    QTest::qWait(150);
    QCOMPARE(f.sd.generations.size(), 1);
    int jobs = 0;
    for (Job *j : f.studio.jobs()->jobs())
        jobs += j->kind() == QLatin1String("generation") ? 1 : 0;
    QCOMPARE(jobs, 1);

    // Cancelled, then retried from the Queue: the results come to the strip.
    QTRY_COMPARE(generate->text(), QStringLiteral("Cancel"));
    generate->click();
    QTRY_VERIFY_WITH_TIMEOUT(!strip->isRunning(), 10000);
    Job *job = f.studio.jobs()->jobs().last();
    QTRY_COMPARE(job->state(), Job::State::Cancelled);
    QVERIFY(job->canRetry());
    f.sd.hold = false;
    f.studio.jobs()->retry(job);
    QVERIFY(strip->isRunning());
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QVERIFY(!strip->isRunning());
    strip->discard();

    // The engine removed while it works.
    f.sd.hold = true;
    generate->click();
    QTRY_COMPARE_WITH_TIMEOUT(f.sd.generations.size(), 3, 10000);
    QVERIFY(strip->isRunning());
    f.studio.providers()->removeProvider(f.provider->instanceId());
    f.provider = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(!strip->isRunning(), 5000);
    QCOMPARE(f.studio.jobs()->jobs().last()->state(), Job::State::Failed);
    QVERIFY(f.studio.jobs()->jobs().last()->errorString().contains(QStringLiteral("removed")));
    QTRY_COMPARE(generate->text(), QStringLiteral("Set up AI…")); // no engine left
    QVERIFY(generate->isEnabled());
}

// The engine nobody chose follows the one in use: the first one set up while the editor is open, then
// the new default. Choosing one in ⋯ pins it.
void TestCanvas::promptBarEngine()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Settings::instance().remove(QStringLiteral("editor/ai"));
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    auto cleanup = qScopeGuard([&] {
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    studio.openProject(dir.filePath(QStringLiteral("engine.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    EditorPage *editor = window.editor();
    AiOptions *options = editor->aiOptions();
    studio.openImageAsDocument(detailedPhoto(QSize(320, 240)), QStringLiteral("Photo"));
    PromptBar *bar = editor->promptBar();
    QTRY_VERIFY(bar->plan().action == PromptAction::NoEngine);

    const QVariantMap url{{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1")}};
    ai::Provider *a = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("A"), url);
    QVERIFY(a);
    QVERIFY(options->providerId(true).isEmpty()); // set up, not chosen
    QCOMPARE(options->provider(studio.providers(), true), a);
    ai::Provider *b = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("B"), url);
    studio.providers()->setDefaultImageProvider(b->instanceId());
    QCOMPARE(options->provider(studio.providers(), true), b);
    QCOMPARE(window.findChild<QToolButton *>(QStringLiteral("imageEngine"))->text(), QStringLiteral("B"));
    // The pickers show the engine the runs go to.
    bar->showOptions();
    auto *engine = bar->optionsPopup()->findChild<ProviderCombo *>(QStringLiteral("promptEngine"));
    QCOMPARE(engine->providerId(), b->instanceId());
    QCOMPARE(editor->aiPanel()->findChild<ProviderCombo *>()->providerId(), b->instanceId());
    // A choice of the user's pins it (on a photo: for changes); choosing the one in use again follows
    // the default.
    engine->setCurrentIndex(engine->findData(a->instanceId()));
    emit engine->activated(engine->currentIndex());
    QCOMPARE(options->providerId(true), a->instanceId());
    QVERIFY(options->providerId(false).isEmpty());
    QCOMPARE(editor->aiPanel()->findChild<ProviderCombo *>()->providerId(), a->instanceId());
    engine->setCurrentIndex(engine->findData(b->instanceId()));
    emit engine->activated(engine->currentIndex());
    QVERIFY(options->providerId(true).isEmpty());
    bar->optionsPopup()->hide();
}

// A change of the whole picture is made at the engine's size (about 1 MP); the result comes back at
// the picture's size with its fine detail where the engine kept its shapes, so a colorized photo is as
// sharp as it was.
void TestCanvas::wholeImageKeepsDetail()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    f.sd.echo = true;
    // Grey stripes and pixel noise: detail that scaling to 1 MP takes away.
    QImage photo(2400, 1600, QImage::Format_ARGB32_Premultiplied);
    quint32 seed = 7;
    for (int y = 0; y < photo.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(photo.scanLine(y));
        for (int x = 0; x < photo.width(); ++x) {
            seed = seed * 1664525u + 1013904223u;
            const int l = std::clamp(90 + (x / 3 % 2) * 50 + int(seed >> 26) - 32, 0, 255);
            line[x] = qRgb(l, l, l);
        }
    }
    doc::Document *doc = f.studio.openImageAsDocument(photo, QStringLiteral("Old photo"));
    QTRY_VERIFY(f.bar()->plan().action == PromptAction::EditImage);
    f.words()->setPlainText(QStringLiteral("colorize this photo"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(f.strip()->count(), 2, 20000);
    const QJsonObject sent = f.sd.last();
    QVERIFY(sent.value(QStringLiteral("width")).toInt() < 1400); // made smaller
    const QImage result = f.strip()->currentImage();
    QCOMPARE(result.size(), doc->size());
    // Its brightness is the photo's, pixel for pixel (the tint keeps it), and it has colour.
    double error = 0;
    for (int y = 0; y < photo.height(); y += 3) {
        for (int x = 0; x < photo.width(); x += 3)
            error += std::abs(qGray(result.pixel(x, y)) - qGray(photo.pixel(x, y)));
    }
    error /= double(photo.width() / 3) * (photo.height() / 3);
    QVERIFY2(error < 6.0, qPrintable(QString::number(error)));
    QVERIFY(qRed(result.pixel(1200, 800)) > qBlue(result.pixel(1200, 800)) + 10);
    // The engine's picture scaled up alone has lost the stripes and the noise.
    const QImage plain = img::scaled(MockSdCpp::tinted(img::scaled(photo, QSize(sent.value(QStringLiteral("width")).toInt(),
                                                                                sent.value(QStringLiteral("height")).toInt()))),
                                     photo.size());
    double plainError = 0;
    for (int y = 0; y < photo.height(); y += 3) {
        for (int x = 0; x < photo.width(); x += 3)
            plainError += std::abs(qGray(plain.pixel(x, y)) - qGray(photo.pixel(x, y)));
    }
    plainError /= double(photo.width() / 3) * (photo.height() / 3);
    QVERIFY2(plainError > 2 * error, qPrintable(QStringLiteral("%1 %2").arg(plainError).arg(error)));
    f.strip()->discard();
}

// The editor's own commands are in the menus as well (Edit: Undo, Redo; Select; View: the zoom), as
// the same actions as their keys.
void TestCanvas::editorMenus()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Studio studio;
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto menu = [&window](const QString &title) -> QMenu * {
        for (QAction *a : window.menuBar()->actions()) {
            if (a->text() == title)
                return a->menu();
        }
        return nullptr;
    };
    auto texts = [](QMenu *m) {
        QStringList list;
        for (QAction *a : m->actions()) {
            if (!a->isSeparator())
                list << a->text();
        }
        return list;
    };
    QMenu *edit = menu(QStringLiteral("&Edit"));
    QMenu *select = menu(QStringLiteral("&Select"));
    QMenu *view = menu(QStringLiteral("&View"));
    QVERIFY(edit && select && view);
    QCOMPARE(texts(edit).mid(0, 2), (QStringList{QStringLiteral("Undo"), QStringLiteral("Redo")}));
    QCOMPARE(texts(select), (QStringList{QStringLiteral("Select all"), QStringLiteral("Deselect"), QStringLiteral("Invert selection"),
                                         QStringLiteral("By description…"), QStringLiteral("Delete selected pixels")}));
    QVERIFY(texts(view).contains(QStringLiteral("Zoom in")) && texts(view).contains(QStringLiteral("Fit to window")));
    // The editor's own action: its key shows in the menu.
    QAction *undo = window.findChild<QAction *>(QStringLiteral("editor-undo"));
    QCOMPARE(undo, window.editor()->editorAction(QStringLiteral("undo")));
    QCOMPARE(undo->shortcut(), QKeySequence(QStringLiteral("Ctrl+Z")));
    QVERIFY(!undo->isEnabled()); // no document
    doc::Document *doc = studio.newDocument(QSize(64, 48), QStringLiteral("Menus"));
    QVERIFY(undo->isEnabled());
    window.findChild<QAction *>(QStringLiteral("editor-select-all"))->trigger();
    QVERIFY(doc->hasSelection());
    window.findChild<QAction *>(QStringLiteral("editor-deselect"))->trigger();
    QVERIFY(!doc->hasSelection());
    undo->trigger();
    QVERIFY(doc->hasSelection());
    // Documents open with the rectangle selection, the start of a change with words.
    QCOMPARE(window.editor()->canvas()->tool()->id(), QStringLiteral("select-rect"));
    // File: the picture's file as in other editors, then the project (tst_files has the rest).
    const QStringList file = texts(menu(QStringLiteral("&File")));
    const qsizetype at = file.indexOf(QStringLiteral("Save"));
    QVERIFY(at > 0);
    QCOMPARE(file.mid(at, 6), (QStringList{QStringLiteral("Save"), QStringLiteral("Save as…"), QStringLiteral("Export a copy…"),
                                           QStringLiteral("Close document"), QStringLiteral("Save to project"),
                                           QStringLiteral("Save a copy to project images")}));
    // Save to project says what it is for.
    bool save = false;
    for (QAction *a : menu(QStringLiteral("&File"))->actions())
        save = save || (a->text() == QLatin1String("Save to project") && a->toolTip().contains(QStringLiteral("File → Save")));
    QVERIFY(save);

    // Edit: Cut, Copy and Paste with their usual keys (the editor's own actions), and Copy merged.
    QCOMPARE(texts(edit).mid(2, 4), (QStringList{QStringLiteral("Cut"), QStringLiteral("Copy"), QStringLiteral("Copy merged"),
                                                 QStringLiteral("Paste as layer")}));
    QAction *cut = window.findChild<QAction *>(QStringLiteral("editor-cut"));
    QAction *copy = window.findChild<QAction *>(QStringLiteral("editor-copy"));
    QAction *paste = window.findChild<QAction *>(QStringLiteral("editor-paste-layer"));
    QVERIFY(cut && copy && paste);
    QCOMPARE(cut->shortcut(), QKeySequence(QStringLiteral("Ctrl+X")));
    QCOMPARE(copy->shortcut(), QKeySequence(QStringLiteral("Ctrl+C")));
    QCOMPARE(paste->shortcut(), QKeySequence(QStringLiteral("Ctrl+V")));
    // Copy takes the active layer's selected pixels; Cut also clears them, as one step.
    const QUuid layer = doc->addLayer(QStringLiteral("Red"), [] {
        QImage red(64, 48, QImage::Format_ARGB32_Premultiplied);
        red.fill(Qt::red);
        return red;
    }());
    QImage area = img::createMask(doc->size());
    QPainter(&area).fillRect(QRect(10, 10, 20, 8), Qt::white);
    doc->applySelection(area, QStringLiteral("Select"));
    copy->trigger();
    QImage copied = QApplication::clipboard()->image();
    QCOMPARE(copied.size(), QSize(20, 8));
    QCOMPARE(copied.pixelColor(5, 5), QColor(Qt::red));
    const int steps = doc->undoStack()->index();
    cut->trigger();
    QCOMPARE(doc->undoStack()->index(), steps + 1);
    QCOMPARE(qAlpha(doc->findLayer(layer)->image.pixel(15, 12)), 0);
    QCOMPARE(doc->findLayer(layer)->image.pixelColor(5, 5), QColor(Qt::red));
    doc->undoStack()->undo();
    QCOMPARE(doc->findLayer(layer)->image.pixelColor(15, 12), QColor(Qt::red));
    // Cut needs a selection; Copy without one takes the whole layer.
    doc->clearSelection();
    cut->trigger();
    QCOMPARE(doc->findLayer(layer)->image.pixelColor(15, 12), QColor(Qt::red));
    copy->trigger();
    QCOMPARE(QApplication::clipboard()->image().size(), doc->size());

    // Image: the size of the picture and of its canvas, turning and mirroring it.
    QMenu *imageMenu = menu(QStringLiteral("&Image"));
    QVERIFY(imageMenu);
    QCOMPARE(texts(imageMenu), (QStringList{QStringLiteral("Image size…"), QStringLiteral("Canvas size…"), QStringLiteral("Rotate 90° clockwise"),
                                            QStringLiteral("Rotate 90° counter-clockwise"), QStringLiteral("Rotate 180°"),
                                            QStringLiteral("Flip horizontally"), QStringLiteral("Flip vertically")}));
    QCOMPARE(window.findChild<QAction *>(QStringLiteral("editor-image-size"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+I")));
    window.findChild<QAction *>(QStringLiteral("editor-rotate-cw"))->trigger();
    QCOMPARE(doc->size(), QSize(48, 64));
    doc->undoStack()->undo();
    QCOMPARE(doc->size(), QSize(64, 48));

    // View → Theme: the choices of Settings → General (Automatic and High contrast too), from the
    // next start, with a translated sentence.
    QMenu *themes = window.findChild<QMenu *>(QStringLiteral("themeMenu"));
    QVERIFY(themes);
    QCOMPARE(texts(themes), (QStringList{QStringLiteral("Automatic"), QStringLiteral("Dark"), QStringLiteral("Light"), QStringLiteral("High contrast")}));
    const QString theme = Settings::instance().theme();
    auto restoreTheme = qScopeGuard([theme] { Settings::instance().setValue(QStringLiteral("ui/theme"), theme); });
    window.findChild<QAction *>(QStringLiteral("theme-high-contrast"))->trigger();
    QCOMPARE(Settings::instance().theme(), QStringLiteral("high-contrast"));
    emit themes->aboutToShow();
    QVERIFY(window.findChild<QAction *>(QStringLiteral("theme-high-contrast"))->isChecked());
    QVERIFY(!window.findChild<QAction *>(QStringLiteral("theme-dark"))->isChecked());

    // Help: the user guide, built in, whose contents links go to their sections, and the keys.
    QAction *guideAction = window.findChild<QAction *>(QStringLiteral("userGuide"));
    QVERIFY(guideAction && window.findChild<QAction *>(QStringLiteral("keyboardShortcuts")));
    QCOMPARE(guideAction->shortcut(), QKeySequence(QKeySequence::HelpContents));
    guideAction->trigger();
    auto *guide = window.findChild<UserGuideDialog *>();
    QVERIFY(guide && guide->isVisible());
    QCOMPARE(guide->document(), QStringLiteral("USER_GUIDE.md"));
    QVERIFY(guide->browser()->toPlainText().contains(QStringLiteral("Selecting an area")));
    emit guide->browser()->anchorClicked(QUrl(QStringLiteral("#5-describing-a-change")));
    QCOMPARE(guide->browser()->textCursor().block().text(), QStringLiteral("5. Describing a change"));
    emit guide->browser()->anchorClicked(QUrl(QStringLiteral("PLUGINS.md")));
    QCOMPARE(guide->document(), QStringLiteral("PLUGINS.md"));
    guideAction->trigger();
    QCOMPARE(window.findChildren<UserGuideDialog *>().size(), 1); // the same window, back at the guide
    QCOMPARE(guide->document(), QStringLiteral("USER_GUIDE.md"));
    guide->close();

    // AI: Remove background and Upscale need an engine that can do them. With one that cannot, they
    // are off and say what would help (not "Set up AI", which would not); the AI tab says so too.
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    auto removeEngines = qScopeGuard([&studio] {
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
    });
    ai::Provider *local = studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Local"),
                                                          {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}});
    QVERIFY(local);
    studio.providers()->setDefaultImageProvider(local->instanceId());
    QAction *removeBackground = window.findChild<QAction *>(QStringLiteral("ai-remove-background"));
    QAction *upscale2 = window.findChild<QAction *>(QStringLiteral("ai-upscale-2"));
    QVERIFY(removeBackground && upscale2);
    QVERIFY(!removeBackground->isEnabled());
    QVERIFY2(removeBackground->toolTip().startsWith(QStringLiteral("None of your image engines can remove a background. fal.ai")),
             qPrintable(removeBackground->toolTip()));
    QVERIFY(!upscale2->isEnabled());
    QVERIFY(upscale2->toolTip().contains(QStringLiteral("upscaler folder (Settings → Local servers)")));
    window.editor()->showAiPanel(AiPanel::Mode::RemoveBackground);
    auto *warning = window.editor()->aiPanel()->findChild<QLabel *>(QStringLiteral("aiCapsWarning"));
    auto *runButton = window.editor()->aiPanel()->findChild<QPushButton *>(QStringLiteral("aiRun"));
    QVERIFY(warning && runButton);
    QTRY_COMPARE(warning->text(), removeBackground->toolTip());
    QVERIFY(!runButton->isEnabled());
    // An engine that can: on again, with their own tooltips, and the AI tab names it.
    ai::Provider *stability = studio.providers()->addProvider(QStringLiteral("stability"), QStringLiteral("Stability"),
                                                              {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1/v2beta")}});
    QVERIFY(stability);
    QVERIFY(removeBackground->isEnabled());
    QCOMPARE(removeBackground->toolTip(), QStringLiteral("Cut out the subject onto a transparent layer"));
    QVERIFY(upscale2->isEnabled());
    QTRY_VERIFY(runButton->isEnabled());
    QVERIFY(warning->text().startsWith(QStringLiteral("Will use")));
    // No document: off as before.
    studio.closeDocument(doc);
    QVERIFY(!removeBackground->isEnabled());
}

// Image → Image size… and Canvas size…: the proportions kept, and the picture where it is put.
void TestCanvas::imageSizeDialogs()
{
    ImageSizeDialog image(ImageSizeDialog::Kind::Image, QSize(4000, 3000));
    auto spins = image.findChildren<QSpinBox *>();
    QCOMPARE(spins.size(), 2);
    spins[0]->setValue(2000);
    QCOMPARE(image.newSize(), QSize(2000, 1500));
    spins[1]->setValue(300);
    QCOMPARE(image.newSize(), QSize(400, 300));
    image.findChild<QCheckBox *>()->setChecked(false);
    spins[0]->setValue(1000);
    QCOMPARE(image.newSize(), QSize(1000, 300));

    ImageSizeDialog canvas(ImageSizeDialog::Kind::Canvas, QSize(100, 80));
    canvas.setNewSize(QSize(140, 100));
    QCOMPARE(canvas.canvasRect(), QRect(-20, -10, 140, 100)); // in the middle
    canvas.setAnchor(-1, -1);
    QCOMPARE(canvas.canvasRect(), QRect(0, 0, 140, 100));
    canvas.setAnchor(1, 1);
    QCOMPARE(canvas.canvasRect(), QRect(-40, -20, 140, 100));
    canvas.setNewSize(QSize(60, 80)); // smaller: cut, from the right side here
    canvas.setAnchor(-1, 0);
    QCOMPARE(canvas.canvasRect(), QRect(0, 0, 60, 80));
    doc::Document doc(QSize(100, 80));
    doc.resizeCanvas(canvas.canvasRect(), QStringLiteral("Canvas size"));
    QCOMPARE(doc.size(), QSize(60, 80));
}

// The selection tools' options: New, Add, Subtract and Intersect without holding a key, and a soft
// edge.
void TestCanvas::selectionOptions()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    Studio studio;
    MainWindow window(&studio);
    window.resize(1280, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    doc::Document *doc = studio.newDocument(QSize(200, 100), QStringLiteral("Select"));
    EditorPage *editor = window.editor();
    CanvasView *canvas = editor->canvas();
    QCOMPARE(canvas->tool()->id(), QStringLiteral("select-rect"));
    auto *add = editor->findChild<QToolButton *>(QStringLiteral("selectionAdd"));
    auto *fresh = editor->findChild<QToolButton *>(QStringLiteral("selectionNew"));
    auto *intersect = editor->findChild<QToolButton *>(QStringLiteral("selectionIntersect"));
    QVERIFY(add && fresh && intersect && add->isVisible());
    QVERIFY(fresh->isChecked());
    QTRY_VERIFY(canvas->isFitted());
    auto drag = [&](const QRect &r) {
        QWidget *surface = canvas->focusProxy();
        const QPoint from = canvas->mapFromDoc(QPointF(r.topLeft())).toPoint();
        const QPoint to = canvas->mapFromDoc(QPointF(r.bottomRight() + QPoint(1, 1))).toPoint();
        QTest::mousePress(surface, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(surface, (from + to) / 2);
        QTest::mouseMove(surface, to);
        QTest::mouseRelease(surface, Qt::LeftButton, Qt::NoModifier, to);
    };
    drag(QRect(10, 10, 40, 40));
    QVERIFY(doc->hasSelection());
    const QRect first = img::maskBounds(doc->selection(), 128);
    QVERIFY2(qAbs(first.left() - 10) <= 2 && qAbs(first.width() - 40) <= 3, qPrintable(QStringLiteral("%1 %2").arg(first.left()).arg(first.width())));
    // Add, without Shift: both areas.
    add->click();
    drag(QRect(120, 10, 40, 40));
    QVERIFY(doc->selection().pixelColor(30, 30).red() > 200 && doc->selection().pixelColor(140, 30).red() > 200);
    // Intersect: only the overlap stays.
    intersect->click();
    drag(QRect(30, 0, 100, 100));
    QVERIFY(doc->selection().pixelColor(15, 30).red() < 50);
    QVERIFY(doc->selection().pixelColor(40, 30).red() > 200);
    QVERIFY(doc->selection().pixelColor(140, 30).red() < 50);
    // New again, with a soft edge: the mask fades out around the shape.
    fresh->click();
    auto *feather = editor->findChild<SliderField *>(QStringLiteral("selectionFeather"));
    QVERIFY(feather);
    const int keepFeather = editor->editorState()->selectionFeather();
    auto restore = qScopeGuard([&] { editor->editorState()->setSelectionFeather(keepFeather); });
    feather->findChild<QDoubleSpinBox *>()->setValue(8); // as typed
    QCOMPARE(editor->editorState()->selectionFeather(), 8);
    drag(QRect(60, 30, 80, 40));
    const int edge = qGray(doc->selection().pixel(60, 50));
    QVERIFY2(edge > 20 && edge < 235, qPrintable(QString::number(edge)));
    QVERIFY(qGray(doc->selection().pixel(100, 50)) > 240);
    QVERIFY(qGray(doc->selection().pixel(10, 50)) < 10);
    // The buttons follow the mode, the modes the buttons.
    editor->editorState()->setSelectionMode(EditorState::SelectionMode::Subtract);
    QVERIFY(editor->findChild<QToolButton *>(QStringLiteral("selectionSubtract"))->isChecked());
    editor->editorState()->setSelectionMode(EditorState::SelectionMode::Replace);
}

// New from prompt: without an engine the way to one is in the dialog; an engine that makes one image
// per request gets one job per image.
void TestCanvas::newFromPromptEngines()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    for (ai::Provider *p : studio.providers()->providers())
        studio.providers()->removeProvider(p->instanceId());
    auto cleanup = qScopeGuard([&] {
        for (ai::Provider *p : studio.providers()->providers())
            studio.providers()->removeProvider(p->instanceId());
        Settings::instance().remove(QStringLiteral("newFromPrompt"));
    });
    studio.openProject(dir.filePath(QStringLiteral("engines.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    {
        NewFromPromptDialog dialog(&studio);
        dialog.show();
        auto *setUp = dialog.findChild<QPushButton *>(QStringLiteral("newFromPromptSetUpAi"));
        QVERIFY(setUp && setUp->isVisible());
        QCOMPARE(dialog.findChild<ProviderCombo *>()->currentText(), QStringLiteral("No image engine"));
        bool asked = false;
        connect(&dialog, &NewFromPromptDialog::setUpAiRequested, &dialog, [&asked] { asked = true; });
        setUp->click();
        QVERIFY(asked && dialog.isVisible()); // the wizard opens over it (tst_setup entryPoints)
    }
    // Stability AI makes one image per request.
    ai::Provider *stability = studio.providers()->addProvider(QStringLiteral("stability"), QStringLiteral("Stability"),
                                                              {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:1/v2beta")}});
    QVERIFY(stability);
    QVERIFY(!qobject_cast<ai::ImageProvider *>(stability)->capabilities().batch);
    NewFromPromptDialog dialog(&studio);
    QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("newFromPromptSetUpAi"))->isVisibleTo(&dialog));
    QList<Job *> jobs;
    int results = 0;
    connect(&dialog, &NewFromPromptDialog::generationStarted, &dialog,
            [&](doc::Document *, const QList<Job *> &started, const QString &, int count) {
                jobs = started;
                results = count;
            });
    dialog.findChild<AutoTextEdit *>()->setPlainText(QStringLiteral("a red kite"));
    for (QSpinBox *s : dialog.findChildren<QSpinBox *>()) {
        if (s->accessibleName() == QLatin1String("Images to make"))
            s->setValue(3);
    }
    QVERIFY(dialog.create());
    QCOMPARE(jobs.size(), 3);
    QCOMPARE(results, 3);
    for (Job *j : jobs)
        j->cancel();
}

// The start screen shows the results that were kept, not every one a run made; documents carry a
// "Layers" badge. Browse project images tells how an image was made in plain lines.
void TestCanvas::recentAndDetails()
{
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    QTemporaryDir dir;
    Studio studio;
    studio.openProject(dir.filePath(QStringLiteral("recent.lumen")));
    QTRY_VERIFY_WITH_TIMEOUT(studio.isProjectOpen(), 5000);
    const QJsonObject made{{QStringLiteral("user_prompt"), QStringLiteral("a red kite")}, {QStringLiteral("provider"), QStringLiteral("Klein")},
                           {QStringLiteral("model"), QStringLiteral("flux-2-klein-4b.safetensors")}, {QStringLiteral("mode"), QStringLiteral("edit")},
                           {QStringLiteral("width"), 1024}, {QStringLiteral("height"), 768}, {QStringLiteral("seed"), 42.0},
                           {QStringLiteral("steps"), 4}, {QStringLiteral("negative_prompt"), QString()}};
    auto kept = studio.project()->addAsset(content(), doc::AssetKind::Edit, QStringLiteral("Kept"), made);
    auto dropped = studio.project()->addAsset(content(), doc::AssetKind::Edit, QStringLiteral("Dropped"), made);
    QTRY_VERIFY_WITH_TIMEOUT(kept.isFinished() && dropped.isFinished(), 5000);
    // Keep marks the result it adds.
    doc::Document *doc = studio.newDocument(content().size(), QStringLiteral("Kite"));
    MainWindow window(&studio);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.editor()->variantStrip()->addVariants(doc, {content()}, QStringLiteral("a red kite"),
                                                 QJsonObject{{QStringLiteral("asset"), kept.result().id}});
    window.editor()->variantStrip()->keep();
    QTRY_VERIFY(studio.project()->asset(kept.result().id)->metadata.value(QStringLiteral("kept")).toBool());
    auto saved = studio.project()->saveDocument(doc);
    QTRY_VERIFY_WITH_TIMEOUT(saved.isFinished(), 5000);
    studio.closeDocument(doc);
    StartScreen *start = window.editor()->startScreen();
    QTRY_VERIFY(window.editor()->showsStartScreen());
    QStringList names;
    QTRY_VERIFY(!start->recentTiles().isEmpty());
    for (QToolButton *t : start->recentTiles())
        names << t->accessibleName();
    QVERIFY2(names.contains(QStringLiteral("Kept")) && !names.contains(QStringLiteral("Dropped")), qPrintable(names.join(u',')));
    QVERIFY(names.contains(QStringLiteral("Kite")));

    const QString details = LibraryPage::detailsText(*studio.project()->asset(kept.result().id));
    QVERIFY2(details.contains(QStringLiteral("Made with: Klein (flux-2-klein-4b.safetensors)")), qPrintable(details));
    QVERIFY(details.contains(QStringLiteral("How: Changed from a description")));
    QVERIFY(details.contains(QStringLiteral("Seed: 42")) && details.contains(QStringLiteral("Steps: 4")));
    QVERIFY(!details.contains(u'{') && !details.contains(QStringLiteral("user_prompt")));
}

// One routing for the editor's engines (Settings → AI): new images (a blank canvas, New from
// prompt) go to the image engine, changes of a picture (the prompt bar's edits, Remove object, the
// AI menu's variation, the AI panel) to the engine for changes, unless one is chosen in ⋯ or the
// panel; the status bar names the engine the prompt bar uses now.
void TestCanvas::engineRouting()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    // The mock klein server changes pictures; another engine, where nothing answers, makes new ones.
    ai::Provider *maker = f.studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("New images here"),
                                                            {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}});
    QVERIFY(maker);
    f.studio.providers()->setDefaultImageProvider(maker->instanceId());
    f.studio.providers()->setEditImageProvider(f.provider->instanceId());
    EditorPage *editor = f.editor();
    PromptBar *bar = f.bar();
    VariantStrip *strip = f.strip();
    auto *status = f.window->findChild<QToolButton *>(QStringLiteral("imageEngine"));
    auto *panelEngine = editor->aiPanel()->findChild<ProviderCombo *>();
    QVERIFY(status && panelEngine);
    auto lastJobEngine = [&f] {
        Job *last = f.studio.jobs()->jobs().isEmpty() ? nullptr : f.studio.jobs()->jobs().last();
        return last ? last->metadata().value(QStringLiteral("provider")).toString() : QString();
    };

    // No document: the engine New from prompt uses.
    QVERIFY(editor->showsStartScreen());
    QCOMPARE(bar->engine(), maker);
    QCOMPARE(status->text(), QStringLiteral("New images here"));
    NewFromPromptDialog *fromPrompt = f.window->newFromPrompt();
    QCOMPARE(fromPrompt->findChild<ProviderCombo *>()->providerId(), maker->instanceId());
    fromPrompt->reject();

    // A photo: its changes go to the engine for changes, and the status bar names it.
    doc::Document *photo = f.studio.openImageAsDocument(detailedPhoto(), QStringLiteral("Photo"));
    QTRY_COMPARE(bar->engine(), f.provider);
    QCOMPARE(status->text(), QStringLiteral("Klein"));
    QTRY_VERIFY(bar->plan().action == PromptAction::EditImage);
    QCOMPARE(panelEngine->providerId(), f.provider->instanceId());
    bar->showOptions();
    auto *engine = bar->optionsPopup()->findChild<ProviderCombo *>(QStringLiteral("promptEngine"));
    QCOMPARE(engine->providerId(), f.provider->instanceId());
    bar->optionsPopup()->hide();
    f.words()->setPlainText(QStringLiteral("colorize it"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(f.sd.generations.size(), 1);
    strip->discard();
    // Remove object on a selection, and the AI menu's variation.
    QImage mask = img::createMask(photo->size());
    QPainter(&mask).fillRect(QRect(300, 200, 200, 160), Qt::white);
    photo->applySelection(mask, QStringLiteral("Select"));
    QTRY_VERIFY(bar->plan().region);
    f.button(QStringLiteral("promptRemove"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(f.sd.generations.size(), 2);
    strip->discard();
    photo->clearSelection();
    f.window->findChild<QAction *>(QStringLiteral("ai-variation"))->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(strip->count(), 2, 10000);
    QCOMPARE(f.sd.generations.size(), 3);
    QCOMPARE(lastJobEngine(), QStringLiteral("Klein"));
    strip->discard();

    // A blank canvas: a new image, on the image engine (it does not answer; nothing reaches klein).
    doc::Document *blank = f.studio.newDocument(QSize(512, 384), QStringLiteral("Blank"));
    QTRY_COMPARE(bar->engine(), maker);
    QCOMPARE(status->text(), QStringLiteral("New images here"));
    QTRY_VERIFY(bar->plan().action == PromptAction::NewImage);
    f.words()->setPlainText(QStringLiteral("a lighthouse at dusk"));
    QTest::keyClick(f.words(), Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(!strip->isRunning(), 10000);
    QCOMPARE(lastJobEngine(), QStringLiteral("New images here"));
    QCOMPARE(f.sd.generations.size(), 3);

    // An engine chosen in the editor is for one use. In ⋯ on the blank canvas (named as in Settings
    // → AI): for new images, here, in New from prompt and on the start screen; changes keep theirs.
    ai::Provider *third = f.studio.providers()->addProvider(QStringLiteral("sdcpp"), QStringLiteral("Third"),
                                                            {{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:9")}});
    QVERIFY(third);
    AiOptions *options = editor->aiOptions();
    bar->showOptions();
    auto engineLabel = [&] {
        auto *form = qobject_cast<QFormLayout *>(bar->optionsPopup()->layout());
        auto *label = form ? qobject_cast<QLabel *>(form->labelForField(engine)) : nullptr;
        return label ? label->text() : QString();
    };
    QCOMPARE(engineLabel(), QStringLiteral("Image engine"));
    QCOMPARE(engine->providerId(), maker->instanceId());
    engine->setCurrentIndex(engine->findData(third->instanceId()));
    emit engine->activated(engine->currentIndex());
    QCOMPARE(options->providerId(false), third->instanceId());
    QVERIFY(options->providerId(true).isEmpty());
    QCOMPARE(bar->engine(), third);
    QTRY_COMPARE(status->text(), QStringLiteral("Third"));
    QVERIFY2(status->toolTip().contains(QStringLiteral("chosen in the editor")), qPrintable(status->toolTip()));
    QCOMPARE(panelEngine->providerId(), f.provider->instanceId()); // the panel changes pictures
    bar->optionsPopup()->hide();
    f.studio.setActiveDocument(photo);
    QTRY_COMPARE(bar->engine(), f.provider);
    QTRY_COMPARE(status->text(), QStringLiteral("Klein"));
    f.studio.closeDocument(photo);
    f.studio.closeDocument(blank);
    QTRY_VERIFY(editor->showsStartScreen());
    QTRY_COMPARE(status->text(), QStringLiteral("Third"));
    fromPrompt = f.window->newFromPrompt();
    auto *fromPromptEngine = fromPrompt->findChild<ProviderCombo *>();
    QCOMPARE(fromPromptEngine->providerId(), third->instanceId());
    // A choice in New from prompt is the editor's too; the engine of Settings → AI follows it again.
    fromPromptEngine->setCurrentIndex(fromPromptEngine->findData(maker->instanceId()));
    emit fromPromptEngine->activated(fromPromptEngine->currentIndex());
    QVERIFY(options->providerId(false).isEmpty());
    QTRY_COMPARE(status->text(), QStringLiteral("New images here"));
    fromPromptEngine->setCurrentIndex(fromPromptEngine->findData(third->instanceId()));
    emit fromPromptEngine->activated(fromPromptEngine->currentIndex());
    QCOMPARE(options->providerId(false), third->instanceId());
    QTRY_COMPARE(status->text(), QStringLiteral("Third"));
    // The last choice wins: another image engine in Settings → AI (or from the setup wizard) takes
    // the place of the editor's choice, here and in the open dialog.
    f.studio.providers()->setDefaultImageProvider(f.provider->instanceId());
    QVERIFY(options->providerId(false).isEmpty());
    QTRY_COMPARE(status->text(), QStringLiteral("Klein"));
    QCOMPARE(fromPromptEngine->providerId(), f.provider->instanceId());
    fromPrompt->reject();
    f.studio.providers()->setDefaultImageProvider(maker->instanceId());

    // On a photo, a choice in the AI panel (or ⋯, which shows it as the engine for changes) is for
    // changes; Settings → AI without an engine for changes then has changes use the image engine,
    // in place of the editor's choice.
    photo = f.studio.openImageAsDocument(detailedPhoto(), QStringLiteral("Photo again"));
    QTRY_COMPARE(bar->engine(), f.provider);
    panelEngine->setProviderId(third->instanceId());
    QCOMPARE(options->providerId(true), third->instanceId());
    QVERIFY(options->providerId(false).isEmpty());
    QTRY_COMPARE(bar->engine(), third);
    QTRY_COMPARE(status->text(), QStringLiteral("Third"));
    bar->showOptions();
    QCOMPARE(engineLabel(), QStringLiteral("Engine for changes"));
    QCOMPARE(engine->providerId(), third->instanceId());
    bar->optionsPopup()->hide();
    f.studio.providers()->setEditImageProvider(QString());
    QVERIFY(options->providerId(true).isEmpty());
    QTRY_COMPARE(bar->engine(), maker);
    QTRY_COMPARE(status->text(), QStringLiteral("New images here"));
    QCOMPARE(panelEngine->providerId(), maker->instanceId());
    f.studio.closeDocument(photo);
}

// The Filters tab in the editor's right panel next to Layers and History, also reached from View →
// Filters, and the filter editor from Tools (New filter…, Filter editor…), owned by the main window
// and closed with it. On the canvas AI results to choose from win over the filter's preview, which
// comes back when they are discarded, or for the new picture when one is kept; with the fitted view
// too. A filter's slider keeps the keyboard when results arrive.
void TestCanvas::filtersInTheShell()
{
    PromptBarFixture f;
    QVERIFY(f.setUp(true));
    MainWindow *window = f.window.get();
    EditorPage *editor = f.editor();
    CanvasView *canvas = editor->canvas();
    VariantStrip *strip = f.strip();
    QAction *viewFilters = window->findChild<QAction *>(QStringLiteral("viewFilters"));
    QAction *newFilter = window->findChild<QAction *>(QStringLiteral("newFilter"));
    QAction *filterEditor = window->findChild<QAction *>(QStringLiteral("filterEditor"));
    QVERIFY(viewFilters && newFilter && filterEditor);
    // Nothing to filter on the start screen, and nobody would see which filter the editor opens.
    QVERIFY(!viewFilters->isEnabled());
    QVERIFY(!filterEditor->isEnabled());
    QVERIFY(newFilter->isEnabled());

    QImage red(640, 480, QImage::Format_ARGB32_Premultiplied);
    red.fill(kRed);
    doc::Document *doc = f.studio.openImageAsDocument(red, QStringLiteral("Red"));
    QVERIFY(viewFilters->isEnabled());
    FiltersPanel *panel = editor->filtersPanel();
    QTabWidget *side = nullptr;
    for (QTabWidget *t : editor->findChildren<QTabWidget *>()) {
        if (t->indexOf(panel) >= 0)
            side = t;
    }
    QVERIFY(side);
    QStringList tabs;
    for (int i = 0; i < side->count(); ++i) {
        if (side->isTabVisible(i))
            tabs << side->tabText(i);
    }
    QCOMPARE(tabs, (QStringList{QStringLiteral("Layers"), QStringLiteral("History"), QStringLiteral("Filters")}));
    viewFilters->trigger();
    QCOMPARE(side->currentWidget(), panel);
    // A filter is chosen from the start (the first built-in one, not the list's heading), so the
    // preview shows and Tools → Filter editor… opens it.
    QVERIFY2(panel->currentPath().startsWith(QStringLiteral(":/filters/builtin/")), qPrintable(panel->currentPath()));
    QCOMPARE(panel->list()->currentItem()->data(Qt::UserRole).toString(), panel->currentPath());
    QVERIFY(filterEditor->isEnabled());
    QVERIFY(panel->applyButton()->isEnabled());
    const int opened = int(NodeEditorWindow::openWindows().size());
    filterEditor->trigger();
    QTRY_COMPARE(int(NodeEditorWindow::openWindows().size()), opened + 1);
    QPointer<NodeEditorWindow> firstCopy = NodeEditorWindow::openWindows().last();
    QVERIFY2(!firstCopy->path().startsWith(QStringLiteral(":/")), qPrintable(firstCopy->path())); // a copy to change
    firstCopy->close();
    window->activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), window);
    QVERIFY(panel->selectFilter(QStringLiteral(":/filters/builtin/black-and-white.lumenfilter")));
    QTRY_VERIFY(!canvas->preview(CanvasView::Preview::Filter).isNull());
    QCOMPARE(canvas->preview(), canvas->preview(CanvasView::Preview::Filter));
    // What the canvas draws at a point of the picture (its middle by default).
    auto shownAt = [&](const QPointF &docPoint) {
        QWidget *surface = canvas->focusProxy();
        const QImage shot = surface->grab().toImage();
        return QColor(shot.pixel((canvas->mapFromDoc(docPoint) * shot.devicePixelRatio()).toPoint()));
    };
    auto shown = [&] { return shownAt(QPointF(doc->size().width() / 2.0, doc->size().height() / 2.0)); };
    auto grey = [](const QColor &c) { return std::abs(c.red() - c.green()) <= 2 && std::abs(c.green() - c.blue()) <= 2; };
    QTRY_VERIFY(grey(shown()));
    QVERIFY(canvas->isFitted());

    // Results arrive: they win on the canvas, the filter's preview waits and the panel says why;
    // the fitted picture stays whole above the strip.
    QImage blue(doc->size(), QImage::Format_ARGB32_Premultiplied);
    blue.fill(QColor(0, 90, 200));
    QImage green(doc->size(), QImage::Format_ARGB32_Premultiplied);
    green.fill(QColor(40, 170, 60));
    canvas->focusProxy()->setFocus();
    strip->addVariants(doc, {blue, green}, QStringLiteral("Two"));
    QCOMPARE(canvas->preview(), strip->currentImage());
    QVERIFY(canvas->preview(CanvasView::Preview::Filter).isNull());
    QVERIFY(panel->findChild<QLabel *>(QStringLiteral("FilterPreviewNote"))->isVisible());
    QTRY_COMPARE(shown(), QColor(0, 90, 200));
    QVERIFY(canvas->isFitted());
    QTRY_VERIFY(QRectF(canvas->focusProxy()->rect()).contains(canvas->mapFromDoc(QRectF(doc->bounds()))));
    // On the canvas the arrows and Esc go to the results.
    QTest::keyClick(canvas->focusProxy(), Qt::Key_Right);
    QCOMPARE(strip->current(), 1);
    QTRY_COMPARE(shown(), QColor(40, 170, 60));
    QTest::keyClick(canvas->focusProxy(), Qt::Key_Escape);
    QCOMPARE(strip->count(), 0);
    // Discarded: the filter's preview comes back, over the fitted picture.
    QTRY_VERIFY(!canvas->preview(CanvasView::Preview::Filter).isNull());
    QVERIFY(!panel->findChild<QLabel *>(QStringLiteral("FilterPreviewNote"))->isVisible());
    QTRY_VERIFY(grey(shown()));
    QVERIFY(canvas->isFitted());

    // A result of a selected area is transparent around it: the canvas shows it over the picture,
    // as Keep would add it (not over the checkerboard, and not over the filter's waiting preview).
    QImage area(doc->size(), QImage::Format_ARGB32_Premultiplied);
    area.fill(Qt::transparent);
    const QRect inside(doc->size().width() / 4, doc->size().height() / 4, doc->size().width() / 2, doc->size().height() / 2);
    QPainter(&area).fillRect(inside, QColor(0, 90, 200));
    strip->addVariants(doc, {area}, QStringLiteral("Area"));
    QCOMPARE(canvas->preview(), area);
    QTRY_VERIFY(QRectF(canvas->focusProxy()->rect()).contains(canvas->mapFromDoc(QRectF(doc->bounds()))));
    QTRY_COMPARE(shown(), QColor(0, 90, 200));
    QTRY_COMPARE(shownAt(QPointF(20, 20)), kRed);
    QTRY_COMPARE(shownAt(QPointF(doc->size().width() - 20, doc->size().height() - 20)), kRed);
    strip->discard();
    QTRY_VERIFY(grey(shown()));

    // A filter's slider has the keyboard when results arrive: it keeps it, and its keys stay its own.
    QVERIFY(panel->selectFilter(QStringLiteral(":/filters/builtin/old-photo.lumenfilter")));
    QSlider *slider = nullptr;
    for (ParamField *field : panel->fields()) {
        if (!slider)
            slider = field->findChild<QSlider *>();
    }
    QVERIFY(slider);
    QTRY_VERIFY(!canvas->preview(CanvasView::Preview::Filter).isNull());
    slider->setFocus();
    QCOMPARE(QApplication::focusWidget(), slider);
    const int value = slider->value();
    strip->addVariants(doc, {blue, green}, QStringLiteral("Two again"));
    QCOMPARE(QApplication::focusWidget(), slider);
    QVERIFY(canvas->preview(CanvasView::Preview::Filter).isNull());
    // Apply waits too: it would change a layer under the results, which Keep then covers.
    QVERIFY(!panel->applyButton()->isEnabled());
    const int layersWithResults = doc->layerCount();
    QVERIFY(!panel->applySelected());
    QVERIFY2(panel->statusText().startsWith(QStringLiteral("Keep or discard the AI results first")), qPrintable(panel->statusText()));
    QCOMPARE(doc->layerCount(), layersWithResults);
    QTest::keyClick(slider, slider->value() < slider->maximum() ? Qt::Key_Right : Qt::Key_Left);
    QVERIFY(slider->value() != value);
    QCOMPARE(strip->current(), 0);
    QTest::qWait(100);
    QVERIFY(canvas->preview(CanvasView::Preview::Filter).isNull()); // a setting changed: it still waits
    strip->discard();
    QTRY_VERIFY(!canvas->preview(CanvasView::Preview::Filter).isNull());
    QVERIFY(panel->applyButton()->isEnabled());
    // Kept: the preview comes back, of the picture with the kept result on top.
    QImage white(doc->size(), QImage::Format_ARGB32_Premultiplied);
    white.fill(Qt::white);
    strip->addVariants(doc, {white}, QStringLiteral("White"));
    QVERIFY(canvas->preview(CanvasView::Preview::Filter).isNull());
    const int layers = doc->layerCount();
    strip->keep();
    QCOMPARE(doc->layerCount(), layers + 1);
    QTRY_VERIFY(!canvas->preview(CanvasView::Preview::Filter).isNull());
    QTRY_VERIFY(shown().red() > 200);
    QVERIFY(shown() != kRed);

    // What Apply said belongs to its picture: another document starts without it.
    QSignalSpy applied(panel->runner(), &FilterRunner::finished);
    QVERIFY(panel->applySelected());
    QVERIFY(applied.wait(10000));
    QVERIFY2(panel->statusText().startsWith(QStringLiteral("Applied")), qPrintable(panel->statusText()));
    doc::Document *other = f.studio.newDocument(QSize(320, 240), QStringLiteral("Other"));
    QTRY_COMPARE(f.studio.activeDocument(), other);
    QCOMPARE(panel->statusText(), QString());

    // Tools → Filter editor… opens the chosen filter (a built-in one as a copy to change), New
    // filter… asks for a name; both in windows owned by the main window, which close with it.
    const int before = int(NodeEditorWindow::openWindows().size());
    filterEditor->trigger();
    QTRY_COMPARE(int(NodeEditorWindow::openWindows().size()), before + 1);
    QPointer<NodeEditorWindow> copy = NodeEditorWindow::openWindows().last();
    QCOMPARE(copy->parentWidget(), window);
    QVERIFY(copy->isVisible());
    QCOMPARE(side->currentWidget(), panel);
    QTimer::singleShot(200, window, [] {
        if (auto *ask = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            ask->setTextValue(QStringLiteral("My shell filter"));
            ask->accept();
        }
    });
    newFilter->trigger();
    QTRY_COMPARE(int(NodeEditorWindow::openWindows().size()), before + 2);
    QPointer<NodeEditorWindow> mine = NodeEditorWindow::openWindows().last();
    QCOMPARE(mine->parentWidget(), window);
    QVERIFY(QFileInfo(mine->path()).completeBaseName().contains(QStringLiteral("My shell filter")));
    QTRY_COMPARE(f.studio.jobs()->activeCount() + f.studio.jobs()->pendingCount(), 0); // no question on closing
    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(!copy && !mine && !firstCopy, 5000);
    QVERIFY(!window->isVisible());
}

QTEST_MAIN(TestCanvas)
#include "tst_canvas.moc"
