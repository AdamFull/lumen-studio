#include "ui/dialogs/LocalSetupDialog.h"

#include "ai/GenerationService.h"
#include "ai/ProviderRegistry.h"
#include "core/Error.h"
#include "core/Job.h"
#include "runtime/ModelLibrary.h"
#include "runtime/RuntimeStore.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPromise>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lumen::ui {

using runtime::StarterFile;
using runtime::StarterSet;

namespace {

const QString kLlama = QStringLiteral("llama.cpp");
const QString kSd = QStringLiteral("sd.cpp");

bool forLanguage(const StarterFile &f)
{
    return f.role == QLatin1String("language") || f.role == QLatin1String("projector");
}

// The runtime to reuse: the newest managed install, else the first adopted one.
QString existingRuntime(Studio *studio, const QString &kind)
{
    if (const auto rt = studio->runtimes()->latestManaged(kind))
        return rt->id;
    const QList<runtime::InstalledRuntime> all = studio->runtimes()->runtimes(kind);
    return all.isEmpty() ? QString() : all.first().id;
}

QString setLabel(const QString &id)
{
    if (id == QLatin1String("compact"))
        return LocalSetupDialog::tr("Compact — for GPUs under 8 GB and computers without one");
    if (id == QLatin1String("standard"))
        return LocalSetupDialog::tr("Standard — for 8 GB GPUs");
    if (id == QLatin1String("large"))
        return LocalSetupDialog::tr("Large — for GPUs with 12 GB or more");
    return id;
}

} // namespace

// ---- LocalSetup -------------------------------------------------------------------------------------

LocalSetup::LocalSetup(Studio *studio, QObject *parent)
    : QObject(parent)
    , m_studio(studio)
    , m_sets(runtime::starterSets())
{
}

QStringList LocalSetup::kindsFor(const Options &o)
{
    QStringList kinds;
    if (o.language)
        kinds << kLlama;
    if (o.images)
        kinds << kSd;
    return kinds;
}

QFuture<void> LocalSetup::prepare()
{
    auto promise = std::make_shared<QPromise<void>>();
    promise->start();
    QFuture<void> future = promise->future();
    QPointer<LocalSetup> self(this);
    runtime::probeHardware().then(this, [self, promise](runtime::HardwareInfo hw) {
        if (!self) {
            promise->finish();
            return;
        }
        self->m_hw = hw;
        auto pending = std::make_shared<int>(0);
        auto done = [promise, pending] {
            if (--*pending == 0)
                promise->finish();
        };
        ++*pending; // held until every lookup is started
        for (const QString &kind : {kLlama, kSd}) {
            RuntimeStep step;
            step.kind = kind;
            step.existingId = existingRuntime(self->m_studio, kind);
            self->m_runtimeSteps.insert(kind, step);
            const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(kind);
            if (!step.existingId.isEmpty() || !spec)
                continue;
            ++*pending;
            const runtime::RuntimeSpec specCopy = *spec;
            self->m_studio->runtimes()
                ->feed()
                ->fetch(specCopy)
                .then(self, [self, kind, specCopy, done](const QList<runtime::Release> &releases) {
                    if (self) {
                        RuntimeStep &s = self->m_runtimeSteps[kind];
                        s.plan = runtime::planInstall(specCopy, runtime::recommendedBackend(specCopy, *self->m_hw), releases);
                        if (!s.plan.ok())
                            s.error = s.plan.error;
                    }
                    done();
                })
                .onFailed(self, [self, kind, done](const std::exception &e) {
                    if (self)
                        self->m_runtimeSteps[kind].error = describeException(e);
                    done();
                });
        }
        done();
    });
    return future;
}

QString LocalSetup::recommendedSet() const
{
    const StarterSet *s = runtime::pickStarterSet(m_sets, m_hw ? m_hw->maxVram() : 0);
    return s ? s->id : QString();
}

