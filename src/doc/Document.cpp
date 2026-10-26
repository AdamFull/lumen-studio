#include "doc/Document.h"

#include "core/ImageOps.h"
#include "doc/Commands.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QTransform>

namespace lumen::doc {

Document::Document(const QSize &size, QObject *parent)
    : QObject(parent)
    , m_size(size)
{
    m_undo.setUndoLimit(300);
    m_tiles.reset(size);
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        if (!clean)
            setModified(true);
        // The history's clean state is the file's: undoing back to it clears the marker.
        if (hasFile())
            emit fileChanged();
    });
    connect(&m_undo, &QUndoStack::indexChanged, this, [this] { setModified(true); });
}

Document::~Document() = default;

void Document::setName(const QString &name)
{
    if (name == m_name)
        return;
    m_name = name;
    setModified(true);
    emit nameChanged(name);
}

LayerPtr Document::findLayer(const QUuid &id) const
{
    for (const LayerPtr &l : m_layers) {
        if (l->id == id)
            return l;
    }
    return nullptr;
}

int Document::indexOf(const QUuid &id) const
{
    for (int i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i]->id == id)
            return i;
    }
    return -1;
}

LayerPtr Document::activeLayer() const
{
    return findLayer(m_active);
}

void Document::setActiveLayer(const QUuid &id)
{
    if (id == m_active || (!id.isNull() && !findLayer(id)))
        return;
    m_active = id;
    emit activeLayerChanged(id);
}

bool Document::hasSelection() const
{
    return !m_selection.isNull();
}

QRect Document::selectionBounds() const
{
    return m_selection.isNull() ? QRect() : img::maskBounds(m_selection);
}

void Document::setControlPose(const std::optional<pose::Pose> &pose)
{
    m_pose = pose;
    touch();
    emit controlPoseChanged();
}

// ---- undoable operations ----------------------------------------------------------------------

QUuid Document::addLayer(const QString &name, const QImage &content, const QJsonObject &meta, int index, const QString &undoText)
{
    LayerPtr layer = makeLayer(name, m_size, content);
    layer->meta = meta;
    if (index < 0 || index > m_layers.size()) {
        const int active = indexOf(m_active);
        index = active >= 0 ? active + 1 : int(m_layers.size());
    }
    m_undo.push(new InsertLayerCommand(this, index, layer, undoText.isEmpty() ? tr("Add layer") : undoText));
    return layer->id;
}

void Document::removeLayer(const QUuid &id)
{
    if (!findLayer(id))
        return;
    m_undo.push(new RemoveLayerCommand(this, id, tr("Delete layer")));
}

QUuid Document::duplicateLayer(const QUuid &id)
{
    const LayerPtr src = findLayer(id);
    if (!src)
        return {};
    auto copy = std::make_shared<Layer>(*src);
    copy->id = QUuid::createUuid();
    copy->props.name = tr("%1 copy").arg(src->props.name);
    copy->savedRevision = ~quint64(0);
    m_undo.push(new InsertLayerCommand(this, indexOf(id) + 1, copy, tr("Duplicate layer")));
    return copy->id;
}

void Document::moveLayer(const QUuid &id, int newIndex)
{
    const int from = indexOf(id);
    newIndex = std::clamp(newIndex, 0, int(m_layers.size()) - 1);
    if (from < 0 || from == newIndex)
        return;
    m_undo.push(new MoveLayerCommand(this, from, newIndex));
}

void Document::mergeDown(const QUuid &id)
{
    const int idx = indexOf(id);
    if (idx <= 0)
        return;
    const LayerPtr upper = m_layers[idx];
    const LayerPtr lower = m_layers[idx - 1];
    QImage merged = lower->image.copy();
    {
        QPainter p(&merged);
        if (upper->props.visible) {
            p.setOpacity(upper->props.opacity);
            p.setCompositionMode(toCompositionMode(upper->props.blend));
            p.drawImage(0, 0, upper->image);
        }
    }
    m_undo.beginMacro(tr("Merge down"));
    m_undo.push(new PixelsCommand(this, lower->id, bounds(), lower->image, merged, false, QString()));
    m_undo.push(new RemoveLayerCommand(this, upper->id, QString()));
    m_undo.endMacro();
    setActiveLayer(lower->id);
}

void Document::flattenImage()
{
    if (m_layers.size() <= 1)
        return;
    ContentCommand::State before = ContentCommand::capture(this);
    ContentCommand::State after;
    after.size = m_size;
    auto layer = makeLayer(tr("Background"), m_size, flattened());
    after.layers = {layer};
    after.selection = m_selection;
    after.active = layer->id;
    m_undo.push(new ContentCommand(this, before, after, tr("Flatten image")));
}

