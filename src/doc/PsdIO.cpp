#include "doc/PsdIO.h"

#include "core/ImageOps.h"
#include "doc/ProjectStore.h"

#include <QPainter>

namespace lumen::doc {

namespace {

const std::pair<const char *, BlendMode> kBlendKeys[] = {
    {"norm", BlendMode::Normal},     {"mul ", BlendMode::Multiply},   {"scrn", BlendMode::Screen},
    {"over", BlendMode::Overlay},    {"dark", BlendMode::Darken},     {"lite", BlendMode::Lighten},
    {"div ", BlendMode::ColorDodge}, {"idiv", BlendMode::ColorBurn},  {"hLit", BlendMode::HardLight},
    {"sLit", BlendMode::SoftLight},  {"diff", BlendMode::Difference}, {"smud", BlendMode::Exclusion},
    {"lddg", BlendMode::Add},
};

} // namespace

BlendMode blendModeFromPsdKey(const QByteArray &key, bool *known)
{
    for (const auto &[k, mode] : kBlendKeys) {
        if (key == k) {
            if (known)
                *known = true;
            return mode;
        }
    }
    if (known)
        *known = key == "pass"; // pass-through only applies to groups
    return BlendMode::Normal;
}

QByteArray psdKeyFromBlendMode(BlendMode mode)
{
    for (const auto &[k, m] : kBlendKeys) {
        if (m == mode)
            return QByteArray(k);
    }
    return QByteArray("norm");
}

PsdImport importPsd(const psd::File &file)
{
    PsdImport out;
    out.size = file.size;
    int clipped = 0, masked = 0;
    QStringList unknownBlend;
    const QList<psd::ResolvedLayer> resolved = psd::resolvedLayers(file);
    // PSD panels list the front-most layer first; documents store the bottom layer first.
    for (auto it = resolved.crbegin(); it != resolved.crend(); ++it) {
        const psd::Layer &l = *it->layer;
        QImage canvas(file.size, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        if (!l.image.isNull()) {
            QPainter p(&canvas);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(l.offset, img::normalized(l.image));
        }
        LayerPtr layer = makeLayer(l.name.isEmpty() ? QObject::tr("Layer") : l.name, file.size);
        layer->image = canvas;
        bool known = false;
        layer->props.blend = blendModeFromPsdKey(l.blendKey, &known);
        if (!known && !unknownBlend.contains(QString::fromLatin1(l.blendKey)))
            unknownBlend.append(QString::fromLatin1(l.blendKey).trimmed());
        layer->props.opacity = std::clamp(it->opacity, 0.0, 1.0);
        layer->props.visible = it->visible;
        if (!it->groupPath.isEmpty())
            layer->meta.insert(QStringLiteral("psd_group"), it->groupPath);
        clipped += l.clipped;
        masked += l.hasMask;
        out.layers.append(layer);
    }
    out.composite = file.composite.isNull() ? psd::flatten(file) : img::normalized(file.composite);
    if (clipped)
        out.warnings << QObject::tr("%n clipping mask layer(s) were imported as normal layers", nullptr, clipped);
    if (masked)
        out.warnings << QObject::tr("%n layer mask(s) were ignored", nullptr, masked);
    if (!unknownBlend.isEmpty())
        out.warnings << QObject::tr("unsupported blend modes shown as Normal: %1").arg(unknownBlend.join(QStringLiteral(", ")));
    return out;
}

PsdImport importPsd(const QString &path)
{
    QString error;
    const std::optional<psd::File> file = psd::read(path, &error);
    if (!file)
        throw StoreError(QObject::tr("Cannot open %1: %2").arg(path, error));
    return importPsd(*file);
}

psd::File exportPsd(const DocumentSnapshot &layers, const QSize &size)
{
    psd::File file;
    file.size = size;
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
        psd::Layer l = psd::Layer::pixels(it->props.name, it->image);
        l.visible = it->props.visible;
        l.opacity = std::clamp(int(std::lround(it->props.opacity * 255.0)), 0, 255);
        l.blendKey = psdKeyFromBlendMode(it->props.blend);
        file.layers.append(l);
    }
    file.composite = flatten(layers, size);
    return file;
}

} // namespace lumen::doc
