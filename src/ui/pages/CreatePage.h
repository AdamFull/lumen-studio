#pragma once

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
class CreatePage : public QWidget
{
    Q_OBJECT
public:
    explicit CreatePage(Studio *studio, QWidget *parent = nullptr);

private:
    void send();
    void analyze(std::function<void()> then);
    void plan(const QString &instruction);
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
    bool m_busy = false;
};

} // namespace lumen::ui
