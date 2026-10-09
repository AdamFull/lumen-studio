#pragma once

#include "doc/Document.h"
#include "doc/ProjectStore.h"

#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <memory>
#include <optional>

namespace lumen::doc {

enum class AssetKind { Reference, Generation, Edit, Import, Pose, Mask, Export };

QString assetKindName(AssetKind kind);
QString assetKindLabel(AssetKind kind);
AssetKind assetKindFromName(const QString &name);

struct AssetInfo
{
    QString id;
    AssetKind kind = AssetKind::Import;
    QString name;
    QDateTime created;
    QSize size;
    QString mime;
    QJsonObject metadata; // generation parameters, prompt, provider, seed, source...
    bool favorite = false;
    QStringList tags;
    QString parentId;     // asset this one was derived from

    QString prompt() const { return metadata.value(QStringLiteral("prompt")).toString(); }
};

struct DocumentInfo
{
    QString id;
    QString name;
    QDateTime created;
    QDateTime modified;
    QSize size;
    QImage thumbnail;
};

// Generic JSON record stored in the project (poses, characters, chats, shot plans...).
struct Record
{
    QString id;
    QString type;
    QString name;
    QDateTime created;
    QDateTime modified;
    QJsonObject json;
    QImage thumbnail;
};

struct LoadedDocument
{
    QString id;
    QString name;
    QSize size;
    QList<LayerPtr> layers;
    QImage selection;
    QUuid active;
    QJsonObject settings;
    std::optional<pose::Pose> pose;
};

// An open project file (.lumen, SQLite). GUI-thread facade over ProjectStore; keeps lightweight
// metadata of assets/documents in memory and loads pixels on demand.
class Project : public QObject
{
    Q_OBJECT
public:
    static constexpr int kThumbnailSize = 320;
    static QString fileExtension() { return QStringLiteral("lumen"); }

    explicit Project(QObject *parent = nullptr);
    ~Project() override;

    QFuture<void> open(const QString &path);
    QFuture<void> close();
    bool isOpen() const { return m_open; }
    QString path() const { return m_store.path(); }
    QString displayName() const;

    // ---- assets -------------------------------------------------------------------------------
    const QList<AssetInfo> &assets() const { return m_assets; }
    std::optional<AssetInfo> asset(const QString &id) const;
    QFuture<AssetInfo> addAsset(const QImage &image, AssetKind kind, const QString &name,
                                const QJsonObject &metadata = {}, const QString &parentId = QString());
    QFuture<AssetInfo> importFile(const QString &filePath, AssetKind kind = AssetKind::Import);
    QFuture<QImage> loadImage(const QString &id);
    QFuture<QImage> loadThumbnail(const QString &id);
    QFuture<QByteArray> loadData(const QString &id);
    void updateAsset(const AssetInfo &info); // name, favorite, tags, metadata
    void removeAssets(const QStringList &ids);
    // Exports the original encoded bytes (keeps PNG metadata) or re-encodes to the requested format.
    QFuture<void> exportAsset(const QString &id, const QString &filePath);

    // ---- documents ----------------------------------------------------------------------------
    const QList<DocumentInfo> &documents() const { return m_documents; }
    QFuture<void> saveDocument(Document *doc);
    QFuture<std::shared_ptr<LoadedDocument>> loadDocument(const QString &id);
    void removeDocument(const QString &id);
    bool isSaving(const QString &documentId) const { return m_saving.contains(documentId); }

    // ---- records ------------------------------------------------------------------------------
    QFuture<QList<Record>> records(const QString &type);
    QFuture<Record> saveRecord(Record record);
    void removeRecord(const QString &id, const QString &type);

signals:
    void opened();
    void closed();
    void assetAdded(const lumen::doc::AssetInfo &info);
    void assetChanged(const lumen::doc::AssetInfo &info);
    void assetsRemoved(const QStringList &ids);
    void documentsChanged();
    void documentSaved(const QString &id);
    void recordsChanged(const QString &type);
    void errorOccurred(const QString &message);

private:
    void reportError(const QString &context, const QString &message);
    void finishSave(const QString &docId);

    ProjectStore m_store;
    bool m_open = false;
    QList<AssetInfo> m_assets;
    QList<DocumentInfo> m_documents;
    struct SaveState
    {
        bool pending = false;
        QPointer<Document> doc;
    };
    QHash<QString, SaveState> m_saving;
};

QString futureErrorMessage(const std::exception_ptr &e);

} // namespace lumen::doc
