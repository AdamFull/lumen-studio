#include "runtime/ServerProfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpServer>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ServerProfile)
};

using T = OptionField::Type;

OptionField field(const QString &key, const QString &label, T type, const QVariant &def = {}, const QString &help = {})
{
    OptionField f;
    f.key = key;
    f.label = label;
    f.type = type;
    f.defaultValue = def;
    f.help = help;
    return f;
}

QList<OptionField> llamaOptions()
{
    const QString gguf = Text::tr("GGUF models (*.gguf)");
    QList<OptionField> out;
    OptionField model = field(QStringLiteral("model"), Text::tr("Model"), T::File, {}, Text::tr("The language model (.gguf)."));
    model.filter = gguf;
    model.required = true;
    out << model;
    OptionField mmproj = field(QStringLiteral("mmproj"), Text::tr("Vision projector"), T::File, {},
                               Text::tr("mmproj-*.gguf file that lets a vision model (Qwen3-VL, Gemma 3…) see images."));
    mmproj.filter = gguf;
    out << mmproj;
    OptionField ctx = field(QStringLiteral("context"), Text::tr("Context size"), T::Integer, 16384,
                            Text::tr("Tokens of context. Larger needs more VRAM; 16384 suits planning and the Critic."));
    ctx.minimum = 512;
    ctx.maximum = 1048576;
    out << ctx;
    OptionField ngl = field(QStringLiteral("gpu_layers"), Text::tr("GPU layers"), T::Integer, 999,
                            Text::tr("Layers kept on the GPU (999 = all). Lower it when the model does not fit."));
    ngl.maximum = 999;
    out << ngl;
    OptionField kv = field(QStringLiteral("kv_cache"), Text::tr("KV cache type"), T::Choice, QStringLiteral("q8_0"),
                           Text::tr("q8_0 halves the context memory with no visible quality loss."));
    kv.choices = {QStringLiteral("f16"), QStringLiteral("q8_0"), QStringLiteral("q4_0")};
    out << kv;
    OptionField fa = field(QStringLiteral("flash_attention"), Text::tr("Flash attention"), T::Choice, QStringLiteral("on"));
    fa.choices = {QStringLiteral("auto"), QStringLiteral("on"), QStringLiteral("off")};
    out << fa;
    OptionField sleep = field(QStringLiteral("sleep_idle"), Text::tr("Unload when idle (s)"), T::Integer, 30,
                              Text::tr("Unloads the model from the GPU after this many idle seconds so an image model can use "
                                 "it (0 = never). Reloading takes a few seconds."));
    sleep.maximum = 86400;
    out << sleep;
    out << field(QStringLiteral("jinja"), Text::tr("Use the model's chat template (--jinja)"), T::Boolean, true,
                 Text::tr("Needed for tool calls and reasoning models."));
    OptionField parallel = field(QStringLiteral("parallel"), Text::tr("Parallel requests"), T::Integer, 1,
                                 Text::tr("Slots; the context is shared between them."));
    parallel.minimum = 1;
    parallel.maximum = 64;
    parallel.advanced = true;
    out << parallel;
    OptionField threads = field(QStringLiteral("threads"), Text::tr("CPU threads"), T::Integer, 0, Text::tr("0 = automatic"));
    threads.maximum = 256;
    threads.advanced = true;
    out << threads;
    OptionField reasoning = field(QStringLiteral("reasoning_budget"), Text::tr("Reasoning budget"), T::Integer, -1,
                                  Text::tr("-1 = unlimited thinking, 0 = thinking off (faster for small models)."));
    reasoning.minimum = -1;
    reasoning.maximum = 1000000;
    reasoning.advanced = true;
    out << reasoning;
    return out;
}

