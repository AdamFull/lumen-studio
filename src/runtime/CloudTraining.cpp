#include "runtime/CloudTraining.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "net/HttpClient.h"
#include "runtime/PodAgent.h"
#include "runtime/Trainer.h"
#include "runtime/Zip.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

namespace lumen::runtime {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::CloudTraining)
};

QSet<QString> &activePods()
{
    static QSet<QString> pods;
    return pods;
}

QString randomToken()
{
    QByteArray bytes(24, Qt::Uninitialized);
    for (char &b : bytes)
        b = char(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(bytes.toHex());
}

QString errorText(const std::exception &e)
{
    return QString::fromUtf8(e.what());
}

// One cloud training: pack → pod → agent → setup and uploads → train → download → terminate.
class CloudRun : public QObject
{
public:
    CloudRun(RunPodClient *runPod, net::HttpClient *http, const CloudTrainingOptions &options, Job *job)
        : QObject(job)
        , m_runPod(runPod)
        , m_http(http)
        , m_o(options)
        , m_job(job)
        , m_token(randomToken())
    {
        m_poll.setSingleShot(true);
    }

    void run()
    {
        const TrainerRecipe *recipe = TrainerCatalog::instance().trainer(QStringLiteral("kohya"));
        const TrainerVariant *variant = recipe ? recipe->variant(QStringLiteral("cu128")) : nullptr;
        m_lock = variant ? trainerLinuxLock(*variant) : QByteArray();
        const QByteArray agent = PodAgentClient::agentScript();
        if (!recipe || m_lock.isEmpty() || agent.isEmpty() || !m_tmp.isValid())
            return fail(Text::tr("The cloud trainer's files are missing from this Lumen."));
        if (m_o.checkpointUrl.isEmpty() && !QFileInfo::exists(m_o.training.checkpoint))
            return fail(Text::tr("The base model %1 is missing.").arg(QDir::toNativeSeparators(m_o.training.checkpoint)));
        m_recipe = *recipe;
        m_agentScript = agent;
        QDir().mkpath(QDir(m_o.training.workDir).filePath(QStringLiteral("output")));
        m_log.setFileName(QDir(m_o.training.workDir).filePath(QStringLiteral("train.log")));
        m_log.open(QIODevice::WriteOnly | QIODevice::Truncate);
        stage(Text::tr("Packing the training set…"));
        const QString zip = m_tmp.filePath(QStringLiteral("dataset.zip"));
        QPointer<CloudRun> self(this);
        async::run([zip, images = m_o.training.imagesDir] { return writeZip(zip, images); }).then(this, [self](const QString &error) {
            if (!self)
                return;
            if (!error.isEmpty())
                return self->fail(error);
            self->createPod();
        });
    }

    void cancel()
    {
        if (m_finished)
            return;
        m_cancelled = true;
        m_poll.stop();
        if (m_agent)
            m_agent->cancelTraining(); // best effort; terminating the pod stops it anyway
        m_cancel.cancel();
        end([job = m_job] { job->reportCancelled(); });
    }

private:
    void stage(const QString &text, double progress = -1)
    {
        m_stage = text;
        m_job->metadata().insert(QStringLiteral("stage"), text);
        m_job->setProgress(progress, text + costSuffix());
    }

    QString costSuffix() const
    {
        if (m_pod.costPerHour <= 0 || !m_running.isValid())
            return {};
        const double cost = m_pod.costPerHour * double(m_running.elapsed()) / 3.6e6;
        return Text::tr(" · about $%1 so far").arg(cost, 0, 'f', 2);
    }

    void fail(const QString &message)
    {
        if (m_finished)
            return;
        m_poll.stop();
        m_cancel.cancel();
        end([job = m_job, message] { job->fail(message); });
    }

    // Always the last step: terminate the pod (twice if needed), then report.
    void end(std::function<void()> report)
    {
        m_finished = true;
        m_log.close();
        if (m_pod.id.isEmpty())
            return report();
        const QString id = m_pod.id;
        QPointer<CloudRun> self(this);
        auto done = [self, id, report](const QString &error) {
            activePods().remove(id);
            if (self && !error.isEmpty())
                self->m_job->metadata().insert(QStringLiteral("warning"),
                                               Text::tr("The cloud GPU %1 could not be shut down (%2). Stop it at runpod.io, or Lumen "
                                                        "will offer to on its next start.")
                                                   .arg(id, error));
            report();
        };
        m_runPod->terminate(id)
            .then(this, [done] { done(QString()); })
            .onFailed(this, [self, id, done](const std::exception &) {
                // Once more after a moment (a network hiccup must not leave a paid GPU running).
                QTimer::singleShot(3000, self, [self, id, done] {
                    if (!self)
                        return done(Text::tr("Lumen is shutting down"));
                    self->m_runPod->terminate(id).then(self, [done] { done(QString()); }).onFailed(self, [done](const std::exception &e) {
                        done(errorText(e));
                    });
                });
            });
    }

    // Runs `next` after `future`, or fails the run with its error.
    template<typename T, typename F>
    void then(QFuture<T> future, F next)
    {
        QPointer<CloudRun> self(this);
        if constexpr (std::is_void_v<T>) {
            future.then(this, [self, next] {
                      if (self && !self->m_finished)
                          next();
                  })
                .onFailed(this, [self](const std::exception &e) {
                    if (self && !self->m_finished)
                        self->fail(errorText(e));
                });
        } else {
            future.then(this, [self, next](const T &value) {
                      if (self && !self->m_finished)
                          next(value);
                  })
                .onFailed(this, [self](const std::exception &e) {
                    if (self && !self->m_finished)
                        self->fail(errorText(e));
                });
        }
    }

    // Calls `check` every pollMs until it says done (true), up to `timeoutSec`.
    void poll(std::function<void(std::function<void(bool)>)> check, int timeoutSec, const QString &timeoutMessage)
    {
        QElapsedTimer started;
        started.start();
        auto attempt = std::make_shared<std::function<void()>>();
        QPointer<CloudRun> self(this);
        *attempt = [self, check, started, timeoutSec, timeoutMessage, attempt] {
            if (!self || self->m_finished) {
                *attempt = nullptr;
                return;
            }
            check([self, started, timeoutSec, timeoutMessage, attempt](bool done) {
                if (!self || self->m_finished || done) {
                    *attempt = nullptr;
                    return;
                }
                if (started.elapsed() > qint64(timeoutSec) * 1000) {
                    *attempt = nullptr;
                    return self->fail(timeoutMessage);
                }
                self->m_poll.disconnect();
                connect(&self->m_poll, &QTimer::timeout, self, [attempt] {
                    if (*attempt)
                        (*attempt)();
                });
                self->m_poll.start(self->m_o.pollMs);
            });
        };
        (*attempt)();
    }

    // A poll that failed: a few in a row are tolerated (RunPod's proxy has hiccups).
    void pollFailed(const std::function<void(bool)> &done, const std::exception &e)
    {
        if (m_finished)
            return;
        if (++m_pollFailures > 6)
            return fail(errorText(e));
        done(false);
    }

    void createPod()
    {
        stage(Text::tr("Renting the cloud GPU…"));
        RunPodPodSpec spec;
        spec.name = QLatin1String(RunPodClient::kTrainingPrefix) + m_o.training.outputName + QLatin1Char('-')
                    + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"));
        spec.image = m_o.image;
        spec.gpuId = m_o.gpuId;
        spec.minCudaVersion = QStringLiteral("12.8");
        spec.community = m_o.community;
        spec.diskGb = m_o.diskGb;
        spec.env.insert(QStringLiteral("LUMEN_TOKEN"), m_token);
        spec.env.insert(QStringLiteral("LUMEN_AGENT"), QString::fromLatin1(m_agentScript.toBase64()));
        spec.env.insert(QStringLiteral("LUMEN_WORK"), QStringLiteral("/workspace/lumen"));
        spec.ports = {QStringLiteral("8000/http")};
        spec.cmd = {QStringLiteral("bash"), QStringLiteral("-c"),
                    QStringLiteral("echo \"$LUMEN_AGENT\" | base64 -d > /root/lumen_agent.py && exec python3 /root/lumen_agent.py")};
        QPointer<CloudRun> self(this);
        RunPodClient *runPod = m_runPod;
        m_runPod->createPod(spec)
            .then(m_runPod, [self, runPod](const RunPodPod &pod) {
                // The run may have ended meanwhile (cancelled): this pod must not stay.
                if (!self || self->m_finished) {
                    runPod->terminate(pod.id);
                    return;
                }
                self->m_pod = pod;
                activePods().insert(pod.id);
                self->m_job->metadata().insert(QStringLiteral("pod"), pod.id);
                self->m_job->metadata().insert(QStringLiteral("costPerHour"), pod.costPerHour);
                self->waitForPod();
            })
            .onFailed(this, [self](const std::exception &e) {
                if (self && !self->m_finished)
                    self->fail(errorText(e));
            });
    }

    void waitForPod()
    {
        stage(Text::tr("Starting the cloud GPU…"));
        poll(
            [this](std::function<void(bool)> done) {
                m_runPod->pod(m_pod.id).then(this, [this, done](const RunPodPod &p) {
                    if (m_finished)
                        return;
                    m_pollFailures = 0;
                    if (p.costPerHour > 0)
                        m_pod.costPerHour = p.costPerHour;
                    if (p.isGone())
                        return fail(Text::tr("The cloud GPU stopped while starting (%1).").arg(p.status));
                    if (!p.isRunning())
                        return done(false);
                    m_running.start();
                    m_job->metadata().insert(QStringLiteral("costPerHour"), m_pod.costPerHour);
                    done(true);
                    connectAgent();
                }).onFailed(this, [this, done](const std::exception &e) { pollFailed(done, e); });
            },
            m_o.startTimeoutSec, Text::tr("The cloud GPU did not start in %1 minutes.").arg(m_o.startTimeoutSec / 60));
    }

    void connectAgent()
    {
        stage(Text::tr("Connecting to the cloud GPU…"));
        const QString url = m_o.agentUrl ? m_o.agentUrl(m_pod) : m_pod.proxyUrl(8000);
        m_agent = new PodAgentClient(m_http, url, m_token, this);
        poll(
            [this](std::function<void(bool)> done) {
                // Until the agent runs, the proxy answers with an error: keep asking.
                m_agent->status().then(this, [this, done](const PodAgentStatus &) {
                    if (m_finished)
                        return;
                    done(true);
                    startSetup();
                }).onFailed(this, [done](const std::exception &) { done(false); });
            },
            m_o.startTimeoutSec, Text::tr("The cloud GPU's agent did not answer in %1 minutes.").arg(m_o.startTimeoutSec / 60));
    }

    void startSetup()
    {
        stage(Text::tr("Installing the trainer on the cloud GPU…"));
        if (!m_o.testTrainer.isEmpty()) {
            then(m_agent->upload(trainerScript(), m_o.testTrainer), [this] {
                then(m_agent->setup({{QStringLiteral("skip"), true}}), [this] { uploadSet(); });
            });
            return;
        }
        const QString lockPath = m_tmp.filePath(QStringLiteral("lock.txt"));
        QFile f(lockPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(m_lock) != m_lock.size())
            return fail(Text::tr("Cannot write %1.").arg(QDir::toNativeSeparators(lockPath)));
        f.close();
        const TrainerCatalog &catalog = TrainerCatalog::instance();
        const QJsonObject recipe{
            {QStringLiteral("uv"), QJsonObject{{QStringLiteral("url"), catalog.uvLinux.url.toString()}, {QStringLiteral("sha256"), catalog.uvLinux.sha256}}},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("url"), m_recipe.source.url.toString()},
                                                   {QStringLiteral("sha256"), m_recipe.source.sha256},
                                                   {QStringLiteral("root"), m_recipe.sourceRoot}}},
            {QStringLiteral("python"), m_recipe.python},
            {QStringLiteral("lock"), QStringLiteral("lock.txt")}};
        // The pod installs while the training set and the model travel.
        then(m_agent->upload(QStringLiteral("lock.txt"), lockPath), [this, recipe] { then(m_agent->setup(recipe), [this] { uploadSet(); }); });
    }

    QString trainerScript() const
    {
        return smallModel() ? QStringLiteral("train_network.py") : QStringLiteral("sdxl_train_network.py");
    }

    bool smallModel() const { return m_o.training.family.compare(QLatin1String("SD 1.5"), Qt::CaseInsensitive) == 0; }

    void uploadSet()
    {
        stage(Text::tr("Uploading the training set…"));
        const QString zip = m_tmp.filePath(QStringLiteral("dataset.zip"));
        then(m_agent->upload(QStringLiteral("dataset.zip"), zip, transferProgress(Text::tr("Uploading the training set")), m_cancel), [this] {
            then(m_agent->unzip(QStringLiteral("dataset.zip"), QStringLiteral("img")), [this] { sendModel(); });
        });
    }

    PodAgentClient::Progress transferProgress(const QString &what)
    {
        QPointer<CloudRun> self(this);
        return [self, what](qint64 done, qint64 total) {
            if (self && total > 0)
                self->stage(QCoreApplication::translate("lumen::runtime::CloudTraining", "%1: %2 of %3 MB").arg(what).arg(done / 1048576).arg(total / 1048576));
        };
    }

    void sendModel()
    {
        if (!m_o.checkpointUrl.isEmpty()) {
            stage(Text::tr("The cloud GPU downloads the base model…"));
            then(m_agent->fetch(QStringLiteral("model.safetensors"), m_o.checkpointUrl, m_o.checkpointSha256), [this] { waitUntilReady(); });
            return;
        }
        then(m_agent->upload(QStringLiteral("model.safetensors"), m_o.training.checkpoint, transferProgress(Text::tr("Uploading the base model")),
                             m_cancel),
             [this] { waitUntilReady(); });
    }

    void waitUntilReady()
    {
        poll(
            [this](std::function<void(bool)> done) {
                m_agent->status().then(this, [this, done](const PodAgentStatus &s) {
                    if (m_finished)
                        return;
                    m_pollFailures = 0;
                    const QJsonObject model = s.fetch.value(QStringLiteral("model.safetensors")).toObject();
                    if (model.value(QStringLiteral("state")).toString() == QLatin1String("failed"))
                        return fail(Text::tr("The cloud GPU could not download the base model: %1").arg(model.value(QStringLiteral("error")).toString()));
                    if (s.setupState == QLatin1String("failed"))
                        return fail(Text::tr("Installing the trainer on the cloud GPU failed: %1").arg(s.setupMessage));
                    const bool modelReady = m_o.checkpointUrl.isEmpty() || model.value(QStringLiteral("state")).toString() == QLatin1String("done");
                    if (s.setupState != QLatin1String("ready") || !modelReady) {
                        if (!modelReady && model.value(QStringLiteral("total")).toDouble() > 0)
                            stage(Text::tr("The cloud GPU downloads the base model: %1 of %2 MB")
                                      .arg(qint64(model.value(QStringLiteral("received")).toDouble()) / 1048576)
                                      .arg(qint64(model.value(QStringLiteral("total")).toDouble()) / 1048576));
                        else
                            stage(Text::tr("Installing the trainer on the cloud GPU: %1").arg(s.setupMessage));
                        return done(false);
                    }
                    done(true);
                    train();
                }).onFailed(this, [this, done](const std::exception &e) { pollFailed(done, e); });
            },
            3600, Text::tr("The cloud GPU was not ready within an hour."));
    }

    void train()
    {
        stage(Text::tr("Starting the trainer…"), 0.0);
        TrainingOptions o = m_o.training;
        o.checkpoint = QStringLiteral("{work}/files/model.safetensors");
        o.imagesDir = QStringLiteral("{work}/img");
        o.workDir = QStringLiteral("{work}");
        o.resumeState.clear();
        InstalledRuntime pod;
        pod.directory = QStringLiteral("{work}");
        pod.settings.insert(QStringLiteral("scripts"), m_o.testTrainer.isEmpty() ? QStringLiteral("{work}/sd-scripts") : QStringLiteral("{work}/files"));
        then(m_agent->train(trainingArguments(o, pod)), [this] { followTraining(); });
    }

    void followTraining()
    {
        poll(
            [this](std::function<void(bool)> done) {
                m_agent->log(m_logOffset).then(this, [this, done](const QByteArray &chunk) {
                    if (m_finished)
                        return;
                    m_logOffset += chunk.size();
                    readLog(chunk);
                    m_agent->status().then(this, [this, done](const PodAgentStatus &s) {
                        if (m_finished)
                            return;
                        m_pollFailures = 0;
                        if (s.trainingState == QLatin1String("running") || s.logSize > m_logOffset)
                            return done(false);
                        done(true);
                        if (s.trainingState == QLatin1String("done"))
                            return downloadOutputs();
                        fail(trainerError(s.exitCode));
                    }).onFailed(this, [this, done](const std::exception &e) { pollFailed(done, e); });
                }).onFailed(this, [this, done](const std::exception &e) { pollFailed(done, e); });
            },
            24 * 3600, Text::tr("The training took longer than a day."));
    }

    void readLog(const QByteArray &chunk)
    {
        m_log.write(chunk);
        m_log.flush();
        m_partial += QString::fromUtf8(chunk);
        // tqdm rewrites its line with "\r"; every piece counts.
        static const QRegularExpression breaks(QStringLiteral("[\\r\\n]"));
        QStringList pieces = m_partial.split(breaks);
        m_partial = pieces.takeLast();
        for (const QString &line : std::as_const(pieces)) {
            if (line.trimmed().isEmpty())
                continue;
            m_tail << line;
            if (m_tail.size() > 40)
                m_tail.removeFirst();
            if (!parseTrainingLine(line, &m_progress))
                continue;
            const TrainingProgress &p = m_progress;
            if (p.phase == QLatin1String("caching")) {
                stage(Text::tr("Preparing the images…"));
            } else if (p.phase == QLatin1String("training") && p.totalSteps > 0 && p.step >= 0) {
                m_job->metadata().insert(QStringLiteral("step"), p.step);
                m_job->metadata().insert(QStringLiteral("totalSteps"), p.totalSteps);
                m_job->metadata().insert(QStringLiteral("loss"), p.loss);
                m_job->metadata().insert(QStringLiteral("secondsPerStep"), p.secondsPerStep);
                stage(describeTrainingProgress(p), std::clamp(0.95 * double(p.step) / p.totalSteps, 0.0, 0.95));
            }
        }
    }

    QString trainerError(int exitCode) const
    {
        if (!m_progress.error.isEmpty())
            return m_progress.error;
        static const QRegularExpression errorLine(QStringLiteral("^\\w*(Error|Exception)\\b.*:"));
        QString cause;
        for (const QString &l : m_tail) {
            if (errorLine.match(l.trimmed()).hasMatch())
                cause = l.trimmed();
        }
        return Text::tr("The trainer on the cloud GPU stopped with exit code %1:\n%2")
            .arg(exitCode)
            .arg(cause.isEmpty() ? m_tail.mid(std::max<qsizetype>(0, m_tail.size() - 6)).join(QLatin1Char('\n')) : cause.left(400));
    }

    void downloadOutputs()
    {
        stage(Text::tr("Downloading the LoRA files…"), 0.95);
        then(m_agent->outputs(), [this](const QList<PodOutput> &files) {
            m_outputs = files;
            if (m_outputs.isEmpty())
                return fail(Text::tr("The training on the cloud GPU wrote no LoRA files."));
            downloadNext(0);
        });
    }

    void downloadNext(int index)
    {
        if (index >= m_outputs.size())
            return finishOk();
        const PodOutput &o = m_outputs[index];
        const QString local = QDir(m_o.training.workDir).filePath(QStringLiteral("output/") + QFileInfo(o.path).fileName());
        then(m_agent->download(o, local, transferProgress(Text::tr("Downloading %1 of %2").arg(index + 1).arg(m_outputs.size())), m_cancel),
             [this, index] { downloadNext(index + 1); });
    }

    void finishOk()
    {
        const QStringList files = trainingOutputs(m_o.training.workDir, m_o.training.outputName);
        const QString final = QDir::cleanPath(QDir(m_o.training.workDir).filePath(QStringLiteral("output/") + m_o.training.outputName + QStringLiteral(".safetensors")));
        if (!QFileInfo::exists(final))
            return fail(Text::tr("The final LoRA file is missing from the cloud GPU's output."));
        const QVariantMap result{{QStringLiteral("files"), files},
                                 {QStringLiteral("final"), final},
                                 {QStringLiteral("log"), m_log.fileName()},
                                 {QStringLiteral("pod"), m_pod.id},
                                 {QStringLiteral("costPerHour"), m_pod.costPerHour}};
        stage(Text::tr("Shutting the cloud GPU down…"), 0.99);
        end([job = m_job, result] { job->succeed(result); });
    }

    RunPodClient *m_runPod;
    net::HttpClient *m_http;
    CloudTrainingOptions m_o;
    Job *m_job;
    QString m_token;
    QTemporaryDir m_tmp;
    TrainerRecipe m_recipe;
    QByteArray m_lock;
    QByteArray m_agentScript;
    RunPodPod m_pod;
    QElapsedTimer m_running;
    PodAgentClient *m_agent = nullptr;
    QTimer m_poll;
    async::CancelToken m_cancel;
    QFile m_log;
    qint64 m_logOffset = 0;
    QString m_partial;
    QStringList m_tail;
    TrainingProgress m_progress;
    QList<PodOutput> m_outputs;
    QString m_stage;
    int m_pollFailures = 0;
    bool m_cancelled = false;
    bool m_finished = false;
};

} // namespace

