#pragma once

#include "runtime/ServerProfile.h"

#include <QObject>
#include <QPointer>

namespace lumen {
class Job;
}

namespace lumen::ui {

class Studio;

// Trains a character's LoRA from its training set (M13 §13.6): exports the kept images with their
// captions, runs the trainer as a Queue job, then copies the LoRA into the model library's LoRA
// folder and attaches it to the character. Lives under the Studio, so a training that runs when
// its window closes still finishes.
class CharacterTraining : public QObject
{
    Q_OBJECT
public:
    struct Settings
    {
        QString characterId;
        QString runtimeId;          // installed trainer
        QString checkpoint;         // base model
        runtime::VramTier tier = runtime::VramTier::Small;
        int epochs = 10;
        bool resume = false;        // continue the last stopped run
    };

    // Starts a training; nullptr (and *error) when it cannot start.
    static CharacterTraining *start(Studio *studio, const Settings &settings, QString *error = nullptr);
    // The training running for a character, if any.
    static CharacterTraining *running(const QString &characterId);
    // The newest run folder of a training set and whether it stopped before the end (resumable).
    static QString lastRun(const QString &datasetDir);
    static bool canResume(const QString &runDir);
    // Run folders of a training set, oldest first.
    static QStringList runs(const QString &datasetDir);
    // After a finished training: removes what only a continued training needs (the trainer's
    // states and the exported images). Keeps the epoch files, the test results, run.json and the log.
    static void tidyRun(const QString &runDir);
    // Removes every run but the newest; returns the bytes freed. The LoRAs the character uses are
    // copies in the model library, so they stay.
    static qint64 removeEarlierRuns(const QString &datasetDir);
    static qint64 folderSize(const QString &path);

    QString characterId() const { return m_settings.characterId; }
    QString runDir() const { return m_runDir; }
    Job *job() const { return m_job; }

signals:
    void started(lumen::Job *job);
    // ok: the LoRA is in the library and on the character (`lora` is its file).
    void finished(bool ok, const QString &message, const QString &lora);

private:
    CharacterTraining(Studio *studio, const Settings &settings);
    bool begin(QString *error);
    void launch(const QString &resumeState);
    void done(Job *job);
    void fail(const QString &message);

    Studio *m_studio;
    Settings m_settings;
    QString m_runDir;
    QString m_trigger;
    QString m_family;
    QString m_name;
    int m_images = 0;
    int m_repeats = 1;
    QPointer<Job> m_job;
};

} // namespace lumen::ui
