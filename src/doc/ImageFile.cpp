#include "doc/ImageFile.h"

#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/Psd.h"
#include "doc/PsdIO.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>

#include <array>
#include <cmath>
#include <limits>

namespace lumen::doc {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::doc::ImageFile)
};

bool qtCanWrite(const QByteArray &format)
{
    return QImageWriter::supportedImageFormats().contains(format);
}

// libjpeg's luminance table (Annex K of the JPEG standard), in the order of the coefficients, and
// the zig-zag order in which a file stores them.
constexpr std::array<int, 64> kLuminance{16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                         14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                         18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                         49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<int, 64> kZigZag{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                      12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                      58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The first luminance table (table 0) of a JPEG file, in zig-zag order.
std::optional<std::array<int, 64>> luminanceTable(const QByteArray &data)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData());
    const qsizetype n = data.size();
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8)
        return std::nullopt;
    qsizetype i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xFF)
            return std::nullopt;
        const uchar marker = p[i + 1];
        if (marker == 0xFF) { // fill byte
            ++i;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) // end of image, start of scan: no table before it
            return std::nullopt;
        const int length = (p[i + 2] << 8) | p[i + 3];
        if (length < 2 || i + 2 + length > n)
            return std::nullopt;
        if (marker == 0xDB) {
            qsizetype j = i + 4;
            const qsizetype end = i + 2 + length;
            while (j < end) {
                const int precision = p[j] >> 4;
                const int id = p[j] & 0x0F;
                const qsizetype bytes = precision ? 128 : 64;
                if (j + 1 + bytes > end)
                    return std::nullopt;
                if (id == 0) {
                    std::array<int, 64> table{};
                    for (int k = 0; k < 64; ++k)
                        table[k] = precision ? (p[j + 1 + 2 * k] << 8) | p[j + 2 + 2 * k] : p[j + 1 + k];
                    return table;
                }
                j += 1 + bytes;
            }
        }
        i += 2 + length;
    }
    return std::nullopt;
}

} // namespace

QString ImageFormat::filter() const
{
    QStringList globs;
    for (const QString &s : suffixes)
        globs << QStringLiteral("*.") + s;
    return QStringLiteral("%1 (%2)").arg(name, globs.join(QLatin1Char(' ')));
}

QList<ImageFormat> writableImageFormats()
{
    QList<ImageFormat> out;
    out << ImageFormat{"png", QStringLiteral("PNG"), {QStringLiteral("png")}, true, false, false, -1};
    if (qtCanWrite("jpeg"))
        out << ImageFormat{"jpeg", QStringLiteral("JPEG"), {QStringLiteral("jpg"), QStringLiteral("jpeg")}, false, true, false, 92};
    if (qtCanWrite("webp"))
        out << ImageFormat{"webp", QStringLiteral("WebP"), {QStringLiteral("webp")}, true, true, false, 90};
    if (qtCanWrite("tiff"))
        out << ImageFormat{"tiff", QStringLiteral("TIFF"), {QStringLiteral("tif"), QStringLiteral("tiff")}, true, false, false, -1};
    out << ImageFormat{"psd", Text::tr("Layered Photoshop"), {QStringLiteral("psd")}, true, false, true, -1};
    if (qtCanWrite("bmp"))
        out << ImageFormat{"bmp", QStringLiteral("BMP"), {QStringLiteral("bmp")}, false, false, false, -1};
    return out;
}

std::optional<ImageFormat> imageFormatFor(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty())
        return std::nullopt;
    for (const ImageFormat &f : writableImageFormats()) {
        if (f.suffixes.contains(suffix))
            return f;
    }
    return std::nullopt;
}

std::optional<ImageFormat> imageFormatById(const QByteArray &id)
{
    for (const ImageFormat &f : writableImageFormats()) {
        if (f.id == id)
            return f;
    }
    return std::nullopt;
}

QString imageFileFilter(const QList<ImageFormat> &formats)
{
    QStringList filters;
    for (const ImageFormat &f : formats)
        filters << f.filter();
    return filters.join(QStringLiteral(";;"));
}

