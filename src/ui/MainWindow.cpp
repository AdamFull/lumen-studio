#include "ui/MainWindow.h"

#include "core/Error.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "runtime/CloudTraining.h"
#include "runtime/Hardware.h"
#include "ui/DocumentFiles.h"
#include "ui/Studio.h"
#include "ui/Icons.h"
#include "ui/Shortcuts.h"
#include "ui/Theme.h"
#include "ui/dialogs/NewFromPromptDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/dialogs/SetupWizard.h"
#include "ui/filters/FiltersPanel.h"
#include "ui/filters/NodeEditorWindow.h"
#include "ui/widgets/ServerStatus.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/AiPanel.h"
#include "ui/pages/LibraryPage.h"
#include "ui/pages/PromptBar.h"
#include "ui/pages/StartScreen.h"
#include "ui/pages/VariantStrip.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/JobCard.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/Toasts.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStatusBar>
#include <QTimer>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

// Where a window of its own opens the first time: most of the screen, at most `size`.
void placeWindow(QWidget *window, const QString &key, QSize size)
{
    const QByteArray geometry = Settings::instance().value(key).toByteArray();
    if (!geometry.isEmpty() && window->restoreGeometry(geometry))
        return;
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        size = size.boundedTo(screen->availableGeometry().size() * 0.9);
    window->resize(size);
}

QString geometryKey(const QString &id)
{
    return QStringLiteral("ui/windows/%1/geometry").arg(id);
}

// Browse project images: the project's images (with batch background removal and upscaling) in a
// window of its own, so that "Open in editor" can bring the editor in front of it.
class ProjectImagesDialog : public QDialog
{
public:
    explicit ProjectImagesDialog(Studio *studio)
        : QDialog(nullptr, Qt::Window)
    {
        setObjectName(QStringLiteral("projectImages"));
        setWindowTitle(MainWindow::tr("Project images — Lumen Studio"));
        setWindowIcon(Icons::get(QStringLiteral("logo")));
        setAttribute(Qt::WA_QuitOnClose, false);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(new LibraryPage(studio, this));
        new Toasts(this);
        placeWindow(this, geometryKey(QStringLiteral("library")), QSize(1200, 760));
        // Ctrl+O, Ctrl+N, Settings… work here as in the editor.
        Shortcuts::instance().shareAppWide(this);
    }

protected:
    void hideEvent(QHideEvent *e) override
    {
        Settings::instance().setValue(geometryKey(QStringLiteral("library")), saveGeometry());
        QDialog::hideEvent(e);
    }
};

} // namespace

// A plugin page in a window of its own, built the first time it is opened. It does not keep the
// application running: closing the main window quits.
class PageWindow : public QWidget
{
public:
    PageWindow(const ext::PageSpec &spec, Studio *studio)
        : QWidget(nullptr, Qt::Window)
        , m_id(spec.id)
    {
        setObjectName(QStringLiteral("pageWindow-%1").arg(spec.id));
        setWindowTitle(QStringLiteral("%1 — Lumen Studio").arg(spec.label));
        setWindowIcon(Icons::get(spec.icon));
        setAttribute(Qt::WA_QuitOnClose, false);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        m_page = spec.create(studio, this);
        v->addWidget(m_page);
        new Toasts(this);
        placeWindow(this, geometryKey(spec.id), QSize(1400, 880));
        // The main window's commands that are not about the editor's picture work here too; the
        // page's own keys (its undo) stay its own.
        Shortcuts::instance().shareAppWide(this);
    }
    QWidget *page() const { return m_page; }

protected:
    void hideEvent(QHideEvent *e) override
    {
        Settings::instance().setValue(geometryKey(m_id), saveGeometry());
        QWidget::hideEvent(e);
    }

private:
    QString m_id;
    QWidget *m_page;
};

MainWindow::MainWindow(Studio *studio, QWidget *parent)
    : QMainWindow(parent)
    , m_studio(studio)
{
    setWindowTitle(QStringLiteral("Lumen Studio"));
    setWindowIcon(Icons::get(QStringLiteral("logo")));
    // 1500×920 where it fits; on a laptop at 125 % or 150 % (1536×864, 1280×720) most of the screen.
    QSize size(1500, 920);
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        size = size.boundedTo(screen->availableGeometry().size() * 0.92);
    resize(size);
    setMinimumSize(1024, 640);

    m_pageSpecs = ext::Extensions::instance().pages();
    for (const ext::PanelSpec &spec : ext::Extensions::instance().panels())
        m_panels.append(Panel{spec, nullptr, nullptr});

    // The editor fills the window; the queue, the plugins' panels and the notifications lie over it
    // (outside the editor, so its shortcuts stay out of their text fields).
    auto *central = new QWidget(this);
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_editor = new EditorPage(studio, central);
    v->addWidget(m_editor);
    setCentralWidget(central);
    m_toasts = new Toasts(central);

    StartScreen *start = m_editor->startScreen();
    connect(start, &StartScreen::openImageRequested, this, &MainWindow::openImages);
    connect(start, &StartScreen::pasteRequested, m_editor, &EditorPage::pasteAsNewDocument);
    connect(start, &StartScreen::newFromPromptRequested, this, &MainWindow::newFromPrompt);
    connect(start, &StartScreen::newCanvasRequested, m_editor, &EditorPage::newDocumentDialog);
    connect(start, &StartScreen::browseRequested, this, &MainWindow::browseProjectImages);
    connect(start, &StartScreen::setUpAiRequested, this, &MainWindow::setUpAi);
    connect(m_editor->promptBar(), &PromptBar::setUpAiRequested, this, &MainWindow::setUpAi);
    // The status bar names the engine the prompt bar uses now.
    connect(m_editor->promptBar(), &PromptBar::engineChanged, this, &MainWindow::updateEngine);
    // The panels laid over the editor stay above the prompt bar and its results.
    connect(m_editor, &EditorPage::bottomAreaChanged, this, &MainWindow::placePanels);

    buildStatusBar();
    buildMenus();

    connect(studio, &Studio::pageRequested, this, &MainWindow::showPage);
    connect(studio, &Studio::notification, this, &MainWindow::notify);
    connect(studio, &Studio::projectOpened, this, &MainWindow::updateTitle);
    connect(studio, &Studio::rerunRequested, this, &MainWindow::rerun);
    connect(studio, &Studio::poseForGenerationRequested, this, &MainWindow::newImageWithPose);
    // "Reuse prompt" goes to the editor's prompt field; with no document open it is a new image.
    connect(studio, &Studio::promptRequested, this, [this](const QString &prompt, const QString &negative) {
        if (m_studio->documents().isEmpty())
            newFromPrompt()->setPrompt(prompt, negative);
    });
    connect(studio->providers(), &ai::ProviderRegistry::providersChanged, this, &MainWindow::updateEngine);
    connect(studio->providers(), &ai::ProviderRegistry::rolesChanged, this, &MainWindow::updateEngine);
    connect(studio->jobs(), &JobQueue::countsChanged, this, &MainWindow::updateJobSummary);
    connect(studio->jobs(), &JobQueue::countsChanged, this, [this] {
        updateQueuePanel();
        placePanels();
    });
    connect(studio->jobs(), &JobQueue::jobAdded, this, [this](Job *job) {
        if (m_queueList) {
            auto *card = new JobCard(job, m_studio->jobs(), m_queuePanel);
            connect(card, &JobCard::dismissed, this, [this](Job *j) { m_studio->jobs()->remove(j); });
            m_queueList->insertWidget(0, card);
        }
        updateJobSummary();
    });
    connect(studio->jobs(), &JobQueue::jobAboutToBeRemoved, this, [this](Job *job) {
        if (!m_queueList)
            return;
        for (int i = 0; i < m_queueList->count(); ++i) {
            if (auto *card = qobject_cast<JobCard *>(m_queueList->itemAt(i)->widget()); card && card->job() == job) {
                card->deleteLater();
                break;
            }
        }
    });

    const QByteArray geometry = Settings::instance().value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    // The page the rail showed last (before the editor became the main window); nothing uses it.
    Settings::instance().remove(QStringLiteral("ui/page"));
    updateTitle();
    updateEngine();
    updateJobSummary();
    QTimer::singleShot(400, this, &MainWindow::maybeShowOnboarding);
}

