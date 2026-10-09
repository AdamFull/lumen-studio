#include "ai/providers/FalProvider.h"

#include "ai/ProviderUtil.h"
#include "core/Async.h"
#include "core/ImageOps.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

namespace lumen::ai {

namespace {

const QStringList kModels = {QStringLiteral("fal-ai/flux/dev"), QStringLiteral("fal-ai/flux/schnell"), QStringLiteral("fal-ai/flux-pro/v1.1"),
                             QStringLiteral("fal-ai/flux-pro/v1.1-ultra"), QStringLiteral("fal-ai/recraft-v3")};
const QStringList kAspects = {QStringLiteral("21:9"), QStringLiteral("16:9"), QStringLiteral("4:3"), QStringLiteral("3:2"), QStringLiteral("1:1"),
                              QStringLiteral("2:3"), QStringLiteral("3:4"), QStringLiteral("9:16"), QStringLiteral("9:21")};

QString dataUrl(const QImage &image, int maxSide = 2048)
{
    return img::toDataUrl(img::thumbnail(image, maxSide), "PNG", 1);
}

QString maskDataUrl(const QImage &mask, const QSize &size)
{
    // White = area to generate, the same convention as Lumen masks.
    return img::toDataUrl(img::scaled(mask, size, true).convertToFormat(QImage::Format_Grayscale8), "PNG", 1);
}

} // namespace

ProviderType FalProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("fal");
    t.name = QStringLiteral("fal.ai");
    t.description = tr("FLUX and other hosted models on fal.ai: text-to-image, image-to-image, inpainting (FLUX Fill), instruction "
                       "edits (FLUX Kontext), background removal and upscaling.");
    t.kind = ProviderKind::Image;
    t.icon = QStringLiteral("cloud");
    t.documentation = QUrl(QStringLiteral("https://fal.ai/docs/model-endpoints/queue"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Queue URL"), F::Url, QStringLiteral("https://queue.fal.run"), QString(), {}, 0, 0, true, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), tr("Create a key at fal.ai/dashboard/keys."), {}, 0, 0, true},
        {QStringLiteral("model"), tr("Text-to-image model"), F::Model, kModels.first(), tr("Any fal.ai endpoint id."), kModels},
        {QStringLiteral("img2img_model"), tr("Image-to-image model"), F::Model, QStringLiteral("fal-ai/flux/dev/image-to-image"), QString(),
         {QStringLiteral("fal-ai/flux/dev/image-to-image")}, 0, 0, false, true},
        {QStringLiteral("inpaint_model"), tr("Inpainting model"), F::Model, QStringLiteral("fal-ai/flux-pro/v1/fill"), QString(),
         {QStringLiteral("fal-ai/flux-pro/v1/fill"), QStringLiteral("fal-ai/flux-lora/inpainting")}, 0, 0, false, true},
        {QStringLiteral("edit_model"), tr("Instruction edit model"), F::Model, QStringLiteral("fal-ai/flux-pro/kontext"), QString(),
         {QStringLiteral("fal-ai/flux-pro/kontext"), QStringLiteral("fal-ai/flux-pro/kontext/max")}, 0, 0, false, true},
        {QStringLiteral("timeout_min"), tr("Timeout (minutes)"), F::Integer, 10, QString(), {}, 1, 120, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, 3, QString(), {}, 1, 16, false, true},
    };
    return t;
}

FalProvider::FalProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

QNetworkRequest FalProvider::request(const QUrl &url) const
{
    return net::HttpClient::makeRequest(url, {{"Authorization", "Key " + text(QStringLiteral("api_key")).toUtf8()},
                                              {"Accept", "application/json"}});
}

QFuture<QString> FalProvider::testConnection()
{
    if (text(QStringLiteral("api_key")).isEmpty())
        return QtFuture::makeExceptionalFuture<QString>(std::make_exception_ptr(ProviderError(tr("The fal.ai API key is not set."))));
    // An unknown request id is a cheap authenticated call: 404 means the key was accepted, 401/403 that it was not.
    const QUrl url = endpoint(QStringLiteral("/") + text(QStringLiteral("model")) + QStringLiteral("/requests/00000000-0000-0000-0000-000000000000/status"));
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    opts.maxRetries = 0;
    net::HttpCall *call = http()->get(request(url), opts);
    QPromise<QString> promise;
    QFuture<QString> future = promise.future();
    promise.start();
    QObject::connect(call, &net::HttpCall::finished, call, [call, p = std::move(promise)]() mutable {
        if (call->status() == 401 || call->status() == 403)
            p.setException(std::make_exception_ptr(ProviderError(tr("fal.ai rejected the API key."))));
        else if (call->status() == 0)
            p.setException(std::make_exception_ptr(ProviderError(call->errorString())));
        else
            p.addResult(tr("Connected to fal.ai"));
        p.finish();
    });
    return future;
}

QFuture<QStringList> FalProvider::listModels()
{
    return QtFuture::makeReadyValueFuture(kModels);
}

ImageCapabilities FalProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = true;
    c.imageToImage = true;
    c.inpaint = true;
    c.edit = true;
    c.maxReferences = 0;
    c.poseControl = false;
    c.negativePrompt = false;
    c.seed = true;
    c.steps = true;
    c.cfg = true;
    c.batch = true;
    c.sizeMultiple = 16;
    c.nativePixels = 1024 * 1024;
    c.maxPixels = 2048 * 2048;
    if (text(QStringLiteral("model")).contains(QLatin1String("ultra")))
        c.aspectRatios = kAspects;
    return c;
}

