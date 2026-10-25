#include "runtime/ModelsFolder.h"

#include "core/Async.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "runtime/Hardware.h"
#include "runtime/ModelFormats.h"
#include "runtime/ModelLibrary.h"
#include "runtime/Supervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStorageInfo>
#include <QTimer>

#include <atomic>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cstdio>
#endif

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ModelsFolder)
};

const QString kSidecarSuffix = QStringLiteral(".lumen.json");
const QString kPartSuffix = QStringLiteral(".part");
const QString kCopySuffix = QStringLiteral(".moving");
// Queue groups: one move at a time; downloads write into the folder.
const QString kMoveGroup = QStringLiteral("models-folder");
const QString kDownloadGroup = QStringLiteral("model-download");

Qt::CaseSensitivity pathCase()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString cleanAbsolute(const QString &path)
{
    const QString typed = QDir::fromNativeSeparators(path.trimmed());
    return typed.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(typed).absoluteFilePath());
}

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

// A rename that never copies and never replaces: a file in use or another drive is an error.
bool renameFile(const QString &from, const QString &to, QString *error)
{
#ifdef Q_OS_WIN
    if (MoveFileExW(reinterpret_cast<const wchar_t *>(native(from).utf16()), reinterpret_cast<const wchar_t *>(native(to).utf16()), 0))
        return true;
    *error = qt_error_string(int(GetLastError()));
    return false;
#else
    if (QFileInfo::exists(to)) {
        *error = Text::tr("A file of that name exists.");
        return false;
    }
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    *error = qt_error_string(errno);
    return false;
#endif
}

// Copies in chunks; chunk(copied) returns false to stop. The copy keeps the original's
// modification time.
QString copyFile(const QString &from, const QString &to, const std::function<bool(qint64 copied)> &chunk, bool *stopped)
{
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly))
        return Text::tr("Cannot read %1: %2").arg(native(from), in.errorString());
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Text::tr("Cannot write %1: %2").arg(native(to), out.errorString());
    QByteArray buffer(8 << 20, Qt::Uninitialized);
    qint64 copied = 0;
    for (;;) {
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0)
            return Text::tr("Cannot read %1: %2").arg(native(from), in.errorString());
        if (n == 0)
            break;
        if (out.write(buffer.constData(), n) != n)
            return Text::tr("Cannot write %1: %2").arg(native(to), out.errorString());
        copied += n;
        if (!chunk(copied)) {
            *stopped = true;
            return Text::tr("The move was cancelled.");
        }
    }
    if (!out.flush() || out.error() != QFileDevice::NoError)
        return Text::tr("Cannot write %1: %2").arg(native(to), out.errorString());
    out.close();
    // Set after the writes are closed, so closing does not touch it again.
    QFile stamp(to);
    if (stamp.open(QIODevice::ReadWrite))
        stamp.setFileTime(QFileInfo(from).lastModified(), QFileDevice::FileModificationTime);
    return {};
}

// Removes the folders left empty under `root`, deepest first, and `root` itself when empty.
// Links to folders are left alone.
void removeEmptyFolders(const QString &root)
{
    QStringList dirs;
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = it.next();
        const QFileInfo fi(dir);
        if (!fi.isSymLink() && !fi.isJunction())
            dirs << dir;
    }
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) { return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/')); });
    for (const QString &dir : std::as_const(dirs))
        QDir().rmdir(dir); // only empty ones go
    QDir().rmdir(root);
}

