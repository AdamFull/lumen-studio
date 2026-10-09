#include "ai/providers/SdWebUiProvider.h"

#include "ai/ProviderUtil.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JsonUtil.h"
#include "core/Log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>

namespace lumen::ai {

namespace {

constexpr auto kAuto = "auto";

bool isXlCheckpoint(const QString &name)
{
    const QString n = name.toLower();
    for (const char *marker : {"xl", "pony", "illustrious", "noob", "animagine", "playground", "juggernaut", "flux", "sd3"}) {
        if (n.contains(QLatin1String(marker)))
            return true;
    }
    return false;
}

bool isSd15Checkpoint(const QString &name)
{
    const QString n = name.toLower();
    return n.contains(QLatin1String("v1-5")) || n.contains(QLatin1String("sd15")) || n.contains(QLatin1String("sd_15"))
        || n.contains(QLatin1String("1.5"));
}

QJsonObject controlUnit(const QString &module, const QString &model, const QImage &image, double weight, double start,
                        double end, const QString &controlMode)
{
    return QJsonObject{{QStringLiteral("enabled"), true},
                       {QStringLiteral("module"), module},
                       {QStringLiteral("model"), model},
                       {QStringLiteral("image"), toBase64Png(image)},
                       {QStringLiteral("weight"), weight},
                       {QStringLiteral("guidance_start"), start},
                       {QStringLiteral("guidance_end"), end},
                       {QStringLiteral("resize_mode"), QStringLiteral("Crop and Resize")},
                       {QStringLiteral("control_mode"), controlMode},
                       {QStringLiteral("pixel_perfect"), true},
                       {QStringLiteral("processor_res"), 512}};
}

} // namespace

ProviderType SdWebUiProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("sdwebui");
    t.name = QStringLiteral("Stable Diffusion WebUI (A1111 / Forge)");
    t.description = tr("Local AUTOMATIC1111, Forge or reForge server started with --api. Pose control and character "
                       "references use the ControlNet extension (built into Forge) with OpenPose and IP-Adapter models.");
    t.kind = ProviderKind::Image;
    t.local = true;
    t.documentation = QUrl(QStringLiteral("https://github.com/AUTOMATIC1111/stable-diffusion-webui/wiki/API"));
    t.icon = QStringLiteral("server");
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Server URL"), F::Url, QStringLiteral("http://127.0.0.1:7860"), tr("Address of the WebUI (launched with --api)."), {}, 0, 0, true},
        {QStringLiteral("auth"), tr("API credentials"), F::Secret, QString(), tr("Only when the server uses --api-auth, as user:password."), {}, 0, 0, false, true},
        {QStringLiteral("model"), tr("Checkpoint"), F::Model, QString(), tr("Leave empty to use the checkpoint currently loaded in the WebUI.")},
        {QStringLiteral("native_resolution"), tr("Native resolution"), F::Choice, QString::fromLatin1(kAuto),
         tr("Working resolution of the model family. 'auto' guesses from the checkpoint name (SDXL/Pony/Illustrious = 1024)."),
         {QString::fromLatin1(kAuto), QStringLiteral("512"), QStringLiteral("768"), QStringLiteral("1024"), QStringLiteral("1280")}},
        {QStringLiteral("pose_model"), tr("OpenPose ControlNet model"), F::Model, QString::fromLatin1(kAuto), tr("ControlNet model used for pose control.")},
        {QStringLiteral("reference_mode"), tr("Character reference method"), F::Choice, QStringLiteral("ip-adapter"),
         tr("How reference images guide identity: IP-Adapter (recommended), ControlNet reference-only, or disabled."),
         {QStringLiteral("ip-adapter"), QStringLiteral("reference_only"), QStringLiteral("none")}},
        {QStringLiteral("ipadapter_model"), tr("IP-Adapter model"), F::Model, QString::fromLatin1(kAuto), QString()},
        {QStringLiteral("ipadapter_module"), tr("IP-Adapter preprocessor"), F::Text, QStringLiteral("ip-adapter-auto"),
         tr("e.g. ip-adapter-auto, ip-adapter_clip_sdxl_plus_vith, ip-adapter_face_id_plus"), {}, 0, 0, false, true},
        {QStringLiteral("cn_units"), tr("ControlNet units"), F::Integer, 3, tr("Maximum simultaneous ControlNet units configured in the WebUI settings."), {}, 1, 10, false, true},
        {QStringLiteral("upscaler"), tr("Upscaler"), F::Model, QStringLiteral("R-ESRGAN 4x+"), QString()},
        {QStringLiteral("rembg_model"), tr("Background removal model"), F::Choice, QStringLiteral("isnet-anime"),
         tr("Requires the stable-diffusion-webui-rembg extension."),
         {QStringLiteral("isnet-anime"), QStringLiteral("isnet-general-use"), QStringLiteral("u2net"), QStringLiteral("u2net_human_seg"), QStringLiteral("silueta")}},
        {QStringLiteral("timeout_min"), tr("Timeout (minutes)"), F::Integer, 30, QString(), {}, 1, 240, false, true},
        {QStringLiteral("concurrency"), tr("Parallel jobs"), F::Integer, 1, tr("Local GPUs usually process one job at a time."), {}, 1, 8, false, true},
    };
    return t;
}

