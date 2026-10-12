#include "runtime/ModelFormats.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <functional>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ModelFormats)
};

struct KindName
{
    ModelKind kind;
    const char *key;
    const char *label;
};

const KindName kKinds[] = {
    {ModelKind::Unknown, "unknown", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Other")},
    {ModelKind::Checkpoint, "checkpoint", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Checkpoint")},
    {ModelKind::DiffusionModel, "diffusion_model", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Diffusion model")},
    {ModelKind::Lora, "lora", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "LoRA")},
    {ModelKind::ControlNet, "controlnet", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "ControlNet")},
    {ModelKind::IpAdapter, "ipadapter", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "IP-Adapter")},
    {ModelKind::ClipVision, "clip_vision", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "CLIP vision")},
    {ModelKind::Vae, "vae", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "VAE")},
    {ModelKind::TextEncoder, "text_encoder", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Text encoder")},
    {ModelKind::Upscaler, "upscaler", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Upscaler")},
    {ModelKind::Embedding, "embedding", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Embedding")},
    {ModelKind::Llm, "llm", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Language model")},
    {ModelKind::Projector, "projector", QT_TRANSLATE_NOOP("lumen::runtime::ModelFormats", "Vision projector")},
};

struct Tensors
{
    QStringList names;
    QHash<QString, QList<qint64>> shapes;

    bool any(const std::function<bool(const QString &)> &pred) const
    {
        for (const QString &n : names) {
            if (pred(n))
                return true;
        }
        return false;
    }
    bool hasPrefix(const QString &p) const
    {
        return any([&p](const QString &n) { return n.startsWith(p); });
    }
    bool has(const QString &s) const
    {
        return any([&s](const QString &n) { return n.contains(s); });
    }
    qint64 dim(const QString &name, int i) const { return shapes.value(name).value(i, 0); }
};

// Hints from the file and folder names (ComfyUI / A1111 folder layout, common naming).
ModelKind kindFromNames(const QString &path)
{
    const QFileInfo fi(path);
    const QString file = fi.fileName().toLower();
    const QString folder = fi.absolutePath().toLower();
    auto in = [&folder](const char *f) { return folder.contains(QLatin1String(f)); };
    if (file.startsWith(QLatin1String("mmproj")))
        return ModelKind::Projector;
    if (in("/loras") || in("/lora") || in("/lycoris"))
        return ModelKind::Lora;
    if (in("/controlnet"))
        return ModelKind::ControlNet;
    if (in("/ipadapter") || file.contains(QLatin1String("ip-adapter")) || file.contains(QLatin1String("ip_adapter")))
        return ModelKind::IpAdapter;
    if (in("/clip_vision"))
        return ModelKind::ClipVision;
    if (in("/vae") || file.contains(QLatin1String("vae")))
        return ModelKind::Vae;
    if (in("/upscale") || in("/esrgan") || file.contains(QLatin1String("esrgan")) || file.contains(QLatin1String("_x4")) || file.contains(QLatin1String("4x")))
        return ModelKind::Upscaler;
    if (in("/embeddings"))
        return ModelKind::Embedding;
    if (in("/text_encoders") || in("/clip"))
        return ModelKind::TextEncoder;
    if (in("/diffusion_models") || in("/unet"))
        return ModelKind::DiffusionModel;
    if (in("/checkpoints") || in("/stable-diffusion"))
        return ModelKind::Checkpoint;
    if (file.endsWith(QLatin1String(".gguf")))
        return ModelKind::Llm;
    return ModelKind::Unknown;
}

QString variantFromNames(const QString &path)
{
    const QString n = QFileInfo(path).fileName().toLower();
    if (n.contains(QLatin1String("illustrious")) || n.contains(QLatin1String("noob")) || n.contains(QLatin1String("wai")))
        return QStringLiteral("Illustrious");
    if (n.contains(QLatin1String("pony")))
        return QStringLiteral("Pony");
    if (n.contains(QLatin1String("animagine")))
        return QStringLiteral("Animagine");
    return {};
}

// Classifies a diffusion-side file from its tensor names (safetensors or GGUF).
void classifyDiffusion(const Tensors &t, const QVariantMap &meta, ModelInfo *info)
{
    auto sdFamilyFromUnet = [&t]() -> QString {
        if (t.has(QStringLiteral("label_emb")) || t.has(QStringLiteral("add_embedding")) || t.has(QStringLiteral("conditioner.embedders.1")))
            return QStringLiteral("SDXL");
        return QStringLiteral("SD 1.5");
    };
    // LoRA (kohya, diffusers/peft, LyCORIS).
    if (t.has(QStringLiteral("lora_up")) || t.has(QStringLiteral("lora_down")) || t.has(QStringLiteral(".lora_A.")) || t.has(QStringLiteral(".lora_B."))
        || t.hasPrefix(QStringLiteral("lora_unet_")) || t.has(QStringLiteral(".hada_w1")) || t.has(QStringLiteral(".lokr_w1"))) {
        info->kind = ModelKind::Lora;
        const QString base = meta.value(QStringLiteral("ss_base_model_version")).toString().toLower()
                             + meta.value(QStringLiteral("modelspec.architecture")).toString().toLower();
        if (base.contains(QLatin1String("flux")) || t.has(QStringLiteral("double_blocks")) || t.has(QStringLiteral("single_transformer_blocks")))
            info->family = QStringLiteral("FLUX");
        else if (base.contains(QLatin1String("sdxl")) || base.contains(QLatin1String("xl")) || t.hasPrefix(QStringLiteral("lora_te2_"))
                 || t.has(QStringLiteral("input_blocks_4_1_transformer_blocks_1")))
            info->family = QStringLiteral("SDXL");
        else if (base.contains(QLatin1String("sd_v1")) || base.contains(QLatin1String("v1")) || t.hasPrefix(QStringLiteral("lora_te_text_model")))
            info->family = QStringLiteral("SD 1.5");
        const QString trigger = meta.value(QStringLiteral("modelspec.trigger_phrase")).toString().trimmed();
        if (!trigger.isEmpty())
            info->triggerWords = trigger.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString &w : info->triggerWords)
            w = w.trimmed();
        return;
    }
    // IP-Adapter: image projection + per-layer key/value projections.
    if (t.hasPrefix(QStringLiteral("image_proj.")) && t.hasPrefix(QStringLiteral("ip_adapter."))) {
        info->kind = ModelKind::IpAdapter;
        // Cross-attention width: 2048 for SDXL, 768 for SD 1.5.
        for (const QString &n : t.names) {
            if (n.endsWith(QLatin1String("to_k_ip.weight"))) {
                const qint64 width = t.dim(n, 1);
                info->family = width == 2048 ? QStringLiteral("SDXL") : width == 768 ? QStringLiteral("SD 1.5") : QString();
                break;
            }
        }
        return;
    }
    // ControlNet (A1111 / ldm layout or diffusers).
    if (t.hasPrefix(QStringLiteral("control_model.")) || t.has(QStringLiteral("controlnet_cond_embedding")) || t.has(QStringLiteral("controlnet_down_blocks"))
        || t.has(QStringLiteral("input_hint_block"))) {
        info->kind = ModelKind::ControlNet;
        info->family = sdFamilyFromUnet();
        return;
    }
    // Complete checkpoints.
    if (t.hasPrefix(QStringLiteral("model.diffusion_model.")) && (t.hasPrefix(QStringLiteral("first_stage_model.")) || t.hasPrefix(QStringLiteral("cond_stage_model."))
                                                                  || t.hasPrefix(QStringLiteral("conditioner.")))) {
        info->kind = ModelKind::Checkpoint;
        if (t.has(QStringLiteral("double_blocks")))
            info->family = QStringLiteral("FLUX");
        else if (t.has(QStringLiteral("joint_blocks")))
            info->family = QStringLiteral("SD 3");
        else if (t.hasPrefix(QStringLiteral("conditioner.embedders.1")) || t.has(QStringLiteral("label_emb")))
            info->family = QStringLiteral("SDXL");
        else if (t.hasPrefix(QStringLiteral("cond_stage_model.model.")))
            info->family = QStringLiteral("SD 2");
        else
            info->family = QStringLiteral("SD 1.5");
        return;
    }
    // Diffusion model only.
    if (t.has(QStringLiteral("double_blocks.")) || t.has(QStringLiteral("single_blocks."))) {
        info->kind = ModelKind::DiffusionModel;
        info->family = QStringLiteral("FLUX");
        return;
    }
    if (t.has(QStringLiteral("joint_blocks."))) {
        info->kind = ModelKind::DiffusionModel;
        info->family = QStringLiteral("SD 3");
        return;
    }
    if (t.hasPrefix(QStringLiteral("net.blocks.")) || t.hasPrefix(QStringLiteral("model.diffusion_model.net.blocks."))) {
        // NVIDIA Cosmos-style DiT; with an LLM adapter it is Anima.
        info->kind = ModelKind::DiffusionModel;
        info->family = t.has(QStringLiteral("llm_adapter")) ? QStringLiteral("Anima") : QStringLiteral("Cosmos");
        return;
    }
    if (t.has(QStringLiteral("transformer_blocks.")) && t.has(QStringLiteral("img_mod"))) {
        info->kind = ModelKind::DiffusionModel;
        info->family = QStringLiteral("Qwen-Image");
        return;
    }
    if (t.hasPrefix(QStringLiteral("input_blocks.")) || t.hasPrefix(QStringLiteral("model.diffusion_model.input_blocks."))) {
        info->kind = ModelKind::DiffusionModel;
        info->family = sdFamilyFromUnet();
        return;
    }
    // Image encoders and text encoders.
    if (t.has(QStringLiteral("vision_model.encoder.layers")) || t.hasPrefix(QStringLiteral("visual.transformer.resblocks"))) {
        info->kind = ModelKind::ClipVision;
        return;
    }
    if (t.has(QStringLiteral("text_model.encoder.layers")) || t.hasPrefix(QStringLiteral("encoder.block.")) || t.hasPrefix(QStringLiteral("model.layers."))
        || t.hasPrefix(QStringLiteral("enc.blk."))) {
        info->kind = ModelKind::TextEncoder;
        // Which slot of a server it fills: a language model (Qwen, Llama: sd.cpp --llm), T5, or CLIP.
        info->family = t.hasPrefix(QStringLiteral("model.layers."))                                            ? QStringLiteral("LLM")
                       : t.hasPrefix(QStringLiteral("encoder.block.")) || t.hasPrefix(QStringLiteral("enc.blk.")) ? QStringLiteral("T5")
                                                                                                                  : QStringLiteral("CLIP");
        return;
    }
    // Autoencoders: 16 latent channels (FLUX / SD 3) or 4 (SD / SDXL).
    if ((t.has(QStringLiteral("encoder.down")) || t.has(QStringLiteral("decoder.up"))) && t.has(QStringLiteral("decoder."))) {
        info->kind = ModelKind::Vae;
        for (const QString &n : t.names) {
            if (n.endsWith(QLatin1String("decoder.conv_in.weight"))) {
                const qint64 channels = t.dim(n, 1);
                info->family = channels == 32   ? QStringLiteral("FLUX.2")
                               : channels == 16 ? QStringLiteral("FLUX / SD 3")
                               : channels == 4  ? QStringLiteral("SD / SDXL")
                                                : QString();
                break;
            }
        }
        return;
    }
    // ESRGAN-style upscalers.
    if ((t.hasPrefix(QStringLiteral("conv_first")) && t.hasPrefix(QStringLiteral("body."))) || (t.hasPrefix(QStringLiteral("model.0.")) && t.names.size() < 2000)) {
        info->kind = ModelKind::Upscaler;
        return;
    }
    // Textual inversion embeddings: a handful of small tensors.
    if (t.names.size() <= 4 && (t.has(QStringLiteral("emb_params")) || t.has(QStringLiteral("clip_l")) || t.has(QStringLiteral("clip_g")))) {
        info->kind = ModelKind::Embedding;
        return;
    }
}

QString precisionFromDtypes(const QHash<QString, qint64> &elementsByType)
{
    QString best;
    qint64 most = -1;
    for (auto it = elementsByType.cbegin(); it != elementsByType.cend(); ++it) {
        if (it.value() > most) {
            most = it.value();
            best = it.key();
        }
    }
    if (best.startsWith(QLatin1String("F8")))
        return QStringLiteral("FP8");
    return best;
}

// ---- safetensors --------------------------------------------------------------------------------

void inspectSafetensors(QFile &f, const QString &path, ModelInfo *info)
{
    info->format = QStringLiteral("safetensors");
    QDataStream in(&f);
    in.setByteOrder(QDataStream::LittleEndian);
    quint64 headerSize = 0;
    in >> headerSize;
    if (in.status() != QDataStream::Ok || headerSize == 0 || headerSize > 200ull * 1024 * 1024 || qint64(headerSize) > f.size()) {
        info->error = Text::tr("Not a safetensors file.");
        return;
    }
    const QByteArray header = f.read(qint64(headerSize));
    QJsonParseError pe;
    const QJsonObject root = QJsonDocument::fromJson(header, &pe).object();
    if (pe.error != QJsonParseError::NoError) {
        info->error = Text::tr("Damaged safetensors header: %1").arg(pe.errorString());
        return;
    }
    Tensors t;
    QHash<QString, qint64> byType;
    QVariantMap meta;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (it.key() == QLatin1String("__metadata__")) {
            meta = it.value().toObject().toVariantMap();
            continue;
        }
        const QJsonObject o = it.value().toObject();
        QList<qint64> shape;
        qint64 elements = 1;
        for (const QJsonValue &d : o.value(QStringLiteral("shape")).toArray()) {
            shape << qint64(d.toDouble());
            elements *= qint64(d.toDouble());
        }
        t.names << it.key();
        t.shapes.insert(it.key(), shape);
        info->parameters += elements;
        byType[o.value(QStringLiteral("dtype")).toString()] += elements;
    }
    info->precision = precisionFromDtypes(byType);
    classifyDiffusion(t, meta, info);
    if (info->kind == ModelKind::Unknown)
        info->kind = kindFromNames(path);
}

// ---- GGUF ---------------------------------------------------------------------------------------

class GgufReader
{
public:
    explicit GgufReader(QFile &f)
        : m_in(&f)
    {
        m_in.setByteOrder(QDataStream::LittleEndian);
    }
    bool ok() const { return m_in.status() == QDataStream::Ok; }
    quint32 u32()
    {
        quint32 v = 0;
        m_in >> v;
        return v;
    }
    quint64 u64()
    {
        quint64 v = 0;
        m_in >> v;
        return v;
    }
    QString string()
    {
        const quint64 n = u64();
        if (n > 64ull * 1024 * 1024) {
            m_in.setStatus(QDataStream::ReadCorruptData);
            return {};
        }
        QByteArray b(qsizetype(n), Qt::Uninitialized);
        if (m_in.readRawData(b.data(), int(n)) != int(n))
            m_in.setStatus(QDataStream::ReadPastEnd);
        return QString::fromUtf8(b);
    }
    void skip(qint64 n)
    {
        if (n > 0 && m_in.skipRawData(int(n)) != int(n))
            m_in.setStatus(QDataStream::ReadPastEnd);
    }
    static int scalarSize(quint32 type)
    {
        switch (type) {
        case 0: case 1: case 7: return 1;   // u8, i8, bool
        case 2: case 3: return 2;           // u16, i16
        case 4: case 5: case 6: return 4;   // u32, i32, f32
        case 10: case 11: case 12: return 8; // u64, i64, f64
        default: return 0;
        }
    }
    QVariant scalar(quint32 type)
    {
        switch (type) {
        case 0: { quint8 v; m_in >> v; return v; }
        case 1: { qint8 v; m_in >> v; return v; }
        case 2: { quint16 v; m_in >> v; return v; }
        case 3: { qint16 v; m_in >> v; return v; }
        case 4: { quint32 v; m_in >> v; return v; }
        case 5: { qint32 v; m_in >> v; return v; }
        case 6: { m_in.setFloatingPointPrecision(QDataStream::SinglePrecision); float v; m_in >> v; return v; }
        case 7: { quint8 v; m_in >> v; return v != 0; }
        case 8: return string();
        case 10: { quint64 v; m_in >> v; return v; }
        case 11: { qint64 v; m_in >> v; return v; }
        case 12: { m_in.setFloatingPointPrecision(QDataStream::DoublePrecision); double v; m_in >> v; return v; }
        default: m_in.setStatus(QDataStream::ReadCorruptData); return {};
        }
    }
    // Reads a value; arrays are skipped (vocabularies can hold 150k strings) unless they are short.
    QVariant value(quint32 type)
    {
        if (type != 9)
            return scalar(type);
        const quint32 itemType = u32();
        const quint64 count = u64();
        if (itemType == 8) {
            QVariantList small;
            for (quint64 i = 0; i < count && ok(); ++i) {
                const QString s = string();
                if (count <= 16)
                    small << s;
            }
            return count <= 16 ? QVariant(small) : QVariant();
        }
        const int size = scalarSize(itemType);
        if (size == 0) {
            m_in.setStatus(QDataStream::ReadCorruptData);
            return {};
        }
        skip(qint64(count) * size);
        return {};
    }

private:
    QDataStream m_in;
};

void inspectGguf(QFile &f, const QString &path, ModelInfo *info)
{
    info->format = QStringLiteral("gguf");
    GgufReader r(f);
    const quint32 magic = r.u32();
    const quint32 version = r.u32();
    if (magic != 0x46554747u || version < 2 || version > 3) {
        info->error = Text::tr("Not a GGUF file (or an unsupported version).");
        return;
    }
    const quint64 tensorCount = r.u64();
    const quint64 kvCount = r.u64();
    if (tensorCount > 1000000 || kvCount > 100000) {
        info->error = Text::tr("Damaged GGUF header.");
        return;
    }
    QVariantMap meta;
    for (quint64 i = 0; i < kvCount && r.ok(); ++i) {
        const QString key = r.string();
        const quint32 type = r.u32();
        const QVariant v = r.value(type);
        if (v.isValid())
            meta.insert(key, v);
    }
    Tensors t;
    for (quint64 i = 0; i < tensorCount && r.ok(); ++i) {
        const QString name = r.string();
        const quint32 dims = r.u32();
        if (dims > 8) {
            info->error = Text::tr("Damaged GGUF header.");
            return;
        }
        QList<qint64> shape;
        qint64 elements = 1;
        for (quint32 d = 0; d < dims; ++d) {
            const qint64 n = qint64(r.u64());
            shape << n;
            elements *= n;
        }
        r.u32(); // ggml type
        r.u64(); // offset
        t.names << name;
        t.shapes.insert(name, shape);
        info->parameters += elements;
    }
    if (!r.ok()) {
        info->error = Text::tr("Damaged GGUF header.");
        return;
    }
    const QString arch = meta.value(QStringLiteral("general.architecture")).toString();
    info->title = meta.value(QStringLiteral("general.name")).toString();
    if (meta.contains(QStringLiteral("general.file_type")))
        info->precision = ggufFileTypeName(meta.value(QStringLiteral("general.file_type")).toInt());
    if (info->precision.isEmpty()) {
        static const QRegularExpression quant(QStringLiteral(R"((I?Q\d_[A-Z0-9_]+|Q\d_\d|F16|BF16|F32))"), QRegularExpression::CaseInsensitiveOption);
        const auto m = quant.match(QFileInfo(path).completeBaseName());
        if (m.hasMatch())
            info->precision = m.captured(1).toUpper();
    }
    if (arch == QLatin1String("clip") || meta.contains(QStringLiteral("clip.has_vision_encoder"))) {
        info->kind = ModelKind::Projector;
        info->family = meta.value(QStringLiteral("clip.projector_type")).toString();
        return;
    }
    if (!arch.isEmpty() && t.hasPrefix(QStringLiteral("blk."))) {
        info->kind = ModelKind::Llm;
        info->family = arch;
        auto num = [&meta, &arch](const char *suffix) { return meta.value(arch + QLatin1Char('.') + QLatin1String(suffix)).toInt(); };
        info->contextLength = num("context_length");
        info->layers = num("block_count");
        info->kvHeads = num("attention.head_count_kv");
        const int heads = num("attention.head_count");
        info->headDim = num("attention.key_length");
        if (info->headDim == 0 && heads > 0)
            info->headDim = num("embedding_length") / heads;
        if (info->kvHeads == 0)
            info->kvHeads = heads;
        return;
    }
    // Quantized diffusion models and encoders for stable-diffusion.cpp.
    classifyDiffusion(t, meta, info);
    if (info->kind == ModelKind::Unknown)
        info->kind = kindFromNames(path);
}

} // namespace

