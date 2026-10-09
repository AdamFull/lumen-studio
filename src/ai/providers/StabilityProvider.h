#pragma once

#include "ai/Provider.h"
#include "net/HttpClient.h"

namespace lumen::ai {

// Stability AI platform API (v2beta stable-image endpoints).
class StabilityProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();
    StabilityProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;

    static ImageResult parseResponse(const QByteArray &body);

private:
    QNetworkRequest request(const QString &path) const;
    void runMultipart(ImageOperation *op, const QString &path, std::function<void(net::MultipartBuilder &)> fill);
};

} // namespace lumen::ai
