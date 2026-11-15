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
//
// A graded layer (plan M20 step 3) keeps its pixels from before the grading in `original` and the
// grading in meta["develop"]; `image` is the graded result, so everything that reads the layer sees
// what the canvas shows. Its pixels are not edited: Bake makes the grading part of them.
struct Layer
{
    QUuid id = QUuid::createUuid();
    LayerProps props;
    QImage image;
    QImage original;       // null when the layer has no grading
    QJsonObject meta;      // provenance (generation parameters, source asset...), the grading
    quint64 revision = 0;  // bumped on every pixel change; used to skip re-encoding unchanged layers
    quint64 savedRevision = ~quint64(0);

    bool graded() const { return !original.isNull(); }
    // Tools, filters and cut leave these pixels alone.
    bool pixelsLocked() const { return props.locked || graded(); }
    // The grading: {"steps": [{"effect": "develop", "values": {...}}]}.
    QJsonObject grading() const { return meta.value(QStringLiteral("develop")).toObject(); }
};

// What a layer's grading is, for undo and for setting it: the pixels before it (null: none), its
// steps, and the pixels it gives.
struct Grading
{
    QImage original;
    QJsonObject steps;
    QImage image;
};

using LayerPtr = std::shared_ptr<Layer>;

LayerPtr makeLayer(const QString &name, const QSize &size, const QImage &content = QImage());

// Immutable view of the layer stack used for off-thread flattening/export.
struct LayerSnapshot
{
    QImage image;
    LayerProps props;
    QString group; // the Photoshop group ("Head/Eyes") the layer was in, written back to PSD files
};
using DocumentSnapshot = QList<LayerSnapshot>;

// Flattens a layer stack (bottom to top) into `region` (whole image when region is null).
QImage flatten(const DocumentSnapshot &layers, const QSize &size, const QRect &region = QRect(),
               const QColor &background = Qt::transparent);

} // namespace lumen::doc
