#pragma once

#include "ai/Provider.h"

namespace lumen::ai {

// Client of the Lumen Part Server protocol: a small HTTP service that wraps a character part
// segmentation model (the reference server in tools/partserver wraps see-through).
//
//   GET  /v1/info     -> {"name", "version", "model", "tags": [...], "capabilities": ["segment"]}
//   POST /v1/segment  <- {"image": base64 PNG, "parts": [{"tag", "prompt"}], "options": {"resolution", "seed"}}
//                     -> {"width", "height", "layers": [{"tag", "image": base64 PNG, "x", "y", "kind": "layer"|"mask",
//                                                         "order", "depth", "score"}], "info": {...}}
// Layers may be cropped; x/y place them on the canvas. Errors: HTTP 4xx/5xx with {"error": "..."}.
class PartServerProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();

    PartServerProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;

    // Parses a /v1/segment response (exposed for tests).
    static ImageResult parseSegmentResponse(const QByteArray &body, const QSize &canvas);
};

} // namespace lumen::ai
