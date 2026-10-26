#include "runtime/Components.h"

#include "core/Job.h"
#include "core/JobQueue.h"
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
#include <QtConcurrent/QtConcurrentRun>

namespace lumen::runtime {

namespace {

// One component's files, downloaded one after the other into a staging folder, unpacked, then moved
// into place. Lives as long as its job runs.
class Installer : public QObject
{
public:
    Installer(Components *components, net::HttpClient *http, ComponentSpec spec, QString folder, Job *job)
        : QObject(job)
        , m_components(components)
        , m_http(http)
        , m_spec(std::move(spec))
        , m_folder(std::move(folder))
        , m_staging(m_folder + QStringLiteral(".download"))
        , m_job(job)
    {
        m_total = std::max<qint64>(1, m_spec.downloadSize());
    }

    void run()
    {
        QDir().mkpath(m_staging);
        next();
    }

    void cancel()
    {
        m_cancelled = true;
        if (m_download)
            m_download->cancel();
    }

private:
    void next()
    {
        if (m_cancelled)
            return m_job->reportCancelled();
        if (m_index >= m_spec.files.size())
            return finish();
        const ComponentFile &f = m_spec.files.at(m_index);
        DownloadSpec d;
        d.url = f.url;
        d.sha256 = f.sha256;
        d.size = f.size > 0 ? f.size : -1;
        // An archive is kept beside the files it holds until they are taken out.
        d.destination = f.extract.isEmpty() ? QDir(m_staging).filePath(f.path) : QDir(m_staging).filePath(QStringLiteral("archive-%1.zip").arg(m_index));
        QDir().mkpath(QFileInfo(d.destination).absolutePath());
        if (QFileInfo::exists(d.destination)) {
            // Already downloaded and verified on an earlier try.
            m_done += f.size;
            return unpack(f, d.destination);
        }
        m_download = new FileDownload(m_http->nam(), d, this);
        connect(m_download, &FileDownload::progress, this, [this](qint64 received, qint64) {
            m_job->setProgress(double(m_done + received) / m_total,
                               QCoreApplication::translate("lumen::runtime::Components", "%1 of %2 MB").arg((m_done + received) / 1000000).arg(m_total / 1000000));
        });
        connect(m_download, &FileDownload::finished, this, [this, f, dest = d.destination] {
            FileDownload *dl = m_download;
            m_download = nullptr;
            dl->deleteLater();
            if (m_cancelled)
                return m_job->reportCancelled();
            if (!dl->ok())
                return m_job->fail(QCoreApplication::translate("lumen::runtime::Components", "%1 could not be downloaded: %2").arg(m_spec.name, dl->errorString()));
            m_done += f.size;
            unpack(f, dest);
        });
        m_download->start();
    }

    void unpack(const ComponentFile &f, const QString &archive)
    {
        if (f.extract.isEmpty()) {
            ++m_index;
            return next();
        }
        m_job->setStatus(QCoreApplication::translate("lumen::runtime::Components", "Unpacking…"));
        const QString staging = m_staging;
        QPointer<Installer> self(this);
        QtConcurrent::run([archive, staging, f]() -> QString {
            const QString out = staging + QStringLiteral("/unpacked");
            const QString e = extractZip(archive, out, {}, [&f](const QString &path) { return f.extract.contains(path); });
            if (!e.isEmpty())
                return e;
            for (auto it = f.extract.cbegin(); it != f.extract.cend(); ++it) {
                const QString to = QDir(staging).filePath(it.value());
                QDir().mkpath(QFileInfo(to).absolutePath());
                QFile::remove(to);
                if (!QFile::rename(QDir(out).filePath(it.key()), to))
                    return QCoreApplication::translate("lumen::runtime::Components", "%1 is not in the archive.").arg(it.key());
            }
            QDir(out).removeRecursively();
            QFile::remove(archive);
            return {};
        }).then(this, [self](const QString &error) {
            if (!self)
                return;
            if (!error.isEmpty())
                return self->m_job->fail(error);
            ++self->m_index;
            self->next();
        });
    }

    void finish()
    {
        // The finished folder replaces any older one at once.
        QDir(m_folder).removeRecursively();
        if (!QDir().rename(m_staging, m_folder))
            return m_job->fail(QCoreApplication::translate("lumen::runtime::Components", "%1 could not be put in place at %2.").arg(m_spec.name, QDir::toNativeSeparators(m_folder)));
        m_job->succeed(m_folder);
        if (m_components)
            emit m_components->installed(m_spec.id);
    }

