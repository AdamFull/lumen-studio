#pragma once

#include "core/Psd.h"
#include "doc/Layer.h"

#include <QStringList>

// Conversion between layered PSD files and editor layer stacks. Thread-safe (worker threads).
namespace lumen::doc {

struct PsdImport
{
    QSize size;
    QList<LayerPtr> layers; // bottom to top, document-sized
    QImage composite;       // the file's merged image, or the flattened layers
    QStringList warnings;   // features that were approximated (clipping, masks, unknown blend modes)
};

// Throws StoreError when the file cannot be read.
PsdImport importPsd(const QString &path);
PsdImport importPsd(const psd::File &file);

// Layer stack (bottom to top) to a PSD file with names, visibility, opacity and blend modes.
psd::File exportPsd(const DocumentSnapshot &layers, const QSize &size);

BlendMode blendModeFromPsdKey(const QByteArray &key, bool *known = nullptr);
QByteArray psdKeyFromBlendMode(BlendMode mode);

} // namespace lumen::doc