QList<OptionField> sdOptions()
{
    const QString models = Text::tr("Models (*.safetensors *.gguf *.ckpt *.sft)");
    QList<OptionField> out;
    OptionField model = field(QStringLiteral("model"), Text::tr("Checkpoint"), T::File, {},
                              Text::tr("Single-file checkpoint (SD 1.5, SDXL, Illustrious, Pony…). For FLUX-style models use "
                                 "\"Diffusion model\" with separate text encoders instead."));
    model.filter = models;
    out << model;
    OptionField vae = field(QStringLiteral("vae"), Text::tr("VAE"), T::File, {}, Text::tr("Optional separate VAE (e.g. sdxl_vae.safetensors)."));
    vae.filter = models;
    out << vae;
    OptionField cn = field(QStringLiteral("control_net"), Text::tr("ControlNet"), T::File, {},
                           Text::tr("Needed for pose control (e.g. an SDXL OpenPose ControlNet)."));
    cn.filter = models;
    out << cn;
    OptionField ipa = field(QStringLiteral("ip_adapter"), Text::tr("IP-Adapter"), T::File, {},
                            Text::tr("Keeps a character's identity from a reference image (e.g. ip-adapter_sdxl_vit-h). "
                               "Needs the CLIP vision encoder below."));
    ipa.filter = models;
    out << ipa;
    OptionField clipVision = field(QStringLiteral("clip_vision"), Text::tr("CLIP vision encoder"), T::File, {},
                                   Text::tr("ViT-H image encoder for the IP-Adapter (clip_vision_h.safetensors)."));
    clipVision.filter = models;
    out << clipVision;
    out << field(QStringLiteral("lora_dir"), Text::tr("LoRA folder"), T::Directory, {}, Text::tr("LoRAs in this folder can be used in prompts."));
    out << field(QStringLiteral("upscalers_dir"), Text::tr("Upscaler folder"), T::Directory, {},
                 Text::tr("Folder with ESRGAN models (e.g. RealESRGAN_x4plus_anime_6B.pth) for upscaling and hires fix; "
                          "ComfyUI's models/upscale_models works."));
    OptionField precision = field(QStringLiteral("weight_type"), Text::tr("Weight precision"), T::Choice, QStringLiteral("q8_0"),
                                  Text::tr("Converts the weights when loading. q8_0 halves an SDXL model with no visible change, so the "
                                           "model and a ControlNet fit in 8 GB and run faster; 'file' keeps what the file stores."));
    precision.choices = {QStringLiteral("q8_0"), QStringLiteral("q5_0"), QStringLiteral("q4_K"), QStringLiteral("f16"), QStringLiteral("file")};
    out << precision;
    out << field(QStringLiteral("offload_to_cpu"), Text::tr("Keep weights in RAM"), T::Boolean, true,
                 Text::tr("Moves weights to the GPU only while generating (--offload-to-cpu). Slower, but leaves VRAM for a "
                    "language model on the same GPU."));
    OptionField maxVram = field(QStringLiteral("max_vram"), Text::tr("VRAM budget (GB)"), T::Number, 0.0,
                                Text::tr("0 = use what is free. A budget makes large models run in parts."));
    maxVram.maximum = 256;
    out << maxVram;
    out << field(QStringLiteral("diffusion_fa"), Text::tr("Flash attention"), T::Boolean, true, Text::tr("Less VRAM, usually faster."));
    OptionField diffusion = field(QStringLiteral("diffusion_model"), Text::tr("Diffusion model"), T::File, {},
                                  Text::tr("Standalone diffusion model (FLUX, Qwen-Image, Z-Image…), used instead of a checkpoint."));
    diffusion.filter = models;
    diffusion.advanced = true;
    out << diffusion;
    for (const auto &[key, label] : {std::pair{QStringLiteral("clip_l"), Text::tr("CLIP-L text encoder")},
                                     std::pair{QStringLiteral("clip_g"), Text::tr("CLIP-G text encoder")},
                                     std::pair{QStringLiteral("t5xxl"), Text::tr("T5-XXL text encoder")},
                                     std::pair{QStringLiteral("llm"), Text::tr("LLM text encoder")}}) {
        OptionField f = field(key, label, T::File);
        f.filter = models;
        f.advanced = true;
        out << f;
    }
    OptionField clipCpu = field(QStringLiteral("clip_on_cpu"), Text::tr("Text encoders on the CPU"), T::Boolean, false);
    clipCpu.advanced = true;
    out << clipCpu;
    OptionField threads = field(QStringLiteral("threads"), Text::tr("CPU threads"), T::Integer, 0, Text::tr("0 = automatic"));
    threads.maximum = 256;
    threads.advanced = true;
    out << threads;
    return out;
}

} // namespace

QList<OptionField> serverOptions(const QString &kind)
{
    QList<OptionField> out;
    if (kind == QLatin1String("llama.cpp"))
        out = llamaOptions();
    else if (kind == QLatin1String("sd.cpp"))
        out = sdOptions();
    OptionField extra = field(QStringLiteral("extra_args"), QCoreApplication::translate("lumen::runtime::ServerProfile", "Extra arguments"),
                              T::Text, {}, QCoreApplication::translate("lumen::runtime::ServerProfile", "Added to the command line as typed."));
    extra.advanced = true;
    out << extra;
    return out;
}

