#include "runtime/LoraTraining.h"

#include "core/Async.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "runtime/Supervisor.h"
#include "runtime/Trainer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::LoraTraining)
};

bool smallFamily(const QString &family)
{
    return family == QLatin1String("SD 1.5") || family == QLatin1String("SD 2");
}

} // namespace

TrainingPreset trainingPreset(VramTier tier)
{
    TrainingPreset p;
    const QStringList adafactor{QStringLiteral("scale_parameter=False"), QStringLiteral("relative_step=False"), QStringLiteral("warmup_init=False")};
    switch (tier) {
    case VramTier::Small:
        p.dim = 16;
        p.alpha = 8;
        p.unetOnly = true;
        p.optimizer = QStringLiteral("Adafactor");
        p.optimizerArgs = adafactor;
        p.unetLr = 1e-4;
        p.batch = 1;
        break;
    case VramTier::Medium:
        p.dim = 32;
        p.alpha = 16;
        p.unetOnly = true;
        p.optimizer = QStringLiteral("Adafactor");
        p.optimizerArgs = adafactor;
        p.unetLr = 1e-4;
        p.batch = 2;
        break;
    case VramTier::Large:
        p.dim = 32;
        p.alpha = 16;
        p.unetOnly = false;
        p.optimizer = QStringLiteral("AdamW8bit");
        p.unetLr = 1e-4;
        p.textLr = 5e-5;
        p.batch = 2;
        break;
    }
    return p;
}

int repeatsFor(int images, int stepsPerEpoch)
{
    if (images <= 0)
        return 1;
    return std::clamp(int(std::lround(double(stepsPerEpoch) / images)), 1, 40);
}

int totalSteps(int images, int repeats, int epochs, int batch)
{
    const int perEpoch = (images * repeats + std::max(1, batch) - 1) / std::max(1, batch);
    return perEpoch * epochs;
}

QStringList trainingArguments(const TrainingOptions &o, const InstalledRuntime &trainer)
{
    const QString scripts = trainer.settings.value(QStringLiteral("scripts")).toString();
    const bool small = smallFamily(o.family);
    const TrainingPreset p = trainingPreset(o.tier);
    auto native = [](const QString &path) { return QDir::toNativeSeparators(path); };
    QStringList a;
    a << native(QDir(scripts).filePath(small ? QStringLiteral("train_network.py") : QStringLiteral("sdxl_train_network.py")));
    a << QStringLiteral("--pretrained_model_name_or_path") << native(o.checkpoint);
    a << QStringLiteral("--train_data_dir") << native(o.imagesDir);
    a << QStringLiteral("--output_dir") << native(QDir(o.workDir).filePath(QStringLiteral("output")));
    a << QStringLiteral("--output_name") << o.outputName;
    a << QStringLiteral("--save_model_as") << QStringLiteral("safetensors");
    a << QStringLiteral("--save_precision") << QStringLiteral("bf16");
    // Images are already cropped to buckets; the trainer only groups them.
    a << QStringLiteral("--resolution") << (small ? QStringLiteral("512,512") : QStringLiteral("1024,1024"));
    a << QStringLiteral("--enable_bucket") << QStringLiteral("--bucket_no_upscale") << QStringLiteral("--bucket_reso_steps") << QStringLiteral("64");
    a << QStringLiteral("--min_bucket_reso") << (small ? QStringLiteral("256") : QStringLiteral("512"));
    a << QStringLiteral("--max_bucket_reso") << (small ? QStringLiteral("1024") : QStringLiteral("2048"));
    // Captions: shuffled with the trigger word kept first, unless the text encoders' outputs are
    // cached (the trainer then needs fixed captions).
    const bool cacheText = p.unetOnly && !small;
    a << QStringLiteral("--caption_extension") << QStringLiteral(".txt");
    if (!cacheText)
        a << QStringLiteral("--shuffle_caption") << QStringLiteral("--keep_tokens") << QStringLiteral("1");
    a << QStringLiteral("--network_module") << QStringLiteral("networks.lora");
    a << QStringLiteral("--network_dim") << QString::number(p.dim) << QStringLiteral("--network_alpha") << QString::number(p.alpha);
    if (p.unetOnly) {
        a << QStringLiteral("--network_train_unet_only");
        if (cacheText)
            a << QStringLiteral("--cache_text_encoder_outputs") << QStringLiteral("--cache_text_encoder_outputs_to_disk");
    } else {
        a << QStringLiteral("--text_encoder_lr") << QString::number(p.textLr);
    }
    a << QStringLiteral("--learning_rate") << QString::number(p.unetLr) << QStringLiteral("--unet_lr") << QString::number(p.unetLr);
    a << QStringLiteral("--optimizer_type") << p.optimizer;
    if (!p.optimizerArgs.isEmpty())
        a << QStringLiteral("--optimizer_args") << p.optimizerArgs;
    a << QStringLiteral("--lr_scheduler") << QStringLiteral("constant_with_warmup") << QStringLiteral("--lr_warmup_steps") << QStringLiteral("0.05");
    a << QStringLiteral("--max_train_epochs") << QString::number(o.epochs) << QStringLiteral("--save_every_n_epochs") << QStringLiteral("1");
    a << QStringLiteral("--mixed_precision") << QStringLiteral("bf16") << QStringLiteral("--full_bf16");
    if (p.fp8Base)
        a << QStringLiteral("--fp8_base");
    a << QStringLiteral("--gradient_checkpointing") << QStringLiteral("--sdpa");
    a << QStringLiteral("--cache_latents") << QStringLiteral("--cache_latents_to_disk") << QStringLiteral("--no_half_vae");
    a << QStringLiteral("--train_batch_size") << QString::number(p.batch) << QStringLiteral("--max_data_loader_n_workers") << QStringLiteral("0");
    a << QStringLiteral("--seed") << QString::number(o.seed);
    // A state per epoch (only the last is kept) lets a stopped training continue.
    a << QStringLiteral("--save_state") << QStringLiteral("--save_last_n_epochs_state") << QStringLiteral("1");
    a << QStringLiteral("--tokenizer_cache_dir") << native(QDir(trainer.directory).filePath(QStringLiteral("tokenizers")));
    if (!o.title.isEmpty())
        a << QStringLiteral("--metadata_title") << o.title;
    a << QStringLiteral("--metadata_description") << QStringLiteral("Character LoRA trained with Lumen Studio. Trigger word: %1").arg(o.trigger);
    a << QStringLiteral("--training_comment") << QStringLiteral("trigger: %1").arg(o.trigger);
    if (!o.resumeState.isEmpty())
        a << QStringLiteral("--resume") << native(o.resumeState);
    return a;
}

