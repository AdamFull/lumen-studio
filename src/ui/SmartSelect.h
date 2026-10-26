#pragma once

#include "ai/Segmenter.h"

#include <QFuture>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <memory>

namespace lumen {
class Job;
}

namespace lumen::runtime {
class Components;
}

namespace lumen::ui {

// Smart selection's engine (plan M18 §18.1): SAM 2.1 tiny through ONNX Runtime, both components
// downloaded the first time (about 145 MB), opened on a worker thread on first use and kept.
// Segmenting runs off the GUI thread; one at a time.
class SmartSelect : public QObject
{
    Q_OBJECT
public:
    SmartSelect(runtime::Components *components, QObject *parent = nullptr);

    // The components it needs, and whether they are all there.
    static QStringList componentIds();
    bool isReady() const;
    qint64 missingSize() const;
    // Starts downloading what is missing (Queue jobs); the jobs, the running ones included.
    QList<Job *> install();
    // A download it started is still running.
    bool isInstalling() const;

    // The object the prompt points at in `picture` (Grayscale8, the picture's size). Fails with the
    // reason in plain words.
    QFuture<QImage> segment(const QImage &picture, const ai::SegmentPrompt &prompt);

    // Tests: a segmenter of their own instead of SAM 2.
    void setSegmenter(std::shared_ptr<ai::Segmenter> segmenter) { m_segmenter = std::move(segmenter); }

signals:
    // Every component it needs is installed.
    void ready();

private:
    runtime::Components *m_components;
    std::shared_ptr<ai::Segmenter> m_segmenter;
    QFuture<std::shared_ptr<ai::Segmenter>> m_opening;
    QList<QPointer<Job>> m_jobs;
};

} // namespace lumen::ui
