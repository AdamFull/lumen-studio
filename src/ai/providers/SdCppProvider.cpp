#include "ai/providers/SdCppProvider.h"

#include "ai/ModelDefaults.h"
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

QStringList stringsOf(const QJsonArray &a)
{
    QStringList out;
    for (const QJsonValue &v : a)
        out << (v.isObject() ? v.toObject().value(QStringLiteral("name")).toString() : v.toString());
    out.removeAll(QString());
    return out;
}

QString jobError(const QJsonObject &job, const QString &fallback)
{
    const QJsonValue e = job.value(QStringLiteral("error"));
    if (e.isObject()) {
        const QString message = e.toObject().value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
            return message;
    }
    if (e.isString() && !e.toString().isEmpty())
        return e.toString();
    return fallback;
}

} // namespace

ProviderType SdCppProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("sdcpp");
    t.name = QStringLiteral("stable-diffusion.cpp (sd-server)");
    t.description = tr("Local stable-diffusion.cpp server. Lumen can download and start it for you (Settings → Local servers). "
                       "Runs SD 1.5, SDXL (Illustrious, Pony…), FLUX and more without Python; pose control and character "
                       "references need a ControlNet and an IP-Adapter loaded with the server.");
    t.kind = ProviderKind::Image;
    t.local = true;
    t.documentation = QUrl(QStringLiteral("https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/server/api.md"));
    t.icon = QStringLiteral("server");
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Server URL"), F::Url, QStringLiteral("http://127.0.0.1:7861"), tr("Address of sd-server."), {}, 0, 0, true},
        {QStringLiteral("sampler"), tr("Sampler"), F::Model, QString(), tr("Empty = the server's default (e.g. euler_a).")},
        {QStringLiteral("scheduler"), tr("Scheduler"), F::Model, QString(), tr("Empty = the server's default.")},
        {QStringLiteral("native_resolution"), tr("Native resolution"), F::Choice, QString::fromLatin1(kAuto),
         tr("Working resolution of the loaded model. 'auto' assumes 1024 (SDXL) unless the model name says 1.5."),
         {QString::fromLatin1(kAuto), QStringLiteral("512"), QStringLiteral("768"), QStringLiteral("1024"), QStringLiteral("1280")}},
        {QStringLiteral("control_net"), tr("Server has a ControlNet (pose control)"), F::Boolean, false,
         tr("The server was started with --control-net. Set automatically for servers Lumen runs.")},
        {QStringLiteral("ip_adapter"), tr("Server has an IP-Adapter (character references)"), F::Boolean, false,
         tr("The server was started with --ip-adapter and --clip_vision. Set automatically for servers Lumen runs.")},
        {QStringLiteral("upscaler"), tr("Server has an upscaler"), F::Boolean, false,
         tr("The server was started with --hires-upscalers-dir. Set automatically for servers Lumen runs.")},
        {QStringLiteral("vae_tiling"), tr("VAE tiling"), F::Boolean, true,
         tr("Encodes and decodes images in tiles. Needed on 8 GB GPUs for SDXL with a ControlNet; slightly slower."), {}, 0, 0, false, true},
        {QStringLiteral("ip_adapter_pose_weight"), tr("IP-Adapter weight with pose control"), F::Number, 0.3,
         tr("The IP-Adapter also copies the reference's pose. With a pose map its weight is limited to this value."), {}, 0, 1, false, true},
        {QStringLiteral("preview"), tr("Live preview"), F::Choice, QStringLiteral("proj"),
         tr("How previews are made while sampling: 'proj' is fast and rough, 'tae'/'vae' need the matching model."),
         {QStringLiteral("none"), QStringLiteral("proj"), QStringLiteral("tae"), QStringLiteral("vae")}, 0, 0, false, true},
        {QStringLiteral("timeout_min"), tr("Timeout (minutes)"), F::Integer, 30, QString(), {}, 1, 240, false, true},
        {QStringLiteral("concurrency"), tr("Parallel jobs"), F::Integer, 1, tr("The server runs one generation at a time."), {}, 1, 8, false, true},
    };
    return t;
}

SdCppProvider::SdCppProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