QString startPolicyKey(StartPolicy p)
{
    switch (p) {
    case StartPolicy::AtStartup:
        return QStringLiteral("startup");
    case StartPolicy::Manual:
        return QStringLiteral("manual");
    case StartPolicy::OnDemand:
        break;
    }
    return QStringLiteral("demand");
}

StartPolicy startPolicyFromKey(const QString &key)
{
    if (key == QLatin1String("startup"))
        return StartPolicy::AtStartup;
    if (key == QLatin1String("manual"))
        return StartPolicy::Manual;
    return StartPolicy::OnDemand;
}

QVariant ServerProfile::option(const QString &key, const QString &kind) const
{
    const QVariant v = options.value(key);
    if (v.isValid() && !(v.typeId() == QMetaType::QString && v.toString().isEmpty()))
        return v;
    for (const OptionField &f : serverOptions(kind)) {
        if (f.key == key)
            return f.defaultValue;
    }
    return {};
}

QJsonObject ServerProfile::toJson() const
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("runtime"), runtimeId},
                       {QStringLiteral("port"), port},
                       {QStringLiteral("options"), QJsonObject::fromVariantMap(options)},
                       {QStringLiteral("start"), startPolicyKey(startPolicy)},
                       {QStringLiteral("idleStopMinutes"), idleStopMinutes},
                       {QStringLiteral("startupTimeoutSec"), startupTimeoutSec}};
}

ServerProfile ServerProfile::fromJson(const QJsonObject &o)
{
    ServerProfile p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.runtimeId = o.value(QStringLiteral("runtime")).toString();
    p.port = o.value(QStringLiteral("port")).toInt();
    p.options = o.value(QStringLiteral("options")).toObject().toVariantMap();
    p.startPolicy = startPolicyFromKey(o.value(QStringLiteral("start")).toString());
    p.idleStopMinutes = o.value(QStringLiteral("idleStopMinutes")).toInt();
    p.startupTimeoutSec = o.value(QStringLiteral("startupTimeoutSec")).toInt(600);
    return p;
}

QStringList splitArguments(const QString &text)
{
    QStringList out;
    QString cur;
    bool quoted = false;
    bool have = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            have = true;
        } else if (c.isSpace() && !quoted) {
            if (have)
                out << cur;
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have)
        out << cur;
    return out;
}

QString LaunchCommand::commandLine() const
{
    QStringList parts;
    auto quote = [](const QString &s) {
        return s.isEmpty() || s.contains(QLatin1Char(' ')) || s.contains(QLatin1Char('"'))
                   ? QLatin1Char('"') + QString(s).replace(QLatin1Char('"'), QStringLiteral("\\\"")) + QLatin1Char('"')
                   : s;
    };
    parts << quote(QDir::toNativeSeparators(program));
    for (const QString &a : arguments)
        parts << quote(a);
    return parts.join(QLatin1Char(' '));
}

