#include "ai/providers/StabilityProvider.h"

#include "ai/ProviderUtil.h"
#include "core/ImageOps.h"

#include <QJsonDocument>

namespace lumen::ai {

namespace {

const QStringList kAspects = {QStringLiteral("21:9"), QStringLiteral("16:9"), QStringLiteral("3:2"), QStringLiteral("5:4"),
                              QStringLiteral("1:1"), QStringLiteral("4:5"), QStringLiteral("2:3"), QStringLiteral("9:16"),
                              QStringLiteral("9:21")};

QByteArray png(const QImage &image, int maxSide = 2048)
{
    return img::encode(img::thumbnail(image, maxSide), "PNG", 1);
}

} // namespace

ProviderType StabilityProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("stability");
    t.name = QStringLiteral("Stability AI");
    t.description = tr("Stable Image Ultra / Core / SD 3.5 through the Stability platform API, with inpainting, style reference, "
                       "background removal and upscaling.");
    t.kind = ProviderKind::Image;
    t.icon = QStringLiteral("image");
    t.documentation = QUrl(QStringLiteral("https://platform.stability.ai/docs/api-reference"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Base URL"), F::Url, QStringLiteral("https://api.stability.ai/v2beta"), QString(), {}, 0, 0, true, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), QString(), {}, 0, 0, true},
        {QStringLiteral("model"), tr("Model"), F::Choice, QStringLiteral("ultra"), tr("Generation service used for text-to-image."),
         {QStringLiteral("ultra"), QStringLiteral("core"), QStringLiteral("sd3.5-large"), QStringLiteral("sd3.5-large-turbo"), QStringLiteral("sd3.5-medium")}},
        {QStringLiteral("timeout_s"), tr("Timeout (seconds)"), F::Integer, 300, QString(), {}, 30, 3600, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, 3, QString(), {}, 1, 16, false, true},
    };
    return t;
}

StabilityProvider::StabilityProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

QNetworkRequest StabilityProvider::request(const QString &path) const
{
    return net::HttpClient::makeRequest(endpoint(path), {{"Authorization", "Bearer " + text(QStringLiteral("api_key")).toUtf8()},
                                                         {"Accept", "application/json"},
                                                         {"stability-client-id", "lumen-studio"}});
}

QFuture<QString> StabilityProvider::testConnection()
{
    // Account balance is a cheap authenticated endpoint (v1).
    QString base = text(QStringLiteral("url"));
    base.replace(QStringLiteral("/v2beta"), QStringLiteral("/v1"));
    QNetworkRequest req = net::HttpClient::makeRequest(QUrl(base + QStringLiteral("/user/balance")),
                                                       {{"Authorization", "Bearer " + text(QStringLiteral("api_key")).toUtf8()}});
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QString>(http()->get(req, opts), [](net::HttpCall *call) {
        return tr("Connected - %1 credits").arg(call->json().value(QStringLiteral("credits")).toDouble(), 0, 'f', 1);
    });
}

QFuture<QStringList> StabilityProvider::listModels()
{
    return QtFuture::makeReadyValueFuture(QStringList{QStringLiteral("ultra"), QStringLiteral("core"), QStringLiteral("sd3.5-large"),
                                                      QStringLiteral("sd3.5-large-turbo"), QStringLiteral("sd3.5-medium")});
}

ImageCapabilities StabilityProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = true;
    c.imageToImage = true;
    c.inpaint = true;
    c.edit = false;
    c.maxReferences = 1; // style reference
    c.poseControl = false;
    c.negativePrompt = true;
    c.seed = true;
    c.batch = false;
    c.aspectRatios = kAspects;
    c.nativePixels = 1024 * 1024;
    return c;
}

ImageResult StabilityProvider::parseResponse(const QByteArray &body)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString finish = o.value(QStringLiteral("finish_reason")).toString();
    if (finish == QLatin1String("CONTENT_FILTERED"))
        throw ProviderError(QObject::tr("Stability AI filtered the result (content policy)."));
    ImageResult r;
    GeneratedImage g;
    g.image = decodeBase64Image(o.value(QStringLiteral("image")).toString());
    g.seed = qint64(o.value(QStringLiteral("seed")).toDouble(-1));
    if (!g.image.isNull())
        r.images.append(g);
    return r;
}

