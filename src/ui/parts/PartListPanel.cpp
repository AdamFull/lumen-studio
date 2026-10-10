#include "ui/parts/PartListPanel.h"

#include "core/Async.h"
#include "core/ImageOps.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/parts/PartTools.h"
#include "ui/widgets/Controls.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

constexpr int kLabelRole = Qt::UserRole + 1;
constexpr int kColName = 0, kColState = 1, kColVisible = 2, kColLock = 3;

QIcon swatchIcon(const QColor &c)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.setBrush(c);
    p.drawRoundedRect(QRectF(1.5, 1.5, 11, 11), 3, 3);
    return QIcon(pm);
}

} // namespace

PartListPanel::PartListPanel(PartEditContext *context, QWidget *parent)
    : QWidget(parent)
    , m_context(context)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->addWidget(sectionLabel(tr("Parts"), this));
    header->addStretch();
    m_attention = iconButton(QStringLiteral("warning"), tr("Show only parts that need attention (empty, failed, not generated)"), this, true);
    header->addWidget(m_attention);
    v->addLayout(header);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(12);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setUniformRowHeights(true);
    m_tree->setIconSize(QSize(14, 14));
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(kColName, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(kColState, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(kColVisible, QHeaderView::Fixed);
    m_tree->header()->setSectionResizeMode(kColLock, QHeaderView::Fixed);
    m_tree->setColumnWidth(kColVisible, 26);
    m_tree->setColumnWidth(kColLock, 26);
    v->addWidget(m_tree, 1);

    // Unassigned pixels indicator.
    auto *unassignedRow = new QHBoxLayout;
    m_unassigned = mutedLabel(QString(), this);
    m_unassigned->setWordWrap(true);
    m_showUnassigned = iconButton(QStringLiteral("eye"), tr("Highlight character pixels that belong to no part"), this, true);
    unassignedRow->addWidget(m_unassigned, 1);
    unassignedRow->addWidget(m_showUnassigned);
    v->addLayout(unassignedRow);

    // Actions.
    auto *actions = new QHBoxLayout;
    actions->setSpacing(4);
    auto *add = iconButton(QStringLiteral("plus"), tr("Add a part"), this);
    add->setPopupMode(QToolButton::InstantPopup);
    add->setMenu(buildAddMenu(add));
    m_merge = iconButton(QStringLiteral("merge"), tr("Merge the selected parts into one"), this);
    m_split = iconButton(QStringLiteral("scissors"), tr("Split the selected part"), this);
    m_split->setPopupMode(QToolButton::InstantPopup);
    auto *splitMenu = new QMenu(m_split);
    splitMenu->addAction(tr("Left / right"), this, [this] {
        if (m_context->activeLabel())
            emit splitRequested(m_context->activeLabel(), Split::LeftRight);
    });
    splitMenu->addAction(tr("Separate islands"), this, [this] {
        if (m_context->activeLabel())
            emit splitRequested(m_context->activeLabel(), Split::Islands);
    });
    splitMenu->addAction(tr("By skeleton (upper arm / forearm / hand...)"), this, [this] {
        if (m_context->activeLabel())
            emit splitRequested(m_context->activeLabel(), Split::Skeleton);
    });
    splitMenu->addSeparator();
    splitMenu->addAction(Icons::get(QStringLiteral("scissors")), tr("Draw a cut line..."), this, &PartListPanel::cutToolRequested);
    m_split->setMenu(splitMenu);
    m_delete = iconButton(QStringLiteral("trash"), tr("Delete the selected parts (their pixels become unassigned)"), this);
    actions->addWidget(add);
    actions->addWidget(m_merge);
    actions->addWidget(m_split);
    actions->addStretch();
    actions->addWidget(m_delete);
    v->addLayout(actions);

    auto *recount = new QTimer(this);
    recount->setSingleShot(true);
    recount->setInterval(350);
    connect(recount, &QTimer::timeout, this, [this] {
        if (!m_set || !m_set->hasSource()) {
            m_unassigned->clear();
            return;
        }
        const QImage labels = m_set->labels();
        const QImage sil = m_set->silhouette();
        QPointer<PartListPanel> self(this);
        async::run([labels, sil] {
            qint64 free = 0, total = 0;
            const bool all = sil.isNull() || sil.size() != labels.size();
            for (int y = 0; y < labels.height(); ++y) {
                const uchar *l = labels.constScanLine(y);
                const uchar *s = all ? nullptr : sil.constScanLine(y);
                for (int x = 0; x < labels.width(); ++x) {
                    if (s && s[x] < 128)
                        continue;
                    ++total;
                    free += l[x] == 0;
                }
            }
            return std::make_pair(free, total);
        }).then(this, [self](std::pair<qint64, qint64> r) {
            if (!self)
                return;
            const double pct = r.second > 0 ? 100.0 * double(r.first) / double(r.second) : 0.0;
            self->m_unassigned->setText(r.first == 0 ? tr("Every character pixel belongs to a part.")
                                                     : tr("Unassigned: %1% of the character").arg(pct, 0, 'f', pct < 1 ? 2 : 1));
        });
    });

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &PartListPanel::syncSelection);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &PartListPanel::showContextMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
        const int label = item->data(0, kLabelRole).toInt();
        if (!m_set || label == 0 || (column != kColVisible && column != kColLock))
            return;
        doc::Part p = *m_set->part(label);
        if (column == kColVisible)
            p.visible = !p.visible;
        else
            p.locked = !p.locked;
        m_set->updatePart(p, column == kColVisible ? (p.visible ? tr("Show part") : tr("Hide part"))
                                                   : (p.locked ? tr("Lock part") : tr("Unlock part")));
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        const int label = item->data(0, kLabelRole).toInt();
        if (label && column == kColName)
            renamePart(label);
    });
    connect(m_attention, &QToolButton::toggled, this, &PartListPanel::rebuild);
    connect(m_showUnassigned, &QToolButton::toggled, this, &PartListPanel::showUnassignedChanged);
    connect(m_merge, &QToolButton::clicked, this, &PartListPanel::mergeSelected);
    connect(m_delete, &QToolButton::clicked, this, &PartListPanel::deleteSelected);

    connect(context, &PartEditContext::setChanged, this, [this, recount] {
        if (m_set)
            m_set->disconnect(this);
        m_set = m_context->set();
        if (m_set) {
            connect(m_set, &doc::PartSet::partsChanged, this, &PartListPanel::rebuild);
            connect(m_set, &doc::PartSet::partStateChanged, this, &PartListPanel::updateStates);
            connect(m_set, &doc::PartSet::resultChanged, this, &PartListPanel::updateStates);
            connect(m_set, &doc::PartSet::taxonomyChanged, this, &PartListPanel::rebuild);
            connect(m_set, &doc::PartSet::labelsChanged, recount, qOverload<>(&QTimer::start));
            connect(m_set, &doc::PartSet::sourceChanged, recount, qOverload<>(&QTimer::start));
        }
        rebuild();
        recount->start();
    });
    connect(context, &PartEditContext::activeLabelChanged, this, [this](int label) {
        if (m_syncing)
            return;
        m_syncing = true;
        m_tree->clearSelection();
        QTreeWidgetItemIterator it(m_tree);
        while (*it) {
            if ((*it)->data(0, kLabelRole).toInt() == label && label != 0) {
                (*it)->setSelected(true);
                m_tree->scrollToItem(*it);
            }
            ++it;
        }
        m_syncing = false;
    });
    rebuild();
}