QList<LocalSetup::FileStep> LocalSetup::fileSteps(const Options &o) const
{
    QList<FileStep> out;
    for (const StarterSet &s : m_sets) {
        if (s.id != o.setId)
            continue;
        for (const StarterFile &f : s.files) {
            if (forLanguage(f) ? !o.language : !o.images)
                continue;
            FileStep step;
            step.file = f;
            // Already here: the same checksum, or the same name and size (as saved by Lumen, or as
            // published).
            runtime::ModelLibrary *lib = m_studio->models();
            auto local = lib->find(f.sha256, f.fileName(), f.size);
            if (!local)
                local = lib->find(QString(), f.path.section(QLatin1Char('/'), -1), f.size);
            if (local)
                step.localPath = local->path;
            out << step;
        }
    }
    return out;
}

qint64 LocalSetup::downloadSize(const Options &o) const
{
    qint64 total = 0;
    for (const FileStep &f : fileSteps(o)) {
        if (f.localPath.isEmpty())
            total += f.file.size;
    }
    for (const QString &kind : kindsFor(o)) {
        const RuntimeStep s = m_runtimeSteps.value(kind);
        if (s.existingId.isEmpty() && s.plan.ok())
            total += m_studio->runtimes()->pendingDownload(s.plan);
    }
    return total;
}

void LocalSetup::run(const Options &o)
{
    m_options = o;
    m_running = true;
    m_jobs.clear();
    m_runtimeJobs.clear();
    m_fileJobs.clear();
    m_paths.clear();
    for (const QString &kind : kindsFor(o)) {
        const RuntimeStep s = m_runtimeSteps.value(kind);
        // Installed meanwhile (an earlier attempt)?
        if (!s.existingId.isEmpty() || !existingRuntime(m_studio, kind).isEmpty())
            continue;
        if (!s.plan.ok()) {
            m_running = false;
            emit finished(false, tr("%1 cannot be downloaded: %2").arg(kind, s.error.isEmpty() ? tr("no release found") : s.error));
            return;
        }
        Job *job = m_studio->runtimes()->install(s.plan);
        m_runtimeJobs.insert(kind, job);
        m_jobs << job;
    }
    for (const FileStep &f : fileSteps(o)) {
        if (!f.localPath.isEmpty()) {
            m_paths.insert(f.file.role, f.localPath);
            continue;
        }
        Job *job = m_studio->hub()->download(runtime::starterDownload(f.file, m_studio->hub()->baseUrl(runtime::HubSource::HuggingFace)));
        m_fileJobs.insert(f.file.role, job);
        m_jobs << job;
    }
    for (const QPointer<Job> &job : std::as_const(m_jobs))
        connect(job, &Job::finished, this, &LocalSetup::checkDone);
    emit jobsChanged();
    checkDone();
}

void LocalSetup::checkDone()
{
    if (!m_running)
        return;
    QStringList errors;
    for (const QPointer<Job> &job : std::as_const(m_jobs)) {
        if (!job) {
            errors << tr("A step was removed from the Queue.");
            continue;
        }
        if (!job->isFinished())
            return;
        if (job->state() != Job::State::Succeeded)
            errors << QStringLiteral("%1: %2").arg(job->title(), job->state() == Job::State::Cancelled ? tr("cancelled") : job->errorString());
    }
    m_running = false;
    if (!errors.isEmpty()) {
        emit finished(false, errors.join(QLatin1Char('\n')));
        return;
    }
    configure();
}

