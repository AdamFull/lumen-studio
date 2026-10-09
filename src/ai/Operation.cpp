#include "ai/Operation.h"

#include "core/Log.h"

namespace lumen::ai {

Operation::Operation(QObject *parent)
    : QObject(parent)
{
}

void Operation::cancel()
{
    if (isFinished() || m_cancelRequested)
        return;
    m_cancelRequested = true;
    if (m_cancelHandler) {
        auto handler = m_cancelHandler;
        handler();
    } else {
        reportCancelled();
    }
}

void Operation::reportProgress(double fraction, const QString &status)
{
    if (!isFinished())
        emit progress(fraction, status);
}

void Operation::reportPreview(const QImage &image)
{
    if (!isFinished() && !image.isNull())
        emit preview(image);
}

void Operation::fail(const QString &error)
{
    if (isFinished())
        return;
    if (m_cancelRequested) {
        end(State::Cancelled);
        return;
    }
    m_error = error;
    qCWarning(lcAi).noquote() << "Operation failed:" << log::redact(error);
    end(State::Failed);
}

void Operation::reportCancelled()
{
    if (!isFinished()) {
        m_error = tr("Cancelled");
        end(State::Cancelled);
    }
}

void Operation::complete()
{
    if (m_cancelRequested) {
        reportCancelled();
        return;
    }
    end(State::Succeeded);
}

void Operation::end(State s)
{
    m_state = s;
    m_cancelHandler = nullptr;
    emit finished();
    if (m_autoDelete)
        deleteLater();
}

void ImageOperation::succeed(ImageResult result)
{
    if (isFinished())
        return;
    m_result = std::move(result);
    complete();
}

void ChatOperation::succeed(ChatResult result)
{
    if (isFinished())
        return;
    m_result = std::move(result);
    complete();
}

void ChatOperation::appendText(const QString &delta)
{
    if (delta.isEmpty() || isFinished())
        return;
    m_partial += delta;
    emit textDelta(delta);
}

void ChatOperation::appendReasoning(const QString &delta)
{
    if (!delta.isEmpty() && !isFinished())
        emit reasoningDelta(delta);
}

} // namespace lumen::ai
