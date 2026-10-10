#include "ui/CharacterTraining.h"

#include "ai/Types.h"
#include "core/Job.h"
#include "doc/CharacterLibrary.h"
#include "doc/LoraDataset.h"
#include "runtime/LoraTraining.h"
#include "runtime/ModelLibrary.h"
#include "runtime/Trainer.h"
#include "ui/Studio.h"
#include "ui/dialogs/CharacterLoraDialog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDirIterator>
#include <QSaveFile>

namespace lumen::ui {

namespace {

QHash<QString, QPointer<CharacterTraining>> &runningTrainings()
{
    static QHash<QString, QPointer<CharacterTraining>> runs;
    return runs;
}

QString uniqueFile(const QString &path)
{
    if (!QFileInfo::exists(path))
        return path;
    const QFileInfo fi(path);
    for (int i = 2;; ++i) {
        const QString candidate = fi.dir().filePath(QStringLiteral("%1-%2.%3").arg(fi.completeBaseName()).arg(i).arg(fi.suffix()));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

} // namespace

CharacterTraining *CharacterTraining::start(Studio *studio, const Settings &settings, QString *error)
{
    if (CharacterTraining *existing = running(settings.characterId)) {
        if (error)
            *error = tr("This character's LoRA is already being trained.");
        return existing;
    }
    auto *t = new CharacterTraining(studio, settings);
    if (!t->begin(error)) {
        delete t;
        return nullptr;
    }
    runningTrainings().insert(settings.characterId, t);
    return t;
}

CharacterTraining *CharacterTraining::running(const QString &characterId)
{
    return runningTrainings().value(characterId);
}

QString CharacterTraining::lastRun(const QString &datasetDir)
{
    const QDir runs(QDir(datasetDir).filePath(QStringLiteral("training")));
    const QStringList names = runs.entryList({QStringLiteral("run-*")}, QDir::Dirs, QDir::Name);
    return names.isEmpty() ? QString() : QDir::cleanPath(runs.filePath(names.last()));
}

QStringList CharacterTraining::runs(const QString &datasetDir)
{
    const QDir dir(QDir(datasetDir).filePath(QStringLiteral("training")));
    QStringList out;
    for (const QString &name : dir.entryList({QStringLiteral("run-*")}, QDir::Dirs, QDir::Name))
        out << QDir::cleanPath(dir.filePath(name));
    return out;
}

void CharacterTraining::tidyRun(const QString &runDir)
{
    if (runDir.isEmpty())
        return;
    const QDir output(QDir(runDir).filePath(QStringLiteral("output")));
    for (const QString &state : output.entryList({QStringLiteral("*-state")}, QDir::Dirs))
        QDir(output.filePath(state)).removeRecursively();
    QDir(QDir(runDir).filePath(QStringLiteral("img"))).removeRecursively();
}

qint64 CharacterTraining::removeEarlierRuns(const QString &datasetDir)
{
    QStringList all = runs(datasetDir);
    if (!all.isEmpty())
        all.removeLast();
    qint64 freed = 0;
    for (const QString &run : std::as_const(all)) {
        const qint64 size = folderSize(run);
        if (QDir(run).removeRecursively())
            freed += size;
    }
    return freed;
}

qint64 CharacterTraining::folderSize(const QString &path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

bool CharacterTraining::canResume(const QString &runDir)
{
    if (runDir.isEmpty())
        return false;
    const QJsonObject run = readJson(QDir(runDir).filePath(QStringLiteral("run.json")));
    const QString name = run.value(QStringLiteral("outputName")).toString();
    return !name.isEmpty() && !runtime::latestTrainingState(runDir).isEmpty()
           && !QFileInfo::exists(QDir(runDir).filePath(QStringLiteral("output/") + name + QStringLiteral(".safetensors")));
}

CharacterTraining::CharacterTraining(Studio *studio, const Settings &settings)
    : QObject(studio)
    , m_studio(studio)
    , m_settings(settings)
{
}

bool CharacterTraining::begin(QString *error)
{
    auto failNow = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto entry = m_studio->characters()->entry(m_settings.characterId);
    if (!entry)
        return failNow(tr("The character is not in the character library."));
    m_name = entry->name;
    const QString datasetDir = m_studio->characters()->datasetDirectory(m_settings.characterId);
    doc::LoraDataset dataset(datasetDir);
    dataset.load();
    m_images = dataset.keptCount();
    m_trigger = dataset.settings().trigger;
    if (m_images == 0)
        return failNow(tr("Keep some images in the training set first."));
    if (m_trigger.isEmpty())
        return failNow(tr("Set a trigger word in the training set's Captions tab first."));
    if (!m_studio->runtimes()->runtime(m_settings.runtimeId))
        return failNow(tr("The LoRA trainer is not installed."));
    m_family = modelFamily(m_studio, m_settings.checkpoint);
    if (m_family.isEmpty())
        m_family = QStringLiteral("SDXL");
    if (m_family != QLatin1String("SDXL") && m_family != QLatin1String("SD 1.5"))
        return failNow(tr("Training works with SDXL (Illustrious, Pony, …) and SD 1.5 models; %1 is %2.")
                           .arg(QFileInfo(m_settings.checkpoint).fileName(), m_family));

    if (m_settings.resume) {
        m_runDir = lastRun(datasetDir);
        if (!canResume(m_runDir))
            return failNow(tr("There is no stopped training to continue."));
        const QJsonObject run = readJson(QDir(m_runDir).filePath(QStringLiteral("run.json")));
        m_settings.checkpoint = run.value(QStringLiteral("checkpoint")).toString(m_settings.checkpoint);
        m_settings.epochs = run.value(QStringLiteral("epochs")).toInt(m_settings.epochs);
        m_settings.tier = runtime::VramTier(run.value(QStringLiteral("tier")).toInt(int(m_settings.tier)));
        m_trigger = run.value(QStringLiteral("trigger")).toString(m_trigger);
        m_family = run.value(QStringLiteral("family")).toString(m_family);
        m_images = run.value(QStringLiteral("images")).toInt(m_images);
        m_repeats = run.value(QStringLiteral("repeats")).toInt(1);
        launch(runtime::latestTrainingState(m_runDir));
        return true;
    }

    m_runDir = QDir(datasetDir).filePath(QStringLiteral("training/run-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    m_repeats = runtime::repeatsFor(m_images);
    QDir().mkpath(m_runDir);
    QSaveFile f(QDir(m_runDir).filePath(QStringLiteral("run.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("character"), m_settings.characterId},
                                          {QStringLiteral("checkpoint"), m_settings.checkpoint},
                                          {QStringLiteral("family"), m_family},
                                          {QStringLiteral("epochs"), m_settings.epochs},
                                          {QStringLiteral("tier"), int(m_settings.tier)},
                                          {QStringLiteral("trigger"), m_trigger},
                                          {QStringLiteral("images"), m_images},
                                          {QStringLiteral("repeats"), m_repeats},
                                          {QStringLiteral("outputName"), m_trigger},
                                          {QStringLiteral("started"), QDateTime::currentDateTime().toString(Qt::ISODate)}})
                    .toJson());
        f.commit();
    }
    // The kept images at training sizes with their captions: "<repeats>_<trigger>".
    const QString images = QDir(m_runDir).filePath(QStringLiteral("img/%1_%2").arg(m_repeats).arg(m_trigger));
    auto *exporter = new doc::LoraDataset(datasetDir, this);
    exporter->load();
    QPointer<CharacterTraining> self(this);
    exporter->exportForTraining(images, m_family)
        .then(this, [self, exporter](int n) {
            exporter->deleteLater();
            if (!self)
                return;
            if (n == 0)
                return self->fail(tr("No images to train on."));
            self->m_images = n;
            self->launch(QString());
        })
        .onFailed(this, [self](const std::exception &e) {
            if (self)
                self->fail(QString::fromLocal8Bit(e.what()));
        });
    return true;
}

void CharacterTraining::launch(const QString &resumeState)
{
    runtime::TrainingOptions o;
    o.runtimeId = m_settings.runtimeId;
    o.checkpoint = m_settings.checkpoint;
    o.family = m_family;
    o.imagesDir = QDir(m_runDir).filePath(QStringLiteral("img"));
    o.workDir = m_runDir;
    o.outputName = m_trigger;
    o.trigger = m_trigger;
    o.epochs = m_settings.epochs;
    o.tier = m_settings.tier;
    o.resumeState = resumeState;
    o.title = m_name;
    m_job = runtime::startTraining(m_studio->runtimes(), m_studio->servers(), m_studio->jobs(), o);
    connect(m_job, &Job::finished, this, &CharacterTraining::done);
    emit started(m_job);
}

void CharacterTraining::done(Job *job)
{
    if (job->state() == Job::State::Cancelled)
        return fail(tr("Training stopped. It can continue from the last finished epoch."));
    if (job->state() != Job::State::Succeeded)
        return fail(job->errorString());
    const QString trained = job->result().toMap().value(QStringLiteral("final")).toString();
    // Into the library's LoRA folder (which the setup wizard points the image server at).
    runtime::ModelLibrary *library = m_studio->models();
    const QString folder = QDir(library->defaultFolder()).filePath(runtime::ModelLibrary::subfolderFor(runtime::ModelKind::Lora));
    QDir().mkpath(folder);
    const QString target = uniqueFile(QDir(folder).filePath(m_trigger + QStringLiteral(".safetensors")));
    if (!QFile::copy(trained, target))
        return fail(tr("Cannot copy the LoRA to %1.").arg(QDir::toNativeSeparators(folder)));
    const QJsonObject sidecar{{QStringLiteral("source"), QStringLiteral("training")},
                              {QStringLiteral("name"), m_name},
                              {QStringLiteral("trainedWords"), QJsonArray{m_trigger}},
                              {QStringLiteral("baseModel"), QFileInfo(m_settings.checkpoint).fileName()},
                              {QStringLiteral("family"), m_family},
                              {QStringLiteral("epochs"), m_settings.epochs},
                              {QStringLiteral("images"), m_images},
                              {QStringLiteral("repeats"), m_repeats},
                              {QStringLiteral("run"), m_runDir},
                              {QStringLiteral("trained"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    QSaveFile f(runtime::sidecarPath(target));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(sidecar).toJson());
        f.commit();
    }
    library->addFile(target);
    ai::CharacterLora lora;
    lora.file = target;
    lora.trigger = m_trigger;
    lora.weight = 0.8;
    lora.family = m_family;
    m_studio->characters()->setLora(m_settings.characterId, lora.toJson());
    runningTrainings().remove(m_settings.characterId);
    tidyRun(m_runDir);
    const QString message = tr("The LoRA of %1 is trained and attached to the character (trigger word: %2).").arg(m_name, m_trigger);
    m_studio->notify(message, QStringLiteral("success"));
    emit finished(true, message, target);
    deleteLater();
}

void CharacterTraining::fail(const QString &message)
{
    runningTrainings().remove(m_settings.characterId);
    m_studio->notify(message, QStringLiteral("error"));
    emit finished(false, message, QString());
    deleteLater();
}

} // namespace lumen::ui
