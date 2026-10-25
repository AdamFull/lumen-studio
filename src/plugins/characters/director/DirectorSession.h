#pragma once

#include "pose/Rig.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <optional>

namespace lumen::doc {
class Project;
}

namespace lumen::ui {

struct ChatEntry
{
    enum class Kind { User, Assistant, Status, Error };
    Kind kind = Kind::User;
    QString text;
    QStringList assetIds; // attachments (user) or results (assistant)
    QDateTime time = QDateTime::currentDateTime();
};

struct ShotState
{
    QJsonObject shot;                    // planned shot (id, title, description, aspect_ratio, background, pose, prompt, negative_prompt)
    std::optional<pose::Pose> editedPose; // user-edited keypoints (overrides the rig pose)
    QStringList results;                  // generated asset ids
    QJsonObject critiques;                // asset id → critic verdict (Director::critiqueSchema())
    bool approved = true;

    QString id() const { return shot.value(QStringLiteral("id")).toString(); }
    pose::RigParams rig() const { return pose::RigParams::fromJson(shot.value(QStringLiteral("pose")).toObject()); }
    QString aspect() const { return shot.value(QStringLiteral("aspect_ratio")).toString(QStringLiteral("2:3")); }
};

// A director conversation: messages, references, character sheet, purpose and the shot plan.
// Persisted as a project record of type "director".
class DirectorSession : public QObject
{
    Q_OBJECT
public:
    explicit DirectorSession(QObject *parent = nullptr);

    QString id() const { return m_id; }
    QString title() const { return m_title; }
    void setTitle(const QString &t);

    const QList<ChatEntry> &messages() const { return m_messages; }
    void addMessage(const ChatEntry &entry);
    void replaceLastStatus(const ChatEntry &entry);

    const QStringList &references() const { return m_references; }
    void addReferences(const QStringList &assetIds);
    void removeReference(const QString &assetId);

    const QJsonObject &character() const { return m_character; }
    void setCharacter(const QJsonObject &sheet);
    // The character's LoRA ({file, trigger, weight, family}); empty when it has none.
    const QJsonObject &characterLora() const { return m_lora; }
    void setCharacterLora(const QJsonObject &lora);
    // The character library entry the session's character was loaded from or saved to.
    QString libraryCharacter() const { return m_libraryCharacter; }
    void setLibraryCharacter(const QString &id);

    QString purpose() const { return m_purpose; }
    void setPurpose(const QString &p);

    const QJsonObject &plan() const { return m_plan; }
    void setPlan(const QJsonObject &plan); // rebuilds shots, keeps results of shots with the same id
    QList<ShotState> &shots() { return m_shots; }
    const QList<ShotState> &shots() const { return m_shots; }
    ShotState *shot(const QString &id);
    void notifyShotChanged(const QString &id);

    QJsonObject toJson() const;
    void fromJson(const QString &id, const QJsonObject &json);

    void bind(doc::Project *project);
    void saveSoon();

signals:
    void changed();
    void messagesChanged();
    void planChanged();
    void shotChanged(const QString &id);
    void characterChanged();
    void referencesChanged();

private:
    void save();
    QString m_id;
    QString m_title;
    QList<ChatEntry> m_messages;
    QStringList m_references;
    QJsonObject m_character;
    QJsonObject m_lora;
    QString m_libraryCharacter;
    QString m_purpose = QStringLiteral("general");
    QJsonObject m_plan;
    QList<ShotState> m_shots;
    QPointer<doc::Project> m_project;
    QTimer m_saveTimer;
    bool m_saving = false;
    bool m_savePending = false;
    QDateTime m_created = QDateTime::currentDateTime();
};

} // namespace lumen::ui