bool parseTrainingLine(const QString &line, TrainingProgress *p)
{
    static const QRegularExpression epochRe(QStringLiteral("^epoch (\\d+)/(\\d+)"));
    static const QRegularExpression stepsRe(QStringLiteral("steps:\\s*\\d+%\\|[^|]*\\|\\s*(\\d+)/(\\d+)\\s*\\[[\\d:]+<([\\d:?]+),\\s*([\\d.]+)(s/it|it/s)"
                                                           "(?:,\\s*avr_loss=([\\d.eE+-]+))?"));
    static const QRegularExpression savedRe(QStringLiteral("saving checkpoint:\\s*(.+\\.safetensors)"));
    const QString l = line.trimmed();
    if (const auto m = epochRe.match(l); m.hasMatch()) {
        p->epoch = m.captured(1).toInt();
        p->epochs = m.captured(2).toInt();
        p->phase = QStringLiteral("training");
        return true;
    }
    if (const auto m = stepsRe.match(l); m.hasMatch()) {
        p->phase = QStringLiteral("training");
        p->step = m.captured(1).toInt();
        p->totalSteps = m.captured(2).toInt();
        p->remaining = m.captured(3);
        const double rate = m.captured(4).toDouble();
        p->secondsPerStep = m.captured(5) == QLatin1String("s/it") ? rate : (rate > 0 ? 1.0 / rate : -1);
        if (!m.captured(6).isEmpty())
            p->loss = m.captured(6).toDouble();
        return true;
    }
    if (const auto m = savedRe.match(l); m.hasMatch()) {
        p->phase = QStringLiteral("saving");
        p->savedFile = m.captured(1).trimmed();
        return true;
    }
    if (l.contains(QLatin1String("OutOfMemoryError")) || l.contains(QLatin1String("CUDA out of memory"))) {
        p->error = Text::tr("The GPU ran out of memory. Close other programs that use the GPU, or choose a smaller preset.");
        return true;
    }
    if (l.contains(QLatin1String("caching latents"), Qt::CaseInsensitive) || l.contains(QLatin1String("caching text encoder outputs"), Qt::CaseInsensitive)) {
        if (p->phase != QLatin1String("caching")) {
            p->phase = QStringLiteral("caching");
            return true;
        }
    }
    return false;
}