bool hasModelFiles(const QString &dir)
{
    QDirIterator it(dir, modelFileFilters(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (QFileInfo(it.next()).size() >= 1024 * 1024) // as the library's scan: placeholders do not count
            return true;
    }
    return false;
}

bool pathBoundary(QChar c)
{
    return c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('=') || c == QLatin1Char(',') || c == QLatin1Char(';');
}

// Paths into `from` inside free text (extra command-line arguments), in either separator style.
QString relocateInText(const QString &text, const QString &from, const QString &to)
{
    QString out = text;
    const QString cleanFrom = QDir::cleanPath(QDir::fromNativeSeparators(from));
    const QString cleanTo = QDir::cleanPath(QDir::fromNativeSeparators(to));
    QStringList forms{cleanFrom};
    if (native(cleanFrom) != cleanFrom)
        forms.prepend(native(cleanFrom));
    for (const QString &f : std::as_const(forms)) {
        const QString t = f == cleanFrom ? cleanTo : native(cleanTo);
        qsizetype i = 0;
        while ((i = out.indexOf(f, i, pathCase())) >= 0) {
            const qsizetype end = i + f.size();
            const QChar before = i > 0 ? out.at(i - 1) : QChar(QLatin1Char(' '));
            const QChar after = end < out.size() ? out.at(end) : QChar(QLatin1Char(' '));
            if (pathBoundary(before) && (pathBoundary(after) || after == QLatin1Char('/') || after == QLatin1Char('\\'))) {
                out.replace(i, f.size(), t);
                i += t.size();
            } else {
                i = end;
            }
        }
    }
    return out;
}

QVariant relocateValue(const QVariant &value, const QString &from, const QString &to, bool *changed)
{
    switch (value.typeId()) {
    case QMetaType::QString: {
        const QString s = value.toString();
        QString moved = relocatePath(s, from, to);
        if (moved == s)
            moved = relocateInText(s, from, to);
        if (moved == s)
            return value;
        *changed = true;
        return moved;
    }
    case QMetaType::QStringList: {
        QStringList list = value.toStringList();
        for (QString &s : list)
            s = relocateValue(s, from, to, changed).toString();
        return list;
    }
    case QMetaType::QVariantList: {
        QVariantList list = value.toList();
        for (QVariant &v : list)
            v = relocateValue(v, from, to, changed);
        return list;
    }
    case QMetaType::QVariantMap: {
        QVariantMap map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            it.value() = relocateValue(it.value(), from, to, changed);
        return map;
    }
    default:
        return value;
    }
}

// The move job: plan, stop servers, move on a worker, then profiles and library.
class FolderMover : public QObject
{
public:
    FolderMover(Job *job, JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, QString to)
        : QObject(job)
        , m_job(job)
        , m_jobs(jobs)
        , m_library(library)
        , m_servers(servers)
        , m_to(std::move(to))
    {
    }
    // A job removed while it runs must not leave the servers blocked.
    ~FolderMover() override { release(); }

    void run()
    {
        if (!m_library)
            return m_job->fail(Text::tr("Shutting down."));
        m_plan = planModelsFolderMove(m_library->defaultFolder(), m_to);
        if (!m_plan.ok())
            return m_job->fail(m_plan.error);
        for (Job *other : m_jobs->jobs()) {
            if (other->group() == kDownloadGroup && !other->isFinished())
                return m_job->fail(Text::tr("Models are being downloaded into the folder. Move it when the downloads are done."));
        }
        if (m_servers && !m_plan.files.isEmpty()) {
            if (!m_servers->gpuReservation().isEmpty())
                return m_job->fail(Text::tr("The models folder cannot be moved now: %1").arg(m_servers->gpuReservation()));
            // Running servers hold model files open; they also must not start meanwhile.
            m_restart = m_servers->reserveGpu(Text::tr("The models folder is being moved; servers start again when it is done."));
            m_reserved = true;
            if (!m_restart.isEmpty())
                m_job->setProgress(-1, Text::tr("Stopping servers…"));
        }
        m_job->setCancelHandler([token = m_token] { token.cancel(); });
        m_waiting.start();
        waitForServers();
    }

private:
    void waitForServers()
    {
        bool busy = false;
        if (m_servers) {
            for (const ServerProfile &p : m_servers->profiles()) {
                const ServerState s = m_servers->state(p.id);
                busy = busy || s == ServerState::Stopping || s == ServerState::Starting || s == ServerState::Ready;
            }
        }
        if (m_token.isCancelled()) {
            release();
            return m_job->reportCancelled();
        }
        // A process that does not go away in time leaves files in use; that move fails and undoes itself.
        if (busy && m_waiting.elapsed() < 20000) {
            QTimer::singleShot(100, this, [this] { waitForServers(); });
            return;
        }
        moveFiles();
    }

    void moveFiles()
    {
        m_job->setProgress(0, Text::tr("Moving models…"));
        auto done = std::make_shared<std::atomic<qint64>>(0);
        auto *poll = new QTimer(this);
        poll->setInterval(200);
        const qint64 total = m_plan.bytes;
        connect(poll, &QTimer::timeout, this, [this, done, total] {
            m_job->setProgress(total > 0 ? double(done->load()) / double(total) : -1.0,
                               Text::tr("Moving models: %1 of %2").arg(formatBytes(done->load()), formatBytes(total)));
        });
        poll->start();
        QPointer<FolderMover> self(this);
        async::run([plan = m_plan, token = m_token, done] {
            return moveModelFiles(plan, [&](qint64 n, qint64) {
                done->store(n);
                return !token.isCancelled();
            });
        }).then(this, [self, poll](const QString &error) {
            if (!self)
                return;
            poll->stop();
            self->finish(error);
        });
    }

    void finish(const QString &error)
    {
        if (!error.isEmpty()) {
            release();
            if (m_token.isCancelled())
                return m_job->reportCancelled();
            return m_job->fail(error);
        }
        const QString from = m_plan.from;
        const QString to = m_plan.to;
        if (m_servers) {
            for (ServerProfile p : m_servers->profiles()) {
                if (relocateOptions(p.options, from, to))
                    m_servers->updateProfile(p);
            }
        }
        if (m_library) {
            m_library->relocate(from, to);
            m_library->setFolder(to);
        }
        release();
        m_job->setProgress(1.0, Text::tr("%n model(s) moved to %1", nullptr, m_plan.models).arg(native(to)));
        m_job->succeed(to);
    }

    void release()
    {
        if (m_servers && std::exchange(m_reserved, false))
            m_servers->releaseGpu(m_restart);
    }

    Job *m_job;
    JobQueue *m_jobs;
    QPointer<ModelLibrary> m_library;
    QPointer<ServerSupervisor> m_servers;
    QString m_to;
    FolderMovePlan m_plan;
    QStringList m_restart;
    bool m_reserved = false;
    async::CancelToken m_token;
    QElapsedTimer m_waiting;
};

} // namespace

