#include "ui/director/DirectorSession.h"

#include "core/JsonUtil.h"
#include "doc/Project.h"

#include <QJsonArray>

namespace lumen::ui {

namespace {

QString kindName(ChatEntry::Kind k)
{
    switch (k) {
    case ChatEntry::Kind::User: return QStringLiteral("user");
    case ChatEntry::Kind::Assistant: return QStringLiteral("assistant");
    case ChatEntry::Kind::Status: return QStringLiteral("status");
    case ChatEntry::Kind::Error: return QStringLiteral("error");
    }
    return QStringLiteral("user");
}

ChatEntry::Kind kindFromName(const QString &n)
{
    if (n == QLatin1String("assistant"))
        return ChatEntry::Kind::Assistant;
    if (n == QLatin1String("status"))
        return ChatEntry::Kind::Status;
    if (n == QLatin1String("error"))
        return ChatEntry::Kind::Error;
    return ChatEntry::Kind::User;
}

} // namespace

DirectorSession::DirectorSession(QObject *parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &DirectorSession::save);
}

void DirectorSession::setTitle(const QString &t)
{
    m_title = t;
    saveSoon();
    emit changed();
}

void DirectorSession::addMessage(const ChatEntry &entry)
{
    m_messages.append(entry);
    if (m_title.isEmpty() && entry.kind == ChatEntry::Kind::User)
        m_title = entry.text.left(48);
    saveSoon();
    emit messagesChanged();
}

void DirectorSession::replaceLastStatus(const ChatEntry &entry)
{
    for (int i = int(m_messages.size()) - 1; i >= 0; --i) {
        if (m_messages[i].kind == ChatEntry::Kind::Status) {
            m_messages[i] = entry;
            saveSoon();
            emit messagesChanged();
            return;
        }
    }
    addMessage(entry);
}

void DirectorSession::addReferences(const QStringList &assetIds)
{
    bool added = false;
    for (const QString &id : assetIds) {
        if (!id.isEmpty() && !m_references.contains(id)) {
            m_references.append(id);
            added = true;
        }
    }
    if (added) {
        saveSoon();
        emit referencesChanged();
    }
}

void DirectorSession::removeReference(const QString &assetId)
{
    if (m_references.removeAll(assetId) > 0) {
        saveSoon();
        emit referencesChanged();
    }
}

void DirectorSession::setCharacter(const QJsonObject &sheet)
{
    m_character = sheet;
    saveSoon();
    emit characterChanged();
}

void DirectorSession::setPurpose(const QString &p)
{
    m_purpose = p;
    saveSoon();
}

void DirectorSession::setPlan(const QJsonObject &plan)
{
    QHash<QString, ShotState> previous;
    for (const ShotState &s : std::as_const(m_shots))
        previous.insert(s.id(), s);
    m_plan = plan;
    m_shots.clear();
    int n = 0;
    for (const QJsonValue &v : plan.value(QStringLiteral("shots")).toArray()) {
        ShotState s;
        s.shot = v.toObject();
        if (s.id().isEmpty())
            s.shot.insert(QStringLiteral("id"), QStringLiteral("shot%1").arg(++n));
        if (previous.contains(s.id()))
            s.results = previous.value(s.id()).results;
        m_shots.append(s);
    }
    if (!plan.value(QStringLiteral("title")).toString().isEmpty())
        m_title = plan.value(QStringLiteral("title")).toString();
    saveSoon();
    emit planChanged();
}

ShotState *DirectorSession::shot(const QString &id)
{
    for (ShotState &s : m_shots) {
        if (s.id() == id)
            return &s;
    }
    return nullptr;
}

void DirectorSession::notifyShotChanged(const QString &id)
{
    // Keep the plan JSON in sync with shot edits (prompts, poses).
    QJsonArray shots;
    for (const ShotState &s : std::as_const(m_shots))
        shots.append(s.shot);
    m_plan.insert(QStringLiteral("shots"), shots);
    saveSoon();
    emit shotChanged(id);
}