MainWindow::~MainWindow()
{
    // The editor's bar and strip hide while the window's children go; nothing to place by then.
    disconnect(m_editor, nullptr, this, nullptr);
    // Windows of their own (no parent): their pages use the studio, which outlives this window.
    for (const QPointer<PageWindow> &w : std::as_const(m_windows))
        delete w.data();
    delete m_projectImages.data();
}

void MainWindow::buildStatusBar()
{
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setContentsMargins(8, 0, 8, 0);
    m_projectLabel = new ElidedLabel(QString(), this);
    m_projectLabel->setProperty("role", QStringLiteral("muted"));
    statusBar()->addWidget(m_projectLabel, 1);

    // The image engine the prompt bar uses, in plain words; Settings → AI changes it, Set up AI adds
    // the first one.
    m_engineButton = new QToolButton(this);
    m_engineButton->setObjectName(QStringLiteral("imageEngine"));
    m_engineButton->setAutoRaise(true);
    m_engineButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_engineButton->setAccessibleName(tr("Image engine"));
    connect(m_engineButton, &QToolButton::clicked, this, [this] {
        if (m_editor->promptBar()->engine())
            openSettings(QStringLiteral("ai"));
        else
            setUpAi();
    });
    statusBar()->addPermanentWidget(m_engineButton);
    statusBar()->addPermanentWidget(new ServerStatusButton(m_studio, [this] { openSettings(QStringLiteral("servers")); }, this));

    // "Safe for work" in one click (also in Settings → General).
    auto *safe = new QToolButton(this);
    safe->setObjectName(QStringLiteral("safeContent"));
    safe->setCheckable(true);
    safe->setAutoRaise(true);
    safe->setIcon(Icons::get(QStringLiteral("shield")));
    safe->setText(tr("Safe for work"));
    safe->setAccessibleName(tr("Safe for work"));
    safe->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto syncSafe = [safe] {
        const bool on = Settings::instance().value(QStringLiteral("generation/safeContent"), false).toBool();
        QSignalBlocker block(safe);
        safe->setChecked(on);
        safe->setToolTip(on ? tr("Safe for work is on: nudity and explicit content are kept out of generated images. Click to turn it off.")
                            : tr("Safe for work is off. Click to keep nudity and explicit content out of generated images."));
    };
    syncSafe();
    connect(safe, &QToolButton::toggled, this, [](bool on) { Settings::instance().setValue(QStringLiteral("generation/safeContent"), on); });
    connect(&Settings::instance(), &Settings::changed, safe, [syncSafe](const QString &key) {
        if (key == QLatin1String("generation/safeContent"))
            syncSafe();
    });
    statusBar()->addPermanentWidget(safe);

    // Running and queued jobs; a click opens the Queue.
    m_jobsButton = new QToolButton(this);
    m_jobsButton->setObjectName(QStringLiteral("jobs"));
    m_jobsButton->setAutoRaise(true);
    m_jobsButton->setCheckable(true);
    m_jobsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_jobsButton->setIcon(Icons::get(QStringLiteral("queue")));
    m_jobsButton->setAccessibleName(tr("Jobs"));
    m_jobsButton->setToolTip(tr("Show running and finished jobs (Ctrl+Shift+Q)"));
    connect(m_jobsButton, &QToolButton::toggled, this, &MainWindow::setQueueVisible);
    statusBar()->addPermanentWidget(m_jobsButton);
}

