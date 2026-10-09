#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>

// PNG text chunk utilities. Qt's PNG writer normalises whitespace in text values, which breaks the
// multi-line "parameters" metadata used by Stable Diffusion tools, so chunks are written directly.
namespace lumen::png {

// Inserts tEXt (Latin-1) or iTXt (UTF-8) chunks right after IHDR. Returns the input unchanged if it is not a PNG.
QByteArray insertText(const QByteArray &png, const QList<QPair<QString, QString>> &entries);

// Reads tEXt, zTXt and iTXt chunks (uncompressed and zlib-compressed).
QMap<QString, QString> readText(const QByteArray &png);

bool isPng(const QByteArray &data);

} // namespace lumen::png
