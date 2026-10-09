#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace lumen {

// Detects GUI-thread stalls: a high-frequency timer measures how late it fires. Any lateness above
// the threshold is logged to the lumen.perf category so blocking code can be found and fixed.
class LatencyWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit LatencyWatchdog(int thresholdMs = 50, QObject *parent = nullptr);

    void start();
    void stop();
    int worstStallMs() const { return m_worst; }

signals:
    void stallDetected(int milliseconds);

private:
    void tick();

    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    int m_threshold;
    int m_worst = 0;
};

} // namespace lumen
