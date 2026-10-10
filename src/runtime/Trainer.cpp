#include "runtime/Trainer.h"

#include "core/Async.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "net/HttpClient.h"
#include "runtime/Download.h"
#include "runtime/Zip.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QPromise>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTimer>
#include <QVersionNumber>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::Trainer)
};

PinnedDownload pinned(const QJsonObject &o)
{
    return {QUrl(o.value(QStringLiteral("url")).toString()), qint64(o.value(QStringLiteral("size")).toDouble(-1)),
            o.value(QStringLiteral("sha256")).toString().toLower()};
}

} // namespace

const TrainerVariant *TrainerRecipe::variant(const QString &id) const
{
    for (const TrainerVariant &v : variants) {
        if (v.id == id)
            return &v;
    }
    return nullptr;
}

const TrainerRecipe *TrainerCatalog::trainer(const QString &id) const
{
    for (const TrainerRecipe &t : trainers) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

TrainerCatalog TrainerCatalog::fromJson(const QByteArray &json)
{
    TrainerCatalog c;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject uv = root.value(QStringLiteral("uv")).toObject();
    c.uvVersion = uv.value(QStringLiteral("version")).toString();
    c.uv = pinned(uv);
    for (const QJsonValue &v : root.value(QStringLiteral("trainers")).toArray()) {
        const QJsonObject o = v.toObject();
        TrainerRecipe r;
        r.id = o.value(QStringLiteral("id")).toString();
        r.name = o.value(QStringLiteral("name")).toString();
        r.description = o.value(QStringLiteral("description")).toString();
        r.homepage = QUrl(o.value(QStringLiteral("homepage")).toString());
        r.license = o.value(QStringLiteral("license")).toString();
        r.version = o.value(QStringLiteral("version")).toString();
        r.commit = o.value(QStringLiteral("commit")).toString();
        r.python = o.value(QStringLiteral("python")).toString();
        const QJsonObject src = o.value(QStringLiteral("source")).toObject();
        r.source = pinned(src);
        r.sourceRoot = src.value(QStringLiteral("root")).toString();
        for (const QJsonValue &vv : o.value(QStringLiteral("variants")).toArray()) {
            const QJsonObject vo = vv.toObject();
            TrainerVariant tv;
            tv.id = vo.value(QStringLiteral("id")).toString();
            tv.label = vo.value(QStringLiteral("label")).toString();
            tv.minCuda = vo.value(QStringLiteral("minCuda")).toString();
            tv.torch = vo.value(QStringLiteral("torch")).toString();
            tv.lock = vo.value(QStringLiteral("lock")).toString();
            tv.downloadSize = qint64(vo.value(QStringLiteral("downloadSize")).toDouble());
            r.variants << tv;
        }
        if (!r.id.isEmpty())
            c.trainers << r;
    }
    return c;
}

const TrainerCatalog &TrainerCatalog::instance()
{
    static const TrainerCatalog catalog = [] {
        QFile f(QStringLiteral(":/runtimes/trainers.json"));
        return f.open(QIODevice::ReadOnly) ? fromJson(f.readAll()) : TrainerCatalog();
    }();
    return catalog;
}

QByteArray trainerLock(const TrainerVariant &variant)
{
    QFile f(QStringLiteral(":/runtimes/trainers/") + variant.lock);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString recommendedTrainerVariant(const TrainerRecipe &recipe, const HardwareInfo &hw, QString *why)
{
    if (hw.nvidiaDriver.isEmpty() || hw.cudaVersion.isNull()) {
        if (why)
            *why = Text::tr("Training needs an NVIDIA GPU with a current driver.");
        return {};
    }
    // The newest build the driver supports (variants are listed newest first).
    for (const TrainerVariant &v : recipe.variants) {
        if (QVersionNumber::compare(hw.cudaVersion, QVersionNumber::fromString(v.minCuda)) >= 0)
            return v.id;
    }
    if (why)
        *why = Text::tr("The NVIDIA driver supports CUDA %1; training needs %2 or newer. Update the driver.")
                   .arg(hw.cudaVersion.toString(), recipe.variants.isEmpty() ? QString() : recipe.variants.last().minCuda);
    return {};
}

bool isTrainerKind(const QString &kind)
{
    return TrainerCatalog::instance().trainer(kind) != nullptr || kind == QLatin1String("kohya");
}

std::optional<InstalledRuntime> detectTrainer(const QString &path, QString *error)
{
    const QFileInfo fi(path);
    QDir dir(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath());
    // kohya_ss (the GUI) keeps sd-scripts in a sub-folder.
    if (!QFileInfo::exists(dir.filePath(QStringLiteral("sdxl_train_network.py"))) && QFileInfo::exists(dir.filePath(QStringLiteral("sd-scripts/sdxl_train_network.py"))))
        dir.setPath(dir.filePath(QStringLiteral("sd-scripts")));
    if (!QFileInfo::exists(dir.filePath(QStringLiteral("sdxl_train_network.py")))) {
        if (error)
            *error = Text::tr("%1 is not a kohya sd-scripts folder (sdxl_train_network.py is missing).").arg(QDir::toNativeSeparators(dir.absolutePath()));
        return std::nullopt;
    }
    QDir parent(dir);
    parent.cdUp();
    QString python;
    for (const QString &candidate : {dir.filePath(QStringLiteral("venv/Scripts/python.exe")), dir.filePath(QStringLiteral(".venv/Scripts/python.exe")),
                                     parent.filePath(QStringLiteral("venv/Scripts/python.exe")), parent.filePath(QStringLiteral(".venv/Scripts/python.exe")),
                                     dir.filePath(QStringLiteral("venv/bin/python")), parent.filePath(QStringLiteral("venv/bin/python"))}) {
        if (QFileInfo::exists(candidate)) {
            python = QDir::cleanPath(candidate);
            break;
        }
    }
    if (python.isEmpty()) {
        if (error)
            *error = Text::tr("No Python environment (venv) found next to %1. Run its setup first.").arg(QDir::toNativeSeparators(dir.absolutePath()));
        return std::nullopt;
    }
    InstalledRuntime rt;
    rt.kind = QStringLiteral("kohya");
    rt.managed = false;
    rt.directory = dir.absolutePath();
    rt.executable = python;
    rt.installed = QDateTime::currentDateTime();
    rt.settings.insert(QStringLiteral("scripts"), dir.absolutePath());
    return rt;
}

QFuture<ProcessResult> runProcess(const QString &program, const QStringList &arguments, const QProcessEnvironment &environment,
                                  const QString &workingDirectory, std::function<void(const QString &line)> line,
                                  const async::CancelToken &token)
{
    auto promise = std::make_shared<QPromise<ProcessResult>>();
    promise->start();
    QFuture<ProcessResult> future = promise->future();
    auto *process = new QProcess;
    process->setProgram(program);
    process->setArguments(arguments);
    process->setProcessEnvironment(environment);
    if (!workingDirectory.isEmpty())
        process->setWorkingDirectory(workingDirectory);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto result = std::make_shared<ProcessResult>();
    auto pending = std::make_shared<QByteArray>();
    auto *poll = new QTimer(process);
    poll->setInterval(250);
    QObject::connect(poll, &QTimer::timeout, process, [process, token] {
        if (token.isCancelled() && process->state() != QProcess::NotRunning)
            process->kill();
    });
    auto flush = [result, pending, line](bool all) {
        while (true) {
            qsizetype nl = pending->indexOf('\n');
            const qsizetype cr = pending->indexOf('\r');
            if (cr >= 0 && (nl < 0 || cr < nl))
                nl = cr;
            if (nl < 0) {
                if (!all || pending->isEmpty())
                    return;
                nl = pending->size();
            }
            const QString l = QString::fromUtf8(pending->left(nl)).trimmed();
            pending->remove(0, std::min(nl + 1, pending->size()));
            if (l.isEmpty())
                continue;
            result->output += l + QLatin1Char('\n');
            if (line)
                line(l);
        }
    };
    QObject::connect(process, &QProcess::readyRead, process, [process, pending, flush] {
        pending->append(process->readAll());
        flush(false);
    });
    QObject::connect(process, &QProcess::finished, process, [process, promise, result, pending, flush](int code, QProcess::ExitStatus status) {
        pending->append(process->readAll());
        flush(true);
        result->started = true;
        result->exitCode = status == QProcess::NormalExit ? code : -1;
        promise->addResult(*result);
        promise->finish();
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, promise, result](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        result->started = false;
        result->output += process->errorString();
        promise->addResult(*result);
        promise->finish();
        process->deleteLater();
    });
    poll->start();
    process->start();
    return future;
}

// ---- RuntimeStore::installTrainer ------------------------------------------------------------------

namespace {

// Drives one trainer install: uv → trainer source → Python → venv → locked packages → check.
class TrainerInstaller : public QObject
{
public:
    TrainerInstaller(RuntimeStore *store, TrainerRecipe recipe, TrainerVariant variant, PinnedDownload uv, QString uvVersion, Job *job)
        : QObject(job)
        , m_store(store)
        , m_recipe(std::move(recipe))
        , m_variant(std::move(variant))
        , m_uv(std::move(uv))
        , m_uvVersion(std::move(uvVersion))
        , m_job(job)
    {
    }

    void run()
    {
        QDir().mkpath(m_store->root());
        const QStorageInfo storage(m_store->root());
        const qint64 needed = m_variant.downloadSize * 2 + (1LL << 30); // wheels + unpacked + room
        if (storage.isValid() && storage.bytesAvailable() < needed)
            return m_job->fail(Text::tr("Not enough disk space: %1 needed on %2, %3 free.")
                                   .arg(formatBytes(needed), storage.rootPath(), formatBytes(storage.bytesAvailable())));
        m_lock = trainerLock(m_variant);
        if (m_lock.isEmpty())
            return m_job->fail(Text::tr("The package list for %1 is missing.").arg(m_variant.label));
        fetchUv();
    }

    void cancel()
    {
        m_cancelled = true;
        m_token.cancel();
        if (m_download)
            m_download->cancel();
    }

private:
    QString tools() const { return QDir(m_store->root()).filePath(QStringLiteral("tools")); }
    QString uvExe() const { return QDir(tools()).filePath(QStringLiteral("uv-%1/uv.exe").arg(m_uvVersion)); }
    QString target() const { return QDir(m_store->root()).filePath(QStringLiteral("%1/%2-%3").arg(m_recipe.id, m_recipe.version, m_variant.id)); }
    QString staging() const { return target() + QStringLiteral(".staging"); }
    QString venvPython() const { return QDir(staging()).filePath(QStringLiteral("venv/Scripts/python.exe")); }

    QProcessEnvironment uvEnvironment() const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("UV_PYTHON_INSTALL_DIR"), QDir::toNativeSeparators(QDir(tools()).filePath(QStringLiteral("python"))));
        env.insert(QStringLiteral("UV_CACHE_DIR"), QDir::toNativeSeparators(QDir(tools()).filePath(QStringLiteral("uv-cache"))));
        env.insert(QStringLiteral("UV_NO_CONFIG"), QStringLiteral("1"));       // ignore the user's uv settings
        env.insert(QStringLiteral("UV_LINK_MODE"), QStringLiteral("copy"));
        env.insert(QStringLiteral("UV_PYTHON_PREFERENCE"), QStringLiteral("only-managed"));
        env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
        env.remove(QStringLiteral("VIRTUAL_ENV"));
        env.remove(QStringLiteral("PYTHONHOME"));
        env.remove(QStringLiteral("PYTHONPATH"));
        return env;
    }

    void progress(double fraction, const QString &status) { m_job->setProgress(fraction, status); }

    bool stop()
    {
        if (m_cancelled) {
            m_job->reportCancelled();
            return true;
        }
        return false;
    }

    // Downloads a pinned file into downloads/ and calls next with its path.
    void download(const PinnedDownload &what, const QString &name, double from, double to, std::function<void(QString)> next)
    {
        DownloadSpec spec;
        spec.url = what.url;
        spec.destination = QDir(m_store->root()).filePath(QStringLiteral("downloads/") + name);
        spec.sha256 = what.sha256;
        spec.size = what.size;
        spec.headers << qMakePair(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("LumenStudio"));
        m_download = new FileDownload(m_store->http()->nam(), spec, this);
        connect(m_download, &FileDownload::progress, this, [this, from, to, name](qint64 received, qint64 total) {
            progress(from + (to - from) * (total > 0 ? double(received) / double(total) : 0.0), Text::tr("Downloading %1…").arg(name));
        });
        connect(m_download, &FileDownload::finished, this, [this, next] {
            FileDownload *d = m_download;
            m_download = nullptr;
            d->deleteLater();
            if (stop())
                return;
            if (!d->ok())
                return m_job->fail(d->errorString());
            next(d->spec().destination);
        });
        m_download->start();
    }

    // Unpacks a zip on a worker thread.
    void unpack(const QString &zip, const QString &dir, std::function<void()> next)
    {
        QPointer<TrainerInstaller> self(this);
        async::run([zip, dir] {
            QDir(dir).removeRecursively();
            return extractZip(zip, dir);
        }).then(this, [self, next, zip](const QString &error) {
            if (!self || self->stop())
                return;
            if (!error.isEmpty())
                return self->m_job->fail(Text::tr("Cannot unpack %1: %2").arg(QFileInfo(zip).fileName(), error));
            next();
        });
    }

    // Runs a command; non-zero exit fails the job with the end of its output.
    void command(const QString &program, const QStringList &args, const QString &status, double from, double to,
                 std::function<void(const ProcessResult &)> next, const QString &workDir = QString())
    {
        progress(from, status);
        QPointer<TrainerInstaller> self(this);
        auto lines = std::make_shared<int>(0);
        auto line = [self, status, from, to, lines](const QString &l) {
            if (!self)
                return;
            ++*lines;
            // uv reports "Downloading torch (2.3GiB)", "Downloaded torch", "Installed 68 packages".
            const double fraction = from + (to - from) * std::min(0.95, *lines / 140.0);
            self->progress(fraction, status + QStringLiteral(" — ") + l.left(90));
        };
        const ProcessRunner runner = m_store->processRunner ? m_store->processRunner : ProcessRunner(runProcess);
        runner(program, args, uvEnvironment(), workDir, line, m_token).then(this, [self, next, program](const ProcessResult &r) {
            if (!self || self->stop())
                return;
            if (!r.started)
                return self->m_job->fail(Text::tr("Cannot start %1: %2").arg(QFileInfo(program).fileName(), r.output.trimmed()));
            if (r.exitCode != 0) {
                const QStringList tail = r.output.trimmed().split(QLatin1Char('\n')).mid(std::max(0, int(r.output.count(QLatin1Char('\n'))) - 8));
                return self->m_job->fail(Text::tr("%1 failed (exit code %2):\n%3").arg(QFileInfo(program).fileName()).arg(r.exitCode).arg(tail.join(QLatin1Char('\n'))));
            }
            next(r);
        });
    }

    void fetchUv()
    {
        if (QFileInfo::exists(uvExe()))
            return fetchSource();
        download(m_uv, QStringLiteral("uv-%1-x86_64-pc-windows-msvc.zip").arg(m_uvVersion), 0.0, 0.02, [this](const QString &zip) {
            unpack(zip, QFileInfo(uvExe()).absolutePath(), [this] {
                if (!QFileInfo::exists(uvExe()))
                    return m_job->fail(Text::tr("The uv archive does not contain uv.exe."));
                fetchSource();
            });
        });
    }

    void fetchSource()
    {
        QDir(staging()).removeRecursively();
        download(m_recipe.source, QStringLiteral("%1-%2.zip").arg(m_recipe.id, m_recipe.commit.left(12)), 0.02, 0.04, [this](const QString &zip) {
            const QString unpacked = staging() + QStringLiteral("/source");
            unpack(zip, unpacked, [this, unpacked] {
                // The archive has one top folder; it becomes <install>/sd-scripts.
                if (!QDir().rename(QDir(unpacked).filePath(m_recipe.sourceRoot), QDir(staging()).filePath(QStringLiteral("sd-scripts"))))
                    return m_job->fail(Text::tr("The trainer's source archive has an unexpected layout."));
                QDir(unpacked).removeRecursively();
                installPython();
            });
        });
    }

    void installPython()
    {
        command(uvExe(), {QStringLiteral("python"), QStringLiteral("install"), m_recipe.python}, Text::tr("Installing Python %1").arg(m_recipe.python),
                0.04, 0.10, [this](const ProcessResult &) { makeVenv(); });
    }

    void makeVenv()
    {
        command(uvExe(), {QStringLiteral("venv"), QStringLiteral("--python"), m_recipe.python, QDir::toNativeSeparators(QDir(staging()).filePath(QStringLiteral("venv")))},
                Text::tr("Creating the Python environment"), 0.10, 0.12, [this](const ProcessResult &) { installPackages(); });
    }

    void installPackages()
    {
        const QString lockPath = QDir(staging()).filePath(QStringLiteral("requirements.lock.txt"));
        QFile f(lockPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(m_lock) != m_lock.size())
            return m_job->fail(Text::tr("Cannot write %1.").arg(QDir::toNativeSeparators(lockPath)));
        f.close();
        command(uvExe(),
                {QStringLiteral("pip"), QStringLiteral("install"), QStringLiteral("--python"), QDir::toNativeSeparators(venvPython()),
                 QStringLiteral("--require-hashes"), QStringLiteral("--no-deps"), QStringLiteral("--index-strategy"),
                 QStringLiteral("unsafe-best-match"), QStringLiteral("-r"), QDir::toNativeSeparators(lockPath)},
                Text::tr("Installing PyTorch and the trainer's packages (about %1)").arg(formatBytes(m_variant.downloadSize)), 0.12, 0.93,
                [this](const ProcessResult &) { verify(); });
    }

    void verify()
    {
        // Imports the trainer's own library too: that loads diffusers, transformers and accelerate.
        const QString script = QStringLiteral("import torch, library.train_util; print('LUMEN', torch.__version__, torch.cuda.is_available(), torch.version.cuda)");
        command(venvPython(), {QStringLiteral("-c"), script}, Text::tr("Checking PyTorch and the GPU"), 0.93, 0.98,
                [this](const ProcessResult &r) {
                    static const QRegularExpression re(QStringLiteral("LUMEN (\\S+) (True|False) (\\S+)"));
                    const QRegularExpressionMatch m = re.match(r.output);
                    if (!m.hasMatch())
                        return m_job->fail(Text::tr("The check of the Python environment printed nothing useful:\n%1").arg(r.output.right(600)));
                    if (!m.captured(1).startsWith(m_variant.torch))
                        return m_job->fail(Text::tr("PyTorch %1 was installed instead of %2.").arg(m.captured(1), m_variant.torch));
                    if (m.captured(2) != QLatin1String("True"))
                        return m_job->fail(Text::tr("PyTorch does not see the NVIDIA GPU. Update the NVIDIA driver (CUDA %1 or newer) and try again.")
                                               .arg(m_variant.minCuda));
                    finish(m.captured(1), m.captured(3));
                },
                QDir(staging()).filePath(QStringLiteral("sd-scripts")));
    }

    void finish(const QString &torch, const QString &cuda)
    {
        progress(0.99, Text::tr("Finishing…"));
        const QString finalDir = target();
        QDir(finalDir).removeRecursively();
        if (!QDir().rename(staging(), finalDir))
            return m_job->fail(Text::tr("Cannot move the new files into %1.").arg(QDir::toNativeSeparators(finalDir)));
        // uv's download cache (several GB) is not needed once the environment exists.
        QDir(QDir(tools()).filePath(QStringLiteral("uv-cache"))).removeRecursively();
        InstalledRuntime rt;
        rt.id = QStringLiteral("%1/%2-%3").arg(m_recipe.id, m_recipe.version, m_variant.id);
        rt.kind = m_recipe.id;
        rt.managed = true;
        rt.version = m_recipe.version;
        rt.backend = m_variant.id;
        rt.directory = QDir::cleanPath(finalDir);
        rt.executable = QDir::cleanPath(QDir(finalDir).filePath(QStringLiteral("venv/Scripts/python.exe")));
        rt.installed = QDateTime::currentDateTime();
        rt.versionInfo = QStringLiteral("%1 %2 · PyTorch %3 · CUDA %4").arg(m_recipe.name, m_recipe.version, torch, cuda);
        rt.settings.insert(QStringLiteral("scripts"), QDir::cleanPath(QDir(finalDir).filePath(QStringLiteral("sd-scripts"))));
        rt.settings.insert(QStringLiteral("torch"), torch);
        m_store->addInstalled(rt);
        m_job->succeed(rt.id);
    }

    RuntimeStore *m_store;
    TrainerRecipe m_recipe;
    TrainerVariant m_variant;
    PinnedDownload m_uv;
    QString m_uvVersion;
    Job *m_job;
    QByteArray m_lock;
    QPointer<FileDownload> m_download;
    async::CancelToken m_token;
    bool m_cancelled = false;
};

} // namespace

