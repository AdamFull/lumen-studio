#pragma once

#include "doc/Document.h"

#include <QDateTime>
#include <QPointer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace lumen::ui {

class CanvasView;
class FilterCatalog;
class FilterRunner;
class NodeEditorWindow;
class ParamField;
class Studio;

// The Filters tab of the editor's right panel (M17 §17.6): the built-in and the saved filters with
// small previews, the chosen filter's settings (the ones its graph publishes) as sliders and
// lists, a live preview on the canvas at reduced resolution while they move, and Apply: the filter
// on the active layer, through the selection, onto a new layer (or the layer itself) as one undo
// step, in the background with progress and Cancel. New filter…, Edit…, Duplicate and Delete
// manage the saved filters (Edit opens the filter editor, NodeEditorWindow); built-in filters are
// edited as a copy. The actions stay in view under the details, which scroll.
class FiltersPanel : public QWidget
{
    Q_OBJECT
public:
    // `catalog` defaults to the app's (FilterCatalog::of(studio)); tests give their own folder.
    FiltersPanel(Studio *studio, CanvasView *canvas, QWidget *parent = nullptr, FilterCatalog *catalog = nullptr);
    ~FiltersPanel() override;

    FilterCatalog *catalog() const { return m_catalog; }
    FilterRunner *runner() const { return m_runner; }
    QListWidget *list() const { return m_list; }

    bool selectFilter(const QString &path);
    QString currentPath() const { return m_path; }
    // The chosen filter's settings as shown, in the graph's order.
    QList<ParamField *> fields() const { return m_fields; }
    ParamField *field(const QString &label) const;

    // ---- The buttons, callable directly (tests) --------------------------------------------
    // A new filter named `name` (asks for one when empty), opened in the filter editor. Its path.
    QString newFilter(const QString &name = {});
    // Opens the chosen filter in the filter editor; a built-in one as a copy (the copy made before,
    // while it is unchanged).
    NodeEditorWindow *editSelected();
    QPushButton *applyButton() const { return m_applyButton; }
    QString duplicateSelected();
    bool deleteSelected();
    void openFolder();
    bool applySelected();
    void cancel();

    // The text under the Apply button: what happened, or why the filter cannot be applied.
    QString statusText() const;
    // Questions to the user (deleting a filter); tests answer them without a dialog.
    using Confirm = std::function<bool(const QString &question)>;
    void setConfirm(Confirm confirm) { m_confirm = std::move(confirm); }

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void setDocument(doc::Document *doc);
    void rebuildList();
    void loadSelected();
    void rebuildSettings();
    void updateButtons();
    void updateSelectionHint();
    // Says which steps of the chosen filter cannot run now (an AI step without a capable engine).
    void updateAvailability();
    void setStatus(const QString &text, bool problem = false);
    void updatePreviewState();
    void updateThumbnail(const QString &path);
    void sendSourceTo(NodeEditorWindow *window) const;
    QListWidgetItem *itemFor(const QString &path) const;

    Studio *m_studio;
    QPointer<CanvasView> m_canvas;
    FilterCatalog *m_catalog;
    FilterRunner *m_runner;
    QPointer<doc::Document> m_doc;
    QString m_path;          // the chosen filter
    QDateTime m_loadedStamp; // the file's time when it was loaded
    bool m_shown = false;
    Confirm m_confirm;

    QListWidget *m_list;
    QPushButton *m_newButton;
    QPushButton *m_editButton;
    QPushButton *m_duplicateButton;
    QPushButton *m_deleteButton;
    QToolButton *m_folderButton;
    QWidget *m_details;
    QLabel *m_title;
    QLabel *m_about;
    QVBoxLayout *m_settingsLayout;
    QList<ParamField *> m_fields;
    QCheckBox *m_previewCheck;
    QComboBox *m_target;
    QWidget *m_footer; // the line about the selection, the reason it cannot run, the actions
    QLabel *m_selectionHint;
    QLabel *m_availability;
    QPushButton *m_runButton;
    QPushButton *m_applyButton;
    QWidget *m_progressRow;
    QProgressBar *m_progress;
    QLabel *m_progressText;
    QPushButton *m_cancelButton;
    QLabel *m_status;
    // The step that runs now (progress), for the bar and the text while its job reports.
    int m_stepsDone = 0;
    int m_stepsTotal = 0;
    QString m_stepName;
};

} // namespace lumen::ui
