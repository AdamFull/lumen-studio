#include "runtime/ModelPresets.h"

#include "runtime/Hardware.h"
#include "runtime/ModelLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ModelPresets)
};

QStringList lowerStrings(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &s : v.toArray())
        out << s.toString().toLower();
    return out;
}

QStringList strings(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &s : v.toArray())
        out << s.toString();
    return out;
}

std::optional<VramTier> tierFromKey(const QString &key)
{
    if (key == QLatin1String("small"))
        return VramTier::Small;
    if (key == QLatin1String("medium"))
        return VramTier::Medium;
    if (key == QLatin1String("large"))
        return VramTier::Large;
    return std::nullopt;
}

QString fileName(const QString &path)
{
    return QFileInfo(QDir::fromNativeSeparators(path)).fileName();
}

// The label of an sd.cpp option in the server form ("LLM text encoder").
QString optionLabel(const QString &option)
{
    for (const OptionField &f : serverOptions(QStringLiteral("sd.cpp"))) {
        if (f.key == option)
            return f.label;
    }
    return option;
}

QString describe(const ModelInfo &info)
{
    return info.variant.isEmpty() ? info.family : QStringLiteral("%1 %2").arg(info.family, info.variant);
}

// What a file is, in a few words: "T5 text encoder", "Qwen3 8B text encoder", "FLUX / SD 3 VAE".
QString whatFileIs(const ModelInfo &info)
{
    const QString what = info.variant.isEmpty() ? info.family : info.variant;
    if (info.kind == ModelKind::TextEncoder)
        return what.isEmpty() ? Text::tr("text encoder") : Text::tr("%1 text encoder").arg(what);
    if (info.kind == ModelKind::Vae)
        return what.isEmpty() ? Text::tr("VAE") : Text::tr("%1 VAE").arg(what);
    return what.isEmpty() ? modelKindLabel(info.kind) : QStringLiteral("%1 (%2)").arg(modelKindLabel(info.kind), what);
}

} // namespace

QUrl PresetFile::url(const QString &huggingFaceBase) const
{
    return QUrl(QStringLiteral("%1/%2/resolve/%3/%4").arg(huggingFaceBase, repo, revision, path));
}

QString PresetFile::fileName() const
{
    return QFileInfo(path).fileName();
}

HubFile PresetSlot::hubFile() const
{
    HubFile f;
    f.path = file.fileName();
    f.url = file.url();
    f.size = file.size;
    f.sha256 = file.sha256;
    f.kind = kind;
    f.hubType = kind == ModelKind::Vae ? QStringLiteral("VAE") : QStringLiteral("Text Encoder");
    f.format = file.path.endsWith(QLatin1String(".gguf")) ? QStringLiteral("gguf") : QStringLiteral("safetensors");
    f.precision = precisionFromName(file.path);
    f.required = required;
    return f;
}

bool ModelPreset::matches(const ModelInfo &info, const QString &path) const
{
    if (!kinds.contains(info.kind) || info.family != family)
        return false;
    if (!variant.isEmpty() && info.variant != variant)
        return false;
    const QString name = fileName(path).toLower();
    if (!nameAny.isEmpty() && std::none_of(nameAny.cbegin(), nameAny.cend(), [&](const QString &w) { return name.contains(w); }))
        return false;
    return std::none_of(nameNone.cbegin(), nameNone.cend(), [&](const QString &w) { return name.contains(w); });
}

const PresetSlot *ModelPreset::slot(const QString &option) const
{
    for (const PresetSlot &s : components) {
        if (s.option == option)
            return &s;
    }
    return nullptr;
}

