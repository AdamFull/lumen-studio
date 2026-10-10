#pragma once

#include "runtime/Hardware.h"
#include "runtime/RuntimeStore.h"

#include <QFuture>
#include <QProcessEnvironment>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

namespace lumen::runtime {

// LoRA trainers (M13 §13.5): Python programs Lumen installs into a private environment with uv,
// every download pinned (resources/runtimes/trainers.json and its hashed lock files).

struct PinnedDownload
{
    QUrl url;
    qint64 size = -1;
    QString sha256;
};

struct TrainerVariant
{
    QString id;          // "cu128"
    QString label;
    QString minCuda;     // lowest CUDA version the driver must support
    QString torch;       // PyTorch version the lock installs
    QString lock;        // lock file name next to trainers.json
    QString linuxLock;   // the same pins for Linux x86_64 (cloud GPU pods, M15); empty = none
    qint64 downloadSize = 0; // estimate of what uv downloads
};

struct TrainerRecipe
{
    QString id;          // "kohya" (the runtime kind)
    QString name;
    QString description;
    QUrl homepage;
    QString license;
    QString version;     // "v0.12.0"
    QString commit;
    QString python;      // exact CPython version uv installs ("3.10.21")
    PinnedDownload source;
    QString sourceRoot;  // top folder inside the source archive
    QList<TrainerVariant> variants;

    const TrainerVariant *variant(const QString &id) const;
};

struct TrainerCatalog
{
    QString uvVersion;
    PinnedDownload uv;
    PinnedDownload uvLinux; // uv for Linux x86_64 (cloud GPU pods)
    QList<TrainerRecipe> trainers;

    const TrainerRecipe *trainer(const QString &id) const;
    static TrainerCatalog fromJson(const QByteArray &json);
    static const TrainerCatalog &instance(); // the bundled list
};

// The requirements lock of a variant (bundled resource); empty when missing.
QByteArray trainerLock(const TrainerVariant &variant);
// The Linux lock of a variant (cloud GPU pods); empty when it has none.
QByteArray trainerLinuxLock(const TrainerVariant &variant);
// The variant for this computer: the newest CUDA build the NVIDIA driver supports; empty without
// a suitable NVIDIA GPU (*why says why).
QString recommendedTrainerVariant(const TrainerRecipe &recipe, const HardwareInfo &hw, QString *why = nullptr);

// Whether a runtime kind is a trainer (no server is run from it).
bool isTrainerKind(const QString &kind);

// Recognises an existing kohya sd-scripts checkout or kohya_ss install (folder or a script in it)
// with its Python environment (venv / .venv next to it or one level up).
std::optional<InstalledRuntime> detectTrainer(const QString &path, QString *error = nullptr);

// Runs a program; `line` receives output lines as they come. The token kills it.
QFuture<ProcessResult> runProcess(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment,
                                  const QString &workingDirectory, std::function<void(const QString &line)> line,
                                  const async::CancelToken &token);

} // namespace lumen::runtime
