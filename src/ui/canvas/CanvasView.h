#pragma once

#include "doc/Document.h"
#include "doc/TileGrid.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QWidget>

namespace lumen::ui {

class Tool;
class EditorState;

struct ToolEvent
{
    QPointF docPos;
    QPointF widgetPos;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers modifiers;
    double pressure = 1.0;
    bool tablet = false;
};

// Interactive view of a document: zoom/pan, GPU-accelerated tiled rendering, selection overlay,
// pose overlay and tool interaction. The actual drawing surface is a QOpenGLWidget when available,
// otherwise a raster widget (headless/offscreen runs).
class CanvasView : public QWidget
{
    Q_OBJECT
public:
    explicit CanvasView(EditorState *state, QWidget *parent = nullptr);
    ~CanvasView() override;

    void setDocument(doc::Document *doc);
    doc::Document *document() const { return m_doc; }
    EditorState *state() const { return m_state; }

    void setTool(Tool *tool);
    Tool *tool() const { return m_tool; }

    // ---- view transform -------------------------------------------------------------------------
    double zoom() const { return m_zoom; }
    void setZoom(double zoom, const QPointF &anchorWidgetPos);
    void zoomIn();
    void zoomOut();
    void fitToView();
    void actualPixels();
    QPointF mapToDoc(const QPointF &widgetPos) const;
    QPointF mapFromDoc(const QPointF &docPos) const;
    QRectF mapFromDoc(const QRectF &docRect) const;
    QRectF visibleDocRect() const;
    void panBy(const QPointF &delta);

    // Requests a repaint of a document-space area (plus overlays).
    void updateDocRect(const QRectF &docRect);
    void updateAll();

    void setCursorOverride(const QCursor &cursor);

    // Renders the surface into a widget painter (called by the drawing surface).
    void render(QPainter &p, const QRect &widgetRect);

signals:
    void zoomChanged(double zoom);
    void cursorMoved(const QPointF &docPos);
    void colorPicked(const QColor &color);
    void documentChanged(doc::Document *doc);
    // Zoom, pan or size changed (document-to-widget mapping).
    void viewChanged();
    // A tool gesture (mouse/pen pressed) started or ended.
    void interactionChanged(bool active);

protected:
    void resizeEvent(QResizeEvent *e) override;
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void handleMouse(QMouseEvent *e);
    void handleTablet(QTabletEvent *e);
    void handleWheel(QWheelEvent *e);
    void handleKey(QKeyEvent *e, bool press);
    void dispatch(QEvent::Type type, const ToolEvent &ev);
    void rebuildSelectionOverlay(const QRect &rect);
    void scheduleContour();
    void drawCheckerboard(QPainter &p, const QRectF &rect);
    void clampPan();

    EditorState *m_state;
    QWidget *m_surface = nullptr;
    QPointer<doc::Document> m_doc;
    Tool *m_tool = nullptr;
    double m_zoom = 1.0;
    QPointF m_pan;            // widget position of document origin
    bool m_fitPending = true;
    bool m_spacePan = false;
    bool m_panning = false;
    QPointF m_lastPanPos;
    bool m_toolActive = false;
    bool m_tabletActive = false;

    doc::TileGrid m_overlay;  // selection tint
    QPainterPath m_contour;   // marching ants (document space)
    QTimer m_contourTimer;
    QTimer m_antsTimer;
    int m_antsOffset = 0;
    quint64 m_contourGeneration = 0;
    QPixmap m_checker;
};

} // namespace lumen::ui
