#include "ai/StructuredChat.h"

#include "ai/Gate.h"
#include "core/JsonSchema.h"
#include "core/JsonUtil.h"
#include "core/Log.h"

namespace lumen::ai {

StructuredChatOperation::StructuredChatOperation(LlmProvider *provider, ChatRequest request, int maxRepairs, QObject *parent)
    : Operation(parent)
    , m_provider(provider)
    , m_request(std::move(request))
    , m_maxRepairs(maxRepairs)
{
    onCancel([this] {
        if (m_current)
            m_current->cancel();
        else
            reportCancelled();
    });
}

void StructuredChatOperation::start()
{
    QMetaObject::invokeMethod(this, &StructuredChatOperation::runAttempt, Qt::QueuedConnection);
}

void StructuredChatOperation::runAttempt()
{
    if (isFinished())
        return;
    if (!m_provider) {
        fail(tr("No language model is configured. Add one in Settings → Providers."));
        return;
    }
    ++m_attempt;
    reportProgress(-1.0, m_attempt == 1 ? tr("Thinking...") : tr("Correcting the answer (attempt %1)...").arg(m_attempt));
    ChatOperation *op = gatedChat(m_provider, m_request);
    m_current = op;
    connect(op, &ChatOperation::textDelta, this, &StructuredChatOperation::textDelta);
    connect(op, &Operation::finished, this, [this, op] {
        m_current = nullptr;
        if (op->state() == State::Cancelled) {
            reportCancelled();
            return;
        }
        if (op->state() == State::Failed) {
            fail(op->errorString());
            return;
        }
        m_raw = op->result().text;
        QString parseError;
        QJsonValue value = json::extractJson(m_raw, &parseError);
        // Some models answer through a tool call even when asked for JSON.
        if (!value.isObject() && !op->result().toolCalls.isEmpty())
            value = op->result().toolCalls.first().arguments;
        QStringList errors;
        if (!value.isObject()) {
            errors << QStringLiteral("the reply was not a JSON object (%1)").arg(parseError);
        } else {
            json::SchemaValidator validator(m_request.jsonSchema);
            validator.validate(value, &errors);
        }
        if (errors.isEmpty()) {
            m_result = value.toObject();
            complete();
            return;
        }
        qCInfo(lcAi) << "Structured output invalid (attempt" << m_attempt << "):" << errors.mid(0, 8);
        if (m_attempt > m_maxRepairs) {
            fail(tr("The language model did not produce a valid answer: %1").arg(errors.mid(0, 5).join(QStringLiteral("; "))));
            return;
        }
        // Repair loop: show the model its answer and the exact validation errors.
        m_request.messages.append(ChatMessage::assistant(m_raw.left(20000)));
        m_request.messages.append(ChatMessage::user(
            QStringLiteral("Your previous reply is invalid:\n- %1\nReply again with ONLY the complete corrected JSON object.")
                .arg(errors.mid(0, 20).join(QStringLiteral("\n- ")))));
        runAttempt();
    });
}

StructuredChatOperation *requestJson(LlmProvider *provider, const ChatRequest &request, int maxRepairs)
{
    auto *op = new StructuredChatOperation(provider, request, maxRepairs);
    op->start();
    return op;
}

} // namespace lumen::ai
