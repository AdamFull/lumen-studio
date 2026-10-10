#pragma once

#include "doc/LoraDataset.h"

#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QPointer>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;

namespace lumen {
class Job;
}

namespace lumen::ai {
class Operation;
}

namespace lumen::ui {

class CharacterTraining;
class DirectorSession;
class Studio;

// The training set of a character's LoRA (M13 §13.3): images from the Director's "LoRA training
// set" shots or from files, scored by the Critic for identity, with near-duplicates flagged; the
// user keeps or rejects each one. Exports cropped, bucketed images for training.
class LoraDatasetDialog : public QDialog
{
    Q_OBJECT
public:
    // `planShots` asks the Create page to plan the training shots (may be empty).
    LoraDatasetDialog(Studio *studio, const QString &characterId, DirectorSession *session, std::function<void()> planShots,
                      QWidget *parent = nullptr);
    ~LoraDatasetDialog() override;

    doc::LoraDataset *dataset() const { return m_dataset; }
    QString characterId() const { return m_characterId; }

    // Imports the session's shot results that are not in the set yet; resolves with their count.
    void addSessionResults(std::function<void(int)> done = {});
    // Scores unscored images with the Critic one after another; images below the minimum are rejected.
    void scoreAll();
    bool isScoring() const { return !m_scoreQueue.isEmpty() || m_scoring; }
    // Tags the kept images that have no tags yet with the vision model and writes their captions.
    void captionAll();
    bool isCaptioning() const { return !m_captionQueue.isEmpty() || m_captioning; }
    // Captions from the stored tags with the current trigger word and outfit setting (hand-edited
    // captions only get the new trigger word).
    void rebuildCaptions(const QString &previousTrigger = QString());
    // Identity score of a critique for training images: the lower of overall and identity.
    static int datasetScore(const QJsonObject &critique);
    // Starts (or continues) training this character's LoRA with the Train tab's settings.
    void startTraining(bool resume = false);

private:
    void reload();
    void showCurrent();
    void setSelectedKept(bool kept);
    void removeSelected();
    void addFiles();
    void exportImages();
    void scoreNext();
    void captionNext();
    void reloadCaptions();
    void applySettings();
    void askReplace();
    void askAddTag();
    void askRemoveTag();
    QWidget *buildTrainTab();
    void refreshTrainTab();
    void bindTraining(CharacterTraining *training);
    void showTrainingJob();
    QStringList selectedIds() const;
    QPixmap thumbnail(const QString &id);

    Studio *m_studio;
    QString m_characterId;
    QPointer<DirectorSession> m_session;
    std::function<void()> m_planShots;
    doc::LoraDataset *m_dataset;
    QListWidget *m_grid;
    QLabel *m_details;
    QLabel *m_status;
    QPushButton *m_score;
    QSpinBox *m_minimum;
    QHash<QString, QPixmap> m_thumbs;
    QStringList m_loading;
    QStringList m_scoreQueue;
    bool m_scoring = false;
    QPointer<ai::Operation> m_critique;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_captions = nullptr;
    QLineEdit *m_trigger = nullptr;
    QCheckBox *m_outfit = nullptr;
    QPushButton *m_captionButton = nullptr;
    QLabel *m_tagSummary = nullptr;
    QStringList m_captionQueue;
    bool m_captioning = false;
    QPointer<ai::Operation> m_tagging;
    bool m_fillingCaptions = false;
    QLabel *m_trainer = nullptr;
    QPushButton *m_installTrainer = nullptr;
    QComboBox *m_checkpoint = nullptr;
    QComboBox *m_preset = nullptr;
    QSpinBox *m_epochs = nullptr;
    QLabel *m_trainSummary = nullptr;
    QPushButton *m_trainButton = nullptr;
    QPushButton *m_resumeButton = nullptr;
    QProgressBar *m_trainBar = nullptr;
    QLabel *m_trainStatus = nullptr;
    QPointer<Job> m_trainingJob;
    QJsonObject m_sheet;
    QList<QImage> m_references;
    bool m_referencesLoaded = false;
};

} // namespace lumen::ui
