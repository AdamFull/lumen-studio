#pragma once

#include "runtime/ModelFormats.h"

#include <QFuture>
#include <QList>
#include <QString>

namespace lumen::ai {
class ImageProvider;
}

namespace lumen::ui {

class Studio;

// A LoRA an image engine can load, and what it was made for (plan M18 §18.3).
struct LoraChoice
{
    QString name;              // what the server calls it (the file name without extension)
    QString path;              // relative to the server's LoRA folder
    QString file;              // on this computer, when Lumen knows the folder
    runtime::ModelInfo info;   // from the file's header; unknown for a server elsewhere
    QString mismatch;          // why it does not fit the engine's model; empty = fits or cannot tell
};

struct LoraListing
{
    QList<LoraChoice> loras;   // those that fit first, then by name
    QString problem;           // why there are none to choose: no LoRA folder, the engine takes none…
    QString model;             // the engine's model as people name it ("FLUX.2 [klein] 4B"), if known
    QString profileId;         // the server Lumen runs for the engine, if it does
};

// The LoRAs `provider` can use. For a server Lumen runs, its LoRA folder is read here (on a worker
// thread, headers only), so listing never starts the server; a server elsewhere is asked.
QFuture<LoraListing> listLoras(Studio *studio, ai::ImageProvider *provider);

// The LoRAs of `loras` that fit and whose name or trigger words match `words` (plan M18 §18.3), best
// first, at most `max`: "a watercolor fox" suggests "watercolor_style_v2". They are offered with a
// tick box, never used without one.
QList<LoraChoice> suggestedLoras(const QList<LoraChoice> &loras, const QString &words, int max = 3);

// The LoRA files of a folder (what sd-server lists from its --lora-model-dir), with their headers.
// Blocking; for a worker thread.
QList<LoraChoice> lorasInFolder(const QString &folder, const QString &family, const QString &variant);

} // namespace lumen::ui
