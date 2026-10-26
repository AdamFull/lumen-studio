#pragma once

#include "doc/Document.h"
#include "ui/pages/AiPanel.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class QActionGroup;
class QLabel;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QUndoView;
class QHBoxLayout;

namespace lumen::ui {

class AiOptions;
class CanvasView;
class ColorSwatch;
class EditorState;
class FiltersPanel;
class LayersPanel;
class PromptBar;
class StartScreen;
class Studio;
class Tool;
class VariantStrip;

// Layered image editor with AI-assisted editing: the main window's content. The canvas has the
// prompt bar under it (and its results above the bar); the right panel shows Layers, History and
// Filters, and the AI panel when it is asked for. While no document is open the start screen shows
// instead.
class EditorPage : public QWidget
{
    Q_OBJECT
public:
    explicit EditorPage(Studio *studio, QWidget *parent = nullptr);

    CanvasView *canvas() const { return m_canvas; }
    EditorState *editorState() const { return m_state; }
    AiPanel *aiPanel() const { return m_ai; }
    AiOptions *aiOptions() const { return m_aiOptions; }
    PromptBar *promptBar() const { return m_bar; }
    VariantStrip *variantStrip() const { return m_strip; }
    // The AI panel in the right panel (hidden until asked for), on one of its actions if given.
    void showAiPanel(std::optional<AiPanel::Mode> mode = std::nullopt);
    void hideAiPanel();
    bool isAiPanelShown() const;
    // The Filters tab of the right panel (View → Filters, Tools → New filter… / Filter editor…).
    FiltersPanel *filtersPanel() const { return m_filters; }
    void showFiltersPanel();
    StartScreen *startScreen() const { return m_start; }
    bool showsStartScreen() const;
    void selectTool(const QString &id);
    // The editor's own commands by shortcut id ("undo", "select-all", "zoom-in"…), for the main
    // window's menus: the same actions, so a customised key works in both.
    QAction *editorAction(const QString &id) const;
    // How far down (in `in`'s coordinates) panels laid over the editor may reach: above the results
    // strip and the prompt bar while a document is open, so they hide neither.
    int overlayBottom(const QWidget *in) const;

    // Actions also exposed through the main window menus.
    void newDocumentDialog();
    void exportImage();
    void saveToLibrary();
    void copyMerged();
    void pasteAsLayer();
    // The clipboard's picture as a new document.
    void pasteAsNewDocument();
    void importAsLayer();

signals:
    // The prompt bar or the results strip moved, appeared or went (overlayBottom changed).
    void bottomAreaChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void setDocument(doc::Document *doc);
    void rebuildTabs();
    void updateStatus();
    void updateStack();
    // The right panel's tab names, or only their icons beside the AI tab when the names do not fit.
    void updateSideTabs();
    void deleteSelectionPixels();

    Studio *m_studio;
    EditorState *m_state;
    QStackedWidget *m_stack;
    StartScreen *m_start;
    QWidget *m_body;
    CanvasView *m_canvas;
    AiOptions *m_aiOptions;
    VariantStrip *m_strip;
    PromptBar *m_bar;
    QTabBar *m_tabs;
    QHBoxLayout *m_optionsHost;
    QWidget *m_options = nullptr;
    QList<Tool *> m_tools;
    QHash<QString, QAction *> m_toolActions;
    QHash<QString, QAction *> m_actions; // by shortcut id without "editor."
    QActionGroup *m_toolGroup;
    LayersPanel *m_layers;
    AiPanel *m_ai;
    QUndoView *m_history;
    FiltersPanel *m_filters;
    QTabWidget *m_side;
    QLabel *m_zoomLabel;
    QLabel *m_infoLabel;
    QLabel *m_hintLabel;
    ColorSwatch *m_fg;
    ColorSwatch *m_bg;
    QPointer<doc::Document> m_doc;
    bool m_rebuildingTabs = false;
};

} // namespace lumen::ui