LaunchCommand buildLaunch(const ServerProfile &p, const InstalledRuntime &rt)
{
    LaunchCommand cmd;
    cmd.program = rt.executable;
    cmd.workingDirectory = QFileInfo(rt.executable).absolutePath();
    const QString kind = rt.kind;
    auto str = [&](const QString &key) { return p.option(key, kind).toString().trimmed(); };
    auto num = [&](const QString &key) { return p.option(key, kind).toLongLong(); };
    auto flag = [&](const QString &key) { return p.option(key, kind).toBool(); };
    auto file = [&](const QString &flagName, const QString &key) {
        const QString path = str(key);
        if (path.isEmpty())
            return;
        if (!QFileInfo::exists(path) && cmd.error.isEmpty())
            cmd.error = Text::tr("%1 does not exist.").arg(QDir::toNativeSeparators(path));
        cmd.arguments << flagName << QDir::toNativeSeparators(path);
    };
    QStringList &a = cmd.arguments;

    if (kind == QLatin1String("llama.cpp")) {
        if (str(QStringLiteral("model")).isEmpty())
            cmd.error = Text::tr("Choose a model for %1.").arg(p.name);
        file(QStringLiteral("-m"), QStringLiteral("model"));
        file(QStringLiteral("--mmproj"), QStringLiteral("mmproj"));
        a << QStringLiteral("--host") << QStringLiteral("127.0.0.1") << QStringLiteral("--port") << QString::number(p.port);
        a << QStringLiteral("-c") << QString::number(num(QStringLiteral("context")));
        a << QStringLiteral("-ngl") << QString::number(num(QStringLiteral("gpu_layers")));
        const QString kv = str(QStringLiteral("kv_cache"));
        if (!kv.isEmpty() && kv != QLatin1String("f16"))
            a << QStringLiteral("-ctk") << kv << QStringLiteral("-ctv") << kv;
        const QString fa = str(QStringLiteral("flash_attention"));
        if (!fa.isEmpty())
            a << QStringLiteral("-fa") << fa;
        if (flag(QStringLiteral("jinja")))
            a << QStringLiteral("--jinja");
        if (num(QStringLiteral("sleep_idle")) > 0)
            a << QStringLiteral("--sleep-idle-seconds") << QString::number(num(QStringLiteral("sleep_idle")));
        if (num(QStringLiteral("parallel")) > 1)
            a << QStringLiteral("-np") << QString::number(num(QStringLiteral("parallel")));
        if (num(QStringLiteral("threads")) > 0)
            a << QStringLiteral("-t") << QString::number(num(QStringLiteral("threads")));
        if (num(QStringLiteral("reasoning_budget")) >= 0)
            a << QStringLiteral("--reasoning-budget") << QString::number(num(QStringLiteral("reasoning_budget")));
    } else if (kind == QLatin1String("sd.cpp")) {
        if (str(QStringLiteral("model")).isEmpty() && str(QStringLiteral("diffusion_model")).isEmpty())
            cmd.error = Text::tr("Choose a checkpoint or a diffusion model for %1.").arg(p.name);
        if (!str(QStringLiteral("ip_adapter")).isEmpty() && str(QStringLiteral("clip_vision")).isEmpty() && cmd.error.isEmpty())
            cmd.error = Text::tr("The IP-Adapter needs a CLIP vision encoder.");
        file(QStringLiteral("-m"), QStringLiteral("model"));
        file(QStringLiteral("--diffusion-model"), QStringLiteral("diffusion_model"));
        file(QStringLiteral("--vae"), QStringLiteral("vae"));
        file(QStringLiteral("--clip_l"), QStringLiteral("clip_l"));
        file(QStringLiteral("--clip_g"), QStringLiteral("clip_g"));
        file(QStringLiteral("--t5xxl"), QStringLiteral("t5xxl"));
        file(QStringLiteral("--llm"), QStringLiteral("llm"));
        file(QStringLiteral("--control-net"), QStringLiteral("control_net"));
        file(QStringLiteral("--ip-adapter"), QStringLiteral("ip_adapter"));
        file(QStringLiteral("--clip_vision"), QStringLiteral("clip_vision"));
        file(QStringLiteral("--lora-model-dir"), QStringLiteral("lora_dir"));
        file(QStringLiteral("--hires-upscalers-dir"), QStringLiteral("upscalers_dir"));
        a << QStringLiteral("--listen-ip") << QStringLiteral("127.0.0.1") << QStringLiteral("--listen-port") << QString::number(p.port);
        const QString weightType = str(QStringLiteral("weight_type"));
        if (!weightType.isEmpty() && weightType != QLatin1String("file"))
            a << QStringLiteral("--type") << weightType;
        if (flag(QStringLiteral("offload_to_cpu")))
            a << QStringLiteral("--offload-to-cpu");
        const double vram = p.option(QStringLiteral("max_vram"), kind).toDouble();
        if (vram > 0)
            a << QStringLiteral("--max-vram") << QString::number(vram);
        if (flag(QStringLiteral("diffusion_fa")))
            a << QStringLiteral("--diffusion-fa");
        if (flag(QStringLiteral("clip_on_cpu")))
            a << QStringLiteral("--clip-on-cpu");
        if (num(QStringLiteral("threads")) > 0)
            a << QStringLiteral("-t") << QString::number(num(QStringLiteral("threads")));
    } else {
        cmd.error = Text::tr("Lumen cannot start %1 servers.").arg(kind);
    }
    a << splitArguments(str(QStringLiteral("extra_args")));
    if (p.port <= 0 && cmd.error.isEmpty())
        cmd.error = Text::tr("No port set.");
    return cmd;
}

QString providerUrl(const QString &kind, int port)
{
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);
    return kind == QLatin1String("llama.cpp") ? base + QStringLiteral("/v1") : base;
}

QString providerTypeFor(const QString &kind)
{
    if (kind == QLatin1String("llama.cpp"))
        return QStringLiteral("llamacpp");
    if (kind == QLatin1String("sd.cpp"))
        return QStringLiteral("sdcpp");
    return {};
}

bool portIsFree(int port)
{
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, quint16(port));
}

int choosePort(int preferred, const QList<int> &taken)
{
    for (int port = std::max(1024, preferred); port < 65535; ++port) {
        if (!taken.contains(port) && portIsFree(port))
            return port;
    }
    return 0;
}

} // namespace lumen::runtime
