#pragma once

#include "doc/CompressedImage.h"
#include "doc/Document.h"

#include <QPointer>
#include <QUndoCommand>

namespace lumen::doc {

enum CommandId { LayerPropsCommandId = 1001 };

// Replaces a rectangle of layer pixels. `appliedAlready` = the change was painted directly and the
// first redo() must not rewrite it.
class PixelsCommand : public QUndoCommand
{
public:
    PixelsCommand(Document *doc, const QUuid &layer, const QRect &rect, const QImage &before, const QImage &after,
                  bool appliedAlready, const QString &text);
    void redo() override;
    void undo() override;

private:
    QPointer<Document> m_doc;
    QUuid m_layer;
    QRect m_rect;
    CompressedImage m_before;
    CompressedImage m_after;
    bool m_skipRedo;
};

class SelectionCommand : public QUndoCommand
{
public:
    // before/after are full-document masks; null image = no selection.
    SelectionCommand(Document *doc, const QRect &rect, const QImage &before, const QImage &after, bool beforeNull,
                     bool afterNull, bool appliedAlready, const QString &text);
    void redo() override;
    void undo() override;

private:
    void apply(const CompressedImage &pixels, bool isNull);
    QPointer<Document> m_doc;
    QRect m_rect;
    CompressedImage m_before;
    CompressedImage m_after;
    bool m_beforeNull;
    bool m_afterNull;
    bool m_skipRedo;
};

class InsertLayerCommand : public QUndoCommand
{
public:
    InsertLayerCommand(Document *doc, int index, LayerPtr layer, const QString &text);
    void redo() override;
    void undo() override;

private:
    QPointer<Document> m_doc;
    int m_index;
    LayerPtr m_layer;
    QUuid m_previousActive;
};

class RemoveLayerCommand : public QUndoCommand
{
public:
    RemoveLayerCommand(Document *doc, const QUuid &id, const QString &text, QUndoCommand *parent = nullptr);
    void redo() override;
    void undo() override;

private:
    QPointer<Document> m_doc;
    QUuid m_id;
    int m_index = -1;
    LayerPtr m_layer;
    bool m_wasActive = false;
};

class MoveLayerCommand : public QUndoCommand
{
public:
    MoveLayerCommand(Document *doc, int from, int to);
    void redo() override;
    void undo() override;

private:
    QPointer<Document> m_doc;
    int m_from;
    int m_to;
};

class LayerPropsCommand : public QUndoCommand
{
public:
    LayerPropsCommand(Document *doc, const QUuid &id, const LayerProps &before, const LayerProps &after, const QString &text);
    void redo() override;
    void undo() override;
    int id() const override { return LayerPropsCommandId; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    QPointer<Document> m_doc;
    QUuid m_layer;
    LayerProps m_before;
    LayerProps m_after;
};

// Swaps the complete document content (canvas resize, crop, scale, flatten).
class ContentCommand : public QUndoCommand
{
public:
    struct State
    {
        QSize size;
        QList<LayerPtr> layers;
        QImage selection;
        QUuid active;
    };
    ContentCommand(Document *doc, State before, State after, const QString &text);
    void redo() override;
    void undo() override;
    static State capture(const Document *doc);

private:
    void apply(const State &s);
    QPointer<Document> m_doc;
    State m_before;
    State m_after;
};

} // namespace lumen::doc