FalProvider::Call FalProvider::buildCall(const ImageRequest &r) const
{
    Call call;
    QJsonObject &in = call.input;
    in.insert(QStringLiteral("prompt"), r.prompt);
    if (r.seed >= 0)
        in.insert(QStringLiteral("seed"), double(r.seed % 4294967295LL));
    in.insert(QStringLiteral("num_images"), std::clamp(r.batchCount, 1, 4));
    in.insert(QStringLiteral("output_format"), QStringLiteral("png"));
    switch (r.mode) {
    case GenMode::Inpaint: {
        call.model = text(QStringLiteral("inpaint_model"));
        const QImage init = img::thumbnail(r.initImage, 2048);
        in.insert(QStringLiteral("image_url"), dataUrl(init));
        in.insert(QStringLiteral("mask_url"), maskDataUrl(r.mask, init.size()));
        if (call.model.contains(QLatin1String("lora")))
            in.insert(QStringLiteral("strength"), std::clamp(r.denoise, 0.0, 1.0));
        break;
    }
    case GenMode::Edit:
        call.model = text(QStringLiteral("edit_model"));
        in.insert(QStringLiteral("image_url"), dataUrl(r.initImage));
        in.insert(QStringLiteral("guidance_scale"), r.cfg);
        break;
    case GenMode::ImageToImage:
        call.model = text(QStringLiteral("img2img_model"));
        in.insert(QStringLiteral("image_url"), dataUrl(r.initImage));
        in.insert(QStringLiteral("strength"), std::clamp(r.denoise, 0.0, 1.0));
        in.insert(QStringLiteral("num_inference_steps"), r.steps);
        in.insert(QStringLiteral("guidance_scale"), r.cfg);
        break;
    default: {
        call.model = r.model.isEmpty() ? text(QStringLiteral("model")) : r.model;
        if (call.model.contains(QLatin1String("ultra"))) {
            in.insert(QStringLiteral("aspect_ratio"), closestAspectRatio(r.size, kAspects));
        } else {
            in.insert(QStringLiteral("image_size"), QJsonObject{{QStringLiteral("width"), r.size.width()}, {QStringLiteral("height"), r.size.height()}});
            if (!call.model.contains(QLatin1String("pro"))) {
                in.insert(QStringLiteral("num_inference_steps"), call.model.contains(QLatin1String("schnell")) ? std::min(r.steps, 4) : r.steps);
                in.insert(QStringLiteral("guidance_scale"), r.cfg);
            }
        }
        break;
    }
    }
    if (call.model.startsWith(QLatin1String("fal-ai/flux")))
        in.insert(QStringLiteral("enable_safety_checker"), true);
    for (auto it = r.extras.constBegin(); it != r.extras.constEnd(); ++it)
        in.insert(it.key(), it.value());
    return call;
}

QStringList FalProvider::imageUrls(const QJsonObject &output)
{
    QStringList urls;
    for (const QJsonValue &v : output.value(QStringLiteral("images")).toArray()) {
        const QString url = v.isString() ? v.toString() : v.toObject().value(QStringLiteral("url")).toString();
        if (!url.isEmpty())
            urls << url;
    }
    const QString single = output.value(QStringLiteral("image")).toObject().value(QStringLiteral("url")).toString();
    if (!single.isEmpty())
        urls << single;
    return urls;
}

