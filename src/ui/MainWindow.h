#pragma once

#include "ui/Studio.h"
#include "ui/ext/Extension.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class QButtonGroup;
class QLabel;
class QStackedWidget;
class QToolButton;
class QFrame;
class QVBoxLayout;

namespace lumen::ui {

class EditorPage;
class Toasts;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(Studio *studio, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Pages by id: the core pages (pages::Editor...) and those of plugins that are on.
    void showPage(const QString &id);
    QWidget *pageWidget(const QString &id) const { return m_pageWidgets.value(id); }
    QStringList pageIds() const;
    void openSettings(const QString &tab = QString());
    // Cloud GPUs of earlier trainings that are still rented (after a crash or a failed shutdown):
    // offers to shut them down. Done once at start-up when a RunPod key is saved.
    void checkLeftoverCloudPods();
    // Models in an earlier version's folder (Studio::legacyModelsFolder): asks once whether to move
    // them to the new models folder (ask replaces the question, for tests). Called at start-up by
    // main. Returns the move job, if any.
    Job *offerModelsFolderMove(const std::function<bool(const runtime::FolderMovePlan &plan)> &ask = {});
    // Renders the window into a PNG (used by automated UI checks).
    bool saveScreenshot(const QString &path);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void buildMenus();
    QWidget *buildNavRail();
    void toggleQueue();
    void togglePanel(int index);
    void placePanels();
    void updateJobSummary();
    void updateTitle();
    void newProject();
    void openProject();
    void openImages();
    void importToLibrary();
    void maybeShowOnboarding();

    Studio *m_studio;
    QStackedWidget *m_pages;
    // Rail pages in rail order (core and plugin pages); the nav button id is the index.
    QList<ext::PageSpec> m_pageSpecs;
    QHash<QString, QWidget *> m_pageWidgets;
    struct Panel
    {
        ext::PanelSpec spec;
        QToolButton *button = nullptr;
        QPointer<QWidget> widget;
    };
    QList<Panel> m_panels;
    QButtonGroup *m_nav;
    EditorPage *m_editor = nullptr;
    Toasts *m_toasts;
    QFrame *m_queuePanel = nullptr;
    QVBoxLayout *m_queueList = nullptr;
    QToolButton *m_queueButton = nullptr;
    QLabel *m_jobLabel;
    QLabel *m_projectLabel;
    bool m_closing = false;
};

} // namespace lumen::ui
