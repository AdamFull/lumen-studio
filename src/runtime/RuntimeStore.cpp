#include "runtime/RuntimeStore.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "net/HttpClient.h"
#include "runtime/Download.h"
#include "runtime/Zip.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QPromise>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>

#include <atomic>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::RuntimeStore)
};

const QString kCompleteMarker = QStringLiteral(".lumen-complete");

QString backendLabel(const QString &kind, const QString &backend)
{
    if (const RuntimeSpec *spec = Catalog::instance().runtime(kind)) {
        if (const BackendSpec *b = spec->backend(backend))
            return b->label;
    }
    return backend;
}

} // namespace

// ---- InstalledRuntime -----------------------------------------------------------------------------

QString InstalledRuntime::displayName() const
{
    const RuntimeSpec *spec = Catalog::instance().runtime(kind);
    QString name = spec ? spec->name : kind;
    if (!version.isEmpty())
        name += QLatin1Char(' ') + version;
    if (managed && !backend.isEmpty())
        name += QStringLiteral(" (%1)").arg(backendLabel(kind, backend));
    return name;
}

QJsonObject InstalledRuntime::toJson() const
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("managed"), managed},
                       {QStringLiteral("version"), version},
                       {QStringLiteral("backend"), backend},
                       {QStringLiteral("directory"), directory},
                       {QStringLiteral("executable"), executable},
                       {QStringLiteral("libraryDirs"), QJsonArray::fromStringList(libraryDirs)},
                       {QStringLiteral("installed"), installed.toString(Qt::ISODate)},
                       {QStringLiteral("versionInfo"), versionInfo},
                       {QStringLiteral("assets"), assets}};
}

InstalledRuntime InstalledRuntime::fromJson(const QJsonObject &o)
{
    InstalledRuntime rt;
    rt.id = o.value(QStringLiteral("id")).toString();
    rt.kind = o.value(QStringLiteral("kind")).toString();
    rt.managed = o.value(QStringLiteral("managed")).toBool();
    rt.version = o.value(QStringLiteral("version")).toString();
    rt.backend = o.value(QStringLiteral("backend")).toString();
    rt.directory = o.value(QStringLiteral("directory")).toString();
    rt.executable = o.value(QStringLiteral("executable")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("libraryDirs")).toArray())
        rt.libraryDirs << v.toString();
    rt.installed = QDateTime::fromString(o.value(QStringLiteral("installed")).toString(), Qt::ISODate);
    rt.versionInfo = o.value(QStringLiteral("versionInfo")).toString();
    rt.assets = o.value(QStringLiteral("assets")).toArray();
    return rt;
}

QProcessEnvironment runtimeEnvironment(const InstalledRuntime &rt)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList path;
    for (const QString &dir : rt.libraryDirs)
        path << QDir::toNativeSeparators(dir);
    path << QDir::toNativeSeparators(QFileInfo(rt.executable).absolutePath());
    const QString sep = QDir::listSeparator();
    env.insert(QStringLiteral("PATH"), path.join(sep) + sep + env.value(QStringLiteral("PATH")));
    return env;
}