void LocalSetup::configure()
{
    for (auto it = m_fileJobs.cbegin(); it != m_fileJobs.cend(); ++it)
        m_paths.insert(it.key(), it.value()->result().toString());
    auto runtimeId = [this](const QString &kind) {
        if (m_runtimeJobs.contains(kind))
            return m_runtimeJobs.value(kind)->result().toString();
        const QString existing = m_runtimeSteps.value(kind).existingId;
        return existing.isEmpty() ? existingRuntime(m_studio, kind) : existing;
    };
    const runtime::VramTier tier = runtime::vramTierFor(m_hw ? m_hw->maxVram() : 0);
    runtime::ServerSupervisor *servers = m_studio->servers();
    const StarterSet *set = nullptr;
    for (const StarterSet &s : std::as_const(m_sets)) {
        if (s.id == m_options.setId)
            set = &s;
    }
    QStringList created;

    if (m_options.language) {
        const QString model = m_paths.value(QStringLiteral("language"));
        const QString mmproj = m_paths.value(QStringLiteral("projector"));
        for (const runtime::ServerProfile &p : servers->profiles()) {
            for (const runtime::LlamaModel &m : runtime::llamaModels(p)) {
                if (QFileInfo(m.model) == QFileInfo(model))
                    m_llamaProfile = p.id;
            }
        }
        if (m_llamaProfile.isEmpty()) {
            const StarterFile *f = set ? set->file(QStringLiteral("language")) : nullptr;
            runtime::ServerProfile p = servers->createProfile(runtimeId(kLlama), f ? f->name : tr("Local assistant"));
            p.options = runtime::presetOptions(kLlama, tier);
            p.options.insert(QStringLiteral("model"), QDir::toNativeSeparators(model));
            if (!mmproj.isEmpty())
                p.options.insert(QStringLiteral("mmproj"), QDir::toNativeSeparators(mmproj));
            addServer(m_studio, p, true);
            m_llamaProfile = p.id;
            created << p.name;
        }
    }
    if (m_options.images) {
        const QString model = m_paths.value(QStringLiteral("checkpoint"));
        for (const runtime::ServerProfile &p : servers->profiles()) {
            if (servers->kindOf(p.id) == kSd && QFileInfo(p.options.value(QStringLiteral("model")).toString()) == QFileInfo(model))
                m_sdProfile = p.id;
        }
        if (m_sdProfile.isEmpty()) {
            const StarterFile *f = set ? set->file(QStringLiteral("checkpoint")) : nullptr;
            runtime::ServerProfile p = servers->createProfile(runtimeId(kSd), f ? f->name : tr("Local images"));
            p.options = runtime::presetOptions(kSd, tier);
            p.options.insert(QStringLiteral("model"), QDir::toNativeSeparators(model));
            for (const char *role : {"control_net", "ip_adapter", "clip_vision"}) {
                const QString path = m_paths.value(QLatin1String(role));
                if (!path.isEmpty())
                    p.options.insert(QLatin1String(role), QDir::toNativeSeparators(path));
            }
            // Character LoRAs (downloaded or trained) land in the library's LoRA folder.
            p.options.insert(QStringLiteral("lora_dir"),
                             QDir::toNativeSeparators(QDir(m_studio->models()->defaultFolder()).filePath(runtime::ModelLibrary::subfolderFor(runtime::ModelKind::Lora))));
            const QString upscaler = m_paths.value(QStringLiteral("upscaler"));
            if (!upscaler.isEmpty())
                p.options.insert(QStringLiteral("upscalers_dir"), QDir::toNativeSeparators(QFileInfo(upscaler).absolutePath()));
            addServer(m_studio, p, true);
            m_sdProfile = p.id;
            created << p.name;
        }
        // Image generation goes to the local server from now on.
        if (ai::Provider *provider = m_studio->providerForServer(m_sdProfile))
            m_studio->providers()->setDefaultImageProvider(provider->instanceId());
    }
    emit finished(true, created.isEmpty() ? tr("Everything was already set up.") : tr("Added: %1.").arg(created.join(QStringLiteral(", "))));
}

// ---- LocalSetupDialog -------------------------------------------------------------------------------