QNetworkRequest SdCppProvider::request(const QString &path) const
{
    return net::HttpClient::makeRequest(endpoint(path));
}

QFuture<QJsonObject> SdCppProvider::serverCapabilities()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    opts.maxRetries = 0;
    return futureFromCall<QJsonObject>(http()->get(request(QStringLiteral("/sdcpp/v1/capabilities")), opts),
                                       [](net::HttpCall *call) { return call->json(); });
}

QFuture<QString> SdCppProvider::testConnection()
{
    return serverCapabilities().then([](const QJsonObject &caps) {
        const QString model = caps.value(QStringLiteral("model")).toObject().value(QStringLiteral("name")).toString();
        return model.isEmpty() ? tr("Connected") : tr("Connected - model: %1").arg(model);
    });
}

QFuture<QStringList> SdCppProvider::listModels()
{
    // One model per server process: switching means starting the server with another model.
    return serverCapabilities().then([](const QJsonObject &caps) {
        const QString model = caps.value(QStringLiteral("model")).toObject().value(QStringLiteral("name")).toString();
        return model.isEmpty() ? QStringList() : QStringList{model};
    });
}

// What every sd-server since 2024 knows, for when the server cannot be asked (a server Lumen runs is
// started only for a run, not to fill a list).
const QStringList kKnownSamplers{QStringLiteral("euler"),     QStringLiteral("euler_a"), QStringLiteral("heun"),
                                 QStringLiteral("dpm2"),      QStringLiteral("dpm++2s_a"), QStringLiteral("dpm++2m"),
                                 QStringLiteral("dpm++2mv2"), QStringLiteral("ipndm"),   QStringLiteral("ipndm_v"),
                                 QStringLiteral("lcm"),       QStringLiteral("ddim_trailing"), QStringLiteral("tcd")};
const QStringList kKnownSchedulers{QStringLiteral("discrete"), QStringLiteral("karras"), QStringLiteral("exponential"), QStringLiteral("ays"),
                                   QStringLiteral("gits")};

QFuture<QStringList> SdCppProvider::listSamplers()
{
    return serverCapabilities()
        .then([](const QJsonObject &caps) {
            const QStringList listed = stringsOf(caps.value(QStringLiteral("samplers")).toArray());
            return listed.isEmpty() ? kKnownSamplers : listed;
        })
        .onFailed([](const std::exception_ptr &) { return kKnownSamplers; });
}

QFuture<QStringList> SdCppProvider::listSchedulers()
{
    return serverCapabilities()
        .then([](const QJsonObject &caps) {
            const QStringList listed = stringsOf(caps.value(QStringLiteral("schedulers")).toArray());
            return listed.isEmpty() ? kKnownSchedulers : listed;
        })
        .onFailed([](const std::exception_ptr &) { return kKnownSchedulers; });
}

QFuture<QStringList> SdCppProvider::listOptions(const QString &settingKey)
{
    if (settingKey == QLatin1String("sampler"))
        return listSamplers();
    if (settingKey == QLatin1String("scheduler"))
        return listSchedulers();
    return Provider::listOptions(settingKey);
}

ImageCapabilities SdCppProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = true;
    c.imageToImage = true;
    c.inpaint = true;
    // What the model can do comes from its preset (servers Lumen runs; see ModelDefaults).
    const ModelDefaults d = modelDefaultsOf(this);
    c.edit = d.edit;
    c.regionEdit = d.edit;
    c.poseControl = setting(QStringLiteral("control_net")).toBool();
    // References: one for the IP-Adapter, else those the model takes itself (FLUX.2 ref_images).
    c.maxReferences = setting(QStringLiteral("ip_adapter")).toBool() ? 1 : d.references;
    c.seed = c.steps = c.cfg = c.samplers = true;
    c.negativePrompt = !d.isValid() || d.negativePrompt;
    c.batch = true;
    c.loras = true;
    c.sizeMultiple = 64;
    const QString res = text(QStringLiteral("native_resolution"));
    const int side = res != QLatin1String(kAuto) && res.toInt() > 0 ? res.toInt() : d.nativeResolution > 0 ? d.nativeResolution : 1024;
    c.nativePixels = qint64(side) * side;
    c.maxPixels = 2048 * 2048;
    return c;
}

