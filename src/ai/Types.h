#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

#include "core/Error.h"

// Provider-neutral request/response model shared by all AI back-ends.
namespace lumen::ai {

LUMEN_DECLARE_ERROR(ProviderError);

// ---- Image generation ---------------------------------------------------------------------------

enum class GenMode {
    TextToImage,
    ImageToImage, // init image + denoise
    Inpaint,      // init image + mask (white = regenerate)
    Edit          // instruction-based edit of the init image (no mask needed)
};
QString genModeName(GenMode m);
GenMode genModeFromName(const QString &name);

enum class ReferenceRole {
    Character, // identity / appearance to reproduce
    Style,     // art style to follow
    Composition,
    Other
};
QString referenceRoleName(ReferenceRole r);
ReferenceRole referenceRoleFromName(const QString &name);

struct ReferenceImage
{
    QImage image;
    ReferenceRole role = ReferenceRole::Character;
    double weight = 0.8;
    QString assetId;
    QString label;
};

enum class ControlType { OpenPose, Depth, Canny, Lineart, Scribble };
QString controlTypeName(ControlType t);

struct ControlInput
{
    ControlType type = ControlType::OpenPose;
    QImage image;          // already preprocessed control map (e.g. rendered OpenPose skeleton)
    double weight = 1.0;
    double start = 0.0;
    double end = 1.0;
};

// A LoRA applied to a generation. Servers load LoRAs from their own LoRA folder; the provider
// finds the file there by its path or name (resolveLoras) and fills in `resolved`.
struct LoraRef
{
    QString name;           // file name without extension ("akane_v2")
    QString file;           // full path on this computer, when known
    double weight = 0.8;
    QString resolved;       // what the server calls it (relative path or name); set by the provider

    QJsonObject toJson() const;
    static LoraRef fromJson(const QJsonObject &o);
};

// A LoRA a server offers: its name (file stem) and path relative to the server's LoRA folder.
struct ServerLora
{
    QString name;
    QString path;
};
// Finds each requested LoRA among the server's: by the file's path (the server's relative path is
// a suffix of it), else by name. Fills `resolved` with the server's path; returns false and sets
// *error when one is missing.
bool resolveLoras(QList<LoraRef> &loras, const QList<ServerLora> &available, QString *error);

// A character's LoRA (M13): the file, the trigger word that calls the character up, the weight
// that works best and the base model family it was trained for ("SDXL", "Illustrious", …).
struct CharacterLora
{
    QString file;
    QString trigger;
    double weight = 0.8;
    QString family;

    bool isValid() const { return !file.isEmpty(); }
    LoraRef ref() const;
    QJsonObject toJson() const;
    static CharacterLora fromJson(const QJsonObject &o);
};

struct ImageRequest
{
    GenMode mode = GenMode::TextToImage;
    QString prompt;
    QString negativePrompt;
    QSize size{1024, 1024};
    qint64 seed = -1;
    int steps = 28;
    double cfg = 6.0;
    // The user chose `cfg` (no page sets it since the Generate page went); otherwise the model's
    // recommended value applies (PromptStyle::cfg, set by GenerationService).
    bool cfgChosen = false;
    // Likewise for `steps`: otherwise the model preset's (PromptStyle::steps; FLUX.2 klein takes 4).
    bool stepsChosen = false;
    QString sampler;
    QString scheduler;
    // Settings only some models take (plan M18 §18.4); 0 = the model's (its preset, else the
    // server's): FLUX.1 dev's distilled guidance, an edit model's image guidance, the flow shift of
    // flow models (Qwen-Image), and how many of the text encoder's last layers to skip (SD 1.5, SDXL).
    double guidance = 0;
    double imageCfg = 0;
    double flowShift = 0;
    int clipSkip = 0;
    QString model;
    int batchCount = 1;

    QImage initImage;
    QImage mask;            // Grayscale8, same size as initImage
    double denoise = 0.6;
    int maskBlur = 4;

    QList<ReferenceImage> references;
    QList<ControlInput> controls;
    QList<LoraRef> loras;

    bool transparentBackground = false;
    QString quality;        // provider-specific quality tier (e.g. OpenAI "high")
    QJsonObject extras;     // provider-specific overrides, merged into the payload last