    QPointer<Components> m_components;
    net::HttpClient *m_http;
    ComponentSpec m_spec;
    QString m_folder;
    QString m_staging;
    Job *m_job;
    QPointer<FileDownload> m_download;
    int m_index = 0;
    qint64 m_done = 0;
    qint64 m_total = 1;
    bool m_cancelled = false;
};

} // namespace

qint64 ComponentSpec::downloadSize() const
{
    qint64 n = 0;
    for (const ComponentFile &f : files)
        n += f.size;
    return n;
}

QStringList ComponentSpec::destinations() const
{
    QStringList out;
    for (const ComponentFile &f : files) {
        if (f.extract.isEmpty())
            out << f.path;
        else
            out << f.extract.values();
    }
    return out;
}

QList<ComponentSpec> parseComponents(const QByteArray &json, QString *error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (error)
            *error = pe.errorString();
        return {};
    }
    QList<ComponentSpec> out;
    for (const QJsonValue &v : doc.object().value(QStringLiteral("components")).toArray()) {
        const QJsonObject o = v.toObject();
        ComponentSpec c;
        c.id = o.value(QStringLiteral("id")).toString();
        c.name = o.value(QStringLiteral("name")).toString();
        c.license = o.value(QStringLiteral("license")).toString();
        c.folder = o.value(QStringLiteral("folder")).toString(c.id);
        c.inModels = o.value(QStringLiteral("location")).toString() == QLatin1String("models");
        // Files of a Hugging Face repository at a commit: "repo", "revision" and each file's "path".
        const QString repo = o.value(QStringLiteral("repo")).toString();
        const QString revision = o.value(QStringLiteral("revision")).toString();
        for (const QJsonValue &fv : o.value(QStringLiteral("files")).toArray()) {
            const QJsonObject fo = fv.toObject();
            ComponentFile f;
            f.path = fo.value(QStringLiteral("path")).toString();
            f.url = fo.contains(QStringLiteral("url")) ? QUrl(fo.value(QStringLiteral("url")).toString())
                                                       : QUrl(QStringLiteral("https://huggingface.co/%1/resolve/%2/%3").arg(repo, revision, f.path));
            f.sha256 = fo.value(QStringLiteral("sha256")).toString();
            f.size = qint64(fo.value(QStringLiteral("size")).toDouble());
            const QJsonObject extract = fo.value(QStringLiteral("extract")).toObject();
            for (auto it = extract.begin(); it != extract.end(); ++it)
                f.extract.insert(it.key(), it.value().toString());
            c.files << f;
        }
        if (c.id.isEmpty() || c.files.isEmpty()) {
            if (error)
                *error = QCoreApplication::translate("lumen::runtime::Components", "A component without an id or files.");
            return {};
        }
        out << c;
    }
    return out;
}

const QList<ComponentSpec> &componentSpecs()
{
    static const QList<ComponentSpec> specs = [] {
        QFile f(QStringLiteral(":/presets/components.json"));
        f.open(QIODevice::ReadOnly);
        return parseComponents(f.readAll());
    }();
    return specs;
}

Components::Components(net::HttpClient *http, JobQueue *jobs, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_jobs(jobs)
    , m_specs(componentSpecs())
{
}

void Components::setRoots(const QString &dataRoot, const QString &modelsRoot)
{
    m_dataRoot = dataRoot;
    m_modelsRoot = modelsRoot;
}

const ComponentSpec *Components::spec(const QString &id) const
{
    for (const ComponentSpec &s : m_specs) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

QString Components::folder(const QString &id) const
{
    const ComponentSpec *s = spec(id);
    if (!s)
        return {};
    return s->inModels ? QDir(m_modelsRoot).filePath(QStringLiteral("detection/") + s->folder)
                       : QDir(m_dataRoot).filePath(QStringLiteral("components/") + s->folder);
}

QString Components::path(const QString &id, const QString &destination) const
{
    return QDir(folder(id)).filePath(destination);
}

bool Components::isInstalled(const QString &id) const
{
    const ComponentSpec *s = spec(id);
    if (!s)
        return false;
    for (const QString &d : s->destinations()) {
        if (!QFileInfo::exists(path(id, d)))
            return false;
    }
    return true;
}

qint64 Components::missingSize(const QStringList &ids) const
{
    qint64 n = 0;
    for (const QString &id : ids) {
        if (const ComponentSpec *s = spec(id); s && !isInstalled(id))
            n += s->downloadSize();
    }
    return n;
}

Job *Components::install(const QString &id)
{
    if (QPointer<Job> running = m_running.value(id); running && !running->isFinished())
        return running;
    const ComponentSpec *s = spec(id);
    if (!s)
        return nullptr;
    const ComponentSpec spec = *s;
    const QString target = folder(id);
    QPointer<Components> self(this);
    Job *job = m_jobs->submit(QCoreApplication::translate("lumen::runtime::Components", "Download %1").arg(spec.name), QStringLiteral("download"), [self, spec, target](Job *job) {
        if (!self)
            return job->fail(QCoreApplication::translate("lumen::runtime::Components", "Shutting down."));
        auto *installer = new Installer(self, self->m_http, spec, target, job);
        job->setCancelHandler([installer = QPointer<Installer>(installer)] {
            if (installer)
                installer->cancel();
        });
        installer->run();
    }, QStringLiteral("download"));
    m_running.insert(id, job);
    return job;
}

} // namespace lumen::runtime
