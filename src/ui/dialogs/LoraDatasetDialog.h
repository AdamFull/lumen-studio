#pragma once

#include "doc/LoraDataset.h"

#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QPointer>

#include <functional>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace lumen::ai {
class Operation;
}

namespace lumen::ui {

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
    // Identity score of a critique for training images: the lower of overall and identity.
    static int datasetScore(const QJsonObject &critique);

private:
    void reload();
    void showCurrent();
    void setSelectedKept(bool kept);
    void removeSelected();
    void addFiles();
    void exportImages();
    void scoreNext();
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
    QJsonObject m_sheet;
    QList<QImage> m_references;
    bool m_referencesLoaded = false;
};

} // namespace lumen::ui
