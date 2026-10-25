#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace lumen {
class Job;
class JobQueue;
} // namespace lumen

namespace lumen::runtime {

class ModelLibrary;
class ServerSupervisor;

// Lumen's own models folder (plan M17 §17.3): moving it, and the one-time move out of app data.
// Folders added to the library (ComfyUI, WebUI, LM Studio…) are scanned in place and never moved,
// also when they are inside Lumen's folder.

// The folder of earlier versions: <app data>/models.
QString legacyModelsFolder();

// The same file or folder (case-insensitive on Windows); a path inside a folder (not the folder).
bool samePath(const QString &a, const QString &b);
bool isInsideFolder(const QString &path, const QString &folder);

// What moving the models folder involves, or why it cannot be done.
struct FolderMovePlan
{
    QString from;
    QString to;
    // Relative to `from`: model files with their "<file>.lumen.json" sidecars, and unfinished
    // downloads ("<file>.part"). Other files stay, and so do the folders in `keep`.
    QStringList files;
    // Relative to `from`: links (symbolic links, junctions) that lead to models elsewhere, a folder
    // with model files or a model file. The link moves, not what it leads to (ComfyUI's folder
    // stays ComfyUI's), so this is only possible on the same drive.
    QStringList links;
    QStringList keep;        // absolute: folders inside `from` that stay (folders added to the library)
    int models = 0;          // model files among them, also those reached through links
    qint64 bytes = 0;
    bool sameVolume = false; // renamed in place; otherwise copied, then deleted
    QStringList conflicts;   // files that exist in `to` already
    QString error;           // why it cannot be done
    bool ok() const { return error.isEmpty(); }
    bool isEmpty() const { return files.isEmpty() && links.isEmpty(); }
};
// Refused: no target, the same folder, a target inside `from`, a file instead of a folder, files
// of the same names in the target, not enough free space there, links to move across drives.
// The folders in `keep` stay with everything in them, and what a link leads to moves only with it.
FolderMovePlan planModelsFolderMove(const QString &from, const QString &to, const QStringList &keep = {});
// The library's own folder to `to`; the folders added to the library stay.
FolderMovePlan planModelsFolderMove(const ModelLibrary *library, const QString &to);

// Moves the planned files; blocking, for a worker thread. progress(done, total) is called between
// files and copied chunks and returns false to cancel. A rename stays on the drive; across drives
// a file is copied under a temporary name (keeping its modification time, so the library's index
// still knows it) and the originals are deleted once all copies are complete. Links are renamed.
// On an error or a cancel everything moved goes back and the folders the move created are
// removed, so the models stay together in one place. Returns the error, "" on success.
QString moveModelFiles(const FolderMovePlan &plan, const std::function<bool(qint64 done, qint64 total)> &progress = {});

// `path` inside `from` (or `from` itself) at the same place in `to`, with the separators `path`
// used; any other path unchanged.
QString relocatePath(const QString &path, const QString &from, const QString &to);
// Where `path` is after moveModelFiles moved `from` to `to`: relocatePath, unless the file or
// folder stayed behind (it is still there and not in `to`): a file that is not a model, one in a
// folder added to the library, one written while the files moved.
QString movedPath(const QString &path, const QString &from, const QString &to);
// Every path into `from` in server options after the move (movedPath): files, folders, the further
// models of a llama.cpp server and paths typed into the extra arguments. Returns whether anything
// changed.
bool relocateOptions(QVariantMap &options, const QString &from, const QString &to);

// Moves the library's folder to `to` as a Queue job ("Move models folder", one at a time):
// * it fails without changing anything when the plan is refused, while models are downloaded
//   into the folder or a LoRA is trained (the result is copied into it), or, with files to move,
//   while the GPU is reserved;
// * while it runs the library is moving (ModelLibrary::isMoving): downloads, trainings and the
//   LoRA evaluation's copies refuse, and scans wait;
// * running servers stop while files move (they hold model files open) and start again after;
// * server profiles that point into the old folder are rewritten;
// * the library takes the new folder (folderChanged) and its entries keep their checksums and
//   lookups (relocate).
// Once the files are in place a cancel is ignored. The job's result is the new folder. `servers`
// may be null.
Job *moveModelsFolder(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &to);

// The first start after the standard folder left app data: `legacy` has model files and no folder
// is chosen. Returns `legacy` then, else "".
QString legacyModelsToAsk(const ModelLibrary *library, const QString &legacy = legacyModelsFolder());
// Asks once (the caller keeps that it asked). `legacy` becomes the chosen folder first, so its
// models stay usable; moveThem(plan) then decides whether they move to the standard folder (the
// returned job, after which the standard folder is in effect and nothing is chosen). A refused plan
// is passed too (to say why they stay); its answer is ignored.
Job *settleLegacyModels(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &legacy,
                        const std::function<bool(const FolderMovePlan &plan)> &moveThem);

} // namespace lumen::runtime