void MainWindow::buildMenus()
{
    const QString general = tr("General");
    // `everywhere`: the command also works while another window of Lumen is active (a plugin page,
    // the filter editor, Browse project images); those that change the editor's document do not.
    auto bind = [general](QAction *action, const QString &id, bool everywhere = false) {
        if (action->objectName().isEmpty())
            action->setObjectName(QStringLiteral("app-%1").arg(id));
        Shortcuts::instance().bindAction(action, QStringLiteral("app.") + id, general, QStringLiteral("global"));
        if (everywhere)
            Shortcuts::instance().addAppWide(action);
    };
    // Actions that replace a page of the old navigation rail keep its shortcut id and key.
    auto bindPage = [general](QAction *action, const QString &icon, const QString &name, const QKeySequence &key) {
        Shortcuts::instance().bindAction(action, QStringLiteral("nav.%1").arg(icon), general, QStringLiteral("global"),
                                         key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key}, name);
        Shortcuts::instance().addAppWide(action);
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    bind(file->addAction(Icons::get(QStringLiteral("plus")), tr("New blank canvas…"), QKeySequence::New, this, [this] {
        showPage(pages::Editor);
        m_editor->newDocumentDialog();
    }), QStringLiteral("new-document"), true);
    QAction *fromPrompt = file->addAction(Icons::get(QStringLiteral("sparkles")), tr("New from prompt…"), this, &MainWindow::newFromPrompt);
    fromPrompt->setObjectName(QStringLiteral("newFromPrompt"));
    bindPage(fromPrompt, QStringLiteral("sparkles"), tr("New from prompt"), QKeySequence(QStringLiteral("Alt+2")));
    bind(file->addAction(Icons::get(QStringLiteral("image")), tr("Open image…"), QKeySequence::Open, this, &MainWindow::openImages),
         QStringLiteral("open-image"), true);
    // Pictures opened from disk or saved as, newest first; one that is gone is shown greyed.
    QMenu *recentFiles = file->addMenu(Icons::get(QStringLiteral("history")), tr("Open recent"));
    recentFiles->setObjectName(QStringLiteral("openRecent"));
    recentFiles->setToolTipsVisible(true);
    connect(recentFiles, &QMenu::aboutToShow, this, [this, recentFiles] { fillRecentFiles(recentFiles); });
    bind(file->addAction(Icons::get(QStringLiteral("copy")), tr("Paste from clipboard"), this, [this] {
        showPage(pages::Editor);
        m_editor->pasteAsNewDocument();
    }), QStringLiteral("paste-new-document"), true);
    QAction *browse = file->addAction(Icons::get(QStringLiteral("gallery")), tr("Browse project images…"), this, &MainWindow::browseProjectImages);
    browse->setObjectName(QStringLiteral("browseProjectImages"));
    bindPage(browse, QStringLiteral("gallery"), tr("Browse project images"), QKeySequence(QStringLiteral("Alt+6")));
    file->addSeparator();
    // The picture's file, as in other editors: Save writes it back, Save as… chooses another file the
    // document then saves to, Export a copy… writes one it does not.
    auto documentAction = [this, file, bind](const QString &id, const QString &icon, const QString &text, const QKeySequence &key,
                                             const QString &tip, std::function<void()> fn) {
        QAction *a = file->addAction(icon.isEmpty() ? QIcon() : Icons::get(icon), text, key, this, std::move(fn));
        a->setToolTip(tip);
        a->setStatusTip(tip);
        bind(a, id);
        m_documentActions << a;
        return a;
    };
    documentAction(QStringLiteral("save"), QStringLiteral("save"), tr("Save"), QKeySequence::Save,
                   tr("Write the picture to the file it was opened from or last saved as (a new picture asks for a file)"),
                   [this] { m_editor->files()->save(m_studio->activeDocument()); });
    documentAction(QStringLiteral("save-as"), QString(), tr("Save as…"), QKeySequence(QStringLiteral("Ctrl+Shift+S")),
                   tr("Write the picture to a new file (PNG, JPEG, layered PSD…); Save writes to that file from then on"),
                   [this] { m_editor->files()->saveAs(m_studio->activeDocument()); });
    documentAction(QStringLiteral("export-image"), QStringLiteral("download"), tr("Export a copy…"), QKeySequence(QStringLiteral("Ctrl+Shift+E")),
                   tr("Write a copy of the picture to a file; Save keeps writing to the picture's own file"),
                   [this] { m_editor->files()->exportCopy(m_studio->activeDocument()); });
    documentAction(QStringLiteral("close-document"), QString(), tr("Close document"), QKeySequence::Close,
                   tr("Close the picture (asks first when it has changes not saved to its file)"),
                   [this] { m_editor->files()->close(m_studio->activeDocument()); });
    file->addSeparator();
    // The project keeps every document with its layers, saved by itself a few seconds after a change.
    QAction *saveAll = file->addAction(Icons::get(QStringLiteral("save")), tr("Save to project"), this, [this] {
        m_studio->saveAll().then(this, [this] {
            m_studio->notify(tr("Saved to the project with their layers. File → Save writes the picture's file."), QStringLiteral("success"));
        });
    });
    saveAll->setToolTip(tr("Save the open documents, with their layers, in the project now (it also does so by itself). File → Save "
                           "writes the picture's file."));
    saveAll->setStatusTip(saveAll->toolTip());
    bind(saveAll, QStringLiteral("save-all"));
    QAction *toLibrary = file->addAction(Icons::get(QStringLiteral("gallery")), tr("Save a copy to project images"), this,
                                         [this] { m_editor->saveToLibrary(); });
    toLibrary->setToolTip(tr("Add the picture, merged, to the project's images (File → Browse project images)"));
    toLibrary->setStatusTip(toLibrary->toolTip());
    bind(toLibrary, QStringLiteral("save-to-library")); // Ctrl+Shift+S went to Save as…
    m_documentActions << toLibrary;
    file->addSeparator();
    bind(file->addAction(Icons::get(QStringLiteral("folder")), tr("New project…"), this, &MainWindow::newProject), QStringLiteral("new-project"), true);
    bind(file->addAction(Icons::get(QStringLiteral("folder")), tr("Open project…"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this, &MainWindow::openProject),
         QStringLiteral("open-project"), true);
    QMenu *recent = file->addMenu(tr("Recent projects"));
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        recent->clear();
        for (const QString &path : Settings::instance().recentProjects()) {
            QAction *a = recent->addAction(QFileInfo(path).completeBaseName(), this, [this, path] { m_studio->openProject(path); });
            a->setToolTip(path);
            a->setEnabled(QFileInfo::exists(path));
        }
        if (recent->isEmpty())
            recent->addAction(tr("(none)"))->setEnabled(false);
    });
    file->addSeparator();
    // QKeySequence::Quit has no key on Windows; Ctrl+Q as in other image editors.
    bind(file->addAction(tr("Quit"), QKeySequence(QStringLiteral("Ctrl+Q")), this, &QWidget::close), QStringLiteral("quit"), true);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    // The editor's own commands: the same actions as its keys, so a key changed in Settings →
    // Shortcuts shows and works here too.
    auto editorAction = [this](QMenu *menu, const QString &id, const QString &icon = QString()) {
        if (QAction *a = m_editor->editorAction(id)) {
            if (!icon.isEmpty())
                a->setIcon(Icons::get(icon));
            a->setObjectName(QStringLiteral("editor-%1").arg(id));
            menu->addAction(a);
            m_documentActions << a;
        }
    };
    editorAction(edit, QStringLiteral("undo"), QStringLiteral("undo"));
    editorAction(edit, QStringLiteral("redo"), QStringLiteral("redo"));
    edit->addSeparator();
    bind(edit->addAction(Icons::get(QStringLiteral("copy")), tr("Copy merged"), QKeySequence(QStringLiteral("Ctrl+Shift+C")), this, [this] { m_editor->copyMerged(); }), QStringLiteral("copy-merged"));
    bind(edit->addAction(tr("Paste as layer"), this, [this] {
        showPage(pages::Editor);
        m_editor->pasteAsLayer();
    }), QStringLiteral("paste-layer-menu"));
    bind(edit->addAction(tr("Import image as layer…"), this, [this] {
        showPage(pages::Editor);
        m_editor->importAsLayer();
    }), QStringLiteral("import-layer"));
    edit->addSeparator();
    bind(edit->addAction(Icons::get(QStringLiteral("settings")), tr("Settings…"), QKeySequence(QStringLiteral("Ctrl+,")), this, [this] { openSettings(); }), QStringLiteral("settings"), true);

    // Selecting is where changing a picture with words starts.
    QMenu *select = menuBar()->addMenu(tr("&Select"));
    editorAction(select, QStringLiteral("select-all"));
    editorAction(select, QStringLiteral("deselect"));
    editorAction(select, QStringLiteral("invert-selection"));
    select->addSeparator();
    editorAction(select, QStringLiteral("delete-pixels"), QStringLiteral("trash"));

    QMenu *view = menuBar()->addMenu(tr("&View"));
    editorAction(view, QStringLiteral("zoom-in"), QStringLiteral("zoom-in"));
    editorAction(view, QStringLiteral("zoom-out"), QStringLiteral("zoom-out"));
    editorAction(view, QStringLiteral("fit"), QStringLiteral("fit"));
    editorAction(view, QStringLiteral("actual-pixels"));
    view->addSeparator();
    // The right panel's Filters tab: built-in and saved filters for the picture or the selection.
    QAction *filters = view->addAction(Icons::get(QStringLiteral("adjust")), tr("Filters"), this, [this] { m_editor->showFiltersPanel(); });
    filters->setObjectName(QStringLiteral("viewFilters"));
    filters->setToolTip(tr("Show the Filters panel: black and white, old photo, colorize with AI and your own filters"));
    filters->setStatusTip(filters->toolTip());
    bind(filters, QStringLiteral("filters-panel"));
    m_documentActions << filters;
    m_queueAction = view->addAction(Icons::get(QStringLiteral("queue")), tr("Job queue"));
    m_queueAction->setObjectName(QStringLiteral("jobQueue"));
    m_queueAction->setCheckable(true);
    m_queueAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")));
    bind(m_queueAction, QStringLiteral("job-queue"), true);
    connect(m_queueAction, &QAction::toggled, this, &MainWindow::setQueueVisible);
    // Panels of plugins (the assistant), over the right side of the editor.
    for (int index = 0; index < m_panels.size(); ++index) {
        const ext::PanelSpec &spec = m_panels.at(index).spec;
        QAction *a = view->addAction(Icons::get(spec.icon), spec.label);
        a->setObjectName(QStringLiteral("panel-%1").arg(spec.id));
        a->setCheckable(true);
        a->setToolTip(spec.tooltip);
        a->setStatusTip(spec.tooltip);
        if (!spec.shortcutId.isEmpty())
            Shortcuts::instance().bindAction(a, spec.shortcutId, general, QStringLiteral("global"), {spec.shortcut}, spec.label);
        Shortcuts::instance().addAppWide(a);
        connect(a, &QAction::toggled, this, [this, index](bool on) { setPanelVisible(index, on); });
        m_panels[index].action = a;
    }
    view->addSeparator();
    view->addAction(Icons::get(Theme::instance().isDark() ? QStringLiteral("sun") : QStringLiteral("moon")), tr("Toggle light/dark theme"), this, [this] {
        const QString next = Theme::instance().isDark() ? QStringLiteral("light") : QStringLiteral("dark");
        Settings::instance().setValue(QStringLiteral("ui/theme"), next);
        QMessageBox::information(this, tr("Theme"), tr("The %1 theme will be applied the next time Lumen Studio starts.").arg(next));
    });

    // What is not "describe a change" (that is the prompt bar's): the AI panel's actions.
    QMenu *aiMenu = menuBar()->addMenu(tr("&AI"));
    auto aiAction = [this](QMenu *menu, const QString &name, const QString &icon, const QString &text, const QString &tip,
                           std::function<void()> fn) {
        QAction *a = menu->addAction(Icons::get(icon), text, this, std::move(fn));
        a->setObjectName(name);
        a->setToolTip(tip);
        a->setStatusTip(tip);
        m_documentActions << a;
        return a;
    };
    QMenu *upscale = aiMenu->addMenu(Icons::get(QStringLiteral("upscale")), tr("Upscale"));
    for (const double factor : {2.0, 3.0, 4.0}) {
        aiAction(upscale, QStringLiteral("ai-upscale-%1").arg(int(factor)), QStringLiteral("upscale"), QStringLiteral("%1×").arg(int(factor)),
                 tr("A larger copy of the image, %1 times as wide, as a new document").arg(int(factor)),
                 [this, factor] { m_editor->aiPanel()->upscale(factor); });
    }
    aiAction(aiMenu, QStringLiteral("ai-remove-background"), QStringLiteral("scissors"), tr("Remove background"),
             tr("Cut out the subject onto a transparent layer"), [this] { m_editor->aiPanel()->run(AiPanel::Mode::RemoveBackground); });
    aiAction(aiMenu, QStringLiteral("ai-expand"), QStringLiteral("outpaint"), tr("Expand canvas…"),
             tr("Make the canvas larger and let the image engine paint the new area"),
             [this] { m_editor->showAiPanel(AiPanel::Mode::Expand); });
    aiAction(aiMenu, QStringLiteral("ai-variation"), QStringLiteral("variation"), tr("Variation"),
             tr("Re-imagine the image (or the selection), keeping its layout; the words in the prompt bar guide it"),
             [this] { m_editor->aiPanel()->run(AiPanel::Mode::Variation); });
    // Poses are the Poses plugin's: without it nothing shows or edits a detected pose.
    if (m_studio->hasPage(QStringLiteral("poses")))
        aiAction(aiMenu, QStringLiteral("ai-detect-pose"), QStringLiteral("person"), tr("Detect pose"),
                 tr("Find the people's pose in the image; it becomes the document's control pose"),
                 [this] { m_editor->aiPanel()->run(AiPanel::Mode::DetectPose); });
    aiMenu->addSeparator();
    QAction *allOptions = aiMenu->addAction(Icons::get(QStringLiteral("adjust")), tr("All AI options…"), this, [this] { m_editor->showAiPanel(); });
    allOptions->setObjectName(QStringLiteral("ai-all-options"));
    m_documentActions << allOptions;

    // Windows of their own: the filter editor, and one per plugin page.
    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
    QAction *newFilter = tools->addAction(Icons::get(QStringLiteral("plus")), tr("New filter…"), this, [this] {
        if (m_editor->filtersPanel()->newFilter().isEmpty())
            return;
        if (!m_studio->documents().isEmpty())
            m_editor->showFiltersPanel();
    });
    newFilter->setObjectName(QStringLiteral("newFilter"));
    newFilter->setToolTip(tr("Build a filter of your own from steps in the filter editor; it is listed in the Filters panel"));
    newFilter->setStatusTip(newFilter->toolTip());
    bind(newFilter, QStringLiteral("new-filter"), true);
    // The filter chosen in the Filters panel, which shows it: with a document, like View → Filters
    // (on the start screen nobody sees which filter that is).
    QAction *filterEditor = tools->addAction(Icons::get(QStringLiteral("edit")), tr("Filter editor…"), this, [this] {
        m_editor->showFiltersPanel();
        // A filter that cannot be opened says why under the panel's Apply.
        if (m_editor->filtersPanel()->currentPath().isEmpty())
            m_studio->notify(tr("Choose a filter in the Filters panel to open it in the filter editor."));
        else
            m_editor->filtersPanel()->editSelected();
    });
    filterEditor->setObjectName(QStringLiteral("filterEditor"));
    filterEditor->setToolTip(tr("Open the filter chosen in the Filters panel in the filter editor (a built-in filter as a copy to change)"));
    filterEditor->setStatusTip(filterEditor->toolTip());
    bind(filterEditor, QStringLiteral("filter-editor"));
    m_documentActions << filterEditor;
    tools->addSeparator();
    for (const ext::PageSpec &spec : std::as_const(m_pageSpecs)) {
        QAction *a = tools->addAction(Icons::get(spec.icon), spec.label);
        a->setObjectName(QStringLiteral("tool-%1").arg(spec.id));
        a->setToolTip(spec.tooltip);
        a->setStatusTip(spec.tooltip);
        bindPage(a, spec.icon, tr("Open %1").arg(spec.label), spec.shortcut);
        connect(a, &QAction::triggered, this, [this, id = spec.id] { showPage(id); });
    }
    if (!m_pageSpecs.isEmpty())
        tools->addSeparator();
    QAction *plugins = tools->addAction(Icons::get(QStringLiteral("settings")), tr("Plugins…"), this, [this] { openSettings(QStringLiteral("plugins")); });
    plugins->setToolTip(tr("Character creation, poses and Live2D parts are plugins: switch them on in Settings → Plugins"));
    plugins->setStatusTip(plugins->toolTip());
    connect(m_studio, &Studio::documentsChanged, this, &MainWindow::updateDocumentActions);
    updateDocumentActions();

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *setup = help->addAction(Icons::get(QStringLiteral("sparkles")), tr("Set up AI…"), this, &MainWindow::setUpAi);
    setup->setObjectName(QStringLiteral("setUpAi"));
    setup->setToolTip(tr("Get an image engine: on this computer, an online service or a server that is already running"));
    setup->setStatusTip(setup->toolTip());
    help->addSeparator();
    QAction *models = help->addAction(Icons::get(QStringLiteral("folder")), tr("Open models folder"), this, [this] {
        const QString folder = m_studio->models()->defaultFolder();
        QDir().mkpath(folder);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    models->setObjectName(QStringLiteral("openModelsFolder"));
    help->addAction(Icons::get(QStringLiteral("folder")), tr("Open log folder"), this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(paths::logs()));
    });
    help->addSeparator();
    help->addAction(Icons::get(QStringLiteral("info")), tr("About Lumen Studio"), this, [this] {
        QMessageBox::about(this, tr("About Lumen Studio"),
                           tr("<h3>Lumen Studio %1</h3><p>AI-assisted image generation and editing.</p>"
                              "<p>Built with Qt %2.</p>").arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    });
}

