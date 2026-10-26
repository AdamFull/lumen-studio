#include "ui/MainWindow.h"

#include "core/Error.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "runtime/CloudTraining.h"
#include "runtime/Hardware.h"
#include "ui/Studio.h"
#include "ui/Icons.h"
#include "ui/Shortcuts.h"
#include "ui/Theme.h"
#include "ui/canvas/ContextBar.h"
#include "ui/canvas/EditorState.h"
#include "ui/dialogs/NewFromPromptDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/widgets/ServerStatus.h"
#include "ui/pages/EditorPage.h"
#include "ui/pages/LibraryPage.h"
#include "ui/pages/StartScreen.h"
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

    // The image engine in plain words; Settings → Providers changes it.
    m_engineButton = new QToolButton(this);
    m_engineButton->setObjectName(QStringLiteral("imageEngine"));
    m_engineButton->setAutoRaise(true);
    m_engineButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_engineButton->setAccessibleName(tr("Image engine"));
    connect(m_engineButton, &QToolButton::clicked, this, [this] { openSettings(QStringLiteral("providers")); });
    statusBar()->addPermanentWidget(m_engineButton);
    statusBar()->addPermanentWidget(new ServerStatusButton(m_studio, [this] { openSettings(QStringLiteral("servers")); }, this));

    // "Safe for work" in one click (also in Settings → General).
    auto *safe = new QToolButton(this);
    safe->setObjectName(QStringLiteral("safeContent"));
    safe->setCheckable(true);
    safe->setAutoRaise(true);
    safe->setIcon(Icons::get(QStringLiteral("shield")));
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
    auto bind = [general](QAction *action, const QString &id) {
        Shortcuts::instance().bindAction(action, QStringLiteral("app.") + id, general, QStringLiteral("global"));
    };
    // Actions that replace a page of the old navigation rail keep its shortcut id and key.
    auto bindPage = [general](QAction *action, const QString &icon, const QString &name, const QKeySequence &key) {
        Shortcuts::instance().bindAction(action, QStringLiteral("nav.%1").arg(icon), general, QStringLiteral("global"),
                                         key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key}, name);
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    bind(file->addAction(Icons::get(QStringLiteral("plus")), tr("New blank canvas..."), QKeySequence::New, this, [this] {
        showPage(pages::Editor);
        m_editor->newDocumentDialog();
    }), QStringLiteral("new-document"));
    QAction *fromPrompt = file->addAction(Icons::get(QStringLiteral("sparkles")), tr("New from prompt..."), this, &MainWindow::newFromPrompt);
    fromPrompt->setObjectName(QStringLiteral("newFromPrompt"));
    bindPage(fromPrompt, QStringLiteral("sparkles"), tr("New from prompt"), QKeySequence(QStringLiteral("Alt+2")));
    bind(file->addAction(Icons::get(QStringLiteral("image")), tr("Open image..."), QKeySequence::Open, this, &MainWindow::openImages), QStringLiteral("open-image"));
    bind(file->addAction(Icons::get(QStringLiteral("copy")), tr("Paste from clipboard"), this, [this] {
        showPage(pages::Editor);
        m_editor->pasteAsNewDocument();
    }), QStringLiteral("paste-new-document"));
    QAction *browse = file->addAction(Icons::get(QStringLiteral("gallery")), tr("Browse project images..."), this, &MainWindow::browseProjectImages);
    browse->setObjectName(QStringLiteral("browseProjectImages"));
    bindPage(browse, QStringLiteral("gallery"), tr("Browse project images"), QKeySequence(QStringLiteral("Alt+6")));
    file->addSeparator();
    bind(file->addAction(Icons::get(QStringLiteral("save")), tr("Save all"), QKeySequence::Save, this, [this] {
        m_studio->saveAll().then(this, [this] { m_studio->notify(tr("Project saved."), QStringLiteral("success")); });
    }), QStringLiteral("save-all"));
    bind(file->addAction(Icons::get(QStringLiteral("download")), tr("Export image..."), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this,
                    [this] { m_editor->exportImage(); }), QStringLiteral("export-image"));
    bind(file->addAction(Icons::get(QStringLiteral("gallery")), tr("Save a copy to project images"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), this,
                    [this] { m_editor->saveToLibrary(); }), QStringLiteral("save-to-library"));
    bind(file->addAction(tr("Close document"), QKeySequence::Close, this, [this] { m_studio->closeDocument(m_studio->activeDocument()); }), QStringLiteral("close-document"));
    file->addSeparator();
    bind(file->addAction(Icons::get(QStringLiteral("folder")), tr("New project..."), this, &MainWindow::newProject), QStringLiteral("new-project"));
    bind(file->addAction(Icons::get(QStringLiteral("folder")), tr("Open project..."), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this, &MainWindow::openProject), QStringLiteral("open-project"));
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
    bind(file->addAction(tr("Quit"), QKeySequence::Quit, this, &QWidget::close), QStringLiteral("quit"));

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    bind(edit->addAction(Icons::get(QStringLiteral("copy")), tr("Copy merged"), QKeySequence(QStringLiteral("Ctrl+Shift+C")), this, [this] { m_editor->copyMerged(); }), QStringLiteral("copy-merged"));
    bind(edit->addAction(tr("Paste as layer"), this, [this] {
        showPage(pages::Editor);
        m_editor->pasteAsLayer();
    }), QStringLiteral("paste-layer-menu"));
    bind(edit->addAction(tr("Import image as layer..."), this, [this] {
        showPage(pages::Editor);
        m_editor->importAsLayer();
    }), QStringLiteral("import-layer"));
    edit->addSeparator();
    bind(edit->addAction(Icons::get(QStringLiteral("settings")), tr("Settings..."), QKeySequence(QStringLiteral("Ctrl+,")), this, [this] { openSettings(); }), QStringLiteral("settings"));

    QMenu *view = menuBar()->addMenu(tr("&View"));
    m_queueAction = view->addAction(Icons::get(QStringLiteral("queue")), tr("Job queue"));
    m_queueAction->setObjectName(QStringLiteral("jobQueue"));
    m_queueAction->setCheckable(true);
    m_queueAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")));
    bind(m_queueAction, QStringLiteral("job-queue"));
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
        connect(a, &QAction::toggled, this, [this, index](bool on) { setPanelVisible(index, on); });
        m_panels[index].action = a;
    }
    view->addSeparator();
    QAction *contextBar = view->addAction(Icons::get(QStringLiteral("sparkles")), tr("Generative toolbar on the canvas"));
    bind(contextBar, QStringLiteral("context-bar"));
    contextBar->setCheckable(true);
    contextBar->setChecked(m_editor->editorState()->showContextBar());
    connect(contextBar, &QAction::toggled, this, [this](bool on) {
        m_editor->editorState()->setShowContextBar(on);
        m_editor->contextBar()->setEnabledByUser(on);
    });
    view->addAction(Icons::get(Theme::instance().isDark() ? QStringLiteral("sun") : QStringLiteral("moon")), tr("Toggle light/dark theme"), this, [this] {
        const QString next = Theme::instance().isDark() ? QStringLiteral("light") : QStringLiteral("dark");
        Settings::instance().setValue(QStringLiteral("ui/theme"), next);
        QMessageBox::information(this, tr("Theme"), tr("The %1 theme will be applied the next time Lumen Studio starts.").arg(next));
    });

    // One action per plugin page; each opens in a window of its own.
    QMenu *tools = menuBar()->addMenu(tr("&Tools"));
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
    QAction *plugins = tools->addAction(Icons::get(QStringLiteral("settings")), tr("Plugins..."), this, [this] { openSettings(QStringLiteral("plugins")); });
    plugins->setToolTip(tr("Character creation, poses and Live2D parts are plugins: switch them on in Settings → Plugins"));
    plugins->setStatusTip(plugins->toolTip());

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    QAction *setup = help->addAction(Icons::get(QStringLiteral("sparkles")), tr("Set up AI..."), this, &MainWindow::setUpAi);
    setup->setObjectName(QStringLiteral("setUpAi"));
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
    help->addAction(Icons::get(QStringLiteral("folder")), tr("Open ComfyUI workflow folder"), this, [] {
        const QString dir = paths::appData() + QStringLiteral("/workflows/comfyui");
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    help->addSeparator();
    help->addAction(Icons::get(QStringLiteral("info")), tr("About Lumen Studio"), this, [this] {
        QMessageBox::about(this, tr("About Lumen Studio"),
                           tr("<h3>Lumen Studio %1</h3><p>AI-assisted image generation and editing.</p>"
                              "<p>Built with Qt %2.</p>").arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    });
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
        auto *clear = new QToolButton(m_queuePanel);
        clear->setText(tr("Clear finished"));
        connect(clear, &QToolButton::clicked, m_studio->jobs(), &JobQueue::clearFinished);
        auto *cancelAll = new QToolButton(m_queuePanel);
        cancelAll->setText(tr("Cancel all"));
        connect(cancelAll, &QToolButton::clicked, m_studio->jobs(), &JobQueue::cancelAll);
        auto *close = iconButton(QStringLiteral("close"), tr("Close"), m_queuePanel);
        connect(close, &QToolButton::clicked, this, [this] { setQueueVisible(false); });
        head->addWidget(cancelAll);
        head->addWidget(clear);
        head->addWidget(close);
        v->addLayout(head);
        auto *scroll = new QScrollArea(m_queuePanel);
        scroll->setWidgetResizable(true);
        auto *content = new QWidget(scroll);
        m_queueList = new QVBoxLayout(content);
        m_queueList->setContentsMargins(0, 0, 4, 0);
        m_queueList->setSpacing(6);
        m_queueList->addStretch();
        scroll->setWidget(content);
        v->addWidget(scroll, 1);
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
    placePanels();
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
    const int width = std::min(460, std::max(320, area.width() / 3));
    int right = area.width() - 12; // the queue stays left of an open panel
    for (const Panel &panel : std::as_const(m_panels)) {
        if (!panel.widget || !panel.widget->isVisible())
            continue;
        panel.widget->setGeometry(area.width() - width - 12, 12, width, area.height() - 24);
        panel.widget->raise();
        right = area.width() - width - 24;
    }
    // Above the jobs button of the status bar.
    if (m_queuePanel && m_queuePanel->isVisible()) {
        const int w = std::min(440, right - 12);
        const int h = std::min(540, area.height() - 24);
        m_queuePanel->setGeometry(right - w, area.height() - h - 12, w, h);
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
    const ai::ImageProvider *engine = imageEngineInUse(m_studio->providers());
    if (!engine) {
        m_engineButton->setText(tr("No image engine - Set up AI"));
        m_engineButton->setIcon(QIcon(Icons::pixmap(QStringLiteral("warning"), 14, Theme::palette().warning)));
        m_engineButton->setToolTip(tr("Nothing can draw or change images yet. Click to add an image engine in Settings → Providers, "
                                      "or use Help → Set up AI."));
        return;
    }
    m_engineButton->setText(imageEngineName(engine));
    m_engineButton->setIcon(Icons::get(engine->type().local ? QStringLiteral("server") : QStringLiteral("cloud")));
    m_engineButton->setToolTip(tr("The image engine new images and edits use (%1). Click to change it in Settings → Providers.")
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
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open images"), QString(), Studio::openFileFilter());
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

void MainWindow::setUpAi()
{
    OnboardingDialog dlg(m_studio, this);
    dlg.exec();
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
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
        e->accept();
        return;
    }
    const int running = m_studio->jobs()->activeCount() + m_studio->jobs()->pendingCount();
    if (running > 0) {
        const auto answer = QMessageBox::question(this, tr("Quit"), tr("%n job(s) are still running. Quit anyway?", nullptr, running));
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        m_studio->jobs()->cancelAll();
    }
    // Save open documents before quitting; the window closes when everything is on disk. The close
    // is queued: with nothing to save the future is already finished and its continuation runs
    // here, inside this close event, where a nested close() does nothing.
    e->ignore();
    m_closing = true;
    statusBar()->showMessage(tr("Saving..."));
    auto closeLater = [this] { QTimer::singleShot(0, this, [this] { close(); }); };
    m_studio->saveAll().then(this, closeLater).onFailed(this, [closeLater](const std::exception &) { closeLater(); });
}

} // namespace lumen::ui
