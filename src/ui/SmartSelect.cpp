#include "ui/SmartSelect.h"

#include "ai/GenerationService.h"
#include "ai/Provider.h"
#include "ai/ProviderRegistry.h"
#include "core/ImageOps.h"
#include "core/Job.h"
#include "runtime/Components.h"

#include <QJsonObject>
#include <QPromise>
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

QFuture<std::shared_ptr<ai::Segmenter>> SmartSelect::openSegmenter()
{
    if (m_segmenter)
        return QtFuture::makeReadyValueFuture(m_segmenter);
    // Opened once, on a worker thread (loading the models takes about half a second).
    if (!m_opening.isValid()) {
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
    QPointer<SmartSelect> self(this);
    return m_opening.then(this, [self](const std::shared_ptr<ai::Segmenter> &s) {
        if (self && !self->m_segmenter)
            self->m_segmenter = s;
        return s;
    });
}

QFuture<QImage> SmartSelect::segment(const QImage &picture, const ai::SegmentPrompt &prompt)
{
    if (!m_segmenter && !isReady())
        return QtFuture::makeExceptionalFuture<QImage>(std::make_exception_ptr(
            ai::ProviderError(tr("Smart selection needs ONNX Runtime and SAM 2.1, which are not downloaded yet."))));
    return openSegmenter().then(QtFuture::Launch::Async, [picture, prompt](const std::shared_ptr<ai::Segmenter> &s) {
        QString error;
        const QImage mask = s->segment(picture, prompt, &error);
        if (mask.isNull())
            throw ai::ProviderError(error);
        return mask;
    });
}

QStringList SmartSelect::objectComponentIds()
{
    return componentIds() << QStringLiteral("grounding-dino-tiny");
}

bool SmartSelect::objectsReady() const
{
    if (m_fakeDetect && m_segmenter)
        return true;
    for (const QString &id : objectComponentIds()) {
        if (!m_components->isInstalled(id))
            return false;
    }
    return true;
}

qint64 SmartSelect::objectsMissingSize() const
{
    return objectsReady() ? 0 : m_components->missingSize(objectComponentIds());
}

QList<Job *> SmartSelect::installObjects()
{
    QList<Job *> jobs;
    for (const QString &id : objectComponentIds()) {
        if (!m_components->isInstalled(id)) {
            if (Job *job = m_components->install(id)) {
                jobs << job;
                m_jobs << job;
            }
        }
    }
    return jobs;
}

QFuture<SmartSelect::FoundArea> SmartSelect::findObjects(const QImage &picture, const QString &description)
{
    if (!objectsReady()) {
        if (ai::ImageProvider *engine = objectsEngine())
            return findWithEngine(engine, picture, description);
        FoundArea none;
        none.error = tr("Finding objects by name needs the Grounding DINO model, which is not downloaded yet.");
        return QtFuture::makeReadyValueFuture(none);
    }
    // The detector, opened once on a worker thread like the segmenter (a test's stands in).
    QFuture<Detect> detect;
    if (m_fakeDetect) {
        detect = QtFuture::makeReadyValueFuture(Detect(m_fakeDetect));
    } else {
        if (!m_detector && !m_openingDetector.isValid()) {
            const QString dll = m_components->path(QStringLiteral("onnxruntime"), QStringLiteral("onnxruntime.dll"));
            const QString model = m_components->path(QStringLiteral("grounding-dino-tiny"), QStringLiteral("onnx/model_int8.onnx"));
            const QString vocab = m_components->path(QStringLiteral("grounding-dino-tiny"), QStringLiteral("vocab.txt"));
            m_openingDetector = QtConcurrent::run([dll, model, vocab]() -> std::shared_ptr<ai::GroundingDino> {
                QString error;
                auto rt = ai::OnnxRuntime::load(dll, &error);
                std::shared_ptr<ai::GroundingDino> d = rt ? ai::GroundingDino::open(rt, model, vocab, &error) : nullptr;
                if (!d)
                    throw ai::ProviderError(error);
                return d;
            });
        }
        QPointer<SmartSelect> self(this);
        QFuture<std::shared_ptr<ai::GroundingDino>> opened = m_detector ? QtFuture::makeReadyValueFuture(m_detector) : m_openingDetector;
        detect = opened.then(this, [self](const std::shared_ptr<ai::GroundingDino> &d) {
            if (self && !self->m_detector)
                self->m_detector = d;
            return Detect([d](const QImage &picture, const QString &words) {
                QString error;
                const QList<ai::Detection> found = d->detect(picture, words, 0.3f, &error);
                if (found.isEmpty() && !error.isEmpty())
                    throw ai::ProviderError(error);
                return found;
            });
        });
    }
    // The segmenter opens on the GUI thread's side; the finding and cutting out run on a worker.
    QFuture<std::shared_ptr<ai::Segmenter>> segmenter = openSegmenter();
    return detect
        .then(this, [segmenter](const Detect &d) mutable { return segmenter.then([d](const std::shared_ptr<ai::Segmenter> &s) { return std::make_pair(d, s); }); })
        .unwrap()
        .then(QtFuture::Launch::Async, [picture, description](const std::pair<Detect, std::shared_ptr<ai::Segmenter>> &both) {
            FoundArea area;
            QList<ai::Detection> boxes;
            try {
                boxes = both.first(picture, description);
            } catch (...) {
                area.error = ai::errorMessage(std::current_exception());
                return area;
            }
            if (boxes.isEmpty()) {
                area.error = tr("Nothing that looks like \"%1\" was found.").arg(description);
                return area;
            }
            QImage mask = img::createMask(picture.size());
            for (const ai::Detection &det : boxes) {
                ai::SegmentPrompt prompt;
                prompt.box = det.box;
                const QImage object = both.second->segment(picture, prompt, &area.error);
                if (object.isNull())
                    return area;
                mask = img::combineMasks(mask, object, img::MaskOp::Union);
            }
            area.mask = mask;
            area.objects = int(boxes.size());
            return area;
        })
        .onFailed([](const std::exception_ptr &e) {
            // Opening the models failed.
            FoundArea area;
            area.error = ai::errorMessage(e);
            return area;
        });
}

ai::ImageProvider *SmartSelect::objectsEngine() const
{
    if (!m_generation)
        return nullptr;
    // The engine for changes first, as for the filters' AI steps.
    const ai::ImageProvider *forChanges = m_generation->registry()->imageProviderFor(true);
    return m_generation->providerForUtility(ai::Utility::FindArea, forChanges ? forChanges->instanceId() : QString());
}

QFuture<SmartSelect::FoundArea> SmartSelect::findWithEngine(ai::ImageProvider *engine, const QImage &picture, const QString &description)
{
    auto promise = std::make_shared<QPromise<FoundArea>>();
    promise->start();
    const QFuture<FoundArea> future = promise->future();
    Job *job = m_generation->submitUtility(engine->instanceId(), ai::Utility::FindArea, picture,
                                           QJsonObject{{QStringLiteral("what"), description}}, tr("Find: %1").arg(description.left(40)),
                                           false);
    const QSize size = picture.size();
    connect(job, &Job::finished, this, [promise, size, description, name = engine->displayName()](Job *job) {
        FoundArea area;
        area.foundBy = name;
        if (job->state() == Job::State::Cancelled) {
            area.error = tr("Looking for \"%1\" was cancelled.").arg(description);
        } else if (job->state() == Job::State::Failed) {
            area.error = job->errorString();
        } else {
            area.mask = ai::areaFromMasks(ai::GenerationService::outputOf(job).images, size, &area.objects);
            if (area.mask.isNull())
                area.error = tr("Nothing that looks like \"%1\" was found.").arg(description);
        }
        promise->addResult(area);
        promise->finish();
    });
    return future;
}

} // namespace lumen::ui
