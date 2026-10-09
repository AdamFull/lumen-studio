#pragma once

#include "ui/canvas/CanvasView.h"

#include <QCursor>
#include <QKeySequence>
#include <QObject>

class QKeyEvent;
class QPainter;

namespace lumen::ui {

// Base class of canvas tools. Tools receive document-space events from the CanvasView,
// mutate the document through its API and may paint overlays.
class Tool : public QObject
{
    Q_OBJECT
public:
    explicit Tool(EditorState *state, QObject *parent = nullptr)
        : QObject(parent)
        , m_state(state)
    {
    }

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString iconName() const = 0;
    virtual QString shortcut() const { return {}; }
    virtual QString hint() const { return {}; }

    virtual void activated(CanvasView *) {}
    virtual void deactivated(CanvasView *) {}
    virtual void press(CanvasView *, const ToolEvent &) {}
    virtual void move(CanvasView *, const ToolEvent &) {}
    virtual void release(CanvasView *, const ToolEvent &) {}
    virtual void hover(CanvasView *, const ToolEvent &) {}
    virtual bool key(CanvasView *, QKeyEvent *) { return false; }
    virtual void paintOverlay(CanvasView *, QPainter &) {}
    virtual QCursor cursor() const { return Qt::CrossCursor; }
    // Brush-like tools show a circle cursor with this document-space radius (0 = none).
    virtual double brushRadius() const { return 0.0; }
    // Options widget shown in the tool options bar (owned by the caller).
    virtual QWidget *createOptions(QWidget *parent) { Q_UNUSED(parent); return nullptr; }

signals:
    void overlayChanged();
    void message(const QString &text);

protected:
    EditorState *state() const { return m_state; }

private:
    EditorState *m_state;
};

} // namespace lumen::ui