int estimateJpegQuality(const QByteArray &data)
{
    const std::optional<std::array<int, 64>> table = luminanceTable(data);
    if (!table)
        return -1;
    // The quality whose scaled standard table is closest (jpeg_set_quality with baseline tables).
    int best = -1;
    double bestError = std::numeric_limits<double>::max();
    for (int quality = 1; quality <= 100; ++quality) {
        const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
        double error = 0;
        for (int k = 0; k < 64; ++k) {
            const int expected = std::clamp((kLuminance[kZigZag[k]] * scale + 50) / 100, 1, 255);
            error += std::abs(expected - (*table)[k]);
        }
        if (error < bestError) {
            bestError = error;
            best = quality;
        }
    }
    // Tables another encoder made may fit no quality; one or two steps off on average is still it.
    return bestError / 64.0 <= 2.0 ? best : -1;
}

int estimateJpegQuality(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    // The tables come before the picture data; the first 64 KB hold them in any ordinary file.
    return estimateJpegQuality(file.read(64 * 1024));
}

bool hasCameraDetails(const QByteArray &data)
{
    const auto *d = reinterpret_cast<const uchar *>(data.constData());
    const qsizetype n = data.size();
    auto be16 = [d](qsizetype i) { return (int(d[i]) << 8) | d[i + 1]; };
    auto be32 = [d](qsizetype i) { return (quint32(d[i]) << 24) | (quint32(d[i + 1]) << 16) | (quint32(d[i + 2]) << 8) | d[i + 3]; };
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        // JPEG: an Exif APP1 segment among those before the picture data.
        qsizetype i = 2;
        while (i + 4 <= n) {
            if (d[i] != 0xFF)
                return false;
            const uchar marker = d[i + 1];
            if (marker == 0xFF) { // fill byte
                ++i;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA) // end, or the picture data
                return false;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                i += 2;
                continue;
            }
            const int length = be16(i + 2);
            if (marker == 0xE1 && i + 10 <= n && data.mid(i + 4, 6) == QByteArray("Exif\0\0", 6))
                return true;
            i += 2 + length;
        }
        return false;
    }
    if (data.startsWith("\x89PNG\r\n\x1a\n")) {
        // PNG: an eXIf chunk.
        qsizetype i = 8;
        while (i + 8 <= n) {
            const QByteArray type = data.mid(i + 4, 4);
            if (type == "eXIf")
                return true;
            if (type == "IEND")
                return false;
            i += 12 + qsizetype(be32(i));
        }
        return false;
    }
    if (n >= 30 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP" && data.mid(12, 4) == "VP8X") {
        // WebP: the Exif flag of the extended header (the chunk itself comes after the picture).
        return (d[20] & 0x08) != 0;
    }
    return false;
}

bool hasCameraDetails(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // The details come before the picture data in JPEG and PNG files as cameras and phones write them.
    return hasCameraDetails(file.read(256 * 1024));
}

SaveEffects saveEffects(const DocumentSnapshot &layers, const QSize &size, const ImageFormat &format)
{
    SaveEffects e;
    e.mergesLayers = !format.layers && layers.size() > 1;
    e.lossy = format.lossy;
    if (!format.alpha) {
        const QImage merged = flatten(layers, size);
        for (int y = 0; y < merged.height() && !e.dropsTransparency; ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(merged.constScanLine(y));
            for (int x = 0; x < merged.width(); ++x) {
                if (qAlpha(line[x]) < 255) {
                    e.dropsTransparency = true;
                    break;
                }
            }
        }
    }
    return e;
}

void writeImageFile(const QString &path, const ImageFormat &format, const DocumentSnapshot &layers, const QSize &size, int quality)
{
    const QString name = QFileInfo(path).fileName();
    QByteArray bytes;
    if (format.layers) {
        bytes = psd::toBytes(exportPsd(layers, size));
    } else {
        // A format without transparency gets the picture on white, as it looks on a white page.
        const QImage merged = flatten(layers, size, QRect(), format.alpha ? QColor(Qt::transparent) : QColor(Qt::white));
        const int q = quality > 0 ? quality : format.defaultQuality;
        // For PNG the quality is the zlib level: 6 makes small files and is still quick.
        bytes = img::encode(merged, format.id.toUpper().constData(), format.id == "png" ? 6 : q);
    }
    if (bytes.isEmpty())
        throw Error(Text::tr("Lumen cannot write %1 files here.").arg(format.name));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw Error(Text::tr("%1 could not be written: %2").arg(name, file.errorString()));
}

} // namespace lumen::doc
