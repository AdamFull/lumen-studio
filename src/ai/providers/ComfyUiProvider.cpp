#include "ai/providers/ComfyUiProvider.h"

#include "ai/ProviderUtil.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JsonUtil.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <QSet>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QPromise>
#include <QtEndian>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#ifdef LUMEN_HAVE_WEBSOCKETS
#include <QWebSocket>
#endif

namespace lumen::ai {

namespace {

constexpr auto kAuto = "auto";
constexpr auto kBuiltin = "builtin";

QString pickByName(const QStringList &options, const QStringList &keys)
{
    for (const QString &key : keys) {
        for (const QString &o : options) {
            if (o.toLower().contains(key))
                return o;
        }
    }
    return {};
}

qint64 randomSeed()
{
    return qint64(QRandomGenerator::global()->bounded(quint32(2147483647)));
}

} // namespace

// ---- ComfyGraph -----------------------------------------------------------------------------------

QString ComfyGraph::add(const QString &classType, const QJsonObject &inputs)
{
    const QString id = QString::number(m_next++);
    m_nodes.insert(id, QJsonObject{{QStringLiteral("class_type"), classType}, {QStringLiteral("inputs"), inputs}});
    return id;
}

void ComfyGraph::set(const QString &node, const QString &input, const QJsonValue &value)
{
    QJsonObject n = m_nodes.value(node).toObject();
    QJsonObject inputs = n.value(QStringLiteral("inputs")).toObject();
    inputs.insert(input, value);
    n.insert(QStringLiteral("inputs"), inputs);
    m_nodes.insert(node, n);
}

// ---- ComfyTemplate ----------------------------------------------------------------------------------

std::optional<ComfyTemplate> ComfyTemplate::fromJson(const QJsonObject &json, const QString &id, QString *error)
{
    ComfyTemplate t;
    t.id = id;
    t.name = json.value(QStringLiteral("name")).toString(id);
    t.description = json.value(QStringLiteral("description")).toString();
    t.workflow = json.value(QStringLiteral("workflow")).toObject();
    t.bindings = json.value(QStringLiteral("bindings")).toObject();
    t.modes = json::toStringList(json.value(QStringLiteral("modes")).toArray());
    if (t.modes.isEmpty())
        t.modes = {QStringLiteral("txt2img")};
    t.maxReferences = json.value(QStringLiteral("max_references")).toInt(t.bindings.value(QStringLiteral("reference_images")).toArray().size());
    t.poseControl = json.value(QStringLiteral("pose_control")).toBool(t.bindings.contains(QStringLiteral("pose_image")));
    const int native = json.value(QStringLiteral("native_resolution")).toInt(1024);
    t.nativePixels = qint64(native) * native;
    t.sizeMultiple = std::max(1, json.value(QStringLiteral("size_multiple")).toInt(8));
    t.outputs = json.value(QStringLiteral("outputs")).toObject();
    t.outputKind = json.value(QStringLiteral("output_kind")).toString(QStringLiteral("auto"));
    if (t.workflow.isEmpty()) {
        if (error)
            *error = QStringLiteral("template has no \"workflow\" (export it with Save (API Format))");
        return std::nullopt;
    }
    for (auto it = t.workflow.begin(); it != t.workflow.end(); ++it) {
        if (!it.value().toObject().contains(QStringLiteral("class_type"))) {
            if (error)
                *error = QStringLiteral("workflow is not in API format (node %1 has no class_type)").arg(it.key());
            return std::nullopt;
        }
    }
    return t;
}

// ---- Provider -----------------------------------------------------------------------------------------

ProviderType ComfyUiProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("comfyui");
    t.name = QStringLiteral("ComfyUI");
    t.description = tr("Local ComfyUI server. Uses a built-in SD/SDXL graph (checkpoint, ControlNet OpenPose, IP-Adapter references, "
                       "inpainting) or your own exported workflows such as FLUX Kontext or Qwen-Image-Edit.");
    t.kind = ProviderKind::Image;
    t.local = true;
    t.icon = QStringLiteral("server");
    t.documentation = QUrl(QStringLiteral("https://docs.comfy.org/development/comfyui-server/comms_routes"));
    using F = SettingField::Type;
    QStringList workflows{QString::fromLatin1(kBuiltin)};
    QStringList segmenters{QString::fromLatin1(kAuto)};
    for (const ComfyTemplate &tpl : loadTemplates()) {
        if (tpl.isGeneration())
            workflows << tpl.id;
        if (tpl.isSegmentation())
            segmenters << tpl.id;
    }
    t.fields = {
        {QStringLiteral("url"), tr("Server URL"), F::Url, QStringLiteral("http://127.0.0.1:8188"), QString(), {}, 0, 0, true},
        {QStringLiteral("workflow"), tr("Workflow"), F::Choice, QString::fromLatin1(kBuiltin),
         tr("'builtin' builds an SD/SDXL graph automatically. Other entries are templates from the workflows folder."), workflows},
        {QStringLiteral("model"), tr("Checkpoint"), F::Model, QString(), tr("Used by the built-in graph.")},
        {QStringLiteral("native_resolution"), tr("Native resolution"), F::Choice, QStringLiteral("1024"), QString(),
         {QStringLiteral("512"), QStringLiteral("768"), QStringLiteral("1024"), QStringLiteral("1280")}},
        {QStringLiteral("sampler"), tr("Default sampler"), F::Model, QStringLiteral("dpmpp_2m"), QString()},
        {QStringLiteral("scheduler"), tr("Default scheduler"), F::Model, QStringLiteral("karras"), QString()},
        {QStringLiteral("pose_controlnet"), tr("OpenPose ControlNet"), F::Model, QString::fromLatin1(kAuto), tr("From models/controlnet.")},
        {QStringLiteral("ipadapter_preset"), tr("IP-Adapter preset"), F::Choice, QStringLiteral("PLUS (high strength)"),
         tr("Requires the ComfyUI_IPAdapter_plus nodes. 'none' disables character references in the built-in graph."),
         {QStringLiteral("PLUS (high strength)"), QStringLiteral("STANDARD (medium strength)"), QStringLiteral("PLUS FACE (portraits)"),
          QStringLiteral("FULL FACE - SD1.5 only (portraits stronger)"), QStringLiteral("none")}},
        {QStringLiteral("upscale_model"), tr("Upscale model"), F::Model, QString::fromLatin1(kAuto), tr("From models/upscale_models.")},
        {QStringLiteral("segment_workflow"), tr("Part segmentation workflow"), F::Choice, QString::fromLatin1(kAuto),
         tr("Template used to segment character parts (Live2D slicing). 'auto' picks the first segmentation template."),
         segmenters, 0, 0, false, true},
        {QStringLiteral("timeout_min"), tr("Timeout (minutes)"), F::Integer, 30, QString(), {}, 1, 240, false, true},
        {QStringLiteral("concurrency"), tr("Parallel jobs"), F::Integer, 1, QString(), {}, 1, 8, false, true},
        {QStringLiteral("free_vram"), tr("Free VRAM when idle"), F::Boolean, false,
         tr("Unload ComfyUI's models a few seconds after the last job, so a local language model (llama.cpp, Ollama, "
            "LM Studio) on the same GPU runs at full speed. Recommended for GPUs with 8-12 GB.")},
    };
    return t;
}

QString ComfyUiProvider::workflowDirectory()
{
    return paths::appData() + QStringLiteral("/workflows/comfyui");
}

