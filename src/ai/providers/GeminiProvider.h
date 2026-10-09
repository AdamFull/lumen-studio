#pragma once

#include "ai/Provider.h"

namespace lumen::ai {

// Google Gemini API (generativelanguage.googleapis.com).
// GeminiChatProvider: text/vision chat with streaming, function calling and JSON schema output.
// GeminiImageProvider: native image generation/editing models (multi-image references, instruction edits).
class GeminiChatProvider : public LlmProvider
{
    Q_OBJECT
public:
    static ProviderType describe();
    GeminiChatProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    LlmCapabilities capabilities() const override;
    ChatOperation *chat(const ChatRequest &request) override;

    QJsonObject buildBody(const ChatRequest &request) const;
};

class GeminiImageProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();
    GeminiImageProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;

    // Builds the multimodal prompt (instruction + ordered images) for a request. Exposed for tests.
    static QJsonObject buildBody(const ImageRequest &request, const QStringList &aspectRatios);
    static ImageResult parseResponse(const QByteArray &body);
};

// Shared helpers.
QNetworkRequest geminiRequest(const Provider *provider, const QString &path);
// Natural-language instruction describing how the attached images must be used (shared with OpenAI images).
QString referenceInstruction(const ImageRequest &request, int firstImageIndex, bool hasInit, bool hasPose);

} // namespace lumen::ai
