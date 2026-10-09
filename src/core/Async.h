#pragma once

#include <QFuture>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <memory>
#include <utility>

namespace lumen::async {

// Shared worker pool used for CPU-heavy work (image codecs, compositing, thumbnails...).
// Never run long blocking waits on it.
inline QThreadPool *pool()
{
    static QThreadPool *p = [] {
        auto *tp = new QThreadPool;
        tp->setObjectName(QStringLiteral("LumenWorkers"));
        return tp;
    }();
    return p;
}

inline void setMaxThreads(int n)
{
    pool()->setMaxThreadCount(qMax(1, n));
}

// Runs fn on the worker pool and returns a future. Chain with
// `.then(contextObject, [](Result r){...})` to receive the result on the context's thread.
template<typename Fn>
auto run(Fn &&fn)
{
    return QtConcurrent::run(pool(), std::forward<Fn>(fn));
}

// Cooperative cancellation flag shareable between threads.
class CancelToken
{
public:
    CancelToken() : m_flag(std::make_shared<std::atomic_bool>(false)) {}
    void cancel() const { m_flag->store(true, std::memory_order_relaxed); }
    bool isCancelled() const { return m_flag->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic_bool> m_flag;
};

} // namespace lumen::async
