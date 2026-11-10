#include "ai/providers/OpenAiImageProvider.h"

#include "ai/OnlineCatalog.h"
#include "ai/ProviderUtil.h"
#include "ai/providers/GeminiProvider.h"
#include "core/ImageOps.h"
#include "core/JsonUtil.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace lumen::ai {

ProviderType OpenAiImageProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("openai_image");
    t.name = QStringLiteral("OpenAI Images");
    t.description = tr("GPT-Image models: high quality text-to-image, multi-image character references, masked edits and transparent backgrounds.");
    t.kind = ProviderKind::Image;
    t.icon = QStringLiteral("image");
    t.documentation = QUrl(QStringLiteral("https://platform.openai.com/docs/guides/image-generation"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Base URL"), F::Url, QStringLiteral("https://api.openai.com/v1"), QString(), {}, 0, 0, true, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), QString(), {}, 0, 0, true},
        {QStringLiteral("organization"), tr("Organization ID"), F::Text, QString(), QString(), {}, 0, 0, false, true},
        {QStringLiteral("model"), tr("Model"), F::Model, online::defaultModel(t.id),
         tr("Sunburst: more detail and more accurate edits. Flare: faster."), online::modelIds(t.id)},
        {QStringLiteral("quality"), tr("Quality"), F::Choice, QStringLiteral("high"), tr("xhigh and max: GPT Image 2.5 and later."),
         {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max"),
          QStringLiteral("auto")}},
        {QStringLiteral("input_fidelity"), tr("Reference fidelity"), F::Choice, QStringLiteral("high"),
         tr("How closely edits preserve faces and details from input images. Only GPT Image 1.5 and 1 mini take it."),
         {QStringLiteral("low"), QStringLiteral("high")}, 0, 0, false, true},
        {QStringLiteral("moderation"), tr("Moderation"), F::Choice, QStringLiteral("auto"), QString(),
         {QStringLiteral("auto"), QStringLiteral("low")}, 0, 0, false, true},
        {QStringLiteral("timeout_s"), tr("Timeout (seconds)"), F::Integer, 600, QString(), {}, 30, 3600, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, 3, QString(), {}, 1, 16, false, true},
    };
    return t;
}

OpenAiImageProvider::OpenAiImageProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

QNetworkRequest OpenAiImageProvider::request(const QString &path) const
{
    QList<QPair<QByteArray, QByteArray>> h{{"Authorization", "Bearer " + text(QStringLiteral("api_key")).toUtf8()}};
    if (!text(QStringLiteral("organization")).isEmpty())
        h.append(qMakePair(QByteArray("OpenAI-Organization"), text(QStringLiteral("organization")).toUtf8()));
    return net::HttpClient::makeRequest(endpoint(path), h);
}

QFuture<QString> OpenAiImageProvider::testConnection()
{
    return listModels().then([](QStringList models) {
        return models.isEmpty() ? tr("Connected") : tr("Connected - image models: %1").arg(models.join(QStringLiteral(", ")));
    });
}

QFuture<QStringList> OpenAiImageProvider::listModels()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/models")), opts), [typeId = type().id](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : call->json().value(QStringLiteral("data")).toArray()) {
            const QString id = v.toObject().value(QStringLiteral("id")).toString();
            // Shut-down models can stay listed for a while; they are not offered.
            if (id.contains(QLatin1String("image")) && online::replacement(typeId, id).isEmpty())
                out << id;
        }
        out.sort();
        return out;
    });
}

QString OpenAiImageProvider::modelFor(const QString &requested) const
{
    QString model = requested.isEmpty() ? text(QStringLiteral("model")) : requested;
    if (model.isEmpty())
        model = online::defaultModel(type().id);
    const QString next = online::replacement(type().id, model);
    return next.isEmpty() ? model : next;
}

ImageCapabilities OpenAiImageProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = true;
    c.imageToImage = true;
    c.inpaint = true;   // mask on the first input image
    c.edit = true;
    c.maxReferences = 15;
    c.poseControl = true; // skeleton sent as a guidance image
    c.negativePrompt = false;
    c.seed = false;
    c.transparentBackground = true;
    c.qualityTiers = true;
    c.nativePixels = 1024 * 1024;
    if (const auto m = online::model(type().id, modelFor(QString())); m && m->freeSize) {
        c.sizeMultiple = 16;
        c.maxPixels = 2560 * 1440; // larger is experimental
    } else {
        c.fixedSizes = {QSize(1024, 1024), QSize(1536, 1024), QSize(1024, 1536)};
    }
    return c;
}

QString OpenAiImageProvider::pickFreeSize(const QSize &requested)
{
    if (requested.isEmpty())
        return QStringLiteral("1024x1024");
    double w = requested.width();
    double h = requested.height();
    // 1:3 to 3:1.
    if (w > 3 * h)
        w = 3 * h;
    if (h > 3 * w)
        h = 3 * w;
    // Small requests are drawn at a working size and scaled back by Lumen; large ones stay below
    // the experimental range.
    const double pixels = w * h;
    const double minimum = 1024.0 * 640;
    const double maximum = 2560.0 * 1440;
    const double scale = pixels < minimum ? std::sqrt(minimum / pixels) : pixels > maximum ? std::sqrt(maximum / pixels) : 1.0;
    auto snap = [](double v) { return std::max(16, int(std::lround(v / 16.0)) * 16); };
    return QStringLiteral("%1x%2").arg(snap(w * scale)).arg(snap(h * scale));
}

