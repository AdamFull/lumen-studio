#pragma once

#include <QWidget>

class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lumen::ui {

// Settings page listing every registered shortcut (Shortcuts registry) with a primary and an
// alternative key. Assigning a key that is taken in the same scope moves it (the other action
// loses it) and says so. Changes apply immediately.
class ShortcutsPage : public QWidget
{
    Q_OBJECT
public:
    explicit ShortcutsPage(QWidget *parent = nullptr);

    // Assigns keys to an action (primary, alternative); returns the ids that lost a key.
    QStringList assign(const QString &id, const QKeySequence &primary, const QKeySequence &alternative);
    void select(const QString &id);
    QString selectedId() const { return m_current; }

private:
    void rebuild();
    void filter(const QString &text);
    void showCurrent();
    void refreshItem(QTreeWidgetItem *item);
    void commitEdits();

    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QLabel *m_title;
    QKeySequenceEdit *m_primary;
    QKeySequenceEdit *m_alternative;
    QPushButton *m_clear;
    QPushButton *m_reset;
    QPushButton *m_resetAll;
    QLabel *m_message;
    QString m_current;
    bool m_updating = false;
};

} // namespace lumen::ui