QList<ComfyTemplate> ComfyUiProvider::loadTemplates()
{
    QList<ComfyTemplate> list;
    QStringList dirs{workflowDirectory(), paths::bundledData() + QStringLiteral("/workflows/comfyui")};
    QSet<QString> seen;
    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        for (const QString &file : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            const QString id = QFileInfo(file).completeBaseName();
            if (seen.contains(id))
                continue;
            QFile f(dir.filePath(file));
            if (!f.open(QIODevice::ReadOnly))
                continue;
            QString error;
            if (auto t = ComfyTemplate::fromJson(QJsonDocument::fromJson(f.readAll()).object(), id, &error)) {
                list.append(*t);
                seen.insert(id);
            } else {
                qCWarning(lcAi) << "Skipping ComfyUI template" << file << ":" << error;
            }
        }
    }
    return list;
}

ComfyUiProvider::ComfyUiProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
    , m_clientId(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

ComfyUiProvider::~ComfyUiProvider() = default;

void ComfyUiProvider::onConfigured()
{
    m_nodeCache.clear();
}

QNetworkRequest ComfyUiProvider::request(const QString &path) const
{
    return net::HttpClient::makeRequest(endpoint(path));
}

QFuture<QString> ComfyUiProvider::testConnection()
{
    net::CallOptions opts;
    opts.timeoutMs = 8000;
    opts.maxRetries = 0;
    return futureFromCall<QString>(http()->get(request(QStringLiteral("/system_stats")), opts), [](net::HttpCall *call) {
        const QJsonObject o = call->json();
        const QString version = o.value(QStringLiteral("system")).toObject().value(QStringLiteral("comfyui_version")).toString();
        const QJsonArray devices = o.value(QStringLiteral("devices")).toArray();
        const QString device = devices.isEmpty() ? QString() : devices.at(0).toObject().value(QStringLiteral("name")).toString();
        return tr("Connected - ComfyUI %1 %2").arg(version, device).simplified();
    });
}

QFuture<QStringList> ComfyUiProvider::nodeOptions(const QString &nodeClass, const QString &input)
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/object_info/") + nodeClass), opts), [nodeClass, input](net::HttpCall *call) {
        const QJsonObject node = call->json().value(nodeClass).toObject();
        const QJsonObject required = node.value(QStringLiteral("input")).toObject().value(QStringLiteral("required")).toObject();
        const QJsonValue spec = required.value(input).toArray().at(0);
        // Newer ComfyUI: ["COMBO", {"options": [...]}]; older: [[...options...]]
        if (spec.isArray())
            return json::toStringList(spec.toArray());
        return json::toStringList(required.value(input).toArray().at(1).toObject().value(QStringLiteral("options")).toArray());
    });
}

QFuture<bool> ComfyUiProvider::hasNode(const QString &nodeClass)
{
    if (m_nodeCache.contains(nodeClass))
        return QtFuture::makeReadyValueFuture(m_nodeCache.value(nodeClass));
    net::CallOptions opts;
    opts.timeoutMs = 10000;
    opts.maxRetries = 0;
    QPointer<ComfyUiProvider> self(this);
    net::HttpCall *call = http()->get(request(QStringLiteral("/object_info/") + nodeClass), opts);
    auto promise = std::make_shared<QPromise<bool>>();
    promise->start();
    connect(call, &net::HttpCall::finished, this, [self, call, promise, nodeClass] {
        const bool present = call->ok() && call->json().contains(nodeClass);
        if (self)
            self->m_nodeCache.insert(nodeClass, present);
        promise->addResult(present);
        promise->finish();
    });
    return promise->future();
}

QFuture<QStringList> ComfyUiProvider::listModels()
{
    return nodeOptions(QStringLiteral("CheckpointLoaderSimple"), QStringLiteral("ckpt_name"));
}

QFuture<QStringList> ComfyUiProvider::listSamplers()
{
    return nodeOptions(QStringLiteral("KSampler"), QStringLiteral("sampler_name"));
}

QFuture<QStringList> ComfyUiProvider::listSchedulers()
{
    return nodeOptions(QStringLiteral("KSampler"), QStringLiteral("scheduler"));
}

QFuture<QStringList> ComfyUiProvider::listOptions(const QString &settingKey)
{
    auto withAuto = [](QStringList l) {
        l.prepend(QString::fromLatin1(kAuto));
        return l;
    };
    if (settingKey == QLatin1String("pose_controlnet"))
        return nodeOptions(QStringLiteral("ControlNetLoader"), QStringLiteral("control_net_name")).then(withAuto);
    if (settingKey == QLatin1String("upscale_model"))
        return nodeOptions(QStringLiteral("UpscaleModelLoader"), QStringLiteral("model_name")).then(withAuto);
    if (settingKey == QLatin1String("sampler"))
        return listSamplers();
    if (settingKey == QLatin1String("scheduler"))
        return listSchedulers();
    return Provider::listOptions(settingKey);
}

std::optional<ComfyTemplate> ComfyUiProvider::activeTemplate() const
{
    const QString wf = text(QStringLiteral("workflow"));
    if (wf.isEmpty() || wf == QLatin1String(kBuiltin))
        return std::nullopt;
    for (const ComfyTemplate &t : loadTemplates()) {
        if (t.id == wf)
            return t;
    }
    return std::nullopt;
}

std::optional<ComfyTemplate> ComfyUiProvider::segmentationTemplate() const
{
    const QString wanted = text(QStringLiteral("segment_workflow"));
    std::optional<ComfyTemplate> first;
    for (const ComfyTemplate &t : loadTemplates()) {
        if (!t.isSegmentation())
            continue;
        if (t.id == wanted)
            return t;
        if (!first)
            first = t;
    }
    return first;
}

ImageCapabilities ComfyTemplate::capabilities() const
{
    ImageCapabilities c;
    c.negativePrompt = c.seed = c.steps = c.cfg = true;
    c.maxPixels = 2048 * 2048;
    c.sizeMultiple = sizeMultiple;
    c.textToImage = modes.contains(QStringLiteral("txt2img"));
    c.imageToImage = modes.contains(QStringLiteral("img2img"));
    c.inpaint = modes.contains(QStringLiteral("inpaint"));
    c.edit = modes.contains(QStringLiteral("edit"));
    c.maxReferences = maxReferences;
    c.poseControl = poseControl;
    c.nativePixels = nativePixels;
    c.samplers = bindings.contains(QStringLiteral("sampler"));
    c.batch = bindings.contains(QStringLiteral("batch"));
    c.transparentBackground = isLayerDiffuse();
    for (const QJsonValue &node : workflow) {
        if (node.toObject().value(QStringLiteral("class_type")).toString() == QLatin1String("CheckpointLoaderSimple"))
            c.loras = true;
    }
    return c;
}

std::optional<ComfyTemplate> ComfyUiProvider::findTemplate(const QString &id)
{
    for (const ComfyTemplate &t : loadTemplates()) {
        if (t.id == id)
            return t;
    }
    return std::nullopt;
}

std::optional<ComfyTemplate> ComfyUiProvider::layerDiffuseTemplate()
{
    for (const ComfyTemplate &t : loadTemplates()) {
        if (t.isLayerDiffuse())
            return t;
    }
    return std::nullopt;
}

std::optional<ComfyTemplate> ComfyUiProvider::templateFor(const ImageRequest &request) const
{
    const QString id = request.extras.value(QLatin1String(kTemplateExtra)).toString();
    if (!id.isEmpty()) {
        if (auto t = findTemplate(id))
            return t;
    }
    return activeTemplate();
}

ImageCapabilities ComfyUiProvider::capabilitiesFor(const ImageRequest &request) const
{
    if (request.extras.contains(QLatin1String(kTemplateExtra))) {
        if (auto t = templateFor(request))
            return t->capabilities();
    }
    return capabilities();
}

