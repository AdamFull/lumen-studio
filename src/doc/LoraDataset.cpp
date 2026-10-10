#include "doc/LoraDataset.h"

#include "core/Async.h"
#include "core/ImageOps.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>

namespace lumen::doc {

namespace {

QJsonObject toJson(const DatasetItem &i)
{
    QJsonObject o{{QStringLiteral("id"), i.id},
                  {QStringLiteral("file"), i.file},
                  {QStringLiteral("kept"), i.kept},
                  {QStringLiteral("score"), i.score},
                  {QStringLiteral("hash"), QString::number(i.hash, 16)},
                  {QStringLiteral("width"), i.size.width()},
                  {QStringLiteral("height"), i.size.height()},
                  {QStringLiteral("added"), i.added.toString(Qt::ISODate)}};
    if (!i.source.isEmpty())
        o.insert(QStringLiteral("source"), i.source);
    if (!i.shot.isEmpty())
        o.insert(QStringLiteral("shot"), i.shot);
    if (!i.critique.isEmpty())
        o.insert(QStringLiteral("critique"), i.critique);
    if (!i.duplicateOf.isEmpty())
        o.insert(QStringLiteral("duplicate_of"), i.duplicateOf);
    if (!i.caption.isEmpty())
        o.insert(QStringLiteral("caption"), i.caption);
    return o;
}

DatasetItem fromJson(const QJsonObject &o)
{
    DatasetItem i;
    i.id = o.value(QStringLiteral("id")).toString();
    i.file = o.value(QStringLiteral("file")).toString();
    i.source = o.value(QStringLiteral("source")).toObject();
    i.shot = o.value(QStringLiteral("shot")).toObject();
    i.kept = o.value(QStringLiteral("kept")).toBool(true);
    i.score = o.value(QStringLiteral("score")).toInt(-1);
    i.critique = o.value(QStringLiteral("critique")).toObject();
    i.hash = o.value(QStringLiteral("hash")).toString().toULongLong(nullptr, 16);
    i.duplicateOf = o.value(QStringLiteral("duplicate_of")).toString();
    i.caption = o.value(QStringLiteral("caption")).toString();
    i.size = QSize(o.value(QStringLiteral("width")).toInt(), o.value(QStringLiteral("height")).toInt());
    i.added = QDateTime::fromString(o.value(QStringLiteral("added")).toString(), Qt::ISODate);
    return i;
}

} // namespace

quint64 differenceHash(const QImage &image)
{
    if (image.isNull())
        return 0;
    // 9×8 grey thumbnail; each bit says whether a pixel is brighter than its right neighbour.
    const QImage small = img::flatten(image, Qt::white).scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    quint64 hash = 0;
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        const uchar *row = small.constScanLine(y);
        for (int x = 0; x < 8; ++x, ++bit) {
            if (row[x] > row[x + 1])
                hash |= quint64(1) << bit;
        }
    }
    return hash;
}

int hashDistance(quint64 a, quint64 b)
{
    return std::popcount(a ^ b);
}

QList<QSize> trainingBuckets(const QString &family)
{
    // The usual buckets: SDXL's from its paper (about one megapixel), SD 1.5's about 512².
    if (family == QLatin1String("SD 1.5") || family == QLatin1String("SD 2"))
        return {{512, 512}, {448, 576}, {576, 448}, {384, 640}, {640, 384}, {384, 704}, {704, 384}, {320, 768}, {768, 320}};
    return {{1024, 1024}, {960, 1088}, {1088, 960}, {896, 1152}, {1152, 896}, {832, 1216}, {1216, 832},
            {768, 1344}, {1344, 768}, {640, 1536}, {1536, 640}};
}

QSize bucketFor(const QSize &size, const QList<QSize> &buckets)
{
    if (buckets.isEmpty() || size.isEmpty())
        return size;
    const double aspect = double(size.width()) / size.height();
    QSize best = buckets.first();
    double bestError = 1e9;
    for (const QSize &b : buckets) {
        const double error = std::abs(std::log(double(b.width()) / b.height() / aspect));
        if (error < bestError) {
            bestError = error;
            best = b;
        }
    }
    return best;
}

QImage cropToBucket(const QImage &image, const QSize &bucket)
{
    if (image.isNull() || bucket.isEmpty())
        return image;
    const QImage scaled = image.scaled(bucket, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = (scaled.width() - bucket.width()) / 2;
    const int y = (scaled.height() - bucket.height()) / 4; // keep more of the top: heads matter most
    return scaled.copy(x, y, bucket.width(), bucket.height());
}

LoraDataset::LoraDataset(const QString &directory, QObject *parent)
    : QObject(parent)
    , m_dir(directory)
{
}

QString LoraDataset::imagePath(const QString &id) const
{
    for (const DatasetItem &i : m_items) {
        if (i.id == id)
            return m_dir + QStringLiteral("/images/") + i.file;
    }
    return {};
}

void LoraDataset::load()
{
    m_items.clear();
    QFile f(m_dir + QStringLiteral("/dataset.json"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("items")).toArray()) {
            DatasetItem i = fromJson(v.toObject());
            if (!i.id.isEmpty() && QFile::exists(m_dir + QStringLiteral("/images/") + i.file))
                m_items << i;
        }
    }
    emit changed();
}

