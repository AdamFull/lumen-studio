#include "ui/widgets/FlowLayout.h"

#include <QHBoxLayout>
#include <QWidget>

namespace lumen::ui {

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing)
    : QLayout(parent)
    , m_hSpacing(hSpacing)
    , m_vSpacing(vSpacing)
{
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

QWidget *FlowLayout::addGroup(const QList<QWidget *> &widgets, int spacing)
{
    auto *group = new QWidget(parentWidget());
    auto *h = new QHBoxLayout(group);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(spacing);
    for (QWidget *w : widgets) {
        w->setParent(group);
        h->addWidget(w);
    }
    addWidget(group);
    return group;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= m_items.size())
        return nullptr;
    return m_items.takeAt(index);
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), false);
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : m_items) {
        if (!item->isEmpty())
            size = size.expandedTo(item->minimumSize().expandedTo(item->sizeHint().boundedTo(item->maximumSize())));
    }
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    // All items in one row.
    int width = 0;
    int height = 0;
    int visible = 0;
    for (const QLayoutItem *item : m_items) {
        if (item->isEmpty())
            continue;
        width += item->sizeHint().width();
        height = std::max(height, item->sizeHint().height());
        ++visible;
    }
    const QMargins m = contentsMargins();
    return QSize(width + std::max(0, visible - 1) * m_hSpacing + m.left() + m.right(), height + m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, true);
}

// Places the items row by row, each row vertically centred; returns the height used.
int FlowLayout::doLayout(const QRect &rect, bool apply) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.marginsRemoved(m);
    int x = area.x();
    int y = area.y();
    int rowHeight = 0;
    QList<std::pair<QLayoutItem *, QPoint>> row;
    auto flushRow = [&] {
        if (apply && !row.isEmpty()) {
            // The first right-aligned item and everything after it go to the row's right end.
            const auto &[lastItem, lastPos] = row.constLast();
            const int free = area.right() + 1 - (lastPos.x() + lastItem->sizeHint().width());
            bool right = false;
            for (const auto &[item, pos] : row) {
                right = right || (item->alignment() & Qt::AlignRight);
                const QSize s = item->sizeHint().boundedTo(QSize(area.width(), QWIDGETSIZE_MAX));
                const int x = pos.x() + (right ? std::max(0, free) : 0);
                item->setGeometry(QRect(QPoint(x, pos.y() + (rowHeight - s.height()) / 2), s));
            }
        }
        row.clear();
    };
    for (QLayoutItem *item : m_items) {
        if (item->isEmpty())
            continue;
        const QSize s = item->sizeHint();
        if (x > area.x() && x + s.width() > area.right() + 1) {
            flushRow();
            x = area.x();
            y += rowHeight + m_vSpacing;
            rowHeight = 0;
        }
        row.append({item, QPoint(x, y)});
        x += s.width() + m_hSpacing;
        rowHeight = std::max(rowHeight, s.height());
    }
    flushRow();
    return y + rowHeight - rect.y() + m.bottom();
}

} // namespace lumen::ui