void MainWindow::updateDocumentActions()
{
    const bool open = !m_studio->documents().isEmpty();
    for (QAction *a : std::as_const(m_documentActions))
        a->setEnabled(open);
}

void MainWindow::showPage(const QString &id)
{
    if (id == pages::Editor) {
        if (isMinimized())
            showNormal();
        raise();
        activateWindow();
        return;
    }
    if (id == pages::Generate) {
        newFromPrompt();
        return;
    }
    if (id == pages::Library) {
        browseProjectImages();
        return;
    }
    const auto spec = std::find_if(m_pageSpecs.cbegin(), m_pageSpecs.cend(), [&](const ext::PageSpec &s) { return s.id == id; });
    if (spec == m_pageSpecs.cend())
        return;
    QPointer<PageWindow> &window = m_windows[id];
    if (!window)
        window = new PageWindow(*spec, m_studio);
    if (window->isMinimized())
        window->showNormal();
    else
        window->show();
    window->raise();
    window->activateWindow();
    m_lastOpened = window.data();
}

QWidget *MainWindow::pageWidget(const QString &id) const
{
    if (id == pages::Editor)
        return m_editor;
    const QPointer<PageWindow> window = m_windows.value(id);
    return window ? window->page() : nullptr;
}

QWidget *MainWindow::pageWindow(const QString &id) const
{
    return m_windows.value(id).data();
}

