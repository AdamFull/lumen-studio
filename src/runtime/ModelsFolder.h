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
// Folders added to the library (ComfyUI, WebUI, LM Studio…) are scanned in place and never moved.

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
    // downloads ("<file>.part"). Other files stay.
    QStringList files;
    int models = 0;          // model files among them
    qint64 bytes = 0;
    bool sameVolume = false; // renamed in place; otherwise copied, then deleted
    QStringList conflicts;   // files that exist in `to` already
    QString error;           // why it cannot be done
    bool ok() const { return error.isEmpty(); }
};
// Refused: no target, the same folder, a target inside `from`, a file instead of a folder, files
// of the same names in the target, not enough free space there.
FolderMovePlan planModelsFolderMove(const QString &from, const QString &to);

// Moves the planned files; blocking, for a worker thread. progress(done, total) is called between
// files and copied chunks and returns false to cancel. A rename stays on the drive; across drives
// a file is copied under a temporary name (keeping its modification time, so the library's index
// still knows it) and the originals are deleted once all copies are complete. On an error or a
// cancel everything moved goes back, so the models stay together in one place. Returns the error,
// "" on success.
QString moveModelFiles(const FolderMovePlan &plan, const std::function<bool(qint64 done, qint64 total)> &progress = {});

// `path` inside `from` (or `from` itself) at the same place in `to`, with the separators `path`
// used; any other path unchanged.
QString relocatePath(const QString &path, const QString &from, const QString &to);
// Every path into `from` in server options: files, folders, the further models of a llama.cpp
// server and paths typed into the extra arguments. Returns whether anything changed.
bool relocateOptions(QVariantMap &options, const QString &from, const QString &to);

// Moves the library's folder to `to` as a Queue job ("Move models folder", one at a time):
// * it fails without changing anything when the plan is refused, while models are downloaded
//   into the folder, or, with files to move, while the GPU is reserved (a LoRA training writes
//   into the folder);
// * running servers stop while files move (they hold model files open) and start again after;
// * server profiles that point into the old folder are rewritten;
// * the library takes the new folder (folderChanged) and its entries keep their checksums and
//   lookups (relocate).
// The job's result is the new folder. `servers` may be null.
Job *moveModelsFolder(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &to);

// The first start after the standard folder left app data: `legacy` has model files and no folder
// is chosen. Returns `legacy` then, else "".
QString legacyModelsToAsk(const ModelLibrary *library, const QString &legacy = legacyModelsFolder());
// Asks once. `legacy` becomes the chosen folder first, so its models stay usable and the question
// does not come again; moveThem(plan) then decides whether they move to the standard folder (the
// returned job, which makes that the chosen folder once the files are there). A refused plan is
// passed too (to say why they stay); its answer is ignored.
Job *settleLegacyModels(JobQueue *jobs, ModelLibrary *library, ServerSupervisor *servers, const QString &legacy,
                        const std::function<bool(const FolderMovePlan &plan)> &moveThem);

} // namespace lumen::runtime
