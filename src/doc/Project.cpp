#include "doc/Project.h"

#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/Log.h"
#include "core/PngText.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSqlQuery>
#include <QUuid>
#include <QPainter>
#include <QtConcurrent/QtConcurrentMap>

namespace lumen::doc {

namespace {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray toJsonBytes(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QJsonObject fromJsonBytes(const QVariant &v)
{
    return QJsonDocument::fromJson(v.toByteArray()).object();
}

// PNG with an A1111-compatible "parameters" text chunk when generation info is available.
QByteArray encodeAssetPng(const QImage &image, const QJsonObject &metadata)
{
    QByteArray bytes = img::encode(image, "PNG", 3);
    QList<QPair<QString, QString>> text{{QStringLiteral("Software"), QStringLiteral("Lumen Studio")}};
    const QString parameters = metadata.value(QStringLiteral("parameters")).toString();
    if (!parameters.isEmpty())
        text.append({QStringLiteral("parameters"), parameters});
    return png::insertText(bytes, text);
}

QByteArray encodeThumb(const QImage &image)
{
    return img::encode(img::thumbnail(image, Project::kThumbnailSize), "PNG", 1);
}

AssetInfo assetFromQuery(const QSqlQuery &q)
{
    AssetInfo a;
    a.id = q.value(0).toString();
    a.kind = assetKindFromName(q.value(1).toString());
    a.name = q.value(2).toString();
    a.created = QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong());
    a.size = QSize(q.value(4).toInt(), q.value(5).toInt());
    a.mime = q.value(6).toString();
    a.metadata = fromJsonBytes(q.value(7));
    a.favorite = q.value(8).toBool();
    a.tags = q.value(9).toString().split(u',', Qt::SkipEmptyParts);
    a.parentId = q.value(10).toString();
    return a;
}

} // namespace

QString assetKindName(AssetKind kind)
{
    switch (kind) {
    case AssetKind::Reference: return QStringLiteral("reference");
    case AssetKind::Generation: return QStringLiteral("generation");
    case AssetKind::Edit: return QStringLiteral("edit");
    case AssetKind::Import: return QStringLiteral("import");
    case AssetKind::Pose: return QStringLiteral("pose");
    case AssetKind::Mask: return QStringLiteral("mask");
    case AssetKind::Export: return QStringLiteral("export");
    }
    return QStringLiteral("import");
}

QString assetKindLabel(AssetKind kind)
{
    switch (kind) {
    case AssetKind::Reference: return QObject::tr("Reference");
    case AssetKind::Generation: return QObject::tr("Generated");
    case AssetKind::Edit: return QObject::tr("Edited");
    case AssetKind::Import: return QObject::tr("Imported");
    case AssetKind::Pose: return QObject::tr("Pose map");
    case AssetKind::Mask: return QObject::tr("Mask");
    case AssetKind::Export: return QObject::tr("Export");
    }
    return {};
}

AssetKind assetKindFromName(const QString &name)
{
    for (AssetKind k : {AssetKind::Reference, AssetKind::Generation, AssetKind::Edit, AssetKind::Import,
                        AssetKind::Pose, AssetKind::Mask, AssetKind::Export}) {
        if (assetKindName(k) == name)
            return k;
    }
    return AssetKind::Import;
}

QString futureErrorMessage(const std::exception_ptr &e)
{
    try {
        if (e)
            std::rethrow_exception(e);
    } catch (const std::exception &ex) {
        return describeException(ex);
    } catch (...) {
        return QObject::tr("Unknown error");
    }
    return {};
}

Project::Project(QObject *parent)
    : QObject(parent)
{
}

Project::~Project() = default;

QString Project::displayName() const
{
    return QFileInfo(path()).completeBaseName();
}

void Project::reportError(const QString &context, const QString &message)
{
    qCWarning(lcDoc).noquote() << context << ":" << message;
    emit errorOccurred(context + QStringLiteral(": ") + message);
}

QFuture<void> Project::open(const QString &path)
{
    m_open = false;
    m_assets.clear();
    m_documents.clear();
    struct Loaded
    {
        QList<AssetInfo> assets;
        QList<DocumentInfo> documents;
    };
    return m_store.open(path)
        .then(this, [this] {
            return m_store.run<std::shared_ptr<Loaded>>([](QSqlDatabase &db) {
                auto loaded = std::make_shared<Loaded>();
                QSqlQuery q(db);
                ProjectStore::check(q.exec(QStringLiteral(
                    "SELECT id, kind, name, created, width, height, mime, metadata, favorite, tags, parent "
                    "FROM assets ORDER BY created")), q);
                while (q.next())
                    loaded->assets.append(assetFromQuery(q));
                ProjectStore::check(q.exec(QStringLiteral(
                    "SELECT id, name, created, modified, width, height, thumb FROM documents ORDER BY modified DESC")), q);
                while (q.next()) {
                    DocumentInfo d;
                    d.id = q.value(0).toString();
                    d.name = q.value(1).toString();
                    d.created = QDateTime::fromMSecsSinceEpoch(q.value(2).toLongLong());
                    d.modified = QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong());
                    d.size = QSize(q.value(4).toInt(), q.value(5).toInt());
                    d.thumbnail = img::decode(q.value(6).toByteArray());
                    loaded->documents.append(d);
                }
                return loaded;
            });
        })
        .unwrap()
        .then(this, [this](std::shared_ptr<Loaded> loaded) {
            m_assets = loaded->assets;
            m_documents = loaded->documents;
            m_open = true;
            emit opened();
        });
}