QList<int> PartListPanel::selectedLabels() const
{
    QList<int> labels;
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        const int l = item->data(0, kLabelRole).toInt();
        if (l)
            labels.append(l);
    }
    return labels;
}

void PartListPanel::showState(QTreeWidgetItem *item, const doc::Part &p) const
{
    item->setText(kColState, stateText(p));
    // A warning (the face check of an expression) shows until another result is chosen.
    const bool warn = !p.warning.isEmpty() && p.state != doc::PartState::Failed;
    item->setIcon(kColState, warn ? Icons::get(QStringLiteral("warning")) : QIcon());
    item->setForeground(kColState, p.state == doc::PartState::Failed ? Theme::palette().danger
                                   : warn                             ? Theme::palette().warning
                                                                      : Theme::palette().textMuted);
    item->setToolTip(kColState, warn ? p.warning : p.error);
}

QString PartListPanel::stateText(const doc::Part &p) const
{
    switch (p.state) {
    case doc::PartState::Empty: return tr("empty");
    case doc::PartState::Queued: return tr("queued");
    case doc::PartState::Generating: return tr("generating…");
    case doc::PartState::Failed: return tr("failed");
    case doc::PartState::Ready: return p.variants.size() > 1 ? tr("ready · %1/%2").arg(p.chosen + 1).arg(p.variants.size()) : tr("ready");
    case doc::PartState::Approved: return tr("approved");
    case doc::PartState::Masked: break;
    }
    if (m_set && m_set->hasSource()) {
        const double pct = 100.0 * double(p.pixels) / double(std::max<qint64>(1, qint64(m_set->size().width()) * m_set->size().height()));
        return QStringLiteral("%1%").arg(pct, 0, 'f', pct < 1 ? 2 : 1);
    }
    return {};
}

