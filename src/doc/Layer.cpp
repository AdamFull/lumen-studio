#include "doc/Layer.h"

namespace lumen::doc {

QStringList blendModeNames()
{
    return {QStringLiteral("Normal"),     QStringLiteral("Multiply"),   QStringLiteral("Screen"),
            QStringLiteral("Overlay"),    QStringLiteral("Darken"),     QStringLiteral("Lighten"),
            QStringLiteral("Color Dodge"), QStringLiteral("Color Burn"), QStringLiteral("Hard Light"),
            QStringLiteral("Soft Light"), QStringLiteral("Difference"), QStringLiteral("Exclusion"),
            QStringLiteral("Add")};
}

QString blendModeName(BlendMode mode)
{
    return blendModeNames().value(static_cast<int>(mode));
}

BlendMode blendModeFromName(const QString &name)
{
    const int i = blendModeNames().indexOf(name);
    return i >= 0 ? static_cast<BlendMode>(i) : BlendMode::Normal;
}

QPainter::CompositionMode toCompositionMode(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Normal: return QPainter::CompositionMode_SourceOver;
    case BlendMode::Multiply: return QPainter::CompositionMode_Multiply;
    case BlendMode::Screen: return QPainter::CompositionMode_Screen;
    case BlendMode::Overlay: return QPainter::CompositionMode_Overlay;
    case BlendMode::Darken: return QPainter::CompositionMode_Darken;
    case BlendMode::Lighten: return QPainter::CompositionMode_Lighten;
    case BlendMode::ColorDodge: return QPainter::CompositionMode_ColorDodge;
    case BlendMode::ColorBurn: return QPainter::CompositionMode_ColorBurn;
    case BlendMode::HardLight: return QPainter::CompositionMode_HardLight;
    case BlendMode::SoftLight: return QPainter::CompositionMode_SoftLight;
    case BlendMode::Difference: return QPainter::CompositionMode_Difference;
    case BlendMode::Exclusion: return QPainter::CompositionMode_Exclusion;
    case BlendMode::Add: return QPainter::CompositionMode_Plus;
    }
    return QPainter::CompositionMode_SourceOver;
}

QJsonObject LayerProps::toJson() const
{
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("opacity"), opacity},
                       {QStringLiteral("blend"), blendModeName(blend)},
                       {QStringLiteral("visible"), visible},
                       {QStringLiteral("locked"), locked},
                       {QStringLiteral("alphaLocked"), alphaLocked}};
}

LayerProps LayerProps::fromJson(const QJsonObject &json)
{
    LayerProps p;
    p.name = json.value(QStringLiteral("name")).toString();
    p.opacity = std::clamp(json.value(QStringLiteral("opacity")).toDouble(1.0), 0.0, 1.0);
    p.blend = blendModeFromName(json.value(QStringLiteral("blend")).toString());
    p.visible = json.value(QStringLiteral("visible")).toBool(true);
    p.locked = json.value(QStringLiteral("locked")).toBool(false);
    p.alphaLocked = json.value(QStringLiteral("alphaLocked")).toBool(false);
    return p;
}

LayerPtr makeLayer(const QString &name, const QSize &size, const QImage &content)
{
    auto layer = std::make_shared<Layer>();
    layer->props.name = name;
    layer->image = QImage(size, QImage::Format_ARGB32_Premultiplied);
    layer->image.fill(Qt::transparent);
    if (!content.isNull()) {
        QPainter p(&layer->image);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(0, 0, content);
    }
    return layer;
}

QImage flatten(const DocumentSnapshot &layers, const QSize &size, const QRect &region, const QColor &background)
{
    const QRect r = region.isNull() ? QRect(QPoint(0, 0), size) : region;
    QImage out(r.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    QPainter p(&out);
    for (const LayerSnapshot &l : layers) {
        if (!l.props.visible || l.props.opacity <= 0.0 || l.image.isNull())
            continue;
        p.setOpacity(l.props.opacity);
        p.setCompositionMode(toCompositionMode(l.props.blend));
        p.drawImage(QPoint(0, 0), l.image, r);
    }
    p.end();
    return out;
}

} // namespace lumen::doc