QJsonObject DirectorSession::toJson() const
{
    QJsonArray messages;
    for (const ChatEntry &m : m_messages) {
        if (m.kind == ChatEntry::Kind::Status)
            continue;
        messages.append(QJsonObject{{QStringLiteral("kind"), kindName(m.kind)},
                                    {QStringLiteral("text"), m.text},
                                    {QStringLiteral("assets"), json::toArray(m.assetIds)},
                                    {QStringLiteral("time"), m.time.toString(Qt::ISODate)}});
    }
    QJsonArray shots;
    for (const ShotState &s : m_shots) {
        QJsonObject o{{QStringLiteral("id"), s.id()}, {QStringLiteral("results"), json::toArray(s.results)}, {QStringLiteral("approved"), s.approved}};
        if (s.editedPose)
            o.insert(QStringLiteral("edited_pose"), s.editedPose->toOpenPoseJson());
        shots.append(o);
    }
    return QJsonObject{{QStringLiteral("title"), m_title},
                       {QStringLiteral("messages"), messages},
                       {QStringLiteral("references"), json::toArray(m_references)},
                       {QStringLiteral("character"), m_character},
                       {QStringLiteral("purpose"), m_purpose},
                       {QStringLiteral("plan"), m_plan},
                       {QStringLiteral("shot_states"), shots},
                       {QStringLiteral("created"), m_created.toString(Qt::ISODate)}};
}

void DirectorSession::fromJson(const QString &id, const QJsonObject &o)
{
    m_id = id;
    m_title = o.value(QStringLiteral("title")).toString();
    m_messages.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject m = v.toObject();
        ChatEntry e;
        e.kind = kindFromName(m.value(QStringLiteral("kind")).toString());
        e.text = m.value(QStringLiteral("text")).toString();
        e.assetIds = json::toStringList(m.value(QStringLiteral("assets")).toArray());
        e.time = QDateTime::fromString(m.value(QStringLiteral("time")).toString(), Qt::ISODate);
        m_messages.append(e);
    }
    m_references = json::toStringList(o.value(QStringLiteral("references")).toArray());
    m_character = o.value(QStringLiteral("character")).toObject();
    m_purpose = o.value(QStringLiteral("purpose")).toString(QStringLiteral("general"));
    m_created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
    m_shots.clear();
    m_plan = QJsonObject();
    setPlan(o.value(QStringLiteral("plan")).toObject());
    for (const QJsonValue &v : o.value(QStringLiteral("shot_states")).toArray()) {
        const QJsonObject st = v.toObject();
        if (ShotState *s = shot(st.value(QStringLiteral("id")).toString())) {
            s->results = json::toStringList(st.value(QStringLiteral("results")).toArray());
            s->approved = st.value(QStringLiteral("approved")).toBool(true);
            if (st.contains(QStringLiteral("edited_pose")))
                s->editedPose = pose::Pose::fromOpenPoseJson(st.value(QStringLiteral("edited_pose")).toObject());
        }
    }
    m_saveTimer.stop();
    emit messagesChanged();
    emit characterChanged();
    emit referencesChanged();
    emit planChanged();
}

void DirectorSession::bind(doc::Project *project)
{
    m_project = project;
}

void DirectorSession::saveSoon()
{
    m_saveTimer.start();
}

void DirectorSession::save()
{
    if (!m_project || !m_project->isOpen())
        return;
    // A new session gets its id from the first save; later saves wait for it to avoid duplicate records.
    if (m_saving) {
        m_savePending = true;
        return;
    }
    m_saving = true;
    doc::Record r;
    r.id = m_id;
    r.type = QStringLiteral("director");
    r.name = m_title.isEmpty() ? tr("Untitled session") : m_title;
    r.json = toJson();
    QPointer<DirectorSession> self(this);
    m_project->saveRecord(r)
        .then(this, [self](doc::Record saved) {
            if (!self)
                return;
            if (self->m_id.isEmpty())
                self->m_id = saved.id;
            self->m_saving = false;
            if (self->m_savePending) {
                self->m_savePending = false;
                self->save();
            }
        })
        .onFailed(this, [self](const std::exception &) {
            if (self)
                self->m_saving = false;
        });
}

} // namespace lumen::ui