QFuture<QString> probeVersion(const InstalledRuntime &rt, const QStringList &args)
{
    auto promise = std::make_shared<QPromise<QString>>();
    promise->start();
    QFuture<QString> future = promise->future();
    auto *proc = new QProcess;
    proc->setProgram(rt.executable);
    proc->setArguments(args.isEmpty() ? QStringList{QStringLiteral("--version")} : args);
    proc->setProcessEnvironment(runtimeEnvironment(rt));
    proc->setWorkingDirectory(QFileInfo(rt.executable).absolutePath());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    auto settled = std::make_shared<bool>(false);
    auto settle = [promise, settled, proc](const QString &result, const QString &error) {
        if (*settled)
            return;
        *settled = true;
        if (error.isEmpty())
            promise->addResult(result);
        else
            promise->setException(std::make_exception_ptr(Error(error)));
        promise->finish();
        proc->deleteLater();
    };
    QObject::connect(proc, &QProcess::errorOccurred, proc, [proc, settle](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            settle({}, Text::tr("%1 cannot be started: %2").arg(QDir::toNativeSeparators(proc->program()), proc->errorString()));
    });
    QObject::connect(proc, &QProcess::finished, proc, [proc, settle](int code, QProcess::ExitStatus status) {
        const QString out = QString::fromLocal8Bit(proc->readAll());
        QString line;
        for (const QString &l : out.split(QLatin1Char('\n'))) {
            const QString t = l.trimmed();
            if (t.isEmpty() || t.startsWith(QLatin1String("ggml_")) || t.startsWith(QLatin1String("load_backend")))
                continue; // backend initialisation chatter
            line = t;
            break;
        }
        // Windows reports missing DLLs as 0xC0000135 (STATUS_DLL_NOT_FOUND).
        if (status == QProcess::CrashExit || quint32(code) == 0xC0000135u) {
            settle({}, quint32(code) == 0xC0000135u ? Text::tr("%1 cannot start: a required DLL is missing (for CUDA builds: the CUDA runtime "
                                                         "or a recent NVIDIA driver).").arg(QFileInfo(proc->program()).fileName())
                                                    : Text::tr("%1 crashed while reporting its version (exit code 0x%2).")
                                                          .arg(QFileInfo(proc->program()).fileName())
                                                          .arg(quint32(code), 8, 16, QLatin1Char('0')));
            return;
        }
        settle(line.left(200), {});
    });
    QTimer::singleShot(20000, proc, [proc, settle] {
        proc->kill();
        settle({}, Text::tr("%1 did not answer --version within 20 seconds.").arg(QFileInfo(proc->program()).fileName()));
    });
    proc->start();
    return future;
}

QStringList findExistingInstalls()
{
    QStringList found;
    auto add = [&found](const QString &path) {
        const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (QFileInfo(clean).isExecutable() && !found.contains(clean, Qt::CaseInsensitive))
            found << clean;
    };
    QStringList roots;
#ifdef Q_OS_WIN
    for (const char *var : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty())
            roots << v;
    }
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
        roots << QDir(local).filePath(QStringLiteral("Programs"));
#else
    roots << QStringLiteral("/opt") << QStringLiteral("/usr/local/bin");
#endif
    roots << QDir::homePath() << QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    for (const RuntimeSpec &spec : Catalog::instance().runtimes) {
        const QString base = QFileInfo(spec.executable).completeBaseName();
        const QString onPath = QStandardPaths::findExecutable(base);
        if (!onPath.isEmpty())
            add(onPath);
        for (const QString &root : std::as_const(roots)) {
            // One folder deep: "C:/Program Files/llama-b10964-bin-win-cuda-12.4-x64/llama-server.exe".
            const QDir dir(root);
            if (QFileInfo::exists(dir.filePath(spec.executable)))
                add(dir.filePath(spec.executable));
            for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString candidate = dir.filePath(sub + QLatin1Char('/') + spec.executable);
                if (QFileInfo::exists(candidate))
                    add(candidate);
            }
        }
    }
    return found;
}

// ---- Installer ------------------------------------------------------------------------------------

namespace {

// Drives one install job: downloads (resumable, verified) → unpack (worker thread) → version probe.
class Installer : public QObject
{
public:
    Installer(RuntimeStore *store, InstallPlan plan, Job *job)
        : QObject(job)
        , m_store(store)
        , m_plan(std::move(plan))
        , m_job(job)
    {
        m_assets = m_plan.libraries;
        m_assets << m_plan.main;
        m_total = std::max<qint64>(1, m_plan.downloadSize());
    }

    void run()
    {
        const RuntimeSpec *spec = Catalog::instance().runtime(m_plan.runtimeId);
        if (!spec) {
            m_job->fail(Text::tr("Unknown runtime %1.").arg(m_plan.runtimeId));
            return;
        }
        m_spec = *spec;
        QDir().mkpath(m_store->root());
        const QStorageInfo storage(m_store->root());
        const qint64 needed = m_plan.downloadSize() * 3; // archives + unpacked files
        if (storage.isValid() && storage.bytesAvailable() < needed) {
            m_job->fail(Text::tr("Not enough disk space: %1 needed on %2, %3 free.")
                            .arg(formatBytes(needed), storage.rootPath(), formatBytes(storage.bytesAvailable())));
            return;
        }
        nextDownload();
    }

