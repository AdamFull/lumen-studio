#pragma once

#include "ai/Types.h"

#include <QPointer>
#include <QWidget>

class QHBoxLayout;
class QScrollArea;

namespace lumen::doc {
class Project;
}

namespace lumen::ui {

class Studio;

// MIME type used to drag assets out of the library.
inline const char *kAssetMime = "application/x-lumen-asset-ids";

// Horizontal strip of reference images with per-image role and weight. Accepts dropped files,
// dragged library assets and pasted images; can import new images into the project as references.
class ReferenceStrip : public QWidget
{
    Q_OBJECT
public:
    explicit ReferenceStrip(Studio *studio, QWidget *parent = nullptr);

    QList<ai::ReferenceImage> references() const { return m_refs; }
    int count() const { return int(m_refs.size()); }
    void clear();
    void addImage(const QImage &image, const QString &assetId = QString(), ai::ReferenceRole role = ai::ReferenceRole::Character);
    void addAsset(const QString &assetId);
    void setMaximum(int max);
    void setCompact(bool compact);

signals:
    void changed();

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void rebuild();
    void browse();
    void importFiles(const QStringList &files);

    Studio *m_studio;
    QList<ai::ReferenceImage> m_refs;
    QHBoxLayout *m_items = nullptr;
    QWidget *m_container = nullptr;
    int m_max = 16;
    bool m_compact = false;
};

} // namespace lumen::ui
