#pragma once

#include "filters/Evaluator.h"
#include "filters/Graph.h"

#include <QFuture>
#include <QImage>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class QAction;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace lumen::ui {

class FilterCatalog;
class GraphModel;
class NodeMenu;
class NodeScene;
class NodeView;
class ParamField;

// The node editor (M17 §17.6): a window of its own in which a filter is built from nodes. Nodes
// are added from a searchable menu (the toolbar's Add node, or a right-click on the canvas) and
// connected by dragging from port to port; ports of types that do not fit refuse with the reason.
// The side panel edits the selected node's settings (sliders, choices, a colour picker, a curve
// editor for Curves), publishes a setting as a slider of the Filters panel, shows the selected
// node's result on the picture and lists the filter's problems in plain words. Every edit is an
// undo step of the window's own history. Save writes into the filters folder (FilterCatalog), so
// the Filters panel shows the change.
class NodeEditorWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit NodeEditorWindow(FilterCatalog *catalog, QWidget *parent = nullptr);
    ~NodeEditorWindow() override;

    // The window that edits `path`, raised, or a new one that opens it (a window of its own, closed
    // with `parent`). Built-in filters are read-only; the Filters panel opens a copy of them.
    static NodeEditorWindow *openFilter(FilterCatalog *catalog, const QString &path, QWidget *parent,
                                        QString *error = nullptr);
    static QList<NodeEditorWindow *> openWindows();

    bool open(const QString &path, QString *error = nullptr);
    // A filter that has no file yet: Save asks for a name.
    void setGraph(const filters::Graph &graph);
    QString path() const { return m_path; }

    // The picture the previews show (the active layer), its selection and the whole picture (every
    // visible layer merged, for an Input node that reads it); a null image shows the sample picture.
    void setSource(const QImage &image, const QImage &selection = {}, const QImage &picture = {});

    GraphModel *model() const { return m_model; }
    NodeScene *scene() const { return m_scene; }
    NodeView *view() const { return m_view; }
    NodeMenu *nodeMenu() const { return m_menu; }

    // ---- Editing: the toolbar, the canvas and the side panel go through these -----------------
    // Adds a node at `scenePos` (default: the middle of the view) and selects it; returns its id.
    QString addNode(const QString &typeId, std::optional<QPointF> scenePos = std::nullopt);
    // As a drag from port to port: refused, with the reason in the status bar, when it does not fit.
    bool connectPorts(const QString &fromNode, const QString &fromPort, const QString &toNode, const QString &toPort);
    bool deleteSelection();
    void selectNode(const QString &id);
    // The node the side panel shows: the one selected node; empty for none or several.
    QString selectedNode() const { return m_selected; }
    bool setParam(const QString &node, const QString &param, const QVariant &value);
    // Shows a setting in the Filters panel, as a slider with this label and range (both 0: the
    // setting's own), or as a list for a choice.
    bool publish(const QString &node, const QString &param, const QString &label, double minimum = 0.0,
                 double maximum = 0.0);
    bool unpublish(const QString &node, const QString &param);
    void frameAll();

    // Saves into the filter's file; one without a file asks for a name first.
    bool save(QString *error = nullptr);
    // Saves as a new filter named `name` in the filters folder; the window edits that file from then on.
    bool saveAs(const QString &name, QString *error = nullptr);
    bool isModified() const;

    // What the window shows now: the last refusal or error of the status bar, the problems listed.
    QString message() const { return m_message; }
    QStringList problems() const;
    // The selected node's result (the filter's when no node is selected), as the side panel shows
    // it, and the text under it (an error, a value, "press Run").
    QImage preview() const { return m_previewImage; }
    QString previewText() const;
    bool isPreviewPending() const;

    // Asked when a window with changes closes (tests answer without a dialog).
    using SaveQuestion = std::function<QMessageBox::StandardButton(const QString &name)>;
    void setSaveQuestion(SaveQuestion question) { m_saveQuestion = std::move(question); }

signals:
    void saved(const QString &path);
    void previewUpdated();

protected:
    void closeEvent(QCloseEvent *e) override;
    // The window it belongs to closing (the app quitting) asks about unsaved changes too.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Asks about unsaved changes; true when the window may close.
    bool confirmClose();
    void setRunning(bool running);
    void buildSidePanel(QWidget *parent);
    void modelChanged();
    void selectionChanged();
    void rebuildNodeSection();
    void updateNodeValues();
    void updateFilterSection();
    void updateProblems();
    void updateTitle();
    void showMessage(const QString &message);
    void schedulePreview();
    void startPreview();
    void showPreview(const QImage &image, const QString &text);
    void updatePreviewTitle();
    void askToPublish(const QString &node, const QString &param);
    void openNodeMenu(const QPointF &scenePos, const QPoint &screenPos);

    FilterCatalog *m_catalog;
    GraphModel *m_model;
    NodeScene *m_scene;
    NodeView *m_view;
    NodeMenu *m_menu;
    QString m_path;
    QString m_selected;
    QString m_message;
    std::optional<QPointF> m_addAt;
    SaveQuestion m_saveQuestion;

    QAction *m_runAction = nullptr;
    QAction *m_deleteAction = nullptr;
    // Side panel.
    QWidget *m_nodeBox = nullptr;
    QVBoxLayout *m_nodeLayout = nullptr;
    QLabel *m_nodeTitle = nullptr;
    QLabel *m_nodeHelp = nullptr;
    QList<ParamField *> m_fields;
    QList<QToolButton *> m_publishButtons;
    QLabel *m_previewTitle = nullptr;
    QLabel *m_previewLabel = nullptr;
    QLabel *m_previewInfo = nullptr;
    QPushButton *m_previewRun = nullptr;
    QListWidget *m_problems = nullptr;
    QLabel *m_noProblems = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_description = nullptr;
    QVBoxLayout *m_publishedLayout = nullptr;
    QLabel *m_noPublished = nullptr;

    // Previews of the selected node.
    QImage m_source;
    QImage m_selection;
    QImage m_picture;
    std::unique_ptr<filters::Evaluator> m_evaluator;
    QFuture<filters::EvalResult> m_previewFuture;
    QTimer m_previewTimer;
    int m_previewGeneration = 0;
    int m_shownGeneration = 0; // the preview on screen is of this generation
    bool m_runRequested = false;
    bool m_running = false;       // AI steps run for the preview
    bool m_pendingAfterRun = false; // a change during the run: preview again when it ends
    QPointer<QWidget> m_owner;
    QImage m_previewImage;
    QString m_previewText;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // see FilterRunner
};

} // namespace lumen::ui
