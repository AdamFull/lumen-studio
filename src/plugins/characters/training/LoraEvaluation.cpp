#include "plugins/characters/training/LoraEvaluation.h"

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/ProviderRegistry.h"
#include "ai/StructuredChat.h"
#include "core/Job.h"
#include "doc/CharacterLibrary.h"
#include "doc/LoraDataset.h"
#include "runtime/LoraTraining.h"
#include "runtime/ModelLibrary.h"
#include "runtime/Supervisor.h"
#include "ui/Studio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

namespace lumen::ui {

namespace {

QHash<QString, QPointer<LoraEvaluation>> &runningEvaluations()
{
    static QHash<QString, QPointer<LoraEvaluation>> runs;
    return runs;
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

constexpr double kWeights[] = {0.6, 0.8, 1.0};

} // namespace

QList<QPair<int, QString>> LoraEvaluation::epochFiles(const QString &runDir)
{
    const QJsonObject run = readJson(QDir(runDir).filePath(QStringLiteral("run.json")));
    const QString name = run.value(QStringLiteral("outputName")).toString();
    const int epochs = run.value(QStringLiteral("epochs")).toInt();
    QList<QPair<int, QString>> out;
    static const QRegularExpression numbered(QStringLiteral("-(\\d{6})\\.safetensors$"));
    for (const QString &f : runtime::trainingOutputs(runDir, name)) {
        const auto m = numbered.match(f);
        if (m.hasMatch())
            out.append({m.captured(1).toInt(), f});
        else
            out.append({std::max(epochs, out.isEmpty() ? 1 : out.last().first + 1), f}); // the final file
    }
    return out;
}

QJsonObject LoraEvaluation::saved(const QString &runDir)
{
    return readJson(QDir(runDir).filePath(QStringLiteral("eval/eval.json")));
}

LoraEvaluation *LoraEvaluation::start(Studio *studio, const QString &characterId, const QString &runDir, QString *error)
{
    if (LoraEvaluation *existing = running(characterId))
        return existing;
    auto *e = new LoraEvaluation(studio, characterId, runDir);
    if (!e->begin(error)) {
        delete e;
        return nullptr;
    }
    runningEvaluations().insert(characterId, e);
    return e;
}

LoraEvaluation *LoraEvaluation::running(const QString &characterId)
{
    return runningEvaluations().value(characterId);
}

LoraEvaluation::LoraEvaluation(Studio *studio, const QString &characterId, const QString &runDir)
    : QObject(studio)
    , m_studio(studio)
    , m_characterId(characterId)
    , m_runDir(runDir)
{
}

bool LoraEvaluation::begin(QString *error)
{
    auto failNow = [error](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
    const auto entry = m_studio->characters()->entry(m_characterId);
    if (!entry)
        return failNow(tr("The character is not in the character library."));
    // The test copies go into the server's LoRA folder, which is usually in the models folder.
    if (m_studio->models()->isMoving())
        return failNow(tr("The models folder is being moved. Test the LoRA when that is done."));
    m_sheet = entry->sheet;
    m_trigger = readJson(QDir(m_runDir).filePath(QStringLiteral("run.json"))).value(QStringLiteral("trigger")).toString();
    const QList<QPair<int, QString>> all = epochFiles(m_runDir);
    if (all.isEmpty() || m_trigger.isEmpty())
        return failNow(tr("This training has no LoRA files to test yet."));
    m_epochs = all.mid(std::max<qsizetype>(0, all.size() - 3)); // the newest three epochs
    // A Lumen-run stable-diffusion.cpp server: its LoRA folder gets the test files.
    ai::ImageProvider *provider = m_studio->providers()->defaultImageProvider();
    const QString serverId = provider ? provider->text(QStringLiteral("managed_server")) : QString();
    const auto profile = serverId.isEmpty() ? std::nullopt : m_studio->servers()->profile(serverId);
    if (!provider || !profile || m_studio->servers()->kindOf(serverId) != QLatin1String("sd.cpp"))
        return failNow(tr("Testing needs a stable-diffusion.cpp server run by Lumen as the image generator (Settings → Local servers)."));
    m_providerId = provider->instanceId();
    m_loraDir = profile->option(QStringLiteral("lora_dir"), QStringLiteral("sd.cpp")).toString();
    if (m_loraDir.isEmpty()) {
        runtime::ServerProfile p = *profile;
        m_loraDir = QDir(m_studio->models()->defaultFolder()).filePath(runtime::ModelLibrary::subfolderFor(runtime::ModelKind::Lora));
        p.options.insert(QStringLiteral("lora_dir"), QDir::toNativeSeparators(m_loraDir));
        m_studio->servers()->updateProfile(p);
        if (m_studio->servers()->state(serverId) != runtime::ServerState::Stopped)
            m_studio->servers()->restart(serverId);
    }
    m_loraDir = QDir::fromNativeSeparators(m_loraDir);
    // Test copies (the server only loads LoRAs from its own folder; it rescans it per request).
    QDir().mkpath(QDir(m_loraDir).filePath(QStringLiteral("lumen-eval")));
    for (const auto &[epoch, file] : std::as_const(m_epochs)) {
        QFile::remove(evalLora(epoch));
        if (!QFile::copy(file, evalLora(epoch)))
            return failNow(tr("Cannot copy the LoRA into %1.").arg(QDir::toNativeSeparators(m_loraDir)));
    }
    m_shots = ai::Director::loraTestShots(m_sheet, m_trigger);
    QDir().mkpath(QDir(m_runDir).filePath(QStringLiteral("eval")));
    QPointer<LoraEvaluation> self(this);
    m_studio->characters()->loadReferences(m_characterId).then(this, [self](const QList<QImage> &refs) {
        if (!self)
            return;
        self->m_references = refs;
        self->m_stage = 1;
        // Stage 1: the baseline and every candidate epoch at weight 0.8.
        for (int s = 0; s < self->m_shots.size(); ++s)
            self->m_cells.append({0, 0, self->m_shots[s].value(QStringLiteral("id")).toString()});
        for (const auto &[epoch, file] : std::as_const(self->m_epochs)) {
            for (int s = 0; s < self->m_shots.size(); ++s)
                self->m_cells.append({epoch, 0.8, self->m_shots[s].value(QStringLiteral("id")).toString()});
        }
        for (int i = 0; i < self->m_cells.size(); ++i)
            self->m_pending << i;
        emit self->changed();
        self->generateNext();
    });
    return true;
}

QString LoraEvaluation::evalLora(int epoch) const
{
    return QDir(m_loraDir).filePath(QStringLiteral("lumen-eval/%1-e%2.safetensors").arg(m_trigger).arg(epoch, 2, 10, QLatin1Char('0')));
}

double LoraEvaluation::meanScore(int epoch, double weight) const
{
    double sum = 0;
    int n = 0;
    for (const Cell &c : m_cells) {
        if (c.epoch == epoch && qFuzzyCompare(c.weight + 1, weight + 1) && c.score >= 0) {
            sum += c.score;
            ++n;
        }
    }
    return n > 0 ? sum / n : -1;
}

void LoraEvaluation::generateNext()
{
    if (m_cancelled)
        return finish(false, tr("Testing stopped."));
    if (m_pending.isEmpty()) {
        critiqueNext();
        return;
    }
    const int index = m_pending.takeFirst();
    Cell &cell = m_cells[index];
    ai::ImageProvider *provider = m_studio->providers()->imageProvider(m_providerId);
    if (!provider)
        return finish(false, tr("The image generator is gone."));
    QJsonObject shot;
    int shotIndex = 0;
    for (int s = 0; s < m_shots.size(); ++s) {
        if (m_shots[s].value(QStringLiteral("id")).toString() == cell.shot) {
            shot = m_shots[s];
            shotIndex = s;
        }
    }
    const ai::ImageCapabilities caps = provider->capabilities();
    // The same prompt for all; the baseline shows the reference instead of using the LoRA (and has
    // no trigger word).
    if (cell.epoch == 0)
        shot = ai::Director::loraTestShots(m_sheet, QString()).value(shotIndex);
    const QSize size = ai::Director::sizeForAspect(shot.value(QStringLiteral("aspect_ratio")).toString(), caps.nativePixels, std::max(64, caps.sizeMultiple));
    QList<ai::ReferenceImage> refs;
    if (cell.epoch == 0) {
        for (const QImage &img : std::as_const(m_references)) {
            ai::ReferenceImage r;
            r.image = img;
            r.role = ai::ReferenceRole::Character;
            r.weight = 0.8;
            refs << r;
        }
    }
    ai::GenerationSpec spec;
    spec.providerId = m_providerId;
    spec.request = ai::Director::buildShotRequest(shot, refs, ai::Director::poseForShot(shot, size), ai::promptStyleFor(provider), caps);
    spec.request.seed = 4242 + shotIndex; // the same seed per shot for every candidate
    spec.request.batchCount = 1;
    if (cell.epoch > 0) {
        ai::CharacterLora lora;
        lora.file = evalLora(cell.epoch);
        lora.trigger = m_trigger;
        lora.weight = cell.weight;
        ai::Director::applyCharacterLora(spec.request, lora, caps);
    }
    spec.title = cell.epoch > 0 ? tr("LoRA test: epoch %1, weight %2").arg(cell.epoch).arg(cell.weight) : tr("LoRA test: without LoRA");
    spec.saveAssets = false;
    const int total = int(m_cells.size());
    m_status = tr("Generating test image %1 of %2…").arg(total - int(m_pending.size())).arg(total);
    emit changed();
    m_job = m_studio->generation()->submit(spec);
    QPointer<LoraEvaluation> self(this);
    connect(m_job, &Job::finished, this, [self, index](Job *job) {
        if (!self)
            return;
        Cell &c = self->m_cells[index];
        const ai::GenerationOutput out = ai::GenerationService::outputOf(job);
        if (job->state() == Job::State::Succeeded && !out.images.isEmpty()) {
            c.image = QDir(self->m_runDir).filePath(QStringLiteral("eval/s%1-e%2-w%3-%4.png").arg(self->m_stage).arg(c.epoch).arg(int(c.weight * 100)).arg(c.shot));
            out.images.first().save(c.image);
            self->m_toCritique << index;
        } else {
            c.error = job->state() == Job::State::Cancelled ? tr("cancelled") : job->errorString();
            if (job->state() == Job::State::Failed && c.error.contains(QLatin1String("LoRA")))
                return self->finish(false, c.error);
            // Once more: the image server can drop a job while the servers take turns on one GPU.
            if (job->state() == Job::State::Failed && self->m_retries.value(index) < 1) {
                self->m_retries[index]++;
                c.error.clear();
                self->m_pending.prepend(index);
            }
        }
        emit self->changed();
        self->generateNext();
    });
}

void LoraEvaluation::critiqueNext()
{
    if (m_cancelled)
        return finish(false, tr("Testing stopped."));
    if (m_toCritique.isEmpty())
        return stageDone();
    const int index = m_toCritique.takeFirst();
    const Cell cell = m_cells[index];
    QJsonObject shot;
    for (const QJsonObject &s : std::as_const(m_shots)) {
        if (s.value(QStringLiteral("id")).toString() == cell.shot)
            shot = s;
    }
    m_status = tr("The Critic compares the test images with the references (%1 left)…").arg(m_toCritique.size() + 1);
    emit changed();
    ai::StructuredChatOperation *op = m_studio->director()->checkIdentity(QImage(cell.image), shot, m_sheet, m_references);
    m_critique = op;
    QPointer<LoraEvaluation> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, index] {
        if (!self)
            return;
        if (op->state() == ai::Operation::State::Succeeded) {
            // Kept with the names of the features checked and the identity they add up to.
            QJsonObject critique = op->result();
            critique.insert(QStringLiteral("checklist"), QJsonArray::fromStringList(ai::Director::identityChecklist(self->m_sheet)));
            critique.insert(QStringLiteral("identity"), ai::Director::checklistIdentity(critique));
            self->m_cells[index].critique = critique;
            self->m_cells[index].score = ai::Director::loraTestScore(critique);
        } else if (op->state() == ai::Operation::State::Failed) {
            // Once more after a pause (the language server may be reloading its model); then the
            // image stays unscored and the test goes on.
            if (self->m_retries.value(-1 - index) < 1) {
                self->m_retries[-1 - index]++;
                self->m_toCritique.prepend(index);
                QTimer::singleShot(5000, self, [self] {
                    if (self)
                        self->critiqueNext();
                });
                return;
            }
            self->m_cells[index].error = tr("The Critic failed: %1").arg(op->errorString());
        }
        self->save();
        emit self->changed();
        self->critiqueNext();
    });
}

void LoraEvaluation::stageDone()
{
    if (m_stage == 1) {
        // The best epoch at 0.8; on a tie the earlier one (less likely to be overtrained).
        double best = -1;
        for (const auto &[epoch, file] : std::as_const(m_epochs)) {
            const double s = meanScore(epoch, 0.8);
            if (s > best + 0.05) {
                best = s;
                m_bestEpoch = epoch;
            }
        }
        if (m_bestEpoch == 0)
            return finish(false, tr("No test image could be scored."));
        // Stage 2: the best epoch at the other weights.
        m_stage = 2;
        for (double w : {0.6, 1.0}) {
            for (const QJsonObject &s : std::as_const(m_shots)) {
                m_cells.append({m_bestEpoch, w, s.value(QStringLiteral("id")).toString()});
                m_pending << int(m_cells.size()) - 1;
            }
        }
        emit changed();
        generateNext();
        return;
    }
    // The best weight; on a tie the lower one (more room for prompts and other LoRAs).
    double best = -1;
    for (double w : kWeights) {
        const double s = meanScore(m_bestEpoch, w);
        if (s > best + 0.05) {
            best = s;
            m_bestWeight = w;
        }
    }
    const double baseline = meanScore(0, 0);
    finish(true, baseline >= 0 ? tr("Recommended: epoch %1 at weight %2 (score %3; without the LoRA %4).")
                                     .arg(m_bestEpoch)
                                     .arg(m_bestWeight)
                                     .arg(best, 0, 'f', 1)
                                     .arg(baseline, 0, 'f', 1)
                               : tr("Recommended: epoch %1 at weight %2 (score %3).").arg(m_bestEpoch).arg(m_bestWeight).arg(best, 0, 'f', 1));
}

void LoraEvaluation::finish(bool ok, const QString &message)
{
    if (m_finished)
        return;
    m_finished = true;
    m_complete = ok;
    m_status = message;
    save();
    QDir(QDir(m_loraDir).filePath(QStringLiteral("lumen-eval"))).removeRecursively();
    runningEvaluations().remove(m_characterId);
    emit changed();
    emit finished(ok, message);
    deleteLater();
}

void LoraEvaluation::cancel()
{
    m_cancelled = true;
    if (m_job)
        m_job->cancel();
    if (m_critique)
        m_critique->cancel();
}

void LoraEvaluation::save() const
{
    QJsonArray cells;
    for (const Cell &c : m_cells) {
        cells.append(QJsonObject{{QStringLiteral("epoch"), c.epoch},
                                 {QStringLiteral("weight"), c.weight},
                                 {QStringLiteral("shot"), c.shot},
                                 {QStringLiteral("image"), c.image},
                                 {QStringLiteral("score"), c.score},
                                 {QStringLiteral("critique"), c.critique},
                                 {QStringLiteral("error"), c.error}});
    }
    QJsonArray shots;
    for (const QJsonObject &s : m_shots)
        shots.append(QJsonObject{{QStringLiteral("id"), s.value(QStringLiteral("id"))}, {QStringLiteral("title"), s.value(QStringLiteral("title"))}});
    QSaveFile f(QDir(m_runDir).filePath(QStringLiteral("eval/eval.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("cells"), cells},
                                          {QStringLiteral("shots"), shots},
                                          {QStringLiteral("bestEpoch"), m_bestEpoch},
                                          {QStringLiteral("bestWeight"), m_bestWeight},
                                          {QStringLiteral("finished"), m_finished && m_complete},
                                          {QStringLiteral("status"), m_status},
                                          {QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate)}})
                    .toJson());
        f.commit();
    }
}