QStringList MainWindow::pageIds() const
{
    QStringList ids{QString(pages::Editor)};
    for (const ext::PageSpec &spec : m_pageSpecs)
        ids << spec.id;
    return ids;
}

NewFromPromptDialog *MainWindow::newFromPrompt()
{
    showPage(pages::Editor);
    if (!m_newFromPrompt) {
        m_newFromPrompt = new NewFromPromptDialog(m_studio, this);
        m_newFromPrompt->setAttribute(Qt::WA_DeleteOnClose);
        // The engine for new images the editor uses (also on a blank canvas), and the status bar names.
        m_newFromPrompt->shareEngineChoice(m_editor->aiOptions());
        // Its results are chosen like the prompt bar's, in the strip above the bar.
        connect(m_newFromPrompt, &NewFromPromptDialog::generationStarted, this,
                [this](doc::Document *doc, const QList<Job *> &jobs, const QString &name, int results) {
                    m_editor->variantStrip()->track(doc, jobs, name, results);
                });
        // The wizard opens over the dialog, which follows the engine it sets up.
        connect(m_newFromPrompt, &NewFromPromptDialog::setUpAiRequested, this, &MainWindow::setUpAi);
        m_newFromPrompt->open();
    }
    m_newFromPrompt->raise();
    m_newFromPrompt->activateWindow();
    m_lastOpened = m_newFromPrompt.data();
    return m_newFromPrompt;
}

