#pragma once

#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <memory>

namespace lumen::doc {

enum class BlendMode {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn,
    HardLight, SoftLight, Difference, Exclusion, Add
};

QStringList blendModeNames();
QString blendModeName(BlendMode mode);
BlendMode blendModeFromName(const QString &name);
QPainter::CompositionMode toCompositionMode(BlendMode mode);

struct LayerProps
{
    QString name;
    double opacity = 1.0;
    BlendMode blend = BlendMode::Normal;
    bool visible = true;
    bool locked = false;
    bool alphaLocked = false;

    bool operator==(const LayerProps &o) const
    {
        return name == o.name && opacity == o.opacity && blend == o.blend && visible == o.visible
            && locked == o.locked && alphaLocked == o.alphaLocked;
    }
    QJsonObject toJson() const;
    static LayerProps fromJson(const QJsonObject &json);
};

// A raster layer. Pixels are document-sized ARGB32_Premultiplied. Owned by a Document through
// shared pointers so undo commands can keep removed layers alive.
struct Layer
{
    QUuid id = QUuid::createUuid();
    LayerProps props;
    QImage image;
    QJsonObject meta;      // provenance (generation parameters, source asset...)
    quint64 revision = 0;  // bumped on every pixel change; used to skip re-encoding unchanged layers
    quint64 savedRevision = ~quint64(0);
};

using LayerPtr = std::shared_ptr<Layer>;

LayerPtr makeLayer(const QString &name, const QSize &size, const QImage &content = QImage());

// Immutable view of the layer stack used for off-thread flattening/export.
struct LayerSnapshot
{
    QImage image;
    LayerProps props;
};
using DocumentSnapshot = QList<LayerSnapshot>;

// Flattens a layer stack (bottom to top) into `region` (whole image when region is null).
QImage flatten(const DocumentSnapshot &layers, const QSize &size, const QRect &region = QRect(),
               const QColor &background = Qt::transparent);

} // namespace lumen::doc
