#include "core/Psd.h"

#include <QBuffer>
#include <QHash>
#include <QFile>
#include <QPainter>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>

namespace lumen::psd {

namespace {

constexpr int kMaxDimension = 30000;
const QString kDividerName = QStringLiteral("</Layer group>");

enum SectionType : quint32 { SectionOther = 0, SectionOpenFolder = 1, SectionClosedFolder = 2, SectionDivider = 3 };

class Writer
{
public:
    QByteArray data;
    void u8(quint8 v) { data.append(char(v)); }
    void u16(quint16 v)
    {
        char b[2];
        qToBigEndian(v, b);
        data.append(b, 2);
    }
    void i16(qint16 v) { u16(quint16(v)); }
    void u32(quint32 v)
    {
        char b[4];
        qToBigEndian(v, b);
        data.append(b, 4);
    }
    void i32(qint32 v) { u32(quint32(v)); }
    void raw(const char *s, int n) { data.append(s, n); }
    void raw(const QByteArray &b) { data.append(b); }
    void pad(int multiple)
    {
        while (data.size() % multiple)
            data.append('\0');
    }
    // Reserves a u32 length field; returns its position.
    qsizetype beginLength()
    {
        const qsizetype pos = data.size();
        u32(0);
        return pos;
    }
    void endLength(qsizetype pos)
    {
        qToBigEndian(quint32(data.size() - pos - 4), data.data() + pos);
    }
};

class Reader
{
public:
    Reader(const QByteArray &d)
        : m_data(d)
    {
    }
    bool ok() const { return m_ok; }
    qsizetype pos() const { return m_pos; }
    qsizetype remaining() const { return m_data.size() - m_pos; }
    void seek(qsizetype p)
    {
        if (p < 0 || p > m_data.size())
            m_ok = false;
        else
            m_pos = p;
    }
    void skip(qsizetype n) { seek(m_pos + n); }
    const uchar *ptr() const { return reinterpret_cast<const uchar *>(m_data.constData()) + m_pos; }
    const uchar *end() const { return reinterpret_cast<const uchar *>(m_data.constData()) + m_data.size(); }
    bool need(qsizetype n)
    {
        if (!m_ok || remaining() < n) {
            m_ok = false;
            return false;
        }
        return true;
    }
    quint8 u8()
    {
        if (!need(1))
            return 0;
        return quint8(m_data[m_pos++]);
    }
    quint16 u16()
    {
        if (!need(2))
            return 0;
        const quint16 v = qFromBigEndian<quint16>(m_data.constData() + m_pos);
        m_pos += 2;
        return v;
    }
    qint16 i16() { return qint16(u16()); }
    quint32 u32()
    {
        if (!need(4))
            return 0;
        const quint32 v = qFromBigEndian<quint32>(m_data.constData() + m_pos);
        m_pos += 4;
        return v;
    }
    qint32 i32() { return qint32(u32()); }
    QByteArray bytes(qsizetype n)
    {
        if (!need(n))
            return {};
        const QByteArray b = m_data.mid(m_pos, n);
        m_pos += n;
        return b;
    }

private:
    const QByteArray &m_data;
    qsizetype m_pos = 0;
    bool m_ok = true;
};

// Encodes a channel (rows of `width` bytes) with PackBits; returns [row counts][data].
QByteArray encodeChannelRle(const QList<QByteArray> &rows)
{
    Writer w;
    QList<QByteArray> packed;
    packed.reserve(rows.size());
    for (const QByteArray &row : rows)
        packed.append(packBits(reinterpret_cast<const uchar *>(row.constData()), int(row.size())));
    for (const QByteArray &p : packed)
        w.u16(quint16(p.size()));
    for (const QByteArray &p : packed)
        w.raw(p);
    return w.data;
}

// Extracts channel rows (A, R, G, B) from a non-premultiplied ARGB32 image.
QList<QByteArray> channelRows(const QImage &argb, int channel)
{
    QList<QByteArray> rows;
    rows.reserve(argb.height());
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *src = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        QByteArray row(argb.width(), Qt::Uninitialized);
        for (int x = 0; x < argb.width(); ++x) {
            switch (channel) {
            case -1: row[x] = char(qAlpha(src[x])); break;
            case 0: row[x] = char(qRed(src[x])); break;
            case 1: row[x] = char(qGreen(src[x])); break;
            default: row[x] = char(qBlue(src[x])); break;
            }
        }
        rows.append(row);
    }
    return rows;
}

QRect opaqueBounds(const QImage &argb)
{
    int minX = argb.width(), minY = argb.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(row[x])) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = y;
            }
        }
    }
    if (maxX < 0)
        return {};
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

