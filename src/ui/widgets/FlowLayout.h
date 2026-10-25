#pragma once

#include <QLayout>
#include <QList>

class QWidget;

namespace lumen::ui {

// Lays items out left to right and wraps to a new row when the width runs out, so a toolbar
// stays usable on a narrow window instead of squeezing its buttons. Items keep their size hint.
// Its minimum width is that of the widest item; the height follows from the width
// (heightForWidth), which QBoxLayout parents honour. An item aligned right (setAlignment(widget,
// Qt::AlignRight)) moves, with the items after it, to the right end of its row.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = 6, int vSpacing = 6);
    ~FlowLayout() override;

    // Adds several widgets as one item that wraps as a whole (e.g. a label and its field).
    QWidget *addGroup(const QList<QWidget *> &widgets, int spacing = 6);

    void addItem(QLayoutItem *item) override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    int doLayout(const QRect &rect, bool apply) const;

    QList<QLayoutItem *> m_items;
    int m_hSpacing;
    int m_vSpacing;
};

} // namespace lumen::ui