bool LoraDataset::save() const
{
    QDir().mkpath(m_dir);
    QJsonArray items;
    for (const DatasetItem &i : m_items)
        items.append(toJson(i));
    QSaveFile f(m_dir + QStringLiteral("/dataset.json"));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("items"), items}}).toJson());
    return f.commit();
}

std::optional<DatasetItem> LoraDataset::item(const QString &id) const
{
    for (const DatasetItem &i : m_items) {
        if (i.id == id)
            return i;
    }
    return std::nullopt;
}

DatasetItem *LoraDataset::find(const QString &id)
{
    for (DatasetItem &i : m_items) {
        if (i.id == id)
            return &i;
    }
    return nullptr;
}

int LoraDataset::keptCount() const
{
    return int(std::count_if(m_items.begin(), m_items.end(), [](const DatasetItem &i) { return i.kept; }));
}

bool LoraDataset::hasAsset(const QString &assetId) const
{
    return !assetId.isEmpty() && std::any_of(m_items.begin(), m_items.end(), [&](const DatasetItem &i) {
        return i.source.value(QStringLiteral("asset")).toString() == assetId;
    });
}

QFuture<DatasetItem> LoraDataset::add(const QImage &image, const QJsonObject &source, const QJsonObject &shot)
{
    const QString dir = m_dir + QStringLiteral("/images");
    const QString id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    QPointer<LoraDataset> self(this);
    return async::run([dir, id, image, source, shot] {
               if (image.isNull())
                   throw std::runtime_error("empty image");
               QDir().mkpath(dir);
               DatasetItem i;
               i.id = id;
               i.file = id + QStringLiteral(".png");
               i.source = source;
               i.shot = shot;
               i.size = image.size();
               i.hash = differenceHash(image);
               i.added = QDateTime::currentDateTime();
               QSaveFile f(dir + QLatin1Char('/') + i.file);
               if (!f.open(QIODevice::WriteOnly) || f.write(img::encode(image, "PNG", 1)) < 0 || !f.commit())
                   throw std::runtime_error("cannot write the dataset image");
               return i;
           })
        .then(this, [self](DatasetItem i) {
            if (self) {
                self->m_items << i;
                self->save();
                emit self->changed();
            }
            return i;
        });
}

void LoraDataset::setKept(const QString &id, bool kept)
{
    if (DatasetItem *i = find(id); i && i->kept != kept) {
        i->kept = kept;
        save();
        emit changed();
    }
}

void LoraDataset::setCritique(const QString &id, int score, const QJsonObject &critique)
{
    if (DatasetItem *i = find(id)) {
        i->score = score;
        i->critique = critique;
        save();
        emit changed();
    }
}

void LoraDataset::setCaption(const QString &id, const QString &caption)
{
    if (DatasetItem *i = find(id); i && i->caption != caption) {
        i->caption = caption;
        save();
        emit changed();
    }
}

bool LoraDataset::remove(const QString &id)
{
    for (int k = 0; k < m_items.size(); ++k) {
        if (m_items[k].id != id)
            continue;
        QFile::remove(m_dir + QStringLiteral("/images/") + m_items[k].file);
        m_items.removeAt(k);
        for (DatasetItem &other : m_items) {
            if (other.duplicateOf == id)
                other.duplicateOf.clear();
        }
        save();
        emit changed();
        return true;
    }
    return false;
}

int LoraDataset::markDuplicates(int maxDistance)
{
    // Better first: higher score, then earlier.
    QList<int> order(m_items.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) { return m_items[a].score > m_items[b].score; });
    int flagged = 0;
    for (DatasetItem &i : m_items)
        i.duplicateOf.clear();
    for (int x = 0; x < order.size(); ++x) {
        DatasetItem &candidate = m_items[order[x]];
        for (int y = 0; y < x; ++y) {
            const DatasetItem &better = m_items[order[y]];
            if (!better.duplicateOf.isEmpty())
                continue;
            if (hashDistance(candidate.hash, better.hash) <= maxDistance) {
                candidate.duplicateOf = better.id;
                candidate.kept = false;
                ++flagged;
                break;
            }
        }
    }
    save();
    emit changed();
    return flagged;
}

QFuture<int> LoraDataset::exportForTraining(const QString &folder, const QString &family) const
{
    QList<QPair<QString, QString>> work; // image path, caption
    for (const DatasetItem &i : m_items) {
        if (i.kept)
            work.append({m_dir + QStringLiteral("/images/") + i.file, i.caption});
    }
    const QList<QSize> buckets = trainingBuckets(family);
    return async::run([work, folder, buckets] {
        QDir().mkpath(folder);
        for (const QString &old : QDir(folder).entryList({QStringLiteral("*.png"), QStringLiteral("*.txt")}, QDir::Files))
            QFile::remove(folder + QLatin1Char('/') + old);
        int n = 0;
        for (const auto &[path, caption] : work) {
            const QImage image(path);
            if (image.isNull())
                continue;
            const QImage out = cropToBucket(img::flatten(image, Qt::white), bucketFor(image.size(), buckets));
            const QString base = folder + QStringLiteral("/%1").arg(n + 1, 3, 10, QLatin1Char('0'));
            if (!out.save(base + QStringLiteral(".png")))
                throw std::runtime_error("cannot write the training images");
            QFile txt(base + QStringLiteral(".txt"));
            if (txt.open(QIODevice::WriteOnly))
                txt.write(caption.toUtf8());
            ++n;
        }
        return n;
    });
}

} // namespace lumen::doc
