#include "doc/CharacterLibrary.h"

#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <optional>

namespace lumen::doc {

namespace {

constexpr int kThumbSide = 256;

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeJson(const QString &path, const QJsonObject &json)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    return f.commit();
}

QString refName(int i)
{
    return QStringLiteral("ref_%1.png").arg(i);
}

} // namespace

CharacterLibrary::CharacterLibrary(const QString &directory, QObject *parent)
    : QObject(parent)
    , m_dir(directory)
{
    QDir().mkpath(m_dir);
}

QString CharacterLibrary::defaultDirectory()
{
    return paths::appData() + QStringLiteral("/characters");
}

QString CharacterLibrary::folder(const QString &id) const
{
    return m_dir + u'/' + id;
}

QList<CharacterEntry> CharacterLibrary::entries() const
{
    QList<CharacterEntry> out;
    for (const QString &id : QDir(m_dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (auto e = entry(id))
            out << *e;
    }
    std::sort(out.begin(), out.end(), [](const CharacterEntry &a, const CharacterEntry &b) { return a.modified > b.modified; });
    return out;
}

std::optional<CharacterEntry> CharacterLibrary::entry(const QString &id) const
{
    const QJsonObject json = readJson(folder(id) + QStringLiteral("/character.json"));
    if (json.isEmpty())
        return std::nullopt;
    CharacterEntry e;
    e.id = id;
    e.name = json.value(QStringLiteral("name")).toString();
    e.sheet = json.value(QStringLiteral("sheet")).toObject();
    e.notes = json.value(QStringLiteral("notes")).toString();
    e.created = QDateTime::fromString(json.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
    e.modified = QDateTime::fromString(json.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
    e.referenceCount = json.value(QStringLiteral("references")).toInt();
    e.thumbnail = QImage(folder(id) + QStringLiteral("/thumb.png"));
    e.lora = json.value(QStringLiteral("lora")).toObject();
    return e;
}

QFuture<CharacterEntry> CharacterLibrary::save(const QString &name, const QJsonObject &sheet, const QList<QImage> &references,
                                               const QString &notes, const QString &existingId, const std::optional<QJsonObject> &loraIn)
{
    const QString id = existingId.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128).left(16) : existingId;
    const QString dir = folder(id);
    const std::optional<CharacterEntry> previous = existingId.isEmpty() ? std::nullopt : entry(existingId);
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime created = previous ? previous->created : now;
    const QJsonObject lora = loraIn ? *loraIn : (previous ? previous->lora : QJsonObject());
    return async::run([dir, id, name, sheet, references, notes, now, created, lora] {
               QDir().mkpath(dir);
               // Replace the reference set.
               for (const QString &old : QDir(dir).entryList({QStringLiteral("ref_*.png")}, QDir::Files))
                   QFile::remove(dir + u'/' + old);
               int n = 0;
               for (const QImage &image : references) {
                   if (image.isNull())
                       continue;
                   QSaveFile f(dir + u'/' + refName(n));
                   if (!f.open(QIODevice::WriteOnly))
                       throw std::runtime_error("cannot write the character references");
                   f.write(img::encode(image, "PNG", 1));
                   if (!f.commit())
                       throw std::runtime_error("cannot write the character references");
                   ++n;
               }
               QImage thumb;
               if (!references.isEmpty() && !references.first().isNull())
                   thumb = img::thumbnail(references.first(), kThumbSide);
               if (!thumb.isNull())
                   thumb.save(dir + QStringLiteral("/thumb.png"));
               QJsonObject json{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("sheet"), sheet},
                                {QStringLiteral("notes"), notes},
                                {QStringLiteral("references"), n},
                                {QStringLiteral("created"), created.toString(Qt::ISODateWithMs)},
                                {QStringLiteral("modified"), now.toString(Qt::ISODateWithMs)}};
               if (!lora.isEmpty())
                   json.insert(QStringLiteral("lora"), lora);
               if (!writeJson(dir + QStringLiteral("/character.json"), json))
                   throw std::runtime_error("cannot write character.json");
               CharacterEntry e;
               e.id = id;
               e.name = name;
               e.sheet = sheet;
               e.notes = notes;
               e.created = created;
               e.modified = now;
               e.referenceCount = n;
               e.thumbnail = thumb;
               e.lora = lora;
               return e;
           })
        .then(this, [this](CharacterEntry e) {
            emit changed();
            return e;
        });
}

QFuture<QList<QImage>> CharacterLibrary::loadReferences(const QString &id) const
{
    const QString dir = folder(id);
    const int count = entry(id) ? entry(id)->referenceCount : 0;
    return async::run([dir, count] {
        QList<QImage> images;
        for (int i = 0; i < count; ++i) {
            QFile f(dir + u'/' + refName(i));
            if (f.open(QIODevice::ReadOnly)) {
                const QImage image = img::decode(f.readAll());
                if (!image.isNull())
                    images << img::normalized(image);
            }
        }
        return images;
    });
}

bool CharacterLibrary::rename(const QString &id, const QString &name)
{
    const QString path = folder(id) + QStringLiteral("/character.json");
    QJsonObject json = readJson(path);
    if (json.isEmpty() || name.trimmed().isEmpty())
        return false;
    json.insert(QStringLiteral("name"), name.trimmed());
    json.insert(QStringLiteral("modified"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    if (!writeJson(path, json))
        return false;
    emit changed();
    return true;
}

bool CharacterLibrary::setLora(const QString &id, const QJsonObject &lora)
{
    const QString path = folder(id) + QStringLiteral("/character.json");
    QJsonObject json = readJson(path);
    if (json.isEmpty())
        return false;
    if (lora.isEmpty())
        json.remove(QStringLiteral("lora"));
    else
        json.insert(QStringLiteral("lora"), lora);
    json.insert(QStringLiteral("modified"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    if (!writeJson(path, json))
        return false;
    emit changed();
    return true;
}

bool CharacterLibrary::remove(const QString &id)
{
    if (id.isEmpty() || id.contains(u'/') || id.contains(u'\\') || id.startsWith(u'.'))
        return false;
    QDir dir(folder(id));
    if (!dir.exists())
        return false;
    const bool ok = dir.removeRecursively();
    if (ok)
        emit changed();
    return ok;
}

} // namespace lumen::doc