LocalSetupDialog::LocalSetupDialog(Studio *studio, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_setup(new LocalSetup(studio, studio)) // outlives the dialog while it works
{
    setWindowTitle(tr("Set up local AI"));
    resize(760, 620);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(20, 16, 20, 16);
    m_pages = new QStackedWidget(this);
    v->addWidget(m_pages, 1);

    // Plan.
    auto *plan = new QWidget(m_pages);
    auto *pv = new QVBoxLayout(plan);
    pv->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(tr("Set up local AI"), plan);
    Theme::setRole(title, QStringLiteral("title"));
    pv->addWidget(title);
    pv->addWidget(mutedLabel(tr("Lumen downloads the server programs and a set of open models, then sets them up: an assistant that "
                                "understands images (llama.cpp) and image generation with pose control, reference images and "
                                "upscaling (stable-diffusion.cpp). Everything runs on this computer; files you already have are reused."),
                             plan));
    m_hardware = mutedLabel(tr("Checking this computer…"), plan);
    pv->addWidget(m_hardware);
    auto *form = new QFormLayout;
    m_set = new QComboBox(plan);
    form->addRow(tr("Model set"), m_set);
    m_language = new QCheckBox(tr("Assistant: language and vision model"), plan);
    m_language->setChecked(true);
    m_images = new QCheckBox(tr("Image generation: SDXL model with pose ControlNet, IP-Adapter and upscaler"), plan);
    m_images->setChecked(true);
    form->addRow(QString(), m_language);
    form->addRow(QString(), m_images);
    pv->addLayout(form);
    m_plan = new QTreeWidget(plan);
    m_plan->setObjectName(QStringLiteral("setupPlan"));
    m_plan->setHeaderLabels({tr("Item"), tr("Size"), tr("Status"), tr("Licence")});
    m_plan->setRootIsDecorated(false);
    m_plan->setUniformRowHeights(true);
    pv->addWidget(m_plan, 1);
    m_summary = new QLabel(plan);
    m_summary->setWordWrap(true);
    pv->addWidget(m_summary);
    m_pages->addWidget(plan);

    // Progress.
    auto *progress = new QWidget(m_pages);
    auto *gv = new QVBoxLayout(progress);
    gv->setContentsMargins(0, 0, 0, 0);
    auto *ptitle = new QLabel(tr("Setting up…"), progress);
    Theme::setRole(ptitle, QStringLiteral("title"));
    gv->addWidget(ptitle);
    gv->addWidget(mutedLabel(tr("Downloads continue in the Queue when you close this window; Lumen finishes the setup when they are done. "
                                "Interrupted downloads resume where they stopped."),
                             progress));
    m_progress = new QTreeWidget(progress);
    m_progress->setHeaderLabels({tr("Step"), tr("Progress")});
    m_progress->setRootIsDecorated(false);
    gv->addWidget(m_progress, 1);
    m_progressStatus = mutedLabel(QString(), progress);
    gv->addWidget(m_progressStatus);
    m_pages->addWidget(progress);

    // Result.
    auto *result = new QWidget(m_pages);
    auto *rv = new QVBoxLayout(result);
    rv->setContentsMargins(0, 0, 0, 0);
    m_result = new QLabel(result);
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::RichText);
    rv->addWidget(m_result);
    auto *tryRow = new QHBoxLayout;
    m_try = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Generate a test image"), result);
    Theme::setRole(m_try, QStringLiteral("primary"));
    m_again = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Try again"), result);
    tryRow->addWidget(m_try);
    tryRow->addWidget(m_again);
    tryRow->addStretch();
    rv->addLayout(tryRow);
    m_image = new QLabel(result);
    m_image->setAlignment(Qt::AlignCenter);
    rv->addWidget(m_image, 1);
    m_pages->addWidget(result);

    auto *buttons = new QHBoxLayout;
    auto *close = new QPushButton(tr("Close"), this);
    m_start = new QPushButton(Icons::get(QStringLiteral("download")), tr("Set up"), this);
    Theme::setRole(m_start, QStringLiteral("primary"));
    m_start->setEnabled(false);
    buttons->addStretch();
    buttons->addWidget(close);
    buttons->addWidget(m_start);
    v->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &LocalSetupDialog::start);
    connect(m_set, &QComboBox::currentIndexChanged, this, &LocalSetupDialog::refreshPlan);
    connect(m_language, &QCheckBox::toggled, this, &LocalSetupDialog::refreshPlan);
    connect(m_images, &QCheckBox::toggled, this, &LocalSetupDialog::refreshPlan);
    connect(m_try, &QPushButton::clicked, this, &LocalSetupDialog::tryGeneration);
    connect(m_again, &QPushButton::clicked, this, [this] {
        m_pages->setCurrentIndex(0);
        m_start->setVisible(true);
        refreshPlan();
    });
    connect(m_setup, &LocalSetup::jobsChanged, this, &LocalSetupDialog::showProgress);
    connect(m_setup, &LocalSetup::finished, this, &LocalSetupDialog::showResult);
    // The setup object goes away once it is done and nobody shows it.
    connect(this, &QDialog::finished, m_setup, [setup = m_setup] {
        if (setup && !setup->isRunning())
            setup->deleteLater();
    });
    connect(m_setup, &LocalSetup::finished, m_setup, [setup = m_setup, self = QPointer<LocalSetupDialog>(this)] {
        if (!self || !self->isVisible())
            setup->deleteLater();
    });

    QPointer<LocalSetupDialog> self(this);
    m_setup->prepare().then(this, [self] {
        if (!self || !self->m_setup)
            return;
        const auto hw = self->m_setup->hardware();
        self->m_hardware->setText(hw ? tr("This computer: %1").arg(hw->summary()) : QString());
        const QString recommended = self->m_setup->recommendedSet();
        QSignalBlocker block(self->m_set);
        for (const StarterSet &s : self->m_setup->sets())
            self->m_set->addItem(setLabel(s.id) + (s.id == recommended ? tr(" (recommended)") : QString()), s.id);
        self->m_set->setCurrentIndex(std::max(0, self->m_set->findData(recommended)));
        block.unblock();
        self->refreshPlan();
    });
}

