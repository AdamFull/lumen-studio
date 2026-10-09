#include "ui/canvas/ContextBar.h"

#include "doc/Document.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/widgets/Controls.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

namespace lumen::ui {

namespace {
constexpr int kGap = 12; // between the selection and the bar
}

ContextBar::ContextBar(CanvasView *view)
    : QFrame(view)
    , m_view(view)
{
    setObjectName(QStringLiteral("ContextBar"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    const Palette &pal = Theme::palette();
    setStyleSheet(QStringLiteral("#ContextBar { background: %1; border: 1px solid %2; border-radius: 10px; }")
                      .arg(pal.surface.name(), pal.border.name()));
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 6, 6, 6);
    h->setSpacing(6);
    auto *icon = new QToolButton(this);
    icon->setIcon(Icons::get(QStringLiteral("sparkles")));
    icon->setAutoRaise(true);
    icon->setFocusPolicy(Qt::NoFocus);
    icon->setToolTip(tr("Generative fill for the selection"));
    h->addWidget(icon);
    m_prompt = new QLineEdit(this);
    m_prompt->setPlaceholderText(tr("Describe what should appear here…"));
    m_prompt->setMinimumWidth(260);
    m_prompt->setClearButtonEnabled(true);
    h->addWidget(m_prompt, 1);
    m_generate = new QPushButton(tr("Generate"), this);
    Theme::setRole(m_generate, QStringLiteral("primary"));
    m_generate->setToolTip(tr("Inpaint the selection with this prompt (Enter)"));
    h->addWidget(m_generate);
    m_remove = new QPushButton(tr("Remove"), this);
    m_remove->setToolTip(tr("Remove the selected content and fill it from the surroundings"));
    h->addWidget(m_remove);
    m_more = iconButton(QStringLiteral("adjust"), tr("More options in the AI panel"), this);
    h->addWidget(m_more);
    m_close = iconButton(QStringLiteral("close"), tr("Hide until the selection changes"), this);
    m_close->setObjectName(QStringLiteral("ContextBarClose"));
    h->addWidget(m_close);

    connect(m_prompt, &QLineEdit::returnPressed, m_generate, &QPushButton::click);
    connect(m_generate, &QPushButton::clicked, this, [this] { emit generateRequested(m_prompt->text().trimmed()); });
    connect(m_remove, &QPushButton::clicked, this, &ContextBar::removeRequested);
    connect(m_more, &QToolButton::clicked, this, &ContextBar::moreRequested);
    connect(m_close, &QToolButton::clicked, this, [this] {
        m_dismissed = true;
        hide();
    });

    m_timer.setSingleShot(true);
    m_timer.setInterval(60);
    connect(&m_timer, &QTimer::timeout, this, &ContextBar::refresh);
    connect(view, &CanvasView::documentChanged, this, &ContextBar::attach);
    connect(view, &CanvasView::viewChanged, this, &ContextBar::place);
    connect(view, &CanvasView::interactionChanged, this, [this](bool active) {
        m_interacting = active;
        if (active)
            hide();
        else
            schedule();
    });
    attach(view->document());
    hide();
}

void ContextBar::attach(doc::Document *doc)
{
    if (m_doc)
        m_doc->disconnect(this);
    m_doc = doc;
    m_dismissed = false;
    if (doc) {
        connect(doc, &doc::Document::selectionChanged, this, [this] {
            m_dismissed = false;
            schedule();
        });
        connect(doc, &doc::Document::sizeChanged, this, &ContextBar::schedule);
    }
    schedule();
}

void ContextBar::setEnabledByUser(bool on)
{
    m_enabled = on;
    schedule();
}

void ContextBar::setRunning(int count)
{
    m_generate->setText(count > 0 ? tr("Generate (%1 running)").arg(count) : tr("Generate"));
    if (isVisible())
        place();
}

QString ContextBar::prompt() const
{
    return m_prompt->text();
}

void ContextBar::setPrompt(const QString &prompt)
{
    m_prompt->setText(prompt);
}

void ContextBar::schedule()
{
    m_timer.start();
}

void ContextBar::refresh()
{
    m_anchor = (m_doc && m_doc->hasSelection()) ? m_doc->selectionBounds() : QRect();
    place();
}

void ContextBar::place()
{
    if (!m_view || !m_enabled || m_dismissed || m_interacting || m_anchor.isEmpty()) {
        hide();
        return;
    }
    adjustSize();
    const QSize s = sizeHint().expandedTo(QSize(minimumSizeHint().width(), 0));
    const int width = std::min(std::max(s.width(), 440), m_view->width() - 16);
    const QRectF sel = m_view->mapFromDoc(QRectF(m_anchor));
    int x = int(sel.center().x() - width / 2.0);
    x = std::clamp(x, 8, std::max(8, m_view->width() - width - 8));
    int y = int(sel.bottom()) + kGap;
    if (y + s.height() > m_view->height() - 8) {
        // No room below: above the selection, or inside the view's bottom edge.
        y = int(sel.top()) - kGap - s.height();
        if (y < 8)
            y = m_view->height() - s.height() - 8;
    }
    setGeometry(x, y, width, s.height());
    show();
    raise();
}

} // namespace lumen::ui