QFuture<void> Project::close()
{
    if (!m_open)
        return QtFuture::makeReadyVoidFuture();
    m_open = false;
    return m_store.close().then(this, [this] {
        m_assets.clear();
        m_documents.clear();
        emit closed();
    });
}

std::optional<AssetInfo> Project::asset(const QString &id) const
{
    for (const AssetInfo &a : m_assets) {
        if (a.id == id)
            return a;
    }
    return std::nullopt;
}

QFuture<AssetInfo> Project::addAsset(const QImage &image, AssetKind kind, const QString &name, const QJsonObject &metadata,
                                     const QString &parentId)
{
    AssetInfo info;
    info.id = newId();
    info.kind = kind;
    info.name = name;
    info.created = QDateTime::currentDateTime();
    info.size = image.size();
    info.mime = QStringLiteral("image/png");
    info.metadata = metadata;
    info.parentId = parentId;

    struct Encoded
    {
        QByteArray data;
        QByteArray thumb;
    };
    return async::run([image, metadata] {
               return Encoded{encodeAssetPng(image, metadata), encodeThumb(image)};
           })
        .then(this, [this, info](Encoded enc) {
            return m_store.run<AssetInfo>([info, enc](QSqlDatabase &db) {
                QSqlQuery q(db);
                ProjectStore::check(q.prepare(QStringLiteral(
                    "INSERT INTO assets(id, kind, name, created, width, height, mime, data, thumb, metadata, favorite, tags, parent) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)")), q);
                q.addBindValue(info.id);
                q.addBindValue(assetKindName(info.kind));
                q.addBindValue(info.name);
                q.addBindValue(info.created.toMSecsSinceEpoch());
                q.addBindValue(info.size.width());
                q.addBindValue(info.size.height());
                q.addBindValue(info.mime);
                q.addBindValue(enc.data);
                q.addBindValue(enc.thumb);
                q.addBindValue(toJsonBytes(info.metadata));
                q.addBindValue(0);
                q.addBindValue(QString());
                q.addBindValue(info.parentId);
                ProjectStore::check(q.exec(), q);
                return info;
            });
        })
        .unwrap()
        .then(this, [this](AssetInfo stored) {
            m_assets.append(stored);
            emit assetAdded(stored);
            return stored;
        });
}

QFuture<AssetInfo> Project::importFile(const QString &filePath, AssetKind kind)
{
    const QString name = QFileInfo(filePath).completeBaseName();
    return async::run([filePath] {
               QFile file(filePath);
               if (!file.open(QIODevice::ReadOnly))
                   throw StoreError(QObject::tr("Cannot open %1: %2").arg(filePath, file.errorString()));
               const QByteArray bytes = file.readAll();
               QImage image = img::normalized(img::decode(bytes));
               if (image.isNull())
                   throw StoreError(QObject::tr("Unsupported or damaged image: %1").arg(filePath));
               QJsonObject meta;
               const QString params = png::readText(bytes).value(QStringLiteral("parameters"));
               if (!params.isEmpty())
                   meta.insert(QStringLiteral("parameters"), params);
               meta.insert(QStringLiteral("source"), filePath);
               return std::make_pair(image, meta);
           })
        .then(this, [this, kind, name](std::pair<QImage, QJsonObject> r) { return addAsset(r.first, kind, name, r.second); })
        .unwrap();
}

