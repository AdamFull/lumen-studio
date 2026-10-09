#include "ui/widgets/JobCard.h"

#include "core/JobQueue.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

JobCard::JobCard(Job *job, JobQueue *queue, QWidget *parent)
    : QFrame(parent)
    , m_job(job)
    , m_queue(queue)
{
    setProperty("card", QStringLiteral("true"));
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 8, 8, 8);
    h->setSpacing(10);
    m_preview = new QLabel(this);
    m_preview->setFixedSize(52, 52);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg(Theme::palette().raised.name()));
    h->addWidget(m_preview);
    auto *v = new QVBoxLayout;
    v->setSpacing(3);
    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    m_status = new QLabel(this);
    m_status->setProperty("role", QStringLiteral("muted"));
    m_status->setWordWrap(true);
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false);
    v->addWidget(m_title);
    v->addWidget(m_status);
    v->addWidget(m_progress);
    h->addLayout(v, 1);
    m_cancel = iconButton(QStringLiteral("stop"), tr("Cancel"), this);
    m_retry = iconButton(QStringLiteral("refresh"), tr("Retry"), this);
    m_dismiss = iconButton(QStringLiteral("close"), tr("Dismiss"), this);
    h->addWidget(m_cancel, 0, Qt::AlignTop);
    h->addWidget(m_retry, 0, Qt::AlignTop);
    h->addWidget(m_dismiss, 0, Qt::AlignTop);
    connect(m_cancel, &QToolButton::clicked, this, [this] {
        if (m_job)
            m_job->cancel();
    });
    connect(m_retry, &QToolButton::clicked, this, [this] {
        if (m_job)
            m_queue->retry(m_job);
    });
    connect(m_dismiss, &QToolButton::clicked, this, [this] { emit dismissed(m_job); });
    if (job) {
        connect(job, &Job::stateChanged, this, &JobCard::refresh);
        connect(job, &Job::progressChanged, this, &JobCard::refresh);
        connect(job, &Job::previewChanged, this, &JobCard::refresh);
        connect(job, &Job::titleChanged, this, &JobCard::refresh);
    }
    refresh();
}

void JobCard::refresh()
{
    if (!m_job) {
        m_status->setText(tr("Removed"));
        return;
    }
    const QString title = m_job->title();
    m_title->setText(title.size() > 70 ? title.left(68) + QStringLiteral("…") : title);
    m_title->setToolTip(title);
    const Job::State s = m_job->state();
    QString status = m_job->statusText();
    switch (s) {
    case Job::State::Queued:
        status = tr("Waiting in queue");
        break;
    case Job::State::Running:
        if (status.isEmpty())
            status = tr("Working...");
        break;
    case Job::State::Succeeded:
        status = tr("Done in %1 s").arg(m_job->startedAt().msecsTo(m_job->finishedAt()) / 1000.0, 0, 'f', 1);
        break;
    case Job::State::Failed:
        status = m_job->errorString();
        break;
    case Job::State::Cancelled:
        status = tr("Cancelled");
        break;
    }
    m_status->setText(status);
    m_status->setProperty("role", s == Job::State::Failed ? QStringLiteral("error") : QStringLiteral("muted"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_progress->setVisible(!m_job->isFinished());
    if (m_job->progress() < 0) {
        m_progress->setRange(0, 0);
    } else {
        m_progress->setRange(0, 1000);
        m_progress->setValue(int(m_job->progress() * 1000));
    }
    QImage preview = m_job->preview();
    if (!preview.isNull()) {
        m_preview->setPixmap(QPixmap::fromImage(preview.scaled(52, 52, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    } else {
        const QString icon = s == Job::State::Failed ? QStringLiteral("warning")
                           : s == Job::State::Succeeded ? QStringLiteral("check")
                           : s == Job::State::Cancelled ? QStringLiteral("close")
                                                        : QStringLiteral("sparkles");
        m_preview->setPixmap(Icons::pixmap(icon, 22, s == Job::State::Failed ? Theme::palette().danger : Theme::palette().icon));
    }
    m_cancel->setVisible(!m_job->isFinished());
    m_retry->setVisible(m_job->canRetry());
    m_dismiss->setVisible(m_job->isFinished());
}

} // namespace lumen::ui