QString OpenAiImageProvider::pickSize(const QSize &requested)
{
    if (requested.isEmpty())
        return QStringLiteral("1024x1024");
    const double aspect = double(requested.width()) / requested.height();
    if (aspect > 1.2)
        return QStringLiteral("1536x1024");
    if (aspect < 1.0 / 1.2)
        return QStringLiteral("1024x1536");
    return QStringLiteral("1024x1024");
}

ImageResult OpenAiImageProvider::parseResponse(const QByteArray &body)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    ImageResult r;
    for (const QJsonValue &v : o.value(QStringLiteral("data")).toArray()) {
        GeneratedImage g;
        g.image = decodeBase64Image(v.toObject().value(QStringLiteral("b64_json")).toString());
        g.info.insert(QStringLiteral("revised_prompt"), v.toObject().value(QStringLiteral("revised_prompt")));
        if (!g.image.isNull())
            r.images.append(g);
    }
    r.info.insert(QStringLiteral("usage"), o.value(QStringLiteral("usage")));
    return r;
}

ImageOperation *OpenAiImageProvider::generate(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("OpenAI API key is not set. Add it in Settings → Providers."));
        return op;
    }
    const QString model = modelFor(r.model);
    const auto known = online::model(type().id, model);
    const QString quality = r.quality.isEmpty() ? text(QStringLiteral("quality")) : r.quality;
    const QString size = known && known->freeSize ? pickFreeSize(r.size) : pickSize(r.size);
    // GPT Image 2 and later refuse it.
    const QString fidelity = known && known->inputFidelity ? text(QStringLiteral("input_fidelity")) : QString();
    const QString moderation = text(QStringLiteral("moderation"));
    const int n = std::clamp(r.batchCount, 1, 10);
    net::CallOptions opts;
    opts.timeoutMs = std::max(30, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.maxRetries = 0;

    const ControlInput *pose = nullptr;
    for (const ControlInput &c : r.controls) {
        if (c.type == ControlType::OpenPose)
            pose = &c;
    }
    const bool hasInit = r.mode != GenMode::TextToImage && !r.initImage.isNull();
    const bool useEdits = hasInit || !r.references.isEmpty() || pose;
    QString prompt = r.prompt;
    if (!r.negativePrompt.isEmpty())
        prompt += QStringLiteral("\nAvoid: ") + r.negativePrompt;

    if (!useEdits) {
        runImageRequest(op, http(), request(QStringLiteral("/images/generations")),
                        [=] {
                            QJsonObject body{{QStringLiteral("model"), model},
                                             {QStringLiteral("prompt"), prompt},
                                             {QStringLiteral("n"), n},
                                             {QStringLiteral("size"), size},
                                             {QStringLiteral("quality"), quality},
                                             {QStringLiteral("output_format"), QStringLiteral("png")},
                                             {QStringLiteral("moderation"), moderation},
                                             {QStringLiteral("background"), r.transparentBackground ? QStringLiteral("transparent") : QStringLiteral("auto")}};
                            return Payload{json::compact(json::merge(body, r.extras))};
                        },
                        [](const HttpResponse &resp) { return parseResponse(resp.body); }, opts);
        return op;
    }

    const QImage poseImage = pose ? pose->image : QImage();
    runImageRequest(op, http(), request(QStringLiteral("/images/edits")),
                    [=] {
                        net::MultipartBuilder mb;
                        // The mask (if any) applies to the first image, so the image being edited goes first.
                        const QString instruction = referenceInstruction(r, 1, hasInit, pose != nullptr);
                        mb.addField(QStringLiteral("model"), model);
                        mb.addField(QStringLiteral("prompt"), instruction.isEmpty() ? prompt : instruction + QStringLiteral("\n\n") + prompt);
                        mb.addField(QStringLiteral("n"), QString::number(n));
                        mb.addField(QStringLiteral("size"), size);
                        mb.addField(QStringLiteral("quality"), quality);
                        if (!fidelity.isEmpty())
                            mb.addField(QStringLiteral("input_fidelity"), fidelity);
                        mb.addField(QStringLiteral("output_format"), QStringLiteral("png"));
                        mb.addField(QStringLiteral("background"), r.transparentBackground ? QStringLiteral("transparent") : QStringLiteral("auto"));
                        int idx = 0;
                        auto addImage = [&mb, &idx](const QImage &img) {
                            mb.addFile(QStringLiteral("image[]"), QStringLiteral("image%1.png").arg(idx++),
                                       img::encode(img::thumbnail(img, 2048), "PNG", 1), QStringLiteral("image/png"));
                        };
                        if (hasInit)
                            addImage(r.initImage);
                        for (const ReferenceImage &ref : r.references)
                            addImage(ref.image);
                        if (!poseImage.isNull())
                            addImage(poseImage);
                        if (hasInit && r.mode == GenMode::Inpaint && !r.mask.isNull()) {
                            // OpenAI convention: fully transparent pixels mark the area to edit.
                            const QImage init = img::thumbnail(r.initImage, 2048);
                            const QImage alpha = img::alphaFromMask(init, img::scaled(r.mask, init.size(), true), true);
                            mb.addFile(QStringLiteral("mask"), QStringLiteral("mask.png"), img::encode(alpha, "PNG", 1), QStringLiteral("image/png"));
                        }
                        Payload p;
                        p.body = mb.finish();
                        p.contentType = mb.contentType();
                        return p;
                    },
                    [](const HttpResponse &resp) { return parseResponse(resp.body); }, opts);
    return op;
}

} // namespace lumen::ai