ImageCapabilities ComfyUiProvider::capabilities() const
{
    if (auto tpl = activeTemplate())
        return tpl->capabilities();
    ImageCapabilities c;
    c.negativePrompt = c.seed = c.steps = c.cfg = c.samplers = true;
    c.sizeMultiple = 8;
    c.maxPixels = 2048 * 2048;
    c.textToImage = c.imageToImage = c.inpaint = true;
    c.edit = false;
    c.poseControl = true;
    c.loras = true;
    c.maxReferences = text(QStringLiteral("ipadapter_preset")) == QLatin1String("none") ? 0 : 4;
    const int native = setting(QStringLiteral("native_resolution")).toInt();
    c.nativePixels = qint64(native > 0 ? native : 1024) * (native > 0 ? native : 1024);
    return c;
}

QJsonObject ComfyUiProvider::buildStandardGraph(const ImageRequest &r, const UploadedImages &images, bool hasIpAdapter) const
{
    ComfyGraph g;
    QString ckpt = r.model.isEmpty() ? text(QStringLiteral("model")) : r.model;
    if (ckpt.isEmpty())
        throw ProviderError(tr("Select a checkpoint for ComfyUI in Settings → Providers."));
    const QString loader = g.add(QStringLiteral("CheckpointLoaderSimple"), {{QStringLiteral("ckpt_name"), ckpt}});
    QJsonArray model = ComfyGraph::link(loader, 0);
    const QJsonArray clip = ComfyGraph::link(loader, 1);
    const QJsonArray vae = ComfyGraph::link(loader, 2);

    // Character references through IP-Adapter Plus (unified loader + one advanced apply over a batch).
    if (!images.references.isEmpty() && hasIpAdapter && text(QStringLiteral("ipadapter_preset")) != QLatin1String("none")) {
        const QString ipl = g.add(QStringLiteral("IPAdapterUnifiedLoader"),
                                  {{QStringLiteral("model"), model}, {QStringLiteral("preset"), text(QStringLiteral("ipadapter_preset"))}});
        QJsonArray batch;
        double weight = 0.0;
        for (int i = 0; i < images.references.size(); ++i) {
            const QString load = g.add(QStringLiteral("LoadImage"), {{QStringLiteral("image"), images.references[i]}});
            const QJsonArray img = ComfyGraph::link(load, 0);
            batch = batch.isEmpty() ? img : ComfyGraph::link(g.add(QStringLiteral("ImageBatch"), {{QStringLiteral("image1"), batch}, {QStringLiteral("image2"), img}}), 0);
            weight += i < r.references.size() ? r.references[i].weight : 0.8;
        }
        weight /= images.references.size();
        const QString apply = g.add(QStringLiteral("IPAdapterAdvanced"),
                                    {{QStringLiteral("model"), ComfyGraph::link(ipl, 0)},
                                     {QStringLiteral("ipadapter"), ComfyGraph::link(ipl, 1)},
                                     {QStringLiteral("image"), batch},
                                     {QStringLiteral("weight"), weight},
                                     {QStringLiteral("weight_type"), QStringLiteral("linear")},
                                     {QStringLiteral("combine_embeds"), QStringLiteral("average")},
                                     {QStringLiteral("start_at"), 0.0},
                                     {QStringLiteral("end_at"), 1.0},
                                     {QStringLiteral("embeds_scaling"), QStringLiteral("V only")}});
        model = ComfyGraph::link(apply, 0);
    }

    const QString pos = g.add(QStringLiteral("CLIPTextEncode"), {{QStringLiteral("text"), r.prompt}, {QStringLiteral("clip"), clip}});
    const QString neg = g.add(QStringLiteral("CLIPTextEncode"), {{QStringLiteral("text"), r.negativePrompt}, {QStringLiteral("clip"), clip}});
    QJsonArray positive = ComfyGraph::link(pos, 0);
    QJsonArray negative = ComfyGraph::link(neg, 0);

    if (!images.pose.isEmpty()) {
        QString cn = text(QStringLiteral("pose_controlnet"));
        if (cn.isEmpty() || cn == QLatin1String(kAuto))
            cn = r.extras.value(QStringLiteral("_auto_pose_controlnet")).toString();
        if (cn.isEmpty())
            throw ProviderError(tr("Pose control needs an OpenPose ControlNet in ComfyUI's models/controlnet folder "
                                   "(e.g. controlnet-openpose-sdxl or xinsir union). Select it in the provider settings."));
        double weight = 1.0, start = 0.0, end = 1.0;
        for (const ControlInput &c : r.controls) {
            if (c.type == ControlType::OpenPose) {
                weight = c.weight;
                start = c.start;
                end = c.end;
            }
        }
        const QString cnLoader = g.add(QStringLiteral("ControlNetLoader"), {{QStringLiteral("control_net_name"), cn}});
        const QString poseImg = g.add(QStringLiteral("LoadImage"), {{QStringLiteral("image"), images.pose}});
        const QString apply = g.add(QStringLiteral("ControlNetApplyAdvanced"),
                                    {{QStringLiteral("positive"), positive},
                                     {QStringLiteral("negative"), negative},
                                     {QStringLiteral("control_net"), ComfyGraph::link(cnLoader, 0)},
                                     {QStringLiteral("image"), ComfyGraph::link(poseImg, 0)},
                                     {QStringLiteral("strength"), weight},
                                     {QStringLiteral("start_percent"), start},
                                     {QStringLiteral("end_percent"), end},
                                     {QStringLiteral("vae"), vae}});
        positive = ComfyGraph::link(apply, 0);
        negative = ComfyGraph::link(apply, 1);
    }

    QJsonArray latent;
    double denoise = 1.0;
    const int batch = std::max(1, r.batchCount);
    if (r.mode == GenMode::TextToImage || images.init.isEmpty()) {
        latent = ComfyGraph::link(g.add(QStringLiteral("EmptyLatentImage"), {{QStringLiteral("width"), r.size.width()},
                                                                             {QStringLiteral("height"), r.size.height()},
                                                                             {QStringLiteral("batch_size"), batch}}), 0);
    } else {
        const QString init = g.add(QStringLiteral("LoadImage"), {{QStringLiteral("image"), images.init}});
        denoise = r.denoise;
        if (r.mode == GenMode::Inpaint && !images.mask.isEmpty()) {
            const QString maskImg = g.add(QStringLiteral("LoadImage"), {{QStringLiteral("image"), images.mask}});
            const QString mask = g.add(QStringLiteral("ImageToMask"), {{QStringLiteral("image"), ComfyGraph::link(maskImg, 0)},
                                                                       {QStringLiteral("channel"), QStringLiteral("red")}});
            const QString cond = g.add(QStringLiteral("InpaintModelConditioning"),
                                       {{QStringLiteral("positive"), positive},
                                        {QStringLiteral("negative"), negative},
                                        {QStringLiteral("vae"), vae},
                                        {QStringLiteral("pixels"), ComfyGraph::link(init, 0)},
                                        {QStringLiteral("mask"), ComfyGraph::link(mask, 0)},
                                        {QStringLiteral("noise_mask"), true}});
            positive = ComfyGraph::link(cond, 0);
            negative = ComfyGraph::link(cond, 1);
            latent = ComfyGraph::link(cond, 2);
        } else {
            latent = ComfyGraph::link(g.add(QStringLiteral("VAEEncode"), {{QStringLiteral("pixels"), ComfyGraph::link(init, 0)},
                                                                         {QStringLiteral("vae"), vae}}), 0);
        }
        if (batch > 1)
            latent = ComfyGraph::link(g.add(QStringLiteral("RepeatLatentBatch"), {{QStringLiteral("samples"), latent},
                                                                                 {QStringLiteral("amount"), batch}}), 0);
    }

    const QString sampler = r.sampler.isEmpty() ? text(QStringLiteral("sampler")) : r.sampler;
    const QString scheduler = r.scheduler.isEmpty() ? text(QStringLiteral("scheduler")) : r.scheduler;
    const QString ks = g.add(QStringLiteral("KSampler"), {{QStringLiteral("seed"), double(r.seed >= 0 ? r.seed : randomSeed())},
                                                          {QStringLiteral("steps"), r.steps},
                                                          {QStringLiteral("cfg"), r.cfg},
                                                          {QStringLiteral("sampler_name"), sampler.isEmpty() ? QStringLiteral("euler") : sampler},
                                                          {QStringLiteral("scheduler"), scheduler.isEmpty() ? QStringLiteral("normal") : scheduler},
                                                          {QStringLiteral("denoise"), denoise},
                                                          {QStringLiteral("model"), model},
                                                          {QStringLiteral("positive"), positive},
                                                          {QStringLiteral("negative"), negative},
                                                          {QStringLiteral("latent_image"), latent}});
    const QString decode = g.add(QStringLiteral("VAEDecode"), {{QStringLiteral("samples"), ComfyGraph::link(ks, 0)}, {QStringLiteral("vae"), vae}});
    g.add(QStringLiteral("PreviewImage"), {{QStringLiteral("images"), ComfyGraph::link(decode, 0)}});
    return g.json();
}

