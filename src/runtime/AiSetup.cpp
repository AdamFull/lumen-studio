#include "runtime/AiSetup.h"

#include "core/Async.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <climits>
#include <tuple>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::AiSetup)
};

constexpr double kGiB = 1024.0 * 1024 * 1024;

// A file a planner can use, with the found folder it is in ("" = in the library).
struct Candidate
{
    const ModelEntry *entry;
    QString folder;
};

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

bool isPinnedFile(const ModelEntry &e, const PresetFile &file)
{
    if (!file.isValid())
        return false;
    if (!e.sha256.isEmpty() && e.sha256.compare(file.sha256, Qt::CaseInsensitive) == 0)
        return true;
    return e.size == file.size && QFileInfo(e.path).fileName().compare(file.fileName(), Qt::CaseInsensitive) == 0;
}

// The file of a found folder or the library that fills a slot: the pinned file itself, else one of
// the slot's header family and variant, preferring one named like the pinned file, then one next
// to the model (the same ComfyUI folder).
std::optional<Candidate> standIn(const QList<Candidate> &all, const PresetSlot &slot, const QString &modelFolder)
{
    for (const Candidate &c : all) {
        if (c.entry->info.kind == slot.kind && isPinnedFile(*c.entry, slot.file))
            return c;
    }
    const QString stem = QFileInfo(slot.file.path).completeBaseName();
    std::optional<Candidate> best;
    int bestScore = -1;
    for (const Candidate &c : all) {
        const ModelInfo &info = c.entry->info;
        if (info.kind != slot.kind || info.family != slot.family || (!slot.variant.isEmpty() && info.variant != slot.variant))
            continue;
        const int score = (QFileInfo(c.entry->path).fileName().startsWith(stem, Qt::CaseInsensitive) ? 2 : 0) + (c.folder == modelFolder ? 1 : 0);
        if (score > bestScore) {
            best = c;
            bestScore = score;
        }
    }
    return best;
}

QList<PlannedFile> componentsFor(const QList<Candidate> &all, const ModelPreset &preset, const QString &modelFolder)
{
    QList<PlannedFile> out;
    for (const PresetSlot &slot : preset.components) {
        PlannedFile f;
        f.option = slot.option;
        f.label = slot.label;
        f.kind = slot.kind;
        f.required = slot.required;
        f.file = slot.file;
        if (const auto c = standIn(all, slot, modelFolder)) {
            f.path = c->entry->path;
            f.folder = c->folder;
        }
        out << f;
    }
    return out;
}

int missingRequired(const QList<PlannedFile> &files)
{
    return int(std::count_if(files.cbegin(), files.cend(), [](const PlannedFile &f) { return f.required && f.missing(); }));
}

} // namespace

const GpuInfo *imageGpu(const HardwareInfo &hw)
{
    const GpuInfo *best = nullptr;
    for (const GpuInfo &g : hw.gpus) {
        if (!(g.isNvidia() || g.isAmd()) || g.integrated)
            continue;
        if (!best || g.dedicatedVram > best->dedicatedVram)
            best = &g;
    }
    return best;
}

LocalRecommendation recommendLocalModel(const HardwareInfo &hw)
{
    LocalRecommendation r;
    const GpuInfo *gpu = imageGpu(hw);
    r.tier = vramTierFor(gpu ? gpu->dedicatedVram : 0);
    if (gpu && double(gpu->dedicatedVram) / kGiB >= kFullModelVramGb) {
        r.fit = LocalRecommendation::Fit::Full;
        r.presetId = QStringLiteral("flux2-klein-4b");
        r.alternativeId = QStringLiteral("sdxl");
        // Up to 8 GB the image model needs the whole card; a language model next to it would make
        // both slow (§12.1).
        r.languageModel = r.tier != VramTier::Small;
    } else {
        r.fit = gpu ? LocalRecommendation::Fit::Light : LocalRecommendation::Fit::NoGpu;
        r.presetId = QStringLiteral("sdxl");
        r.alternativeId = QStringLiteral("flux2-klein-4b");
    }
    return r;
}

QFuture<QList<FoundFolder>> readFolders(const QStringList &folders)
{
    return async::run([folders] {
        QList<FoundFolder> out;
        for (const QString &folder : folders) {
            if (!QFileInfo(folder).isDir())
                continue;
            out << FoundFolder{QDir::cleanPath(folder), ModelLibrary::readFolder(folder)};
        }
        return out;
    });
}

QList<PlannedFile> LocalModelPlan::downloads() const
{
    QList<PlannedFile> out;
    for (const PlannedFile &f : files) {
        if (f.missing() && f.file.isValid())
            out << f;
    }
    return out;
}

