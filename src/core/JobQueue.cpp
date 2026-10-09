#include "core/JobQueue.h"

#include <QTimer>

namespace lumen {

JobQueue::JobQueue(QObject *parent)
    : QObject(parent)
{
}

Job *JobQueue::enqueue(Job *job)
{
    job->setParent(this);
    m_jobs.append(job);
    connect(job, &Job::stateChanged, this, [this, job] {
        emit jobChanged(job);
        emit countsChanged(activeCount(), pendingCount());
    });
    connect(job, &Job::progressChanged, this, [this, job] { emit jobChanged(job); });
    connect(job, &Job::previewChanged, this, [this, job] { emit jobChanged(job); });
    connect(job, &Job::titleChanged, this, [this, job] { emit jobChanged(job); });
    connect(job, &Job::finished, this, [this] { scheduleLater(); });
    emit jobAdded(job);
    emit countsChanged(activeCount(), pendingCount());
    // Deferred so callers can connect to the job before it starts.
    scheduleLater();
    return job;
}

Job *JobQueue::submit(const QString &title, const QString &group, Job::Starter starter, const QString &kind)
{
    auto *job = new Job(title, group, std::move(starter));
    job->setKind(kind);
    return enqueue(job);
}

void JobQueue::setConcurrency(const QString &group, int maxParallel)
{
    m_limits.insert(group, qMax(1, maxParallel));
    scheduleLater();
}

int JobQueue::concurrency(const QString &group) const
{
    return m_limits.value(group, m_defaultConcurrency);
}

void JobQueue::retry(Job *job)
{
    if (!job || !job->canRetry())
        return;
    job->resetForRetry();
    emit countsChanged(activeCount(), pendingCount());
    scheduleLater();
}

void JobQueue::cancelAll()
{
    const auto copy = m_jobs;
    for (Job *job : copy)
        job->cancel();
}

void JobQueue::clearFinished()
{
    const auto copy = m_jobs;
    for (Job *job : copy) {
        if (job->isFinished())
            remove(job);
    }
}

void JobQueue::remove(Job *job)
{
    if (!job || !m_jobs.contains(job))
        return;
    if (!job->isFinished())
        job->cancel();
    emit jobAboutToBeRemoved(job);
    m_jobs.removeOne(job);
    job->disconnect(this);
    job->deleteLater();
    emit countsChanged(activeCount(), pendingCount());
    scheduleLater();
}

Job *JobQueue::find(const QUuid &id) const
{
    for (Job *job : m_jobs) {
        if (job->id() == id)
            return job;
    }
    return nullptr;
}

int JobQueue::activeCount() const
{
    int n = 0;
    for (Job *job : m_jobs)
        n += job->state() == Job::State::Running ? 1 : 0;
    return n;
}

int JobQueue::pendingCount() const
{
    int n = 0;
    for (Job *job : m_jobs)
        n += job->state() == Job::State::Queued ? 1 : 0;
    return n;
}

void JobQueue::scheduleLater()
{
    if (m_schedulePending)
        return;
    m_schedulePending = true;
    QTimer::singleShot(0, this, [this] {
        m_schedulePending = false;
        schedule();
    });
}

void JobQueue::schedule()
{
    QHash<QString, int> running;
    for (Job *job : std::as_const(m_jobs)) {
        if (job->state() == Job::State::Running)
            running[job->group()]++;
    }
    // FIFO across all groups; each group is limited independently.
    const auto copy = m_jobs;
    for (Job *job : copy) {
        if (job->state() != Job::State::Queued)
            continue;
        int &count = running[job->group()];
        if (count >= concurrency(job->group()))
            continue;
        ++count;
        job->start();
    }
}

} // namespace lumen