ImageCapabilities SdCppProvider::capabilitiesFor(const ImageRequest &request) const
{
    ImageCapabilities c = capabilities();
    // An edit's source image is the first reference image.
    if (request.mode == GenMode::Edit && !setting(QStringLiteral("ip_adapter")).toBool())
        c.maxReferences = std::max(0, c.maxReferences - 1);
    return c;
}

QJsonObject SdCppProvider::buildPayload(const ImageRequest &r) const
{
    // The request's values, else the provider's settings, else the model preset's.
    const ModelDefaults d = modelDefaultsOf(this);
    QJsonObject guidance{{QStringLiteral("txt_cfg"), r.cfg}};
    const double distilled = r.guidance > 0 ? r.guidance : d.guidance;
    if (distilled > 0)
        guidance.insert(QStringLiteral("distilled_guidance"), distilled);
    const double imageCfg = r.imageCfg > 0 ? r.imageCfg : d.imageCfg;
    if (imageCfg > 0)
        guidance.insert(QStringLiteral("img_cfg"), imageCfg);
    QJsonObject sample{{QStringLiteral("sample_steps"), r.steps > 0 ? r.steps : d.steps > 0 ? d.steps : 20},
                       {QStringLiteral("guidance"), guidance}};
    const double flowShift = r.flowShift > 0 ? r.flowShift : d.flowShift;
    if (flowShift > 0)
        sample.insert(QStringLiteral("flow_shift"), flowShift);
    const QString sampler = !r.sampler.isEmpty() ? r.sampler : !text(QStringLiteral("sampler")).isEmpty() ? text(QStringLiteral("sampler")) : d.sampler;
    const QString scheduler = !r.scheduler.isEmpty() ? r.scheduler : !text(QStringLiteral("scheduler")).isEmpty() ? text(QStringLiteral("scheduler")) : d.scheduler;
    if (!sampler.isEmpty())
        sample.insert(QStringLiteral("sample_method"), sampler);
    if (!scheduler.isEmpty())
        sample.insert(QStringLiteral("scheduler"), scheduler);
    const QString preview = text(QStringLiteral("preview")).isEmpty() ? QStringLiteral("proj") : text(QStringLiteral("preview"));
    QJsonObject p{{QStringLiteral("prompt"), r.prompt},
                  {QStringLiteral("negative_prompt"), r.negativePrompt},
                  {QStringLiteral("width"), r.size.width()},
                  {QStringLiteral("height"), r.size.height()},
                  {QStringLiteral("seed"), double(r.seed)},
                  {QStringLiteral("batch_count"), std::max(1, r.batchCount)},
                  {QStringLiteral("clip_skip"), r.clipSkip > 0 ? r.clipSkip : -1},
                  {QStringLiteral("sample_params"), sample},
                  {QStringLiteral("preview"), preview},
                  {QStringLiteral("preview_interval"), 2},
                  {QStringLiteral("output_format"), QStringLiteral("png")},
                  {QStringLiteral("embed_image_metadata"), false}};
    if (setting(QStringLiteral("vae_tiling")).toBool()) {
        p.insert(QStringLiteral("vae_tiling_params"),
                 QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("tile_size_w"), 512}, {QStringLiteral("tile_size_h"), 512}});
    }
    // Reference images (FLUX.2, Kontext, Qwen-Image-Edit): an edit's source first, then the
    // request's references. Without an IP-Adapter, references guide a new image the same way.
    const bool ipAdapter = setting(QStringLiteral("ip_adapter")).toBool();
    if (r.mode == GenMode::Edit || (!ipAdapter && d.references > 0 && !r.references.isEmpty())) {
        QJsonArray refs;
        if (r.mode == GenMode::Edit)
            refs.append(toBase64Png(img::flatten(r.initImage, Qt::white).convertToFormat(QImage::Format_RGB888)));
        for (const ReferenceImage &ref : r.references) {
            if (refs.size() >= std::max(1, d.references))
                break;
            refs.append(toBase64Png(img::flatten(ref.image, Qt::white).convertToFormat(QImage::Format_RGB888)));
        }
        p.insert(QStringLiteral("ref_images"), refs);
    }
    if (r.mode == GenMode::ImageToImage || r.mode == GenMode::Inpaint) {
        p.insert(QStringLiteral("init_image"), toBase64Png(r.initImage));
        p.insert(QStringLiteral("strength"), r.denoise);
        if (r.mode == GenMode::Inpaint && !r.mask.isNull())
            p.insert(QStringLiteral("mask_image"), toBase64Png(img::thresholdMask(r.mask, 1)));
    }
    bool posed = false;
    if (setting(QStringLiteral("control_net")).toBool()) {
        for (const ControlInput &c : r.controls) {
            if (c.type != ControlType::OpenPose)
                continue;
            p.insert(QStringLiteral("control_image"), toBase64Png(c.image.convertToFormat(QImage::Format_RGB888)));
            p.insert(QStringLiteral("control_strength"), c.weight);
            posed = true;
            break; // one ControlNet per server
        }
    }
    if (ipAdapter && !r.references.isEmpty()) {
        // One reference image: prefer the character reference.
        const ReferenceImage *ref = &r.references.first();
        for (const ReferenceImage &candidate : r.references) {
            if (candidate.role == ReferenceRole::Character) {
                ref = &candidate;
                break;
            }
        }
        p.insert(QStringLiteral("ip_adapter_image"), toBase64Png(img::flatten(ref->image, Qt::white).convertToFormat(QImage::Format_RGB888)));
        // A strong IP-Adapter also copies the reference's pose and overrides the pose map.
        const QVariant cap = setting(QStringLiteral("ip_adapter_pose_weight"));
        const double limit = cap.isValid() ? cap.toDouble() : 0.3;
        p.insert(QStringLiteral("ip_adapter_strength"), posed ? std::min(ref->weight, limit) : ref->weight);
    }
    if (!r.loras.isEmpty()) {
        QJsonArray loras;
        for (const LoraRef &l : r.loras)
            loras.append(QJsonObject{{QStringLiteral("path"), l.resolved.isEmpty() ? l.name : l.resolved}, {QStringLiteral("multiplier"), l.weight}});
        p.insert(QStringLiteral("lora"), loras);
    }
    return json::merge(p, r.extras);
}

