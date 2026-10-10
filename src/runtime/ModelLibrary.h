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

    // <app data> by default (models.json, models/); tests use a temporary folder.
    void setDataDir(const QString &dir);
    void load();
    void save() const;

    // Lumen's own folder with typed sub-folders (checkpoints, loras, llm, …).
    QString defaultFolder() const;
    static QString subfolderFor(ModelKind kind);

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

private:
    QString indexPath() const;

    net::HttpClient *m_http;
    QString m_dataDir;
    QString m_civitaiBase{QStringLiteral("https://civitai.com")};
    QList<ModelRoot> m_roots;
    QHash<QString, ModelEntry> m_entries; // by path
    bool m_scanning = false;
    bool m_rescanPending = false;
};

} // namespace lumen::runtime