QFuture<QByteArray> Project::loadData(const QString &id)
{
    return m_store.run<QByteArray>([id](QSqlDatabase &db) {
        QSqlQuery q(db);
        ProjectStore::check(q.prepare(QStringLiteral("SELECT data FROM assets WHERE id=?")), q);
        q.addBindValue(id);
        ProjectStore::check(q.exec(), q);
        if (!q.next())
            throw StoreError("asset not found");
        return q.value(0).toByteArray();
    });
}

QFuture<QImage> Project::loadImage(const QString &id)
{
    return loadData(id).then(async::pool(), [](QByteArray data) {
        QImage image = img::normalized(img::decode(data));
        if (image.isNull())
            throw StoreError("asset image could not be decoded");
        return image;
    });
}

QFuture<QImage> Project::loadThumbnail(const QString &id)
{
    return m_store
        .run<QByteArray>([id](QSqlDatabase &db) {
            QSqlQuery q(db);
            ProjectStore::check(q.prepare(QStringLiteral("SELECT thumb FROM assets WHERE id=?")), q);
            q.addBindValue(id);
            ProjectStore::check(q.exec(), q);
            return q.next() ? q.value(0).toByteArray() : QByteArray();
        })
        .then(async::pool(), [](QByteArray data) { return img::decode(data); });
}

void Project::updateAsset(const AssetInfo &info)
{
    for (AssetInfo &a : m_assets) {
        if (a.id == info.id) {
            a.name = info.name;
            a.favorite = info.favorite;
            a.tags = info.tags;
            a.metadata = info.metadata;
            emit assetChanged(a);
            break;
        }
    }
    m_store
        .run<void>([info](QSqlDatabase &db) {
            QSqlQuery q(db);
            ProjectStore::check(q.prepare(QStringLiteral("UPDATE assets SET name=?, favorite=?, tags=?, metadata=? WHERE id=?")), q);
            q.addBindValue(info.name);
            q.addBindValue(info.favorite ? 1 : 0);
            q.addBindValue(info.tags.join(u','));
            q.addBindValue(toJsonBytes(info.metadata));
            q.addBindValue(info.id);
            ProjectStore::check(q.exec(), q);
        })
        .onFailed(this, [this](const std::exception &e) { reportError(tr("Updating asset failed"), describeException(e)); });
}

void Project::removeAssets(const QStringList &ids)
{
    if (ids.isEmpty())
        return;
    m_assets.removeIf([&ids](const AssetInfo &a) { return ids.contains(a.id); });
    emit assetsRemoved(ids);
    m_store
        .run<void>([ids](QSqlDatabase &db) {
            db.transaction();
            QSqlQuery q(db);
            for (const QString &id : ids) {
                ProjectStore::check(q.prepare(QStringLiteral("DELETE FROM assets WHERE id=?")), q);
                q.addBindValue(id);
                ProjectStore::check(q.exec(), q);
            }
            db.commit();
        })
        .onFailed(this, [this](const std::exception &e) { reportError(tr("Deleting assets failed"), describeException(e)); });
}

QFuture<void> Project::exportAsset(const QString &id, const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    return loadData(id).then(async::pool(), [filePath, suffix](QByteArray data) {
        QByteArray out = data;
        if (suffix != QLatin1String("png")) {
            const QImage image = img::decode(data);
            const QByteArray fmt = suffix == QLatin1String("jpg") ? QByteArray("JPEG") : suffix.toLatin1().toUpper();
            out = img::encode(image, fmt.constData(), 95);
            if (out.isEmpty())
                throw StoreError(QObject::tr("Unsupported export format: %1").arg(suffix));
        }
        QSaveFile f(filePath);
        if (!f.open(QIODevice::WriteOnly))
            throw StoreError(f.errorString());
        f.write(out);
        if (!f.commit())
            throw StoreError(f.errorString());
    });
}

// ---- documents --------------------------------------------------------------------------------

