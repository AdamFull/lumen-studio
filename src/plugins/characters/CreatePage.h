#pragma once

#include "plugins/characters/assistant/DirectorActions.h"
#include "plugins/characters/director/DirectorSession.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace lumen {
class Job;
}

namespace lumen::ui {

class AutoTextEdit;
class ProviderCombo;
class ReferenceStrip;
class ShotCard;
class Studio;
class ThumbnailCache;

// AI director workspace: chat with attachments → character sheet → shot plan with editable poses →
// reference- and pose-guided generation of every shot.
class CreatePage : public QWidget, public DirectorActions
{
    Q_OBJECT
public:
    explicit CreatePage(Studio *studio, QWidget *parent = nullptr);

    // DirectorActions (the assistant drives the director through these).
    QJsonObject directorState() const override;
    void analyzeReferences(const QStringList &assetIds, const QString &notes,
                           std::function<void(QJsonObject sheet, QString error)> done) override;
    void planShots(const QString &request, const QString &purpose, int count,
                   std::function<void(QJsonObject plan, QString error)> done) override;
    void revisePose(const QString &shotId, const QString &instruction, std::function<void(QJsonObject shot, QString error)> done) override;
    void generateShots(const QStringList &shotIds, std::function<void(QStringList assets, QString error)> done) override;

private:
    void send();
    void analyze(std::function<void()> then);
    void plan(const QString &instruction, std::function<void(QString error)> done = {});
    // A shot has no generation or critic round running any more.
    void shotSettled(const QString &shotId);
    void shotFailed(const QString &shotId, const QString &error);
    // attempt > 0: a Critic retry with `shotOverride` (the shot with the critic's revised prompt).
    void generateShot(const QString &shotId, int attempt = 0, const QJsonObject &shotOverride = {});
    // A shot of the session's character and its partner (M16 §16.4): the compose-and-refine
    // pipeline (ai::startCastGeneration), one job per image; its check replaces the page's Critic.
    void generateCastShot(const QString &shotId, int attempt, const QJsonObject &shotJson);
    // A shot's job ended: its images become the shot's results; `critic` queues the Critic's review.
    void shotJobFinished(Job *job, QPointer<DirectorSession> session, const QString &shotId, int attempt, const QJsonObject &shotJson,
                         bool critic);
    // Critic: reviews new results of a shot and retries it while the best score is below the threshold.
    void critique(const QString &shotId, const QStringList &assetIds, int attempt, const QJsonObject &shotUsed);
    // `then` runs once the character is saved.
    void saveCharacterToLibrary(std::function<void()> then = {});
    void editCharacterLora();
    // The character's LoRA training set; saves the character to the library first if needed.
    void openTrainingSet();
    void planTrainingShots();
    void loadCharacterFromLibrary();
    // Add a second character…: one from the library joins the session's shots.
    void choosePartner();
    void generateAll();
    void newSession();
    void loadSessions(const QString &select = QString());
    void openSession(const QString &id);
    void bindSession(DirectorSession *session);
    void rebuildChat();
    void rebuildShots();
    void refreshCharacter();
    void setBusy(bool busy);
    void loadReferenceImages(std::function<void(QList<QImage>)> then);
    QWidget *bubble(const ChatEntry &entry);

    Studio *m_studio;
    ThumbnailCache *m_thumbs;
    QPointer<DirectorSession> m_session;
    // Chat column.
    QComboBox *m_sessions;
    QScrollArea *m_chatScroll;
    QVBoxLayout *m_chat;
    ReferenceStrip *m_attachments;
    AutoTextEdit *m_input;
    QComboBox *m_purpose;
    QSpinBox *m_count;
    QPushButton *m_send;
    // Board.
    QLabel *m_characterSummary;
    QVBoxLayout *m_refThumbs;
    QWidget *m_refRow;
    QPushButton *m_analyze;
    QToolButton *m_lora;
    QAction *m_removePartner = nullptr;
    QPointer<QDialog> m_trainingSet;
    // The session's library character as last seen: a change makes it the project's active
    // character (the first session shown at startup leaves the restored one alone).
    std::optional<QString> m_seenLibraryCharacter;
    ProviderCombo *m_provider;
    QSpinBox *m_variants;
    QCheckBox *m_critic;
    QSpinBox *m_criticRetries;
    QSpinBox *m_criticThreshold;
    QPushButton *m_generateAll;
    QVBoxLayout *m_shots;
    QLabel *m_planTitle;
    QLabel *m_emptyHint;
    QHash<QString, QPointer<ShotCard>> m_cards;
    QHash<QString, int> m_running;
    // Critic rounds wait until no generation is running (an LLM and an image model sharing one GPU
    // must not run at the same time), then start after the image providers released the GPU.
    struct PendingCritique
    {
        QString shotId;
        QStringList assetIds;
        int attempt;
        QJsonObject shotUsed;
    };
    QList<PendingCritique> m_pendingCritiques;
    int m_generating = 0;
    int m_critiquing = 0;
    QList<PendingCritique> m_pendingRetries; // shot with the critic's fix, attempt number
    void startPendingCritiques();
    void startPendingRetries();
    bool m_busy = false;
    struct ShotWaiter
    {
        QStringList pending;
        QHash<QString, int> baseline; // results per shot when the generation started
        QStringList errors;
        std::function<void(QStringList, QString)> done;
    };
    QList<std::shared_ptr<ShotWaiter>> m_waiters;
};

} // namespace lumen::ui
