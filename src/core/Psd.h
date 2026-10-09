#pragma once

#include <QImage>
#include <QList>
#include <QPoint>
#include <QString>

#include <optional>

class QIODevice;

// Adobe Photoshop (PSD, version 1) layered file writer and reader: 8-bit RGB, RLE (PackBits)
// channels, transparency, layer groups and Unicode layer names. Readable by Photoshop, Krita,
// GIMP, Clip Studio and Live2D Cubism Editor.
namespace lumen::psd {

struct Layer
{
    enum class Kind { Pixels, GroupBegin, GroupEnd };

    Kind kind = Kind::Pixels;
    QString name;
    QImage image;      // any format; cropped to its opaque bounds when written
    QPoint offset;     // position of `image` on the canvas
    int opacity = 255;
    bool visible = true;
    bool groupOpen = true; // GroupBegin only

    static Layer pixels(const QString &name, const QImage &image, const QPoint &offset = {})
    {
        Layer l;
        l.name = name;
        l.image = image;
        l.offset = offset;
        return l;
    }
    static Layer groupBegin(const QString &name, bool open = true)
    {
        Layer l;
        l.kind = Kind::GroupBegin;
        l.name = name;
        l.groupOpen = open;
        return l;
    }
    static Layer groupEnd()
    {
        Layer l;
        l.kind = Kind::GroupEnd;
        return l;
    }
};

struct File
{
    QSize size;
    // Top-to-bottom order (as in a layers panel). A group is GroupBegin, its children, GroupEnd.
    QList<Layer> layers;
    QImage composite; // flattened preview (computed from the layers when null on write)
};

bool write(QIODevice *device, const File &file, QString *error = nullptr);
bool write(const QString &path, const File &file, QString *error = nullptr);
QByteArray toBytes(const File &file);

std::optional<File> read(QIODevice *device, QString *error = nullptr);
std::optional<File> read(const QString &path, QString *error = nullptr);

// PackBits encoding of one row (exposed for tests).
QByteArray packBits(const uchar *data, int length);
bool unpackBits(const uchar *&src, const uchar *end, uchar *dst, int length);

// Flattens the pixel layers (respecting visibility and opacity, normal blending) onto a transparent canvas.
QImage flatten(const File &file);

} // namespace lumen::psd