QFuture<void> Project::saveDocument(Document *doc)
{
    if (!doc)
        return QtFuture::makeReadyVoidFuture();
    if (doc->id().isEmpty())
        doc->setId(newId());
    const QString docId = doc->id();
    if (m_saving.contains(docId)) {
        m_saving[docId].pending = true;
        m_saving[docId].doc = doc;
        return QtFuture::makeReadyVoidFuture();
    }
    m_saving.insert(docId, SaveState{false, doc});

    struct LayerJob
    {
        QString id;
        int position;
        QJsonObject props;
        QJsonObject meta;
        QImage image;
        bool encode;
        quint64 revision;
        QByteArray data;
    };
    struct SaveJob
    {
        QString id, name;
        QSize size;
        QJsonObject state;
        QList<LayerJob> layers;
        DocumentSnapshot snapshot;
        QImage selection;
        QByteArray thumb;
        qint64 now;
    };
    auto job = std::make_shared<SaveJob>();
    job->id = docId;
    job->name = doc->name();
    job->size = doc->size();
    job->snapshot = doc->snapshot();
    job->selection = doc->selection();
    job->now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject state;
    state.insert(QStringLiteral("active"), doc->activeLayerId().toString(QUuid::WithoutBraces));
    state.insert(QStringLiteral("settings"), doc->settings());
    if (doc->controlPose())
        state.insert(QStringLiteral("pose"), doc->controlPose()->toOpenPoseJson());
    job->state = state;
    for (int i = 0; i < doc->layerCount(); ++i) {
        const LayerPtr l = doc->layerAt(i);
        job->layers.append(LayerJob{l->id.toString(QUuid::WithoutBraces), i, l->props.toJson(), l->meta, l->image,
                                    l->revision != l->savedRevision, l->revision, {}});
    }
    QPointer<Document> guard(doc);
    const quint64 revisionAtSave = doc->revision();

    return async::run([job] {
               QtConcurrent::blockingMap(async::pool(), job->layers, [](LayerJob &lj) {
                   if (lj.encode)
                       lj.data = img::encode(lj.image, "PNG", 1);
                   lj.image = QImage();
               });
               if (!job->selection.isNull())
                   job->state.insert(QStringLiteral("selection"), QString::fromLatin1(img::toBase64(job->selection, "PNG", 1)));
               job->thumb = encodeThumb(flatten(job->snapshot, job->size));
               job->snapshot.clear();
           })
        .then(this, [this, job] {
            return m_store.run<void>([job](QSqlDatabase &db) {
                db.transaction();
                QSqlQuery q(db);
                ProjectStore::check(q.prepare(QStringLiteral(
                    "INSERT INTO documents(id, name, created, modified, width, height, state, thumb) VALUES(?,?,?,?,?,?,?,?) "
                    "ON CONFLICT(id) DO UPDATE SET name=excluded.name, modified=excluded.modified, width=excluded.width, "
                    "height=excluded.height, state=excluded.state, thumb=excluded.thumb")), q);
                q.addBindValue(job->id);
                q.addBindValue(job->name);
                q.addBindValue(job->now);
                q.addBindValue(job->now);
                q.addBindValue(job->size.width());
                q.addBindValue(job->size.height());
                q.addBindValue(toJsonBytes(job->state));
                q.addBindValue(job->thumb);
                ProjectStore::check(q.exec(), q);
                QStringList ids;
                for (const LayerJob &lj : job->layers) {
                    ids << lj.id;
                    if (lj.encode) {
                        ProjectStore::check(q.prepare(QStringLiteral(
                            "INSERT OR REPLACE INTO layers(id, document_id, position, props, meta, data) VALUES(?,?,?,?,?,?)")), q);
                        q.addBindValue(lj.id);
                        q.addBindValue(job->id);
                        q.addBindValue(lj.position);
                        q.addBindValue(toJsonBytes(lj.props));
                        q.addBindValue(toJsonBytes(lj.meta));
                        q.addBindValue(lj.data);
                    } else {
                        ProjectStore::check(q.prepare(QStringLiteral("UPDATE layers SET position=?, props=?, meta=? WHERE id=?")), q);
                        q.addBindValue(lj.position);
                        q.addBindValue(toJsonBytes(lj.props));
                        q.addBindValue(toJsonBytes(lj.meta));
                        q.addBindValue(lj.id);
                    }
                    ProjectStore::check(q.exec(), q);
                }
                // Remove layers that no longer exist in the document.
                ProjectStore::check(q.prepare(QStringLiteral("SELECT id FROM layers WHERE document_id=?")), q);
                q.addBindValue(job->id);
                ProjectStore::check(q.exec(), q);
                QStringList stale;
                while (q.next()) {
                    if (!ids.contains(q.value(0).toString()))
                        stale << q.value(0).toString();
                }
                for (const QString &id : stale) {
                    ProjectStore::check(q.prepare(QStringLiteral("DELETE FROM layers WHERE id=?")), q);
                    q.addBindValue(id);
                    ProjectStore::check(q.exec(), q);
                }
                if (!db.commit())
                    throw StoreError("commit failed");
            });
        })
        .unwrap()
        .then(this, [this, job, guard, revisionAtSave] {
            if (guard) {
                for (const LayerJob &lj : job->layers) {
                    if (const LayerPtr l = guard->findLayer(QUuid(lj.id)); l && lj.encode)
                        l->savedRevision = lj.revision;
                }
                if (guard->revision() == revisionAtSave)
                    guard->setModified(false);
            }
            DocumentInfo info;
            info.id = job->id;
            info.name = job->name;
            info.modified = QDateTime::fromMSecsSinceEpoch(job->now);
            info.size = job->size;
            info.thumbnail = img::decode(job->thumb);
            bool found = false;
            for (DocumentInfo &d : m_documents) {
                if (d.id == info.id) {
                    info.created = d.created;
                    d = info;
                    found = true;
                }
            }
            if (!found) {
                info.created = info.modified;
                m_documents.prepend(info);
            }
            emit documentsChanged();
            emit documentSaved(job->id);
            finishSave(job->id);
        })
        .onFailed(this, [this, docId](const std::exception &e) {
            reportError(tr("Saving document failed"), describeException(e));
            finishSave(docId);
        });
}

