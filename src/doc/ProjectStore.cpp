#include "doc/ProjectStore.h"

#include "core/Log.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace lumen::doc {

namespace {

constexpr int kSchemaVersion = 1;

const char *kSchema[] = {
    "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)",
    "CREATE TABLE IF NOT EXISTS assets ("
    " id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT, created INTEGER NOT NULL,"
    " width INTEGER, height INTEGER, mime TEXT, data BLOB, thumb BLOB, metadata TEXT,"
    " favorite INTEGER DEFAULT 0, tags TEXT, parent TEXT)",
    "CREATE INDEX IF NOT EXISTS assets_kind ON assets(kind, created)",
    "CREATE TABLE IF NOT EXISTS documents ("
    " id TEXT PRIMARY KEY, name TEXT, created INTEGER, modified INTEGER, width INTEGER, height INTEGER,"
    " state TEXT, thumb BLOB)",
    "CREATE TABLE IF NOT EXISTS layers ("
    " id TEXT PRIMARY KEY, document_id TEXT NOT NULL, position INTEGER, props TEXT, meta TEXT, data BLOB)",
    "CREATE INDEX IF NOT EXISTS layers_doc ON layers(document_id, position)",
    "CREATE TABLE IF NOT EXISTS records ("
    " id TEXT PRIMARY KEY, type TEXT NOT NULL, name TEXT, created INTEGER, modified INTEGER, json TEXT, thumb BLOB)",
    "CREATE INDEX IF NOT EXISTS records_type ON records(type, modified)",
};

} // namespace

void ProjectStore::check(bool ok, const QSqlQuery &query)
{
    if (!ok)
        throw StoreError(query.lastError().text().toStdString());
}

ProjectStore::ProjectStore(QObject *parent)
    : QObject(parent)
    , m_connection(QStringLiteral("lumen-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    m_thread.setObjectName(QStringLiteral("LumenProjectStore"));
    m_worker = new QObject;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

ProjectStore::~ProjectStore()
{
    const QString conn = m_connection;
    QMetaObject::invokeMethod(m_worker, [conn] {
        {
            QSqlDatabase db = QSqlDatabase::database(conn, false);
            if (db.isOpen())
                db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

QFuture<void> ProjectStore::open(const QString &path)
{
    m_path = path;
    auto promise = std::make_shared<QPromise<void>>();
    QFuture<void> future = promise->future();
    promise->start();
    const QString conn = m_connection;
    QMetaObject::invokeMethod(m_worker, [promise, conn, path] {
        try {
            QSqlDatabase db = QSqlDatabase::contains(conn) ? QSqlDatabase::database(conn, false)
                                                           : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            if (db.isOpen())
                db.close();
            db.setDatabaseName(path);
            if (!db.open())
                throw StoreError(db.lastError().text().toStdString());
            QSqlQuery q(db);
            // WAL keeps the GUI responsive (readers never block writers) and is crash safe.
            for (const char *pragma : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA foreign_keys=ON",
                                       "PRAGMA busy_timeout=5000"}) {
                q.exec(QString::fromLatin1(pragma));
            }
            for (const char *stmt : kSchema)
                check(q.exec(QString::fromLatin1(stmt)), q);
            check(q.exec(QStringLiteral("SELECT value FROM meta WHERE key='schema'")), q);
            const int version = q.next() ? q.value(0).toInt() : 0;
            if (version > kSchemaVersion)
                throw StoreError("project was created by a newer version of Lumen Studio");
            check(q.prepare(QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES('schema', ?)")), q);
            q.addBindValue(kSchemaVersion);
            check(q.exec(), q);
            qCInfo(lcDoc) << "Opened project" << path;
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    }, Qt::QueuedConnection);
    return future;
}

QFuture<void> ProjectStore::close()
{
    return run<void>([](QSqlDatabase &db) {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
        q.finish();
        db.close();
    });
}

} // namespace lumen::doc
