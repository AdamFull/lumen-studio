#pragma once

#include "ai/Provider.h"

#include <QMap>

namespace lumen::ai {

// Anthropic Messages API (Claude). Streaming, vision, tool use, structured outputs
// (output_config.format), effort control and server-side refusal fallbacks.
class AnthropicProvider : public LlmProvider
{
    Q_OBJECT
public:
    static ProviderType describe();

    AnthropicProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    LlmCapabilities capabilities() const override;
    ChatOperation *chat(const ChatRequest &request) override;

    // Exposed for tests.
    QJsonObject buildBody(const ChatRequest &request) const;
    QList<QPair<QByteArray, QByteArray>> headers(const QString &model) const;

private:
    QNetworkRequest request(const QString &path, const QString &model = QString()) const;
    bool fallbackSupported(const QString &model) const;
};

// Accumulates Anthropic streaming events into a ChatResult (shared with tests).
class AnthropicStreamAccumulator
{
public:
    // Returns text / thinking deltas for live display. Throws ProviderError on error events.
    void feed(const QString &event, const QByteArray &data, QString *textDelta, QString *thinkingDelta);
    // Processes a complete (non-streaming) response body.
    void feedMessage(const QJsonObject &message);
    ChatResult finish();

private:
    struct Block
    {
        QJsonObject block;
        QString partialJson;
    };
    QMap<int, Block> m_blocks;
    QString m_stopReason;
    QJsonObject m_stopDetails;
    QJsonObject m_usage;
};

} // namespace lumen::ai
