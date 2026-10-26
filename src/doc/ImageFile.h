#pragma once

#include "doc/Layer.h"

#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

// Image files as File → Save, Save as… and Export a copy… write them (M17): the formats Lumen can
// write here, what saving in one of them changes about a document, and the writing itself.
namespace lumen::doc {

struct ImageFormat
{
    QByteArray id;        // "png", "jpeg", "webp", "tiff", "bmp", "psd"
    QString name;         // "PNG", "JPEG"… as the save dialogs list it
    QStringList suffixes; // the first is the one a new file gets
    bool alpha = true;    // keeps transparency
    bool lossy = false;   // compresses with some loss of quality each time
    bool layers = false;  // keeps the layers (PSD)
    int defaultQuality = -1;

    // "JPEG (*.jpg *.jpeg)", for QFileDialog.
    QString filter() const;
};

// The formats Lumen can write on this computer, in the order the save dialogs list them: PNG, JPEG,
// WebP and TIFF where Qt can write them, layered Photoshop (Lumen's own writer), BMP.
QList<ImageFormat> writableImageFormats();
// The writable format of a file by its suffix ("photo.JPG" → JPEG); nullopt when Lumen cannot
// write it (GIF) or the name has no suffix.
std::optional<ImageFormat> imageFormatFor(const QString &path);
std::optional<ImageFormat> imageFormatById(const QByteArray &id);
// The filters of `formats` joined for QFileDialog.
QString imageFileFilter(const QList<ImageFormat> &formats);

// The quality a JPEG file was saved with (1–100), told from its luminance table as libjpeg (and so
// Qt) makes it; -1 when it cannot be told (not a JPEG, or tables of another encoder that fit no
// quality well).
int estimateJpegQuality(const QByteArray &data);
int estimateJpegQuality(const QString &path);

// What saving `layers` in `format` changes compared with the document.
struct SaveEffects
{
    bool mergesLayers = false;      // several layers become one picture
    bool dropsTransparency = false; // transparent pixels become white (no alpha in the format)
    bool lossy = false;             // compressed with some loss of quality

    bool any() const { return mergesLayers || dropsTransparency || lossy; }
};
SaveEffects saveEffects(const DocumentSnapshot &layers, const QSize &size, const ImageFormat &format);

// Writes the layers as an image file: merged (on white for a format without transparency), or with
// its layers for PSD. Through a temporary file that replaces `path` only once it is complete, so a
// failed save leaves the file as it was. `quality` -1 is the format's default. Throws lumen::Error
// with the reason in plain words. Any thread.
void writeImageFile(const QString &path, const ImageFormat &format, const DocumentSnapshot &layers, const QSize &size,
                    int quality = -1);

} // namespace lumen::doc
