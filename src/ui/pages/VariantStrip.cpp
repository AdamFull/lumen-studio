#include "ui/pages/VariantStrip.h"

#include "ai/GenerationService.h"
#include "core/ImageOps.h"
#include "core/Job.h"
#include "doc/Project.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/widgets/Controls.h"

#include <QApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>

namespace lumen::ui {

namespace {

constexpr int kThumbHeight = 48;

QImage fitted(const QImage &image, const QSize &size)
{
    return image.size() == size ? image : img::normalized(image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

} // namespace

VariantStrip::VariantStrip(Studio *studio, CanvasView *canvas, QWidget *parent)
    : QFrame(parent)
    , m_studio(studio)
    , m_canvas(canvas)
{
    setObjectName(QStringLiteral("variantStrip"));
    setAccessibleName(tr("Results"));
    setFocusPolicy(Qt::StrongFocus);
    const Palette &pal = Theme::palette();
    setStyleSheet(QStringLiteral("#variantStrip { background: %1; border-top: 1px solid %2; }").arg(pal.panel.name(), pal.border.name()));
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(10, 6, 10, 6);
    h->setSpacing(6);
    m_thumbs = new QHBoxLayout;
    m_thumbs->setSpacing(4);
    h->addLayout(m_thumbs);
    m_label = new ElidedLabel(QString(), this);
    m_label->setObjectName(QStringLiteral("variantLabel"));
    m_label->setProperty("role", QStringLiteral("muted"));
    h->addSpacing(6);
    h->addWidget(m_label, 1);
    m_keep = new QPushButton(Icons::get(QStringLiteral("check")), tr("Keep"), this);
    m_keep->setObjectName(QStringLiteral("variantKeep"));
    m_keep->setToolTip(tr("Add this result as a new layer (Enter)"));
    Theme::setRole(m_keep, QStringLiteral("primary"));
    m_discard = new QPushButton(Icons::get(QStringLiteral("close")), tr("Discard"), this);
    m_discard->setObjectName(QStringLiteral("variantDiscard"));
    m_discard->setToolTip(tr("Drop all the results; the image stays as it is (Esc)"));
    h->addWidget(m_keep);
    h->addWidget(m_discard);
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    connect(m_group, &QButtonGroup::idClicked, this, [this](int index) {
        setCurrent(index);
        setFocus();
    });
    connect(m_keep, &QPushButton::clicked, this, &VariantStrip::keep);
    connect(m_discard, &QPushButton::clicked, this, &VariantStrip::discard);
    // The line says which keys work, and they work only where the keyboard is.
    connect(qApp, &QApplication::focusChanged, this, [this] {
        if (isVisible())
            m_label->setFullText(label());
    });
    hide();
}

VariantStrip::Run *VariantStrip::shown()
{
    auto it = m_doc ? m_runs.find(m_doc.data()) : m_runs.end();
    return it == m_runs.end() ? nullptr : &it.value();
}

const VariantStrip::Run *VariantStrip::shown() const
{
    auto it = m_doc ? m_runs.constFind(m_doc.data()) : m_runs.cend();
    return it == m_runs.cend() ? nullptr : &it.value();
}

bool VariantStrip::Run::has(const Job *job) const
{
    return std::any_of(jobs.cbegin(), jobs.cend(), [job](const Tracked &t) { return t.job == job; });
}

bool VariantStrip::running(const Run &run)
{
    return run.submitting > 0 || std::any_of(run.jobs.cbegin(), run.jobs.cend(), [](const Tracked &t) { return t.job && !t.job->isFinished(); });
}

VariantStrip::Run &VariantStrip::runFor(doc::Document *doc)
{
    if (!m_runs.contains(doc)) {
        // Results no longer fit a document that changed its size, and go with a closed one.
        connect(doc, &QObject::destroyed, this, [this, doc] {
            m_runs.remove(doc);
            rebuild();
        });
        connect(doc, &doc::Document::sizeChanged, this, [this, doc] { drop(doc); });
    }
    return m_runs[doc];
}

void VariantStrip::setDocument(doc::Document *doc)
{
    m_doc = doc;
    rebuild();
}

void VariantStrip::expect(doc::Document *doc)
{
    if (!doc)
        return;
    Run &run = runFor(doc);
    // A new run replaces the results on show (Generate again), unless one is still going.
    if (!running(run))
        run = Run();
    ++run.submitting;
    rebuild();
}

void VariantStrip::track(doc::Document *doc, const QList<Job *> &jobs, const QString &name, int results)
{
    if (!doc)
        return;
    Run &run = runFor(doc);
    const bool stopped = run.stopped;
    if (run.submitting > 0)
        --run.submitting;
    else if (!running(run) && !jobs.isEmpty())
        run = Run();
    if (run.submitting == 0)
        run.stopped = false;
    if (jobs.isEmpty()) {
        // An expected run that could not be submitted.
        removeIfEmpty(doc);
        rebuild();
        return;
    }
    // Each job makes its share (one each for engines that make one image per request).
    const int each = std::max(1, results / int(jobs.size()));
    for (Job *job : jobs) {
        run.jobs << Tracked{job, each};
        watch(doc, job, name, each);
    }
    if (stopped) {
        for (Job *job : jobs)
            job->cancel();
    }
    rebuild();
}

void VariantStrip::watch(doc::Document *doc, Job *job, const QString &name, int results)
{
    QPointer<doc::Document> target(doc);
    connect(job, &Job::finished, this, [this, target, name](Job *j) {
        if (target)
            finished(target, j, name);
    });
    connect(job, &Job::progressChanged, this, [this, target] {
        if (target && target == m_doc)
            emit stateChanged();
    });
    // Retry in the Queue puts a failed or cancelled job back in the queue: its results come here again.
    connect(job, &Job::stateChanged, this, [this, target, job, results](Job::State state) {
        if (target && state == Job::State::Queued)
            rejoin(target, job, results);
    });
}

void VariantStrip::rejoin(doc::Document *doc, Job *job, int results)
{
    Run &run = runFor(doc);
    if (run.has(job))
        return;
    // Joins what is on show: the user asked for these results again.
    run.jobs << Tracked{job, results};
    rebuild();
}

void VariantStrip::finished(doc::Document *doc, Job *job, const QString &name)
{
    auto it = m_runs.find(doc);
    if (it == m_runs.end() || !it->has(job))
        return;
    Run &run = it.value();
    run.jobs.removeIf([job](const Tracked &t) { return t.job == job || !t.job; });
    const bool first = run.variants.isEmpty();
    if (job->state() == Job::State::Succeeded) {
        const ai::GenerationOutput out = ai::GenerationService::outputOf(job);
        // A result of a selection shows the area it changed (the engine's picture of it).
        const bool region = !out.region.isEmpty() && out.rawImages.size() == out.images.size();
        for (int i = 0; i < out.images.size(); ++i) {
            QJsonObject meta{{QStringLiteral("generated"), true}};
            if (i < out.assets.size() && !out.assets[i].id.isEmpty())
                meta.insert(QStringLiteral("asset"), out.assets[i].id);
            run.variants << Variant{fitted(out.images[i], doc->size()), region ? out.rawImages[i] : out.images[i], name, meta};
        }
    } else if (job->state() == Job::State::Failed) {
        m_studio->notify(job->errorString(), QStringLiteral("error"));
    }
    removeIfEmpty(doc);
    rebuild();
    if (first && doc == m_doc && count() > 0)
        emit resultsArrived();
}

void VariantStrip::removeIfEmpty(doc::Document *doc)
{
    auto it = m_runs.find(doc);
    if (it == m_runs.end() || !it->variants.isEmpty() || it->submitting > 0)
        return;
    if (running(*it))
        return;
    m_runs.erase(it);
    disconnect(doc, nullptr, this, nullptr);
}

void VariantStrip::addVariants(doc::Document *doc, const QList<QImage> &images, const QString &name, const QJsonObject &meta,
                               const QList<QImage> &thumbs)
{
    if (!doc || images.isEmpty())
        return;
    Run &run = runFor(doc);
    const bool first = run.variants.isEmpty();
    for (int i = 0; i < images.size(); ++i)
        run.variants << Variant{fitted(images[i], doc->size()), thumbs.value(i, images[i]), name, meta};
    rebuild();
    if (first && doc == m_doc)
        emit resultsArrived();
}

void VariantStrip::drop(doc::Document *doc)
{
    auto it = m_runs.find(doc);
    if (it == m_runs.end())
        return;
    const QList<Tracked> jobs = it->jobs;
    // A run still being prepared keeps its place, so that its jobs (they come later) are shown.
    if (it->submitting > 0) {
        const int submitting = it->submitting;
        *it = Run();
        it->submitting = submitting;
    } else {
        m_runs.erase(it);
        disconnect(doc, nullptr, this, nullptr);
    }
    for (const Tracked &t : jobs) {
        if (t.job && !t.job->isFinished())
            t.job->cancel();
    }
    rebuild();
}

int VariantStrip::count() const
{
    const Run *run = shown();
    return run ? int(run->variants.size()) : 0;
}

int VariantStrip::current() const
{
    const Run *run = shown();
    return run ? run->current : -1;
}

void VariantStrip::setCurrent(int index)
{
    Run *run = shown();
    if (!run || run->variants.isEmpty())
        return;
    run->current = std::clamp(index, 0, int(run->variants.size()) - 1);
    refresh();
}

QImage VariantStrip::currentImage() const
{
    const Run *run = shown();
    return run && !run->variants.isEmpty() ? run->variants.value(run->current).image : QImage();
}

QString VariantStrip::currentName() const
{
    const Run *run = shown();
    return run && !run->variants.isEmpty() ? run->variants.value(run->current).name : QString();
}

bool VariantStrip::isRunning() const
{
    const Run *run = shown();
    return run && running(*run);
}

double VariantStrip::progress() const
{
    const Run *run = shown();
    if (!run)
        return -1.0;
    double sum = 0;
    int known = 0;
    for (const Tracked &t : run->jobs) {
        if (t.job && !t.job->isFinished() && t.job->progress() >= 0) {
            sum += t.job->progress();
            ++known;
        }
    }
    return known > 0 ? sum / known : -1.0;
}

int VariantStrip::pending() const
{
    const Run *run = shown();
    if (!run)
        return 0;
    int results = 0;
    for (const Tracked &t : run->jobs) {
        if (t.job && !t.job->isFinished())
            results += t.results;
    }
    return results;
}

QString VariantStrip::status() const
{
    const Run *run = shown();
    if (!run)
        return QString();
    for (const Tracked &t : run->jobs) {
        if (t.job && !t.job->isFinished())
            return t.job->statusText();
    }
    return QString();
}

QString VariantStrip::label() const
{
    const Run *run = shown();
    const int n = run ? int(run->variants.size()) : 0;
    if (n == 0)
        return QString();
    if (isRunning())
        return tr("Result %1 of %2 · more on the way").arg(run->current + 1).arg(n);
    // The keys work while the strip or the canvas has the keyboard, not while the user types.
    QWidget *focus = QApplication::focusWidget();
    const bool keys = !focus || focus == this || isAncestorOf(focus) || focus == m_canvas->focusProxy();
    if (n == 1)
        return keys ? tr("Keep it (Enter) or discard it (Esc)") : tr("Keep it or discard it");
    return keys ? tr("Result %1 of %2 · ←/→ to compare · Generate again replaces these").arg(run->current + 1).arg(n)
                : tr("Result %1 of %2 · click one to compare · Generate again replaces these").arg(run->current + 1).arg(n);
}

void VariantStrip::keep()
{
    Run *run = shown();
    if (!run || run->variants.isEmpty())
        return;
    const Variant v = run->variants.value(run->current);
    doc::Document *doc = m_doc;
    drop(doc);
    // On top, so the layer looks as the preview did; one step in the history.
    const QUuid id = doc->addLayer(v.name, v.image, v.meta, doc->layerCount(), tr("Keep AI result"));
    doc->setActiveLayer(id);
    // The project's images remember which results were kept (the start screen shows only those).
    if (doc::Project *project = m_studio->project()) {
        if (auto info = project->asset(v.meta.value(QStringLiteral("asset")).toString())) {
            info->metadata.insert(QStringLiteral("kept"), true);
            project->updateAsset(*info);
        }
    }
}

void VariantStrip::discard()
{
    if (m_doc)
        drop(m_doc);
}

void VariantStrip::stop()
{
    Run *run = shown();
    if (!run)
        return;
    if (run->submitting > 0)
        run->stopped = true;
    const QList<Tracked> jobs = run->jobs;
    for (const Tracked &t : jobs) {
        if (t.job && !t.job->isFinished())
            t.job->cancel();
    }
}

bool VariantStrip::handleKey(QKeyEvent *e)
{
    if (count() == 0 || (e->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier)
        return false;
    switch (e->key()) {
    case Qt::Key_Left:
        setCurrent((current() + count() - 1) % count());
        return true;
    case Qt::Key_Right:
        setCurrent((current() + 1) % count());
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        keep();
        return true;
    case Qt::Key_Escape:
        discard();
        return true;
    default:
        return false;
    }
}

void VariantStrip::keyPressEvent(QKeyEvent *e)
{
    if (!handleKey(e))
        QFrame::keyPressEvent(e);
}

void VariantStrip::rebuild()
{
    for (QAbstractButton *b : m_group->buttons()) {
        m_group->removeButton(b);
        m_thumbs->removeWidget(b);
        b->hide();
        b->deleteLater();
    }
    const Run *run = shown();
    const int n = run ? int(run->variants.size()) : 0;
    for (int i = 0; i < n; ++i) {
        const QImage &thumb = run->variants[i].thumb;
        const QSize size = thumb.size().scaled(QSize(kThumbHeight * 2, kThumbHeight), Qt::KeepAspectRatio).expandedTo(QSize(16, 16));
        auto *b = new QToolButton(this);
        b->setCheckable(true);
        b->setIcon(QIcon(QPixmap::fromImage(thumb.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
        b->setIconSize(size);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAccessibleName(tr("Result %1").arg(i + 1));
        b->setToolTip(tr("Result %1 (←/→)").arg(i + 1));
        m_group->addButton(b, i);
        m_thumbs->addWidget(b);
    }
    refresh();
}

void VariantStrip::refresh()
{
    const Run *run = shown();
    const bool show = run && !run->variants.isEmpty();
    setVisible(show);
    if (show) {
        if (QAbstractButton *b = m_group->button(run->current))
            b->setChecked(true);
        m_label->setFullText(label());
    }
    if (m_canvas->document() == m_doc)
        m_canvas->setPreview(CanvasView::Preview::Result, show ? run->variants.value(run->current).image : QImage());
    emit stateChanged();
}

} // namespace lumen::ui
