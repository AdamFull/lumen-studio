#include "ai/providers/PartServerProvider.h"

#include "ai/ProviderUtil.h"
#include "core/ImageOps.h"
#include "core/JsonUtil.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>

namespace lumen::ai {

ProviderType PartServerProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("partserver");
    t.name = QStringLiteral("Lumen Part Server");
    t.description = tr("Character part segmentation for Live2D slicing (see-through or another model behind the Lumen "
                       "Part Server protocol). Start tools/partserver/server.py next to your see-through checkout.");
    t.kind = ProviderKind::Image;
    t.local = true;
    t.icon = QStringLiteral("layers");
    t.documentation = QUrl(QStringLiteral("https://github.com/shitagaki-lab/see-through"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Server URL"), F::Url, QStringLiteral("http://127.0.0.1:7870"), QString(), {}, 0, 0, true},
        {QStringLiteral("resolution"), tr("Resolution"), F::Choice, QStringLiteral("1280"),
         tr("Working resolution of the model. Lower values need less VRAM."),
         {QStringLiteral("768"), QStringLiteral("1024"), QStringLiteral("1280"), QStringLiteral("1536")}},
        {QStringLiteral("seed"), tr("Seed"), F::Integer, 42, tr("Fixed seed keeps repeated runs identical."), {}, 0, 2147483647, false, true},
        {QStringLiteral("timeout_min"), tr("Timeout (minutes)"), F::Integer, 20, QString(), {}, 1, 240, false, true},
        {QStringLiteral("concurrency"), tr("Parallel jobs"), F::Integer, 1, QString(), {}, 1, 4, false, true},
    };
    return t;
}

PartServerProvider::PartServerProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

QFuture<QString> PartServerProvider::testConnection()
{
    net::CallOptions opts;
    opts.timeoutMs = 8000;
    opts.maxRetries = 0;
    net::HttpCall *call = http()->get(net::HttpClient::makeRequest(endpoint(QStringLiteral("/v1/info"))), opts);
    return futureFromCall<QString>(call, [](net::HttpCall *c) {
        if (!c->ok())
            throw ProviderError(c->errorString());
        const QJsonObject o = c->json();
        const QString name = o.value(QStringLiteral("name")).toString(QStringLiteral("Part server"));
        const QString model = o.value(QStringLiteral("model")).toString();
        const int tags = int(o.value(QStringLiteral("tags")).toArray().size());
        QString summary = tr("Connected to %1").arg(name);
        if (!model.isEmpty())
            summary += QStringLiteral(" · ") + model;
        if (tags > 0)
            summary += QStringLiteral(" · ") + tr("%n part type(s)", nullptr, tags);
        return summary;
    });
}

QFuture<QStringList> PartServerProvider::listModels()
{
    return QtFuture::makeReadyValueFuture(QStringList());
}

ImageCapabilities PartServerProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = false;
    c.batch = false;
    return c;
}

ImageOperation *PartServerProvider::generate(const ImageRequest &)
{
    auto *op = new ImageOperation(this);
    failLater(op, tr("%1 only segments character parts; choose an image generation provider.").arg(displayName()));
    return op;
}

bool PartServerProvider::supportsUtility(Utility u) const
{
    return u == Utility::SegmentParts;
}

ImageResult PartServerProvider::parseSegmentResponse(const QByteArray &body, const QSize &canvasIn)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    if (o.contains(QStringLiteral("error")))
        throw ProviderError(o.value(QStringLiteral("error")).toString());
    QSize canvas = canvasIn;
    const int w = o.value(QStringLiteral("width")).toInt(), h = o.value(QStringLiteral("height")).toInt();
    const QSize serverCanvas = (w > 0 && h > 0) ? QSize(w, h) : canvas;
    ImageResult result;
    result.info = o.value(QStringLiteral("info")).toObject();
    const QJsonArray layers = o.value(QStringLiteral("layers")).toArray();
    int index = 0;
    for (const QJsonValue &v : layers) {
        const QJsonObject l = v.toObject();
        QImage image = decodeBase64Image(l.value(QStringLiteral("image")).toString());
        if (image.isNull())
            continue;
        const QString kind = l.value(QStringLiteral("kind")).toString(QStringLiteral("layer"));
        const QPoint offset(l.value(QStringLiteral("x")).toInt(), l.value(QStringLiteral("y")).toInt());
        // Place on the server canvas, then scale to the requested canvas if the server resized.
        QImage full;
        if (kind == QLatin1String("mask")) {
            full = img::createMask(serverCanvas, 0);
            QImage mask = image.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < mask.height(); ++y) {
                const int ty = y + offset.y();
                if (ty < 0 || ty >= full.height())
                    continue;
                for (int x = 0; x < mask.width(); ++x) {
                    const int tx = x + offset.x();
                    if (tx >= 0 && tx < full.width())
                        full.scanLine(ty)[tx] = mask.constScanLine(y)[x];
                }
            }
            if (serverCanvas != canvas)
                full = full.scaled(canvas, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        } else {
            full = QImage(serverCanvas, QImage::Format_ARGB32_Premultiplied);
            full.fill(Qt::transparent);
            QPainter p(&full);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(offset, image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
            p.end();
            if (serverCanvas != canvas)
                full = full.scaled(canvas, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        GeneratedImage g;
        g.image = full;
        g.info = QJsonObject{{QStringLiteral("label"), l.value(QStringLiteral("tag")).toString(l.value(QStringLiteral("label")).toString())},
                             {QStringLiteral("kind"), kind},
                             {QStringLiteral("order"), l.value(QStringLiteral("order")).toInt(index)}};
        if (l.contains(QStringLiteral("depth")))
            g.info.insert(QStringLiteral("depth"), l.value(QStringLiteral("depth")).toDouble());
        if (l.contains(QStringLiteral("score")))
            g.info.insert(QStringLiteral("score"), l.value(QStringLiteral("score")).toDouble());
        result.images.append(g);
        ++index;
    }
    if (result.images.isEmpty())
        throw ProviderError(QObject::tr("The part server returned no layers"));
    return result;
}

ImageOperation *PartServerProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &params)
{
    if (utility != Utility::SegmentParts)
        return ImageProvider::runUtility(utility, image, params);
    auto *op = new ImageOperation(this);
    net::CallOptions opts;
    opts.timeoutMs = std::max(1, setting(QStringLiteral("timeout_min")).toInt()) * 60000;
    opts.maxRetries = 0; // segmentation is expensive; never repeat it silently
    QJsonObject options{{QStringLiteral("resolution"), params.value(QStringLiteral("resolution")).toInt(text(QStringLiteral("resolution")).toInt())},
                        {QStringLiteral("seed"), params.value(QStringLiteral("seed")).toInt(setting(QStringLiteral("seed")).toInt())}};
    const QJsonArray parts = params.value(QStringLiteral("parts")).toArray();
    const QSize canvas = image.size();
    op->reportProgress(-1.0, tr("Segmenting parts (this can take a few minutes)..."));
    runImageRequest(
        op, http(), net::HttpClient::makeRequest(endpoint(QStringLiteral("/v1/segment"))),
        [image, parts, options] {
            const QJsonObject body{{QStringLiteral("image"), toBase64Png(image)},
                                   {QStringLiteral("parts"), parts},
                                   {QStringLiteral("options"), options}};
            return Payload{json::compact(body)};
        },
        [canvas](const HttpResponse &resp) { return parseSegmentResponse(resp.body, canvas); }, opts);
    return op;
}

} // namespace lumen::ai