void FalProvider::runCall(ImageOperation *op, std::function<Call()> build)
{
    struct Run
    {
        QList<QPointer<net::HttpCall>> calls;
        QPointer<QTimer> poll;
        QUrl statusUrl, responseUrl, cancelUrl;
        QList<GeneratedImage> images;
        int pending = 0;
        bool cancelled = false;
    };
    auto run = std::make_shared<Run>();
    QPointer<FalProvider> self(this);
    const int timeoutMin = std::max(1, setting(QStringLiteral("timeout_min")).toInt());

    op->onCancel([self, run, op] {
        run->cancelled = true;
        if (run->poll)
            run->poll->stop();
        for (const auto &c : run->calls) {
            if (c)
                c->abort();
        }
        if (self && run->cancelUrl.isValid()) {
            const QNetworkRequest req = self->request(run->cancelUrl);
            net::CallOptions o;
            o.maxRetries = 0;
            o.quiet = true;
            self->http()->custom([req](QNetworkAccessManager &nam) { return nam.put(req, QByteArray()); }, o);
        }
        op->reportCancelled();
    });

    auto finishWith = [op, run](QJsonObject output) {
        ImageResult result;
        result.images = run->images;
        result.info = output;
        if (result.images.isEmpty()) {
            op->fail(output.value(QStringLiteral("has_nsfw_concepts")).toArray().contains(true)
                         ? tr("fal.ai filtered the result (safety checker).")
                         : tr("fal.ai returned no images."));
            return;
        }
        op->succeed(std::move(result));
    };

    // Step 3: the result JSON, then each image (data URIs inline, URLs downloaded).
    auto fetchResult = [self, op, run, finishWith] {
        net::CallOptions o;
        o.timeoutMs = 60000;
        net::HttpCall *call = self->http()->get(self->request(run->responseUrl), o);
        run->calls << call;
        QObject::connect(call, &net::HttpCall::finished, op, [self, op, run, call, finishWith] {
            if (run->cancelled || op->isFinished())
                return;
            if (!call->ok()) {
                op->fail(call->errorString());
                return;
            }
            const QJsonObject output = call->json();
            const QStringList urls = imageUrls(output);
            const qint64 seed = qint64(output.value(QStringLiteral("seed")).toDouble(-1));
            run->images.resize(urls.size());
            run->pending = int(urls.size());
            if (urls.isEmpty()) {
                finishWith(output);
                return;
            }
            op->reportProgress(0.95, tr("Downloading"));
            for (int i = 0; i < urls.size(); ++i) {
                auto store = [op, run, i, seed, output, finishWith](QImage image) {
                    if (run->cancelled || op->isFinished())
                        return;
                    run->images[i].image = img::normalized(image);
                    run->images[i].seed = seed >= 0 ? seed + i : -1;
                    if (--run->pending == 0) {
                        run->images.removeIf([](const GeneratedImage &g) { return g.image.isNull(); });
                        finishWith(output);
                    }
                };
                if (urls[i].startsWith(QLatin1String("data:"))) {
                    async::run([url = urls[i]] { return img::fromBase64(url.toLatin1()); }).then(op, store);
                    continue;
                }
                net::CallOptions d;
                d.timeoutMs = 120000;
                net::HttpCall *download = self->http()->get(net::HttpClient::makeRequest(QUrl(urls[i])), d);
                run->calls << download;
                QObject::connect(download, &net::HttpCall::finished, op, [op, run, download, store] {
                    if (run->cancelled || op->isFinished())
                        return;
                    if (!download->ok()) {
                        op->fail(tr("Downloading the fal.ai result failed: %1").arg(download->errorString()));
                        return;
                    }
                    async::run([bytes = download->body()] { return decodeImageBytes(bytes); }).then(op, store);
                });
            }
        });
    };

    // Step 2: poll the status until COMPLETED.
    auto startPolling = [self, op, run, fetchResult, timeoutMin] {
        auto *timer = new QTimer(op);
        run->poll = timer;
        auto started = std::make_shared<QElapsedTimer>();
        started->start();
        auto busy = std::make_shared<bool>(false);
        QObject::connect(timer, &QTimer::timeout, op, [self, op, run, fetchResult, started, busy, timeoutMin, timer] {
            if (!self || run->cancelled || op->isFinished() || *busy)
                return;
            if (started->elapsed() > qint64(timeoutMin) * 60000) {
                timer->stop();
                op->fail(tr("fal.ai did not finish within %1 minutes").arg(timeoutMin));
                return;
            }
            *busy = true;
            net::CallOptions po;
            po.timeoutMs = 15000;
            po.maxRetries = 1;
            po.quiet = true;
            net::HttpCall *h = self->http()->get(self->request(run->statusUrl), po);
            run->calls << h;
            QObject::connect(h, &net::HttpCall::finished, op, [op, run, h, busy, fetchResult, timer] {
                *busy = false;
                if (run->cancelled || op->isFinished())
                    return;
                if (!h->ok()) {
                    // A failed request answers its status (and result) with an error.
                    if (h->status() >= 400 && h->status() != 429) {
                        timer->stop();
                        op->fail(h->errorString());
                    }
                    return;
                }
                const QJsonObject s = h->json();
                const QString status = s.value(QStringLiteral("status")).toString();
                if (status == QLatin1String("IN_QUEUE")) {
                    const int pos = s.value(QStringLiteral("queue_position")).toInt(-1);
                    op->reportProgress(-1.0, pos >= 0 ? tr("Queued (position %1)").arg(pos + 1) : tr("Queued"));
                } else if (status == QLatin1String("IN_PROGRESS")) {
                    op->reportProgress(-1.0, tr("Generating"));
                } else if (status == QLatin1String("COMPLETED")) {
                    timer->stop();
                    const QString error = s.value(QStringLiteral("error")).toString();
                    if (!error.isEmpty())
                        op->fail(tr("fal.ai: %1").arg(error));
                    else
                        fetchResult();
                }
            });
        });
        timer->start(1000);
    };

    // Step 1: build the input (image encoding) off the GUI thread and submit it.
    op->reportProgress(-1.0, tr("Uploading"));
    async::run(std::move(build)).then(op, [self, op, run, startPolling](Call call) {
        if (!self || run->cancelled)
            return;
        if (call.model.isEmpty()) {
            op->fail(tr("No fal.ai model is configured for this action."));
            return;
        }
        const QString base = self->text(QStringLiteral("url"));
        const QUrl submit(base + u'/' + call.model);
        net::CallOptions o;
        o.timeoutMs = 120000;
        o.maxRetries = 0;
        net::HttpCall *c = self->http()->postJson(self->request(submit), call.input, o);
        run->calls << c;
        QObject::connect(c, &net::HttpCall::finished, op, [op, run, c, call, base, startPolling] {
            if (run->cancelled || op->isFinished())
                return;
            if (!c->ok()) {
                op->fail(c->errorString());
                return;
            }
            const QJsonObject o = c->json();
            const QString id = o.value(QStringLiteral("request_id")).toString();
            if (id.isEmpty()) {
                op->fail(tr("fal.ai did not accept the request."));
                return;
            }
            // The URLs come with the submit response; models with sub-paths are rooted at the app id.
            const QString root = base + u'/' + call.model + QStringLiteral("/requests/") + id;
            auto url = [&o](const char *key, const QString &fallback) {
                const QString v = o.value(QLatin1String(key)).toString();
                return QUrl(v.isEmpty() ? fallback : v);
            };
            run->statusUrl = url("status_url", root + QStringLiteral("/status"));
            run->responseUrl = url("response_url", root);
            run->cancelUrl = url("cancel_url", root + QStringLiteral("/cancel"));
            op->reportProgress(-1.0, tr("Queued"));
            startPolling();
        });
    });
}