void MainWindow::rerun(const QString &assetId)
{
    doc::Project *project = m_studio->project();
    const auto info = project ? project->asset(assetId) : std::nullopt;
    if (!info)
        return;
    newFromPrompt()->fillFrom(*info);
}

void MainWindow::newImageWithPose(const pose::Pose &pose)
{
    newFromPrompt()->setPose(pose);
}

QDialog *MainWindow::browseProjectImages()
{
    if (!m_projectImages)
        m_projectImages = new ProjectImagesDialog(m_studio);
    if (m_projectImages->isMinimized())
        m_projectImages->showNormal();
    else
        m_projectImages->show();
    m_projectImages->raise();
    m_projectImages->activateWindow();
    m_lastOpened = m_projectImages.data();
    return m_projectImages;
}

void MainWindow::setQueueVisible(bool show)
{
    if (show && !m_queuePanel) {
        m_queuePanel = new QFrame(centralWidget());
        m_queuePanel->setObjectName(QStringLiteral("queuePanel"));
        m_queuePanel->setProperty("card", QStringLiteral("raised"));
        auto *v = new QVBoxLayout(m_queuePanel);
        v->setContentsMargins(12, 12, 12, 12);
        auto *head = new QHBoxLayout;
        auto *title = new QLabel(tr("Jobs"), m_queuePanel);
        Theme::setRole(title, QStringLiteral("heading"));
        head->addWidget(title);
        head->addStretch();
        m_clearFinished = new QToolButton(m_queuePanel);
        m_clearFinished->setText(tr("Clear finished"));
        connect(m_clearFinished, &QToolButton::clicked, m_studio->jobs(), &JobQueue::clearFinished);
        m_cancelAll = new QToolButton(m_queuePanel);
        m_cancelAll->setText(tr("Cancel all"));
        connect(m_cancelAll, &QToolButton::clicked, m_studio->jobs(), &JobQueue::cancelAll);
        auto *cancelAll = m_cancelAll;
        auto *clear = m_clearFinished;
        auto *close = iconButton(QStringLiteral("close"), tr("Close"), m_queuePanel);
        connect(close, &QToolButton::clicked, this, [this] { setQueueVisible(false); });
        head->addWidget(cancelAll);
        head->addWidget(clear);
        head->addWidget(close);
        v->addLayout(head);
        m_queueEmpty = mutedLabel(tr("Nothing is running. Generations, upscales and downloads appear here."), m_queuePanel);
        m_queueEmpty->setObjectName(QStringLiteral("queueEmpty"));
        v->addWidget(m_queueEmpty);
        auto *scroll = new QScrollArea(m_queuePanel);
        scroll->setWidgetResizable(true);
        auto *content = new QWidget(scroll);
        m_queueList = new QVBoxLayout(content);
        m_queueList->setContentsMargins(0, 0, 4, 0);
        m_queueList->setSpacing(6);
        m_queueList->addStretch();
        scroll->setWidget(content);
        v->addWidget(scroll, 1);
        m_queueScroll = scroll;
        const auto &jobs = m_studio->jobs()->jobs();
        for (Job *job : jobs) {
            auto *card = new JobCard(job, m_studio->jobs(), m_queuePanel);
            connect(card, &JobCard::dismissed, this, [this](Job *j) { m_studio->jobs()->remove(j); });
            m_queueList->insertWidget(0, card);
        }
    }
    // The menu action and the status bar button show the same state.
    for (QObject *o : {static_cast<QObject *>(m_queueAction), static_cast<QObject *>(m_jobsButton)}) {
        if (!o)
            continue;
        QSignalBlocker block(o);
        if (auto *a = qobject_cast<QAction *>(o))
            a->setChecked(show);
        else if (auto *b = qobject_cast<QToolButton *>(o))
            b->setChecked(show);
    }
    if (!m_queuePanel)
        return;
    m_queuePanel->setVisible(show);
    updateQueuePanel();
    placePanels();
}

void MainWindow::updateQueuePanel()
{
    if (!m_queuePanel)
        return;
    const QList<Job *> &jobs = m_studio->jobs()->jobs();
    const bool any = !jobs.isEmpty();
    m_queueEmpty->setVisible(!any);
    m_queueScroll->setVisible(any);
    m_cancelAll->setEnabled(std::any_of(jobs.cbegin(), jobs.cend(), [](const Job *j) { return !j->isFinished(); }));
    m_clearFinished->setEnabled(std::any_of(jobs.cbegin(), jobs.cend(), [](const Job *j) { return j->isFinished(); }));
}

QWidget *MainWindow::queuePanel() const
{
    return m_queuePanel;
}

NewFromPromptDialog *MainWindow::newFromPromptDialog() const
{
    return m_newFromPrompt;
}

QDialog *MainWindow::projectImagesDialog() const
{
    return m_projectImages;
}

void MainWindow::setPanelVisible(int index, bool show)
{
    Panel &panel = m_panels[index];
    if (show && !panel.widget) {
        panel.widget = panel.spec.create(m_studio, centralWidget(), [this, index] {
            if (QAction *a = m_panels[index].action)
                a->setChecked(false);
        });
    }
    if (panel.widget)
        panel.widget->setVisible(show);
    placePanels();
}

void MainWindow::placePanels()
{
    const QSize area = centralWidget()->size();
    // Above the prompt bar and the results strip, which they would hide (Generate, Keep, the zoom).
    const int bottom = std::clamp(m_editor->overlayBottom(centralWidget()), std::min(area.height(), 160), area.height());
    const int width = std::min(460, std::max(320, area.width() / 3));
    int right = area.width() - 12; // the queue stays left of an open panel
    for (const Panel &panel : std::as_const(m_panels)) {
        if (!panel.widget || !panel.widget->isVisible())
            continue;
        panel.widget->setGeometry(area.width() - width - 12, 12, width, bottom - 24);
        panel.widget->raise();
        right = area.width() - width - 24;
    }
    // Over the right side, as tall as its jobs (at most 540 px).
    if (m_queuePanel && m_queuePanel->isVisible()) {
        const int w = std::min(440, right - 12);
        const int wanted = m_queueScroll->isVisible() ? 540 : m_queuePanel->layout()->minimumSize().height() + 24;
        const int h = std::min(std::max(160, wanted), bottom - 24);
        m_queuePanel->setGeometry(right - w, bottom - h - 12, w, h);
        m_queuePanel->raise();
    }
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    placePanels();
}

