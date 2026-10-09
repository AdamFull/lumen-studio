#pragma once

#include <QFuture>
#include <QObject>
#include <QPromise>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>

#include <functional>
#include <memory>
#include <stdexcept>

namespace lumen::doc {

class StoreError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Owns a dedicated thread with a single SQLite connection. All database work is serialised on that
// thread and results are delivered as QFutures, so the GUI never waits on disk I/O.
class ProjectStore : public QObject
{
    Q_OBJECT
public:
    explicit ProjectStore(QObject *parent = nullptr);
    ~ProjectStore() override;

    // Opens (or creates) the database file and migrates the schema.
    QFuture<void> open(const QString &path);
    QFuture<void> close();
    QString path() const { return m_path; }

    // Runs fn on the database thread. Exceptions (StoreError) are forwarded to the future.
    template<typename T>
    QFuture<T> run(std::function<T(QSqlDatabase &)> fn)
    {
        auto promise = std::make_shared<QPromise<T>>();
        QFuture<T> future = promise->future();
        promise->start();
        const QString conn = m_connection;
        QMetaObject::invokeMethod(m_worker, [promise, fn = std::move(fn), conn] {
            QSqlDatabase db = QSqlDatabase::database(conn, false);
            try {
                if (!db.isOpen())
                    throw StoreError("project database is not open");
                if constexpr (std::is_void_v<T>) {
                    fn(db);
                } else {
                    promise->addResult(fn(db));
                }
            } catch (...) {
                promise->setException(std::current_exception());
            }
            promise->finish();
        }, Qt::QueuedConnection);
        return future;
    }

    // Throws StoreError with the query's error text when the query fails.
    static void check(bool ok, const QSqlQuery &query);

private:
    QThread m_thread;
    QObject *m_worker = nullptr;
    QString m_connection;
    QString m_path;
};

} // namespace lumen::doc