QString legacyModelsFolder()
{
    return QDir::cleanPath(QDir(paths::appData()).filePath(QStringLiteral("models")));
}

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), pathCase()) == 0;
}

bool isInsideFolder(const QString &path, const QString &folder)
{
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
    QString f = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (f.isEmpty())
        return false;
    if (!f.endsWith(QLatin1Char('/'))) // a drive root ("C:/") ends with one already
        f += QLatin1Char('/');
    return p.size() > f.size() && p.startsWith(f, pathCase());
}

FolderMovePlan planModelsFolderMove(const QString &from, const QString &to)
{
    FolderMovePlan plan;
    plan.from = cleanAbsolute(from);
    plan.to = cleanAbsolute(to);
    if (plan.to.isEmpty()) {
        plan.error = Text::tr("Choose a folder for the models.");
        return plan;
    }
    if (samePath(plan.from, plan.to)) {
        plan.error = Text::tr("The models are in %1 already.").arg(native(plan.to));
        return plan;
    }
    if (isInsideFolder(plan.to, plan.from)) {
        plan.error = Text::tr("%1 is inside the current models folder. Choose a folder outside it.").arg(native(plan.to));
        return plan;
    }
    if (QFileInfo::exists(plan.to) && !QFileInfo(plan.to).isDir()) {
        plan.error = Text::tr("%1 is a file, not a folder.").arg(native(plan.to));
        return plan;
    }
    const QStringList filters = modelFileFilters();
    const QDir source(plan.from);
    QDirIterator it(plan.from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    QStringList files;
    while (it.hasNext()) {
        const QString path = it.next();
        const QString name = QFileInfo(path).fileName();
        const bool model = QDir::match(filters, name);
        const bool partial = name.endsWith(kPartSuffix, Qt::CaseInsensitive) && QDir::match(filters, name.chopped(kPartSuffix.size()));
        if (!model && !partial)
            continue; // sidecars go with their model; anything else stays
        files << source.relativeFilePath(path);
        plan.bytes += QFileInfo(path).size();
        if (model) {
            ++plan.models;
            if (QFileInfo::exists(sidecarPath(path))) {
                files << source.relativeFilePath(sidecarPath(path));
                plan.bytes += QFileInfo(sidecarPath(path)).size();
            }
        }
    }
    plan.files = files;
    for (const QString &rel : std::as_const(plan.files)) {
        if (QFileInfo::exists(QDir(plan.to).filePath(rel)))
            plan.conflicts << rel;
    }
    if (!plan.conflicts.isEmpty()) {
        QStringList shown;
        for (const QString &c : plan.conflicts.mid(0, 3))
            shown << native(c);
        plan.error = Text::tr("%1 has files with the same names already: %2. Move or rename them first.")
                         .arg(native(plan.to), shown.join(QStringLiteral(", ")) + (plan.conflicts.size() > 3 ? QStringLiteral(", …") : QString()));
        return plan;
    }
    // The drive of the target: the deepest part of it that exists.
    QString existing = plan.to;
    while (!QFileInfo::exists(existing)) {
        const QString up = QFileInfo(existing).absolutePath();
        if (up == existing)
            break;
        existing = up;
    }
    const QStorageInfo target(existing);
    const QStorageInfo here(plan.from);
    plan.sameVolume = target.isValid() && here.isValid()
                      && (!target.device().isEmpty() && !here.device().isEmpty() ? target.device() == here.device() : target.rootPath() == here.rootPath());
    const qint64 needed = plan.bytes + 256LL * 1024 * 1024;
    if (!plan.sameVolume && plan.bytes > 0 && target.isValid() && target.bytesAvailable() < needed)
        plan.error = Text::tr("Not enough free space on %1: %2 needed, %3 free.")
                         .arg(native(target.rootPath()), formatBytes(needed), formatBytes(target.bytesAvailable()));
    return plan;
}

QString moveModelFiles(const FolderMovePlan &plan, const std::function<bool(qint64 done, qint64 total)> &progress)
{
    if (!plan.ok())
        return plan.error;
    if (!QDir().mkpath(plan.to))
        return Text::tr("Cannot create %1.").arg(native(plan.to));
    struct Moved
    {
        QString from;
        QString to;
        bool copied;
    };
    QList<Moved> moved;
    qint64 bytesDone = 0;
    bool stopped = false;
    QString error;
    auto report = [&](qint64 n) { return !progress || progress(n, plan.bytes); };
    for (const QString &rel : plan.files) {
        if (!report(bytesDone)) {
            stopped = true;
            error = Text::tr("The move was cancelled.");
            break;
        }
        const QString src = QDir(plan.from).filePath(rel);
        const QString dst = QDir(plan.to).filePath(rel);
        const qint64 size = QFileInfo(src).size();
        if (!QFileInfo::exists(src))
            continue; // gone since the plan (an unfinished download that was cleaned up)
        if (QFileInfo::exists(dst)) {
            error = Text::tr("%1 exists already.").arg(native(dst));
            break;
        }
        if (!QDir().mkpath(QFileInfo(dst).absolutePath())) {
            error = Text::tr("Cannot create %1.").arg(native(QFileInfo(dst).absolutePath()));
            break;
        }
        if (plan.sameVolume) {
            QString why;
            if (!renameFile(src, dst, &why)) {
                error = Text::tr("Cannot move %1: %2").arg(native(src), why);
                break;
            }
            moved.append({src, dst, false});
        } else {
            const QString temp = dst + kCopySuffix;
            error = copyFile(src, temp, [&](qint64 copied) { return report(bytesDone + copied); }, &stopped);
            QString why;
            if (error.isEmpty() && !renameFile(temp, dst, &why))
                error = Text::tr("Cannot move %1: %2").arg(native(temp), why);
            if (!error.isEmpty()) {
                QFile::remove(temp);
                break;
            }
            moved.append({src, dst, true});
        }
        bytesDone += size;
    }
    if (error.isEmpty()) {
        report(bytesDone);
        // Copies: the originals go once every file is complete in the new folder.
        for (const Moved &m : std::as_const(moved)) {
            if (m.copied && !QFile::remove(m.from))
                qCWarning(lcApp) << "models folder move: could not delete" << m.from;
        }
        removeEmptyFolders(plan.from);
        return {};
    }
    // Back to how it was: renamed files return, copies are deleted.
    for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
        QString why;
        if (it->copied)
            QFile::remove(it->to);
        else if (!renameFile(it->to, it->from, &why))
            qCWarning(lcApp) << "models folder move: could not move back" << it->to << why;
    }
    return error;
}

