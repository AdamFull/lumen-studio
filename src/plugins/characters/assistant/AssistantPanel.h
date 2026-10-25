#pragma once

#include "ai/Agent.h"

#include <QFrame>
#include <QHash>
#include <QPointer>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace lumen::ui {

class AutoTextEdit;
class Studio;
class ThumbnailCache;

// Chat with the tool-calling assistant: it plans, generates, edits and poses through the studio's
// services (assistantTools). Text streams in; every tool call shows as a chip with its status and
// the images it produced (click to open them in the editor).
class AssistantPanel : public QFrame
{
    Q_OBJECT
public:
    explicit AssistantPanel(Studio *studio, QWidget *parent = nullptr);

    void send(const QString &text);
    void newChat();
    void stop();
    bool isRunning() const { return m_run != nullptr; }
    // Conversation so far (system prompt, messages, tool calls and results).
    const QList<ai::ChatMessage> &history() const { return m_history; }
    // Plain text of the last assistant answer shown.
    QString lastReply() const { return m_lastReply; }

signals:
    void closeRequested();
    void runFinished();

private:
    QLabel *addBubble(const QString &text, bool user);
    QWidget *addToolChip(const QString &callId, const QString &name, const QJsonObject &arguments);
    void finishToolChip(const QString &callId, const ai::ToolResult &result);
    void showSuggestions(bool show);
    void scrollToEnd();
    void updateModelLabel();
    void setRunning(bool running);

    Studio *m_studio;
    ThumbnailCache *m_thumbs;
    QLabel *m_model;
    QScrollArea *m_scroll;
    QVBoxLayout *m_messages;
    QWidget *m_suggestions;
    AutoTextEdit *m_input;
    QPushButton *m_send;
    QList<ai::ChatMessage> m_history;
    QPointer<ai::AgentRun> m_run;
    QPointer<QLabel> m_streaming;
    QString m_streamText;
    QString m_lastReply;
    QHash<QString, QPointer<QWidget>> m_chips;
};

} // namespace lumen::ui