SdWebUiProvider::SdWebUiProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

void SdWebUiProvider::onConfigured()
{
    m_cnModelsLoaded = false;
    m_cnModels.clear();
}

QNetworkRequest SdWebUiProvider::request(const QString &path) const
{
    QNetworkRequest req = net::HttpClient::makeRequest(endpoint(path));
    const QString auth = text(QStringLiteral("auth"));
    if (!auth.isEmpty())
        req.setRawHeader("Authorization", "Basic " + auth.toUtf8().toBase64());
    return req;
}

QFuture<QString> SdWebUiProvider::testConnection()
{
    net::CallOptions opts;
    opts.timeoutMs = 8000;
    opts.maxRetries = 0;
    return futureFromCall<QString>(http()->get(request(QStringLiteral("/sdapi/v1/options")), opts), [](net::HttpCall *call) {
        const QJsonObject o = call->json();
        const QString model = o.value(QStringLiteral("sd_model_checkpoint")).toString();
        return model.isEmpty() ? tr("Connected") : tr("Connected - current checkpoint: %1").arg(model);
    });
}

QFuture<QStringList> SdWebUiProvider::listModels()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/sdapi/v1/sd-models")), opts), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : QJsonDocument::fromJson(call->body()).array())
            out << v.toObject().value(QStringLiteral("title")).toString();
        return out;
    });
}

QFuture<QStringList> SdWebUiProvider::listSamplers()
{
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/sdapi/v1/samplers"))), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : QJsonDocument::fromJson(call->body()).array())
            out << v.toObject().value(QStringLiteral("name")).toString();
        return out;
    });
}

QFuture<QStringList> SdWebUiProvider::listSchedulers()
{
    net::CallOptions opts;
    opts.maxRetries = 0;
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/sdapi/v1/schedulers")), opts), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : QJsonDocument::fromJson(call->body()).array())
            out << v.toObject().value(QStringLiteral("label")).toString();
        return out;
    });
}

QFuture<QStringList> SdWebUiProvider::listOptions(const QString &settingKey)
{
    if (settingKey == QLatin1String("pose_model") || settingKey == QLatin1String("ipadapter_model")) {
        return controlNetModels().then(this, [](QStringList models) {
            models.prepend(QString::fromLatin1(kAuto));
            return models;
        });
    }
    if (settingKey == QLatin1String("upscaler")) {
        return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/sdapi/v1/upscalers"))), [](net::HttpCall *call) {
            QStringList out;
            for (const QJsonValue &v : QJsonDocument::fromJson(call->body()).array())
                out << v.toObject().value(QStringLiteral("name")).toString();
            return out;
        });
    }
    return Provider::listOptions(settingKey);
}

