#pragma once

#include "ai/Provider.h"

#include <QHash>
#include <QJsonObject>

namespace lumen::ai {

// Small helper to assemble ComfyUI API-format graphs.
class ComfyGraph
{
public:
    // Adds a node and returns its id.
    QString add(const QString &classType, const QJsonObject &inputs = {});
    void set(const QString &node, const QString &input, const QJsonValue &value);
    static QJsonArray link(const QString &node, int output) { return QJsonArray{node, output}; }
    const QJsonObject &json() const { return m_nodes; }

private:
    QJsonObject m_nodes;
    int m_next = 1;
};

// A user workflow (exported from ComfyUI with "Save (API Format)") plus bindings that tell Lumen
// where to inject prompts, seeds, sizes and images. Stored as JSON files in the workflow directory.
struct ComfyTemplate
{
    QString id;
    QString name;
    QString description;
    QJsonObject workflow;
    QJsonObject bindings; // key → {"node": "...", "input": "..."} or array of those for reference_images
    QStringList modes;    // supported GenMode names
    int maxReferences = 0;
    bool poseControl = false;
    qint64 nativePixels = 1024 * 1024;

    static std::optional<ComfyTemplate> fromJson(const QJsonObject &json, const QString &id, QString *error = nullptr);
};

// ComfyUI server adapter.
class ComfyUiProvider : public ImageProvider
{
    Q_OBJECT
public:
    static ProviderType describe();
    static QString workflowDirectory();
    static QList<ComfyTemplate> loadTemplates();

    ComfyUiProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent);
    ~ComfyUiProvider() override;

    QFuture<QString> testConnection() override;
    QFuture<QStringList> listModels() override;
    QFuture<QStringList> listOptions(const QString &settingKey) override;
    QFuture<QStringList> listSamplers() override;
    QFuture<QStringList> listSchedulers() override;

    ImageCapabilities capabilities() const override;
    ImageOperation *generate(const ImageRequest &request) override;
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;

    // Graph construction (exposed for tests). Image inputs refer to uploaded file names.
    struct UploadedImages
    {
        QString init, mask, pose;
        QStringList references;
    };
    QJsonObject buildStandardGraph(const ImageRequest &request, const UploadedImages &images, bool hasIpAdapter) const;
    static QJsonObject applyTemplate(const ComfyTemplate &tpl, const ImageRequest &request, const UploadedImages &images);

protected:
    void onConfigured() override;

private:
    QNetworkRequest request(const QString &path) const;
    QFuture<QStringList> nodeOptions(const QString &nodeClass, const QString &input);
    QFuture<bool> hasNode(const QString &nodeClass);
    std::optional<ComfyTemplate> activeTemplate() const;
    void runGraph(ImageOperation *op, std::function<QJsonObject(const UploadedImages &)> buildGraph, const QList<QPair<QString, QImage>> &uploads);

    QString m_clientId;
    QHash<QString, bool> m_nodeCache;
};

} // namespace lumen::ai
