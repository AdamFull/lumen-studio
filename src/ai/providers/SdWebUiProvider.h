#pragma once

#include "ai/Provider.h"

#include <QStringList>

namespace lumen::ai {

// AUTOMATIC1111 / Forge / reForge Stable Diffusion WebUI (started with --api).
// Uses the ControlNet extension (built into Forge) for pose control and IP-Adapter references.
class SdWebUiProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();

    SdWebUiProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    QFuture<QStringList> listOptions(const QString &settingKey) override;
    QFuture<QStringList> listSamplers() override;
    QFuture<QStringList> listSchedulers() override;

    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;

    // Exposed for tests: builds the txt2img/img2img JSON payload (encodes images).
    QJsonObject buildPayload(const ImageRequest &request, const QStringList &controlNetModels) const;
    static ImageResult parseResponse(const QByteArray &body, int expectedImages);
    static QString pickControlNetModel(const QStringList &models, const QString &kind, const QString &checkpoint);

protected:
    void onConfigured() override;

private:
    QNetworkRequest request(const QString &path) const;
    QFuture<QStringList> controlNetModels();
    QString checkpointName(const ImageRequest &request) const;
    qint64 nativePixels() const;

    QStringList m_cnModels;
    bool m_cnModelsLoaded = false;
};

} // namespace lumen::ai