void RuntimeStore::setTrainerCatalog(const TrainerCatalog &catalog)
{
    m_trainerCatalog = std::make_shared<TrainerCatalog>(catalog);
}

Job *RuntimeStore::installTrainer(const QString &trainerId, const QString &variantId)
{
    const TrainerCatalog catalog = m_trainerCatalog ? *m_trainerCatalog : TrainerCatalog::instance();
    const TrainerRecipe *recipe = catalog.trainer(trainerId);
    const TrainerVariant *variant = recipe ? recipe->variant(variantId) : nullptr;
    const QString title = Text::tr("Install %1 %2").arg(recipe ? recipe->name : trainerId, recipe ? recipe->version : QString());
    m_jobs->setConcurrency(QStringLiteral("runtime-install"), 1);
    QPointer<RuntimeStore> self(this);
    const std::optional<TrainerRecipe> r = recipe ? std::optional<TrainerRecipe>(*recipe) : std::nullopt;
    const std::optional<TrainerVariant> v = variant ? std::optional<TrainerVariant>(*variant) : std::nullopt;
    return m_jobs->submit(title, QStringLiteral("runtime-install"), [self, r, v, catalog](Job *job) {
        if (!self)
            return job->fail(Text::tr("Shutting down."));
        if (!r || !v)
            return job->fail(Text::tr("Unknown trainer."));
        auto *installer = new TrainerInstaller(self, *r, *v, catalog.uv, catalog.uvVersion, job);
        job->setCancelHandler([installer = QPointer<TrainerInstaller>(installer)] {
            if (installer)
                installer->cancel();
        });
        installer->run();
    }, QStringLiteral("install"));
}

} // namespace lumen::runtime