void writePascalName(Writer &w, const QString &name)
{
    QByteArray latin = name.toLatin1().left(255);
    for (char &c : latin) {
        if (uchar(c) < 32)
            c = '_';
    }
    const qsizetype start = w.data.size();
    w.u8(quint8(latin.size()));
    w.raw(latin);
    while ((w.data.size() - start) % 4)
        w.u8(0);
}

void writeAdditional(Writer &w, const char key[4], const QByteArray &payload)
{
    w.raw("8BIM", 4);
    w.raw(key, 4);
    QByteArray padded = payload;
    while (padded.size() % 4)
        padded.append('\0');
    w.u32(quint32(padded.size()));
    w.raw(padded);
}

QByteArray unicodeName(const QString &name)
{
    Writer w;
    w.u32(quint32(name.size()));
    for (QChar c : name)
        w.u16(c.unicode());
    return w.data;
}

struct EncodedLayer
{
    QRect rect;           // canvas coordinates
    QList<QByteArray> channels; // compression + data per channel, order: -1, 0, 1, 2
};

const qint16 kChannelIds[4] = {-1, 0, 1, 2};

QString readUnicode(const QByteArray &payload)
{
    if (payload.size() < 4)
        return {};
    const quint32 count = qFromBigEndian<quint32>(payload.constData());
    QString s;
    for (quint32 i = 0; i < count && 4 + qsizetype(i) * 2 + 1 < payload.size(); ++i) {
        const quint16 c = qFromBigEndian<quint16>(payload.constData() + 4 + i * 2);
        if (c == 0 && i + 1 == count)
            break;
        s.append(QChar(c));
    }
    return s;
}

bool decodeChannel(const QByteArray &blob, int width, int height, QByteArray *out)
{
    out->resize(qsizetype(width) * height);
    if (width == 0 || height == 0)
        return true;
    if (blob.size() < 2)
        return false;
    const quint16 compression = qFromBigEndian<quint16>(blob.constData());
    const uchar *p = reinterpret_cast<const uchar *>(blob.constData()) + 2;
    const uchar *end = reinterpret_cast<const uchar *>(blob.constData()) + blob.size();
    if (compression == 0) {
        if (end - p < qsizetype(width) * height)
            return false;
        memcpy(out->data(), p, size_t(qsizetype(width) * height));
        return true;
    }
    if (compression != 1)
        return false;
    if (end - p < qsizetype(height) * 2)
        return false;
    p += qsizetype(height) * 2; // row byte counts (not needed: PackBits is self-delimiting per row)
    for (int y = 0; y < height; ++y) {
        if (!unpackBits(p, end, reinterpret_cast<uchar *>(out->data()) + qsizetype(y) * width, width))
            return false;
    }
    return true;
}

} // namespace

// ---- PackBits -----------------------------------------------------------------------------------

QByteArray packBits(const uchar *data, int length)
{
    QByteArray out;
    out.reserve(length + length / 128 + 2);
    int i = 0;
    while (i < length) {
        // Run of identical bytes?
        int run = 1;
        while (i + run < length && run < 128 && data[i + run] == data[i])
            ++run;
        if (run >= 3 || (run == 2 && i + run == length)) {
            out.append(char(qint8(1 - run)));
            out.append(char(data[i]));
            i += run;
            continue;
        }
        // Literal sequence until the next run of 3.
        const int start = i;
        int count = 0;
        while (i < length && count < 128) {
            if (i + 2 < length && data[i] == data[i + 1] && data[i] == data[i + 2])
                break;
            ++i;
            ++count;
        }
        out.append(char(count - 1));
        out.append(reinterpret_cast<const char *>(data + start), count);
    }
    return out;
}

bool unpackBits(const uchar *&src, const uchar *end, uchar *dst, int length)
{
    int written = 0;
    while (written < length) {
        if (src >= end)
            return false;
        const qint8 n = qint8(*src++);
        if (n >= 0) {
            const int count = n + 1;
            if (end - src < count || written + count > length)
                return false;
            memcpy(dst + written, src, size_t(count));
            src += count;
            written += count;
        } else if (n != -128) {
            const int count = 1 - n;
            if (src >= end || written + count > length)
                return false;
            memset(dst + written, *src++, size_t(count));
            written += count;
        }
    }
    return true;
}

// ---- Writing ------------------------------------------------------------------------------------