QJsonObject ComfyUiProvider::applyTemplate(const ComfyTemplate &tpl, const ImageRequest &r, const UploadedImages &images)
{
    QJsonObject wf = tpl.workflow;
    auto setInput = [&wf](const QJsonObject &binding, const QJsonValue &value) {
        const QString node = binding.value(QStringLiteral("node")).toString();
        const QString input = binding.value(QStringLiteral("input")).toString();
        if (node.isEmpty() || input.isEmpty() || !wf.contains(node))
            return;
        QJsonObject n = wf.value(node).toObject();
        QJsonObject inputs = n.value(QStringLiteral("inputs")).toObject();
        inputs.insert(input, value);
        n.insert(QStringLiteral("inputs"), inputs);
        wf.insert(node, n);
    };
    auto bind = [&](const QString &key, const QJsonValue &value) {
        const QJsonValue b = tpl.bindings.value(key);
        if (b.isObject())
            setInput(b.toObject(), value);
        else if (b.isArray())
            for (const QJsonValue &each : b.toArray())
                setInput(each.toObject(), value);
    };
    bind(QStringLiteral("prompt"), r.prompt);
    bind(QStringLiteral("negative"), r.negativePrompt);
    bind(QStringLiteral("seed"), double(r.seed >= 0 ? r.seed : randomSeed()));
    bind(QStringLiteral("steps"), r.steps);
    bind(QStringLiteral("cfg"), r.cfg);
    bind(QStringLiteral("width"), r.size.width());
    bind(QStringLiteral("height"), r.size.height());
    bind(QStringLiteral("batch"), std::max(1, r.batchCount));
    bind(QStringLiteral("denoise"), r.mode == GenMode::TextToImage ? 1.0 : r.denoise);
    if (!r.sampler.isEmpty())
        bind(QStringLiteral("sampler"), r.sampler);
    if (!r.scheduler.isEmpty())
        bind(QStringLiteral("scheduler"), r.scheduler);
    if (!r.model.isEmpty())
        bind(QStringLiteral("checkpoint"), r.model);
    if (!images.init.isEmpty())
        bind(QStringLiteral("init_image"), images.init);
    if (!images.mask.isEmpty())
        bind(QStringLiteral("mask_image"), images.mask);
    if (!images.pose.isEmpty())
        bind(QStringLiteral("pose_image"), images.pose);
    const QJsonArray refs = tpl.bindings.value(QStringLiteral("reference_images")).toArray();
    for (int i = 0; i < refs.size(); ++i) {
        // Unused reference slots repeat the last available reference so the graph stays valid.
        if (images.references.isEmpty())
            break;
        setInput(refs.at(i).toObject(), images.references.value(std::min<qsizetype>(i, images.references.size() - 1)));
    }
    return wf;
}