    void cancel()
    {
        m_cancelled = true;
        m_token.cancel();
        if (m_download)
            m_download->cancel();
    }

private:
    QString downloadsDir() const { return QDir(m_store->root()).filePath(QStringLiteral("downloads")); }
    QString kindDir() const { return QDir(m_store->root()).filePath(m_spec.id); }
    QString libraryDir(const ReleaseAsset &a) const { return m_store->libraryDir(m_spec.id, a); }

    void nextDownload()
    {
        if (m_cancelled)
            return m_job->reportCancelled();
        if (m_index >= m_assets.size()) {
            unpack();
            return;
        }
        const ReleaseAsset &a = m_assets[m_index];
        // Shared CUDA runtimes already unpacked by an earlier install are reused as they are.
        if (m_index < m_plan.libraries.size() && m_store->hasLibrary(m_spec.id, a)) {
            m_done += a.size;
            ++m_index;
            nextDownload();
            return;
        }
        DownloadSpec ds;
        ds.url = a.url;
        ds.destination = QDir(downloadsDir()).filePath(a.name);
        ds.sha256 = a.sha256;
        ds.size = a.size;
        m_download = new FileDownload(m_store->http()->nam(), ds, this);
        m_job->setProgress(fraction(0), Text::tr("Downloading %1…").arg(a.name));
        connect(m_download, &FileDownload::progress, this, [this, a](qint64 received, qint64) {
            m_job->setProgress(fraction(received), Text::tr("Downloading %1 — %2 of %3")
                                                       .arg(a.name, formatBytes(m_done + received), formatBytes(m_total)));
        });
        connect(m_download, &FileDownload::finished, this, [this, a] {
            FileDownload *d = m_download;
            m_download = nullptr;
            d->deleteLater();
            if (m_cancelled)
                return m_job->reportCancelled();
            if (!d->ok())
                return m_job->fail(d->errorString());
            m_done += a.size;
            m_archives << d->spec().destination;
            ++m_index;
            nextDownload();
        });
        m_download->start();
    }

    double fraction(qint64 current) const { return 0.85 * double(m_done + current) / double(m_total); }

    void unpack()
    {
        struct Work
        {
            QString archive;
            QString target;
            bool library;
        };
        QList<Work> work;
        for (int i = 0; i < m_plan.libraries.size(); ++i) {
            const QString dir = libraryDir(m_plan.libraries[i]);
            m_libraryDirs << dir;
            if (!QFileInfo::exists(QDir(dir).filePath(kCompleteMarker)))
                work.append({QDir(downloadsDir()).filePath(m_plan.libraries[i].name), dir, true});
        }
        m_folder = QStringLiteral("%1-%2").arg(m_plan.tag, m_plan.backendId);
        const QString staging = QDir(kindDir()).filePath(QStringLiteral(".staging-") + m_folder);
        work.append({QDir(downloadsDir()).filePath(m_plan.main.name), staging, false});

        auto progress = std::make_shared<std::atomic<int>>(0);
        auto *poll = new QTimer(this);
        poll->setInterval(200);
        connect(poll, &QTimer::timeout, this, [this, progress] {
            m_job->setProgress(0.85 + 0.13 * progress->load() / 1000.0, Text::tr("Unpacking…"));
        });
        poll->start();
        const async::CancelToken token = m_token;
        QPointer<Installer> self(this);
        async::run([work, token, progress]() -> QString {
            for (int i = 0; i < work.size(); ++i) {
                const Work &w = work[i];
                QDir(w.target).removeRecursively();
                const QString err = extractZip(w.archive, w.target, [&](qint64 done, qint64 total) {
                    const double part = total > 0 ? double(done) / double(total) : 1.0;
                    progress->store(int(1000.0 * (i + part) / work.size()));
                    return !token.isCancelled();
                });
                if (!err.isEmpty()) {
                    QDir(w.target).removeRecursively();
                    return err;
                }
                if (w.library) {
                    QFile marker(QDir(w.target).filePath(kCompleteMarker));
                    marker.open(QIODevice::WriteOnly);
                }
            }
            return {};
        }).then(this, [self, poll, staging](const QString &error) {
            if (!self)
                return;
            poll->deleteLater();
            if (self->m_cancelled) {
                QDir(staging).removeRecursively();
                return self->m_job->reportCancelled();
            }
            if (!error.isEmpty())
                return self->m_job->fail(error);
            self->activate(staging);
        });
    }

