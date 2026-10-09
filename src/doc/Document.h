#pragma once

#include "doc/Layer.h"
#include "doc/TileGrid.h"
#include "pose/Skeleton.h"

#include <QJsonObject>
#include <QObject>
#include <QUndoStack>
#include <QUuid>

#include <optional>

namespace lumen::doc {

// A layered raster document with a selection mask (which doubles as the AI inpainting mask).
// GUI-thread object. Pixel data is shared copy-on-write, so snapshots for worker threads are cheap.
class Document : public QObject
{
    Q_OBJECT
public:
    explicit Document(const QSize &size, QObject *parent = nullptr);
    ~Document() override;

    // ---- identity --------------------------------------------------------------------------
    QString id() const { return m_id; }
    void setId(const QString &id) { m_id = id; }
    QString name() const { return m_name; }
    void setName(const QString &name);
    QSize size() const { return m_size; }
    QRect bounds() const { return QRect(QPoint(0, 0), m_size); }

    // ---- layers (index 0 = bottom) ---------------------------------------------------------
    const QList<LayerPtr> &layers() const { return m_layers; }
    int layerCount() const { return int(m_layers.size()); }
    LayerPtr layerAt(int index) const { return m_layers.value(index); }
    LayerPtr findLayer(const QUuid &id) const;
    int indexOf(const QUuid &id) const;
    LayerPtr activeLayer() const;
    QUuid activeLayerId() const { return m_active; }
    void setActiveLayer(const QUuid &id);

    // ---- selection / mask (Grayscale8, null = no selection) ---------------------------------
    const QImage &selection() const { return m_selection; }
    bool hasSelection() const;
    QRect selectionBounds() const;

    // ---- control pose attached to the document (used for pose-guided generation) ------------
    const std::optional<pose::Pose> &controlPose() const { return m_pose; }
    void setControlPose(const std::optional<pose::Pose> &pose);

    // ---- undoable high-level operations -----------------------------------------------------
    QUndoStack *undoStack() { return &m_undo; }
    QUuid addLayer(const QString &name, const QImage &content = QImage(), const QJsonObject &meta = {},
                   int index = -1, const QString &undoText = QString());
    void removeLayer(const QUuid &id);
    QUuid duplicateLayer(const QUuid &id);
    void moveLayer(const QUuid &id, int newIndex);
    void mergeDown(const QUuid &id);
    void flattenImage();
    void setLayerProps(const QUuid &id, const LayerProps &props, const QString &undoText = QString());
    // Replaces pixels of a layer inside rect with `pixels` (rect-sized) as one undo step.
    void applyPixels(const QUuid &id, const QRect &rect, const QImage &pixels, const QString &undoText);
    // Records an already-performed direct paint (stroke) given the pixels before the change.
    void commitPaint(const QUuid &id, const QRect &rect, const QImage &before, const QString &undoText);
    void applySelection(const QImage &mask, const QString &undoText);
    void clearSelection();
    void selectAll();
    void invertSelection();
    void commitSelectionPaint(const QRect &rect, const QImage &before, bool wasEmpty, const QString &undoText);
    // Crop (rect inside bounds) or extend (rect larger than bounds, negative origin allowed).
    void resizeCanvas(const QRect &newRect, const QString &undoText);
    void scaleImage(const QSize &newSize);

    // ---- direct mutation API (used by commands and tools; no undo) --------------------------
    void insertLayerRaw(int index, const LayerPtr &layer);
    LayerPtr takeLayerRaw(int index);
    void moveLayerRaw(int from, int to);
    void setLayerPropsRaw(const QUuid &id, const LayerProps &props);
    void writePixelsRaw(const QUuid &id, const QRect &rect, const QImage &pixels);
    void setSelectionRaw(const QImage &mask);
    void writeSelectionRaw(const QRect &rect, const QImage &pixels);
    // Direct access for painting tools; call notify* after modifying.
    QImage *layerImageForPainting(const QUuid &id);
    QImage &selectionForPainting();
    void notifyPixelsChanged(const QUuid &id, const QRect &rect);
    void notifySelectionChanged(const QRect &rect);
    void replaceContentRaw(const QSize &size, const QList<LayerPtr> &layers, const QImage &selection);

    // ---- rendering --------------------------------------------------------------------------
    DocumentSnapshot snapshot() const;
    QImage flattened(const QRect &region = QRect()) const;
    // Display cache; the canvas calls updateComposite for the visible region before painting.
    TileGrid &compositeTiles() { return m_tiles; }
    void updateComposite(const QRect &docRect, int level);

    // ---- state ------------------------------------------------------------------------------
    bool isModified() const { return m_modified; }
    void setModified(bool modified);
    quint64 revision() const { return m_revision; }
    QJsonObject settings() const { return m_settings; }
    void setSettings(const QJsonObject &s) { m_settings = s; }

signals:
    void nameChanged(const QString &name);
    void layersChanged();
    void layerPropsChanged(const QUuid &id);
    void pixelsChanged(const QUuid &id, const QRect &rect);
    void compositeChanged(const QRect &rect);
    void selectionChanged(const QRect &rect);
    void activeLayerChanged(const QUuid &id);
    void sizeChanged(const QSize &size);
    void modifiedChanged(bool modified);
    void controlPoseChanged();

private:
    void touch();
    void invalidate(const QRect &rect);

    QString m_id;
    QString m_name;
    QSize m_size;
    QList<LayerPtr> m_layers;
    QUuid m_active;
    QImage m_selection;
    std::optional<pose::Pose> m_pose;
    QUndoStack m_undo;
    TileGrid m_tiles;
    bool m_modified = false;
    quint64 m_revision = 0;
    QJsonObject m_settings;
};

} // namespace lumen::doc