QList<ModelPreset> parseModelPresets(const QByteArray &json, const QString &familyFile, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QList<ModelPreset>();
    };
    QJsonParseError pe;
    const QJsonObject root = QJsonDocument::fromJson(json, &pe).object();
    if (pe.error != QJsonParseError::NoError)
        return fail(pe.errorString());
    if (root.value(QStringLiteral("version")).toInt() != 1)
        return fail(QStringLiteral("unknown version"));
    const QUrl familyDocs(root.value(QStringLiteral("docs")).toString());

    // The model types the file's presets belong to: id → name.
    QHash<QString, QString> types;
    const QJsonObject typeObjects = root.value(QStringLiteral("types")).toObject();
    for (auto it = typeObjects.begin(); it != typeObjects.end(); ++it) {
        if (it.value().toString().isEmpty())
            return fail(QStringLiteral("type %1 needs a name").arg(it.key()));
        types.insert(it.key(), it.value().toString());
    }

    QHash<QString, PresetFile> files;
    const QJsonObject fileObjects = root.value(QStringLiteral("files")).toObject();
    for (auto it = fileObjects.begin(); it != fileObjects.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        PresetFile f;
        f.id = it.key();
        f.repo = o.value(QStringLiteral("repo")).toString();
        f.revision = o.value(QStringLiteral("revision")).toString();
        f.path = o.value(QStringLiteral("path")).toString();
        f.size = qint64(o.value(QStringLiteral("size")).toDouble());
        f.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
        f.license = o.value(QStringLiteral("license")).toString();
        if (f.repo.isEmpty() || f.revision.size() != 40 || f.path.isEmpty() || f.size <= 0 || f.sha256.size() != 64)
            return fail(QStringLiteral("file %1: repo, a 40-character revision, path, size and sha256 are needed").arg(f.id));
        files.insert(f.id, f);
    }

    QList<ModelPreset> out;
    for (const QJsonValue &v : root.value(QStringLiteral("presets")).toArray()) {
        const QJsonObject o = v.toObject();
        ModelPreset p;
        p.id = o.value(QStringLiteral("id")).toString();
        p.name = o.value(QStringLiteral("name")).toString();
        p.familyFile = familyFile;
        p.note = o.value(QStringLiteral("note")).toString();
        p.docs = o.contains(QStringLiteral("docs")) ? QUrl(o.value(QStringLiteral("docs")).toString()) : familyDocs;
        if (p.id.isEmpty() || p.name.isEmpty())
            return fail(QStringLiteral("a preset needs an id and a name"));
        p.type = o.value(QStringLiteral("type")).toString();
        if (p.type.isEmpty()) {
            p.type = p.id;
            p.typeName = p.name;
        } else if (!types.contains(p.type)) {
            return fail(QStringLiteral("preset %1: unknown type %2 (name it under \"types\")").arg(p.id, p.type));
        } else {
            p.typeName = types.value(p.type);
        }
        p.optionalVae = o.value(QStringLiteral("optional_vae")).toString();

        const QJsonObject match = o.value(QStringLiteral("match")).toObject();
        p.family = match.value(QStringLiteral("family")).toString();
        p.variant = match.value(QStringLiteral("variant")).toString();
        for (const QString &k : strings(match.value(QStringLiteral("kinds"))))
            p.kinds << modelKindFromKey(k);
        p.nameAny = lowerStrings(match.value(QStringLiteral("name_any")));
        p.nameNone = lowerStrings(match.value(QStringLiteral("name_none")));
        if (p.family.isEmpty() || p.kinds.isEmpty() || p.kinds.contains(ModelKind::Unknown))
            return fail(QStringLiteral("preset %1: match needs a family and checkpoint / diffusion_model kinds").arg(p.id));

        for (const QJsonValue &sv : o.value(QStringLiteral("slots")).toArray()) {
            const QJsonObject so = sv.toObject();
            PresetSlot s;
            s.option = so.value(QStringLiteral("option")).toString();
            s.kind = modelKindFromKey(so.value(QStringLiteral("kind")).toString());
            s.family = so.value(QStringLiteral("family")).toString();
            s.variant = so.value(QStringLiteral("variant")).toString();
            s.label = so.value(QStringLiteral("label")).toString();
            s.required = so.value(QStringLiteral("required")).toBool(true);
            const QString fileId = so.value(QStringLiteral("file")).toString();
            if (!files.contains(fileId))
                return fail(QStringLiteral("preset %1: unknown file %2").arg(p.id, fileId));
            s.file = files.value(fileId);
            static const QStringList options{QStringLiteral("vae"), QStringLiteral("clip_l"), QStringLiteral("clip_g"), QStringLiteral("t5xxl"),
                                             QStringLiteral("llm")};
            if (!options.contains(s.option) || (s.kind != ModelKind::Vae && s.kind != ModelKind::TextEncoder) || s.family.isEmpty())
                return fail(QStringLiteral("preset %1: slot %2 needs an sd.cpp option, a vae / text_encoder kind and a family").arg(p.id, s.option));
            p.components << s;
        }
        if (o.contains(QStringLiteral("download"))) {
            const QString fileId = o.value(QStringLiteral("download")).toString();
            if (!files.contains(fileId))
                return fail(QStringLiteral("preset %1: unknown download %2").arg(p.id, fileId));
            p.download = files.value(fileId);
        }

        const QJsonObject can = o.value(QStringLiteral("can")).toObject();
        p.generate = can.value(QStringLiteral("generate")).toBool(true);
        p.edit = can.value(QStringLiteral("edit")).toBool(false);
        p.inpaint = can.value(QStringLiteral("inpaint")).toBool(true);
        p.references = can.value(QStringLiteral("references")).toInt(0);
        p.negativePrompt = can.value(QStringLiteral("negative_prompt")).toBool(true);
        p.loras = can.value(QStringLiteral("loras")).toBool(true);
        p.controlNetFamilies = strings(can.value(QStringLiteral("control_net")));
        p.ipAdapterFamilies = strings(can.value(QStringLiteral("ip_adapter")));

        const QJsonObject sampling = o.value(QStringLiteral("sampling")).toObject();
        p.cfg = sampling.value(QStringLiteral("cfg")).toDouble();
        p.guidance = sampling.value(QStringLiteral("guidance")).toDouble();
        p.imageCfg = sampling.value(QStringLiteral("image_cfg")).toDouble();
        p.flowShift = sampling.value(QStringLiteral("flow_shift")).toDouble();
        p.steps = sampling.value(QStringLiteral("steps")).toInt();
        p.sampler = sampling.value(QStringLiteral("sampler")).toString();
        p.scheduler = sampling.value(QStringLiteral("scheduler")).toString();
        p.nativeResolution = sampling.value(QStringLiteral("native_resolution")).toInt();
        p.dialect = sampling.value(QStringLiteral("dialect")).toString();

        const QJsonObject vram = o.value(QStringLiteral("vram")).toObject();
        for (auto it = vram.begin(); it != vram.end(); ++it) {
            const auto tier = tierFromKey(it.key());
            if (!tier)
                return fail(QStringLiteral("preset %1: vram tiers are small, medium and large").arg(p.id));
            p.vram.insert(*tier, it.value().toObject().toVariantMap());
        }
        p.serverOptions = o.value(QStringLiteral("server")).toObject().toVariantMap();
        out << p;
    }
    if (error)
        error->clear();
    return out;
}

