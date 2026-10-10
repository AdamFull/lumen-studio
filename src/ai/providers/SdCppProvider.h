#pragma once

#include "ai/Provider.h"

#include <QJsonObject>

namespace lumen::ai {

// stable-diffusion.cpp's sd-server through its native async API (/sdcpp/v1): jobs are submitted
// with POST img_gen and polled for progress, live previews and results; they can be cancelled.
// The server loads one model set at start-up, so ControlNet (pose control), IP-Adapter (character
// references) and the ESRGAN upscaler are only available when the server was started with them;
// for servers Lumen runs these settings follow the server's launch options automatically.
class SdCppProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();

    SdCppProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    QFuture<QStringList> listOptions(const QString &settingKey) override;
    QFuture<QStringList> listSamplers() override;
    QFuture<QStringList> listSchedulers() override;

    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;

    // Exposed for tests: the img_gen request body (encodes images; call from a worker thread).
    QJsonObject buildPayload(const ImageRequest &request) const;
    // The LoRAs a server lists in its capabilities.
    static QList<ServerLora> serverLoras(const QJsonObject &capabilities);

    int pollIntervalMs = 500;

private:
    QNetworkRequest request(const QString &path) const;
    QFuture<QJsonObject> serverCapabilities();
    void poll(ImageOperation *op, const QString &jobId, const ImageRequest &r);
};

} // namespace lumen::ai