void PartListPanel::rebuild()
{
    m_syncing = true;
    m_tree->clear();
    m_merge->setEnabled(false);
    m_split->setEnabled(false);
    m_delete->setEnabled(false);
    if (!m_set) {
        m_syncing = false;
        return;
    }
    const doc::PartTaxonomy &taxonomy = m_set->taxonomy();
    QHash<QString, QTreeWidgetItem *> groups;
    QStringList groupOrder = taxonomy.groups();
    groupOrder.append(QStringLiteral("custom"));
    for (const QString &g : groupOrder) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(kColName, doc::partGroupLabel(g));
        item->setFlags(Qt::ItemIsEnabled);
        QFont f = item->font(kColName);
        f.setBold(true);
        item->setFont(kColName, f);
        item->setForeground(kColName, Theme::palette().textMuted);
        groups.insert(g, item);
    }
    // Front-most parts first, like a layers panel.
    QList<const doc::Part *> order = m_set->partsInDrawOrder();
    std::reverse(order.begin(), order.end());
    const bool attentionOnly = m_attention->isChecked();
    for (const doc::Part *p : order) {
        const bool needsAttention = p->state == doc::PartState::Empty || p->state == doc::PartState::Failed
                                    || p->state == doc::PartState::Masked;
        if (attentionOnly && !needsAttention)
            continue;
        const doc::PartKind *kind = taxonomy.find(p->tag);
        QTreeWidgetItem *parent = groups.value(kind ? kind->group : QStringLiteral("custom"));
        auto *item = new QTreeWidgetItem(parent);
        item->setData(0, kLabelRole, p->label);
        item->setIcon(kColName, swatchIcon(p->color));
        item->setText(kColName, p->displayName());
        item->setToolTip(kColName, kind ? tr("%1 · draw order %2").arg(kind->name).arg(p->order) : tr("Custom part · draw order %1").arg(p->order));
        showState(item, *p);
        item->setIcon(kColVisible, Icons::get(p->visible ? QStringLiteral("eye") : QStringLiteral("eye-off")));
        item->setToolTip(kColVisible, p->visible ? tr("Hide") : tr("Show"));
        item->setIcon(kColLock, Icons::get(p->locked ? QStringLiteral("lock") : QStringLiteral("unlock")));
        item->setToolTip(kColLock, p->locked ? tr("Unlock (allow painting over this part)") : tr("Lock (protect from painting)"));
        if (p->label == m_context->activeLabel())
            item->setSelected(true);
    }
    for (QTreeWidgetItem *g : std::as_const(groups)) {
        if (g->childCount() == 0) {
            delete g;
        } else {
            g->setExpanded(true);
            g->setFirstColumnSpanned(true);
        }
    }
    if (m_tree->topLevelItemCount() == 0) {
        auto *hint = new QTreeWidgetItem(m_tree);
        hint->setText(kColName, m_set->hasSource() ? tr("No parts yet - run Auto-segment or add one with +") : tr("Choose a source image"));
        hint->setFlags(Qt::NoItemFlags);
        hint->setForeground(kColName, Theme::palette().textMuted);
        hint->setFirstColumnSpanned(true);
    }
    m_syncing = false;
    const QList<int> sel = selectedLabels();
    m_merge->setEnabled(sel.size() >= 2);
    m_split->setEnabled(sel.size() == 1);
    m_delete->setEnabled(!sel.isEmpty());
}

