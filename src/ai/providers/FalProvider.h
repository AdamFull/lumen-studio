#pragma once

#include "ai/Provider.h"
#include "net/HttpClient.h"

namespace lumen::ai {

// fal.ai serverless models through the queue API (submit, poll the status, fetch the result).
// One model endpoint per mode, configurable: FLUX for text-to-image and image-to-image, FLUX Fill
// for inpainting, FLUX Kontext for instruction edits, BiRefNet and ESRGAN for the utilities.
class FalProvider : public ImageProvider
{
    Q_OBJECT
public:
    // A model call: endpoint id (e.g. "fal-ai/flux/dev") and its JSON input.
    struct Call
    {
        QString model;
        QJsonObject input;
    };

    static ProviderType describe();
    FalProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;

    // Pure helpers (tested).
    Call buildCall(const ImageRequest &request) const;
    // Image URLs (http(s) or data URIs) of a result: {"images": [{"url"}]} or {"image": {"url"}}.
    static QStringList imageUrls(const QJsonObject &output);

private:
    QNetworkRequest request(const QUrl &url) const;
    // Submits `call` (its input built on a worker thread) and completes `op` with the result images.
    void runCall(ImageOperation *op, std::function<Call()> build);
};

} // namespace lumen::ai
