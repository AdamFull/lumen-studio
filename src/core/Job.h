#pragma once

#include <QDateTime>
#include <QImage>
#include <QObject>
#include <QUuid>
#include <QVariant>

#include <functional>

namespace lumen {

// A unit of asynchronous work tracked by the JobQueue (typically one AI request).
// All members must be used from the GUI thread; workers report back via queued calls.
class Job : public QObject
{
    Q_OBJECT
public:
    enum class State { Queued, Running, Succeeded, Failed, Cancelled };
    Q_ENUM(State)

    // The starter launches the work. It must not block; it reports through the job's
    // setProgress / setPreview / succeed / fail methods (possibly much later).
    using Starter = std::function<void(Job *)>;

    Job(QString title, QString group, Starter starter, QObject *parent = nullptr);
    ~Job() override;

    QUuid id() const { return m_id; }
    QString title() const { return m_title; }
    void setTitle(const QString &t);
    QString group() const { return m_group; }
    QString kind() const { return m_kind; }
    void setKind(const QString &k) { m_kind = k; }
    State state() const { return m_state; }
    bool isFinished() const
    {
        return m_state == State::Succeeded || m_state == State::Failed || m_state == State::Cancelled;
    }
    bool isActive() const { return m_state == State::Running; }
    double progress() const { return m_progress; } // < 0 means indeterminate
    QString statusText() const { return m_status; }
    QImage preview() const { return m_preview; }
    QString errorString() const { return m_error; }
    QVariant result() const { return m_result; }
    QDateTime createdAt() const { return m_created; }
    QDateTime startedAt() const { return m_started; }
    QDateTime finishedAt() const { return m_finished; }
    int attempt() const { return m_attempt; }
    bool canRetry() const { return m_state == State::Failed || m_state == State::Cancelled; }
    QVariantMap &metadata() { return m_metadata; }
    const QVariantMap &metadata() const { return m_metadata; }

    // Cancellation handler for running work (aborts requests, interrupts servers).
    // Reset on every (re)start; the starter installs it.
    void setCancelHandler(std::function<void()> handler) { m_cancelHandler = std::move(handler); }

    // Reporting API used by the work launched by the starter.
    void setProgress(double progress, const QString &status = QString());
    void setStatus(const QString &status);
    void setPreview(const QImage &preview);
    void succeed(const QVariant &result = {});
    void fail(const QString &error);
    void reportCancelled();
    bool isCancelRequested() const { return m_cancelRequested; }

    // Control API.
    void cancel();

signals:
    void stateChanged(lumen::Job::State state);
    void progressChanged(double progress, const QString &status);
    void previewChanged();
    void finished(lumen::Job *job);
    void titleChanged();

private:
    friend class JobQueue;
    void start();
    void resetForRetry();
    void setState(State s);

    QUuid m_id = QUuid::createUuid();
    QString m_title;
    QString m_group;
    QString m_kind;
    Starter m_starter;
    std::function<void()> m_cancelHandler;
    State m_state = State::Queued;
    double m_progress = -1.0;
    QString m_status;
    QImage m_preview;
    QString m_error;
    QVariant m_result;
    QDateTime m_created = QDateTime::currentDateTime();
    QDateTime m_started;
    QDateTime m_finished;
    int m_attempt = 0;
    bool m_cancelRequested = false;
    QVariantMap m_metadata;
};

} // namespace lumen