void Document::setLayerProps(const QUuid &id, const LayerProps &props, const QString &undoText)
{
    const LayerPtr layer = findLayer(id);
    if (!layer || layer->props == props)
        return;
    m_undo.push(new LayerPropsCommand(this, id, layer->props, props, undoText.isEmpty() ? tr("Layer properties") : undoText));
}

void Document::applyPixels(const QUuid &id, const QRect &rect, const QImage &pixels, const QString &undoText)
{
    const LayerPtr layer = findLayer(id);
    const QRect r = rect.intersected(bounds());
    if (!layer || r.isEmpty())
        return;
    const QImage before = layer->image.copy(r);
    QImage after = pixels;
    if (r != rect)
        after = pixels.copy(r.translated(-rect.topLeft()));
    m_undo.push(new PixelsCommand(this, id, r, before, img::normalized(after), false, undoText));
}

void Document::commitPaint(const QUuid &id, const QRect &rect, const QImage &before, const QString &undoText)
{
    const LayerPtr layer = findLayer(id);
    const QRect r = rect.intersected(bounds());
    if (!layer || r.isEmpty())
        return;
    m_undo.push(new PixelsCommand(this, id, r, before, layer->image.copy(r), true, undoText));
}

void Document::applySelection(const QImage &mask, const QString &undoText)
{
    const bool beforeNull = m_selection.isNull();
    const bool afterNull = mask.isNull() || img::maskIsEmpty(mask);
    if (beforeNull && afterNull)
        return;
    const QImage before = beforeNull ? img::createMask(m_size) : m_selection;
    const QImage after = afterNull ? img::createMask(m_size) : img::scaled(mask.convertToFormat(QImage::Format_Grayscale8), m_size, false);
    m_undo.push(new SelectionCommand(this, bounds(), before, after, beforeNull, afterNull, false, undoText));
}

void Document::clearSelection()
{
    applySelection(QImage(), tr("Deselect"));
}

void Document::selectAll()
{
    applySelection(img::createMask(m_size, 255), tr("Select all"));
}

void Document::invertSelection()
{
    applySelection(m_selection.isNull() ? img::createMask(m_size, 255) : img::invertMask(m_selection), tr("Invert selection"));
}

void Document::commitSelectionPaint(const QRect &rect, const QImage &before, bool wasEmpty, const QString &undoText)
{
    const QRect r = rect.intersected(bounds());
    if (r.isEmpty() || m_selection.isNull())
        return;
    m_undo.push(new SelectionCommand(this, r, before, m_selection.copy(r), wasEmpty, false, true, undoText));
}

