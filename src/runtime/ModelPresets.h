#pragma once

#include "runtime/ModelFormats.h"
#include "runtime/ModelHub.h"
#include "runtime/ServerProfile.h"

#include <QMap>
#include <QUrl>
#include <QVariantMap>

#include <optional>

namespace lumen::runtime {

class ModelLibrary;

// ---- Model presets (plan M17 §17.3) ----
// What a model family needs and how to run it, as data (resources/presets/models/*.json, one file
// per family): the files its model files never carry (VAE, text encoders), pinned to a Hugging
// Face commit with their SHA-256; which add-ons fit; sampling defaults; launch options per GPU size.
// Choosing a model fills every component slot from the library, so a model cannot be started
// with the wrong slots or without its text encoder.

// A file a preset needs, pinned to a commit.
struct PresetFile
{
    QString id;              // key in its family file
    QString repo;            // "Comfy-Org/flux2-klein-4B"
    QString revision;        // commit
    QString path;            // in the repository
    qint64 size = 0;
    QString sha256;
    QString license;

    // The download address on huggingface.co (or a test server).
    QUrl url(const QString &huggingFaceBase = QStringLiteral("https://huggingface.co")) const;
    QString fileName() const;
    bool isValid() const { return !repo.isEmpty(); }
};

// A component: a server option the model needs a file for.
struct PresetSlot
{
    QString option;          // sd.cpp server option: "vae", "clip_l", "clip_g", "t5xxl", "llm"
    ModelKind kind = ModelKind::Unknown; // Vae or TextEncoder
    QString family;          // ModelInfo::family a library file must have ("FLUX.2", "LLM", "CLIP", "T5")
    QString variant;         // and its variant, if set ("Qwen3 4B", "CLIP-L")
    QString label;           // what it is, for messages ("Qwen3 4B text encoder")
    bool required = true;
    PresetFile file;

    HubFile hubFile() const; // the pinned download as a HubFile (kind, url, size, sha256, required)
};

struct ModelPreset
{
    QString id;              // "flux2-klein-4b"
    QString name;            // "FLUX.2 [klein] 4B"
    QString familyFile;      // the file it came from ("flux2")
    QUrl docs;               // the sd.cpp page it follows
    QString note;            // a hint for the user (shown with the model)
    // Its model type (ModelType below): a type its family file names ("types"), else its own id
    // and name.
    QString type;
    QString typeName;

    // Matching a model file: its header family (and variant), kind, and words in its file name.
    QString family;
    QString variant;
    QList<ModelKind> kinds;  // Checkpoint and/or DiffusionModel
    QStringList nameAny;     // the file name contains one of these (lower case); empty = any
    QStringList nameNone;    // and none of these

    QList<PresetSlot> components;
    // A checkpoint carries its own VAE; a separate one of this header family may replace it
    // ("SD / SDXL"). Empty when the model takes no other VAE than its component.
    QString optionalVae;
    // The model file to download when the library has none (the setup wizard, §17.5), pinned like
    // the components; GGUF where sd.cpp's docs list one. Invalid for presets without one.
    PresetFile download;

    // What it can do.
    bool generate = true;
    bool edit = false;       // instruction edits with reference images
    bool inpaint = true;
    int references = 0;      // reference images for edits
    bool negativePrompt = true;
    bool loras = true;
    QStringList controlNetFamilies; // ControlNet / IP-Adapter families that fit; empty = none do
    QStringList ipAdapterFamilies;

    // Sampling defaults; 0 / empty = the server's.
    double cfg = 0;
    double guidance = 0;     // distilled guidance (FLUX.1 dev)
    double imageCfg = 0;
    double flowShift = 0;
    int steps = 0;
    QString sampler;
    QString scheduler;
    int nativeResolution = 0;
    QString dialect;         // "natural" or "tags"

    // Launch options per GPU size, applied over presetOptions("sd.cpp", tier).
    QMap<VramTier, QVariantMap> vram;
    QVariantMap serverOptions; // always (e.g. "extra_args")