QFuture<QStringList> SdWebUiProvider::controlNetModels()
{
    if (m_cnModelsLoaded)
        return QtFuture::makeReadyValueFuture(m_cnModels);
    net::CallOptions opts;
    opts.timeoutMs = 10000;
    opts.maxRetries = 0;
    QPointer<SdWebUiProvider> self(this);
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/controlnet/model_list")), opts), [self](net::HttpCall *call) {
        QStringList out = json::toStringList(call->json().value(QStringLiteral("model_list")).toArray());
        if (self) {
            self->m_cnModels = out;
            self->m_cnModelsLoaded = true;
        }
        return out;
    });
}

QString SdWebUiProvider::pickControlNetModel(const QStringList &models, const QString &kind, const QString &checkpoint)
{
    const bool xl = isXlCheckpoint(checkpoint) || !isSd15Checkpoint(checkpoint);
    auto familyMatches = [xl](const QString &m) {
        const QString n = m.toLower();
        const bool modelXl = n.contains(QLatin1String("xl")) || n.contains(QLatin1String("union"));
        return xl ? modelXl : !n.contains(QLatin1String("xl"));
    };
    QStringList keys;
    if (kind == QLatin1String("pose"))
        keys = {QStringLiteral("openpose"), QStringLiteral("pose"), QStringLiteral("union")};
    else
        keys = {QStringLiteral("ip-adapter-plus"), QStringLiteral("ip-adapter_plus"), QStringLiteral("ip-adapter"), QStringLiteral("ip_adapter")};
    for (const QString &key : keys) {
        for (const QString &m : models) {
            if (m.toLower().contains(key) && familyMatches(m) && !m.toLower().contains(QLatin1String("faceid")))
                return m;
        }
    }
    for (const QString &key : keys) {
        for (const QString &m : models) {
            if (m.toLower().contains(key))
                return m;
        }
    }
    return {};
}

QString SdWebUiProvider::checkpointName(const ImageRequest &request) const
{
    return request.model.isEmpty() ? text(QStringLiteral("model")) : request.model;
}

qint64 SdWebUiProvider::nativePixels() const
{
    const QString setting = text(QStringLiteral("native_resolution"));
    if (setting != QLatin1String(kAuto) && setting.toInt() > 0)
        return qint64(setting.toInt()) * setting.toInt();
    const QString ckpt = text(QStringLiteral("model"));
    if (!ckpt.isEmpty() && isSd15Checkpoint(ckpt) && !isXlCheckpoint(ckpt))
        return 512 * 512;
    return 1024 * 1024;
}

ImageCapabilities SdWebUiProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = true;
    c.imageToImage = true;
    c.inpaint = true;
    c.edit = false;
    const int units = std::max(1, setting(QStringLiteral("cn_units")).toInt());
    c.maxReferences = text(QStringLiteral("reference_mode")) == QLatin1String("none") ? 0 : std::max(0, units - 1);
    c.poseControl = true;
    c.negativePrompt = c.seed = c.steps = c.cfg = c.samplers = true;
    c.batch = true;
    c.sizeMultiple = 8;
    c.nativePixels = nativePixels();
    c.maxPixels = 2048 * 2048;
    return c;
}