QString LoraEvaluation::apply(Studio *studio, const QString &characterId, const QString &runDir, int epoch, double weight, QString *error)
{
    QString file;
    for (const auto &[e, f] : epochFiles(runDir)) {
        if (e == epoch)
            file = f;
    }
    const QJsonObject run = readJson(QDir(runDir).filePath(QStringLiteral("run.json")));
    const QString trigger = run.value(QStringLiteral("trigger")).toString();
    if (file.isEmpty() || trigger.isEmpty()) {
        if (error)
            *error = tr("Epoch %1 of this training is missing.").arg(epoch);
        return {};
    }
    runtime::ModelLibrary *library = studio->models();
    if (library->isMoving()) {
        if (error)
            *error = tr("The models folder is being moved. Try again when that is done.");
        return {};
    }
    const QString folder = QDir(library->defaultFolder()).filePath(runtime::ModelLibrary::subfolderFor(runtime::ModelKind::Lora));
    QDir().mkpath(folder);
    const QString target = QDir(folder).filePath(QStringLiteral("%1-e%2.safetensors").arg(trigger).arg(epoch, 2, 10, QLatin1Char('0')));
    QFile::remove(target);
    if (!QFile::copy(file, target)) {
        if (error)
            *error = tr("Cannot copy the LoRA to %1.").arg(QDir::toNativeSeparators(folder));
        return {};
    }
    QJsonObject sidecar{{QStringLiteral("source"), QStringLiteral("training")},
                        {QStringLiteral("trainedWords"), QJsonArray{trigger}},
                        {QStringLiteral("baseModel"), QFileInfo(run.value(QStringLiteral("checkpoint")).toString()).fileName()},
                        {QStringLiteral("family"), run.value(QStringLiteral("family"))},
                        {QStringLiteral("epoch"), epoch},
                        {QStringLiteral("weight"), weight},
                        {QStringLiteral("run"), runDir}};
    QSaveFile f(runtime::sidecarPath(target));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(sidecar).toJson());
        f.commit();
    }
    library->addFile(target);
    ai::CharacterLora lora;
    lora.file = target;
    lora.trigger = trigger;
    lora.weight = weight;
    lora.family = run.value(QStringLiteral("family")).toString();
    studio->characters()->setLora(characterId, lora.toJson());
    return target;
}

} // namespace lumen::ui