QString relocatePath(const QString &path, const QString &from, const QString &to)
{
    const QString typed = QDir::fromNativeSeparators(path.trimmed());
    if (typed.isEmpty() || from.isEmpty())
        return path;
    const QString p = QDir::cleanPath(typed);
    const QString f = QDir::cleanPath(QDir::fromNativeSeparators(from));
    QString rest;
    if (!samePath(p, f)) {
        if (!isInsideFolder(p, f))
            return path;
        rest = p.mid(f.size());
        while (rest.startsWith(QLatin1Char('/')))
            rest.remove(0, 1);
    }
    const QString base = QDir::cleanPath(QDir::fromNativeSeparators(to));
    const QString out = rest.isEmpty() ? base : QDir::cleanPath(base + QLatin1Char('/') + rest);
    return path.contains(QLatin1Char('\\')) ? native(out) : out;
}

bool relocateOptions(QVariantMap &options, const QString &from, const QString &to)
{
    bool changed = false;
    for (auto it = options.begin(); it != options.end(); ++it)
        it.value() = relocateValue(it.value(), from, to, &changed);
    return changed;
}

Job *moveModelsFolder(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &to)
{
    jobs->setConcurrency(kMoveGroup, 1);
    QPointer<ModelLibrary> lib(library);
    QPointer<ServerSupervisor> sup(servers);
    QPointer<JobQueue> queue(jobs);
    return jobs->submit(Text::tr("Move models folder"), kMoveGroup, [lib, sup, queue, to](Job *job) {
        if (!lib || !queue)
            return job->fail(Text::tr("Shutting down."));
        auto *mover = new FolderMover(job, queue, lib, sup, to);
        mover->run();
    }, QStringLiteral("models-folder"));
}

QString legacyModelsToAsk(const ModelLibrary *library, const QString &legacy)
{
    if (!library || legacy.isEmpty() || !library->chosenFolder().isEmpty() || samePath(legacy, library->standardFolder()))
        return {};
    return hasModelFiles(legacy) ? QDir::cleanPath(QDir::fromNativeSeparators(legacy)) : QString();
}

Job *settleLegacyModels(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &legacy,
                        const std::function<bool(const FolderMovePlan &plan)> &moveThem)
{
    if (!library || legacy.isEmpty())
        return nullptr;
    library->setFolder(legacy);
    const FolderMovePlan plan = planModelsFolderMove(legacy, library->standardFolder());
    const bool move = moveThem && moveThem(plan);
    if (!move || !plan.ok())
        return nullptr;
    return moveModelsFolder(jobs, library, servers, plan.to);
}

} // namespace lumen::runtime