QImage flatten(const File &file)
{
    QImage out(file.size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    struct Item
    {
        const Layer *layer;
        double opacity;
    };
    QList<Item> items;
    QList<std::pair<bool, double>> groups; // visible, opacity
    bool visible = true;
    double opacity = 1.0;
    for (const Layer &l : file.layers) {
        switch (l.kind) {
        case Layer::Kind::GroupBegin:
            groups.append({visible, opacity});
            visible = visible && l.visible;
            opacity *= l.opacity / 255.0;
            break;
        case Layer::Kind::GroupEnd:
            if (!groups.isEmpty()) {
                visible = groups.last().first;
                opacity = groups.last().second;
                groups.removeLast();
            }
            break;
        case Layer::Kind::Pixels:
            if (visible && l.visible && !l.image.isNull())
                items.append({&l, opacity * l.opacity / 255.0});
            break;
        }
    }
    QPainter p(&out);
    for (auto it = items.crbegin(); it != items.crend(); ++it) {
        p.setOpacity(it->opacity);
        p.drawImage(it->layer->offset, it->layer->image);
    }
    p.end();
    return out;
}

QByteArray toBytes(const File &file)
{
    const QSize size = file.size;
    Writer w;
    // Header.
    w.raw("8BPS", 4);
    w.u16(1);
    w.raw("\0\0\0\0\0\0", 6);
    w.u16(4); // R, G, B, A
    w.u32(quint32(size.height()));
    w.u32(quint32(size.width()));
    w.u16(8);
    w.u16(3); // RGB
    w.u32(0); // colour mode data
    w.u32(0); // image resources

    // File order is bottom-to-top: reverse the panel order. GroupEnd becomes the hidden divider
    // record that precedes the children; GroupBegin becomes the folder record after them.
    QList<const Layer *> order;
    for (auto it = file.layers.crbegin(); it != file.layers.crend(); ++it)
        order.append(&*it);

    QList<EncodedLayer> encoded;
    encoded.reserve(order.size());
    for (const Layer *l : order) {
        EncodedLayer e;
        if (l->kind == Layer::Kind::Pixels && !l->image.isNull()) {
            const QImage argb = l->image.convertToFormat(QImage::Format_ARGB32);
            const QRect bounds = opaqueBounds(argb);
            if (!bounds.isEmpty()) {
                const QImage crop = argb.copy(bounds);
                e.rect = bounds.translated(l->offset);
                for (qint16 id : kChannelIds) {
                    Writer c;
                    c.u16(1);
                    c.raw(encodeChannelRle(channelRows(crop, id)));
                    e.channels.append(c.data);
                }
            }
        }
        if (e.channels.isEmpty()) {
            for (int i = 0; i < 4; ++i)
                e.channels.append(QByteArray("\0\0", 2)); // raw, no pixels
        }
        encoded.append(e);
    }

    const qsizetype layerMaskPos = w.beginLength();
    const qsizetype layerInfoPos = w.beginLength();
    w.i16(qint16(-int(order.size()))); // negative: merged image has an alpha channel
    for (int i = 0; i < order.size(); ++i) {
        const Layer *l = order[i];
        const EncodedLayer &e = encoded[i];
        w.i32(e.rect.isEmpty() ? 0 : e.rect.top());
        w.i32(e.rect.isEmpty() ? 0 : e.rect.left());
        w.i32(e.rect.isEmpty() ? 0 : e.rect.top() + e.rect.height());
        w.i32(e.rect.isEmpty() ? 0 : e.rect.left() + e.rect.width());
        w.u16(4);
        for (int c = 0; c < 4; ++c) {
            w.i16(kChannelIds[c]);
            w.u32(quint32(e.channels[c].size()));
        }
        const bool isFolder = l->kind == Layer::Kind::GroupBegin;
        const bool isDivider = l->kind == Layer::Kind::GroupEnd;
        w.raw("8BIM", 4);
        w.raw(isFolder ? "pass" : "norm", 4);
        w.u8(quint8(std::clamp(l->opacity, 0, 255)));
        w.u8(0); // clipping
        quint8 flags = 0;
        if (!l->visible)
            flags |= 0x02;
        if (isFolder || isDivider)
            flags |= 0x08 | 0x10;
        w.u8(flags);
        w.u8(0);
        const qsizetype extraPos = w.beginLength();
        w.u32(0); // layer mask data
        w.u32(0); // blending ranges
        const QString name = isDivider ? kDividerName : l->name;
        writePascalName(w, name);
        writeAdditional(w, "luni", unicodeName(name));
        if (isFolder || isDivider) {
            Writer s;
            s.u32(isDivider ? SectionDivider : (l->groupOpen ? SectionOpenFolder : SectionClosedFolder));
            if (isFolder) {
                s.raw("8BIM", 4);
                s.raw("pass", 4);
            }
            writeAdditional(w, "lsct", s.data);
        }
        w.endLength(extraPos);
    }
    for (const EncodedLayer &e : encoded) {
        for (const QByteArray &c : e.channels)
            w.raw(c);
    }
    w.pad(4);
    w.endLength(layerInfoPos);
    w.u32(0); // global layer mask info
    w.endLength(layerMaskPos);

    // Merged image: RLE, channel order R, G, B, A.
    QImage composite = file.composite.isNull() ? flatten(file) : file.composite;
    composite = composite.convertToFormat(QImage::Format_ARGB32);
    if (composite.size() != size)
        composite = composite.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    w.u16(1);
    QList<QByteArray> packedRows;
    for (int channel : {0, 1, 2, -1}) {
        for (const QByteArray &row : channelRows(composite, channel))
            packedRows.append(packBits(reinterpret_cast<const uchar *>(row.constData()), int(row.size())));
    }
    for (const QByteArray &p : packedRows)
        w.u16(quint16(p.size()));
    for (const QByteArray &p : packedRows)
        w.raw(p);
    return w.data;
}

bool write(QIODevice *device, const File &file, QString *error)
{
    if (file.size.isEmpty() || file.size.width() > kMaxDimension || file.size.height() > kMaxDimension) {
        if (error)
            *error = QStringLiteral("Unsupported canvas size for PSD");
        return false;
    }
    const QByteArray bytes = toBytes(file);
    if (device->write(bytes) != bytes.size()) {
        if (error)
            *error = device->errorString();
        return false;
    }
    return true;
}

bool write(const QString &path, const File &file, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    if (!write(&f, file, error)) {
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

// ---- Reading ------------------------------------------------------------------------------------

std::optional<File> read(QIODevice *device, QString *error)
{
    auto fail = [error](const QString &msg) -> std::optional<File> {
        if (error)
            *error = msg;
        return std::nullopt;
    };
    const QByteArray data = device->readAll();
    Reader r(data);
    if (r.bytes(4) != "8BPS")
        return fail(QStringLiteral("Not a PSD file"));
    if (r.u16() != 1)
        return fail(QStringLiteral("Only PSD version 1 is supported"));
    r.skip(6);
    const int channels = r.u16();
    const int height = int(r.u32());
    const int width = int(r.u32());
    const int depth = r.u16();
    const int mode = r.u16();
    if (!r.ok() || width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension)
        return fail(QStringLiteral("Invalid PSD header"));
    if (depth != 8 || mode != 3)
        return fail(QStringLiteral("Only 8-bit RGB PSD files are supported"));
    r.skip(r.u32()); // colour mode data
    r.skip(r.u32()); // image resources

    File file;
    file.size = QSize(width, height);
    const quint32 layerMaskLength = r.u32();
    const qsizetype layerMaskEnd = r.pos() + layerMaskLength;
    if (layerMaskLength > 0) {
        const quint32 layerInfoLength = r.u32();
        const qsizetype layerInfoEnd = r.pos() + layerInfoLength;
        if (layerInfoLength > 0) {
            const int count = std::abs(int(r.i16()));
            struct Record
            {
                QRect rect;
                QList<std::pair<qint16, quint32>> channels;
                int opacity = 255;
                bool visible = true;
                QString name;
                quint32 section = SectionOther;
            };
            QList<Record> records;
            for (int i = 0; i < count && r.ok(); ++i) {
                Record rec;
                const int top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
                rec.rect = QRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
                const int nch = r.u16();
                for (int c = 0; c < nch && r.ok(); ++c) {
                    const qint16 id = r.i16();
                    rec.channels.append({id, r.u32()});
                }
                r.skip(8); // blend signature + key
                rec.opacity = r.u8();
                r.u8();    // clipping
                rec.visible = !(r.u8() & 0x02);
                r.u8();    // filler
                const quint32 extraLength = r.u32();
                const qsizetype extraEnd = r.pos() + extraLength;
                r.skip(r.u32()); // layer mask
                r.skip(r.u32()); // blending ranges
                const int nameLength = r.u8();
                rec.name = QString::fromLatin1(r.bytes(nameLength));
                const int padded = ((nameLength + 1 + 3) / 4) * 4;
                r.skip(padded - nameLength - 1);
                while (r.ok() && extraEnd - r.pos() >= 12) {
                    const QByteArray sig = r.bytes(4);
                    const QByteArray key = r.bytes(4);
                    const quint32 len = r.u32();
                    if (sig != "8BIM" && sig != "8B64")
                        break;
                    const QByteArray payload = r.bytes(len);
                    if (key == "luni") {
                        const QString u = readUnicode(payload);
                        if (!u.isEmpty())
                            rec.name = u;
                    } else if (key == "lsct" && payload.size() >= 4) {
                        rec.section = qFromBigEndian<quint32>(payload.constData());
                    }
                }
                r.seek(extraEnd);
                records.append(rec);
            }
            // Channel image data.
            QList<Layer> fileOrder;
            for (const Record &rec : std::as_const(records)) {
                Layer layer;
                layer.name = rec.name;
                layer.opacity = rec.opacity;
                layer.visible = rec.visible;
                QHash<qint16, QByteArray> planes;
                bool ok = true;
                for (const auto &[id, length] : rec.channels) {
                    const QByteArray blob = r.bytes(length);
                    if (!r.ok())
                        return fail(QStringLiteral("Truncated PSD layer data"));
                    if (id >= -1 && id <= 2 && rec.section == SectionOther) {
                        QByteArray plane;
                        ok = ok && decodeChannel(blob, rec.rect.width(), rec.rect.height(), &plane);
                        planes.insert(id, plane);
                    }
                }
                if (!ok)
                    return fail(QStringLiteral("Corrupt PSD channel data in layer \"%1\"").arg(rec.name));
                if (rec.section == SectionOpenFolder || rec.section == SectionClosedFolder) {
                    layer.kind = Layer::Kind::GroupBegin;
                    layer.groupOpen = rec.section == SectionOpenFolder;
                } else if (rec.section == SectionDivider) {
                    layer.kind = Layer::Kind::GroupEnd;
                    layer.name.clear();
                } else if (!rec.rect.isEmpty()) {
                    QImage img(rec.rect.size(), QImage::Format_ARGB32);
                    const int w = rec.rect.width();
                    for (int y = 0; y < rec.rect.height(); ++y) {
                        QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                        const qsizetype o = qsizetype(y) * w;
                        for (int x = 0; x < w; ++x) {
                            auto ch = [&](qint16 id, int def) {
                                const auto it = planes.constFind(id);
                                return it == planes.constEnd() ? def : int(uchar(it->at(o + x)));
                            };
                            dst[x] = qRgba(ch(0, 0), ch(1, 0), ch(2, 0), ch(-1, 255));
                        }
                    }
                    layer.image = img;
                    layer.offset = rec.rect.topLeft();
                }
                fileOrder.append(layer);
            }
            for (auto it = fileOrder.crbegin(); it != fileOrder.crend(); ++it)
                file.layers.append(*it);
        }
        r.seek(layerInfoEnd);
    }
    r.seek(layerMaskEnd);

    // Merged image (optional for our purposes).
    if (r.ok() && r.remaining() >= 2) {
        const quint16 compression = r.u16();
        QList<QByteArray> planes;
        const qsizetype pixels = qsizetype(width) * height;
        bool ok = true;
        if (compression == 0) {
            for (int c = 0; c < channels && ok; ++c) {
                const QByteArray plane = r.bytes(pixels);
                ok = r.ok();
                planes.append(plane);
            }
        } else if (compression == 1) {
            r.skip(qsizetype(channels) * height * 2);
            const uchar *p = r.ptr();
            for (int c = 0; c < channels && ok; ++c) {
                QByteArray plane(pixels, Qt::Uninitialized);
                for (int y = 0; y < height && ok; ++y)
                    ok = unpackBits(p, r.end(), reinterpret_cast<uchar *>(plane.data()) + qsizetype(y) * width, width);
                planes.append(plane);
            }
        } else {
            ok = false;
        }
        if (ok && planes.size() >= 3) {
            QImage img(width, height, QImage::Format_ARGB32);
            for (int y = 0; y < height; ++y) {
                QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
                const qsizetype o = qsizetype(y) * width;
                for (int x = 0; x < width; ++x) {
                    const int a = planes.size() >= 4 ? uchar(planes[3][o + x]) : 255;
                    dst[x] = qRgba(uchar(planes[0][o + x]), uchar(planes[1][o + x]), uchar(planes[2][o + x]), a);
                }
            }
            file.composite = img;
        }
    }
    if (!r.ok() && file.layers.isEmpty() && file.composite.isNull())
        return fail(QStringLiteral("Truncated PSD file"));
    return file;
}

std::optional<File> read(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return std::nullopt;
    }
    return read(&f, error);
}

} // namespace lumen::psd
