#include "doc/PsdIO.h"

#include "core/ImageOps.h"
#include "doc/ProjectStore.h"

#include <QColorSpace>
#include <QPainter>

#include <algorithm>
#include <initializer_list>

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
    int clipped = 0, masked = 0, texts = 0, effects = 0, adjustments = 0, smart = 0;
    QStringList unknownBlend;
    // What Photoshop keeps in a layer besides its pixels that Lumen does not: the pixels stay (as
    // Photoshop last drew them), the text, effects or settings behind them go.
    auto has = [](const psd::Layer &l, std::initializer_list<const char *> keys) {
        return std::any_of(keys.begin(), keys.end(), [&l](const char *k) { return l.otherInfo.contains(QByteArray(k)); });
    };
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
        texts += has(l, {"TySh", "tySh"});
        effects += has(l, {"lfx2", "lrFX", "lmfx"});
        adjustments += has(l, {"levl", "curv", "brit", "blnc", "hue ", "hue2", "selc", "thrs", "nvrt", "post", "mixr",
                               "grdm", "phfl", "expA", "vibA", "blwh", "clrL", "SoCo", "GdFl", "PtFl"});
        smart += has(l, {"SoLd", "PlLd", "SoLE"});
        out.layers.append(layer);
    }
    // A group's own opacity and hiding went onto its layers (Lumen has no groups; they are written
    // back from the layers' group names, as they were).
    int groupProps = 0;
    for (const psd::Layer &l : file.layers)
        groupProps += l.kind == psd::Layer::Kind::GroupBegin && (l.opacity != 255 || !l.visible);
    out.composite = file.composite.isNull() ? psd::flatten(file) : img::normalized(file.composite);
    if (clipped)
        out.warnings << QObject::tr("%n clipping mask layer(s) were imported as normal layers", nullptr, clipped);
    if (masked)
        out.warnings << QObject::tr("%n layer mask(s) were ignored", nullptr, masked);
    if (!unknownBlend.isEmpty())
        out.warnings << QObject::tr("unsupported blend modes shown as Normal: %1").arg(unknownBlend.join(QStringLiteral(", ")));
    if (texts)
        out.warnings << QObject::tr("%n text layer(s) became pictures of their text", nullptr, texts);
    if (effects)
        out.warnings << QObject::tr("the effects of %n layer(s) (shadows, strokes, glows) are not kept", nullptr, effects);
    if (adjustments)
        out.warnings << QObject::tr("%n adjustment or fill layer(s) are empty layers here", nullptr, adjustments);
    if (smart)
        out.warnings << QObject::tr("%n smart object(s) became plain layers", nullptr, smart);
    if (groupProps)
        out.warnings << QObject::tr("the opacity or hiding of %n group(s) went onto their layers", nullptr, groupProps);
    // A profile of other colours than sRGB (which Lumen shows and saves).
    if (!file.iccProfile.isEmpty()) {
        const QColorSpace space = QColorSpace::fromIccProfile(file.iccProfile);
        if (!space.isValid() || space.primaries() != QColorSpace::Primaries::SRgb)
            out.warnings << QObject::tr("its colour profile is not kept, so colours may shift");
    }
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
    // The groups the layers came from, opened and closed as the layers go from the top down (a
    // group whose layers were moved apart is written once for each run of them).
    QStringList open;
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
        const QStringList path = it->group.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        qsizetype common = 0;
        while (common < open.size() && common < path.size() && open[common] == path[common])
            ++common;
        for (qsizetype i = open.size(); i > common; --i)
            file.layers.append(psd::Layer::groupEnd());
        open = open.mid(0, common);
        for (qsizetype i = common; i < path.size(); ++i) {
            file.layers.append(psd::Layer::groupBegin(path[i]));
            open.append(path[i]);
        }
        psd::Layer l = psd::Layer::pixels(it->props.name, it->image);
        l.visible = it->props.visible;
        l.opacity = std::clamp(int(std::lround(it->props.opacity * 255.0)), 0, 255);
        l.blendKey = psdKeyFromBlendMode(it->props.blend);
        file.layers.append(l);
    }
    for (qsizetype i = 0; i < open.size(); ++i)
        file.layers.append(psd::Layer::groupEnd());
    file.composite = flatten(layers, size);
    return file;
}

} // namespace lumen::doc
