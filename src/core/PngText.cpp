#include "core/PngText.h"

#include <QtEndian>

#include <array>

namespace lumen::png {

namespace {

const QByteArray kSignature("\x89PNG\r\n\x1a\n", 8);

quint32 crc32(const QByteArray &data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (char ch : data)
        c = table[(c ^ static_cast<quint8>(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QByteArray chunk(const QByteArray &type, const QByteArray &payload)
{
    QByteArray out;
    out.resize(4);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), out.data());
    const QByteArray body = type + payload;
    out += body;
    QByteArray crc(4, '\0');
    qToBigEndian<quint32>(crc32(body), crc.data());
    out += crc;
    return out;
}

bool isLatin1(const QString &s)
{
    for (QChar c : s) {
        if (c.unicode() > 0xFF)
            return false;
    }
    return true;
}

QByteArray inflateZlib(const QByteArray &zlibData)
{
    // qUncompress expects a 4-byte big-endian expected-size prefix; provide a generous upper bound.
    QByteArray prefixed(4, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(std::min<qsizetype>(zlibData.size() * 64 + 1024, 64 * 1024 * 1024)),
                          prefixed.data());
    prefixed += zlibData;
    return qUncompress(prefixed);
}

} // namespace

bool isPng(const QByteArray &data)
{
    return data.startsWith(kSignature);
}

QByteArray insertText(const QByteArray &png, const QList<QPair<QString, QString>> &entries)
{
    if (!isPng(png) || entries.isEmpty())
        return png;
    // Insert right after IHDR (always the first chunk) so readers that stop at IDAT see the text.
    if (png.size() < 33 || png.mid(12, 4) != "IHDR")
        return png;
    const qsizetype insertAt = 8 + 12 + qFromBigEndian<quint32>(png.constData() + 8);
    QByteArray chunks;
    for (const auto &e : entries) {
        const QByteArray key = e.first.toLatin1().left(79);
        if (isLatin1(e.second)) {
            chunks += chunk("tEXt", key + '\0' + e.second.toLatin1());
        } else {
            // keyword, null, compression flag, compression method, language tag, null, translated keyword, null, text
            QByteArray payload = key;
            payload += '\0';
            payload += '\0';
            payload += '\0';
            payload += '\0';
            payload += '\0';
            payload += e.second.toUtf8();
            chunks += chunk("iTXt", payload);
        }
    }
    QByteArray out = png.left(insertAt);
    out += chunks;
    out += png.mid(insertAt);
    return out;
}

QMap<QString, QString> readText(const QByteArray &png)
{
    QMap<QString, QString> result;
    if (!isPng(png))
        return result;
    qsizetype pos = 8;
    while (pos + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + pos);
        const QByteArray type = png.mid(pos + 4, 4);
        if (pos + 12 + qsizetype(length) > png.size())
            break;
        const QByteArray payload = png.mid(pos + 8, length);
        pos += 12 + length;
        if (type == "IEND")
            break;
        const qsizetype nul = payload.indexOf('\0');
        if (nul <= 0)
            continue;
        const QString key = QString::fromLatin1(payload.left(nul));
        if (type == "tEXt") {
            result.insert(key, QString::fromLatin1(payload.mid(nul + 1)));
        } else if (type == "zTXt") {
            result.insert(key, QString::fromLatin1(inflateZlib(payload.mid(nul + 2))));
        } else if (type == "iTXt") {
            if (payload.size() < nul + 3)
                continue;
            const bool compressed = payload.at(nul + 1) != 0;
            qsizetype p = nul + 3;
            const qsizetype langEnd = payload.indexOf('\0', p);
            if (langEnd < 0)
                continue;
            const qsizetype transEnd = payload.indexOf('\0', langEnd + 1);
            if (transEnd < 0)
                continue;
            const QByteArray text = payload.mid(transEnd + 1);
            result.insert(key, QString::fromUtf8(compressed ? inflateZlib(text) : text));
        }
    }
    return result;
}

} // namespace lumen::png