const QList<ModelPreset> &modelPresets()
{
    static const QList<ModelPreset> presets = [] {
        QList<ModelPreset> all;
        // Family files in name order; within a file, the order the file gives (specific first).
        QStringList paths;
        QDirIterator it(QStringLiteral(":/presets/models"), {QStringLiteral("*.json")}, QDir::Files);
        while (it.hasNext())
            paths << it.next();
        paths.sort();
        for (const QString &path : std::as_const(paths)) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            QString error;
            const QList<ModelPreset> family = parseModelPresets(f.readAll(), QFileInfo(path).completeBaseName(), &error);
            if (!error.isEmpty())
                qWarning("Model presets %s: %s", qPrintable(path), qPrintable(error));
            all << family;
        }
        return all;
    }();
    return presets;
}

const ModelPreset *presetFor(const ModelInfo &info, const QString &path)
{
    for (const ModelPreset &p : modelPresets()) {
        if (p.matches(info, path))
            return &p;
    }
    return nullptr;
}

const ModelPreset *presetById(const QString &id)
{
    for (const ModelPreset &p : modelPresets()) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

QString ModelType::modelOption(const std::optional<ModelInfo> &file) const
{
    if (file && file->kind == ModelKind::Checkpoint && kinds.contains(ModelKind::Checkpoint))
        return QStringLiteral("model");
    return kinds.contains(ModelKind::DiffusionModel) ? QStringLiteral("diffusion_model") : QStringLiteral("model");
}

const PresetSlot *ModelType::slot(const QString &option) const
{
    for (const PresetSlot &s : components) {
        if (s.option == option)
            return &s;
    }
    return nullptr;
}

QStringList ModelType::fileOptions(const QString &modelOption) const
{
    QStringList out{modelOption};
    // Components in the server form's order.
    for (const QString &option : sdFileOptions()) {
        if (slot(option) || (option == QLatin1String("vae") && !optionalVae.isEmpty()))
            out << option;
    }
    if (!controlNetFamilies.isEmpty())
        out << QStringLiteral("control_net");
    if (!ipAdapterFamilies.isEmpty())
        out << QStringLiteral("ip_adapter") << QStringLiteral("clip_vision");
    if (loras)
        out << QStringLiteral("lora_dir");
    out << QStringLiteral("upscalers_dir");
    return out;
}

bool ModelType::contains(const ModelPreset &preset) const
{
    return std::any_of(presets.cbegin(), presets.cend(), [&](const ModelPreset *p) { return p->id == preset.id; });
}

const QList<ModelType> &modelTypes()
{
    static const QList<ModelType> types = [] {
        QList<ModelType> out;
        for (const ModelPreset &p : modelPresets()) {
            auto it = std::find_if(out.begin(), out.end(), [&](const ModelType &t) { return t.id == p.type; });
            if (it == out.end()) {
                ModelType t;
                t.id = p.type;
                t.name = p.typeName;
                t.components = p.components;
                out << t;
                it = std::prev(out.end());
            }
            it->presets << &p;
            for (ModelKind k : p.kinds) {
                if (!it->kinds.contains(k))
                    it->kinds << k;
            }
            if (it->optionalVae.isEmpty())
                it->optionalVae = p.optionalVae;
            for (const QString &f : p.controlNetFamilies) {
                if (!it->controlNetFamilies.contains(f))
                    it->controlNetFamilies << f;
            }
            for (const QString &f : p.ipAdapterFamilies) {
                if (!it->ipAdapterFamilies.contains(f))
                    it->ipAdapterFamilies << f;
            }
            it->loras = it->loras || p.loras;
        }
        std::stable_sort(out.begin(), out.end(), [](const ModelType &a, const ModelType &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
        return out;
    }();
    return types;
}

const ModelType *modelTypeById(const QString &id)
{
    for (const ModelType &t : modelTypes()) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

const ModelType *modelTypeOf(const ModelPreset &preset)
{
    return modelTypeById(preset.type);
}

QStringList sdFileOptions()
{
    return {QStringLiteral("model"),       QStringLiteral("diffusion_model"), QStringLiteral("vae"),         QStringLiteral("clip_l"),
            QStringLiteral("clip_g"),      QStringLiteral("t5xxl"),           QStringLiteral("llm"),         QStringLiteral("control_net"),
            QStringLiteral("ip_adapter"),  QStringLiteral("clip_vision"),     QStringLiteral("lora_dir"),    QStringLiteral("upscalers_dir")};
}

const ModelType *profileModelType(const ModelLibrary *library, const ServerProfile &profile)
{
    auto str = [&profile](const QString &key) { return profile.options.value(key).toString().trimmed(); };
    const QString checkpoint = str(QStringLiteral("model"));
    const QString diffusion = str(QStringLiteral("diffusion_model"));
    // One model, in one slot.
    if (checkpoint.isEmpty() == diffusion.isEmpty())
        return nullptr;
    const QString file = diffusion.isEmpty() ? checkpoint : diffusion;
    const std::optional<ModelInfo> info = modelInfoOf(library, file);
    const ModelPreset *preset = info ? presetFor(*info, file) : nullptr;
    const ModelType *type = preset ? modelTypeOf(*preset) : nullptr;
    if (!type)
        return nullptr;
    const QStringList options = type->fileOptions(type->modelOption(info));
    for (const QString &option : sdFileOptions()) {
        if (!str(option).isEmpty() && !options.contains(option))
            return nullptr;
    }
    return type;
}

QString fileFieldLabel(const QString &option, const ModelType *type)
{
    const PresetSlot *slot = type ? type->slot(option) : nullptr;
    if (option == QLatin1String("vae") && type && !slot && !type->optionalVae.isEmpty())
        return Text::tr("VAE (optional)");
    if (option == QLatin1String("llm") && slot && !slot->variant.isEmpty())
        return Text::tr("Text encoder (%1)").arg(slot->variant);
    return optionLabel(option);
}

const PresetSlot *presetSlotForSha(const QString &sha256)
{
    if (sha256.isEmpty())
        return nullptr;
    for (const ModelPreset &p : modelPresets()) {
        for (const PresetSlot &s : p.components) {
            if (s.file.sha256 == sha256.toLower())
                return &s;
        }
    }
    return nullptr;
}

std::optional<ModelInfo> modelInfoOf(const ModelLibrary *library, const QString &path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (clean.isEmpty())
        return std::nullopt;
    if (library) {
        if (const auto e = library->entry(clean))
            return e->info;
    }
    if (!QFileInfo::exists(clean))
        return std::nullopt;
    return inspectModel(clean);
}

QString mainModelOption(const ModelInfo &info)
{
    if (info.kind == ModelKind::Checkpoint)
        return QStringLiteral("model");
    if (info.kind == ModelKind::DiffusionModel)
        return QStringLiteral("diffusion_model");
    return {};
}

QString slotStandIn(const ModelLibrary *library, const PresetSlot &slot)
{
    if (!library)
        return {};
    if (const auto same = library->find(slot.file.sha256, slot.file.fileName(), slot.file.size))
        return same->path;
    QString best;
    const QString stem = QFileInfo(slot.file.path).completeBaseName();
    for (const ModelEntry &e : library->entries(slot.kind)) {
        if (e.info.family != slot.family || (!slot.variant.isEmpty() && e.info.variant != slot.variant))
            continue;
        // Prefer one named like the pinned file (qwen_image_vae_fp8 over wan_2.1_vae).
        if (best.isEmpty() || QFileInfo(e.path).fileName().startsWith(stem, Qt::CaseInsensitive))
            best = e.path;
    }
    return best;
}

QVariantMap modelServerOptions(const ModelLibrary *library, const QString &path, std::optional<VramTier> tier)
{
    const std::optional<ModelInfo> info = modelInfoOf(library, path);
    if (!info)
        return {};
    const QString option = mainModelOption(*info);
    if (option.isEmpty())
        return {};
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QVariantMap out;
    out.insert(option, QDir::toNativeSeparators(clean));
    out.insert(option == QLatin1String("model") ? QStringLiteral("diffusion_model") : QStringLiteral("model"), QString());
    // Components: recorded with a download (Civitai), else the preset's.
    out.insert(companionServerOptions(library, clean));
    if (const ModelPreset *preset = presetFor(*info, clean)) {
        if (tier && preset->vram.contains(*tier))
            out.insert(preset->vram.value(*tier));
        out.insert(preset->serverOptions);
    }
    return out;
}

bool addonFits(const ModelPreset &preset, const ModelInfo &addon)
{
    if (addon.kind == ModelKind::ControlNet)
        return preset.controlNetFamilies.contains(addon.family);
    if (addon.kind == ModelKind::IpAdapter)
        return preset.ipAdapterFamilies.contains(addon.family);
    return true;
}

QList<ProfileProblem> checkSdProfile(const ModelLibrary *library, const ServerProfile &profile)
{
    QList<ProfileProblem> out;
    auto str = [&profile](const QString &key) { return profile.options.value(key).toString().trimmed(); };
    // Every file the profile names must exist.
    for (const QString &key : {QStringLiteral("model"), QStringLiteral("diffusion_model"), QStringLiteral("vae"), QStringLiteral("clip_l"),
                               QStringLiteral("clip_g"), QStringLiteral("t5xxl"), QStringLiteral("llm"), QStringLiteral("control_net"),
                               QStringLiteral("ip_adapter"), QStringLiteral("clip_vision")}) {
        const QString file = str(key);
        if (!file.isEmpty() && !QFileInfo::exists(QDir::fromNativeSeparators(file)))
            out << ProfileProblem{key, Text::tr("%1 (%2) does not exist.").arg(fileName(file), optionLabel(key)), false, Text::tr("This file does not exist.")};
    }
    if (!out.isEmpty())
        return out;

    // The model in its slot.
    QString modelPath;
    std::optional<ModelInfo> model;
    bool wrongSlot = false;
    for (const QString &key : {QStringLiteral("model"), QStringLiteral("diffusion_model")}) {
        const QString file = str(key);
        if (file.isEmpty())
            continue;
        const auto info = modelInfoOf(library, file);
        if (!info)
            continue;
        if (info->kind == ModelKind::DiffusionModel && key == QLatin1String("model")) {
            out << ProfileProblem{key,
                                  Text::tr("%1 is a diffusion model (%2), not a complete checkpoint: choose it under \"%3\", with its VAE and "
                                           "text encoder.")
                                      .arg(fileName(file), describe(*info), optionLabel(QStringLiteral("diffusion_model"))),
                                  false,
                                  Text::tr("This is a diffusion model (%1), not a complete checkpoint: it belongs under \"%2\".")
                                      .arg(describe(*info), optionLabel(QStringLiteral("diffusion_model")))};
            wrongSlot = true;
        } else if (info->kind == ModelKind::Checkpoint && key == QLatin1String("diffusion_model")) {
            out << ProfileProblem{key,
                                  Text::tr("%1 is a complete checkpoint (%2): choose it under \"%3\".")
                                      .arg(fileName(file), describe(*info), optionLabel(QStringLiteral("model"))),
                                  false,
                                  Text::tr("This is a complete checkpoint (%1): it belongs under \"%2\".").arg(describe(*info), optionLabel(QStringLiteral("model")))};
            wrongSlot = true;
        } else if (info->kind != ModelKind::Checkpoint && info->kind != ModelKind::DiffusionModel && info->kind != ModelKind::Unknown) {
            out << ProfileProblem{key, Text::tr("%1 (%2) is not an image model.").arg(fileName(file), modelKindLabel(info->kind)), false,
                                  Text::tr("This is a %1, not an image model.").arg(whatFileIs(*info))};
        }
        if (!model) {
            model = info;
            modelPath = file;
        }
    }
    if (!model)
        return out;
    const ModelPreset *preset = presetFor(*model, modelPath);
    if (!preset)
        return out;

    // Its components: there, and of the kind it needs (after a wrong slot, whose message asks for them).
    // The fields are named as the server dialog names them for the model's type.
    const ModelType *type = modelTypeOf(*preset);
    for (const PresetSlot &slot : wrongSlot ? QList<PresetSlot>() : preset->components) {
        const QString file = str(slot.option);
        if (file.isEmpty()) {
            if (slot.required)
                out << ProfileProblem{slot.option,
                                      Text::tr("%1 needs a %2 under \"%3\" (%4, %5).")
                                          .arg(preset->name, slot.label, fileFieldLabel(slot.option, type), slot.file.fileName(),
                                               formatBytes(slot.file.size)),
                                      true, Text::tr("Missing: %1 (%2).").arg(slot.file.fileName(), formatBytes(slot.file.size))};
            continue;
        }
        const auto info = modelInfoOf(library, file);
        if (!info || info->kind == ModelKind::Unknown)
            continue;
        if (info->kind != slot.kind || info->family != slot.family || (!slot.variant.isEmpty() && info->variant != slot.variant))
            out << ProfileProblem{slot.option,
                                  Text::tr("%1 (%2: %3) does not fit: %4 needs a %5 under \"%6\".")
                                      .arg(fileName(file), modelKindLabel(info->kind), describe(*info), preset->name, slot.label,
                                           fileFieldLabel(slot.option, type)),
                                  false, Text::tr("This is a %1; %2 needs a %3.").arg(whatFileIs(*info), preset->name, slot.label)};
    }
    // A VAE in place of a checkpoint's own: one of the family the checkpoint can take.
    if (const QString file = str(QStringLiteral("vae")); !wrongSlot && !file.isEmpty() && !preset->slot(QStringLiteral("vae")) && !preset->optionalVae.isEmpty()) {
        const auto info = modelInfoOf(library, file);
        if (info && info->kind != ModelKind::Unknown && (info->kind != ModelKind::Vae || (!info->family.isEmpty() && info->family != preset->optionalVae)))
            out << ProfileProblem{QStringLiteral("vae"),
                                  Text::tr("%1 (%2) does not fit: %3 takes only a VAE for %4 under \"%5\".")
                                      .arg(fileName(file), whatFileIs(*info), preset->name, preset->optionalVae, fileFieldLabel(QStringLiteral("vae"), type)),
                                  false, Text::tr("This is a %1; %2 takes only a VAE for %3.").arg(whatFileIs(*info), preset->name, preset->optionalVae)};
    }
    // Add-ons of the model's family.
    for (const QString &key : {QStringLiteral("control_net"), QStringLiteral("ip_adapter")}) {
        const QString file = str(key);
        if (file.isEmpty())
            continue;
        const auto info = modelInfoOf(library, file);
        if (!info || info->family.isEmpty() || addonFits(*preset, *info))
            continue;
        const QStringList fitting = key == QLatin1String("control_net") ? preset->controlNetFamilies : preset->ipAdapterFamilies;
        out << ProfileProblem{key,
                              fitting.isEmpty() ? Text::tr("%1 is for %2 models and does not work with %3. Remove it from \"%4\".")
                                                      .arg(fileName(file), info->family, preset->name, optionLabel(key))
                                                : Text::tr("%1 is for %2 models; %3 needs one for %4.")
                                                      .arg(fileName(file), info->family, preset->name, fitting.join(QStringLiteral(", "))),
                              false,
                              fitting.isEmpty() ? Text::tr("This is for %1 models and does not work with %2.").arg(info->family, preset->name)
                                                : Text::tr("This is for %1 models; %2 needs one for %3.")
                                                      .arg(info->family, preset->name, fitting.join(QStringLiteral(", ")))};
    }
    return out;
}

} // namespace lumen::runtime