void ComfyUiProvider::runGraph(ImageOperation *op, std::function<QJsonObject(const UploadedImages &)> buildGraph,
                               const QList<QPair<QString, QImage>> &uploads)
{
    struct Run
    {
        UploadedImages images;
        QJsonObject graph;
        int pendingUploads = 0;
        QString promptId;
        bool cancelled = false;
        bool completed = false;
        QList<QPointer<net::HttpCall>> calls;
        QPointer<QTimer> poll;
#ifdef LUMEN_HAVE_WEBSOCKETS
        QPointer<QWebSocket> socket;
#endif
    };
    auto run = std::make_shared<Run>();
    QPointer<ComfyUiProvider> self(this);
    const int timeoutMin = std::max(1, setting(QStringLiteral("timeout_min")).toInt());

    auto cleanup = [run] {
        if (run->poll)
            run->poll->stop();
#ifdef LUMEN_HAVE_WEBSOCKETS
        if (run->socket)
            run->socket->close();
#endif
    };
    op->onCancel([self, run, op, cleanup] {
        run->cancelled = true;
        for (const auto &c : run->calls) {
            if (c)
                c->abort();
        }
        if (self && !run->promptId.isEmpty()) {
            net::CallOptions o;
            o.maxRetries = 0;
            self->http()->postJson(self->request(QStringLiteral("/queue")),
                                   {{QStringLiteral("delete"), QJsonArray{run->promptId}}}, o);
            self->http()->post(self->request(QStringLiteral("/interrupt")), QByteArray("{}"), "application/json", o);
        }
        cleanup();
        op->reportCancelled();
    });

    // Step 3: fetch outputs from history and download images.
    auto fetchResults = [self, op, run, cleanup] {
        if (!self || run->completed || run->cancelled)
            return;
        run->completed = true;
        cleanup();
        op->reportProgress(-1.0, tr("Downloading results..."));
        net::HttpCall *hist = self->http()->get(self->request(QStringLiteral("/history/") + run->promptId));
        QObject::connect(hist, &net::HttpCall::finished, op, [self, op, hist, run] {
            if (!self)
                return;
            if (!hist->ok()) {
                op->fail(hist->errorString());
                return;
            }
            const QJsonObject entry = hist->json().value(run->promptId).toObject();
            const QJsonObject status = entry.value(QStringLiteral("status")).toObject();
            if (status.value(QStringLiteral("status_str")).toString() == QLatin1String("error")) {
                QString msg = tr("ComfyUI execution failed");
                for (const QJsonValue &m : status.value(QStringLiteral("messages")).toArray()) {
                    const QJsonArray pair = m.toArray();
                    if (pair.at(0).toString() == QLatin1String("execution_error")) {
                        const QJsonObject d = pair.at(1).toObject();
                        msg = tr("ComfyUI node %1 (%2) failed: %3").arg(d.value(QStringLiteral("node_id")).toString(),
                                                                       d.value(QStringLiteral("node_type")).toString(),
                                                                       d.value(QStringLiteral("exception_message")).toString().trimmed());
                    }
                }
                op->fail(msg);
                return;
            }
            QList<QJsonObject> files;
            QList<QJsonObject> fileInfo;
            const QJsonObject outputs = entry.value(QStringLiteral("outputs")).toObject();
            for (auto it = outputs.begin(); it != outputs.end(); ++it) {
                const QJsonObject node = run->graph.value(it.key()).toObject();
                const QJsonObject info{{QStringLiteral("node"), it.key()},
                                       {QStringLiteral("class_type"), node.value(QStringLiteral("class_type")).toString()},
                                       {QStringLiteral("title"), node.value(QStringLiteral("_meta")).toObject().value(QStringLiteral("title")).toString()}};
                for (const QJsonValue &img : it.value().toObject().value(QStringLiteral("images")).toArray()) {
                    files.append(img.toObject());
                    fileInfo.append(info);
                }
            }
            if (files.isEmpty()) {
                op->fail(tr("The workflow finished without image outputs (add a SaveImage or PreviewImage node)."));
                return;
            }
            auto results = std::make_shared<QList<QByteArray>>(files.size());
            auto remaining = std::make_shared<int>(int(files.size()));
            for (int i = 0; i < files.size(); ++i) {
                QUrlQuery q;
                q.addQueryItem(QStringLiteral("filename"), files[i].value(QStringLiteral("filename")).toString());
                q.addQueryItem(QStringLiteral("subfolder"), files[i].value(QStringLiteral("subfolder")).toString());
                q.addQueryItem(QStringLiteral("type"), files[i].value(QStringLiteral("type")).toString());
                QUrl url = self->endpoint(QStringLiteral("/view"));
                url.setQuery(q);
                net::HttpCall *dl = self->http()->get(net::HttpClient::makeRequest(url));
                run->calls << dl;
                QObject::connect(dl, &net::HttpCall::finished, op, [op, dl, results, remaining, i, fileInfo] {
                    if (op->isFinished())
                        return;
                    if (!dl->ok()) {
                        op->fail(dl->errorString());
                        return;
                    }
                    (*results)[i] = dl->body();
                    if (--*remaining > 0)
                        return;
                    async::run([results, fileInfo] {
                        ImageResult r;
                        for (int k = 0; k < results->size(); ++k) {
                            GeneratedImage g;
                            g.image = decodeImageBytes(results->at(k));
                            g.info = fileInfo.value(k);
                            if (!g.image.isNull())
                                r.images.append(g);
                        }
                        return r;
                    }).then(op, [op](ImageResult r) {
                        if (r.images.isEmpty())
                            op->fail(tr("Could not decode the images returned by ComfyUI"));
                        else
                            op->succeed(r);
                    });
                });
            }
        });
    };

    // Step 2: queue the prompt and track progress.
    auto queuePrompt = [self, op, run, buildGraph, fetchResults, timeoutMin] {
        if (!self || run->cancelled)
            return;
        QJsonObject graph;
        try {
            graph = buildGraph(run->images);
            run->graph = graph;
        } catch (const std::exception &e) {
            op->fail(describeException(e));
            return;
        }
#ifdef LUMEN_HAVE_WEBSOCKETS
        auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, op);
        run->socket = socket;
        QUrl wsUrl = self->baseUrl();
        wsUrl.setScheme(wsUrl.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
        wsUrl.setPath(wsUrl.path() + QStringLiteral("/ws"));
        wsUrl.setQuery(QStringLiteral("clientId=") + self->m_clientId);
        QObject::connect(socket, &QWebSocket::textMessageReceived, op, [op, run, fetchResults](const QString &message) {
            const QJsonObject o = QJsonDocument::fromJson(message.toUtf8()).object();
            const QString type = o.value(QStringLiteral("type")).toString();
            const QJsonObject data = o.value(QStringLiteral("data")).toObject();
            if (!run->promptId.isEmpty() && data.contains(QStringLiteral("prompt_id"))
                && data.value(QStringLiteral("prompt_id")).toString() != run->promptId)
                return;
            if (type == QLatin1String("progress")) {
                const double max = data.value(QStringLiteral("max")).toDouble();
                const double value = data.value(QStringLiteral("value")).toDouble();
                if (max > 0)
                    op->reportProgress(value / max, tr("Step %1 / %2").arg(int(value)).arg(int(max)));
            } else if (type == QLatin1String("execution_start")) {
                op->reportProgress(-1.0, tr("Running workflow..."));
            } else if ((type == QLatin1String("executing") && data.value(QStringLiteral("node")).isNull() && !run->promptId.isEmpty())
                       || type == QLatin1String("execution_success") || type == QLatin1String("execution_error")
                       || type == QLatin1String("execution_interrupted")) {
                fetchResults();
            }
        });
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, op, [op](const QByteArray &message) {
            // [uint32 event=1 (preview)][uint32 format][image bytes]
            if (message.size() > 8 && qFromBigEndian<quint32>(message.constData()) == 1) {
                const QByteArray bytes = message.mid(8);
                async::run([bytes] { return decodeImageBytes(bytes); }).then(op, [op](QImage img) { op->reportPreview(img); });
            }
        });
        socket->open(wsUrl);
#endif
        net::CallOptions opts;
        opts.timeoutMs = 60000;
        opts.maxRetries = 0;
        const QJsonObject body{{QStringLiteral("prompt"), graph}, {QStringLiteral("client_id"), self->m_clientId}};
        net::HttpCall *call = self->http()->postJson(self->request(QStringLiteral("/prompt")), body, opts);
        run->calls << call;
        op->reportProgress(-1.0, tr("Queued"));
        QObject::connect(call, &net::HttpCall::finished, op, [self, op, call, run, fetchResults, timeoutMin] {
            if (!self || run->cancelled)
                return;
            if (!call->ok()) {
                op->fail(call->errorString());
                return;
            }
            run->promptId = call->json().value(QStringLiteral("prompt_id")).toString();
            if (run->promptId.isEmpty()) {
                op->fail(tr("ComfyUI did not accept the workflow"));
                return;
            }
            // Polling covers servers without WebSocket support and missed events; also enforces the timeout.
            auto *timer = new QTimer(op);
            run->poll = timer;
            auto started = std::make_shared<QElapsedTimer>();
            started->start();
            auto busy = std::make_shared<bool>(false);
            QObject::connect(timer, &QTimer::timeout, op, [self, op, run, fetchResults, started, busy, timeoutMin] {
                if (!self || run->completed || *busy)
                    return;
                if (started->elapsed() > qint64(timeoutMin) * 60000) {
                    op->fail(tr("ComfyUI did not finish within %1 minutes").arg(timeoutMin));
                    return;
                }
                *busy = true;
                net::CallOptions po;
                po.timeoutMs = 10000;
                po.maxRetries = 0;
                po.quiet = true;
                net::HttpCall *h = self->http()->get(self->request(QStringLiteral("/history/") + run->promptId), po);
                QObject::connect(h, &net::HttpCall::finished, op, [h, run, fetchResults, busy] {
                    *busy = false;
                    if (h->ok() && h->json().contains(run->promptId))
                        fetchResults();
                });
            });
#ifdef LUMEN_HAVE_WEBSOCKETS
            timer->start(3000);
#else
            timer->start(1000);
#endif
        });
    };

    // Step 1: upload input images.
    run->pendingUploads = int(uploads.size());
    if (uploads.isEmpty()) {
        QMetaObject::invokeMethod(op, queuePrompt, Qt::QueuedConnection);
        return;
    }
    op->reportProgress(-1.0, tr("Uploading images..."));
    for (const auto &upload : uploads) {
        const QString key = upload.first;
        const QImage image = upload.second;
        async::run([image] { return img::encode(image, "PNG", 1); })
            .then(op, [self, op, run, key, queuePrompt](QByteArray png) {
                if (!self || run->cancelled)
                    return;
                net::MultipartBuilder mb;
                mb.addFile(QStringLiteral("image"), QStringLiteral("lumen_%1.png").arg(QUuid::createUuid().toString(QUuid::Id128)), png,
                           QStringLiteral("image/png"));
                mb.addField(QStringLiteral("type"), QStringLiteral("input"));
                mb.addField(QStringLiteral("subfolder"), QStringLiteral("lumen"));
                mb.addField(QStringLiteral("overwrite"), QStringLiteral("true"));
                net::HttpCall *call = self->http()->post(self->request(QStringLiteral("/upload/image")), mb.finish(), mb.contentType());
                run->calls << call;
                QObject::connect(call, &net::HttpCall::finished, op, [op, call, run, key, queuePrompt] {
                    if (run->cancelled || op->isFinished())
                        return;
                    if (!call->ok()) {
                        op->fail(tr("Uploading an image to ComfyUI failed: %1").arg(call->errorString()));
                        return;
                    }
                    const QJsonObject o = call->json();
                    const QString sub = o.value(QStringLiteral("subfolder")).toString();
                    const QString name = sub.isEmpty() ? o.value(QStringLiteral("name")).toString()
                                                       : sub + u'/' + o.value(QStringLiteral("name")).toString();
                    if (key == QLatin1String("init"))
                        run->images.init = name;
                    else if (key == QLatin1String("mask"))
                        run->images.mask = name;
                    else if (key == QLatin1String("pose"))
                        run->images.pose = name;
                    else {
                        const int idx = key.mid(3).toInt();
                        while (run->images.references.size() <= idx)
                            run->images.references.append(QString());
                        run->images.references[idx] = name;
                    }
                    if (--run->pendingUploads == 0)
                        queuePrompt();
                });
            });
    }
}