ImageOperation *FalProvider::generate(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("The fal.ai API key is not set. Add it in Settings → Providers."));
        return op;
    }
    if ((r.mode == GenMode::Inpaint || r.mode == GenMode::Edit || r.mode == GenMode::ImageToImage) && r.initImage.isNull()) {
        failLater(op, tr("This action needs an input image."));
        return op;
    }
    runCall(op, [this, r] { return buildCall(r); });
    return op;
}

bool FalProvider::supportsUtility(Utility u) const
{
    return u == Utility::Upscale || u == Utility::RemoveBackground;
}

ImageOperation *FalProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &params)
{
    auto *op = new ImageOperation(this);
    if (!supportsUtility(utility)) {
        failLater(op, tr("Not supported by fal.ai"));
        return op;
    }
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("The fal.ai API key is not set. Add it in Settings → Providers."));
        return op;
    }
    const int scale = std::clamp(params.value(QStringLiteral("factor")).toInt(2), 1, 8);
    runCall(op, [utility, image, scale] {
        Call call;
        if (utility == Utility::Upscale) {
            call.model = QStringLiteral("fal-ai/esrgan");
            call.input = {{QStringLiteral("image_url"), dataUrl(image, 2048)}, {QStringLiteral("scale"), scale}};
        } else {
            call.model = QStringLiteral("fal-ai/birefnet");
            call.input = {{QStringLiteral("image_url"), dataUrl(image, 4096)}, {QStringLiteral("output_format"), QStringLiteral("png")}};
        }
        return call;
    });
    return op;
}

} // namespace lumen::ai