void Document::resizeCanvas(const QRect &newRect, const QString &undoText)
{
    if (newRect.isEmpty() || newRect == bounds())
        return;
    ContentCommand::State before = ContentCommand::capture(this);
    ContentCommand::State after;
    after.size = newRect.size();
    after.active = m_active;
    for (const LayerPtr &l : m_layers) {
        auto c = std::make_shared<Layer>(*l);
        QImage img(newRect.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(-newRect.topLeft(), l->image);
        p.end();
        c->image = img;
        after.layers.append(c);
    }
    if (!m_selection.isNull()) {
        QImage sel = img::createMask(newRect.size());
        QPainter p(&sel);
        p.drawImage(-newRect.topLeft(), m_selection);
        p.end();
        after.selection = img::maskIsEmpty(sel) ? QImage() : sel;
    }
    m_undo.push(new ContentCommand(this, before, after, undoText.isEmpty() ? tr("Resize canvas") : undoText));
}

void Document::scaleImage(const QSize &newSize)
{
    if (newSize.isEmpty() || newSize == m_size)
        return;
    ContentCommand::State before = ContentCommand::capture(this);
    ContentCommand::State after;
    after.size = newSize;
    after.active = m_active;
    for (const LayerPtr &l : m_layers) {
        auto c = std::make_shared<Layer>(*l);
        c->image = img::normalized(l->image.scaled(newSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        after.layers.append(c);
    }
    if (!m_selection.isNull())
        after.selection = img::scaled(m_selection, newSize, true);
    m_undo.push(new ContentCommand(this, before, after, tr("Scale image")));
}

namespace {

// Pixel-exact: a turn by a multiple of 90 degrees or a mirror maps every pixel onto one pixel.
QImage turned(const QImage &image, int degrees, std::optional<Qt::Orientation> flip)
{
    if (image.isNull())
        return image;
    QTransform t;
    if (flip)
        t.scale(*flip == Qt::Horizontal ? -1 : 1, *flip == Qt::Vertical ? -1 : 1);
    else
        t.rotate(degrees);
    return image.transformed(t, Qt::FastTransformation);
}

} // namespace

void Document::rotateImage(int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees != 90 && degrees != 180 && degrees != 270)
        return;
    ContentCommand::State before = ContentCommand::capture(this);
    ContentCommand::State after;
    after.size = degrees == 180 ? m_size : m_size.transposed();
    after.active = m_active;
    for (const LayerPtr &l : m_layers) {
        auto c = std::make_shared<Layer>(*l);
        c->image = turned(l->image, degrees, std::nullopt);
        after.layers.append(c);
    }
    if (!m_selection.isNull())
        after.selection = turned(m_selection, degrees, std::nullopt);
    m_undo.push(new ContentCommand(this, before, after,
                                   degrees == 90    ? tr("Rotate 90° clockwise")
                                   : degrees == 270 ? tr("Rotate 90° counter-clockwise")
                                                    : tr("Rotate 180°")));
}

void Document::flipImage(Qt::Orientation orientation)
{
    ContentCommand::State before = ContentCommand::capture(this);
    ContentCommand::State after;
    after.size = m_size;
    after.active = m_active;
    for (const LayerPtr &l : m_layers) {
        auto c = std::make_shared<Layer>(*l);
        c->image = turned(l->image, 0, orientation);
        after.layers.append(c);
    }
    if (!m_selection.isNull())
        after.selection = turned(m_selection, 0, orientation);
    m_undo.push(new ContentCommand(this, before, after, orientation == Qt::Horizontal ? tr("Flip horizontally") : tr("Flip vertically")));
}

// ---- raw mutation -----------------------------------------------------------------------------

void Document::insertLayerRaw(int index, const LayerPtr &layer)
{
    index = std::clamp(index, 0, int(m_layers.size()));
    m_layers.insert(index, layer);
    if (m_active.isNull())
        m_active = layer->id;
    invalidate(bounds());
    touch();
    emit layersChanged();
}

LayerPtr Document::takeLayerRaw(int index)
{
    if (index < 0 || index >= m_layers.size())
        return nullptr;
    LayerPtr layer = m_layers.takeAt(index);
    if (m_active == layer->id) {
        m_active = m_layers.isEmpty() ? QUuid() : m_layers[std::clamp(index - 1, 0, int(m_layers.size()) - 1)]->id;
        emit activeLayerChanged(m_active);
    }
    invalidate(bounds());
    touch();
    emit layersChanged();
    return layer;
}

void Document::moveLayerRaw(int from, int to)
{
    if (from < 0 || from >= m_layers.size() || to < 0 || to >= m_layers.size())
        return;
    m_layers.move(from, to);
    invalidate(bounds());
    touch();
    emit layersChanged();
}

void Document::setLayerPropsRaw(const QUuid &id, const LayerProps &props)
{
    const LayerPtr layer = findLayer(id);
    if (!layer)
        return;
    const bool visualChange = layer->props.opacity != props.opacity || layer->props.blend != props.blend
                           || layer->props.visible != props.visible;
    layer->props = props;
    if (visualChange)
        invalidate(bounds());
    touch();
    emit layerPropsChanged(id);
}

void Document::writePixelsRaw(const QUuid &id, const QRect &rect, const QImage &pixels)
{
    const LayerPtr layer = findLayer(id);
    if (!layer || pixels.isNull())
        return;
    QPainter p(&layer->image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(rect.topLeft(), pixels);
    p.end();
    notifyPixelsChanged(id, rect);
}

void Document::setSelectionRaw(const QImage &mask)
{
    m_selection = mask.isNull() ? QImage() : mask.convertToFormat(QImage::Format_Grayscale8);
    touch();
    emit selectionChanged(bounds());
}

void Document::writeSelectionRaw(const QRect &rect, const QImage &pixels)
{
    if (m_selection.isNull())
        m_selection = img::createMask(m_size);
    QPainter p(&m_selection);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(rect.topLeft(), pixels.convertToFormat(QImage::Format_Grayscale8));
    p.end();
    touch();
    emit selectionChanged(rect);
}

QImage *Document::layerImageForPainting(const QUuid &id)
{
    const LayerPtr layer = findLayer(id);
    return layer ? &layer->image : nullptr;
}

QImage &Document::selectionForPainting()
{
    if (m_selection.isNull())
        m_selection = img::createMask(m_size);
    return m_selection;
}

void Document::notifyPixelsChanged(const QUuid &id, const QRect &rect)
{
    if (const LayerPtr layer = findLayer(id))
        ++layer->revision;
    invalidate(rect);
    touch();
    emit pixelsChanged(id, rect);
}

void Document::notifySelectionChanged(const QRect &rect)
{
    touch();
    emit selectionChanged(rect);
}

void Document::replaceContentRaw(const QSize &size, const QList<LayerPtr> &layers, const QImage &selection)
{
    const bool resized = size != m_size;
    m_size = size;
    m_layers = layers;
    m_selection = selection;
    if (!findLayer(m_active))
        m_active = m_layers.isEmpty() ? QUuid() : m_layers.last()->id;
    if (resized)
        m_tiles.reset(size);
    invalidate(bounds());
    touch();
    if (resized)
        emit sizeChanged(size);
    emit layersChanged();
    emit selectionChanged(bounds());
    emit activeLayerChanged(m_active);
}

// ---- rendering --------------------------------------------------------------------------------

DocumentSnapshot Document::snapshot() const
{
    DocumentSnapshot s;
    s.reserve(m_layers.size());
    for (const LayerPtr &l : m_layers)
        s.append(LayerSnapshot{l->image, l->props, l->meta.value(QStringLiteral("psd_group")).toString()});
    return s;
}

QImage Document::flattened(const QRect &region) const
{
    return flatten(snapshot(), m_size, region);
}

void Document::updateComposite(const QRect &docRect, int level)
{
    const DocumentSnapshot snap = snapshot();
    m_tiles.update(docRect, level, [&snap](QImage &target, const QRect &rect) {
        target.fill(Qt::transparent);
        QPainter p(&target);
        for (const LayerSnapshot &l : snap) {
            if (!l.props.visible || l.props.opacity <= 0.0)
                continue;
            p.setOpacity(l.props.opacity);
            p.setCompositionMode(toCompositionMode(l.props.blend));
            p.drawImage(QPoint(0, 0), l.image, rect);
        }
    });
}

void Document::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modifiedChanged(modified);
}

QString FileLink::fileName() const
{
    return QFileInfo(path).fileName();
}

void FileLink::stamp()
{
    const QFileInfo info(path);
    size = info.exists() ? info.size() : -1;
    modified = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
}

bool FileLink::isUnchangedOnDisk() const
{
    const QFileInfo info(path);
    return !path.isEmpty() && info.exists() && size >= 0 && info.size() == size && info.lastModified().toMSecsSinceEpoch() == modified;
}

QJsonObject FileLink::toJson() const
{
    QJsonObject o{{QStringLiteral("path"), path}, {QStringLiteral("format"), QString::fromLatin1(format)}};
    if (quality > 0)
        o.insert(QStringLiteral("quality"), quality);
    if (!losses.isEmpty())
        o.insert(QStringLiteral("losses"), QJsonArray::fromStringList(losses));
    if (dontAsk)
        o.insert(QStringLiteral("dontAsk"), true);
    if (size >= 0) {
        o.insert(QStringLiteral("size"), double(size));
        o.insert(QStringLiteral("modified"), double(modified));
    }
    return o;
}

FileLink FileLink::fromJson(const QJsonObject &json)
{
    FileLink f;
    f.path = json.value(QStringLiteral("path")).toString();
    f.format = json.value(QStringLiteral("format")).toString().toLatin1();
    f.quality = json.value(QStringLiteral("quality")).toInt(-1);
    for (const QJsonValue &v : json.value(QStringLiteral("losses")).toArray())
        f.losses << v.toString();
    f.dontAsk = json.value(QStringLiteral("dontAsk")).toBool();
    f.size = qint64(json.value(QStringLiteral("size")).toDouble(-1));
    f.modified = qint64(json.value(QStringLiteral("modified")).toDouble(-1));
    return f;
}

void Document::setFile(const FileLink &file)
{
    m_file = file;
    m_fileDirty = false;
    m_undo.setClean();
    emit fileChanged();
}

void Document::updateFile(const FileLink &file)
{
    if (m_file == file)
        return;
    m_file = file;
    emit fileChanged();
}

void Document::markFileSaved()
{
    m_fileDirty = false;
    m_undo.setClean();
    emit fileChanged();
}

bool Document::isFileModified() const
{
    return hasFile() && (m_fileDirty || !m_undo.isClean());
}

void Document::setFileModified(bool modified)
{
    if (m_fileDirty == modified)
        return;
    m_fileDirty = modified;
    emit fileChanged();
}

void Document::touch()
{
    ++m_revision;
    setModified(true);
}

void Document::invalidate(const QRect &rect)
{
    m_tiles.invalidate(rect);
    emit compositeChanged(rect);
}

} // namespace lumen::doc