QFuture<QList<ServerLora>> SdCppProvider::availableLoras()
{
    return serverCapabilities().then([](const QJsonObject &caps) { return serverLoras(caps); });
}

QList<ServerLora> SdCppProvider::serverLoras(const QJsonObject &capabilities)
{
    QList<ServerLora> out;
    for (const QJsonValue &v : capabilities.value(QStringLiteral("loras")).toArray()) {
        const QJsonObject o = v.toObject();
        out.append({o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("path")).toString()});
    }
    return out;
}

ImageOperation *SdCppProvider::generate(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (r.mode == GenMode::Edit && !capabilities().edit) {
        failLater(op, tr("The model on this server does not edit images from an instruction (FLUX.2 [klein], FLUX.1 Kontext and "
                         "Qwen-Image-Edit do); use inpainting or a variation instead."));
        return op;
    }
    QPointer<SdCppProvider> self(this);
    auto cancelled = std::make_shared<bool>(false);
    auto jobId = std::make_shared<QString>();
    op->onCancel([self, op, cancelled, jobId] {
        *cancelled = true;
        if (!self || jobId->isEmpty()) {
            op->reportCancelled();
            return;
        }
        net::CallOptions o;
        o.maxRetries = 0;
        o.quiet = true;
        net::HttpCall *call = self->http()->post(self->request(QStringLiteral("/sdcpp/v1/jobs/%1/cancel").arg(*jobId)), "{}",
                                                 "application/json", o);
        // A queued job is cancelled right away; one that is already generating cannot be stopped
        // (409), so it finishes in the background while the operation ends now.
        QObject::connect(call, &net::HttpCall::finished, op, [op] { op->reportCancelled(); });
    });
    op->reportProgress(-1, tr("Sending…"));
    // LoRAs come from the server's LoRA folder, which it lists in its capabilities.
    QFuture<ImageRequest> prepared = r.loras.isEmpty() ? QtFuture::makeReadyValueFuture(r)
                                                       : serverCapabilities().then([r](const QJsonObject &caps) {
                                                             ImageRequest req = r;
                                                             QString error;
                                                             if (!resolveLoras(req.loras, serverLoras(caps), &error))
                                                                 throw ProviderError(error);
                                                             return req;
                                                         });
    prepared.then(QtFuture::Launch::Async, [self](const ImageRequest &req) -> QByteArray {
        if (!self)
            throw ProviderError(QStringLiteral("provider removed"));
        return json::compact(self->buildPayload(req));
    }).then(op, [self, op, r, cancelled, jobId](const QByteArray &body) {
        if (!self || op->isFinished())
            return;
        if (*cancelled) {
            op->reportCancelled();
            return;
        }
        net::CallOptions o;
        o.timeoutMs = 120000;
        o.maxRetries = 0; // not idempotent
        net::HttpCall *call = self->http()->postJsonBytes(self->request(QStringLiteral("/sdcpp/v1/img_gen")), body, o);
        QObject::connect(call, &net::HttpCall::finished, op, [self, op, call, r, cancelled, jobId] {
            if (!self || op->isFinished())
                return;
            if (!call->ok()) {
                op->fail(call->status() == 429 ? tr("The server's queue is full; try again when the running jobs are done.")
                                               : call->errorString());
                return;
            }
            *jobId = call->json().value(QStringLiteral("id")).toString();
            if (jobId->isEmpty()) {
                op->fail(tr("The server did not return a job id."));
                return;
            }
            if (*cancelled) {
                op->cancel(); // now that there is a job to cancel
                return;
            }
            self->poll(op, *jobId, r);
        });
    }).onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    return op;
}

