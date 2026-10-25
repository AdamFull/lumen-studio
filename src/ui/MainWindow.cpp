#include "ui/MainWindow.h"

#include "core/Error.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "runtime/CloudTraining.h"
#include "runtime/Hardware.h"
#include "ui/Studio.h"
#include "ui/Icons.h"
#include "ui/PageFactory.h"
#include "ui/Shortcuts.h"
#include "ui/Theme.h"
#include "ui/canvas/ContextBar.h"
#include "ui/canvas/EditorState.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/widgets/ServerStatus.h"
#include "ui/pages/EditorPage.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/JobCard.h"
#include "ui/widgets/Toasts.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDesktopServices>
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
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

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

    m_pageSpecs = corePages() + ext::Extensions::instance().pages();
    std::stable_sort(m_pageSpecs.begin(), m_pageSpecs.end(), [](const ext::PageSpec &a, const ext::PageSpec &b) { return a.order < b.order; });
    for (const ext::PanelSpec &spec : ext::Extensions::instance().panels())
        m_panels.append(Panel{spec, nullptr, nullptr});

    auto *central = new QWidget(this);
    auto *h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    h->addWidget(buildNavRail());
    h->addWidget(separator(Qt::Vertical, central));
    m_pages = new QStackedWidget(central);
    h->addWidget(m_pages, 1);
    setCentralWidget(central);

    for (const ext::PageSpec &spec : std::as_const(m_pageSpecs)) {
        QWidget *w = spec.create(studio, m_pages);
        m_pages->addWidget(w);
        m_pageWidgets.insert(spec.id, w);
    }
    m_editor = qobject_cast<EditorPage *>(m_pageWidgets.value(pages::Editor));

    m_toasts = new Toasts(central);
    m_projectLabel = new QLabel(this);
    m_projectLabel->setProperty("role", QStringLiteral("muted"));
    m_jobLabel = new QLabel(this);
    m_jobLabel->setProperty("role", QStringLiteral("muted"));
    statusBar()->addWidget(m_projectLabel);
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
    statusBar()->addPermanentWidget(new ServerStatusButton(studio, [this] { openSettings(QStringLiteral("servers")); }, this));
    statusBar()->addPermanentWidget(m_jobLabel);
    statusBar()->setSizeGripEnabled(false);

    buildMenus();

    connect(studio, &Studio::pageRequested, this, &MainWindow::showPage);
    connect(studio, &Studio::notification, this, [this](const QString &msg, const QString &level) { m_toasts->show(msg, level); });
    connect(studio, &Studio::projectOpened, this, &MainWindow::updateTitle);
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
    // The remembered page, else the first on the rail. Before plugins, "ui/page" was the index of a
    // fixed list of pages.
    QString lastPage = Settings::instance().value(QStringLiteral("ui/page")).toString();
    bool legacy = false;
    if (const int index = lastPage.toInt(&legacy); legacy) {
        static const QStringList legacyPages = {QStringLiteral("create"), QStringLiteral("generate"), QStringLiteral("editor"),
                                                QStringLiteral("poses"),  QStringLiteral("library"),  QStringLiteral("parts")};
        lastPage = legacyPages.value(index);
    }
    showPage(m_pageWidgets.contains(lastPage) ? lastPage : m_pageSpecs.constFirst().id);
    updateTitle();
    updateJobSummary();
    QTimer::singleShot(400, this, &MainWindow::maybeShowOnboarding);
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildNavRail()
{
    auto *rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("Panel"));
    rail->setFixedWidth(86);
    auto *v = new QVBoxLayout(rail);
    v->setContentsMargins(0, 12, 0, 12);
    v->setSpacing(4);
    auto *logo = new QLabel(rail);
    logo->setPixmap(Icons::pixmap(QStringLiteral("logo"), 30, Theme::palette().accent));
    logo->setAlignment(Qt::AlignCenter);
    logo->setToolTip(QStringLiteral("Lumen Studio"));
    v->addWidget(logo);
    v->addSpacing(14);
    m_nav = new QButtonGroup(this);
    for (int index = 0; index < m_pageSpecs.size(); ++index) {
        const ext::PageSpec &spec = m_pageSpecs.at(index);
        auto *b = new QToolButton(rail);
        b->setIcon(Icons::get(spec.icon));
        b->setIconSize(QSize(22, 22));
        double pointSize = 8.0;
        b->setText(fitLabel(spec.label, 68, &pointSize));
        b->setAccessibleName(spec.label);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setFixedSize(74, 58);
        b->setProperty("role", QStringLiteral("nav"));
        b->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(pointSize));
        const QString tip = spec.tooltip;
        Shortcuts::Info info{QStringLiteral("nav.%1").arg(spec.icon), tr("Pages"), tr("Go to %1").arg(spec.label), QStringLiteral("global"),
                             spec.shortcut.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{spec.shortcut}};
        Shortcuts::instance().bind(info, b, [b, tip](const QList<QKeySequence> &keys) {
            b->setShortcut(keys.value(0));
            b->setToolTip(keys.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, Shortcuts::toText(keys)));
        });
        m_nav->addButton(b, index);
        v->addWidget(b, 0, Qt::AlignHCenter);
    }
    connect(m_nav, &QButtonGroup::idClicked, this, [this](int index) { showPage(m_pageSpecs.value(index).id); });
    v->addStretch();
    // Panels of plugins (the assistant), shown over the page area.
    for (int index = 0; index < m_panels.size(); ++index) {
        const ext::PanelSpec &spec = m_panels.at(index).spec;
        auto *b = new QToolButton(rail);
        b->setIcon(Icons::get(spec.icon));
        b->setIconSize(QSize(20, 20));
        double pointSize = 8.0;
        b->setText(fitLabel(spec.label, 68, &pointSize));
        b->setAccessibleName(spec.label);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setFixedSize(74, 52);
        b->setProperty("role", QStringLiteral("nav"));
        b->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(pointSize));
        b->setCheckable(true);
        b->setToolTip(spec.tooltip);
        if (!spec.shortcutId.isEmpty()) {
            const QString tip = spec.tooltip;
            Shortcuts::instance().bind(Shortcuts::Info{spec.shortcutId, tr("General"), spec.label, QStringLiteral("global"), {spec.shortcut}}, b,
                                       [b, tip](const QList<QKeySequence> &keys) {
                                           b->setShortcut(keys.value(0));
                                           b->setToolTip(keys.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, Shortcuts::toText(keys)));
                                       });
        }
        connect(b, &QToolButton::clicked, this, [this, index] { togglePanel(index); });
        m_panels[index].button = b;
        v->addWidget(b, 0, Qt::AlignHCenter);
    }
    m_queueButton = new QToolButton(rail);
    m_queueButton->setIcon(Icons::get(QStringLiteral("queue")));
    m_queueButton->setIconSize(QSize(20, 20));
    double queueSize = 8.0;
    m_queueButton->setText(fitLabel(tr("Queue"), 68, &queueSize));
    m_queueButton->setAccessibleName(tr("Queue"));
    m_queueButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_queueButton->setFixedSize(74, 52);
    m_queueButton->setProperty("role", QStringLiteral("nav"));
    m_queueButton->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(queueSize));
    m_queueButton->setCheckable(true);
    m_queueButton->setToolTip(tr("Show running and finished jobs"));
    connect(m_queueButton, &QToolButton::clicked, this, &MainWindow::toggleQueue);
    v->addWidget(m_queueButton, 0, Qt::AlignHCenter);
    auto *settings = new QToolButton(rail);
    settings->setIcon(Icons::get(QStringLiteral("settings")));
    settings->setIconSize(QSize(20, 20));
    double settingsSize = 8.0;
    settings->setText(fitLabel(tr("Settings"), 68, &settingsSize));
    settings->setAccessibleName(tr("Settings"));
    settings->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    settings->setFixedSize(74, 52);
    settings->setProperty("role", QStringLiteral("nav"));
    settings->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(settingsSize));
    settings->setToolTip(tr("Providers, models and preferences (Ctrl+,)"));
    connect(settings, &QToolButton::clicked, this, [this] { openSettings(); });
    v->addWidget(settings, 0, Qt::AlignHCenter);
    return rail;
}

