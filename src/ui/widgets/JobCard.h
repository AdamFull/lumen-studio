#pragma once

#include "core/Job.h"

#include <QFrame>
#include <QPointer>

class QLabel;
class QProgressBar;
class QToolButton;

namespace lumen {
class JobQueue;
}

namespace lumen::ui {

// Compact live view of a job: preview thumbnail, title, status/progress, cancel/retry/dismiss.
class JobCard : public QFrame
{
    Q_OBJECT
public:
    JobCard(Job *job, JobQueue *queue, QWidget *parent = nullptr);
    Job *job() const { return m_job; }

signals:
    void dismissed(lumen::Job *job);

private:
    void refresh();

    QPointer<Job> m_job;
    JobQueue *m_queue;
    QLabel *m_preview;
    QLabel *m_title;
    QLabel *m_status;
    QProgressBar *m_progress;
    QToolButton *m_cancel;
    QToolButton *m_retry;
    QToolButton *m_dismiss;
};

} // namespace lumen::ui
