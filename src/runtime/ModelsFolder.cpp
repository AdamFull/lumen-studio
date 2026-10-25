#include "runtime/ModelsFolder.h"

#include "core/Async.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "runtime/Download.h"
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

const QString kPartSuffix = FileDownload::partPath(QString()); // unfinished downloads
const QString kCopySuffix = QStringLiteral(".moving");
// Queue groups (the first also the move's kind): one move at a time; downloads write into the
// folder, and a training copies its LoRA into it when it ends (ModelHub, LoraTraining, CloudTraining).
const QString kMoveGroup = QStringLiteral("models-folder");
const QString kDownloadGroup = QStringLiteral("model-download");
const QStringList kTrainingGroups{QStringLiteral("lora-training"), QStringLiteral("cloud-training")};

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

// A symbolic link or a junction: moved as itself, never gone into (it may be ComfyUI's folder).
bool isLink(const QFileInfo &fi)
{
    return fi.isSymbolicLink() || fi.isJunction();
}

bool isModelName(const QString &name)
{
    return QDir::match(modelFileFilters(), name);
}

QFileInfoList entriesOf(const QString &dir)
{
    return QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

bool isKept(const QString &dir, const QStringList &keep)
{
    for (const QString &k : keep) {
        if (samePath(dir, k))
            return true;
    }
    return false;
}

// Removes the folders left empty under `root`, deepest first, and `root` itself when empty. Links
// and the folders in `keep` are left alone, and so is everything under them.
void removeEmptyFolders(const QString &root, const QStringList &keep, bool isRoot = true)
{
    for (const QFileInfo &fi : entriesOf(root)) {
        if (fi.isDir() && !isLink(fi) && !isKept(fi.absoluteFilePath(), keep))
            removeEmptyFolders(fi.absoluteFilePath(), keep, false);
    }
    // A linked root is someone else's folder; removing the link would only hide what is left.
    if (!isRoot || !isLink(QFileInfo(root)))
        QDir().rmdir(root); // only an empty one goes
}

// Model files under `dir`, also through links, as the library's scan finds them.
int countModels(const QString &dir)
{
    int n = 0;
    QDirIterator it(dir, modelFileFilters(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

// Model files of 1 MB or more under `dir` (as the library's scan: placeholders do not count).
bool holdsModels(const QString &dir)
{
    QDirIterator it(dir, modelFileFilters(), QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    while (it.hasNext()) {
        if (QFileInfo(it.next()).size() >= 1024 * 1024)
            return true;
    }
    return false;
}

// What the move takes from `dir` (`plan->from` or a folder in it) into plan->files and
// plan->links: model files with their sidecars, unfinished downloads, and links that lead to
// models. The folders in plan->keep stay, and nothing inside a link is taken on its own.
void collect(const QString &dir, FolderMovePlan *plan)
{
    const QDir source(plan->from);
    for (const QFileInfo &fi : entriesOf(dir)) {
        const QString path = QDir::cleanPath(fi.absoluteFilePath());
        if (isLink(fi)) {
            const int models = fi.isDir() ? countModels(path) : (isModelName(fi.fileName()) ? 1 : 0);
            if (models > 0) {
                plan->links << source.relativeFilePath(path);
                plan->models += models;
            }
            continue; // a link without models stays, as other files do
        }
        if (fi.isDir()) {
            if (!isKept(path, plan->keep))
                collect(path, plan);
            continue;
        }
        const QString name = fi.fileName();
        const bool model = isModelName(name);
        const bool partial = name.endsWith(kPartSuffix, Qt::CaseInsensitive) && isModelName(name.chopped(kPartSuffix.size()));
        if (!model && !partial)
            continue; // sidecars go with their model; anything else stays
        plan->files << source.relativeFilePath(path);
        plan->bytes += fi.size();
        if (model) {
            ++plan->models;
            const QFileInfo sidecar(sidecarPath(path));
            if (sidecar.exists()) {
                plan->files << source.relativeFilePath(sidecar.absoluteFilePath());
                plan->bytes += sidecar.size();
            }
        }
    }
}

QString linksAcrossDrives(const FolderMovePlan &plan)
{
    if (plan.links.isEmpty() || plan.sameVolume)
        return {};
    return Text::tr("%1 is a link to another place. Links can only move to a folder on the same drive: choose one there, or remove "
                    "the link first.")
        .arg(native(QDir(plan.from).filePath(plan.links.first())));
}

// mkpath that notes the folders it made, parents first, so an undo can remove them again.
bool makeFolder(const QString &dir, QStringList *created)
{
    QStringList missing;
    for (QString d = QDir::cleanPath(dir); !QFileInfo::exists(d);) {
        missing.prepend(d);
        const QString up = QFileInfo(d).absolutePath();
        if (up == d)
            break;
        d = up;
    }
    if (!QDir().mkpath(dir))
        return false;
    *created << missing;
    return true;
}

bool pathStart(QChar c)
{
    return c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('=') || c == QLatin1Char(',') || c == QLatin1Char(';');
}

// Paths into `from` inside free text (extra command-line arguments), in either separator style. A
// path starts at the start of the text or after a space, a quote, '=', ',' or ';'. A quoted path
// ends at its closing quote, any other at the next space, ',' or ';'. Only `from` itself or a path
// that goes on into it counts: "D:/Models 2/x" and "D:/Models backup" are not in D:/Models.
QString relocateInText(const QString &text, const QString &from, const QString &to)
{
    const QString cleanFrom = QDir::cleanPath(QDir::fromNativeSeparators(from));
    if (cleanFrom.isEmpty())
        return text;
    QStringList forms{cleanFrom};
    if (native(cleanFrom) != cleanFrom)
        forms << native(cleanFrom);
    QString out = text;
    qsizetype i = 0;
    for (;;) {
        qsizetype start = -1;
        qsizetype length = 0;
        for (const QString &f : std::as_const(forms)) {
            const qsizetype at = out.indexOf(f, i, pathCase());
            if (at >= 0 && (start < 0 || at < start)) {
                start = at;
                length = f.size();
            }
        }
        if (start < 0)
            return out;
        const qsizetype matched = start + length;
        const QChar before = start > 0 ? out.at(start - 1) : QChar(QLatin1Char(' '));
        if (!pathStart(before)) {
            i = matched;
            continue;
        }
        qsizetype end = matched;
        if (before == QLatin1Char('"') || before == QLatin1Char('\'')) {
            end = out.indexOf(before, matched);
            if (end < 0)
                end = out.size();
        } else {
            while (end < out.size() && !out.at(end).isSpace() && out.at(end) != QLatin1Char(',') && out.at(end) != QLatin1Char(';'))
                ++end;
        }
        const bool inside = end == matched || out.at(matched) == QLatin1Char('/') || out.at(matched) == QLatin1Char('\\');
        if (!inside) {
            i = matched;
            continue;
        }
        const QString path = out.mid(start, end - start);
        const QString moved = movedPath(path, from, to);
        out.replace(start, path.size(), moved);
        i = start + moved.size();
    }
}

// File and folder options, also inside lists and maps (a llama.cpp server's further models).
QVariant relocateValue(const QVariant &value, const QString &from, const QString &to, bool *changed)
{
    switch (value.typeId()) {
    case QMetaType::QString: {
        const QString s = value.toString();
        const QString moved = movedPath(s, from, to);
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
    // A job removed while it runs must not leave the servers blocked or the library moving.
    ~FolderMover() override { release(); }

    void run()
    {
        if (!m_library)
            return m_job->fail(Text::tr("Shutting down."));
        for (Job *other : m_jobs->jobs()) {
            if (other->isFinished())
                continue;
            if (other->group() == kDownloadGroup)
                return m_job->fail(Text::tr("Models are being downloaded into the folder. Move it when the downloads are done."));
            if (kTrainingGroups.contains(other->group()))
                return m_job->fail(Text::tr("A LoRA is being trained; it is saved into the folder when it is done. Move the folder after that."));
        }
        m_plan = planModelsFolderMove(m_library, m_to);
        if (!m_plan.ok())
            return m_job->fail(m_plan.error);
        if (m_servers && !m_plan.isEmpty()) {
            if (!m_servers->gpuReservation().isEmpty())
                return m_job->fail(Text::tr("The models folder cannot be moved now: %1").arg(m_servers->gpuReservation()));
            // Running servers hold model files open; they also must not start meanwhile.
            m_restart = m_servers->reserveGpu(Text::tr("The models folder is being moved; servers start again when it is done."));
            m_reserved = true;
            if (!m_restart.isEmpty())
                m_job->setProgress(-1, Text::tr("Stopping servers…"));
        }
        // From here on downloads and trainings refuse, and scans wait.
        m_library->beginMove();
        m_moving = true;
        m_job->setCancelHandler([token = m_token] { token.cancel(); });
        if (!m_reserved)
            return moveFiles(); // no server was stopped, so none to wait for
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
        // The files are in the new folder and the originals are gone: a cancel that came after the
        // last file cannot undo that any more.
        m_job->ignoreCancel();
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
        QString status = Text::tr("%n model(s) moved to %1", nullptr, m_plan.models).arg(native(to));
        // Files that appeared in the old folder while the others moved (nothing Lumen writes does
        // that while the folder moves); paths to them were left as they were.
        FolderMovePlan rest;
        rest.from = from;
        rest.keep = m_plan.keep;
        if (QFileInfo::exists(from))
            collect(from, &rest);
        if (!rest.isEmpty()) {
            qCWarning(lcApp) << "models folder move: files added meanwhile stayed in" << from << rest.files << rest.links;
            status += QLatin1Char(' ') + Text::tr("%n file(s) added meanwhile stayed in %1.", nullptr, int(rest.files.size() + rest.links.size())).arg(native(from));
        }
        m_job->setProgress(1.0, status);
        m_job->succeed(to);
    }

    void release()
    {
        if (m_servers && std::exchange(m_reserved, false))
            m_servers->releaseGpu(m_restart);
        if (m_library && std::exchange(m_moving, false))
            m_library->endMove();
    }

    Job *m_job;
    JobQueue *m_jobs;
    QPointer<ModelLibrary> m_library;
    QPointer<ServerSupervisor> m_servers;
    QString m_to;
    FolderMovePlan m_plan;
    QStringList m_restart;
    bool m_reserved = false;
    bool m_moving = false;
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

FolderMovePlan planModelsFolderMove(const QString &from, const QString &to, const QStringList &keep)
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
    for (const QString &k : keep) {
        if (isInsideFolder(k, plan.from))
            plan.keep << cleanAbsolute(k);
    }
    if (QFileInfo::exists(plan.from))
        collect(plan.from, &plan);
    for (const QString &rel : plan.files + plan.links) {
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
    plan.error = linksAcrossDrives(plan);
    if (!plan.ok())
        return plan;
    const qint64 needed = plan.bytes + 256LL * 1024 * 1024;
    if (!plan.sameVolume && plan.bytes > 0 && target.isValid() && target.bytesAvailable() < needed)
        plan.error = Text::tr("Not enough free space on %1: %2 needed, %3 free.")
                         .arg(native(target.rootPath()), formatBytes(needed), formatBytes(target.bytesAvailable()));
    return plan;
}

FolderMovePlan planModelsFolderMove(const ModelLibrary *library, const QString &to)
{
    QStringList added;
    for (const ModelRoot &r : library->roots()) {
        if (!r.builtin)
            added << r.path;
    }
    return planModelsFolderMove(library->defaultFolder(), to, added);
}

QString moveModelFiles(const FolderMovePlan &plan, const std::function<bool(qint64 done, qint64 total)> &progress)
{
    if (!plan.ok())
        return plan.error;
    if (const QString links = linksAcrossDrives(plan); !links.isEmpty())
        return links;
    QStringList created; // folders made in the target, removed again on an undo
    if (!makeFolder(plan.to, &created))
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
    const QStringList items = plan.files + plan.links;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const QString &rel = items.at(i);
        const bool link = i >= plan.files.size();
        if (!report(bytesDone)) {
            stopped = true;
            error = Text::tr("The move was cancelled.");
            break;
        }
        const QString src = QDir(plan.from).filePath(rel);
        const QString dst = QDir(plan.to).filePath(rel);
        const QFileInfo srcInfo(src);
        if (!srcInfo.exists() && !isLink(srcInfo))
            continue; // gone since the plan (an unfinished download that was cleaned up)
        const qint64 size = link ? 0 : srcInfo.size();
        if (QFileInfo::exists(dst)) {
            error = Text::tr("%1 exists already.").arg(native(dst));
            break;
        }
        if (!makeFolder(QFileInfo(dst).absolutePath(), &created)) {
            error = Text::tr("Cannot create %1.").arg(native(QFileInfo(dst).absolutePath()));
            break;
        }
        if (plan.sameVolume) {
            // A link is renamed as itself, whatever it leads to (links never move across drives).
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
    // The last chance to cancel: after this the originals go.
    if (error.isEmpty() && !report(bytesDone)) {
        stopped = true;
        error = Text::tr("The move was cancelled.");
    }
    if (error.isEmpty()) {
        // Copies: the originals go once every file is complete in the new folder.
        for (const Moved &m : std::as_const(moved)) {
            if (m.copied && !QFile::remove(m.from))
                qCWarning(lcApp) << "models folder move: could not delete" << m.from;
        }
        removeEmptyFolders(plan.from, plan.keep);
        return {};
    }
    // Back to how it was: renamed files return, copies are deleted, new folders removed.
    for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
        QString why;
        if (it->copied)
            QFile::remove(it->to);
        else if (!renameFile(it->to, it->from, &why))
            qCWarning(lcApp) << "models folder move: could not move back" << it->to << why;
    }
    for (auto it = created.crbegin(); it != created.crend(); ++it)
        QDir().rmdir(*it); // only empty ones: a file that could not move back stays visible
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

QString movedPath(const QString &path, const QString &from, const QString &to)
{
    const QString moved = relocatePath(path, from, to);
    if (moved == path)
        return path;
    const QString old = QDir::fromNativeSeparators(path.trimmed());
    if (QFileInfo::exists(old) && !QFileInfo::exists(moved))
        return path; // stayed behind: the old path still leads to it
    return moved;
}

bool relocateOptions(QVariantMap &options, const QString &from, const QString &to)
{
    bool changed = false;
    for (auto it = options.begin(); it != options.end(); ++it) {
        // The only free text: paths are found in it; every other option is a path or a list of them.
        if (it.key() == QLatin1String("extra_args") && it.value().typeId() == QMetaType::QString) {
            const QString text = it.value().toString();
            const QString moved = relocateInText(text, from, to);
            if (moved != text) {
                it.value() = moved;
                changed = true;
            }
        } else {
            it.value() = relocateValue(it.value(), from, to, &changed);
        }
    }
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
    }, kMoveGroup);
}

QString legacyModelsToAsk(const ModelLibrary *library, const QString &legacy)
{
    if (!library || legacy.isEmpty() || !library->chosenFolder().isEmpty() || samePath(legacy, library->standardFolder()))
        return {};
    return holdsModels(legacy) ? QDir::cleanPath(QDir::fromNativeSeparators(legacy)) : QString();
}

Job *settleLegacyModels(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &legacy,
                        const std::function<bool(const FolderMovePlan &plan)> &moveThem)
{
    if (!library || legacy.isEmpty())
        return nullptr;
    library->setFolder(legacy);
    const FolderMovePlan plan = planModelsFolderMove(library, library->standardFolder());
    const bool move = moveThem && moveThem(plan);
    if (!move || !plan.ok())
        return nullptr;
    return moveModelsFolder(jobs, library, servers, plan.to);
}

} // namespace lumen::runtime