    bool matches(const ModelInfo &info, const QString &path) const;
    const PresetSlot *slot(const QString &option) const;
};

// The bundled presets, in matching order (the first that matches a file is its preset).
const QList<ModelPreset> &modelPresets();
// Parses one family file; on a problem returns nothing and names it in `error`.
QList<ModelPreset> parseModelPresets(const QByteArray &json, const QString &familyFile, QString *error = nullptr);
const ModelPreset *presetFor(const ModelInfo &info, const QString &path);
const ModelPreset *presetById(const QString &id);

// ---- Model types (plan M17 §17.5) ----
// What the server dialog asks for first: presets that need the same fields and the same kind of
// files, such as FLUX.1 dev, schnell and Kontext, or SDXL with Illustrious and Pony. A family file
// names its types ("types": {id: name}) and each preset says which one it belongs to ("type"); a
// preset without one is a type of its own, so a new preset file adds a type without code changes.
struct ModelType
{
    QString id;              // "flux1"
    QString name;            // "FLUX.1 (dev, schnell, Kontext; separate files)"
    QList<const ModelPreset *> presets; // in matching order
    QList<ModelKind> kinds;  // what its model files are: Checkpoint and / or DiffusionModel
    QList<PresetSlot> components; // the files its model files do not carry (its first preset's)
    QString optionalVae;     // a VAE family that may replace a checkpoint's own
    QStringList controlNetFamilies; // ControlNet / IP-Adapter families that fit any of its presets
    QStringList ipAdapterFamilies;
    bool loras = false;

    // Where a model file of this type goes: "model" for a checkpoint (when the type takes
    // checkpoints), else "diffusion_model" when it takes diffusion models, else "model".
    QString modelOption(const std::optional<ModelInfo> &file = std::nullopt) const;
    const PresetSlot *slot(const QString &option) const;
    // The sd.cpp file options a server of this type uses, in the server dialog's order: the
    // model's (`modelOption`), its components or an optional VAE, the add-ons it can use (a
    // ControlNet, an IP-Adapter with its CLIP vision encoder, the LoRA folder) and the upscaler
    // folder.
    QStringList fileOptions(const QString &modelOption) const;
    bool contains(const ModelPreset &preset) const;
};

// The types of the bundled presets, by name.
const QList<ModelType> &modelTypes();
const ModelType *modelTypeById(const QString &id);
const ModelType *modelTypeOf(const ModelPreset &preset);
// Every file option of an sd.cpp server (the fields of a model Lumen does not know), in the
// server dialog's order.
QStringList sdFileOptions();
// The type a server profile has: its model's, or none when the model is unknown or missing, or
// the profile sets a file option the type has no place for (a model in the other slot, an SDXL
// ControlNet with Anima).
const ModelType *profileModelType(const ModelLibrary *library, const ServerProfile &profile);
// The label of an sd.cpp file field for a type: "Text encoder (Qwen3 4B)", "VAE (optional)" for
// a checkpoint's own; the server form's label otherwise (`type` may be null).
QString fileFieldLabel(const QString &option, const ModelType *type);
// The pinned file with this SHA-256 in any preset, and the preset (for its licence and source).
const PresetSlot *presetSlotForSha(const QString &sha256);

// What a model file is, from the library or (when it is not in it yet) from its header.
std::optional<ModelInfo> modelInfoOf(const ModelLibrary *library, const QString &path);
// The sd.cpp option a model file goes in: "model" (a complete checkpoint) or "diffusion_model";
// empty for other kinds.
QString mainModelOption(const ModelInfo &info);
// The library file that fills a slot: the pinned file itself, else one with the slot's header
// family and variant (preferring one named like the pinned file). Empty when there is none.
QString slotStandIn(const ModelLibrary *library, const PresetSlot &slot);

// sd.cpp server options for using a model file: the file in its slot ("model" or
// "diffusion_model"; the other slot "" = cleared), the components found in the library, and the
// preset's launch options for the GPU size. Empty for files that are not image models.
QVariantMap modelServerOptions(const ModelLibrary *library, const QString &path, std::optional<VramTier> tier = std::nullopt);

// What keeps an sd.cpp server profile from starting or working, each with the option it is about:
// a model in the wrong slot, a missing or mismatched component, a ControlNet / IP-Adapter of
// another family, a file that is gone. Empty when nothing is known to be wrong.
struct ProfileProblem
{
    QString option;
    QString message;
    bool missing = false;    // a component that is not there yet: downloading it fixes the profile
    // The same in a few words, for next to the option's field ("This is a T5 text encoder; FLUX.2
    // [klein] 4B needs a Qwen3 4B text encoder."): the field shows the file.
    QString brief;
};
QList<ProfileProblem> checkSdProfile(const ModelLibrary *library, const ServerProfile &profile);
// Whether a ControlNet or IP-Adapter file fits a preset.
bool addonFits(const ModelPreset &preset, const ModelInfo &addon);

} // namespace lumen::runtime
