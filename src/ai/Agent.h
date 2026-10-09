#pragma once

#include "ai/Operation.h"
#include "ai/Types.h"

#include <QFuture>
#include <QJsonObject>
#include <QPointer>

#include <functional>

namespace lumen::ai {

class LlmProvider;

// Outcome of a tool call: JSON data for the model, assets it produced, or an error the model can
// react to.
struct ToolResult
{
    QJsonObject data;
    QStringList assetIds;
    QString error;

    static ToolResult failure(const QString &message);
    // What the model sees: the data plus "assets" / "error".
    QJsonObject toJson() const;
};

using ToolHandler = std::function<QFuture<ToolResult>(const QJsonObject &arguments)>;

// Tools an agent may call. Handlers run on the GUI thread and return futures.
class ToolSet
{
public:
    void add(const ToolDefinition &definition, ToolHandler handler);
    QList<ToolDefinition> definitions() const;
    bool contains(const QString &name) const;
    QFuture<ToolResult> run(const QString &name, const QJsonObject &arguments) const;

private:
    QList<ToolDefinition> m_definitions;
    QHash<QString, ToolHandler> m_handlers;
};

// A tool-calling conversation turn: asks the model, runs the tools it calls, feeds the results back
// and repeats until the model answers in text (or the step limit is reached). Models with native
// tool calling use it; others follow a JSON protocol ({"action": tool | "reply", "arguments", "reply"}).
// `messages()` holds the whole exchange for the next turn.
class AgentRun : public Operation
{
    Q_OBJECT
public:
    AgentRun(LlmProvider *provider, const QString &model, const ToolSet &tools, QList<ChatMessage> messages, int maxSteps = 10,
             QObject *parent = nullptr);

    void start();
    const QList<ChatMessage> &messages() const { return m_messages; }
    QString reply() const { return m_reply; }
    QStringList producedAssets() const { return m_assets; }
    int steps() const { return m_steps; }
    bool usesNativeTools() const { return m_native; }

    // Instructions describing the JSON tool protocol (for models without native tool calling).
    static QString jsonProtocol(const QList<ToolDefinition> &tools);
    static QJsonObject jsonProtocolSchema(const QList<ToolDefinition> &tools);

signals:
    void textDelta(const QString &delta);
    void toolStarted(const QString &callId, const QString &name, const QJsonObject &arguments);
    void toolFinished(const QString &callId, const QString &name, const lumen::ai::ToolResult &result);

private:
    void step();
    void stepNative();
    void stepJson();
    void runTools(QList<ToolCall> calls, std::function<void()> then);
    void finishWith(const QString &text);

    QPointer<LlmProvider> m_provider;
    QString m_model;
    ToolSet m_tools;
    QList<ChatMessage> m_messages;
    int m_maxSteps;
    int m_steps = 0;
    bool m_native = true;
    QString m_reply;
    QStringList m_assets;
    QPointer<Operation> m_current;
    bool m_cancelled = false;
};

} // namespace lumen::ai

Q_DECLARE_METATYPE(lumen::ai::ToolResult)