void MainWindow::buildMenus()
{
    const QString general = tr("General");
    auto bind = [general](QAction *action, const QString &id) {
        Shortcuts::instance().bindAction(action, QStringLiteral("app.") + id, general, QStringLiteral("global"));
    };
    QMenu *file = menuBar()->addMenu(tr("&File"));
    bind(file->addAction(Icons::get(QStringLiteral("plus")), tr("New document..."), QKeySequence::New, this, [this] {
        showPage(pages::Editor);
        m_editor->newDocumentDialog();
    }), QStringLiteral("new-document"));
    bind(file->addAction(Icons::get(QStringLiteral("image")), tr("Open image..."), QKeySequence::Open, this, &MainWindow::openImages), QStringLiteral("open-image"));
    bind(file->addAction(Icons::get(QStringLiteral("upload")), tr("Import images to library..."), this, &MainWindow::importToLibrary), QStringLiteral("import-library"));
    file->addSeparator();
    bind(file->addAction(Icons::get(QStringLiteral("save")), tr("Save all"), QKeySequence::Save, this, [this] {
        m_studio->saveAll().then(this, [this] { m_studio->notify(tr("Project saved."), QStringLiteral("success")); });
    }), QStringLiteral("save-all"));
    bind(file->addAction(Icons::get(QStringLiteral("download")), tr("Export image..."), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this,
                    [this] { m_editor->exportImage(); }), QStringLiteral("export-image"));
    bind(file->addAction(Icons::get(QStringLiteral("gallery")), tr("Save image to library"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), this,
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
    bind(view->addAction(Icons::get(QStringLiteral("queue")), tr("Job queue"), QKeySequence(QStringLiteral("Ctrl+Shift+Q")), this, [this] {
        m_queueButton->setChecked(!m_queueButton->isChecked());
        toggleQueue();
    }), QStringLiteral("job-queue"));
    QAction *contextBar = view->addAction(Icons::get(QStringLiteral("sparkles")), tr("Generative toolbar on the canvas"));
    bind(contextBar, QStringLiteral("context-bar"));
    contextBar->setCheckable(true);
    contextBar->setChecked(m_editor && m_editor->editorState()->showContextBar());
    connect(contextBar, &QAction::toggled, this, [this](bool on) {
        if (!m_editor)
            return;
        m_editor->editorState()->setShowContextBar(on);
        m_editor->contextBar()->setEnabledByUser(on);
    });
    view->addAction(Icons::get(Theme::instance().isDark() ? QStringLiteral("sun") : QStringLiteral("moon")), tr("Toggle light/dark theme"), this, [this] {
        const QString next = Theme::instance().isDark() ? QStringLiteral("light") : QStringLiteral("dark");
        Settings::instance().setValue(QStringLiteral("ui/theme"), next);
        QMessageBox::information(this, tr("Theme"), tr("The %1 theme will be applied the next time Lumen Studio starts.").arg(next));
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
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
    QWidget *w = m_pageWidgets.value(id);
    if (!w)
        return;
    m_pages->setCurrentWidget(w);
    for (int index = 0; index < m_pageSpecs.size(); ++index) {
        if (m_pageSpecs.at(index).id == id)
            m_nav->button(index)->setChecked(true);
    }
    Settings::instance().setValue(QStringLiteral("ui/page"), id);
}

QStringList MainWindow::pageIds() const
{
    QStringList ids;
    for (const ext::PageSpec &spec : m_pageSpecs)
        ids << spec.id;
    return ids;
}

void MainWindow::toggleQueue()
{
    if (!m_queuePanel) {
        m_queuePanel = new QFrame(centralWidget());
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
        connect(close, &QToolButton::clicked, this, [this] {
            m_queueButton->setChecked(false);
            m_queuePanel->hide();
        });
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
    const bool show = m_queueButton->isChecked();
    m_queuePanel->setGeometry(94, std::max(20, centralWidget()->height() - 560), 440, std::min(540, centralWidget()->height() - 40));
    m_queuePanel->setVisible(show);
    if (show)
        m_queuePanel->raise();
}

void MainWindow::togglePanel(int index)
{
    Panel &panel = m_panels[index];
    if (!panel.widget) {
        panel.widget = panel.spec.create(m_studio, centralWidget(), [this, index] {
            Panel &p = m_panels[index];
            p.button->setChecked(false);
            if (p.widget)
                p.widget->hide();
        });
    }
    panel.widget->setVisible(panel.button->isChecked());
    placePanels();
}

void MainWindow::placePanels()
{
    const QSize area = centralWidget()->size();
    const int width = std::min(460, std::max(320, area.width() / 3));
    for (const Panel &panel : std::as_const(m_panels)) {
        if (!panel.widget || !panel.widget->isVisible())
            continue;
        panel.widget->setGeometry(area.width() - width - 12, 12, width, area.height() - 24);
        panel.widget->raise();
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
        m_jobLabel->setText(tr("Idle"));
    else
        m_jobLabel->setText(tr("%1 running · %2 queued").arg(active).arg(pending));
    double pointSize = 8.0;
    m_queueButton->setText(fitLabel(active + pending > 0 ? tr("Queue (%1)").arg(active + pending) : tr("Queue"), 68, &pointSize));
    m_queueButton->setStyleSheet(QStringLiteral("font-size: %1pt;").arg(pointSize));
}

void MainWindow::updateTitle()
{
    doc::Project *project = m_studio->project();
    const QString name = project && project->isOpen() ? project->displayName() : QString();
    setWindowTitle(name.isEmpty() ? QStringLiteral("Lumen Studio") : QStringLiteral("%1 — Lumen Studio").arg(name));
    m_projectLabel->setText(project && project->isOpen() ? tr("Project: %1").arg(project->path()) : tr("No project"));
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

void MainWindow::importToLibrary()
{
    doc::Project *project = m_studio->project();
    if (!project || !project->isOpen())
        return;
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import images"), QString(), Studio::openFileFilter());
    for (const QString &f : files) {
        project->importFile(f).then([](doc::AssetInfo) {}).onFailed(this, [this](const std::exception &e) {
            m_studio->notify(describeException(e), QStringLiteral("error"));
        });
    }
    if (!files.isEmpty()) {
        m_studio->notify(tr("Importing %n image(s)...", nullptr, int(files.size())));
        showPage(pages::Library);
    }
}

void MainWindow::openSettings(const QString &tab)
{
    SettingsDialog dlg(m_studio, this);
    if (!tab.isEmpty())
        dlg.showTab(tab);
    dlg.exec();
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
    OnboardingDialog dlg(m_studio, this);
    dlg.exec();
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
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
    return grab().save(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    Settings::instance().setValue(QStringLiteral("ui/geometry"), saveGeometry());
    if (m_closing) {
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
    // Save open documents before quitting; the window closes when everything is on disk.
    e->ignore();
    m_closing = true;
    statusBar()->showMessage(tr("Saving..."));
    m_studio->saveAll().then(this, [this] { close(); }).onFailed(this, [this](const std::exception &) { close(); });
}

} // namespace lumen::ui