void Project::finishSave(const QString &docId)
{
    const SaveState state = m_saving.take(docId);
    if (state.pending && state.doc)
        saveDocument(state.doc);
}

QFuture<std::shared_ptr<LoadedDocument>> Project::loadDocument(const QString &id)
{
    struct Raw
    {
        QString name;
        QSize size;
        QJsonObject state;
        struct L
        {
            QString id;
            QJsonObject props, meta;
            QByteArray data;
        };
        QList<L> layers;
    };
    return m_store
        .run<std::shared_ptr<Raw>>([id](QSqlDatabase &db) {
            auto raw = std::make_shared<Raw>();
            QSqlQuery q(db);
            ProjectStore::check(q.prepare(QStringLiteral("SELECT name, width, height, state FROM documents WHERE id=?")), q);
            q.addBindValue(id);
            ProjectStore::check(q.exec(), q);
            if (!q.next())
                throw StoreError("document not found");
            raw->name = q.value(0).toString();
            raw->size = QSize(q.value(1).toInt(), q.value(2).toInt());
            raw->state = fromJsonBytes(q.value(3));
            ProjectStore::check(q.prepare(QStringLiteral("SELECT id, props, meta, data FROM layers WHERE document_id=? ORDER BY position")), q);
            q.addBindValue(id);
            ProjectStore::check(q.exec(), q);
            while (q.next())
                raw->layers.append({q.value(0).toString(), fromJsonBytes(q.value(1)), fromJsonBytes(q.value(2)), q.value(3).toByteArray()});
            return raw;
        })
        .then(async::pool(), [id](std::shared_ptr<Raw> raw) {
            auto doc = std::make_shared<LoadedDocument>();
            doc->id = id;
            doc->name = raw->name;
            doc->size = raw->size;
            doc->layers.resize(raw->layers.size());
            QList<int> indices;
            for (int i = 0; i < raw->layers.size(); ++i)
                indices << i;
            QtConcurrent::blockingMap(async::pool(), indices, [&](int i) {
                const auto &l = raw->layers[i];
                auto layer = std::make_shared<Layer>();
                layer->id = QUuid(l.id);
                layer->props = LayerProps::fromJson(l.props);
                layer->meta = l.meta;
                QImage image = img::normalized(img::decode(l.data));
                if (image.size() != raw->size) {
                    QImage fixed(raw->size, QImage::Format_ARGB32_Premultiplied);
                    fixed.fill(Qt::transparent);
                    QPainter p(&fixed);
                    p.drawImage(0, 0, image);
                    image = fixed;
                }
                layer->image = image;
                layer->revision = 1;
                layer->savedRevision = 1;
                doc->layers[i] = layer;
            });
            doc->active = QUuid(raw->state.value(QStringLiteral("active")).toString());
            doc->settings = raw->state.value(QStringLiteral("settings")).toObject();
            const QString sel = raw->state.value(QStringLiteral("selection")).toString();
            if (!sel.isEmpty())
                doc->selection = img::fromBase64(sel.toLatin1()).convertToFormat(QImage::Format_Grayscale8);
            if (raw->state.contains(QStringLiteral("pose")))
                doc->pose = pose::Pose::fromOpenPoseJson(raw->state.value(QStringLiteral("pose")).toObject());
            return doc;
        });
}