void SdCppProvider::poll(ImageOperation *op, const QString &jobId, const ImageRequest &r)
{
    QPointer<SdCppProvider> self(this);
    auto timer = new QTimer(op);
    timer->setInterval(pollIntervalMs);
    auto busy = std::make_shared<bool>(false);
    auto lastPreview = std::make_shared<QString>();
    auto started = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch());
    const qint64 timeoutMs = qint64(std::max(1, setting(QStringLiteral("timeout_min")).toInt())) * 60 * 1000;
    auto tick = [self, op, jobId, r, timer, busy, lastPreview, started, timeoutMs] {
        if (!self || op->isFinished() || *busy)
            return;
        if (QDateTime::currentMSecsSinceEpoch() - *started > timeoutMs) {
            timer->stop();
            op->fail(tr("The generation did not finish within the timeout."));
            return;
        }
        *busy = true;
        net::CallOptions o;
        o.timeoutMs = 30000;
        o.maxRetries = 1;
        o.quiet = true;
        net::HttpCall *call = self->http()->get(self->request(QStringLiteral("/sdcpp/v1/jobs/%1").arg(jobId)), o);
        QObject::connect(call, &net::HttpCall::finished, op, [op, call, r, timer, busy, lastPreview] {
            *busy = false;
            if (op->isFinished())
                return;
            if (!call->ok()) {
                if (call->status() == 404 || call->status() == 410) {
                    timer->stop();
                    op->fail(tr("The server no longer knows this job (it was restarted?)."));
                }
                return; // transient: try again on the next tick
            }
            const QJsonObject job = call->json();
            const QString status = job.value(QStringLiteral("status")).toString();
            if (status == QLatin1String("queued")) {
                const int pos = job.value(QStringLiteral("queue_position")).toInt();
                op->reportProgress(-1, pos > 0 ? tr("Queued (position %1)").arg(pos) : tr("Queued"));
            } else if (status == QLatin1String("generating")) {
                const QJsonObject preview = job.value(QStringLiteral("preview")).toObject();
                const int step = preview.value(QStringLiteral("step")).toInt();
                const int total = preview.value(QStringLiteral("total_steps")).toInt();
                const int pass = preview.value(QStringLiteral("pass")).toInt(1);
                const int passes = std::max(1, r.batchCount);
                if (total > 0) {
                    const double fraction = (double(std::min(pass, passes) - 1) + double(step) / total) / passes;
                    op->reportProgress(std::clamp(fraction, 0.0, 1.0),
                                       passes > 1 ? tr("Image %1 of %2 · step %3 / %4").arg(pass).arg(passes).arg(step).arg(total)
                                                  : tr("Step %1 / %2").arg(step).arg(total));
                } else {
                    op->reportProgress(-1, tr("Generating…"));
                }
                const QString b64 = preview.value(QStringLiteral("b64_json")).toString();
                if (!b64.isEmpty() && b64.size() != lastPreview->size() && b64 != *lastPreview) {
                    *lastPreview = b64;
                    async::run([b64] { return decodeBase64Image(b64); }).then(op, [op](QImage img) { op->reportPreview(img); });
                }
            } else if (status == QLatin1String("completed")) {
                timer->stop();
                const QJsonArray images = job.value(QStringLiteral("result")).toObject().value(QStringLiteral("images")).toArray();
                op->reportProgress(1.0, tr("Decoding…"));
                async::run([images, r] {
                    ImageResult res;
                    for (const QJsonValue &v : images) {
                        const QJsonObject o = v.toObject();
                        GeneratedImage g;
                        g.image = decodeBase64Image(o.value(QStringLiteral("b64_json")).toString());
                        if (g.image.isNull())
                            continue;
                        const int index = o.value(QStringLiteral("index")).toInt(int(res.images.size()));
                        // sd.cpp uses seed + index for the images of a batch.
                        g.seed = r.seed >= 0 ? r.seed + index : -1;
                        res.images.append(g);
                    }
                    return res;
                }).then(op, [op](ImageResult res) {
                    if (res.images.isEmpty())
                        op->fail(tr("The server returned no images."));
                    else
                        op->succeed(std::move(res));
                });
            } else if (status == QLatin1String("cancelled")) {
                timer->stop();
                op->reportCancelled();
            } else if (status == QLatin1String("failed")) {
                timer->stop();
                op->fail(jobError(job, tr("The generation failed.")));
            }
        });
    };
    QObject::connect(timer, &QTimer::timeout, op, tick);
    timer->start();
    QTimer::singleShot(0, op, tick);
}

