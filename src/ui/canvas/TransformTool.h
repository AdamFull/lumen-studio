#pragma once

#include "ui/canvas/Tool.h"

#include <QImage>
#include <QPointer>
#include <QTransform>
#include <QUuid>

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

// Free transform of the active layer, or of the selected pixels when there is a selection
// (the selection follows them). Drag inside the box to move, a handle to scale (Shift keeps the
// aspect, Alt scales around the centre) and outside the box to rotate (Shift snaps to 15°).
// Every drag is one undo step; a session keeps resampling the original pixels, so repeated
// adjustments do not blur the result. Enter (or switching tools) finishes, Esc reverts.
class TransformTool : public Tool
{
    Q_OBJECT
public:
    struct Params
    {
        QPointF center; // document position of the source centre
        double sx = 1.0; // negative = flipped
        double sy = 1.0;
        double angle = 0.0; // degrees, clockwise on screen
    };

    explicit TransformTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("transform"); }
    QString name() const override { return tr("Free transform"); }
    QString iconName() const override { return QStringLiteral("transform"); }
    QString shortcut() const override { return QStringLiteral("T"); }
    QString hint() const override;
    void activated(CanvasView *view) override;
    void deactivated(CanvasView *view) override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void hover(CanvasView *view, const ToolEvent &ev) override;
    bool key(CanvasView *view, QKeyEvent *e) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;
    QWidget *createOptions(QWidget *parent) override;

    // Floats the active layer (or selection) if needed; params() is valid afterwards.
    bool begin() { return ensureSession(); }
    // Programmatic adjustments (options bar, tests); each is one undo step.
    bool setParams(const Params &params);
    bool flip(Qt::Orientation orientation);
    bool rotateBy(double degrees);
    // Ends the session (the changes are already in the history).
    void finish();
    // Reverts every step of the current session.
    void cancel();

    bool hasSession() const { return m_doc != nullptr; }
    Params params() const { return m_params; }
    QRect sourceRect() const { return m_src; }
    QTransform transform() const { return transformFor(m_params); }

private:
    bool ensureSession(bool report = true);
    void endSession();
    QTransform transformFor(const Params &params) const;
    QRect boundsFor(const Params &params) const;
    QPolygonF boxFor(const Params &params) const;
    void preview(const Params &params);
    void commit(const QString &undoText);
    int hitTest(const QPointF &widgetPos) const; // 0-7 handles, 8 inside, 9 outside (rotate)

    QPointer<CanvasView> m_view;
    QPointer<doc::Document> m_doc;
    QUuid m_layer;
    quint64 m_revision = 0;
    int m_startIndex = 0;
    bool m_pushed = false;
    QImage m_rest; // the layer with the floating pixels removed
    QImage m_float; // floating pixels, m_src-sized
    QImage m_floatMask; // selection inside m_src as ARGB alpha (null = whole layer)
    QRect m_src;
    Params m_params; // shown on the layer
    Params m_committed; // last state recorded in the history
    QRect m_shown; // layer area covered by the shown pixels
    QImage m_before; // layer before the current drag
    Params m_start;
    QPointF m_pressDoc;
    int m_handle = -1;
    bool m_dragging = false;
    bool m_smooth = true;
};

} // namespace lumen::ui
