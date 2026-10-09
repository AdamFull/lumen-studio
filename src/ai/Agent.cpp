#include "ai/Agent.h"

#include "ai/Provider.h"
#include "ai/StructuredChat.h"
#include "core/Log.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace lumen::ai {

namespace {

QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace

ToolResult ToolResult::failure(const QString &message)
{
    ToolResult r;
    r.error = message;
    return r;
}

QJsonObject ToolResult::toJson() const
{
    QJsonObject o = data;
    if (!assetIds.isEmpty()) {
        QJsonArray ids;
        for (const QString &id : assetIds)
            ids.append(id);
        o.insert(QStringLiteral("assets"), ids);
    }
    if (!error.isEmpty())
        o.insert(QStringLiteral("error"), error);
    if (o.isEmpty())
        o.insert(QStringLiteral("ok"), true);
    return o;
}

// ---- ToolSet ---------------------------------------------------------------------------------------

void ToolSet::add(const ToolDefinition &definition, ToolHandler handler)
{
    m_definitions.removeIf([&](const ToolDefinition &d) { return d.name == definition.name; });
    m_definitions.append(definition);
    m_handlers.insert(definition.name, std::move(handler));
}

QList<ToolDefinition> ToolSet::definitions() const
{
    return m_definitions;
}

bool ToolSet::contains(const QString &name) const
{
    return m_handlers.contains(name);
}

QFuture<ToolResult> ToolSet::run(const QString &name, const QJsonObject &arguments) const
{
    const ToolHandler handler = m_handlers.value(name);
    if (!handler)
        return QtFuture::makeReadyValueFuture(ToolResult::failure(QStringLiteral("unknown tool \"%1\"").arg(name)));
    try {
        return handler(arguments);
    } catch (const std::exception &e) {
        return QtFuture::makeReadyValueFuture(ToolResult::failure(QString::fromUtf8(e.what())));
    }
}

// ---- AgentRun -------------------------------------------------------------------------------------

AgentRun::AgentRun(LlmProvider *provider, const QString &model, const ToolSet &tools, QList<ChatMessage> messages, int maxSteps,
                   QObject *parent)
    : Operation(parent)
    , m_provider(provider)
    , m_model(model)
    , m_tools(tools)
    , m_messages(std::move(messages))
    , m_maxSteps(std::max(1, maxSteps))
{
    onCancel([this] {
        m_cancelled = true;
        if (m_current)
            m_current->cancel();
        reportCancelled();
    });
}

QString AgentRun::jsonProtocol(const QList<ToolDefinition> &tools)
{
    QString text = QStringLiteral(
        "You can use tools. Answer every turn with ONE JSON object and nothing else:\n"
        "- to call a tool: {\"action\": \"<tool name>\", \"arguments\": {...}}\n"
        "- to answer the user: {\"action\": \"reply\", \"reply\": \"<your answer>\"}\n"
        "After a tool call you receive its result and continue. Available tools:\n");
    for (const ToolDefinition &t : tools)
        text += QStringLiteral("\n### %1\n%2\nArguments (JSON schema): %3\n").arg(t.name, t.description, compact(t.parameters));
    return text;
}

QJsonObject AgentRun::jsonProtocolSchema(const QList<ToolDefinition> &tools)
{
    QJsonArray actions{QStringLiteral("reply")};
    for (const ToolDefinition &t : tools)
        actions.append(t.name);
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("action"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), actions}}},
                     {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}},
                     {QStringLiteral("reply"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("action")}}};
}

void AgentRun::start()
{
    if (!m_provider) {
        fail(tr("No language model is configured for the assistant. Add one in Settings → Providers."));
        return;
    }
    m_native = m_provider->capabilities().tools;
    if (!m_native) {
        // Describe the protocol once, in the system message.
        const QString protocol = jsonProtocol(m_tools.definitions());
        if (!m_messages.isEmpty() && m_messages.first().role == ChatRole::System)
            m_messages.first().text += QStringLiteral("\n\n") + protocol;
        else
            m_messages.prepend(ChatMessage::system(protocol));
    }
    QMetaObject::invokeMethod(this, &AgentRun::step, Qt::QueuedConnection);
}

