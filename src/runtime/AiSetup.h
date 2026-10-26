#pragma once

#include "runtime/Hardware.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ModelPresets.h"

#include <QFuture>
#include <QHash>

namespace lumen::runtime {

// ---- Setting up AI on this computer (plan M17 §17.5) ----
// The setup wizard's "Run on this computer": the model that suits the graphics card, which of its
// files are on this computer already (in the library or in the folders it suggests: ComfyUI,
// WebUI, LM Studio, Downloads) and what is left to download.

// The graphics card image models run on: the discrete NVIDIA or AMD GPU with the most memory.
// Null without one (Intel graphics and integrated GPUs are too slow for these models).
const GpuInfo *imageGpu(const HardwareInfo &hw);

// From this much GPU memory on, FLUX.2 [klein] 4B is suggested. 8 GB cards report a little less
// than 8 GB, and the 8 GB tier's options (q8_0 weights, weights in RAM) are what makes it fit.
inline constexpr double kFullModelVramGb = 7.0;

struct LocalRecommendation
{
    enum class Fit {
        Full,  // 8 GB or more: FLUX.2 [klein] 4B, which makes new images and edits them from words
        Light, // a smaller NVIDIA or AMD GPU: an SDXL model (new images and inpainting)
        NoGpu  // no NVIDIA or AMD GPU: an online service is the better choice
    };
    Fit fit = Fit::NoGpu;
    QString presetId;        // the model preset to set up: "flux2-klein-4b" or "sdxl"
    QString alternativeId;   // the other one, offered as the second choice
    VramTier tier = VramTier::Small;
    // A language model fits next to the image model (more than 8 GB). The wizard offers one only
    // when a plugin needs it; the editor works without.
    bool languageModel = false;
};
LocalRecommendation recommendLocalModel(const HardwareInfo &hw);

// A folder with models that is not in the library (ModelLibrary::suggestedRoots), and its files.
struct FoundFolder
{
    QString path;
    QList<ModelEntry> entries;
};
// Reads the folders on a worker thread (ModelLibrary::readFolder); folders that do not exist give
// no entry.
QFuture<QList<FoundFolder>> readFolders(const QStringList &folders);

// One file a model needs: where it is on this computer, or what to download.
struct PlannedFile
{
    QString option;          // its sd.cpp option: "diffusion_model" or "model", then "vae", "llm"…
    QString label;           // "FLUX.2 [klein] 4B", "FLUX.2 VAE"
    ModelKind kind = ModelKind::Unknown;
    bool required = true;
    QString path;            // on this computer; empty = to download
    QString folder;          // a found folder it is in, which the setup adds to the library
    PresetFile file;         // the pinned download (invalid when there is none)

    bool missing() const { return path.isEmpty(); }
};

struct LocalModelPlan
{
    ModelPreset preset;
    QList<PlannedFile> files; // the model first, then its components
    QString error;            // no model file here and none to download

    bool ok() const { return error.isEmpty() && !files.isEmpty(); }
    const PlannedFile *model() const { return files.isEmpty() ? nullptr : &files.first(); }
    // Missing files that can be downloaded, and their total size.
    QList<PlannedFile> downloads() const;
    qint64 downloadSize() const;
    // Found folders the plan takes files from (added to the library when the setup runs).
    QStringList foldersToAdd() const;
};

// What setting up `preset` takes. A model file the preset matches is taken from the library or the
// found folders, preferring one whose components are there too (then one named like the pinned
// download); without one, the preset's pinned download. Each component comes from the library or
// the found folders when a file there fits its slot (the pinned file, else one of its header
// family and variant), else from its pinned download.
LocalModelPlan planLocalModel(const ModelLibrary *library, const QList<FoundFolder> &found, const ModelPreset &preset);

// The download of a planned file into Lumen's models folder (sub-folder by kind), checked against
// its pinned size and SHA-256.
HubDownload plannedDownload(const LocalModelPlan &plan, const PlannedFile &file,
                            const QString &huggingFaceBase = QStringLiteral("https://huggingface.co"));

// sd.cpp server options once every file is there (`paths`: option → file, planned or downloaded):
// the GPU tier's options, the model in its slot, its components, the preset's launch options.
QVariantMap planServerOptions(const ModelLibrary *library, const LocalModelPlan &plan, const QHash<QString, QString> &paths, VramTier tier);

} // namespace lumen::runtime