QJsonObject SdWebUiProvider::buildPayload(const ImageRequest &r, const QStringList &cnModels) const
{
    QJsonObject p{{QStringLiteral("prompt"), r.prompt},
                  {QStringLiteral("negative_prompt"), r.negativePrompt},
                  {QStringLiteral("seed"), double(r.seed)},
                  {QStringLiteral("steps"), r.steps},
                  {QStringLiteral("cfg_scale"), r.cfg},
                  {QStringLiteral("width"), r.size.width()},
                  {QStringLiteral("height"), r.size.height()},
                  {QStringLiteral("batch_size"), std::max(1, r.batchCount)},
                  {QStringLiteral("n_iter"), 1},
                  {QStringLiteral("send_images"), true},
                  {QStringLiteral("save_images"), false}};
    if (!r.sampler.isEmpty())
        p.insert(QStringLiteral("sampler_name"), r.sampler);
    if (!r.scheduler.isEmpty())
        p.insert(QStringLiteral("scheduler"), r.scheduler);
    const QString ckpt = checkpointName(r);
    if (!ckpt.isEmpty()) {
        p.insert(QStringLiteral("override_settings"), QJsonObject{{QStringLiteral("sd_model_checkpoint"), ckpt}});
        p.insert(QStringLiteral("override_settings_restore_afterwards"), false);
    }
    if (r.mode != GenMode::TextToImage) {
        p.insert(QStringLiteral("init_images"), QJsonArray{toBase64Png(r.initImage)});
        p.insert(QStringLiteral("denoising_strength"), r.denoise);
        p.insert(QStringLiteral("resize_mode"), 0);
        if (r.mode == GenMode::Inpaint && !r.mask.isNull()) {
            p.insert(QStringLiteral("mask"), toBase64Png(img::thresholdMask(r.mask, 1)));
            p.insert(QStringLiteral("mask_blur"), r.maskBlur);
            p.insert(QStringLiteral("inpainting_fill"), 1);         // keep original content under the mask
            p.insert(QStringLiteral("inpaint_full_res"), false);    // Lumen already crops the masked region
            p.insert(QStringLiteral("inpainting_mask_invert"), 0);
        }
    }

    QJsonArray units;
    const int maxUnits = std::max(1, setting(QStringLiteral("cn_units")).toInt());
    for (const ControlInput &c : r.controls) {
        if (units.size() >= maxUnits)
            break;
        if (c.type != ControlType::OpenPose)
            continue;
        QString model = text(QStringLiteral("pose_model"));
        if (model.isEmpty() || model == QLatin1String(kAuto))
            model = pickControlNetModel(cnModels, QStringLiteral("pose"), ckpt);
        if (model.isEmpty())
            throw ProviderError(tr("Pose control needs an OpenPose ControlNet model, but none was found on the server. "
                                   "Install one (e.g. controlnet-openpose-sdxl or xinsir union) or pick it in the provider settings."));
        units.append(controlUnit(QStringLiteral("none"), model, c.image, c.weight, c.start, c.end, QStringLiteral("Balanced")));
    }
    const QString refMode = text(QStringLiteral("reference_mode"));
    if (refMode != QLatin1String("none")) {
        QString ipModel = text(QStringLiteral("ipadapter_model"));
        if (refMode == QLatin1String("ip-adapter") && (ipModel.isEmpty() || ipModel == QLatin1String(kAuto)))
            ipModel = pickControlNetModel(cnModels, QStringLiteral("ipadapter"), ckpt);
        for (const ReferenceImage &ref : r.references) {
            if (units.size() >= maxUnits)
                break;
            if (refMode == QLatin1String("ip-adapter") && !ipModel.isEmpty()) {
                units.append(controlUnit(text(QStringLiteral("ipadapter_module")), ipModel, ref.image, ref.weight, 0.0, 1.0,
                                         QStringLiteral("Balanced")));
            } else {
                // reference_only needs no model and works with any checkpoint.
                units.append(controlUnit(QStringLiteral("reference_only"), QStringLiteral("None"), ref.image, ref.weight, 0.0, 1.0,
                                         QStringLiteral("Balanced")));
            }
        }
    }
    if (!units.isEmpty()) {
        p.insert(QStringLiteral("alwayson_scripts"),
                 QJsonObject{{QStringLiteral("controlnet"), QJsonObject{{QStringLiteral("args"), units}}}});
    }
    return json::merge(p, r.extras);
}

ImageResult SdWebUiProvider::parseResponse(const QByteArray &body, int expectedImages)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QJsonArray images = o.value(QStringLiteral("images")).toArray();
    const QJsonObject info = QJsonDocument::fromJson(o.value(QStringLiteral("info")).toString().toUtf8()).object();
    const QJsonArray seeds = info.value(QStringLiteral("all_seeds")).toArray();
    const QJsonArray infotexts = info.value(QStringLiteral("infotexts")).toArray();
    ImageResult result;
    result.info = info;
    // ControlNet may append its detected maps after the generated images; keep only the requested count.
    const qsizetype count = expectedImages > 0 ? std::min<qsizetype>(images.size(), expectedImages) : images.size();
    for (qsizetype i = 0; i < count; ++i) {
        GeneratedImage g;
        g.image = decodeBase64Image(images.at(i).toString());
        if (g.image.isNull())
            continue;
        g.seed = qint64(seeds.at(i).toDouble(info.value(QStringLiteral("seed")).toDouble(-1)));
        g.parameters = infotexts.at(i).toString();
        result.images.append(g);
    }
    return result;
}

