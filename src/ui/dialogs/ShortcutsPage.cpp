#include "ui/dialogs/ShortcutsPage.h"

#include "ui/Shortcuts.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {
constexpr int kIdRole = Qt::UserRole;
}

ShortcutsPage::ShortcutsPage(QWidget *parent)
    : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(8);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search actions or keys…"));
    m_search->setClearButtonEnabled(true);
    v->addWidget(m_search);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Action"), tr("Shortcut")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    v->addWidget(m_tree, 1);

    auto *editor = new QWidget(this);
    editor->setObjectName(QStringLiteral("Panel"));
    auto *g = new QGridLayout(editor);
    g->setContentsMargins(10, 8, 10, 8);
    m_title = new QLabel(editor);
    m_title->setTextFormat(Qt::PlainText);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    g->addWidget(m_title, 0, 0, 1, 4);
    g->addWidget(new QLabel(tr("Shortcut"), editor), 1, 0);
    m_primary = new QKeySequenceEdit(editor);
    m_primary->setMaximumSequenceLength(1);
    g->addWidget(m_primary, 1, 1);
    g->addWidget(new QLabel(tr("Alternative"), editor), 1, 2);
    m_alternative = new QKeySequenceEdit(editor);
    m_alternative->setMaximumSequenceLength(1);
    g->addWidget(m_alternative, 1, 3);
    auto *buttons = new QHBoxLayout;
    m_clear = new QPushButton(tr("Clear"), editor);
    m_reset = new QPushButton(tr("Reset to default"), editor);
    m_resetAll = new QPushButton(tr("Reset all…"), editor);
    buttons->addWidget(m_clear);
    buttons->addWidget(m_reset);
    buttons->addStretch();
    buttons->addWidget(m_resetAll);
    g->addLayout(buttons, 2, 0, 1, 4);
    m_message = mutedLabel(QString(), editor);
    m_message->setWordWrap(true);
    g->addWidget(m_message, 3, 0, 1, 4);
    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);
    v->addWidget(editor);

    connect(m_search, &QLineEdit::textChanged, this, &ShortcutsPage::filter);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_current = item ? item->data(0, kIdRole).toString() : QString();
        m_message->clear();
        showCurrent();
    });
    connect(m_primary, &QKeySequenceEdit::editingFinished, this, &ShortcutsPage::commitEdits);
    connect(m_alternative, &QKeySequenceEdit::editingFinished, this, &ShortcutsPage::commitEdits);
    connect(m_clear, &QPushButton::clicked, this, [this] {
        if (!m_current.isEmpty())
            assign(m_current, {}, {});
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        if (m_current.isEmpty())
            return;
        const auto defaults = Shortcuts::instance().info(m_current).defaults;
        assign(m_current, defaults.value(0), defaults.value(1));
    });
    connect(m_resetAll, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset shortcuts"), tr("Restore every shortcut to its default?")) != QMessageBox::Yes)
            return;
        Shortcuts::instance().resetAll();
        m_message->setText(tr("All shortcuts were reset."));
    });
    connect(&Shortcuts::instance(), &Shortcuts::changed, this, [this](const QString &id) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *group = m_tree->topLevelItem(i);
            for (int j = 0; j < group->childCount(); ++j) {
                if (group->child(j)->data(0, kIdRole).toString() == id)
                    refreshItem(group->child(j));
            }
        }
        if (id == m_current)
            showCurrent();
    });
    rebuild();
}

void ShortcutsPage::rebuild()
{
    m_tree->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    for (const Shortcuts::Info &info : Shortcuts::instance().all()) {
        QTreeWidgetItem *group = groups.value(info.category);
        if (!group) {
            group = new QTreeWidgetItem(m_tree, {info.category});
            group->setFlags(Qt::ItemIsEnabled);
            group->setExpanded(true);
            QFont f = group->font(0);
            f.setBold(true);
            group->setFont(0, f);
            groups.insert(info.category, group);
        }
        auto *item = new QTreeWidgetItem(group, {info.name});
        item->setData(0, kIdRole, info.id);
        refreshItem(item);
    }
    showCurrent();
}

