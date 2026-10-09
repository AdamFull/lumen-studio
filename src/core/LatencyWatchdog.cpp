#include "core/LatencyWatchdog.h"

#include "core/Log.h"

namespace lumen {

namespace {
constexpr int kIntervalMs = 20;
}

LatencyWatchdog::LatencyWatchdog(int thresholdMs, QObject *parent)
    : QObject(parent)
    , m_threshold(thresholdMs)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(kIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &LatencyWatchdog::tick);
}

void LatencyWatchdog::start()
{
    m_clock.start();
    m_last = 0;
    m_timer.start();
}

void LatencyWatchdog::stop()
{
    m_timer.stop();
}

void LatencyWatchdog::tick()
{
    const qint64 now = m_clock.elapsed();
    const int late = static_cast<int>(now - m_last - kIntervalMs);
    m_last = now;
    if (late > m_threshold) {
        m_worst = qMax(m_worst, late);
        qCWarning(lcPerf) << "GUI thread stalled for" << late << "ms";
        emit stallDetected(late);
    }
}

} // namespace lumen
