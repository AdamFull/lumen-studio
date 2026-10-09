#pragma once

#include <QFrame>
#include <QPointer>
#include <QTimer>

class QLineEdit;
class QToolButton;
class QPushButton;

namespace lumen::doc {
class Document;
}

namespace lumen::ui {

class CanvasView;

// Contextual generative toolbar floating next to the selection on the canvas: a prompt field
// with Generate (inpaint the selection), Remove (fill it from the surroundings) and a shortcut to
// the full AI panel. Hidden while drawing, when there is no selection, or after the user closes
// it (until the selection changes).
class ContextBar : public QFrame
{
    Q_OBJECT
public:
    explicit ContextBar(CanvasView *view);

    void setEnabledByUser(bool on);
    bool isEnabledByUser() const { return m_enabled; }
    // Number of generations started from the editor that are still running.
    void setRunning(int count);
    QString prompt() const;
    void setPrompt(const QString &prompt);
    // Recomputes the selection bounds and the position (coalesced).
    void schedule();
    // Document-space selection bounds the bar is attached to (empty = hidden).
    QRect anchorRect() const { return m_anchor; }

signals:
    void generateRequested(const QString &prompt);
    void removeRequested();
    void moreRequested();

private:
    void attach(doc::Document *doc);
    void refresh();
    void place();

    QPointer<CanvasView> m_view;
    QPointer<doc::Document> m_doc;
    QLineEdit *m_prompt;
    QPushButton *m_generate;
    QPushButton *m_remove;
    QToolButton *m_more;
    QToolButton *m_close;
    QTimer m_timer;
    QRect m_anchor;
    bool m_enabled = true;
    bool m_dismissed = false;
    bool m_interacting = false;
};

} // namespace lumen::ui