QStringList trainingOutputs(const QString &workDir, const QString &outputName)
{
    const QDir out(QDir(workDir).filePath(QStringLiteral("output")));
    QStringList epochs = out.entryList({outputName + QStringLiteral("-??????.safetensors")}, QDir::Files, QDir::Name);
    QStringList files;
    for (const QString &f : epochs)
        files << QDir::cleanPath(out.filePath(f));
    const QString final = out.filePath(outputName + QStringLiteral(".safetensors"));
    if (QFileInfo::exists(final))
        files << QDir::cleanPath(final);
    return files;
}

QString latestTrainingState(const QString &workDir)
{
    const QDir out(QDir(workDir).filePath(QStringLiteral("output")));
    const QStringList states = out.entryList({QStringLiteral("*-state")}, QDir::Dirs, QDir::Name);
    return states.isEmpty() ? QString() : QDir::cleanPath(out.filePath(states.last()));
}

namespace {

// Drives one training job: reserve the GPU → wait for servers to stop → run the trainer → release.
class TrainingRun : public QObject
{
public:
    TrainingRun(RuntimeStore *store, ServerSupervisor *servers, TrainingOptions options, Job *job)
        : QObject(job)
        , m_store(store)
        , m_servers(servers)
        , m_options(std::move(options))
        , m_job(job)
    {
    }

    ~TrainingRun() override { releaseGpu(); }

    void run()
    {
        const auto trainer = m_store->runtime(m_options.runtimeId);
        if (!trainer || !isTrainerKind(trainer->kind))
            return m_job->fail(Text::tr("The LoRA trainer is not installed. Install it under Settings → Local servers → Download…"));
        if (!QFileInfo::exists(m_options.checkpoint))
            return m_job->fail(Text::tr("The base model %1 is missing.").arg(QDir::toNativeSeparators(m_options.checkpoint)));
        m_trainer = *trainer;
        QDir().mkpath(QDir(m_options.workDir).filePath(QStringLiteral("output")));
        m_log.setFileName(QDir(m_options.workDir).filePath(QStringLiteral("train.log")));
        m_log.open(QIODevice::Append | QIODevice::Text);
        m_job->setProgress(-1, Text::tr("Stopping the servers to free the GPU…"));
        if (m_servers)
            m_restart = m_servers->reserveGpu(Text::tr("A LoRA is being trained on the GPU; servers start again when it is done."));
        m_reserved = true;
        waitForServers(0);
    }

    void cancel()
    {
        m_cancelled = true;
        m_token.cancel();
    }

private:
    void waitForServers(int attempt)
    {
        bool busy = false;
        if (m_servers) {
            for (const ServerProfile &p : m_servers->profiles())
                busy |= m_servers->state(p.id) != ServerState::Stopped && m_servers->state(p.id) != ServerState::Failed;
        }
        if (m_cancelled)
            return finish();
        if (busy && attempt < 60) {
            QTimer::singleShot(500, this, [this, attempt] { waitForServers(attempt + 1); });
            return;
        }
        launch();
    }

