#pragma once

#include "runtime/ModelFormats.h"

#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QJsonObject>
#include <QObject>

#include <functional>
#include <optional>

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

class RuntimeStore;

// A folder the library scans (recursively). Lumen's own models folder is always one of them.
struct ModelRoot
{
    QString path;
    QString label;
    bool builtin = false;
};

struct ModelEntry
{
    QString path;
    qint64 size = 0;
    QDateTime modified;
    ModelInfo info;
    QString sha256;          // computed on request, kept while size and date do not change
    QJsonObject civitai;     // lookup result (see ModelLibrary::lookupCivitai); {"notFound": true} if unknown
    QJsonObject source;      // where Lumen downloaded it from (the "<file>.lumen.json" sidecar)

    QString name() const;    // file name without extension
};

QJsonObject modelInfoToJson(const ModelInfo &info);
// "<model file>.lumen.json": source, revision, licence, trigger words and checksum of a download.
QString sidecarPath(const QString &modelPath);
QJsonObject readSidecar(const QString &modelPath);
ModelInfo modelInfoFromJson(const QJsonObject &o);

// The model files on this computer: Lumen's models folder plus folders the user adds (ComfyUI,
// WebUI, LM Studio, Downloads…), scanned in place without copying. File headers are read once and
// kept in an index next to the settings; only new or changed files are read again.
class ModelLibrary : public QObject
{
    Q_OBJECT
public:
    explicit ModelLibrary(net::HttpClient *http, QObject *parent = nullptr);

    // <app data> by default (models.json); tests use a temporary folder, which then also holds the
    // standard models folder (<dir>/models).
    void setDataDir(const QString &dir);
    void load();
    void save() const;

    // Lumen's own folder with typed sub-folders (checkpoints, loras, llm, …), where downloads and
    // trained LoRAs go: the folder chosen in Settings → Models, else the standard one.
    QString defaultFolder() const;
    // The folder when none is chosen: paths::defaultModels(), or <data dir>/models after setDataDir.
    QString standardFolder() const;
    QString chosenFolder() const { return m_folder; }
    // Chooses the folder ("" or the standard folder = the standard one); the caller keeps the
    // choice (folderChanged). Files stay where they are (moveModelsFolder moves them). Once loaded,
    // the folder gets its sub-folders, becomes the built-in root and is scanned.
    void setFolder(const QString &path);
    // After the files under `from` moved to `to` (moveModelsFolder): their entries follow them with
    // checksums and lookups. Files that stayed (in added folders, which never move) keep theirs.
    void relocate(const QString &from, const QString &to);
    // While moveModelsFolder moves the files: scans wait (they would see half of them), and what
    // writes into the folder (downloads, trained LoRAs) refuses. endMove() also after a failed move.
    void beginMove();
    void endMove();
    bool isMoving() const { return m_moving; }
    // Sub-folders by kind. ComfyUI's names, so a ComfyUI install can use the same tree; language
    // models and their vision projectors share Lumen's own "llm" (findProjector looks next to the
    // model).
    static QString subfolderFor(ModelKind kind);
    // The sub-folders created in Lumen's folder.
    static QStringList subfolders();

    QList<ModelRoot> roots() const { return m_roots; }
    bool addRoot(const QString &path, const QString &label = QString());
    void removeRoot(const QString &path);
    // Folders with models that are not roots yet: ComfyUI / WebUI installs, LM Studio, Downloads,
    // and folders of models the servers use.
    QStringList suggestedRoots(const RuntimeStore *runtimes, const QStringList &usedFiles = {}) const;

    void rescan();
    bool isScanning() const { return m_scanning; }
    QList<ModelEntry> entries() const;
    QList<ModelEntry> entries(ModelKind kind) const;
    std::optional<ModelEntry> entry(const QString &path) const;
    // A file that is already on this computer: same SHA-256 when known, otherwise the same file
    // name and size.
    std::optional<ModelEntry> find(const QString &sha256, const QString &fileName = QString(), qint64 size = -1) const;
    // Adds (or refreshes) one file without a full rescan, e.g. after a download.
    QFuture<ModelEntry> addFile(const QString &path);

    QFuture<QString> computeHash(const QString &path);
    // Civitai's model page, base model, trigger words and preview for a file (looked up by its
    // SHA-256, which is sent to civitai.com). Resolves to the stored result.
    QFuture<QJsonObject> lookupCivitai(const QString &path);
    void setCivitaiBase(const QString &base) { m_civitaiBase = base; } // tests
    // How files are moved to the recycle bin (tests delete instead).
    std::function<bool(const QString &path, QString *error)> moveToTrash;

    // Moves the file to the recycle bin and forgets it.
    bool trash(const QString &path, QString *error = nullptr);

signals:
    void changed();
    void scanFinished();
    // chosenFolder() changed.
    void folderChanged();
    void relocated(const QString &from, const QString &to);

private:
    QString indexPath() const;
    // Makes defaultFolder() the built-in root, with its sub-folders.
    void useOwnFolder();

    net::HttpClient *m_http;
    QString m_dataDir;
    QString m_standardFolder;
    QString m_folder; // chosen; empty = the standard one
    bool m_loaded = false;
    bool m_moving = false;
    int m_generation = 0; // changes when files move; a scan that started before is done again
    QString m_civitaiBase{QStringLiteral("https://civitai.com")};
    QList<ModelRoot> m_roots;
    QHash<QString, ModelEntry> m_entries; // by path
    bool m_scanning = false;
    bool m_rescanPending = false;
};

} // namespace lumen::runtime