ImageOperation *ComfyUiProvider::generate(const ImageRequest &r)
{
    return track(generateImpl(r));
}

ImageOperation *ComfyUiProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &params)
{
    return track(runUtilityImpl(utility, image, params));
}

ImageOperation *ComfyUiProvider::track(ImageOperation *op)
{
    if (!op)
        return op;
    ++m_activeOps;
    if (m_freeTimer)
        m_freeTimer->stop();
    QPointer<ComfyUiProvider> self(this);
    connect(op, &Operation::finished, this, [self] {
        if (self && --self->m_activeOps == 0)
            self->freeVramSoon();
    });
    return op;
}

QFuture<void> ComfyUiProvider::releaseGpu()
{
    if (!setting(QStringLiteral("free_vram")).toBool() || m_activeOps > 0)
        return QtFuture::makeReadyVoidFuture();
    if (m_freeTimer)
        m_freeTimer->stop();
    net::CallOptions o;
    o.maxRetries = 0;
    o.quiet = true;
    o.timeoutMs = 30000;
    net::HttpCall *call = http()->postJson(request(QStringLiteral("/free")),
                                           QJsonObject{{QStringLiteral("unload_models"), true}, {QStringLiteral("free_memory"), true}}, o);
    auto promise = std::make_shared<QPromise<void>>();
    promise->start();
    QFuture<void> future = promise->future();
    connect(call, &net::HttpCall::finished, this, [promise] {
        // ComfyUI unloads asynchronously after answering: give it a moment before the GPU is used again.
        QTimer::singleShot(1500, [promise] { promise->finish(); });
    });
    return future;
}

void ComfyUiProvider::freeVramSoon()
{
    if (!setting(QStringLiteral("free_vram")).toBool())
        return;
    if (!m_freeTimer) {
        m_freeTimer = new QTimer(this);
        m_freeTimer->setSingleShot(true);
        m_freeTimer->setInterval(4000);
        connect(m_freeTimer, &QTimer::timeout, this, [this] {
            if (m_activeOps > 0)
                return;
            net::CallOptions o;
            o.maxRetries = 0;
            o.quiet = true;
            o.timeoutMs = 15000;
            http()->postJson(request(QStringLiteral("/free")),
                             QJsonObject{{QStringLiteral("unload_models"), true}, {QStringLiteral("free_memory"), true}}, o);
        });
    }
    m_freeTimer->start();
}

QJsonObject ComfyUiProvider::insertLoras(QJsonObject graph, const QList<LoraRef> &loras)
{
    if (loras.isEmpty())
        return graph;
    int next = 1;
    for (auto it = graph.begin(); it != graph.end(); ++it)
        next = std::max(next, it.key().toInt() + 1);
    QStringList loaders;
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        if (it.value().toObject().value(QStringLiteral("class_type")).toString() == QLatin1String("CheckpointLoaderSimple"))
            loaders << it.key();
    }
    for (const QString &loader : std::as_const(loaders)) {
        QJsonArray model = ComfyGraph::link(loader, 0);
        QJsonArray clip = ComfyGraph::link(loader, 1);
        QSet<QString> chain;
        for (const LoraRef &l : loras) {
            QString id = QString::number(next++);
            while (graph.contains(id))
                id = QString::number(next++);
            graph.insert(id, QJsonObject{{QStringLiteral("class_type"), QStringLiteral("LoraLoader")},
                                         {QStringLiteral("inputs"), QJsonObject{{QStringLiteral("model"), model},
                                                                                {QStringLiteral("clip"), clip},
                                                                                {QStringLiteral("lora_name"), l.resolved.isEmpty() ? l.name : l.resolved},
                                                                                {QStringLiteral("strength_model"), l.weight},
                                                                                {QStringLiteral("strength_clip"), l.weight}}},
                                         {QStringLiteral("_meta"), QJsonObject{{QStringLiteral("title"), QStringLiteral("LoRA %1").arg(l.name)}}}});
            chain << id;
            model = ComfyGraph::link(id, 0);
            clip = ComfyGraph::link(id, 1);
        }
        // Everything that used the checkpoint's model or CLIP now uses the end of the chain.
        for (auto it = graph.begin(); it != graph.end(); ++it) {
            if (chain.contains(it.key()))
                continue;
            QJsonObject node = it.value().toObject();
            QJsonObject inputs = node.value(QStringLiteral("inputs")).toObject();
            bool changed = false;
            for (auto in = inputs.begin(); in != inputs.end(); ++in) {
                const QJsonArray link = in.value().toArray();
                if (link.size() != 2 || link.at(0).toString() != loader)
                    continue;
                if (link.at(1).toInt() == 0) {
                    in.value() = model;
                    changed = true;
                } else if (link.at(1).toInt() == 1) {
                    in.value() = clip;
                    changed = true;
                }
            }
            if (changed) {
                node.insert(QStringLiteral("inputs"), inputs);
                it.value() = node;
            }
        }
    }
    return graph;
}