void StabilityProvider::runMultipart(ImageOperation *op, const QString &path, std::function<void(net::MultipartBuilder &)> fill)
{
    net::CallOptions opts;
    opts.timeoutMs = std::max(30, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.maxRetries = 0;
    runImageRequest(op, http(), request(path),
                    [fill] {
                        net::MultipartBuilder mb;
                        fill(mb);
                        mb.addField(QStringLiteral("output_format"), QStringLiteral("png"));
                        Payload p;
                        p.body = mb.finish();
                        p.contentType = mb.contentType();
                        return p;
                    },
                    [](const HttpResponse &resp) { return parseResponse(resp.body); }, opts);
}

ImageOperation *StabilityProvider::generate(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("Stability AI API key is not set. Add it in Settings → Providers."));
        return op;
    }
    if (r.mode == GenMode::Edit) {
        failLater(op, tr("Stability AI does not support instruction edits; use inpainting instead."));
        return op;
    }
    const QString model = r.model.isEmpty() ? text(QStringLiteral("model")) : r.model;
    const QString aspect = closestAspectRatio(r.size, kAspects);
    auto common = [r](net::MultipartBuilder &mb) {
        mb.addField(QStringLiteral("prompt"), r.prompt);
        if (!r.negativePrompt.isEmpty())
            mb.addField(QStringLiteral("negative_prompt"), r.negativePrompt);
        if (r.seed >= 0)
            mb.addField(QStringLiteral("seed"), QString::number(r.seed % 4294967294LL));
    };
    if (r.mode == GenMode::Inpaint) {
        runMultipart(op, QStringLiteral("/stable-image/edit/inpaint"), [r, common](net::MultipartBuilder &mb) {
            common(mb);
            mb.addFile(QStringLiteral("image"), QStringLiteral("image.png"), png(r.initImage), QStringLiteral("image/png"));
            const QImage mask = img::scaled(r.mask, img::thumbnail(r.initImage, 2048).size(), true);
            mb.addFile(QStringLiteral("mask"), QStringLiteral("mask.png"), png(mask), QStringLiteral("image/png"));
            mb.addField(QStringLiteral("grow_mask"), QString::number(std::clamp(r.maskBlur, 0, 20)));
        });
        return op;
    }
    if (!r.references.isEmpty() && r.mode == GenMode::TextToImage) {
        runMultipart(op, QStringLiteral("/stable-image/control/style"), [r, common, aspect](net::MultipartBuilder &mb) {
            common(mb);
            mb.addFile(QStringLiteral("image"), QStringLiteral("style.png"), png(r.references.first().image, 1536), QStringLiteral("image/png"));
            mb.addField(QStringLiteral("fidelity"), QString::number(std::clamp(r.references.first().weight, 0.0, 1.0)));
            mb.addField(QStringLiteral("aspect_ratio"), aspect);
        });
        return op;
    }
    const bool sd3 = model.startsWith(QLatin1String("sd3"));
    const QString path = sd3 ? QStringLiteral("/stable-image/generate/sd3") : QStringLiteral("/stable-image/generate/") + model;
    runMultipart(op, path, [r, common, aspect, sd3, model](net::MultipartBuilder &mb) {
        common(mb);
        if (sd3)
            mb.addField(QStringLiteral("model"), model);
        if (r.mode == GenMode::ImageToImage && !r.initImage.isNull()) {
            if (sd3)
                mb.addField(QStringLiteral("mode"), QStringLiteral("image-to-image"));
            mb.addFile(QStringLiteral("image"), QStringLiteral("init.png"), png(r.initImage), QStringLiteral("image/png"));
            mb.addField(QStringLiteral("strength"), QString::number(std::clamp(r.denoise, 0.0, 1.0)));
        } else {
            mb.addField(QStringLiteral("aspect_ratio"), aspect);
        }
    });
    return op;
}

bool StabilityProvider::supportsUtility(Utility u) const
{
    return u == Utility::Upscale || u == Utility::RemoveBackground;
}

ImageOperation *StabilityProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &)
{
    auto *op = new ImageOperation(this);
    const QString path = utility == Utility::Upscale ? QStringLiteral("/stable-image/upscale/fast")
                                                     : QStringLiteral("/stable-image/edit/remove-background");
    if (!supportsUtility(utility)) {
        failLater(op, tr("Not supported by Stability AI"));
        return op;
    }
    runMultipart(op, path, [image](net::MultipartBuilder &mb) {
        mb.addFile(QStringLiteral("image"), QStringLiteral("image.png"), png(image, 4096), QStringLiteral("image/png"));
    });
    return op;
}

} // namespace lumen::ai