    void launch()
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *name : {"PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV"})
            env.remove(QLatin1String(name));
        env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
        env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        env.insert(QStringLiteral("HF_HOME"), QDir::toNativeSeparators(QDir(m_trainer.directory).filePath(QStringLiteral("hf"))));
        env.insert(QStringLiteral("HF_HUB_DISABLE_TELEMETRY"), QStringLiteral("1"));
        const QStringList args = trainingArguments(m_options, m_trainer);
        writeLog(QStringLiteral("> %1 %2").arg(QDir::toNativeSeparators(m_trainer.executable), args.join(QLatin1Char(' '))));
        m_job->setProgress(0.0, Text::tr("Starting the trainer…"));
        QPointer<TrainingRun> self(this);
        const ProcessRunner runner = m_store->processRunner ? m_store->processRunner : ProcessRunner(runProcess);
        auto line = [self](const QString &l) {
            if (self)
                self->onLine(l);
        };
        runner(m_trainer.executable, args, env, m_trainer.settings.value(QStringLiteral("scripts")).toString(), line, m_token)
            .then(this, [self](const ProcessResult &r) {
                if (self)
                    self->onExit(r);
            });
    }

    void onLine(const QString &l)
    {
        writeLog(l);
        if (!parseTrainingLine(l, &m_progress))
            return;
        const TrainingProgress &p = m_progress;
        if (p.phase == QLatin1String("caching")) {
            m_job->setProgress(-1, Text::tr("Preparing the images…"));
        } else if (p.phase == QLatin1String("training") && p.totalSteps > 0 && p.step >= 0) {
            QString status = p.epochs > 0 ? Text::tr("Epoch %1 of %2 · step %3 of %4").arg(p.epoch).arg(p.epochs).arg(p.step).arg(p.totalSteps)
                                          : Text::tr("Step %1 of %2").arg(p.step).arg(p.totalSteps);
            if (p.loss >= 0)
                status += Text::tr(" · loss %1").arg(p.loss, 0, 'f', 4);
            if (!p.remaining.isEmpty() && p.remaining != QLatin1String("?"))
                status += Text::tr(" · %1 left").arg(p.remaining);
            m_job->setProgress(std::clamp(double(p.step) / p.totalSteps, 0.0, 0.99), status);
            m_job->metadata().insert(QStringLiteral("step"), p.step);
            m_job->metadata().insert(QStringLiteral("totalSteps"), p.totalSteps);
            m_job->metadata().insert(QStringLiteral("loss"), p.loss);
            m_job->metadata().insert(QStringLiteral("secondsPerStep"), p.secondsPerStep);
        } else if (p.phase == QLatin1String("saving") && !p.savedFile.isEmpty()) {
            m_job->metadata().insert(QStringLiteral("lastSaved"), p.savedFile);
        }
    }

    void onExit(const ProcessResult &r)
    {
        if (m_cancelled) {
            finish();
            return m_job->reportCancelled();
        }
        if (!r.started) {
            finish();
            return m_job->fail(Text::tr("Cannot start the trainer: %1").arg(r.output.trimmed()));
        }
        const QStringList files = trainingOutputs(m_options.workDir, m_options.outputName);
        const QString final = QDir(QDir(m_options.workDir).filePath(QStringLiteral("output"))).filePath(m_options.outputName + QStringLiteral(".safetensors"));
        if (r.exitCode != 0 || !QFileInfo::exists(final)) {
            finish();
            if (!m_progress.error.isEmpty())
                return m_job->fail(m_progress.error);
            // Python's own error line says most; the end of the output shows where.
            const QStringList lines = r.output.trimmed().split(QLatin1Char('\n'));
            static const QRegularExpression errorLine(QStringLiteral("^\\w*(Error|Exception)\\b.*:"));
            QString cause;
            for (const QString &l : lines) {
                if (errorLine.match(l.trimmed()).hasMatch())
                    cause = l.trimmed();
            }
            const QString tail = lines.mid(std::max(0, int(lines.size()) - 6)).join(QLatin1Char('\n'));
            return m_job->fail(Text::tr("The trainer stopped with exit code %1:\n%2").arg(r.exitCode).arg(cause.isEmpty() ? tail : cause.left(400)));
        }
        finish();
        m_job->succeed(QVariantMap{{QStringLiteral("files"), files},
                                   {QStringLiteral("final"), QDir::cleanPath(final)},
                                   {QStringLiteral("log"), m_log.fileName()}});
    }

    void finish()
    {
        releaseGpu();
        if (m_log.isOpen())
            m_log.close();
    }

    void releaseGpu()
    {
        if (!m_reserved)
            return;
        m_reserved = false;
        if (!m_servers)
            return;
        // Servers that run all the time come back; the others start when they are needed.
        QStringList restart;
        for (const QString &id : std::as_const(m_restart)) {
            if (const auto p = m_servers->profile(id); p && p->startPolicy == StartPolicy::AtStartup)
                restart << id;
        }
        m_servers->releaseGpu(restart);
    }

    void writeLog(const QString &l)
    {
        if (m_log.isOpen()) {
            m_log.write(l.toUtf8());
            m_log.write("\n");
            m_log.flush();
        }
    }

    RuntimeStore *m_store;
    QPointer<ServerSupervisor> m_servers;
    TrainingOptions m_options;
    Job *m_job;
    InstalledRuntime m_trainer;
    QStringList m_restart;
    bool m_reserved = false;
    bool m_cancelled = false;
    async::CancelToken m_token;
    TrainingProgress m_progress;
    QFile m_log;
};

} // namespace

Job *startTraining(RuntimeStore *store, ServerSupervisor *servers, JobQueue *jobs, const TrainingOptions &options)
{
    jobs->setConcurrency(QStringLiteral("lora-training"), 1);
    QPointer<RuntimeStore> s(store);
    QPointer<ServerSupervisor> sup(servers);
    const QString title = options.resumeState.isEmpty() ? Text::tr("Train LoRA %1").arg(options.outputName)
                                                         : Text::tr("Continue training LoRA %1").arg(options.outputName);
    return jobs->submit(title, QStringLiteral("lora-training"), [s, sup, options](Job *job) {
        if (!s)
            return job->fail(Text::tr("Shutting down."));
        auto *run = new TrainingRun(s, sup, options, job);
        job->setCancelHandler([run = QPointer<TrainingRun>(run)] {
            if (run)
                run->cancel();
        });
        run->run();
    }, QStringLiteral("training"));
}

} // namespace lumen::runtime
