#pragma once

#include "ai/Operation.h"
#include "ai/Provider.h"

#include <QJsonObject>
#include <QPointer>

namespace lumen::ai {

// Runs a chat request whose answer must be a JSON object matching a schema. The reply is extracted,
// validated locally and, when invalid, the model is asked to correct it (up to `maxRepairs` times).
class StructuredChatOperation : public Operation
{
    Q_OBJECT
public:
    StructuredChatOperation(LlmProvider *provider, ChatRequest request, int maxRepairs = 2, QObject *parent = nullptr);

    void start();
    const QJsonObject &result() const { return m_result; }
    const QString &rawText() const { return m_raw; }
    int attempts() const { return m_attempt; }

signals:
    void textDelta(const QString &delta);

private:
    void runAttempt();

    QPointer<LlmProvider> m_provider;
    ChatRequest m_request;
    int m_maxRepairs;
    int m_attempt = 0;
    QJsonObject m_result;
    QString m_raw;
    QPointer<ChatOperation> m_current;
};

// Convenience: creates and starts the operation.
StructuredChatOperation *requestJson(LlmProvider *provider, const ChatRequest &request, int maxRepairs = 2);

} // namespace lumen::ai
