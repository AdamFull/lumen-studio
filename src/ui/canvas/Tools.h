#pragma once

#include "ui/canvas/Tool.h"

#include <QImage>
#include <QPolygonF>
#include <QUuid>

namespace lumen::ui {

// Soft round brush dab (Grayscale8), cached per size/hardness.
QImage brushDab(double diameter, double hardness);
// Scanline flood fill: pixels whose colour differs from the seed by at most `tolerance` (0-255 per channel).
QImage floodFillMask(const QImage &image, const QPoint &seed, int tolerance);

class BrushTool : public Tool
{
    Q_OBJECT
public:
    BrushTool(EditorState *state, bool eraser, QObject *parent = nullptr);
    QString id() const override { return m_eraser ? QStringLiteral("eraser") : QStringLiteral("brush"); }
    QString name() const override { return m_eraser ? tr("Eraser") : tr("Brush"); }
    QString iconName() const override { return m_eraser ? QStringLiteral("eraser") : QStringLiteral("brush"); }
    QString shortcut() const override { return m_eraser ? QStringLiteral("E") : QStringLiteral("B"); }
    QString hint() const override { return tr("[ and ] change the size. Hold Shift to draw a straight line from the last point."); }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    bool key(CanvasView *view, QKeyEvent *e) override;
    double brushRadius() const override;
    QWidget *createOptions(QWidget *parent) override;

private:
    void stampLine(CanvasView *view, const QPointF &from, const QPointF &to, double pressure);
    void stamp(CanvasView *view, const QPointF &pos, double pressure);

    bool m_eraser;
    QUuid m_layer;
    QImage m_base;
    QImage m_stroke;
    QRect m_dirty;
    QPointF m_last;
    QPointF m_lastStrokeEnd;
    bool m_hasLastStroke = false;
    double m_carry = 0.0;
    bool m_active = false;
};

class MaskBrushTool : public Tool
{
    Q_OBJECT
public:
    explicit MaskBrushTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("mask"); }
    QString name() const override { return tr("Mask brush"); }
    QString iconName() const override { return QStringLiteral("mask-brush"); }
    QString shortcut() const override { return QStringLiteral("Q"); }
    QString hint() const override { return tr("Paint the area to regenerate. Alt or right button erases the mask."); }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    bool key(CanvasView *view, QKeyEvent *e) override;
    double brushRadius() const override;
    QWidget *createOptions(QWidget *parent) override;

private:
    void stamp(CanvasView *view, const QPointF &pos, double pressure);
    QImage m_before;
    bool m_wasEmpty = false;
    bool m_subtract = false;
    QRect m_dirty;
    QPointF m_last;
    double m_carry = 0.0;
    bool m_active = false;
};

class SelectionTool : public Tool
{
    Q_OBJECT
public:
    enum class Shape { Rect, Ellipse, Lasso };
    SelectionTool(EditorState *state, Shape shape, QObject *parent = nullptr);
    QString id() const override;
    QString name() const override;
    QString iconName() const override;
    QString shortcut() const override;
    QString hint() const override { return tr("Shift adds, Alt subtracts, Shift+Alt intersects. Click to deselect."); }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;

private:
    QPainterPath shapePath() const;
    Shape m_shape;
    QPointF m_start;
    QPointF m_current;
    QPolygonF m_lasso;
    bool m_active = false;
    Qt::KeyboardModifiers m_mods;
};

class EyedropperTool : public Tool
{
    Q_OBJECT
public:
    explicit EyedropperTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("eyedropper"); }
    QString name() const override { return tr("Eyedropper"); }
    QString iconName() const override { return QStringLiteral("eyedropper"); }
    QString shortcut() const override { return QStringLiteral("I"); }
    QString hint() const override { return tr("Click to pick the foreground colour, Alt+click for the background colour."); }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override { press(view, ev); }
    QWidget *createOptions(QWidget *parent) override;
};

class FillTool : public Tool
{
    Q_OBJECT
public:
    explicit FillTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("fill"); }
    QString name() const override { return tr("Fill"); }
    QString iconName() const override { return QStringLiteral("fill"); }
    QString shortcut() const override { return QStringLiteral("G"); }
    void press(CanvasView *view, const ToolEvent &ev) override;
    QWidget *createOptions(QWidget *parent) override;
};

class MoveTool : public Tool
{
    Q_OBJECT
public:
    explicit MoveTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("move"); }
    QString name() const override { return tr("Move layer"); }
    QString iconName() const override { return QStringLiteral("move"); }
    QString shortcut() const override { return QStringLiteral("V"); }
    QString hint() const override { return tr("Drag to move the active layer. Arrow keys nudge (Shift = 10 px)."); }
    QCursor cursor() const override { return Qt::SizeAllCursor; }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    bool key(CanvasView *view, QKeyEvent *e) override;

private:
    void apply(CanvasView *view, const QPoint &offset);
    QUuid m_layer;
    QImage m_base;
    QPointF m_start;
    QPoint m_offset;
    bool m_active = false;
};

class CropTool : public Tool
{
    Q_OBJECT
public:
    explicit CropTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("crop"); }
    QString name() const override { return tr("Crop / canvas size"); }
    QString iconName() const override { return QStringLiteral("crop"); }
    QString shortcut() const override { return QStringLiteral("C"); }
    QString hint() const override { return tr("Drag the frame (it may extend beyond the image to enlarge the canvas). Enter applies, Esc resets."); }
    void activated(CanvasView *view) override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void hover(CanvasView *view, const ToolEvent &ev) override;
    bool key(CanvasView *view, QKeyEvent *e) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;
    QWidget *createOptions(QWidget *parent) override;

    void apply();
    void reset();
    void setAspect(double aspect);

private:
    int hitTest(CanvasView *view, const QPointF &widgetPos) const;
    QPointer<CanvasView> m_view;
    QRectF m_rect;
    QRectF m_startRect;
    QPointF m_start;
    int m_handle = -1; // 0-7 handles, 8 = move, 9 = new
    double m_aspect = 0.0;
};

class HandTool : public Tool
{
    Q_OBJECT
public:
    explicit HandTool(EditorState *state, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("hand"); }
    QString name() const override { return tr("Pan"); }
    QString iconName() const override { return QStringLiteral("hand"); }
    QString shortcut() const override { return QStringLiteral("H"); }
    QCursor cursor() const override { return Qt::OpenHandCursor; }
    void press(CanvasView *view, const ToolEvent &ev) override { Q_UNUSED(view); m_last = ev.widgetPos; }
    void move(CanvasView *view, const ToolEvent &ev) override
    {
        view->panBy(ev.widgetPos - m_last);
        m_last = ev.widgetPos;
    }

private:
    QPointF m_last;
};

} // namespace lumen::ui