    void activate(const QString &staging)
    {
        // The server executable sits at the top of the archive; search deeper just in case.
        QString exe = QDir(staging).filePath(m_spec.executable);
        if (!QFileInfo::exists(exe)) {
            exe.clear();
            QDirIterator it(staging, {m_spec.executable}, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext())
                exe = it.next();
        }
        if (exe.isEmpty()) {
            QDir(staging).removeRecursively();
            return m_job->fail(Text::tr("The archive does not contain %1.").arg(m_spec.executable));
        }
        const QString relativeExe = QDir(staging).relativeFilePath(exe);
        const QString finalDir = QDir(kindDir()).filePath(m_folder);
        if (QFileInfo::exists(finalDir) && !QDir(finalDir).removeRecursively()) {
            QDir(staging).removeRecursively();
            return m_job->fail(Text::tr("%1 is in use; stop its server and try again.").arg(QDir::toNativeSeparators(finalDir)));
        }
        if (!QDir().rename(staging, finalDir)) {
            QDir(staging).removeRecursively();
            return m_job->fail(Text::tr("Cannot move the new files into %1.").arg(QDir::toNativeSeparators(finalDir)));
        }
        for (const QString &archive : std::as_const(m_archives))
            QFile::remove(archive);

        InstalledRuntime rt;
        rt.id = m_spec.id + QLatin1Char('/') + m_folder;
        rt.kind = m_spec.id;
        rt.managed = true;
        rt.version = m_plan.tag;
        rt.backend = m_plan.backendId;
        rt.directory = finalDir;
        rt.executable = QDir(finalDir).filePath(relativeExe);
        rt.libraryDirs = m_libraryDirs;
        rt.installed = QDateTime::currentDateTime();
        QJsonArray assets;
        for (const ReleaseAsset &a : std::as_const(m_assets))
            assets.append(QJsonObject{{QStringLiteral("name"), a.name}, {QStringLiteral("sha256"), a.sha256}, {QStringLiteral("size"), double(a.size)}});
        rt.assets = assets;
        m_job->setProgress(0.98, Text::tr("Checking that %1 runs…").arg(m_spec.executable));
        QPointer<Installer> self(this);
        probeVersion(rt, m_spec.versionArgs)
            .then(this, [self, rt](QString version) mutable {
                if (!self)
                    return;
                rt.versionInfo = version;
                self->m_store->addInstalled(rt);
                self->m_job->succeed(rt.id);
            })
            .onFailed(this, [self, rt](const std::exception &e) mutable {
                if (!self)
                    return;
                // Keep the files (the user may fix the driver) but say why it won't start.
                rt.versionInfo = describeException(e);
                self->m_store->addInstalled(rt);
                self->m_job->fail(Text::tr("Installed, but the server does not start: %1").arg(describeException(e)));
            });
    }

    RuntimeStore *m_store;
    InstallPlan m_plan;
    Job *m_job;
    RuntimeSpec m_spec;
    QList<ReleaseAsset> m_assets;
    QStringList m_archives;
    QStringList m_libraryDirs;
    QString m_folder;
    QPointer<FileDownload> m_download;
    async::CancelToken m_token;
    int m_index = 0;
    qint64 m_done = 0;
    qint64 m_total = 1;
    bool m_cancelled = false;
};

} // namespace