void MainWindow::updateJobSummary()
{
    const int active = m_studio->jobs()->activeCount();
    const int pending = m_studio->jobs()->pendingCount();
    if (active + pending == 0)
        m_jobsButton->setText(tr("No jobs running"));
    else
        m_jobsButton->setText(tr("%1 running · %2 queued").arg(active).arg(pending));
}

void MainWindow::updateEngine()
{
    // The engine the prompt bar's Generate goes to now: for a new image (a blank canvas, and with no
    // document New from prompt's) or for a change of the picture, as Settings → AI routes them,
    // unless one is chosen in the editor for that use.
    const ai::ImageProvider *engine = m_editor->promptBar()->engine();
    if (!engine) {
        m_engineButton->setText(tr("No image engine – Set up AI"));
        m_engineButton->setIcon(QIcon(Icons::pixmap(QStringLiteral("warning"), 14, Theme::palette().warning)));
        m_engineButton->setToolTip(tr("Nothing can draw or change images yet. Click to set up an image engine on this computer or "
                                      "online."));
        return;
    }
    m_engineButton->setText(imageEngineName(engine));
    m_engineButton->setIcon(Icons::get(engine->type().local ? QStringLiteral("server") : QStringLiteral("cloud")));
    if (m_editor->promptBar()->engineChosenInEditor())
        m_engineButton->setToolTip(tr("The image engine the prompt bar uses now (%1), chosen in the editor. Click to choose the "
                                      "engines in Settings → AI; a choice there replaces it.")
                                       .arg(engine->displayName()));
    else
        m_engineButton->setToolTip(tr("The image engine the prompt bar uses now (%1). Click to choose the engines for new images and "
                                      "for changes in Settings → AI.")
                                       .arg(engine->displayName()));
}

void MainWindow::updateTitle()
{
    doc::Project *project = m_studio->project();
    const bool open = project && project->isOpen();
    const QString name = open ? project->displayName() : QString();
    setWindowTitle(name.isEmpty() ? QStringLiteral("Lumen Studio") : QStringLiteral("%1 — Lumen Studio").arg(name));
    m_projectLabel->setFullText(open ? tr("Project: %1").arg(name) : tr("No project"));
    m_projectLabel->setToolTip(open ? QDir::toNativeSeparators(project->path()) : QString());
}

void MainWindow::notify(const QString &message, const QString &level)
{
    // In the window the user works in: the editor, a plugin page's or Browse project images.
    QWidget *active = QApplication::activeWindow();
    if (active && active != this) {
        if (auto *toasts = active->findChild<Toasts *>(QString(), Qt::FindDirectChildrenOnly)) {
            toasts->show(message, level);
            return;
        }
    }
    m_toasts->show(message, level);
}

void MainWindow::newProject()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("New project"), paths::defaultProjects(),
                                                      tr("Lumen project (*.lumen)"));
    if (path.isEmpty())
        return;
    QString file = path;
    if (!file.endsWith(QStringLiteral(".lumen")))
        file += QStringLiteral(".lumen");
    if (QFileInfo::exists(file)) {
        QMessageBox::warning(this, tr("New project"), tr("That file already exists. Use Open project to open it."));
        return;
    }
    m_studio->openProject(file);
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open project"), paths::defaultProjects(), tr("Lumen project (*.lumen)"));
    if (!path.isEmpty())
        m_studio->openProject(path);
}

void MainWindow::openImages()
{
    // Where the last picture opened or saved is, as other editors start.
    const QString recent = Settings::instance().recentFiles().value(0);
    const QString folder = recent.isEmpty() ? QString() : QFileInfo(recent).absolutePath();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open images"), QFileInfo(folder).isDir() ? folder : QString(),
                                                            Studio::openFileFilter());
    for (const QString &f : files)
        m_studio->openFile(f);
}

void MainWindow::openSettings(const QString &tab)
{
    SettingsDialog dlg(m_studio, this);
    if (!tab.isEmpty())
        dlg.showTab(tab);
    dlg.exec();
}

SetupWizard *MainWindow::setUpAi()
{
    // Asked for once is enough: the first start does not open it again.
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
    return openSetupWizard(m_studio, this);
}

void MainWindow::maybeShowOnboarding()
{
    if (!m_studio->providers()->providers().isEmpty() || Settings::instance().value(QStringLiteral("app/onboarded")).toBool())
        return;
    // Not on top of another question (the models folder at start-up); once that is answered.
    if (QApplication::activeModalWidget()) {
        QTimer::singleShot(400, this, &MainWindow::maybeShowOnboarding);
        return;
    }
    // Asked once: later Help → Set up AI…, the start screen, the prompt bar and the status bar open it.
    setUpAi();
}

Job *MainWindow::offerModelsFolderMove(const std::function<bool(const runtime::FolderMovePlan &plan)> &ask)
{
    if (m_studio->legacyModelsFolder().isEmpty())
        return nullptr;
    // Not on top of another dialog (the first-run welcome); once that is closed.
    if (!ask && QApplication::activeModalWidget()) {
        QTimer::singleShot(400, this, [this] { offerModelsFolderMove(); });
        return nullptr;
    }
    const QString title = tr("Models folder");
    auto question = [this, title](const runtime::FolderMovePlan &plan) {
        if (!plan.ok()) {
            QMessageBox::information(this, title,
                                     tr("Lumen keeps using %1 for models: %2\n\nYou can move them later in Settings → Models.")
                                         .arg(QDir::toNativeSeparators(plan.from), plan.error));
            return false;
        }
        QMessageBox box(QMessageBox::Question, title,
                        tr("Lumen now keeps models in a folder that is easy to find: %1\n\nThe models of the earlier version are in %2 "
                           "(%n file(s), %3). Move them to the new folder?",
                           nullptr, plan.models)
                            .arg(QDir::toNativeSeparators(plan.to), QDir::toNativeSeparators(plan.from), runtime::formatBytes(plan.bytes)),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr("Server settings that use them are updated. You can move the folder later in Settings → Models."));
        QPushButton *move = box.addButton(tr("Move models"), QMessageBox::AcceptRole);
        box.addButton(tr("Keep them where they are"), QMessageBox::RejectRole);
        box.setDefaultButton(move);
        box.exec();
        return box.clickedButton() == move;
    };
    Job *job = m_studio->settleLegacyModels(ask ? ask : std::function<bool(const runtime::FolderMovePlan &)>(question));
    if (job)
        m_studio->notify(tr("Moving the models — progress is shown in the Queue."));
    return job;
}