void Project::removeDocument(const QString &id)
{
    m_documents.removeIf([&id](const DocumentInfo &d) { return d.id == id; });
    emit documentsChanged();
    m_store
        .run<void>([id](QSqlDatabase &db) {
            db.transaction();
            QSqlQuery q(db);
            for (const char *sql : {"DELETE FROM layers WHERE document_id=?", "DELETE FROM documents WHERE id=?"}) {
                ProjectStore::check(q.prepare(QString::fromLatin1(sql)), q);
                q.addBindValue(id);
                ProjectStore::check(q.exec(), q);
            }
            db.commit();
        })
        .onFailed(this, [this](const std::exception &e) { reportError(tr("Deleting document failed"), describeException(e)); });
}

// ---- records ----------------------------------------------------------------------------------

QFuture<QList<Record>> Project::records(const QString &type)
{
    return m_store.run<QList<Record>>([type](QSqlDatabase &db) {
        QList<Record> list;
        QSqlQuery q(db);
        ProjectStore::check(q.prepare(QStringLiteral(
            "SELECT id, name, created, modified, json, thumb FROM records WHERE type=? ORDER BY modified DESC")), q);
        q.addBindValue(type);
        ProjectStore::check(q.exec(), q);
        while (q.next()) {
            Record r;
            r.id = q.value(0).toString();
            r.type = type;
            r.name = q.value(1).toString();
            r.created = QDateTime::fromMSecsSinceEpoch(q.value(2).toLongLong());
            r.modified = QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong());
            r.json = fromJsonBytes(q.value(4));
            r.thumbnail = img::decode(q.value(5).toByteArray());
            list.append(r);
        }
        return list;
    });
}

QFuture<Record> Project::saveRecord(Record record)
{
    if (record.id.isEmpty())
        record.id = newId();
    const QDateTime now = QDateTime::currentDateTime();
    if (!record.created.isValid())
        record.created = now;
    record.modified = now;
    return async::run([record]() mutable {
               const QByteArray thumb = record.thumbnail.isNull() ? QByteArray() : encodeThumb(record.thumbnail);
               return std::make_pair(record, thumb);
           })
        .then(this, [this](std::pair<Record, QByteArray> r) {
            return m_store.run<Record>([r](QSqlDatabase &db) {
                QSqlQuery q(db);
                ProjectStore::check(q.prepare(QStringLiteral(
                    "INSERT INTO records(id, type, name, created, modified, json, thumb) VALUES(?,?,?,?,?,?,?) "
                    "ON CONFLICT(id) DO UPDATE SET name=excluded.name, modified=excluded.modified, json=excluded.json, "
                    "thumb=COALESCE(excluded.thumb, records.thumb)")), q);
                q.addBindValue(r.first.id);
                q.addBindValue(r.first.type);
                q.addBindValue(r.first.name);
                q.addBindValue(r.first.created.toMSecsSinceEpoch());
                q.addBindValue(r.first.modified.toMSecsSinceEpoch());
                q.addBindValue(toJsonBytes(r.first.json));
                q.addBindValue(r.second.isEmpty() ? QVariant(QMetaType(QMetaType::QByteArray)) : QVariant(r.second));
                ProjectStore::check(q.exec(), q);
                return r.first;
            });
        })
        .unwrap()
        .then(this, [this](Record saved) {
            emit recordsChanged(saved.type);
            return saved;
        });
}

void Project::removeRecord(const QString &id, const QString &type)
{
    m_store
        .run<void>([id](QSqlDatabase &db) {
            QSqlQuery q(db);
            ProjectStore::check(q.prepare(QStringLiteral("DELETE FROM records WHERE id=?")), q);
            q.addBindValue(id);
            ProjectStore::check(q.exec(), q);
        })
        .then(this, [this, type] { emit recordsChanged(type); })
        .onFailed(this, [this](const std::exception &e) { reportError(tr("Deleting record failed"), describeException(e)); });
}

} // namespace lumen::doc
