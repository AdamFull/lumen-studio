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
    // What Lumen made do with or does not keep (clipping, masks, unknown blend modes, text, effects,
    // adjustment layers, smart objects, the groups' own opacity, the colour profile), in plain words.
    QStringList warnings;
};

// Throws StoreError when the file cannot be read.
PsdImport importPsd(const QString &path);
PsdImport importPsd(const psd::File &file);

// Layer stack (bottom to top) to a PSD file with names, visibility, opacity, blend modes and the
// groups the layers came from (LayerSnapshot::group).
psd::File exportPsd(const DocumentSnapshot &layers, const QSize &size);

BlendMode blendModeFromPsdKey(const QByteArray &key, bool *known = nullptr);
QByteArray psdKeyFromBlendMode(BlendMode mode);

} // namespace lumen::doc
