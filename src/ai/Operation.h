#pragma once

#include "ai/Types.h"

#include <QImage>
#include <QObject>

#include <functional>

namespace lumen::ai {

// A long-running provider call with progress, optional live previews and cancellation.
// Lives on the GUI thread. Emits finished() exactly once; deletes itself afterwards unless
// setAutoDelete(false) was called.
class Operation : public QObject
{
    Q_OBJECT
public:
    enum class State { Running, Succeeded, Failed, Cancelled };

    explicit Operation(QObject *parent = nullptr);

    State state() const { return m_state; }
    bool isFinished() const { return m_state != State::Running; }
    QString errorString() const { return m_error; }
    void setAutoDelete(bool on) { m_autoDelete = on; }

    void cancel();
    // Installed by the provider: aborts requests / interrupts the server.
    void onCancel(std::function<void()> handler) { m_cancelHandler = std::move(handler); }

    // Reporting (provider side).
    void reportProgress(double fraction, const QString &status = QString());
    void reportPreview(const QImage &preview);
    void fail(const QString &error);
    void reportCancelled();

signals:
    void progress(double fraction, const QString &status); // fraction < 0 = indeterminate
    void preview(const QImage &image);
    void finished();

protected:
    void complete();

private:
    void end(State s);
    State m_state = State::Running;
    QString m_error;
    std::function<void()> m_cancelHandler;
    bool m_cancelRequested = false;
    bool m_autoDelete = true;
};

class ImageOperation : public Operation
{
    Q_OBJECT
public:
    using Operation::Operation;
    const ImageResult &result() const { return m_result; }
    void succeed(ImageResult result);

private:
    ImageResult m_result;
};

class ChatOperation : public Operation
{
    Q_OBJECT
public:
    using Operation::Operation;
    const ChatResult &result() const { return m_result; }
    void succeed(ChatResult result);
    // Streaming (provider side).
    void appendText(const QString &delta);
    void appendReasoning(const QString &delta);
    const QString &partialText() const { return m_partial; }

signals:
    void textDelta(const QString &delta);
    void reasoningDelta(const QString &delta);

private:
    ChatResult m_result;
    QString m_partial;
};

} // namespace lumen::ai
