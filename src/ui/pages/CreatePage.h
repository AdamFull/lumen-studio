#pragma once

#include "ui/assistant/DirectorActions.h"
#include "ui/director/DirectorSession.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;

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
    // Critic: reviews new results of a shot and retries it while the best score is below the threshold.
    void critique(const QString &shotId, const QStringList &assetIds, int attempt, const QJsonObject &shotUsed);
    void saveCharacterToLibrary();
    void loadCharacterFromLibrary();
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
