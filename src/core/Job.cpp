#include "core/Job.h"

#include "core/Error.h"
#include "core/Log.h"

namespace lumen {

Job::Job(QString title, QString group, Starter starter, QObject *parent)
    : QObject(parent)
    , m_title(std::move(title))
    , m_group(std::move(group))
    , m_starter(std::move(starter))
{
}

Job::~Job()
{
    // Make sure no running request outlives the job silently.
    if (m_state == State::Running && m_cancelHandler)
        m_cancelHandler();
}

void Job::setTitle(const QString &t)
{
    if (t == m_title)
        return;
    m_title = t;
    emit titleChanged();
}

void Job::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    if (isFinished()) {
        m_finished = QDateTime::currentDateTime();
        m_cancelHandler = nullptr;
    }
    emit stateChanged(s);
    if (isFinished())
        emit finished(this);
}

void Job::start()
{
    if (m_state != State::Queued)
        return;
    ++m_attempt;
    m_started = QDateTime::currentDateTime();
    m_cancelRequested = false;
    setState(State::Running);
    try {
        m_starter(this);
    } catch (const std::exception &e) {
        fail(describeException(e));
    }
}

void Job::resetForRetry()
{
    m_progress = -1.0;
    m_status.clear();
    m_error.clear();
    m_result.clear();
    m_preview = QImage();
    m_cancelHandler = nullptr;
    m_finished = QDateTime();
    m_cancelRequested = false;
    m_state = State::Queued;
    emit stateChanged(m_state);
    emit progressChanged(m_progress, m_status);
    emit previewChanged();
}

void Job::setProgress(double progress, const QString &status)
{
    if (isFinished())
        return;
    m_progress = progress;
    if (!status.isNull())
        m_status = status;
    emit progressChanged(m_progress, m_status);
}

void Job::setStatus(const QString &status)
{
    if (isFinished())
        return;
    m_status = status;
    emit progressChanged(m_progress, m_status);
}

void Job::setPreview(const QImage &preview)
{
    if (isFinished())
        return;
    m_preview = preview;
    emit previewChanged();
}

void Job::succeed(const QVariant &result)
{
    if (isFinished())
        return;
    if (m_cancelRequested) {
        reportCancelled();
        return;
    }
    m_result = result;
    m_progress = 1.0;
    emit progressChanged(m_progress, m_status);
    setState(State::Succeeded);
}

void Job::fail(const QString &error)
{
    if (isFinished())
        return;
    if (m_cancelRequested) {
        reportCancelled();
        return;
    }
    m_error = error;
    qCWarning(lcApp).noquote() << "Job failed:" << m_title << "-" << error;
    setState(State::Failed);
}

void Job::reportCancelled()
{
    if (isFinished())
        return;
    m_status = tr("Cancelled");
    setState(State::Cancelled);
}

void Job::cancel()
{
    if (isFinished())
        return;
    if (m_state == State::Queued) {
        reportCancelled();
        return;
    }
    m_cancelRequested = true;
    setStatus(tr("Cancelling..."));
    if (m_cancelHandler) {
        auto handler = m_cancelHandler;
        handler();
    } else {
        reportCancelled();
    }
}

} // namespace lumen
