#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace lumen::runtime {

// What a model file is for. Determined from its header (tensor names, metadata), not its name.
enum class ModelKind {
    Unknown,
    Checkpoint,     // complete diffusion model (UNet/DiT + text encoders + VAE)
    DiffusionModel, // UNet/DiT only (FLUX, Qwen-Image, … and split SDXL files)
    Lora,
    ControlNet,
    IpAdapter,
    ClipVision,
    Vae,
    TextEncoder,
    Upscaler,
    Embedding,      // textual inversion
    Llm,            // language model (GGUF)
    Projector,      // vision projector of a language model (mmproj)
};
QString modelKindKey(ModelKind kind);     // stable id: "checkpoint", "lora", …
QString modelKindLabel(ModelKind kind);   // translated
ModelKind modelKindFromKey(const QString &key);
QList<ModelKind> allModelKinds();

struct ModelInfo
{
    ModelKind kind = ModelKind::Unknown;
    QString format;          // "gguf", "safetensors", "pickle"
    QString family;          // "SD 1.5", "SDXL", "SD 3", "FLUX" (FLUX.1), "FLUX.2", … or the LLM architecture ("qwen3vl")
    QString variant;         // finer hint: "Illustrious", "Pony" (names); "Dev", "Schnell", "Klein 4B" (FLUX
                             // headers); "Qwen3 4B", "CLIP-L" (text encoders)
    QString precision;       // "Q4_K_M", "F16", "BF16", "FP8", …
    QString title;           // name stored in the file (GGUF general.name), if any
    qint64 parameters = 0;   // weight count
    int contextLength = 0;   // LLMs
    int layers = 0;          // LLMs: blocks
    int kvHeads = 0;         // LLMs: key/value heads
    int headDim = 0;         // LLMs: per-head size
    QStringList triggerWords; // LoRAs (modelspec.trigger_phrase)
    bool unsafeFormat = false; // pickle: can run code when loaded by Python tools
    QString error;           // the header could not be read
    int inspector = 0;       // kModelInspectorVersion of the code that read the header

    // Memory (bytes) needed to run it: weights plus, for LLMs, a KV cache for the context
    // (kvBytesPerValue: 2 = f16, 1.06 = q8_0).
    qint64 estimatedMemory(qint64 fileSize, int context = 16384, double kvBytesPerValue = 1.0625) const;
};

// Reads the header of a .gguf / .safetensors file (blocking; call from a worker thread). Pickle
// formats (.ckpt, .pt, .pth, .bin) are never unpickled: their kind comes from the file and
// folder names.
// Raised when the header inspection learns something new, so the library reads indexed files
// again (2: Wan / Qwen-Image VAEs, text encoder type and size).
// 3: FLUX.2 (and its klein / dev variants) told from FLUX.1, FLUX.1 dev / schnell, CLIP-L / CLIP-G.
constexpr int kModelInspectorVersion = 3;
ModelInfo inspectModel(const QString &path);

// File extensions Lumen treats as models.
QStringList modelFileFilters();

// llama.cpp file types (general.file_type) → "Q4_K_M" etc.
QString ggufFileTypeName(int type);

} // namespace lumen::runtime
