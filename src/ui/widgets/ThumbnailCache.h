#pragma once

#include <QCache>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>

namespace lumen::doc {
class Project;
}

namespace lumen::ui {

// Lazily loads asset thumbnails from the project (off the GUI thread) and keeps an LRU cache.
class ThumbnailCache : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailCache(QObject *parent = nullptr);
    void setProject(doc::Project *project);
    // Returns the cached thumbnail or a null pixmap (and starts loading it).
    QPixmap get(const QString &assetId);
    void invalidate(const QString &assetId);
    void clear();

signals:
    void ready(const QString &assetId);

private:
    QPointer<doc::Project> m_project;
    QCache<QString, QPixmap> m_cache;
    QSet<QString> m_loading;
    quint64 m_generation = 0;
};

} // namespace lumen::ui
