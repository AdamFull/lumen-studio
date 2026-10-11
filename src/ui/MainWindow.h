#pragma once

#include "ui/Studio.h"

#include <QHash>
#include <QMainWindow>

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

    void showPage(Page page);
    QWidget *pageWidget(Page page) const { return m_pageWidgets.value(page); }
    void openSettings(const QString &tab = QString());
    // Cloud GPUs of earlier trainings that are still rented (after a crash or a failed shutdown):
    // offers to shut them down. Done once at start-up when a RunPod key is saved.
    void checkLeftoverCloudPods();
    // Renders the window into a PNG (used by automated UI checks).
    bool saveScreenshot(const QString &path);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void buildMenus();
    QWidget *buildNavRail();
    void toggleQueue();
    void toggleAssistant();
    void placeAssistant();
    void updateJobSummary();
    void updateTitle();
    void newProject();
    void openProject();
    void openImages();
    void importToLibrary();
    void maybeShowOnboarding();

    Studio *m_studio;
    QStackedWidget *m_pages;
    QHash<Page, QWidget *> m_pageWidgets;
    QButtonGroup *m_nav;
    EditorPage *m_editor = nullptr;
    Toasts *m_toasts;
    QFrame *m_queuePanel = nullptr;
    QVBoxLayout *m_queueList = nullptr;
    QToolButton *m_queueButton = nullptr;
    QToolButton *m_assistantButton = nullptr;
    class AssistantPanel *m_assistant = nullptr;
    QLabel *m_jobLabel;
    QLabel *m_projectLabel;
    bool m_closing = false;
};

} // namespace lumen::ui