bool SdCppProvider::supportsUtility(Utility u) const
{
    return u == Utility::Upscale && setting(QStringLiteral("upscaler")).toBool();
}

ImageOperation *SdCppProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &params)
{
    auto *op = new ImageOperation(this);
    if (utility != Utility::Upscale) {
        failLater(op, tr("stable-diffusion.cpp offers no %1.").arg(utilityName(utility)));
        return op;
    }
    const double factor = std::clamp(params.value(QStringLiteral("factor")).toDouble(2.0), 1.0, 8.0);
    const QSize target(qRound(image.width() * factor), qRound(image.height() * factor));
    net::CallOptions opts;
    opts.timeoutMs = 10 * 60 * 1000;
    opts.maxRetries = 0;
    runImageRequest(
        op, http(), request(QStringLiteral("/sdcpp/v1/upscale")),
        [image] {
            // ESRGAN models are RGB: flatten transparency first.
            return Payload{json::compact(QJsonObject{{QStringLiteral("image"), toBase64Png(img::flatten(image, Qt::white))},
                                                     {QStringLiteral("output_format"), QStringLiteral("png")}})};
        },
        [target](const HttpResponse &resp) {
            const QJsonObject o = QJsonDocument::fromJson(resp.body).object();
            ImageResult r;
            GeneratedImage g;
            g.image = decodeBase64Image(o.value(QStringLiteral("images")).toArray().at(0).toObject().value(QStringLiteral("b64_json")).toString());
            // The model's own scale (usually 4×) is brought to the requested factor.
            if (!g.image.isNull() && g.image.size() != target)
                g.image = g.image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (!g.image.isNull())
                r.images.append(g);
            r.info.insert(QStringLiteral("upscaler"), o.value(QStringLiteral("upscaler")));
            return r;
        },
        opts);
    return op;
}

} // namespace lumen::ai