void AgentRun::step()
{
    if (m_cancelled || isFinished())
        return;
    if (m_steps >= m_maxSteps) {
        finishWith(tr("I stopped after %n tool step(s). Tell me how to continue.", nullptr, m_steps));
        return;
    }
    ++m_steps;
    reportProgress(-1.0, tr("Thinking…"));
    if (m_native)
        stepNative();
    else
        stepJson();
}

void AgentRun::stepNative()
{
    ChatRequest req;
    req.model = m_model;
    req.messages = m_messages;
    req.tools = m_tools.definitions();
    req.temperature = 0.3;
    req.stream = true;
    ChatOperation *op = m_provider->chat(req);
    m_current = op;
    connect(op, &ChatOperation::textDelta, this, &AgentRun::textDelta);
    connect(op, &Operation::finished, this, [this, op] {
        if (m_cancelled || isFinished())
            return;
        if (op->state() != State::Succeeded) {
            fail(op->errorString());
            return;
        }
        const ChatResult result = op->result();
        m_messages.append(ChatMessage::fromResult(result));
        if (result.toolCalls.isEmpty()) {
            finishWith(result.text);
            return;
        }
        runTools(result.toolCalls, [this] { step(); });
    });
}

void AgentRun::stepJson()
{
    ChatRequest req;
    req.model = m_model;
    req.messages = m_messages;
    req.temperature = 0.2;
    req.stream = false;
    req.jsonSchema = jsonProtocolSchema(m_tools.definitions());
    req.schemaName = QStringLiteral("agent_step");
    StructuredChatOperation *op = requestJson(m_provider, req);
    m_current = op;
    connect(op, &Operation::finished, this, [this, op] {
        if (m_cancelled || isFinished())
            return;
        if (op->state() != State::Succeeded) {
            fail(op->errorString());
            return;
        }
        const QJsonObject answer = op->result();
        m_messages.append(ChatMessage::assistant(compact(answer)));
        const QString action = answer.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("reply") || action.isEmpty()) {
            const QString reply = answer.value(QStringLiteral("reply")).toString();
            emit textDelta(reply);
            finishWith(reply);
            return;
        }
        ToolCall call;
        call.id = QStringLiteral("call_%1").arg(m_steps);
        call.name = action;
        call.arguments = answer.value(QStringLiteral("arguments")).toObject();
        runTools({call}, [this] { step(); });
    });
}

void AgentRun::runTools(QList<ToolCall> calls, std::function<void()> then)
{
    if (calls.isEmpty()) {
        then();
        return;
    }
    const ToolCall call = calls.takeFirst();
    reportProgress(-1.0, tr("Running %1…").arg(call.name));
    emit toolStarted(call.id, call.name, call.arguments);
    QPointer<AgentRun> self(this);
    m_tools.run(call.name, call.arguments)
        .then(this, [self, call, calls, then](ToolResult result) {
            if (!self || self->m_cancelled || self->isFinished())
                return;
            self->m_assets += result.assetIds;
            emit self->toolFinished(call.id, call.name, result);
            const QString content = compact(result.toJson());
            if (self->m_native)
                self->m_messages.append(ChatMessage::toolResult(call.id, call.name, content));
            else
                self->m_messages.append(ChatMessage::user(QStringLiteral("Result of %1: %2").arg(call.name, content)));
            self->runTools(calls, then);
        })
        .onFailed(this, [self, call, calls, then](const std::exception &e) {
            if (!self || self->m_cancelled || self->isFinished())
                return;
            const ToolResult failed = ToolResult::failure(QString::fromUtf8(e.what()));
            emit self->toolFinished(call.id, call.name, failed);
            const QString content = compact(failed.toJson());
            if (self->m_native)
                self->m_messages.append(ChatMessage::toolResult(call.id, call.name, content));
            else
                self->m_messages.append(ChatMessage::user(QStringLiteral("Result of %1: %2").arg(call.name, content)));
            self->runTools(calls, then);
        });
}

void AgentRun::finishWith(const QString &text)
{
    m_reply = text;
    if (!m_native && !text.isEmpty()) {
        // Keep the transcript readable for the next turn.
        m_messages.last() = ChatMessage::assistant(text);
    }
    complete();
}

} // namespace lumen::ai