    // Serialisable description (no pixels) for metadata / reproducibility.
    QJsonObject describe() const;
};

struct GeneratedImage
{
    QImage image;
    qint64 seed = -1;
    QString parameters;     // A1111-style info text
    QJsonObject info;
};

struct ImageResult
{
    QList<GeneratedImage> images;
    QJsonObject info;
    QString text;           // optional text returned alongside images (multimodal models)
};

struct ImageCapabilities
{
    bool textToImage = true;
    bool imageToImage = false;
    bool inpaint = false;          // native masked inpainting
    bool edit = false;             // instruction edit of an input image
    // A selection is edited as an instruction edit of its crop (then blended in through the mask)
    // rather than inpainted: local edit models (FLUX.2 klein, Kontext, Qwen-Image-Edit) keep the
    // rest of the crop as it is, where inpainting would repaint the masked area from noise.
    bool regionEdit = false;
    int maxReferences = 0;         // reference images usable for identity/style
    bool poseControl = false;      // ControlNet-style pose conditioning
    bool negativePrompt = false;
    bool seed = false;
    bool steps = false;
    bool cfg = false;
    bool samplers = false;
    bool batch = true;
    bool transparentBackground = false;
    bool qualityTiers = false;
    bool loras = false;            // LoRA files from the server's LoRA folder
    int sizeMultiple = 8;
    QList<QSize> fixedSizes;       // when non-empty only these sizes are accepted
    QStringList aspectRatios;      // providers that take aspect ratio instead of size
    qint64 nativePixels = 1024 * 1024; // preferred working resolution (pixel count)
    qint64 maxPixels = 2048 * 2048;
};

// SegmentParts splits a character illustration into semantic parts.
//   params: {"parts": [{"tag": "front hair", "prompt": "bangs"}, ...] (empty = everything the model knows),
//            "resolution": int (optional), "seed": int (optional)}
//   result: one GeneratedImage per part, sized like the input image. info: {"label": tag or model label,
//           "kind": "layer" (RGBA, possibly with completed hidden areas) | "mask" (Grayscale8),
//           "order": back-to-front index (optional), "depth": 0 back .. 1 front (optional), "score": 0..1 (optional)}
enum class Utility { Upscale, RemoveBackground, DetectPose, Caption, SegmentParts };
QString utilityName(Utility u);

// ---- Chat / LLM ---------------------------------------------------------------------------------

enum class ChatRole { System, User, Assistant, Tool };

struct ChatImage
{
    QImage image;
    QString note; // optional label used in text-only fallbacks
};

struct ToolCall
{
    QString id;
    QString name;
    QJsonObject arguments;
    QString rawArguments;
};

struct ChatMessage
{
    ChatRole role = ChatRole::User;
    QString text;
    QList<ChatImage> images;
    QList<ToolCall> toolCalls; // assistant messages
    QString toolCallId;        // tool result messages
    QString toolName;
    // Provider-native assistant content (e.g. Anthropic content blocks incl. thinking) that must be
    // echoed back unchanged on the next turn. Used only when talking to the same provider type.
    QJsonArray providerContent;
    QString providerType;

    static ChatMessage system(const QString &text);
    static ChatMessage user(const QString &text, const QList<QImage> &images = {});
    static ChatMessage assistant(const QString &text);
    static ChatMessage toolResult(const QString &callId, const QString &name, const QString &content);
    // Assistant message reproducing a provider result (text, tool calls, native content).
    static ChatMessage fromResult(const struct ChatResult &result);
};

struct ToolDefinition
{
    QString name;
    QString description;
    QJsonObject parameters; // JSON schema
};

struct ChatRequest
{
    QList<ChatMessage> messages;
    QString model;          // empty = provider default
    double temperature = 0.6;
    int maxTokens = 4096;
    QJsonObject jsonSchema; // when set, the reply must be a JSON object matching this schema
    QString schemaName = QStringLiteral("response");
    QList<ToolDefinition> tools;
    bool stream = true;
};

struct ChatResult
{
    QString text;
    QJsonArray providerContent; // see ChatMessage::providerContent
    QString providerType;
    QString reasoning;
    QList<ToolCall> toolCalls;
    QString finishReason;
    QJsonObject usage;
};

struct LlmCapabilities
{
    bool vision = true;
    bool tools = true;
    bool jsonSchema = true;
    bool streaming = true;
};

} // namespace lumen::ai
