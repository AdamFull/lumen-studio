#include "ui/widgets/ThumbnailCache.h"

#include "doc/Project.h"

namespace lumen::ui {

ThumbnailCache::ThumbnailCache(QObject *parent)
    : QObject(parent)
{
    m_cache.setMaxCost(64 * 1024 * 1024); // bytes of pixmap data
}

void ThumbnailCache::setProject(doc::Project *project)
{
    m_project = project;
    clear();
}

void ThumbnailCache::clear()
{
    m_cache.clear();
    m_loading.clear();
    ++m_generation;
}

void ThumbnailCache::invalidate(const QString &assetId)
{
    m_cache.remove(assetId);
}

QPixmap ThumbnailCache::get(const QString &assetId)
{
    if (QPixmap *pm = m_cache.object(assetId))
        return *pm;
    if (!m_project || m_loading.contains(assetId))
        return {};
    m_loading.insert(assetId);
    const quint64 generation = m_generation;
    QPointer<ThumbnailCache> self(this);
    m_project->loadThumbnail(assetId)
        .then(this, [self, assetId, generation](QImage img) {
            if (!self || generation != self->m_generation)
                return;
            self->m_loading.remove(assetId);
            if (img.isNull())
                return;
            auto *pm = new QPixmap(QPixmap::fromImage(img));
            self->m_cache.insert(assetId, pm, int(std::max<qint64>(1, qint64(img.sizeInBytes()))));
            emit self->ready(assetId);
        })
        .onFailed(this, [self, assetId](const std::exception &) {
            if (self)
                self->m_loading.remove(assetId);
        });
    return {};
}

} // namespace lumen::ui