void MainWindow::checkLeftoverCloudPods()
{
    runtime::RunPodClient *runPod = m_studio->runPod();
    if (!runPod->hasKey())
        return;
    QPointer<MainWindow> self(this);
    runtime::leftoverCloudPods(runPod)
        .then(this,
              [self, runPod](const QList<runtime::RunPodPod> &pods) {
                  if (!self || pods.isEmpty())
                      return;
                  double perHour = 0;
                  QStringList names;
                  for (const runtime::RunPodPod &p : pods) {
                      perHour += p.costPerHour;
                      names << QStringLiteral("%1 (%2)").arg(p.name, p.status.toLower());
                  }
                  QMessageBox box(QMessageBox::Warning, tr("Cloud GPUs still rented"),
                                  tr("%n cloud GPU(s) of earlier trainings are still rented on RunPod.", nullptr, int(pods.size())),
                                  QMessageBox::Yes | QMessageBox::No, self);
                  box.setObjectName(QStringLiteral("leftoverPods"));
                  box.setInformativeText(tr("No training of this Lumen uses them. Together they cost about $%1 per hour (a stopped "
                                            "one still pays for its disk). Shut them down? Their files are lost.\n\n%2")
                                             .arg(perHour, 0, 'f', 2)
                                             .arg(names.join(u'\n')));
                  box.button(QMessageBox::Yes)->setText(tr("Shut them down"));
                  box.button(QMessageBox::No)->setText(tr("Keep them"));
                  if (box.exec() != QMessageBox::Yes)
                      return;
                  for (const runtime::RunPodPod &p : pods) {
                      const QString name = p.name;
                      runPod->terminate(p.id)
                          .then(self, [self, name] {
                              if (self)
                                  self->m_studio->notify(tr("Cloud GPU %1 is shut down.").arg(name), QStringLiteral("success"));
                          })
                          .onFailed(self, [self, name](const std::exception &e) {
                              if (self)
                                  self->m_studio->notify(tr("Cloud GPU %1 could not be shut down: %2. Stop it at runpod.io.")
                                                             .arg(name, QString::fromUtf8(e.what())),
                                                         QStringLiteral("error"));
                          });
                  }
              })
        .onFailed(this, [](const std::exception &e) { qCWarning(lcApp) << "Leftover cloud GPUs:" << e.what(); });
}

bool MainWindow::saveScreenshot(const QString &path)
{
    // The window opened last (--page poses, generate...), else the editor.
    QWidget *shown = m_lastOpened && m_lastOpened->isVisible() ? m_lastOpened.data() : this;
    return shown->grab().save(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    Settings::instance().setValue(QStringLiteral("ui/geometry"), saveGeometry());
    if (m_closing) {
        // The windows of plugin pages and of the project images close with the editor.
        for (const QPointer<PageWindow> &w : std::as_const(m_windows)) {
            if (w)
                w->close();
        }
        if (m_projectImages)
            m_projectImages->close();
        // So do the filter editors; they asked about unsaved changes when the editor was closed.
        for (NodeEditorWindow *w : NodeEditorWindow::openWindows()) {
            if (w->parentWidget() == this)
                w->close();
        }
        e->accept();
        return;
    }
    e->ignore();
    if (m_quitting)
        return; // the questions or the saves of a quit are on their way
    m_quitting = true;
    // Every question before anything is done about an answer: the running jobs first, then the
    // pictures with changes not saved to their files (Save / Don't save / Cancel, as in other
    // editors), whose "Don't save" closes them and so cancels their prompt-bar runs. Cancel on
    // either leaves everything as it was. Saving a picture may ask too (the first time it saves
    // over one), so this goes on when the answers are in; the window closes once all is on disk.
    const int running = m_studio->jobs()->activeCount() + m_studio->jobs()->pendingCount();
    if (running > 0) {
        const auto answer = QMessageBox::question(this, tr("Quit"), tr("%n job(s) are still running. Quit anyway?", nullptr, running));
        if (answer != QMessageBox::Yes) {
            m_quitting = false;
            return;
        }
    }
    m_editor->files()->confirmQuit().then(this, [this](bool quit) {
        if (!quit) {
            m_quitting = false;
            return;
        }
        m_studio->jobs()->cancelAll();
        // Save open documents (and which are open, for the next start) before quitting. The close is
        // queued: with nothing to save the futures are finished already and the continuation may run
        // inside the close event, where a nested close() does nothing.
        m_closing = true;
        statusBar()->showMessage(tr("Saving..."));
        auto closeLater = [this] { QTimer::singleShot(0, this, [this] { close(); }); };
        m_studio->saveAll()
            .then(this, [this] { return m_studio->saveSession(); })
            .unwrap()
            .then(this, closeLater)
            .onFailed(this, [closeLater](const std::exception &) { closeLater(); });
    });
}

void MainWindow::fillRecentFiles(QMenu *menu)
{
    menu->clear();
    const QStringList files = Settings::instance().recentFiles();
    QStringList names;
    for (const QString &path : files)
        names << QFileInfo(path).fileName();
    for (const QString &path : files) {
        const QFileInfo info(path);
        // Two files of the same name say which folder they are in.
        const QString name = names.count(info.fileName()) > 1 ? QStringLiteral("%1 (%2)").arg(info.fileName(), info.dir().dirName())
                                                               : info.fileName();
        QAction *a = menu->addAction(name, this, [this, path] { m_studio->openFile(path); });
        const bool there = info.exists();
        a->setEnabled(there);
        a->setToolTip(there ? QDir::toNativeSeparators(path) : tr("%1 is not there any more").arg(QDir::toNativeSeparators(path)));
    }
    if (files.isEmpty()) {
        menu->addAction(tr("(none)"))->setEnabled(false);
        return;
    }
    menu->addSeparator();
    menu->addAction(tr("Clear the list"), this, [] { Settings::instance().removeRecentFile(QString()); });
}

} // namespace lumen::ui