void ShortcutsPage::refreshItem(QTreeWidgetItem *item)
{
    const QString id = item->data(0, kIdRole).toString();
    item->setText(1, Shortcuts::toText(Shortcuts::instance().keys(id)));
    QFont f = item->font(1);
    f.setItalic(Shortcuts::instance().isCustomized(id));
    item->setFont(1, f);
    item->setToolTip(1, Shortcuts::instance().isCustomized(id) ? tr("Customised (default: %1)")
                                                                     .arg(Shortcuts::toText(Shortcuts::instance().info(id).defaults))
                                                               : QString());
}

void ShortcutsPage::filter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < group->childCount(); ++j) {
            QTreeWidgetItem *item = group->child(j);
            const bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive)
                               || item->text(1).contains(needle, Qt::CaseInsensitive) || group->text(0).contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            visible += match ? 1 : 0;
        }
        group->setHidden(visible == 0);
    }
}

void ShortcutsPage::select(const QString &id)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *group = m_tree->topLevelItem(i);
        for (int j = 0; j < group->childCount(); ++j) {
            if (group->child(j)->data(0, kIdRole).toString() == id)
                m_tree->setCurrentItem(group->child(j));
        }
    }
}

void ShortcutsPage::showCurrent()
{
    const bool has = !m_current.isEmpty() && Shortcuts::instance().contains(m_current);
    m_updating = true;
    m_primary->setEnabled(has);
    m_alternative->setEnabled(has);
    m_clear->setEnabled(has);
    m_reset->setEnabled(has && Shortcuts::instance().isCustomized(m_current));
    if (has) {
        const Shortcuts::Info info = Shortcuts::instance().info(m_current);
        const auto keys = Shortcuts::instance().keys(m_current);
        m_title->setText(QStringLiteral("%1 — %2").arg(info.category, info.name));
        m_primary->setKeySequence(keys.value(0));
        m_alternative->setKeySequence(keys.value(1));
    } else {
        m_title->setText(tr("Select an action to change its shortcut."));
        m_primary->clear();
        m_alternative->clear();
    }
    m_updating = false;
}

void ShortcutsPage::commitEdits()
{
    if (m_updating || m_current.isEmpty())
        return;
    const auto keys = Shortcuts::instance().keys(m_current);
    if (m_primary->keySequence() == keys.value(0) && m_alternative->keySequence() == keys.value(1))
        return;
    assign(m_current, m_primary->keySequence(), m_alternative->keySequence());
}

QStringList ShortcutsPage::assign(const QString &id, const QKeySequence &primary, const QKeySequence &alternative)
{
    Shortcuts &sc = Shortcuts::instance();
    QStringList moved;
    for (const QKeySequence &key : {primary, alternative}) {
        for (const QString &other : sc.conflicts(id, key)) {
            QList<QKeySequence> rest = sc.keys(other);
            rest.removeAll(key);
            sc.setKeys(other, rest);
            moved << other;
        }
    }
    QList<QKeySequence> keys;
    if (!primary.isEmpty())
        keys << primary;
    if (!alternative.isEmpty() && alternative != primary)
        keys << alternative;
    sc.setKeys(id, keys);
    if (moved.isEmpty()) {
        m_message->setText(keys.isEmpty() ? tr("No shortcut.") : QString());
    } else {
        QStringList names;
        for (const QString &other : std::as_const(moved))
            names << QStringLiteral("%1 — %2").arg(sc.info(other).category, sc.info(other).name);
        m_message->setText(tr("The key was taken from: %1.").arg(names.join(QStringLiteral("; "))));
    }
    showCurrent();
    return moved;
}

} // namespace lumen::ui
