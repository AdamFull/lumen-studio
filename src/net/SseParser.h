#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace lumen::net {

struct SseEvent
{
    QString event; // empty = "message"
    QByteArray data;
    QString id;
};

// Incremental Server-Sent Events parser (WHATWG spec): handles CRLF/LF, multi-line data,
// comments and events split across network chunks.
class SseParser
{
public:
    QList<SseEvent> feed(const QByteArray &chunk);
    // Flushes a trailing event that was not terminated by a blank line.
    QList<SseEvent> finish();
    void reset();

private:
    void processLine(const QByteArray &line, QList<SseEvent> &out);
    void dispatch(QList<SseEvent> &out);

    QByteArray m_buffer;
    QString m_event;
    QByteArray m_data;
    QString m_id;
    bool m_hasData = false;
};

// Newline-delimited JSON (e.g. Ollama native streaming): returns complete lines.
class LineSplitter
{
public:
    QList<QByteArray> feed(const QByteArray &chunk);
    QByteArray finish();

private:
    QByteArray m_buffer;
};

} // namespace lumen::net
