#pragma once

#include "ui/Studio.h"
#include "ui/ext/Extension.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class QAction;
class QDialog;
class QFrame;
class QToolButton;
class QVBoxLayout;

namespace lumen::ui {

class EditorPage;
class ElidedLabel;
class NewFromPromptDialog;
class PageWindow;
class Toasts;

// The main window is the editor: tools on the left, the canvas with document tabs in the centre, the
// AI panel, Layers and History on the right, and the start screen while no document is open. The
// status bar shows the project, the image engine in use and the jobs (the Queue opens from there).
// Plugin pages (Create, Poses, Parts) open in windows of their own from the Tools menu; plugin
// panels (the assistant) are switched on from the View menu.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Studio *studio, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Studio::showPage: "editor" brings this window to the front, "generate" opens New from prompt,
    // "library" Browse project images, and a plugin page opens its window (built the first time,
    // raised afterwards).
    void showPage(const QString &id);
    // The editor, or a plugin page whose window has been opened.
    QWidget *pageWidget(const QString &id) const;
    // The window of a plugin page that has been opened.
    QWidget *pageWindow(const QString &id) const;
    // The editor and the pages of the plugins that are on, in the order of the Tools menu.
    QStringList pageIds() const;
    EditorPage *editor() const { return m_editor; }
    // File → New from prompt: opens the dialog (or brings it to the front) and returns it.
    NewFromPromptDialog *newFromPrompt();
    NewFromPromptDialog *newFromPromptDialog() const; // null while closed
    // File → Browse project images: the project's images in a window of their own.
    QDialog *browseProjectImages();
    QDialog *projectImagesDialog() const; // null until first opened
    void openSettings(const QString &tab = QString());
    // Help → Set up AI: the welcome dialog (servers found on this computer, local setup, online
    // services).
    void setUpAi();
    // The Queue popup over the editor (View → Job queue, the jobs button in the status bar).
    void setQueueVisible(bool show);
    QWidget *queuePanel() const;
    // Cloud GPUs of earlier trainings that are still rented (after a crash or a failed shutdown):
    // offers to shut them down. Done once at start-up when a RunPod key is saved.
    void checkLeftoverCloudPods();
    // Models in an earlier version's folder (Studio::legacyModelsFolder): asks once whether to move
    // them to the new models folder (ask replaces the question, for tests). Called at start-up by
    // main. Returns the move job, if any.
    Job *offerModelsFolderMove(const std::function<bool(const runtime::FolderMovePlan &plan)> &ask = {});
    // Renders the window into a PNG (used by automated UI checks): the one showPage opened last,
    // else this one.
    bool saveScreenshot(const QString &path);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void buildMenus();
    void buildStatusBar();
    void setPanelVisible(int index, bool show);
    void placePanels();
    void updateJobSummary();
    void updateEngine();
    void updateTitle();
    void newProject();
    void openProject();
    void openImages();
    void rerun(const QString &assetId);
    void newImageWithPose(const pose::Pose &pose);
    void notify(const QString &message, const QString &level);
    void maybeShowOnboarding();

    Studio *m_studio;
    EditorPage *m_editor;
    // Plugin pages, in the order of the Tools menu, and the windows opened so far.
    QList<ext::PageSpec> m_pageSpecs;
    QHash<QString, QPointer<PageWindow>> m_windows;
    struct Panel
    {
        ext::PanelSpec spec;
        QAction *action = nullptr;
        QPointer<QWidget> widget;
    };
    QList<Panel> m_panels;
    Toasts *m_toasts;
    QFrame *m_queuePanel = nullptr;
    QVBoxLayout *m_queueList = nullptr;
    QAction *m_queueAction = nullptr;
    QToolButton *m_jobsButton = nullptr;
    QToolButton *m_engineButton = nullptr;
    ElidedLabel *m_projectLabel = nullptr;
    QPointer<NewFromPromptDialog> m_newFromPrompt;
    QPointer<QDialog> m_projectImages;
    QPointer<QWidget> m_lastOpened; // by showPage, for saveScreenshot
    bool m_closing = false;
};

} // namespace lumen::ui