ImageOperation *ComfyUiProvider::generateImpl(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (r.loras.isEmpty()) {
        generateInto(op, r);
        return op;
    }
    // LoRA names as ComfyUI lists them (relative to its loras folder).
    QPointer<ComfyUiProvider> self(this);
    nodeOptions(QStringLiteral("LoraLoader"), QStringLiteral("lora_name"))
        .then(op, [self, op, r](const QStringList &names) {
            if (!self)
                return;
            QList<ServerLora> available;
            for (const QString &n : names)
                available.append({QFileInfo(n).completeBaseName(), QDir::fromNativeSeparators(n)});
            ImageRequest req = r;
            QString error;
            if (!resolveLoras(req.loras, available, &error)) {
                op->fail(error);
                return;
            }
            for (LoraRef &l : req.loras) {
                for (const QString &n : names) {
                    if (QDir::fromNativeSeparators(n) == l.resolved)
                        l.resolved = n;
                }
            }
            self->generateInto(op, req);
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    return op;
}

void ComfyUiProvider::generateInto(ImageOperation *op, const ImageRequest &r)
{
    QList<QPair<QString, QImage>> uploads;
    if (r.mode != GenMode::TextToImage && !r.initImage.isNull())
        uploads.append({QStringLiteral("init"), img::flatten(r.initImage, Qt::white)});
    if (r.mode == GenMode::Inpaint && !r.mask.isNull())
        uploads.append({QStringLiteral("mask"), r.mask.convertToFormat(QImage::Format_RGB32)});
    for (const ControlInput &c : r.controls) {
        if (c.type == ControlType::OpenPose)
            uploads.append({QStringLiteral("pose"), c.image});
    }
    for (int i = 0; i < r.references.size(); ++i)
        uploads.append({QStringLiteral("ref%1").arg(i), img::thumbnail(r.references[i].image, 1024)});

    QPointer<ComfyUiProvider> self(this);
    if (auto tpl = templateFor(r)) {
        const ComfyTemplate t = *tpl;
        ImageRequest req = r;
        req.extras.remove(QLatin1String(kTemplateExtra));
        // Per-request templates (LayerDiffuse) use the provider's checkpoint unless the request names one.
        if (req.model.isEmpty() && r.extras.contains(QLatin1String(kTemplateExtra)))
            req.model = text(QStringLiteral("model"));
        runGraph(op, [t, req](const UploadedImages &images) { return insertLoras(applyTemplate(t, req, images), req.loras); }, uploads);
        return;
    }
    // The built-in graph needs to know which optional components are installed.
    const bool wantsPose = std::any_of(r.controls.begin(), r.controls.end(), [](const ControlInput &c) { return c.type == ControlType::OpenPose; });
    const bool wantsRefs = !r.references.isEmpty() && text(QStringLiteral("ipadapter_preset")) != QLatin1String("none");
    QFuture<bool> ipFuture = wantsRefs ? hasNode(QStringLiteral("IPAdapterUnifiedLoader")) : QtFuture::makeReadyValueFuture(false);
    QFuture<QStringList> cnFuture = wantsPose && (text(QStringLiteral("pose_controlnet")).isEmpty() || text(QStringLiteral("pose_controlnet")) == QLatin1String(kAuto))
                                        ? nodeOptions(QStringLiteral("ControlNetLoader"), QStringLiteral("control_net_name"))
                                        : QtFuture::makeReadyValueFuture(QStringList());
    ipFuture
        .then(op, [self, op, r, uploads, cnFuture](bool hasIp) mutable {
            cnFuture.then(op, [self, op, r, uploads, hasIp](QStringList cnModels) {
                if (!self)
                    return;
                ImageRequest req = r;
                if (!cnModels.isEmpty()) {
                    const QString ckpt = (r.model.isEmpty() ? self->text(QStringLiteral("model")) : r.model).toLower();
                    const bool xl = !ckpt.contains(QLatin1String("1.5")) && !ckpt.contains(QLatin1String("sd15"));
                    QString pick = pickByName(cnModels, xl ? QStringList{QStringLiteral("openpose_xl"), QStringLiteral("openpose-sdxl"),
                                                                         QStringLiteral("xl_openpose"), QStringLiteral("union")}
                                                           : QStringList{QStringLiteral("openpose")});
                    if (pick.isEmpty())
                        pick = pickByName(cnModels, {QStringLiteral("openpose"), QStringLiteral("pose"), QStringLiteral("union")});
                    req.extras.insert(QStringLiteral("_auto_pose_controlnet"), pick);
                }
                QPointer<ComfyUiProvider> s = self;
                self->runGraph(op, [s, req, hasIp](const UploadedImages &images) {
                    if (!s)
                        throw ProviderError(QStringLiteral("provider removed"));
                    return insertLoras(s->buildStandardGraph(req, images, hasIp), req.loras);
                }, uploads);
            }).onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
}

bool ComfyUiProvider::supportsUtility(Utility u) const
{
    if (u == Utility::SegmentParts)
        return segmentationTemplate().has_value();
    return u == Utility::Upscale || u == Utility::DetectPose;
}

ImageOperation *ComfyUiProvider::runUtilityImpl(Utility utility, const QImage &image, const QJsonObject &params)
{
    if (utility == Utility::SegmentParts)
        return segmentParts(image, params);
    auto *op = new ImageOperation(this);
    QPointer<ComfyUiProvider> self(this);
    if (utility == Utility::Upscale) {
        const double factor = params.value(QStringLiteral("factor")).toDouble(2.0);
        nodeOptions(QStringLiteral("UpscaleModelLoader"), QStringLiteral("model_name"))
            .then(op, [self, op, image, factor](QStringList models) {
                if (!self)
                    return;
                QString model = self->text(QStringLiteral("upscale_model"));
                if (model.isEmpty() || model == QLatin1String(kAuto))
                    model = pickByName(models, {QStringLiteral("4x"), QStringLiteral("2x"), QStringLiteral("esrgan")});
                if (model.isEmpty() && !models.isEmpty())
                    model = models.first();
                if (model.isEmpty()) {
                    op->fail(tr("No upscale model found in ComfyUI's models/upscale_models folder."));
                    return;
                }
                const QSize target(int(image.width() * factor), int(image.height() * factor));
                self->runGraph(op, [model, target](const UploadedImages &images) {
                    ComfyGraph g;
                    const QString load = g.add(QStringLiteral("LoadImage"), {{QStringLiteral("image"), images.init}});
                    const QString loader = g.add(QStringLiteral("UpscaleModelLoader"), {{QStringLiteral("model_name"), model}});
                    const QString up = g.add(QStringLiteral("ImageUpscaleWithModel"), {{QStringLiteral("upscale_model"), ComfyGraph::link(loader, 0)},
                                                                                      {QStringLiteral("image"), ComfyGraph::link(load, 0)}});
                    const QString scale = g.add(QStringLiteral("ImageScale"), {{QStringLiteral("image"), ComfyGraph::link(up, 0)},
                                                                              {QStringLiteral("upscale_method"), QStringLiteral("lanczos")},
                                                                              {QStringLiteral("width"), target.width()},
                                                                              {QStringLiteral("height"), target.height()},
                                                                              {QStringLiteral("crop"), QStringLiteral("disabled")}});
                    g.add(QStringLiteral("PreviewImage"), {{QStringLiteral("images"), ComfyGraph::link(scale, 0)}});
                    return g.json();
                }, {{QStringLiteral("init"), image}});
            })
            .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
        return op;
    }
    if (utility == Utility::DetectPose) {
        hasNode(QStringLiteral("DWPreprocessor")).then(op, [self, op, image](bool present) {
            if (!self)
                return;
            if (!present) {
                op->fail(tr("Pose detection needs the comfyui_controlnet_aux custom nodes (DWPreprocessor)."));
                return;
            }
            self->runGraph(op, [image](const UploadedImages &images) {
                ComfyGraph g;
                const QString load = g.add(QStringLiteral("LoadImage"), {{QStringLiteral("image"), images.init}});
                const QString dw = g.add(QStringLiteral("DWPreprocessor"), {{QStringLiteral("image"), ComfyGraph::link(load, 0)},
                                                                           {QStringLiteral("detect_hand"), QStringLiteral("enable")},
                                                                           {QStringLiteral("detect_body"), QStringLiteral("enable")},
                                                                           {QStringLiteral("detect_face"), QStringLiteral("enable")},
                                                                           {QStringLiteral("resolution"), std::clamp(std::min(image.width(), image.height()), 512, 1024)},
                                                                           {QStringLiteral("bbox_detector"), QStringLiteral("yolox_l.onnx")},
                                                                           {QStringLiteral("pose_estimator"), QStringLiteral("dw-ll_ucoco_384_bs5.torchscript.pt")}});
                g.add(QStringLiteral("PreviewImage"), {{QStringLiteral("images"), ComfyGraph::link(dw, 0)}});
                return g.json();
            }, {{QStringLiteral("init"), image}});
        });
        return op;
    }
    failLater(op, tr("This utility is not available for ComfyUI"));
    return op;
}

// ---- Part segmentation ------------------------------------------------------------------------------

namespace {

// Normalises one workflow output into a part layer (RGBA) or mask (Grayscale8) on the source canvas.
GeneratedImage segmentOutput(const GeneratedImage &g, const QString &label, const QString &kindIn, int order, const QSize &canvas)
{
    QImage image = g.image;
    if (image.size() != canvas)
        image = image.scaled(canvas, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QString kind = kindIn;
    if (kind != QLatin1String("mask") && kind != QLatin1String("layer")) {
        kind = QStringLiteral("mask");
        if (image.hasAlphaChannel()) {
            const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < argb.height() && kind == QLatin1String("mask"); y += 4) {
                const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
                for (int x = 0; x < argb.width(); x += 4) {
                    if (qAlpha(row[x]) < 250) {
                        kind = QStringLiteral("layer");
                        break;
                    }
                }
            }
        }
    }
    GeneratedImage out;
    out.image = kind == QLatin1String("mask") ? img::maskFromLuminance(image) : img::normalized(image);
    out.info = QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("kind"), kind}, {QStringLiteral("order"), order}};
    return out;
}

QString outputLabel(const ComfyTemplate &t, const GeneratedImage &g)
{
    const QString node = g.info.value(QStringLiteral("node")).toString();
    if (!t.outputs.isEmpty())
        return t.outputs.value(node).toString();
    const QString title = g.info.value(QStringLiteral("title")).toString();
    return title.isEmpty() ? QStringLiteral("%1 %2").arg(g.info.value(QStringLiteral("class_type")).toString(), node) : title;
}

ImageRequest segmentationRequest(const QImage &image, const QString &prompt, const QJsonObject &params)
{
    ImageRequest req;
    req.mode = GenMode::ImageToImage;
    req.prompt = prompt;
    req.initImage = image;
    req.size = image.size();
    req.seed = params.value(QStringLiteral("seed")).toInt(-1);
    req.denoise = 1.0;
    return req;
}

} // namespace

ImageOperation *ComfyUiProvider::segmentParts(const QImage &image, const QJsonObject &params)
{
    auto *op = new ImageOperation(this);
    const std::optional<ComfyTemplate> tpl = segmentationTemplate();
    if (!tpl) {
        failLater(op, tr("No ComfyUI part segmentation template found. Add one with mode \"segment_parts\" to %1.")
                          .arg(QDir::toNativeSeparators(workflowDirectory())));
        return op;
    }
    const ComfyTemplate t = *tpl;
    const QJsonArray parts = params.value(QStringLiteral("parts")).toArray();
    const QSize canvas = image.size();

    // Whole-character templates (e.g. a decomposition graph): one run, outputs labelled by node.
    if (!t.bindings.contains(QStringLiteral("prompt"))) {
        auto *child = new ImageOperation(op);
        child->setAutoDelete(false);
        op->onCancel([child] { child->cancel(); });
        connect(child, &Operation::progress, op, &Operation::reportProgress);
        connect(child, &Operation::finished, op, [op, child, t, canvas] {
            child->deleteLater();
            if (child->state() == Operation::State::Cancelled) {
                op->reportCancelled();
                return;
            }
            if (child->state() == Operation::State::Failed) {
                op->fail(child->errorString());
                return;
            }
            ImageResult r;
            int order = 0;
            for (const GeneratedImage &g : child->result().images) {
                const QString label = outputLabel(t, g);
                if (label.isEmpty() || label == QLatin1String("ignore"))
                    continue;
                r.images.append(segmentOutput(g, label, t.outputKind, order++, canvas));
            }
            if (r.images.isEmpty())
                op->fail(tr("The segmentation workflow produced no labelled outputs (check its \"outputs\" map)."));
            else
                op->succeed(r);
        });
        const ImageRequest req = segmentationRequest(image, QString(), params);
        runGraph(child, [t, req](const UploadedImages &images) { return applyTemplate(t, req, images); },
                 {{QStringLiteral("init"), image}});
        return op;
    }

    // Text-prompted templates (e.g. Grounding DINO + SAM): one run per requested part.
    if (parts.isEmpty()) {
        failLater(op, tr("Choose the parts to segment."));
        return op;
    }
    struct State
    {
        int index = 0;
        ImageResult result;
        QStringList failures;
        QPointer<ImageOperation> current;
        std::function<void()> next;
    };
    auto state = std::make_shared<State>();
    QPointer<ComfyUiProvider> self(this);
    // `next` holds only a weak reference: the operation's handlers own the state (no cycle).
    state->next = [self, op, weak = std::weak_ptr<State>(state), parts, image, canvas, t, params] {
        const std::shared_ptr<State> state = weak.lock();
        if (!state || !self || op->isFinished())
            return;
        if (state->index >= parts.size()) {
            if (state->result.images.isEmpty()) {
                op->fail(state->failures.isEmpty() ? tr("None of the requested parts was found") : state->failures.first());
            } else {
                state->result.info.insert(QStringLiteral("failures"), json::toArray(state->failures));
                op->succeed(state->result);
            }
            return;
        }
        const QJsonObject part = parts[state->index].toObject();
        const QString tag = part.value(QStringLiteral("tag")).toString();
        QString prompt = part.value(QStringLiteral("prompt")).toString();
        if (prompt.isEmpty())
            prompt = tag;
        op->reportProgress(double(state->index) / double(parts.size()),
                           tr("Segmenting %1 (%2 of %3)").arg(tag).arg(state->index + 1).arg(parts.size()));
        auto *child = new ImageOperation(op);
        child->setAutoDelete(false);
        state->current = child;
        connect(child, &Operation::finished, op, [op, child, state, tag, canvas, t] {
            child->deleteLater();
            if (child->state() == Operation::State::Cancelled)
                return;
            if (child->state() == Operation::State::Succeeded && !child->result().images.isEmpty()) {
                GeneratedImage chosen = child->result().images.first();
                for (const GeneratedImage &g : child->result().images) {
                    if (t.outputs.contains(g.info.value(QStringLiteral("node")).toString())) {
                        chosen = g;
                        break;
                    }
                }
                const GeneratedImage out = segmentOutput(chosen, tag, t.outputKind, state->index, canvas);
                const QImage coverage = out.info.value(QStringLiteral("kind")).toString() == QLatin1String("mask")
                                            ? out.image
                                            : img::maskFromAlpha(out.image);
                if (!img::maskIsEmpty(coverage, 128))
                    state->result.images.append(out);
            } else if (child->state() == Operation::State::Failed) {
                state->failures << QStringLiteral("%1: %2").arg(tag, child->errorString());
            }
            ++state->index;
            if (state->next)
                state->next();
        });
        const ImageRequest req = segmentationRequest(image, prompt, params);
        self->runGraph(child, [t, req](const UploadedImages &images) { return applyTemplate(t, req, images); },
                       {{QStringLiteral("init"), image}});
    };
    op->onCancel([state, op] {
        state->next = nullptr;
        if (state->current)
            state->current->cancel();
        op->reportCancelled();
    });
    QTimer::singleShot(0, op, [state] {
        if (state->next)
            state->next();
    });
    return op;
}

} // namespace lumen::ai
