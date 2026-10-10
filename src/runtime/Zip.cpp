#include "runtime/Zip.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <zlib.h>

#include <cstring>
#include <memory>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::Zip)
};

quint16 u16(const uchar *p)
{
    return quint16(p[0] | (p[1] << 8));
}

quint32 u32(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint64 u64(const uchar *p)
{
    return quint64(u32(p)) | (quint64(u32(p + 4)) << 32);
}

constexpr quint32 kEocd = 0x06054b50;
constexpr quint32 kZip64Locator = 0x07064b50;
constexpr quint32 kZip64Eocd = 0x06064b50;
constexpr quint32 kCentral = 0x02014b50;
constexpr quint32 kLocal = 0x04034b50;

bool readAt(QFile &f, quint64 offset, qint64 size, QByteArray *out)
{
    if (!f.seek(qint64(offset)))
        return false;
    *out = f.read(size);
    return out->size() == size;
}

} // namespace

QString listZip(const QString &zipPath, QList<ZipEntry> *entries)
{
    entries->clear();
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly))
        return Text::tr("Cannot open %1: %2").arg(QDir::toNativeSeparators(zipPath), f.errorString());
    const quint64 fileSize = quint64(f.size());
    if (fileSize < 22)
        return Text::tr("Not a zip archive.");

    // End of central directory: the last 22 bytes plus up to 64 KiB of comment.
    const quint64 tailSize = std::min<quint64>(fileSize, 22 + 65535);
    QByteArray tail;
    if (!readAt(f, fileSize - tailSize, qint64(tailSize), &tail))
        return Text::tr("Cannot read the archive.");
    qint64 eocd = -1;
    for (qint64 i = tail.size() - 22; i >= 0; --i) {
        if (u32(reinterpret_cast<const uchar *>(tail.constData()) + i) == kEocd) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return Text::tr("Not a zip archive (no central directory).");
    const auto *e = reinterpret_cast<const uchar *>(tail.constData()) + eocd;
    quint64 count = u16(e + 10);
    quint64 cdSize = u32(e + 12);
    quint64 cdOffset = u32(e + 16);
    const quint64 eocdPos = fileSize - tailSize + quint64(eocd);
    if ((count == 0xFFFF || cdSize == 0xFFFFFFFF || cdOffset == 0xFFFFFFFF) && eocdPos >= 20) {
        QByteArray loc;
        if (!readAt(f, eocdPos - 20, 20, &loc) || u32(reinterpret_cast<const uchar *>(loc.constData())) != kZip64Locator)
            return Text::tr("Damaged Zip64 archive.");
        const quint64 z64Pos = u64(reinterpret_cast<const uchar *>(loc.constData()) + 8);
        QByteArray rec;
        if (!readAt(f, z64Pos, 56, &rec) || u32(reinterpret_cast<const uchar *>(rec.constData())) != kZip64Eocd)
            return Text::tr("Damaged Zip64 archive.");
        const auto *r = reinterpret_cast<const uchar *>(rec.constData());
        count = u64(r + 32);
        cdSize = u64(r + 40);
        cdOffset = u64(r + 48);
    }
    if (cdOffset + cdSize > fileSize || cdSize > 512ull * 1024 * 1024)
        return Text::tr("Damaged archive (central directory out of range).");
    QByteArray cd;
    if (!readAt(f, cdOffset, qint64(cdSize), &cd))
        return Text::tr("Cannot read the central directory.");

    const auto *p = reinterpret_cast<const uchar *>(cd.constData());
    const auto *end = p + cd.size();
    for (quint64 i = 0; i < count; ++i) {
        if (end - p < 46 || u32(p) != kCentral)
            return Text::tr("Damaged archive (bad central directory entry).");
        ZipEntry entry;
        const quint16 madeBy = u16(p + 4);
        const quint16 flags = u16(p + 8);
        entry.method = u16(p + 10);
        entry.crc32 = u32(p + 16);
        entry.compressedSize = u32(p + 20);
        entry.size = u32(p + 24);
        const quint16 nameLen = u16(p + 28);
        const quint16 extraLen = u16(p + 30);
        const quint16 commentLen = u16(p + 32);
        const quint32 externalAttr = u32(p + 38);
        entry.localHeaderOffset = u32(p + 42);
        if (end - p < 46 + nameLen + extraLen + commentLen)
            return Text::tr("Damaged archive (truncated entry).");
        const QByteArray rawName(reinterpret_cast<const char *>(p + 46), nameLen);
        // Bit 11: UTF-8 names. Older archivers use code page 437; Latin-1 keeps ASCII names intact.
        entry.name = (flags & 0x0800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        entry.name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        entry.directory = entry.name.endsWith(QLatin1Char('/'));
        entry.encrypted = flags & 0x0001;
        if ((madeBy >> 8) == 3) // Unix: the mode lives in the high 16 bits
            entry.symlink = ((externalAttr >> 16) & 0xF000) == 0xA000;
        // Zip64 extended information: present values replace the 0xFFFFFFFF placeholders, in order.
        const uchar *x = p + 46 + nameLen;
        const uchar *xEnd = x + extraLen;
        while (xEnd - x >= 4) {
            const quint16 id = u16(x);
            const quint16 len = u16(x + 2);
            if (xEnd - x - 4 < len)
                break;
            if (id == 0x0001) {
                const uchar *v = x + 4;
                const uchar *vEnd = v + len;
                if (entry.size == 0xFFFFFFFF && vEnd - v >= 8) {
                    entry.size = u64(v);
                    v += 8;
                }
                if (entry.compressedSize == 0xFFFFFFFF && vEnd - v >= 8) {
                    entry.compressedSize = u64(v);
                    v += 8;
                }
                if (entry.localHeaderOffset == 0xFFFFFFFF && vEnd - v >= 8)
                    entry.localHeaderOffset = u64(v);
            }
            x += 4 + len;
        }
        entries->append(entry);
        p += 46 + nameLen + extraLen + commentLen;
    }
    return {};
}

QString safeEntryPath(const QString &name)
{
    QString n = name;
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (n.startsWith(QLatin1Char('/')) || n.contains(QLatin1Char(':')) || n.contains(QChar(0)))
        return {};
    static const QSet<QString> reserved = {
        QStringLiteral("con"),  QStringLiteral("prn"),  QStringLiteral("aux"),  QStringLiteral("nul"),
        QStringLiteral("com1"), QStringLiteral("com2"), QStringLiteral("com3"), QStringLiteral("com4"),
        QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"), QStringLiteral("lpt4")};
    QStringList parts;
    for (const QString &part : n.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("."))
            continue;
        if (part == QLatin1String("..") || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return {};
        if (reserved.contains(part.section(QLatin1Char('.'), 0, 0).toLower()))
            return {};
        parts << part;
    }
    return parts.join(QLatin1Char('/'));
}

QString extractZip(const QString &zipPath, const QString &destDir, const std::function<bool(qint64, qint64)> &progress)
{
    QList<ZipEntry> entries;
    if (QString err = listZip(zipPath, &entries); !err.isEmpty())
        return err;

    // Validate everything before writing a single byte.
    qint64 total = 0;
    QList<QString> targets;
    for (const ZipEntry &e : std::as_const(entries)) {
        const QString rel = safeEntryPath(e.name);
        if (rel.isEmpty() && !e.directory)
            return Text::tr("The archive contains an unsafe path: %1").arg(e.name);
        if (e.symlink)
            return Text::tr("The archive contains a symbolic link: %1").arg(e.name);
        if (e.encrypted)
            return Text::tr("The archive is encrypted.");
        if (!e.directory && e.method != 0 && e.method != 8)
            return Text::tr("Unsupported compression method %1 for %2.").arg(e.method).arg(e.name);
        targets << rel;
        total += qint64(e.size);
    }

    QFile zip(zipPath);
    if (!zip.open(QIODevice::ReadOnly))
        return Text::tr("Cannot open %1: %2").arg(QDir::toNativeSeparators(zipPath), zip.errorString());
    QDir root(destDir);
    if (!root.mkpath(QStringLiteral(".")))
        return Text::tr("Cannot create %1.").arg(QDir::toNativeSeparators(destDir));
    const QString rootPath = QFileInfo(root.absolutePath()).canonicalFilePath() + QLatin1Char('/');

    constexpr qint64 kChunk = 1 << 18;
    QByteArray in(kChunk, Qt::Uninitialized);
    QByteArray out(kChunk, Qt::Uninitialized);
    qint64 done = 0;
    for (int i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        const QString rel = targets[i];
        if (rel.isEmpty())
            continue; // "./" style directory entries
        const QString target = root.absoluteFilePath(rel);
        if (e.directory) {
            if (!root.mkpath(rel))
                return Text::tr("Cannot create %1.").arg(QDir::toNativeSeparators(target));
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return Text::tr("Cannot create %1.").arg(QDir::toNativeSeparators(QFileInfo(target).absolutePath()));
        // Belt and braces: the parent must resolve inside the destination (no junction tricks).
        const QString parent = QFileInfo(QFileInfo(target).absolutePath()).canonicalFilePath() + QLatin1Char('/');
        if (!parent.startsWith(rootPath, Qt::CaseInsensitive))
            return Text::tr("The archive contains an unsafe path: %1").arg(e.name);

        QByteArray local;
        if (!readAt(zip, e.localHeaderOffset, 30, &local) || u32(reinterpret_cast<const uchar *>(local.constData())) != kLocal)
            return Text::tr("Damaged archive (bad local header for %1).").arg(e.name);
        const auto *lh = reinterpret_cast<const uchar *>(local.constData());
        const quint64 dataOffset = e.localHeaderOffset + 30 + u16(lh + 26) + u16(lh + 28);
        if (!zip.seek(qint64(dataOffset)))
            return Text::tr("Damaged archive (%1).").arg(e.name);

        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return Text::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(target), file.errorString());

        uLong crc = crc32(0L, Z_NULL, 0);
        quint64 remaining = e.compressedSize;
        quint64 written = 0;
        z_stream zs;
        std::memset(&zs, 0, sizeof(zs));
        std::unique_ptr<z_stream, void (*)(z_stream *)> inflater(nullptr, [](z_stream *s) { inflateEnd(s); });
        if (e.method == 8) {
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
                return Text::tr("Cannot initialise decompression.");
            inflater.reset(&zs);
        }
        bool streamEnd = e.method == 0;
        while (remaining > 0 || (e.method == 8 && !streamEnd)) {
            const qint64 want = qint64(std::min<quint64>(remaining, quint64(kChunk)));
            const qint64 got = want > 0 ? zip.read(in.data(), want) : 0;
            if (got < 0 || (want > 0 && got == 0))
                return Text::tr("Damaged archive (unexpected end in %1).").arg(e.name);
            remaining -= quint64(got);
            if (e.method == 0) {
                if (file.write(in.constData(), got) != got)
                    return Text::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(target), file.errorString());
                crc = crc32(crc, reinterpret_cast<const Bytef *>(in.constData()), uInt(got));
                written += quint64(got);
                done += got;
            } else {
                zs.next_in = reinterpret_cast<Bytef *>(in.data());
                zs.avail_in = uInt(got);
                for (;;) {
                    zs.next_out = reinterpret_cast<Bytef *>(out.data());
                    zs.avail_out = uInt(out.size());
                    const int rc = inflate(&zs, Z_NO_FLUSH);
                    // Z_BUF_ERROR only means "no progress possible": more input is needed.
                    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
                        return Text::tr("Damaged archive (cannot decompress %1).").arg(e.name);
                    const qint64 produced = out.size() - qint64(zs.avail_out);
                    if (produced > 0) {
                        if (file.write(out.constData(), produced) != produced)
                            return Text::tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(target), file.errorString());
                        crc = crc32(crc, reinterpret_cast<const Bytef *>(out.constData()), uInt(produced));
                        written += quint64(produced);
                        done += produced;
                    }
                    if (rc == Z_STREAM_END) {
                        streamEnd = true;
                        break;
                    }
                    if (zs.avail_out > 0 || rc == Z_BUF_ERROR) {
                        // Input used up. With nothing left to read the stream is cut short.
                        if (remaining == 0 && zs.avail_in == 0 && produced == 0)
                            return Text::tr("Damaged archive (truncated data in %1).").arg(e.name);
                        if (zs.avail_in == 0)
                            break;
                    }
                }
            }
            if (written > e.size)
                return Text::tr("Damaged archive (%1 is larger than declared).").arg(e.name);
            if (progress && !progress(done, total)) {
                file.close();
                file.remove();
                return Text::tr("Cancelled.");
            }
        }
        file.close();
        if (written != e.size || quint32(crc) != e.crc32) {
            QFile::remove(target);
            return Text::tr("Damaged archive (checksum mismatch in %1).").arg(e.name);
        }
    }
    if (progress)
        progress(total, total);
    return {};
}

} // namespace lumen::runtime