qint64 LocalModelPlan::downloadSize() const
{
    qint64 total = 0;
    for (const PlannedFile &f : downloads())
        total += f.file.size;
    return total;
}

QStringList LocalModelPlan::foldersToAdd() const
{
    QStringList out;
    for (const PlannedFile &f : files) {
        if (!f.missing() && !f.folder.isEmpty() && !out.contains(f.folder))
            out << f.folder;
    }
    return out;
}

LocalModelPlan planLocalModel(const ModelLibrary *library, const QList<FoundFolder> &found, const ModelPreset &preset)
{
    LocalModelPlan plan;
    plan.preset = preset;
    // Every file that could be used: the library's, then those of the found folders it lacks.
    const QList<ModelEntry> libraryEntries = library ? library->entries() : QList<ModelEntry>();
    QList<Candidate> all;
    for (const ModelEntry &e : libraryEntries)
        all << Candidate{&e, QString()};
    for (const FoundFolder &folder : found) {
        for (const ModelEntry &e : folder.entries) {
            const bool known = std::any_of(libraryEntries.cbegin(), libraryEntries.cend(), [&](const ModelEntry &l) { return samePath(l.path, e.path); });
            if (!known)
                all << Candidate{&e, folder.path};
        }
    }

    // The model: the candidate with the fewest missing components, then the pinned file, then one
    // the library has (no folder to add).
    std::optional<Candidate> model;
    QList<PlannedFile> components = componentsFor(all, preset, QString());
    std::tuple<int, int, int> bestKey{INT_MAX, INT_MAX, INT_MAX};
    for (const Candidate &c : std::as_const(all)) {
        const ModelInfo &info = c.entry->info;
        if (!preset.kinds.contains(info.kind))
            continue;
        // A file another preset claims first is not this model (klein base is not klein).
        const ModelPreset *own = presetFor(info, c.entry->path);
        if (own ? own->id != preset.id : !preset.matches(info, c.entry->path))
            continue;
        QList<PlannedFile> parts = componentsFor(all, preset, c.folder);
        const std::tuple<int, int, int> key{missingRequired(parts), isPinnedFile(*c.entry, preset.download) ? 0 : 1, c.folder.isEmpty() ? 0 : 1};
        if (key < bestKey) {
            bestKey = key;
            model = c;
            components = parts;
        }
    }

    PlannedFile main;
    main.label = preset.name;
    main.file = preset.download;
    if (model) {
        main.kind = model->entry->info.kind;
        main.path = model->entry->path;
        main.folder = model->folder;
    } else {
        main.kind = preset.kinds.value(0, ModelKind::Checkpoint);
        if (!preset.download.isValid())
            plan.error = Text::tr("No %1 file was found on this computer, and Lumen has none to download for it.").arg(preset.name);
    }
    ModelInfo kind;
    kind.kind = main.kind;
    main.option = mainModelOption(kind);
    plan.files << main << components;
    return plan;
}

HubDownload plannedDownload(const LocalModelPlan &plan, const PlannedFile &file, const QString &huggingFaceBase)
{
    StarterFile s;
    s.id = file.file.id;
    s.role = file.option;
    s.kind = file.kind;
    s.name = file.option == plan.model()->option ? plan.preset.name : QStringLiteral("%1 (%2)").arg(file.label, plan.preset.name);
    s.repo = file.file.repo;
    s.revision = file.file.revision;
    s.path = file.file.path;
    s.size = file.file.size;
    s.sha256 = file.file.sha256;
    s.license = file.file.license;
    return starterDownload(s, huggingFaceBase);
}

QVariantMap planServerOptions(const ModelLibrary *library, const LocalModelPlan &plan, const QHash<QString, QString> &paths, VramTier tier)
{
    const PlannedFile *main = plan.model();
    if (!main)
        return {};
    const QString model = QDir::cleanPath(QDir::fromNativeSeparators(paths.value(main->option, main->path)));
    QVariantMap out = presetOptions(QStringLiteral("sd.cpp"), tier);
    // The model in its slot with what the library knows of it (its own preset, recorded companions).
    out.insert(modelServerOptions(library, model, tier));
    out.insert(main->option, QDir::toNativeSeparators(model));
    out.insert(main->option == QLatin1String("model") ? QStringLiteral("diffusion_model") : QStringLiteral("model"), QString());
    // The plan's components: found or downloaded files the library may not have read yet.
    for (const PlannedFile &f : plan.files.mid(1)) {
        const QString path = paths.value(f.option, f.path);
        if (!path.isEmpty())
            out.insert(f.option, QDir::toNativeSeparators(QDir::cleanPath(path)));
    }
    if (plan.preset.vram.contains(tier))
        out.insert(plan.preset.vram.value(tier));
    out.insert(plan.preset.serverOptions);
    return out;
}

} // namespace lumen::runtime
