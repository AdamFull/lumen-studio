#pragma once

#include "doc/Parts.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace lumen::ui {

class PartEditContext;

// Part list of the slicing workspace: grouped by region, with visibility, lock, state, coverage
// and the structural actions (add, rename, kind, side, merge, split, delete, draw order).
class PartListPanel : public QWidget
{
    Q_OBJECT
public:
    enum class Split { LeftRight, Islands, Skeleton };

    PartListPanel(PartEditContext *context, QWidget *parent = nullptr);

    QList<int> selectedLabels() const;

signals:
    void splitRequested(int label, lumen::ui::PartListPanel::Split mode);
    void generateRequested(const QList<int> &labels);
    void cutToolRequested();
    void showUnassignedChanged(bool show);
    void soloChanged(int label); // 0 = none

private:
    void rebuild();
    void updateStates();
    void syncSelection();
    void showContextMenu(const QPoint &pos);
    QMenu *buildAddMenu(QWidget *parent);
    QMenu *buildKindMenu(QWidget *parent, int label);
    void addPart(const QString &tag, const QString &name = QString());
    void renamePart(int label);
    void moveInDrawOrder(int label, int delta);
    void mergeSelected();
    void deleteSelected();
    QString stateText(const doc::Part &p) const;
    void showState(QTreeWidgetItem *item, const doc::Part &p) const;

    PartEditContext *m_context;
    QPointer<doc::PartSet> m_set;
    QTreeWidget *m_tree;
    QLabel *m_unassigned;
    QToolButton *m_showUnassigned;
    QToolButton *m_attention;
    QToolButton *m_merge;
    QToolButton *m_split;
    QToolButton *m_delete;
    bool m_syncing = false;
    int m_solo = 0;
};

} // namespace lumen::ui
