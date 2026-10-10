#pragma once

#include <QDateTime>
#include <QFuture>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>

namespace lumen::doc {

// A character saved for reuse across projects: its character sheet and reference images.
struct CharacterEntry
{
    QString id;
    QString name;
    QJsonObject sheet;
    QString notes;
    QDateTime created;
    QDateTime modified;
    int referenceCount = 0;
    QImage thumbnail;
    QJsonObject lora;   // the character's LoRA: {file, trigger, weight, family} (ai::CharacterLora)
};

// Characters shared by all projects, one folder per character under <app data>/characters:
// character.json (name, sheet, notes, dates), ref_<n>.png reference images and thumb.png.
// GUI-thread object; image encoding and decoding run on worker threads.
class CharacterLibrary : public QObject
{
    Q_OBJECT
public:
    explicit CharacterLibrary(const QString &directory, QObject *parent = nullptr);
    static QString defaultDirectory();

    QString directory() const { return m_dir; }
    // Entries sorted by most recently modified first (reads the folder).
    QList<CharacterEntry> entries() const;
    std::optional<CharacterEntry> entry(const QString &id) const;
    // Saves a new character, or replaces `existingId`. Resolves with the stored entry. Without
    // `lora`, a replaced character keeps its LoRA.
    QFuture<CharacterEntry> save(const QString &name, const QJsonObject &sheet, const QList<QImage> &references,
                                 const QString &notes = QString(), const QString &existingId = QString(),
                                 const std::optional<QJsonObject> &lora = std::nullopt);
    QFuture<QList<QImage>> loadReferences(const QString &id) const;
    // Folder of the character's LoRA training set (doc::LoraDataset) and training runs.
    QString datasetDirectory(const QString &id) const { return folder(id) + QStringLiteral("/dataset"); }
    bool rename(const QString &id, const QString &name);
    // Attaches a LoRA to the character (an empty object detaches it).
    bool setLora(const QString &id, const QJsonObject &lora);
    bool remove(const QString &id);

signals:
    void changed();

private:
    QString folder(const QString &id) const;
    QString m_dir;
};

} // namespace lumen::doc