Job *startCloudTraining(RunPodClient *runPod, net::HttpClient *http, JobQueue *jobs, const CloudTrainingOptions &options)
{
    jobs->setConcurrency(QStringLiteral("cloud-training"), 2);
    QPointer<RunPodClient> rp(runPod);
    return jobs->submit(Text::tr("Train LoRA %1 on a cloud GPU").arg(options.training.outputName), QStringLiteral("cloud-training"),
                        [rp, http, options](Job *job) {
                            if (!rp)
                                return job->fail(Text::tr("Shutting down."));
                            auto *run = new CloudRun(rp, http, options, job);
                            job->setCancelHandler([run = QPointer<CloudRun>(run)] {
                                if (run)
                                    run->cancel();
                            });
                            run->run();
                        },
                        QStringLiteral("training"));
}

QStringList activeCloudPods()
{
    return QStringList(activePods().begin(), activePods().end());
}

QFuture<QList<RunPodPod>> leftoverCloudPods(RunPodClient *runPod)
{
    return runPod->pods(QLatin1String(RunPodClient::kTrainingPrefix)).then(runPod, [](const QList<RunPodPod> &pods) {
        QList<RunPodPod> out;
        for (const RunPodPod &p : pods) {
            if (!activePods().contains(p.id) && p.status != QLatin1String("TERMINATED"))
                out << p;
        }
        return out;
    });
}

} // namespace lumen::runtime
