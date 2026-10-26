#pragma once

#include "ai/Segmenter.h"
#include "ui/canvas/Tools.h"

#include <QImage>
#include <QPointer>

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

class Studio;

// Smart selection (plan M18 §18.1): a click selects the object under it, a drag selects the object
// in the box. Shift+click adds a point on the same object, Alt+click a point that is not part of
// it, refining what the click found. The options bar's New / Add / Subtract / Intersect and Feather
// apply as for the other selection tools, against the selection there was before this object. SAM
// 2.1 does the cutting out (SmartSelect); the first use offers its download.
class SmartSelectTool : public SelectionTool
{
    Q_OBJECT
public:
    SmartSelectTool(EditorState *state, Studio *studio, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("smart-select"); }
    QString name() const override { return tr("Smart selection"); }
    QString iconName() const override { return QStringLiteral("wand"); }
    QString shortcut() const override { return QStringLiteral("W"); }
    QString hint() const override
    {
        return tr("Click an object to select it, or drag a box around it. Shift+click adds a point on it, Alt+click one that "
                  "is not part of it.");
    }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;

    // The prompt of the object being selected (tests).
    const ai::SegmentPrompt &prompt() const { return m_prompt; }
    bool isBusy() const { return m_busy; }

signals:
    // A mask came back and the selection shows it (tests).
    void selected();

private:
    void run(CanvasView *view);
    void apply(const QImage &mask);
    QImage picture(doc::Document *doc);

    Studio *m_studio;
    QPointF m_start;
    QPointF m_current;
    bool m_pressed = false;
    bool m_dragging = false;
    Qt::KeyboardModifiers m_mods;
    // The object being selected: its prompt, and the selection before it.
    QPointer<doc::Document> m_doc;
    ai::SegmentPrompt m_prompt;
    QImage m_base;
    bool m_busy = false;
    bool m_pending = false;
    bool m_asking = false;
    int m_request = 0;
    // The picture as SAM sees it, until the document's pixels change.
    QImage m_picture;
    QPointer<doc::Document> m_pictureDoc;
};

} // namespace lumen::ui
