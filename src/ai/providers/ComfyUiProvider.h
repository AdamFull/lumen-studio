#pragma once

#include "ai/Provider.h"

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

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
    // Part segmentation templates (mode "segment_parts"): output node id -> part label, and whether
    // outputs are RGBA layers, masks, or decided per image ("auto").
    QJsonObject outputs;
    QString outputKind = QStringLiteral("auto");

    int sizeMultiple = 8;         // LayerDiffuse decoding needs multiples of 64

    bool isSegmentation() const { return modes.contains(QStringLiteral("segment_parts")); }
    // Transparent part completion (LayerDiffuse); selected per request, never the main workflow.
    bool isLayerDiffuse() const { return modes.contains(QStringLiteral("layer_diffuse")); }
    bool isGeneration() const { return !isLayerDiffuse() && modes.size() > (isSegmentation() ? 1 : 0); }
    ImageCapabilities capabilities() const;

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
    // The LoraLoader node's choices (relative to ComfyUI's loras folder).
    QFuture<QList<ServerLora>> availableLoras() override;

    ImageCapabilities capabilities() const override;
    ImageCapabilities capabilitiesFor(const ImageRequest &request) const override;
    ImageOperation *generate(const ImageRequest &request) override;

    // Request extra selecting a template for one request: extras["comfy_template"] = template id.
    static constexpr const char *kTemplateExtra = "comfy_template";
    static std::optional<ComfyTemplate> findTemplate(const QString &id);
    static std::optional<ComfyTemplate> layerDiffuseTemplate();
    bool supportsUtility(Utility u) const override;
    ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params) override;
    // With "Free VRAM when idle": unloads ComfyUI's models now (when no job is running).
    QFuture<void> releaseGpu() override;

    // Graph construction (exposed for tests). Image inputs refer to uploaded file names.
    struct UploadedImages
    {
        QString init, mask, pose;
        QStringList references;
    };
    QJsonObject buildStandardGraph(const ImageRequest &request, const UploadedImages &images, bool hasIpAdapter) const;
    static QJsonObject applyTemplate(const ComfyTemplate &tpl, const ImageRequest &request, const UploadedImages &images);
    // Chains a LoraLoader per LoRA after every checkpoint loader and moves the loader's model and
    // CLIP consumers behind the chain (works for templates too).
    static QJsonObject insertLoras(QJsonObject graph, const QList<LoraRef> &loras);

protected:
    void onConfigured() override;

private:
    ImageOperation *generateImpl(const ImageRequest &request);
    void generateInto(ImageOperation *op, const ImageRequest &request);
    ImageOperation *runUtilityImpl(Utility utility, const QImage &image, const QJsonObject &params);
    // "Free VRAM when idle": counts running operations and asks ComfyUI to unload its models a few
    // seconds after the last one, so a local LLM can use the GPU.
    ImageOperation *track(ImageOperation *op);
    void freeVramSoon();
    QNetworkRequest request(const QString &path) const;
    QFuture<QStringList> nodeOptions(const QString &nodeClass, const QString &input);
    QFuture<bool> hasNode(const QString &nodeClass);
    std::optional<ComfyTemplate> activeTemplate() const;
    std::optional<ComfyTemplate> templateFor(const ImageRequest &request) const;
    std::optional<ComfyTemplate> segmentationTemplate() const;
    ImageOperation *segmentParts(const QImage &image, const QJsonObject &params);
    void runGraph(ImageOperation *op, std::function<QJsonObject(const UploadedImages &)> buildGraph, const QList<QPair<QString, QImage>> &uploads);

    QString m_clientId;
    QHash<QString, bool> m_nodeCache;
    int m_activeOps = 0;
    QPointer<QTimer> m_freeTimer;
};

} // namespace lumen::ai
