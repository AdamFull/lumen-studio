#pragma once

#include "core/Job.h"

#include <QHash>
#include <QList>
#include <QObject>

namespace lumen {

// Schedules jobs with a per-group concurrency limit (group = provider instance).
// Owns its jobs; finished jobs are kept for history until cleared.
class JobQueue : public QObject
{
    Q_OBJECT
public:
    explicit JobQueue(QObject *parent = nullptr);

    Job *enqueue(Job *job);
    Job *submit(const QString &title, const QString &group, Job::Starter starter, const QString &kind = QString());

    void setConcurrency(const QString &group, int maxParallel);
    int concurrency(const QString &group) const;
    void setDefaultConcurrency(int n) { m_defaultConcurrency = qMax(1, n); }

    void retry(Job *job);
    void cancelAll();
    void clearFinished();
    void remove(Job *job);

    const QList<Job *> &jobs() const { return m_jobs; }
    Job *find(const QUuid &id) const;
    int activeCount() const;
    int pendingCount() const;

signals:
    void jobAdded(lumen::Job *job);
    void jobAboutToBeRemoved(lumen::Job *job);
    void jobChanged(lumen::Job *job);
    void countsChanged(int active, int pending);

private:
    void schedule();
    void scheduleLater();

    QList<Job *> m_jobs;
    QHash<QString, int> m_limits;
    int m_defaultConcurrency = 2;
    bool m_schedulePending = false;
};

} // namespace lumen
