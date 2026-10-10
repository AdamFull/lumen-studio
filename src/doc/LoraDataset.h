#pragma once

#include <QDateTime>
#include <QFuture>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSize>
#include <QStringList>

#include <functional>
#include <optional>

namespace lumen::doc {

// One training image of a character LoRA.
struct DatasetItem
{
    QString id;
    QString file;           // file name in <dataset>/images
    QJsonObject source;     // where it came from: {"asset", "session", "shot"} or {"file"}
    QJsonObject shot;       // the planned shot it was generated for (may be empty)
    bool kept = true;       // used for training
    int score = -1;         // Critic identity score 0-10; -1 = not scored
    QJsonObject critique;   // the Critic's answer
    quint64 hash = 0;       // difference hash for near-duplicate detection
    QString duplicateOf;    // id of a better image it nearly repeats
    QStringList tags;       // what the vision model saw (all tags, before pruning)
    QString caption;        // training caption: trigger word first, then the tags that vary
    bool captionEdited = false; // changed by hand: kept when captions are rebuilt
    QSize size;
    QDateTime added;
};

// Perceptual difference hash (64 bits) of an image, and the number of differing bits of two hashes.
quint64 differenceHash(const QImage &image);
int hashDistance(quint64 a, quint64 b);

// Training resolutions ("buckets") for a model family: about the family's native pixel count in
// multiples of 64 (SDXL: 1024×1024, 896×1152, 832×1216, 768×1344, …; SD 1.5: 512-class).
QList<QSize> trainingBuckets(const QString &family);
QSize bucketFor(const QSize &size, const QList<QSize> &buckets);
// Scales the image to cover the bucket and crops the rest, keeping more of the top (heads).
QImage cropToBucket(const QImage &image, const QSize &bucket);

// How captions are written for a training set.
struct DatasetSettings
{
    QString trigger;              // the word that calls the character up ("lmn_mira")
    bool outfitIsIdentity = true; // the outfit belongs to the character (its tags are pruned)
};

// The training images of one character's LoRA: <character folder>/dataset with images/ and
// dataset.json. GUI-thread object; image work runs on worker threads.
class LoraDataset : public QObject
{
    Q_OBJECT
public:
    explicit LoraDataset(const QString &directory, QObject *parent = nullptr);

    QString directory() const { return m_dir; }
    QString imagePath(const QString &id) const;
    void load();
    bool save() const;

    QList<DatasetItem> items() const { return m_items; }
    DatasetSettings settings() const { return m_settings; }
    void setSettings(const DatasetSettings &settings);
    std::optional<DatasetItem> item(const QString &id) const;
    int keptCount() const;
    // An item that came from this project asset already (to avoid adding it twice).
    bool hasAsset(const QString &assetId) const;

    QFuture<DatasetItem> add(const QImage &image, const QJsonObject &source, const QJsonObject &shot = {});
    void setKept(const QString &id, bool kept);
    void setCritique(const QString &id, int score, const QJsonObject &critique);
    // `edited`: the user wrote it (rebuilding captions keeps it).
    void setCaption(const QString &id, const QString &caption, bool edited = true);
    void setTags(const QString &id, const QStringList &tags);
    // Applies a function to every caption at once (find and replace, add or remove a tag).
    void updateCaptions(const std::function<QString(const DatasetItem &)> &change);
    bool remove(const QString &id);
    // Flags images that nearly repeat a better one (higher score, or added earlier); returns how
    // many are flagged. Flagged images are not kept unless the user keeps them again.
    int markDuplicates(int maxDistance = 6);

    // Writes the kept images, cropped to the family's buckets, as PNG with a .txt caption next to
    // each into `folder` (kohya layout: the caller names it "<repeats>_<trigger>"). Resolves with
    // the number of images written.
    QFuture<int> exportForTraining(const QString &folder, const QString &family) const;

signals:
    void changed();

private:
    DatasetItem *find(const QString &id);
    QString m_dir;
    QList<DatasetItem> m_items;
    DatasetSettings m_settings;
};

} // namespace lumen::doc
