#include "ui/MainWindow.h"

#include "core/Error.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ui/Icons.h"
#include "ui/PageFactory.h"
#include "ui/Theme.h"
#include "ui/dialogs/SettingsDialog.h"
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
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

MainWindow::MainWindow(Studio *studio, QWidget *parent)
    : QMainWindow(parent)
    , m_studio(studio)
{
    setWindowTitle(QStringLiteral("Lumen Studio"));
    setWindowIcon(Icons::get(QStringLiteral("logo")));
    resize(1500, 920);
    setMinimumSize(1024, 640);

    auto *central = new QWidget(this);
    auto *h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    h->addWidget(buildNavRail());
    h->addWidget(separator(Qt::Vertical, central));
    m_pages = new QStackedWidget(central);
    h->addWidget(m_pages, 1);
    setCentralWidget(central);

    for (Page page : {Page::Create, Page::Generate, Page::Editor, Page::Poses, Page::Library}) {
        QWidget *w = createPage(page, studio, m_pages);
        m_pages->addWidget(w);
        m_pageWidgets.insert(page, w);
        if (page == Page::Editor)
            m_editor = qobject_cast<EditorPage *>(w);
    }

    m_toasts = new Toasts(central);
    m_projectLabel = new QLabel(this);
    m_projectLabel->setProperty("role", QStringLiteral("muted"));
    m_jobLabel = new QLabel(this);
    m_jobLabel->setProperty("role", QStringLiteral("muted"));
    statusBar()->addWidget(m_projectLabel);
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
    const int lastPage = Settings::instance().value(QStringLiteral("ui/page"), static_cast<int>(Page::Create)).toInt();
    showPage(static_cast<Page>(std::clamp(lastPage, 0, 4)));
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
    const QList<std::tuple<Page, QString, QString, QString>> items = {
        {Page::Create, QStringLiteral("chat"), tr("Create"), tr("AI director: plan character shots from references and a description (Alt+1)")},
        {Page::Generate, QStringLiteral("sparkles"), tr("Generate"), tr("Text/image to image generation (Alt+2)")},
        {Page::Editor, QStringLiteral("brush"), tr("Edit"), tr("Layered editor with inpainting and AI tools (Alt+3)")},
        {Page::Poses, QStringLiteral("person"), tr("Poses"), tr("Pose editor and library (Alt+4)")},
        {Page::Library, QStringLiteral("gallery"), tr("Library"), tr("All images of the project (Alt+5)")},
    };
    int index = 0;
    for (const auto &[page, icon, label, tip] : items) {
        auto *b = new QToolButton(rail);
        b->setIcon(Icons::get(icon));
        b->setIconSize(QSize(22, 22));
        b->setText(label);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setFixedSize(74, 58);
        b->setProperty("role", QStringLiteral("nav"));
        b->setStyleSheet(QStringLiteral("font-size: 8pt;"));
        b->setShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(++index)));
        m_nav->addButton(b, static_cast<int>(page));
        v->addWidget(b, 0, Qt::AlignHCenter);
    }
    connect(m_nav, &QButtonGroup::idClicked, this, [this](int id) { showPage(static_cast<Page>(id)); });
    v->addStretch();
    m_queueButton = new QToolButton(rail);
    m_queueButton->setIcon(Icons::get(QStringLiteral("queue")));
    m_queueButton->setIconSize(QSize(20, 20));
    m_queueButton->setText(tr("Queue"));
    m_queueButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_queueButton->setFixedSize(74, 52);
    m_queueButton->setProperty("role", QStringLiteral("nav"));
    m_queueButton->setStyleSheet(QStringLiteral("font-size: 8pt;"));
    m_queueButton->setCheckable(true);
    m_queueButton->setToolTip(tr("Show running and finished jobs"));
    connect(m_queueButton, &QToolButton::clicked, this, &MainWindow::toggleQueue);
    v->addWidget(m_queueButton, 0, Qt::AlignHCenter);
    auto *settings = new QToolButton(rail);
    settings->setIcon(Icons::get(QStringLiteral("settings")));
    settings->setIconSize(QSize(20, 20));
    settings->setText(tr("Settings"));
    settings->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    settings->setFixedSize(74, 52);
    settings->setProperty("role", QStringLiteral("nav"));
    settings->setStyleSheet(QStringLiteral("font-size: 8pt;"));
    settings->setToolTip(tr("Providers, models and preferences (Ctrl+,)"));
    connect(settings, &QToolButton::clicked, this, [this] { openSettings(); });
    v->addWidget(settings, 0, Qt::AlignHCenter);
    return rail;
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(Icons::get(QStringLiteral("plus")), tr("New document..."), QKeySequence::New, this, [this] {
        showPage(Page::Editor);
        m_editor->newDocumentDialog();
    });
    file->addAction(Icons::get(QStringLiteral("image")), tr("Open image..."), QKeySequence::Open, this, &MainWindow::openImages);
    file->addAction(Icons::get(QStringLiteral("upload")), tr("Import images to library..."), this, &MainWindow::importToLibrary);
    file->addSeparator();
    file->addAction(Icons::get(QStringLiteral("save")), tr("Save all"), QKeySequence::Save, this, [this] {
        m_studio->saveAll().then(this, [this] { m_studio->notify(tr("Project saved."), QStringLiteral("success")); });
    });
    file->addAction(Icons::get(QStringLiteral("download")), tr("Export image..."), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this,
                    [this] { m_editor->exportImage(); });
    file->addAction(Icons::get(QStringLiteral("gallery")), tr("Save image to library"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), this,
                    [this] { m_editor->saveToLibrary(); });
    file->addAction(tr("Close document"), QKeySequence::Close, this, [this] { m_studio->closeDocument(m_studio->activeDocument()); });
    file->addSeparator();
    file->addAction(Icons::get(QStringLiteral("folder")), tr("New project..."), this, &MainWindow::newProject);
    file->addAction(Icons::get(QStringLiteral("folder")), tr("Open project..."), QKeySequence(QStringLiteral("Ctrl+Shift+O")), this, &MainWindow::openProject);
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
    file->addAction(tr("Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(Icons::get(QStringLiteral("copy")), tr("Copy merged"), QKeySequence(QStringLiteral("Ctrl+Shift+C")), this, [this] { m_editor->copyMerged(); });
    edit->addAction(tr("Paste as layer"), this, [this] {
        showPage(Page::Editor);
        m_editor->pasteAsLayer();
    });
    edit->addAction(tr("Import image as layer..."), this, [this] {
        showPage(Page::Editor);
        m_editor->importAsLayer();
    });
    edit->addSeparator();
    edit->addAction(Icons::get(QStringLiteral("settings")), tr("Settings..."), QKeySequence(QStringLiteral("Ctrl+,")), this, [this] { openSettings(); });

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(Icons::get(QStringLiteral("queue")), tr("Job queue"), QKeySequence(QStringLiteral("Ctrl+Shift+Q")), this, [this] {
        m_queueButton->setChecked(!m_queueButton->isChecked());
        toggleQueue();
    });
    view->addAction(Icons::get(Theme::instance().isDark() ? QStringLiteral("sun") : QStringLiteral("moon")), tr("Toggle light/dark theme"), this, [this] {
        const QString next = Theme::instance().isDark() ? QStringLiteral("light") : QStringLiteral("dark");
        Settings::instance().setValue(QStringLiteral("ui/theme"), next);
        QMessageBox::information(this, tr("Theme"), tr("The %1 theme will be applied the next time Lumen Studio starts.").arg(next));
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
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

void MainWindow::showPage(Page page)
{
    if (QWidget *w = m_pageWidgets.value(page))
        m_pages->setCurrentWidget(w);
    if (QAbstractButton *b = m_nav->button(static_cast<int>(page)))
        b->setChecked(true);
    Settings::instance().setValue(QStringLiteral("ui/page"), static_cast<int>(page));
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

void MainWindow::updateJobSummary()
{
    const int active = m_studio->jobs()->activeCount();
    const int pending = m_studio->jobs()->pendingCount();
    if (active + pending == 0)
        m_jobLabel->setText(tr("Idle"));
    else
        m_jobLabel->setText(tr("%1 running · %2 queued").arg(active).arg(pending));
    m_queueButton->setText(active + pending > 0 ? tr("Queue (%1)").arg(active + pending) : tr("Queue"));
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
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open images"), QString(),
                                                            tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    for (const QString &f : files) {
        QImageReader reader(f);
        reader.setAutoTransform(true);
        const QImage img = reader.read();
        if (img.isNull())
            m_studio->notify(tr("Cannot open %1: %2").arg(QFileInfo(f).fileName(), reader.errorString()), QStringLiteral("error"));
        else
            m_studio->openImageAsDocument(img, QFileInfo(f).completeBaseName());
    }
}

void MainWindow::importToLibrary()
{
    doc::Project *project = m_studio->project();
    if (!project || !project->isOpen())
        return;
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import images"), QString(),
                                                            tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    for (const QString &f : files) {
        project->importFile(f).then([](doc::AssetInfo) {}).onFailed(this, [this](const std::exception &e) {
            m_studio->notify(describeException(e), QStringLiteral("error"));
        });
    }
    if (!files.isEmpty()) {
        m_studio->notify(tr("Importing %n image(s)...", nullptr, int(files.size())));
        showPage(Page::Library);
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
    OnboardingDialog dlg(m_studio, this);
    dlg.exec();
    Settings::instance().setValue(QStringLiteral("app/onboarded"), true);
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