LocalSetup::Options LocalSetupDialog::options() const
{
    LocalSetup::Options o;
    o.setId = m_set->currentData().toString();
    o.language = m_language->isChecked();
    o.images = m_images->isChecked();
    return o;
}

void LocalSetupDialog::refreshPlan()
{
    if (!m_setup)
        return;
    const LocalSetup::Options o = options();
    m_plan->clear();
    bool blocked = !o.language && !o.images;
    auto add = [this](const QString &what, qint64 size, const QString &status, const QString &licence, bool ok) {
        auto *item = new QTreeWidgetItem(m_plan, {what, size > 0 ? runtime::formatBytes(size) : QString(), status, licence});
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setForeground(2, ok ? Theme::palette().success : Theme::palette().textMuted);
        return item;
    };
    for (const QString &kind : {QStringLiteral("llama.cpp"), QStringLiteral("sd.cpp")}) {
        if (kind == QLatin1String("llama.cpp") ? !o.language : !o.images)
            continue;
        const LocalSetup::RuntimeStep s = m_setup->runtimeStep(kind);
        const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(kind);
        const QString name = spec ? spec->name : kind;
        if (!s.existingId.isEmpty()) {
            const auto rt = m_studio->runtimes()->runtime(s.existingId);
            add(name, 0, rt && !rt->version.isEmpty() ? tr("Installed (%1)").arg(rt->version) : tr("Installed"), spec ? spec->license : QString(), true);
        } else if (s.plan.ok()) {
            add(QStringLiteral("%1 %2").arg(name, s.plan.tag), m_studio->runtimes()->pendingDownload(s.plan), tr("Download from GitHub"),
                spec ? spec->license : QString(), false);
        } else {
            QTreeWidgetItem *item = add(name, 0, s.error.isEmpty() ? tr("Looking up…") : s.error, QString(), false);
            item->setForeground(2, Theme::palette().danger);
            blocked = true;
        }
    }
    QSet<QString> licences;
    for (const LocalSetup::FileStep &f : m_setup->fileSteps(o)) {
        QTreeWidgetItem *item = add(f.file.name, f.file.size, f.localPath.isEmpty() ? tr("Download from Hugging Face") : tr("Already on this computer"),
                                    f.file.license, !f.localPath.isEmpty());
        item->setToolTip(0, f.localPath.isEmpty() ? QStringLiteral("https://huggingface.co/%1 · %2").arg(f.file.repo, f.file.path)
                                                  : QDir::toNativeSeparators(f.localPath));
        licences << f.file.license;
    }
    for (int c = 0; c < 3; ++c)
        m_plan->resizeColumnToContents(c);
    const qint64 download = m_setup->downloadSize(o);
    const QStorageInfo storage(m_studio->models()->defaultFolder());
    QString text = download > 0 ? tr("To download: <b>%1</b>").arg(runtime::formatBytes(download)) : tr("Nothing to download.");
    if (storage.isValid())
        text += QStringLiteral(" · ") + tr("free on %1: %2").arg(storage.rootPath(), runtime::formatBytes(storage.bytesAvailable()));
    if (storage.isValid() && storage.bytesAvailable() < download + (1LL << 30)) {
        text += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(Theme::palette().danger.name(), tr("Not enough disk space."));
        blocked = true;
    }
    if (!licences.isEmpty())
        text += QStringLiteral("<br>") + tr("By downloading you accept the models' licences (see the Licence column and each model's page).");
    m_summary->setText(text);
    m_start->setEnabled(!blocked && !m_setup->isRunning());
}

