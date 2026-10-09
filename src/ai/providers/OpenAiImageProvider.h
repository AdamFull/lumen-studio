#pragma once

#include "ai/Provider.h"

namespace lumen::ai {

// OpenAI Images API (GPT-Image models): generations, multi-image reference edits and masked edits.
class OpenAiImageProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();
    OpenAiImageProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;

    // Picks the supported output size closest to the requested aspect ratio.
    static QString pickSize(const QSize &requested);
    static ImageResult parseResponse(const QByteArray &body);

private:
    QNetworkRequest request(const QString &path) const;
};

} // namespace lumen::ai
