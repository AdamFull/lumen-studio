#include "doc/Commands.h"

namespace lumen::doc {

// ---- PixelsCommand ----------------------------------------------------------------------------

PixelsCommand::PixelsCommand(Document *doc, const QUuid &layer, const QRect &rect, const QImage &before,
                             const QImage &after, bool appliedAlready, const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_layer(layer)
    , m_rect(rect)
    , m_before(before)
    , m_after(after)
    , m_skipRedo(appliedAlready)
{
}

void PixelsCommand::redo()
{
    if (m_skipRedo) {
        m_skipRedo = false;
        return;
    }
    if (m_doc)
        m_doc->writePixelsRaw(m_layer, m_rect, m_after.image());
}

void PixelsCommand::undo()
{
    if (m_doc)
        m_doc->writePixelsRaw(m_layer, m_rect, m_before.image());
}

// ---- SelectionCommand -------------------------------------------------------------------------

SelectionCommand::SelectionCommand(Document *doc, const QRect &rect, const QImage &before, const QImage &after,
                                   bool beforeNull, bool afterNull, bool appliedAlready, const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_rect(rect)
    , m_before(before)
    , m_after(after)
    , m_beforeNull(beforeNull)
    , m_afterNull(afterNull)
    , m_skipRedo(appliedAlready)
{
}

void SelectionCommand::apply(const CompressedImage &pixels, bool isNull)
{
    if (!m_doc)
        return;
    if (isNull) {
        m_doc->setSelectionRaw(QImage());
        return;
    }
    m_doc->writeSelectionRaw(m_rect, pixels.image());
}

void SelectionCommand::redo()
{
    if (m_skipRedo) {
        m_skipRedo = false;
        return;
    }
    apply(m_after, m_afterNull);
}

void SelectionCommand::undo()
{
    apply(m_before, m_beforeNull);
}

// ---- Layer structure --------------------------------------------------------------------------

InsertLayerCommand::InsertLayerCommand(Document *doc, int index, LayerPtr layer, const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_index(index)
    , m_layer(std::move(layer))
{
}

void InsertLayerCommand::redo()
{
    if (!m_doc)
        return;
    m_previousActive = m_doc->activeLayerId();
    m_doc->insertLayerRaw(m_index, m_layer);
    m_doc->setActiveLayer(m_layer->id);
}

void InsertLayerCommand::undo()
{
    if (!m_doc)
        return;
    const int idx = m_doc->indexOf(m_layer->id);
    if (idx >= 0)
        m_doc->takeLayerRaw(idx);
    m_doc->setActiveLayer(m_previousActive);
}

RemoveLayerCommand::RemoveLayerCommand(Document *doc, const QUuid &id, const QString &text, QUndoCommand *parent)
    : QUndoCommand(text, parent)
    , m_doc(doc)
    , m_id(id)
{
}

void RemoveLayerCommand::redo()
{
    if (!m_doc)
        return;
    m_index = m_doc->indexOf(m_id);
    if (m_index < 0)
        return;
    m_wasActive = m_doc->activeLayerId() == m_id;
    m_layer = m_doc->takeLayerRaw(m_index);
    if (m_wasActive && m_doc->layerCount() > 0)
        m_doc->setActiveLayer(m_doc->layerAt(std::max(0, m_index - 1))->id);
}

void RemoveLayerCommand::undo()
{
    if (!m_doc || !m_layer)
        return;
    m_doc->insertLayerRaw(m_index, m_layer);
    if (m_wasActive)
        m_doc->setActiveLayer(m_id);
}

MoveLayerCommand::MoveLayerCommand(Document *doc, int from, int to)
    : QUndoCommand(QObject::tr("Move layer"))
    , m_doc(doc)
    , m_from(from)
    , m_to(to)
{
}

void MoveLayerCommand::redo()
{
    if (m_doc)
        m_doc->moveLayerRaw(m_from, m_to);
}

void MoveLayerCommand::undo()
{
    if (m_doc)
        m_doc->moveLayerRaw(m_to, m_from);
}

LayerPropsCommand::LayerPropsCommand(Document *doc, const QUuid &id, const LayerProps &before, const LayerProps &after,
                                     const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_layer(id)
    , m_before(before)
    , m_after(after)
{
}

void LayerPropsCommand::redo()
{
    if (m_doc)
        m_doc->setLayerPropsRaw(m_layer, m_after);
}

void LayerPropsCommand::undo()
{
    if (m_doc)
        m_doc->setLayerPropsRaw(m_layer, m_before);
}

bool LayerPropsCommand::mergeWith(const QUndoCommand *other)
{
    const auto *o = static_cast<const LayerPropsCommand *>(other);
    if (o->m_layer != m_layer || o->text() != text())
        return false;
    m_after = o->m_after;
    return true;
}

// ---- ContentCommand ---------------------------------------------------------------------------

ContentCommand::ContentCommand(Document *doc, State before, State after, const QString &text)
    : QUndoCommand(text)
    , m_doc(doc)
    , m_before(std::move(before))
    , m_after(std::move(after))
{
}

ContentCommand::State ContentCommand::capture(const Document *doc)
{
    State s;
    s.size = doc->size();
    for (const LayerPtr &l : doc->layers())
        s.layers.append(std::make_shared<Layer>(*l));
    s.selection = doc->selection();
    s.active = doc->activeLayerId();
    return s;
}

void ContentCommand::apply(const State &s)
{
    if (!m_doc)
        return;
    QList<LayerPtr> copies;
    for (const LayerPtr &l : s.layers) {
        auto c = std::make_shared<Layer>(*l);
        ++c->revision;
        copies.append(c);
    }
    m_doc->replaceContentRaw(s.size, copies, s.selection);
    m_doc->setActiveLayer(s.active);
}

void ContentCommand::redo()
{
    apply(m_after);
}

void ContentCommand::undo()
{
    apply(m_before);
}

} // namespace lumen::doc