void PartListPanel::updateStates()
{
    if (!m_set)
        return;
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        const int label = (*it)->data(0, kLabelRole).toInt();
        if (const doc::Part *p = label ? m_set->part(label) : nullptr) {
            showState(*it, *p);
        }
        ++it;
    }
}

void PartListPanel::syncSelection()
{
    const QList<int> sel = selectedLabels();
    m_merge->setEnabled(sel.size() >= 2);
    m_split->setEnabled(sel.size() == 1);
    m_delete->setEnabled(!sel.isEmpty());
    if (m_syncing)
        return;
    m_syncing = true;
    QTreeWidgetItem *current = m_tree->currentItem();
    int active = current && current->isSelected() ? current->data(0, kLabelRole).toInt() : 0;
    if (!active && !sel.isEmpty())
        active = sel.first();
    m_context->setActiveLabel(active);
    m_syncing = false;
}

QMenu *PartListPanel::buildAddMenu(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        if (!m_set || !m_set->hasSource()) {
            menu->addAction(tr("Choose a source image first"))->setEnabled(false);
            return;
        }
        const doc::PartTaxonomy &taxonomy = m_set->taxonomy();
        for (const QString &g : taxonomy.groups()) {
            QMenu *sub = menu->addMenu(doc::partGroupLabel(g));
            for (const doc::PartKind &k : taxonomy.kinds) {
                if (k.group != g)
                    continue;
                QAction *a = sub->addAction(swatchIcon(k.color), k.name, this, [this, tag = k.tag] { addPart(tag); });
                a->setToolTip(k.description);
            }
        }
        menu->addSeparator();
        menu->addAction(tr("Custom part..."), this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Add part"), tr("Part name:"), QLineEdit::Normal, QString(), &ok).trimmed();
            if (ok && !name.isEmpty())
                addPart(QString(), name);
        });
    });
    return menu;
}

QMenu *PartListPanel::buildKindMenu(QWidget *parent, int label)
{
    auto *menu = new QMenu(tr("Kind"), parent);
    if (!m_set)
        return menu;
    const doc::Part *current = m_set->part(label);
    const doc::PartTaxonomy &taxonomy = m_set->taxonomy();
    for (const QString &g : taxonomy.groups()) {
        QMenu *sub = menu->addMenu(doc::partGroupLabel(g));
        for (const doc::PartKind &k : taxonomy.kinds) {
            if (k.group != g)
                continue;
            QAction *a = sub->addAction(swatchIcon(k.color), k.name, this, [this, label, k] {
                if (!m_set || !m_set->part(label))
                    return;
                doc::Part p = *m_set->part(label);
                const doc::PartKind *old = m_set->taxonomy().find(p.tag);
                if (!old || p.name == old->name)
                    p.name = k.name;
                p.tag = k.tag;
                p.order = k.order * 4;
                p.color = m_set->suggestColor(k.tag);
                m_set->updatePart(p, tr("Change part kind"));
            });
            a->setCheckable(true);
            a->setChecked(current && current->tag == k.tag);
        }
    }
    return menu;
}

void PartListPanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    const int label = item ? item->data(0, kLabelRole).toInt() : 0;
    if (!m_set || !label)
        return;
    const doc::Part part = *m_set->part(label);
    QMenu menu(this);
    menu.addAction(Icons::get(QStringLiteral("sparkles")), tr("Generate this part"), this, [this, label] { emit generateRequested({label}); });
    const QList<int> sel = selectedLabels();
    if (sel.size() > 1)
        menu.addAction(tr("Generate %n selected parts", nullptr, int(sel.size())), this, [this, sel] { emit generateRequested(sel); });
    menu.addSeparator();
    menu.addAction(Icons::get(QStringLiteral("edit")), tr("Rename..."), this, [this, label] { renamePart(label); });
    menu.addMenu(buildKindMenu(&menu, label));
    QMenu *side = menu.addMenu(tr("Side"));
    for (const auto &[value, text] : {std::pair{QString(), tr("None")}, std::pair{QStringLiteral("L"), tr("Left (character's)")},
                                      std::pair{QStringLiteral("R"), tr("Right (character's)")}}) {
        QAction *a = side->addAction(text, this, [this, label, value] {
            doc::Part p = *m_set->part(label);
            p.side = value;
            m_set->updatePart(p, tr("Change side"));
        });
        a->setCheckable(true);
        a->setChecked(part.side == value);
    }
    menu.addAction(Icons::get(QStringLiteral("palette")), tr("Colour..."), this, [this, label] {
        const QColor c = QColorDialog::getColor(m_set->part(label)->color, this, tr("Part colour"));
        if (!c.isValid() || !m_set->part(label))
            return;
        doc::Part p = *m_set->part(label);
        p.color = c;
        m_set->updatePart(p, tr("Change colour"));
    });
    menu.addAction(m_solo == label ? tr("Show all parts") : tr("Solo (show only this part)"), this, [this, label] {
        m_solo = m_solo == label ? 0 : label;
        emit soloChanged(m_solo);
    });
    menu.addSeparator();
    menu.addAction(Icons::get(QStringLiteral("chevron-up")), tr("Bring forward"), this, [this, label] { moveInDrawOrder(label, +1); });
    menu.addAction(Icons::get(QStringLiteral("chevron-down")), tr("Send backward"), this, [this, label] { moveInDrawOrder(label, -1); });
    menu.addSeparator();
    QMenu *split = menu.addMenu(Icons::get(QStringLiteral("scissors")), tr("Split"));
    split->addAction(tr("Left / right"), this, [this, label] { emit splitRequested(label, Split::LeftRight); });
    split->addAction(tr("Separate islands"), this, [this, label] { emit splitRequested(label, Split::Islands); });
    split->addAction(tr("By skeleton"), this, [this, label] { emit splitRequested(label, Split::Skeleton); });
    split->addAction(tr("Draw a cut line..."), this, &PartListPanel::cutToolRequested);
    if (sel.size() >= 2)
        menu.addAction(Icons::get(QStringLiteral("merge")), tr("Merge selected"), this, &PartListPanel::mergeSelected);
    if (!part.extensionAuto)
        menu.addAction(tr("Reset hidden area to automatic"), this, [this, label] { m_set->resetExtension(label); });
    menu.addSeparator();
    menu.addAction(Icons::get(QStringLiteral("trash")), sel.size() > 1 ? tr("Delete selected") : tr("Delete"), this,
                   &PartListPanel::deleteSelected);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void PartListPanel::addPart(const QString &tag, const QString &name)
{
    if (!m_set)
        return;
    const int label = m_set->addPart(tag, name);
    if (label == 0) {
        emit m_context->message(tr("A part set holds at most %1 parts.").arg(doc::PartSet::kMaxParts));
        return;
    }
    m_context->setActiveLabel(label);
}

void PartListPanel::renamePart(int label)
{
    if (!m_set || !m_set->part(label))
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename part"), tr("Name:"), QLineEdit::Normal, m_set->part(label)->name, &ok).trimmed();
    if (!ok || name.isEmpty() || !m_set->part(label))
        return;
    doc::Part p = *m_set->part(label);
    p.name = name;
    m_set->updatePart(p, tr("Rename part"));
}

void PartListPanel::moveInDrawOrder(int label, int delta)
{
    if (!m_set)
        return;
    QList<const doc::Part *> order = m_set->partsInDrawOrder();
    QList<int> labels;
    for (const doc::Part *p : order)
        labels.append(p->label);
    const int i = int(labels.indexOf(label));
    const int j = i + delta;
    if (i < 0 || j < 0 || j >= labels.size())
        return;
    labels.swapItemsAt(i, j);
    m_set->reorder(labels);
}

void PartListPanel::mergeSelected()
{
    QList<int> sel = selectedLabels();
    if (!m_set || sel.size() < 2)
        return;
    // The active part survives.
    if (sel.contains(m_context->activeLabel())) {
        sel.removeAll(m_context->activeLabel());
        sel.prepend(m_context->activeLabel());
    }
    m_set->mergeParts(sel);
    m_context->setActiveLabel(sel.first());
}

void PartListPanel::deleteSelected()
{
    const QList<int> sel = selectedLabels();
    if (!m_set || sel.isEmpty())
        return;
    m_context->setActiveLabel(0);
    m_set->removeParts(sel);
}

} // namespace lumen::ui