QString modelKindKey(ModelKind kind)
{
    for (const KindName &k : kKinds) {
        if (k.kind == kind)
            return QString::fromLatin1(k.key);
    }
    return QStringLiteral("unknown");
}

QString modelKindLabel(ModelKind kind)
{
    for (const KindName &k : kKinds) {
        if (k.kind == kind)
            return QCoreApplication::translate("lumen::runtime::ModelFormats", k.label);
    }
    return {};
}

ModelKind modelKindFromKey(const QString &key)
{
    for (const KindName &k : kKinds) {
        if (key == QLatin1String(k.key))
            return k.kind;
    }
    return ModelKind::Unknown;
}

QList<ModelKind> allModelKinds()
{
    QList<ModelKind> out;
    for (const KindName &k : kKinds)
        out << k.kind;
    return out;
}

QStringList modelFileFilters()
{
    return {QStringLiteral("*.gguf"), QStringLiteral("*.safetensors"), QStringLiteral("*.sft"), QStringLiteral("*.ckpt"),
            QStringLiteral("*.pt"), QStringLiteral("*.pth"), QStringLiteral("*.bin")};
}

QString ggufFileTypeName(int type)
{
    static const QHash<int, QString> names = {
        {0, QStringLiteral("F32")},     {1, QStringLiteral("F16")},     {2, QStringLiteral("Q4_0")},    {3, QStringLiteral("Q4_1")},
        {7, QStringLiteral("Q8_0")},    {8, QStringLiteral("Q5_0")},    {9, QStringLiteral("Q5_1")},    {10, QStringLiteral("Q2_K")},
        {11, QStringLiteral("Q3_K_S")}, {12, QStringLiteral("Q3_K_M")}, {13, QStringLiteral("Q3_K_L")}, {14, QStringLiteral("Q4_K_S")},
        {15, QStringLiteral("Q4_K_M")}, {16, QStringLiteral("Q5_K_S")}, {17, QStringLiteral("Q5_K_M")}, {18, QStringLiteral("Q6_K")},
        {19, QStringLiteral("IQ2_XXS")}, {20, QStringLiteral("IQ2_XS")}, {21, QStringLiteral("Q2_K_S")}, {22, QStringLiteral("IQ3_XS")},
        {23, QStringLiteral("IQ3_XXS")}, {24, QStringLiteral("IQ1_S")}, {25, QStringLiteral("IQ4_NL")}, {26, QStringLiteral("IQ3_S")},
        {27, QStringLiteral("IQ3_M")},  {28, QStringLiteral("IQ2_S")},  {29, QStringLiteral("IQ2_M")},  {30, QStringLiteral("IQ4_XS")},
        {31, QStringLiteral("IQ1_M")},  {32, QStringLiteral("BF16")},
    };
    return names.value(type);
}

