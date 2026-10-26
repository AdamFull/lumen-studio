#pragma once

#include "doc/Document.h"

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

class AiPanel;
class CanvasView;
class ColorSwatch;
class EditorState;
class LayersPanel;
class StartScreen;
class Studio;
class Tool;
class ContextBar;

// Layered image editor with AI-assisted editing: the main window's content. While no document is
// open it shows the start screen instead.
class EditorPage : public QWidget
{
    Q_OBJECT
public:
    explicit EditorPage(Studio *studio, QWidget *parent = nullptr);

    CanvasView *canvas() const { return m_canvas; }
    ContextBar *contextBar() const { return m_contextBar; }
    EditorState *editorState() const { return m_state; }
    AiPanel *aiPanel() const { return m_ai; }
    StartScreen *startScreen() const { return m_start; }
    bool showsStartScreen() const;
    void selectTool(const QString &id);

    // Actions also exposed through the main window menus.
    void newDocumentDialog();
    void exportImage();
    void saveToLibrary();
    void copyMerged();
    void pasteAsLayer();
    // The clipboard's picture as a new document.
    void pasteAsNewDocument();
    void importAsLayer();

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void setDocument(doc::Document *doc);
    void rebuildTabs();
    void updateStatus();
    void updateStack();
    void deleteSelectionPixels();

    Studio *m_studio;
    EditorState *m_state;
    QStackedWidget *m_stack;
    StartScreen *m_start;
    QWidget *m_body;
    CanvasView *m_canvas;
    ContextBar *m_contextBar;
    QTabBar *m_tabs;
    QHBoxLayout *m_optionsHost;
    QWidget *m_options = nullptr;
    QList<Tool *> m_tools;
    QHash<QString, QAction *> m_toolActions;
    QActionGroup *m_toolGroup;
    LayersPanel *m_layers;
    AiPanel *m_ai;
    QUndoView *m_history;
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
