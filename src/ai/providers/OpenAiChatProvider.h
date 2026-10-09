#pragma once

#include "ai/Provider.h"

#include <QMap>

namespace lumen::ai {

// OpenAI Chat Completions API and every compatible server: llama.cpp (llama-server), Ollama, LM Studio,
// KoboldCpp, vLLM, OpenRouter, OpenAI, Groq, DeepSeek... Streaming, vision, structured output, tools.
class OpenAiChatProvider : public LlmProvider
{
    Q_OBJECT
public:
    enum class Flavor { OpenAI, LlamaCpp, Ollama, LmStudio, KoboldCpp, OpenRouter, Generic };
    static ProviderType describe(Flavor flavor);
    static QList<ProviderType> describeAll();

    OpenAiChatProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    LlmCapabilities capabilities() const override;
    ChatOperation *chat(const ChatRequest &request) override;

    // Exposed for tests.
    QJsonObject buildBody(const ChatRequest &request) const;
    Flavor flavor() const { return m_flavor; }

private:
    QNetworkRequest request(const QString &path) const;
    QString structuredMode() const;

    Flavor m_flavor;
};

// Streaming accumulator for OpenAI-style chat completion chunks (shared with tests).
class OpenAiStreamAccumulator
{
public:
    // Processes one SSE "data" payload. Returns text/reasoning deltas through out-params.
    void feed(const QByteArray &data, QString *textDelta, QString *reasoningDelta);
    bool isDone() const { return m_done; }
    ChatResult finish();

private:
    struct PendingCall
    {
        QString id, name, args;
    };
    QString m_text;
    QString m_reasoning;
    QMap<int, PendingCall> m_calls;
    QString m_finish;
    QJsonObject m_usage;
    bool m_done = false;
};

// Splits "<think>...</think>" reasoning emitted inline by some local models.
void splitThinking(QString &text, QString &reasoning);

} // namespace lumen::ai
