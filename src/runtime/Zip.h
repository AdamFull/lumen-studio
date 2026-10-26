#pragma once

#include <QList>
#include <QString>

#include <functional>

namespace lumen::runtime {

struct ZipEntry
{
    QString name;          // as stored, '/' separated
    quint64 size = 0;      // uncompressed
    quint64 compressedSize = 0;
    quint64 localHeaderOffset = 0;
    quint32 crc32 = 0;
    quint16 method = 0;    // 0 = stored, 8 = deflate
    bool directory = false;
    bool encrypted = false;
    bool symlink = false;
};

// Reads the central directory (Zip64 aware). Returns an error message, or an empty string.
QString listZip(const QString &zipPath, QList<ZipEntry> *entries);

// Normalises an entry name to a safe relative path ('/' separated). Returns an empty string for
// names that could escape the destination: absolute paths, drive letters or streams (':'),
// ".." components and reserved Windows device names.
QString safeEntryPath(const QString &name);

// Extracts every entry below destDir, streaming (no entry is held in memory), verifying CRC-32.
// Refuses archives with unsafe names, symbolic links or encryption before writing anything.
// progress(doneBytes, totalBytes) may return false to cancel. With `wanted`, only the files whose
// safe relative path it accepts are written (e.g. two DLLs of a runtime's archive, not its debug
// symbols). Returns an error, or "" on success. Safe to call from a worker thread.
QString extractZip(const QString &zipPath, const QString &destDir,
                   const std::function<bool(qint64 done, qint64 total)> &progress = {},
                   const std::function<bool(const QString &path)> &wanted = {});

// Writes a zip of the files below `sourceDir` (relative names, '/' separated), stored without
// compression (images are compressed already). No Zip64: at most 4 GB and 65535 entries.
// Returns an error message, or "" on success. Safe to call from a worker thread.
QString writeZip(const QString &zipPath, const QString &sourceDir);

} // namespace lumen::runtime
