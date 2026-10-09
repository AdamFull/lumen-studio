#pragma once

#include "doc/Document.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QComboBox;

namespace lumen::ui {

class SliderField;

// Layer stack of the active document: visibility, lock, rename, blend mode, opacity, ordering.
class LayersPanel : public QWidget
{
    Q_OBJECT
public:
    explicit LayersPanel(QWidget *parent = nullptr);
    void setDocument(doc::Document *doc);

private:
    void rebuild();
    void syncActive();
    void scheduleThumbnails();
    void updateThumbnails();
    QWidget *rowWidget(const doc::LayerPtr &layer);

    QPointer<doc::Document> m_doc;
    QListWidget *m_list;
    QComboBox *m_blend;
    SliderField *m_opacity;
    QTimer m_thumbTimer;
    QHash<QUuid, QPixmap> m_thumbs;
    QHash<QUuid, quint64> m_thumbRevision;
    bool m_rebuilding = false;
};

} // namespace lumen::ui