void LocalSetupDialog::start()
{
    if (!m_setup)
        return;
    m_start->setVisible(false);
    m_pages->setCurrentIndex(1);
    m_setup->run(options());
}

void LocalSetupDialog::showProgress()
{
    if (!m_setup)
        return;
    m_progress->clear();
    for (const QPointer<Job> &job : m_setup->jobs()) {
        if (!job)
            continue;
        auto *item = new QTreeWidgetItem(m_progress, {job->title()});
        auto *bar = new QProgressBar(m_progress);
        bar->setRange(0, 1000);
        bar->setTextVisible(true);
        m_progress->setItemWidget(item, 1, bar);
        auto update = [job, bar, item] {
            if (!job)
                return;
            bar->setValue(job->state() == Job::State::Succeeded ? 1000 : int(std::max(0.0, job->progress()) * 1000));
            const QString status = job->state() == Job::State::Failed ? job->errorString()
                                   : job->state() == Job::State::Succeeded ? tr("Done")
                                   : job->state() == Job::State::Queued ? tr("Waiting")
                                                                        : job->statusText();
            bar->setFormat(status.isEmpty() ? QStringLiteral("%p%") : status);
            item->setToolTip(0, status);
        };
        connect(job, &Job::progressChanged, bar, update);
        connect(job, &Job::stateChanged, bar, update);
        update();
    }
    m_progress->setColumnWidth(0, 300);
    m_progressStatus->setText(m_setup->jobs().isEmpty() ? tr("Setting up the servers…") : tr("%n step(s)", nullptr, int(m_setup->jobs().size())));
}

void LocalSetupDialog::showResult(bool ok, const QString &message)
{
    m_pages->setCurrentIndex(2);
    if (ok) {
        m_result->setText(QStringLiteral("<h2>%1</h2><p>%2</p><p>%3</p>")
                              .arg(tr("Local AI is ready"), message.toHtmlEscaped(),
                                   tr("The servers start when Lumen first needs them, which takes a moment the first time. "
                                      "You can change them under Settings → Local servers.")));
    } else {
        m_result->setText(QStringLiteral("<h2>%1</h2><p>%2</p>")
                              .arg(tr("The setup did not finish"), message.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    }
    m_try->setVisible(ok && m_setup && !m_setup->imageServer().isEmpty());
    m_again->setVisible(!ok);
}

void LocalSetupDialog::tryGeneration()
{
    if (!m_setup)
        return;
    ai::Provider *provider = m_studio->providerForServer(m_setup->imageServer());
    if (!provider)
        return;
    ai::GenerationSpec spec;
    spec.providerId = provider->instanceId();
    spec.request.prompt = QStringLiteral("1girl, solo, smile, upper body, looking at viewer, cherry blossoms, masterpiece, best quality");
    spec.request.negativePrompt = QStringLiteral("lowres, bad anatomy, bad hands, worst quality, low quality");
    spec.request.size = QSize(832, 1216);
    spec.request.steps = 24;
    spec.title = tr("Local AI test");
    spec.saveAssets = false;
    Job *job = m_studio->generation()->submit(spec);
    m_try->setEnabled(false);
    m_image->setText(tr("Starting the server and generating — the first image takes about a minute…"));
    QPointer<LocalSetupDialog> self(this);
    connect(job, &Job::finished, this, [self, job] {
        if (!self)
            return;
        self->m_try->setEnabled(true);
        const ai::GenerationOutput out = ai::GenerationService::outputOf(job);
        if (job->state() != Job::State::Succeeded || out.images.isEmpty()) {
            self->m_image->setText(tr("The test failed: %1").arg(job->errorString()));
            return;
        }
        const QSize box = self->m_image->size().boundedTo(QSize(520, 520));
        self->m_image->setPixmap(QPixmap::fromImage(out.images.first()).scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
}

} // namespace lumen::ui
