#include "ui/SmartSelect.h"

#include "ai/Provider.h"
#include "core/Job.h"
#include "runtime/Components.h"

#include <QtConcurrent/QtConcurrentRun>

namespace lumen::ui {

SmartSelect::SmartSelect(runtime::Components *components, QObject *parent)
    : QObject(parent)
    , m_components(components)
{
    connect(components, &runtime::Components::installed, this, [this] {
        if (isReady())
            emit ready();
    });
}

QStringList SmartSelect::componentIds()
{
    return {QStringLiteral("onnxruntime"), QStringLiteral("sam2.1-tiny")};
}

bool SmartSelect::isReady() const
{
    if (m_segmenter)
        return true;
    for (const QString &id : componentIds()) {
        if (!m_components->isInstalled(id))
            return false;
    }
    return true;
}

qint64 SmartSelect::missingSize() const
{
    return m_segmenter ? 0 : m_components->missingSize(componentIds());
}

QList<Job *> SmartSelect::install()
{
    QList<Job *> jobs;
    for (const QString &id : componentIds()) {
        if (!m_components->isInstalled(id)) {
            if (Job *job = m_components->install(id)) {
                jobs << job;
                m_jobs << job;
            }
        }
    }
    return jobs;
}

bool SmartSelect::isInstalling() const
{
    for (const QPointer<Job> &job : m_jobs) {
        if (job && !job->isFinished())
            return true;
    }
    return false;
}

QFuture<QImage> SmartSelect::segment(const QImage &picture, const ai::SegmentPrompt &prompt)
{
    // Opened once, on a worker thread (loading the models takes about half a second).
    if (!m_segmenter && !m_opening.isValid()) {
        if (!isReady())
            return QtFuture::makeExceptionalFuture<QImage>(std::make_exception_ptr(
                ai::ProviderError(tr("Smart selection needs ONNX Runtime and SAM 2.1, which are not downloaded yet."))));
        const QString dll = m_components->path(QStringLiteral("onnxruntime"), QStringLiteral("onnxruntime.dll"));
        const QString encoder = m_components->path(QStringLiteral("sam2.1-tiny"), QStringLiteral("onnx/vision_encoder_int8.onnx"));
        const QString decoder = m_components->path(QStringLiteral("sam2.1-tiny"), QStringLiteral("onnx/prompt_encoder_mask_decoder_int8.onnx"));
        m_opening = QtConcurrent::run([dll, encoder, decoder]() -> std::shared_ptr<ai::Segmenter> {
            QString error;
            auto rt = ai::OnnxRuntime::load(dll, &error);
            std::shared_ptr<ai::Segmenter> s = rt ? ai::Sam2Segmenter::open(rt, encoder, decoder, &error) : nullptr;
            if (!s)
                throw ai::ProviderError(error);
            return s;
        });
    }
    QFuture<std::shared_ptr<ai::Segmenter>> segmenter = m_segmenter ? QtFuture::makeReadyValueFuture(m_segmenter) : m_opening;
    QPointer<SmartSelect> self(this);
    return segmenter
        .then(this, [self](const std::shared_ptr<ai::Segmenter> &s) {
            if (self && !self->m_segmenter)
                self->m_segmenter = s;
            return s;
        })
        .then(QtFuture::Launch::Async, [picture, prompt](const std::shared_ptr<ai::Segmenter> &s) {
            QString error;
            const QImage mask = s->segment(picture, prompt, &error);
            if (mask.isNull())
                throw ai::ProviderError(error);
            return mask;
        });
}

} // namespace lumen::ui