// ---- RuntimeStore ---------------------------------------------------------------------------------

RuntimeStore::RuntimeStore(net::HttpClient *http, JobQueue *jobs, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_jobs(jobs)
    , m_feed(http)
    , m_root(QDir(paths::appData()).filePath(QStringLiteral("runtimes")))
{
}

void RuntimeStore::setRoot(const QString &root)
{
    m_root = root;
    m_feed.setCacheDir(QDir(root).filePath(QStringLiteral("github-cache")));
}

QString RuntimeStore::configPath() const
{
    return QDir(m_root).filePath(QStringLiteral("runtimes.json"));
}

void RuntimeStore::load()
{
    m_runtimes.clear();
    QFile f(configPath());
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("runtimes")).toArray()) {
            InstalledRuntime rt = InstalledRuntime::fromJson(v.toObject());
            if (!rt.id.isEmpty())
                m_runtimes << rt;
        }
    }
    emit changed();
}

void RuntimeStore::save() const
{
    QDir().mkpath(m_root);
    QJsonArray arr;
    for (const InstalledRuntime &rt : m_runtimes)
        arr.append(rt.toJson());
    QFile f(configPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("runtimes"), arr}}).toJson());
}

QList<InstalledRuntime> RuntimeStore::runtimes(const QString &kind) const
{
    QList<InstalledRuntime> out;
    for (const InstalledRuntime &rt : m_runtimes) {
        if (rt.kind == kind)
            out << rt;
    }
    return out;
}

std::optional<InstalledRuntime> RuntimeStore::runtime(const QString &id) const
{
    for (const InstalledRuntime &rt : m_runtimes) {
        if (rt.id == id)
            return rt;
    }
    return std::nullopt;
}

QString RuntimeStore::libraryDir(const QString &kind, const ReleaseAsset &asset) const
{
    return QDir(m_root).filePath(QStringLiteral("%1/lib/%2-%3").arg(kind, QFileInfo(asset.name).completeBaseName(), asset.sha256.left(12)));
}

bool RuntimeStore::hasLibrary(const QString &kind, const ReleaseAsset &asset) const
{
    return QFileInfo::exists(QDir(libraryDir(kind, asset)).filePath(kCompleteMarker));
}

qint64 RuntimeStore::pendingDownload(const InstallPlan &plan) const
{
    qint64 n = plan.main.size;
    for (const ReleaseAsset &a : plan.libraries) {
        if (!hasLibrary(plan.runtimeId, a))
            n += a.size;
    }
    return n;
}

std::optional<InstalledRuntime> RuntimeStore::latestManaged(const QString &kind) const
{
    std::optional<InstalledRuntime> best;
    for (const InstalledRuntime &rt : m_runtimes) {
        if (rt.kind == kind && rt.managed && (!best || rt.installed > best->installed))
            best = rt;
    }
    return best;
}

void RuntimeStore::addInstalled(const InstalledRuntime &rt)
{
    for (int i = 0; i < m_runtimes.size(); ++i) {
        if (m_runtimes[i].id == rt.id) {
            m_runtimes[i] = rt;
            save();
            emit changed();
            return;
        }
    }
    m_runtimes << rt;
    save();
    emit changed();
}

Job *RuntimeStore::install(const InstallPlan &plan)
{
    const RuntimeSpec *spec = Catalog::instance().runtime(plan.runtimeId);
    const QString title = Text::tr("Install %1 %2").arg(spec ? spec->name : plan.runtimeId, plan.tag);
    QPointer<RuntimeStore> self(this);
    m_jobs->setConcurrency(QStringLiteral("runtime-install"), 1);
    return m_jobs->submit(title, QStringLiteral("runtime-install"), [self, plan](Job *job) {
        if (!self)
            return job->fail(Text::tr("Shutting down."));
        auto *installer = new Installer(self, plan, job);
        job->setCancelHandler([installer = QPointer<Installer>(installer)] {
            if (installer)
                installer->cancel();
        });
        installer->run();
    }, QStringLiteral("install"));
}

