#include "ui/canvas/SmartSelectTool.h"

#include "ai/Provider.h"
#include "core/ImageOps.h"
#include "doc/Document.h"
#include "ui/SmartSelect.h"
#include "ui/Studio.h"
#include "ui/canvas/EditorState.h"

#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>

namespace lumen::ui {

SmartSelectTool::SmartSelectTool(EditorState *state, Studio *studio, QObject *parent)
    : SelectionTool(state, Shape::Rect, parent)
    , m_studio(studio)
{
    // A click that waited for the download runs when the components are there.
    connect(studio->smartSelect(), &SmartSelect::ready, this, [this] {
        if (m_pending && m_doc) {
            m_pending = false;
            run(nullptr);
        }
    });
}

void SmartSelectTool::press(CanvasView *, const ToolEvent &ev)
{
    m_pressed = true;
    m_dragging = false;
    m_start = m_current = ev.docPos;
    m_mods = ev.modifiers;
}

void SmartSelectTool::move(CanvasView *view, const ToolEvent &ev)
{
    if (!m_pressed)
        return;
    m_current = ev.docPos;
    // A drag of a few screen pixels is a box; less is a click.
    if (!m_dragging && QLineF(m_start, m_current).length() * view->zoom() > 4)
        m_dragging = true;
    emit overlayChanged();
}

void SmartSelectTool::release(CanvasView *view, const ToolEvent &ev)
{
    if (!m_pressed)
        return;
    m_pressed = false;
    m_current = ev.docPos;
    doc::Document *doc = view->document();
    if (!doc)
        return;
    const bool refine = (m_mods & (Qt::ShiftModifier | Qt::AltModifier)) && !m_dragging && doc == m_doc && !m_prompt.isEmpty();
    if (refine) {
        // A point on the object (Shift) or off it (Alt), for the object already found.
        m_prompt.points << m_current;
        m_prompt.positive << !(m_mods & Qt::AltModifier);
    } else {
        // A new object: its click or box, against the selection there is now.
        m_doc = doc;
        m_base = doc->hasSelection() ? doc->selection() : QImage();
        m_prompt = {};
        if (m_dragging)
            m_prompt.box = QRectF(m_start, m_current).normalized();
        else {
            m_prompt.points = {m_current};
            m_prompt.positive = {true};
        }
    }
    m_dragging = false;
    emit overlayChanged();
    run(view);
}

QImage SmartSelectTool::picture(doc::Document *doc)
{
    if (doc != m_pictureDoc || m_picture.isNull()) {
        if (m_pictureDoc)
            m_pictureDoc->disconnect(this);
        m_pictureDoc = doc;
        m_picture = doc->flattened();
        // A new picture (and a new encoding) only when the pixels change, not the selection.
        auto drop = [this] { m_picture = QImage(); };
        connect(doc, &doc::Document::pixelsChanged, this, drop);
        connect(doc, &doc::Document::layersChanged, this, drop);
        connect(doc, &doc::Document::sizeChanged, this, drop);
    }
    return m_picture;
}

void SmartSelectTool::run(CanvasView *view)
{
    if (!m_doc || m_prompt.isEmpty())
        return;
    SmartSelect *smart = m_studio->smartSelect();
    if (!smart->isReady()) {
        // Asked once; the click runs when the download is done.
        m_pending = true;
        if (m_asking || smart->isInstalling())
            return;
        const double mb = smart->missingSize() / 1e6;
        auto *box = new QMessageBox(QMessageBox::Question, tr("Smart selection"),
                                    tr("Smart selection cuts out the object you click. It needs two downloads, once: ONNX Runtime, the "
                                       "program that runs the model, and the SAM 2.1 tiny model, about %1 MB in all. It runs on this "
                                       "computer, without an image engine.")
                                        .arg(QLocale().toString(mb, 'f', 0)),
                                    QMessageBox::NoButton, view ? view->window() : nullptr);
        box->setObjectName(QStringLiteral("smartSelectDownload"));
        QPushButton *download = box->addButton(tr("Download"), QMessageBox::AcceptRole);
        box->addButton(QMessageBox::Cancel);
        box->setAttribute(Qt::WA_DeleteOnClose);
        m_asking = true;
        connect(box, &QMessageBox::finished, this, [this, box, download] {
            m_asking = false;
            if (box->clickedButton() == download) {
                m_studio->smartSelect()->install();
                m_studio->notify(tr("Downloading what Smart selection needs; the click is used when it is done (the Queue shows how far it is)."),
                                 QStringLiteral("info"));
            } else {
                m_pending = false;
            }
        });
        box->open();
        return;
    }
    if (m_busy) {
        // The newest prompt runs when the one in progress ends.
        m_pending = true;
        return;
    }
    m_busy = true;
    const int request = ++m_request;
    QPointer<SmartSelectTool> self(this);
    smart->segment(picture(m_doc), m_prompt)
        .then(this, [self, request](const QImage &mask) {
            if (!self)
                return;
            self->m_busy = false;
            if (request == self->m_request)
                self->apply(mask);
            if (std::exchange(self->m_pending, false))
                self->run(nullptr);
        })
        .onFailed(this, [self](const std::exception_ptr &e) {
            if (!self)
                return;
            self->m_busy = false;
            self->m_pending = false;
            self->m_studio->notify(tr("Smart selection did not work: %1").arg(ai::errorMessage(e)), QStringLiteral("warning"));
        });
    Q_UNUSED(view);
}

void SmartSelectTool::apply(const QImage &mask)
{
    if (!m_doc || mask.isNull())
        return;
    QImage found = mask;
    if (const int feather = state()->selectionFeather(); feather > 0)
        found = img::blurMask(found, feather);
    QImage result = found;
    if (!m_base.isNull()) {
        switch (state()->selectionMode()) {
        case EditorState::SelectionMode::Replace: break;
        case EditorState::SelectionMode::Add: result = img::combineMasks(m_base, found, img::MaskOp::Union); break;
        case EditorState::SelectionMode::Subtract: result = img::combineMasks(m_base, found, img::MaskOp::Subtract); break;
        case EditorState::SelectionMode::Intersect: result = img::combineMasks(m_base, found, img::MaskOp::Intersect); break;
        }
    }
    m_doc->applySelection(result, tr("Smart selection"));
    emit selected();
}

void SmartSelectTool::paintOverlay(CanvasView *view, QPainter &p)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double z = view->zoom();
    if (m_pressed && m_dragging) {
        QPen pen(QColor(255, 255, 255), 1.5 / z, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(m_start, m_current).normalized());
    }
    // The object's points: green on it, red off it.
    if (m_doc == view->document()) {
        for (int i = 0; i < m_prompt.points.size(); ++i) {
            const bool on = m_prompt.positive.value(i, true);
            p.setPen(QPen(Qt::white, 1.5 / z));
            p.setBrush(on ? QColor(40, 200, 90) : QColor(230, 60, 60));
            p.drawEllipse(m_prompt.points.at(i), 5 / z, 5 / z);
        }
    }
    p.restore();
}

} // namespace lumen::ui
