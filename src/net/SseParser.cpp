#include "net/SseParser.h"

namespace lumen::net {

QList<SseEvent> SseParser::feed(const QByteArray &chunk)
{
    QList<SseEvent> out;
    m_buffer.append(chunk);
    qsizetype start = 0;
    while (true) {
        qsizetype nl = -1;
        for (qsizetype i = start; i < m_buffer.size(); ++i) {
            if (m_buffer.at(i) == '\n' || m_buffer.at(i) == '\r') {
                nl = i;
                break;
            }
        }
        if (nl < 0)
            break;
        // A lone '\r' at the end of the buffer may be followed by '\n' in the next chunk.
        if (m_buffer.at(nl) == '\r' && nl + 1 >= m_buffer.size())
            break;
        const QByteArray line = m_buffer.mid(start, nl - start);
        qsizetype next = nl + 1;
        if (m_buffer.at(nl) == '\r' && next < m_buffer.size() && m_buffer.at(next) == '\n')
            ++next;
        start = next;
        processLine(line, out);
    }
    m_buffer.remove(0, start);
    return out;
}

QList<SseEvent> SseParser::finish()
{
    QList<SseEvent> out;
    if (!m_buffer.isEmpty()) {
        QByteArray line = m_buffer;
        if (line.endsWith('\r'))
            line.chop(1);
        m_buffer.clear();
        processLine(line, out);
    }
    dispatch(out);
    return out;
}

void SseParser::reset()
{
    m_buffer.clear();
    m_event.clear();
    m_data.clear();
    m_id.clear();
    m_hasData = false;
}

void SseParser::processLine(const QByteArray &line, QList<SseEvent> &out)
{
    if (line.isEmpty()) {
        dispatch(out);
        return;
    }
    if (line.startsWith(':'))
        return; // comment / keep-alive
    const qsizetype colon = line.indexOf(':');
    QByteArray field = colon < 0 ? line : line.left(colon);
    QByteArray value = colon < 0 ? QByteArray() : line.mid(colon + 1);
    if (value.startsWith(' '))
        value.remove(0, 1);
    if (field == "data") {
        if (m_hasData)
            m_data.append('\n');
        m_data.append(value);
        m_hasData = true;
    } else if (field == "event") {
        m_event = QString::fromUtf8(value);
    } else if (field == "id") {
        m_id = QString::fromUtf8(value);
    }
}

void SseParser::dispatch(QList<SseEvent> &out)
{
    if (m_hasData)
        out.append(SseEvent{m_event, m_data, m_id});
    m_event.clear();
    m_data.clear();
    m_hasData = false;
}

QList<QByteArray> LineSplitter::feed(const QByteArray &chunk)
{
    QList<QByteArray> lines;
    m_buffer.append(chunk);
    qsizetype start = 0;
    qsizetype nl;
    while ((nl = m_buffer.indexOf('\n', start)) >= 0) {
        QByteArray line = m_buffer.mid(start, nl - start).trimmed();
        if (!line.isEmpty())
            lines.append(line);
        start = nl + 1;
    }
    m_buffer.remove(0, start);
    return lines;
}

QByteArray LineSplitter::finish()
{
    QByteArray rest = m_buffer.trimmed();
    m_buffer.clear();
    return rest;
}

} // namespace lumen::net