qint64 ModelInfo::estimatedMemory(qint64 fileSize, int context, double kvBytesPerValue) const
{
    if (kind == ModelKind::Llm && layers > 0 && kvHeads > 0 && headDim > 0) {
        const int ctx = contextLength > 0 ? std::min(context, contextLength) : context;
        const double kv = 2.0 * layers * kvHeads * headDim * double(ctx) * kvBytesPerValue;
        return fileSize + qint64(kv) + 512ll * 1024 * 1024; // compute buffers
    }
    if (kind == ModelKind::Checkpoint || kind == ModelKind::DiffusionModel)
        return fileSize + 1536ll * 1024 * 1024; // activations at 1024², VAE decode
    return fileSize;
}

ModelInfo inspectModel(const QString &path)
{
    ModelInfo info;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        info.error = f.errorString();
        return info;
    }
    const QByteArray magic = f.peek(4);
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (magic == "GGUF") {
        inspectGguf(f, path, &info);
    } else if (suffix == QLatin1String("safetensors") || suffix == QLatin1String("sft")) {
        inspectSafetensors(f, path, &info);
    } else {
        // Pickle (.ckpt/.pt/.pth/.bin): never unpickled; Python tools can run code from these files.
        info.format = QStringLiteral("pickle");
        info.unsafeFormat = true;
        info.kind = kindFromNames(path);
        if (info.kind == ModelKind::Llm)
            info.kind = ModelKind::Unknown;
    }
    if (info.kind == ModelKind::Checkpoint || info.kind == ModelKind::Lora || info.kind == ModelKind::DiffusionModel || info.kind == ModelKind::ControlNet)
        info.variant = variantFromNames(path);
    return info;
}

} // namespace lumen::runtime