QFuture<InstalledRuntime> RuntimeStore::adopt(const QString &executable)
{
    const QFileInfo fi(executable);
    const RuntimeSpec *spec = Catalog::instance().runtimeForExecutable(fi.fileName());
    if (!spec || !fi.exists()) {
        return QtFuture::makeExceptionalFuture<InstalledRuntime>(std::make_exception_ptr(
            Error(fi.exists() ? Text::tr("%1 is not a server Lumen can run. Choose llama-server or sd-server.").arg(fi.fileName())
                              : Text::tr("%1 does not exist.").arg(QDir::toNativeSeparators(executable)))));
    }
    const QString path = QDir::cleanPath(fi.absoluteFilePath());
    for (const InstalledRuntime &rt : std::as_const(m_runtimes)) {
        if (QFileInfo(rt.executable) == QFileInfo(path))
            return QtFuture::makeReadyValueFuture(rt);
    }
    InstalledRuntime rt;
    rt.id = QStringLiteral("adopted/") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    rt.kind = spec->id;
    rt.managed = false;
    rt.directory = fi.absolutePath();
    rt.executable = path;
    rt.installed = QDateTime::currentDateTime();
    QPointer<RuntimeStore> self(this);
    return probeVersion(rt, spec->versionArgs).then(this, [self, rt](QString version) mutable {
        rt.versionInfo = version;
        // llama.cpp: "version: 0.4.1-dev (build 10964, commit b29c606e2)" → "b10964" (its release tag);
        // older builds: "version: 6123 (abc1234)"; sd.cpp: "… version master-951-f89d9b1, …".
        static const QRegularExpression build(QStringLiteral(R"(build (\d+))"));
        static const QRegularExpression re(QStringLiteral(R"((?:version[:\s]+)(\S+))"), QRegularExpression::CaseInsensitiveOption);
        const auto b = build.match(version);
        const auto m = re.match(version);
        rt.version = b.hasMatch() ? QStringLiteral("b") + b.captured(1) : m.hasMatch() ? m.captured(1).remove(QLatin1Char(',')) : QString();
        if (rt.version.isEmpty()) {
            // Fall back to the folder name of release zips: "llama-b10964-bin-win-cuda-12.4-x64".
            static const QRegularExpression tag(QStringLiteral(R"(\b(b\d{3,}|master-\d+-[0-9a-f]+)\b)"));
            const auto t = tag.match(QFileInfo(rt.directory).fileName());
            rt.version = t.hasMatch() ? t.captured(1) : QString();
        }
        if (self)
            self->addInstalled(rt);
        return rt;
    });
}

bool RuntimeStore::remove(const QString &id, QString *error)
{
    for (int i = 0; i < m_runtimes.size(); ++i) {
        if (m_runtimes[i].id != id)
            continue;
        const InstalledRuntime rt = m_runtimes[i];
        if (rt.managed) {
            // Only ever delete inside our own runtimes folder.
            const QString root = QDir(m_root).canonicalPath();
            const QString dir = QDir(rt.directory).canonicalPath();
            if (!root.isEmpty() && dir.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) {
                if (!QDir(dir).removeRecursively()) {
                    if (error)
                        *error = Text::tr("Some files in %1 are in use. Stop the server and try again.").arg(QDir::toNativeSeparators(dir));
                    return false;
                }
            }
        }
        m_runtimes.removeAt(i);
        // Shared CUDA runtime folders nobody references any more.
        for (const QString &lib : rt.libraryDirs) {
            bool used = false;
            for (const InstalledRuntime &other : std::as_const(m_runtimes))
                used |= other.libraryDirs.contains(lib);
            const QString canon = QDir(lib).canonicalPath();
            if (!used && !canon.isEmpty() && canon.startsWith(QDir(m_root).canonicalPath() + QLatin1Char('/'), Qt::CaseInsensitive))
                QDir(canon).removeRecursively();
        }
        save();
        emit changed();
        return true;
    }
    if (error)
        *error = Text::tr("Unknown runtime.");
    return false;
}

} // namespace lumen::runtime