ImageOperation *SdWebUiProvider::generate(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (r.mode == GenMode::Edit) {
        failLater(op, tr("Stable Diffusion WebUI does not support instruction edits; use inpainting or img2img."));
        return op;
    }
    const bool needsCn = !r.controls.isEmpty() || (!r.references.isEmpty() && text(QStringLiteral("reference_mode")) == QLatin1String("ip-adapter"));
    QFuture<QStringList> models = needsCn ? controlNetModels() : QtFuture::makeReadyValueFuture(QStringList());
    QPointer<SdWebUiProvider> self(this);
    const QString path = r.mode == GenMode::TextToImage ? QStringLiteral("/sdapi/v1/txt2img") : QStringLiteral("/sdapi/v1/img2img");
    const int expected = std::max(1, r.batchCount);
    const int timeoutMin = std::max(1, setting(QStringLiteral("timeout_min")).toInt());

    models
        .then(op, [self, op, r, path, expected, timeoutMin](QStringList cnModels) {
            if (!self)
                return;
            net::CallOptions opts;
            opts.timeoutMs = timeoutMin * 60 * 1000;
            opts.maxRetries = 0; // generation is not idempotent
            auto pollTimer = new QTimer(op);
            auto polling = std::make_shared<bool>(false);
            const QNetworkRequest req = self->request(path);
            runImageRequest(
                op, self->http(), req,
                [self, r, cnModels]() -> Payload {
                    if (!self)
                        throw ProviderError(QStringLiteral("provider removed"));
                    return Payload{json::compact(self->buildPayload(r, cnModels))};
                },
                [expected](const HttpResponse &resp) { return parseResponse(resp.body, expected); }, opts,
                [self, op, pollTimer, polling](net::HttpCall *) {
                    // Poll progress + live preview while the blocking generate call runs.
                    pollTimer->setInterval(800);
                    QObject::connect(pollTimer, &QTimer::timeout, op, [self, op, polling] {
                        if (!self || *polling)
                            return;
                        *polling = true;
                        net::CallOptions po;
                        po.timeoutMs = 5000;
                        po.maxRetries = 0;
                        po.quiet = true;
                        net::HttpCall *poll = self->http()->get(self->request(QStringLiteral("/sdapi/v1/progress?skip_current_image=false")), po);
                        QObject::connect(poll, &net::HttpCall::finished, op, [op, poll, polling] {
                            *polling = false;
                            if (!poll->ok())
                                return;
                            const QJsonObject o = poll->json();
                            const QJsonObject st = o.value(QStringLiteral("state")).toObject();
                            const int step = st.value(QStringLiteral("sampling_step")).toInt();
                            const int steps = st.value(QStringLiteral("sampling_steps")).toInt();
                            const double progress = o.value(QStringLiteral("progress")).toDouble();
                            if (progress > 0.0)
                                op->reportProgress(progress, steps > 0 ? QObject::tr("Step %1 / %2").arg(step).arg(steps) : QString());
                            const QString current = o.value(QStringLiteral("current_image")).toString();
                            if (!current.isEmpty()) {
                                async::run([current] { return decodeBase64Image(current); }).then(op, [op](QImage preview) {
                                    op->reportPreview(preview);
                                });
                            }
                        });
                    });
                    pollTimer->start();
                },
                [self] {
                    if (self) {
                        net::CallOptions io;
                        io.maxRetries = 0;
                        self->http()->post(self->request(QStringLiteral("/sdapi/v1/interrupt")), QByteArray(), "application/json", io);
                    }
                });
            QObject::connect(op, &Operation::finished, pollTimer, &QTimer::stop);
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    return op;
}

bool SdWebUiProvider::supportsUtility(Utility u) const
{
    return u == Utility::Upscale || u == Utility::RemoveBackground || u == Utility::DetectPose || u == Utility::Caption;
}

ImageOperation *SdWebUiProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &params)
{
    auto *op = new ImageOperation(this);
    net::CallOptions opts;
    opts.timeoutMs = 10 * 60 * 1000;
    opts.maxRetries = 0;
    switch (utility) {
    case Utility::Upscale: {
        const double factor = params.value(QStringLiteral("factor")).toDouble(2.0);
        const QString upscaler = params.value(QStringLiteral("upscaler")).toString(text(QStringLiteral("upscaler")));
        runImageRequest(op, http(), request(QStringLiteral("/sdapi/v1/extra-single-image")),
                        [image, factor, upscaler] {
                            return Payload{json::compact(QJsonObject{{QStringLiteral("image"), toBase64Png(image)},
                                                                     {QStringLiteral("resize_mode"), 0},
                                                                     {QStringLiteral("upscaling_resize"), factor},
                                                                     {QStringLiteral("upscaler_1"), upscaler}})};
                        },
                        [](const HttpResponse &resp) {
                            ImageResult r;
                            GeneratedImage g;
                            g.image = decodeBase64Image(QJsonDocument::fromJson(resp.body).object().value(QStringLiteral("image")).toString());
                            if (!g.image.isNull())
                                r.images.append(g);
                            return r;
                        },
                        opts);
        break;
    }
    case Utility::RemoveBackground: {
        const QString model = params.value(QStringLiteral("model")).toString(text(QStringLiteral("rembg_model")));
        runImageRequest(op, http(), request(QStringLiteral("/rembg")),
                        [image, model] {
                            return Payload{json::compact(QJsonObject{{QStringLiteral("input_image"), toBase64Png(image)},
                                                                     {QStringLiteral("model"), model},
                                                                     {QStringLiteral("return_mask"), false},
                                                                     {QStringLiteral("alpha_matting"), false}})};
                        },
                        [](const HttpResponse &resp) {
                            ImageResult r;
                            GeneratedImage g;
                            g.image = decodeBase64Image(QJsonDocument::fromJson(resp.body).object().value(QStringLiteral("image")).toString());
                            if (!g.image.isNull())
                                r.images.append(g);
                            return r;
                        },
                        opts);
        break;
    }
    case Utility::DetectPose: {
        const QString module = params.value(QStringLiteral("module")).toString(QStringLiteral("dw_openpose_full"));
        runImageRequest(op, http(), request(QStringLiteral("/controlnet/detect")),
                        [image, module] {
                            const int res = std::clamp(std::min(image.width(), image.height()), 512, 1024);
                            return Payload{json::compact(QJsonObject{{QStringLiteral("controlnet_module"), module},
                                                                     {QStringLiteral("controlnet_input_images"), QJsonArray{toBase64Png(image)}},
                                                                     {QStringLiteral("controlnet_processor_res"), res}})};
                        },
                        [](const HttpResponse &resp) {
                            const QJsonObject o = QJsonDocument::fromJson(resp.body).object();
                            ImageResult r;
                            GeneratedImage g;
                            g.image = decodeBase64Image(o.value(QStringLiteral("images")).toArray().at(0).toString());
                            if (!g.image.isNull())
                                r.images.append(g);
                            // Newer ControlNet versions return OpenPose JSON for openpose modules.
                            const QJsonValue poses = o.value(QStringLiteral("poses"));
                            if (poses.isArray() && !poses.toArray().isEmpty())
                                r.info.insert(QStringLiteral("openpose"), poses.toArray().at(0));
                            return r;
                        },
                        opts);
        break;
    }
    case Utility::Caption: {
        const QString model = params.value(QStringLiteral("model")).toString(QStringLiteral("clip"));
        runImageRequest(op, http(), request(QStringLiteral("/sdapi/v1/interrogate")),
                        [image, model] {
                            return Payload{json::compact(QJsonObject{{QStringLiteral("image"), toBase64Png(image)},
                                                                     {QStringLiteral("model"), model}})};
                        },
                        [](const HttpResponse &resp) {
                            ImageResult r;
                            r.text = QJsonDocument::fromJson(resp.body).object().value(QStringLiteral("caption")).toString();
                            return r;
                        },
                        opts);
        break;
    }
    }
    return op;
}

} // namespace lumen::ai
