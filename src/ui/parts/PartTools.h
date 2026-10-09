#pragma once

#include "doc/Parts.h"
#include "pose/Skeleton.h"
#include "ui/canvas/Tool.h"

#include <QPointer>
#include <QPolygonF>

#include <optional>

namespace lumen::ui {

// Shared state of the part mask tools: the part set, the active part and tool options.
class PartEditContext : public QObject
{
    Q_OBJECT
public:
    explicit PartEditContext(QObject *parent = nullptr);

    doc::PartSet *set() const { return m_set; }
    void setSet(doc::PartSet *set);
    int activeLabel() const { return m_active; }
    void setActiveLabel(int label);
    const doc::Part *activePart() const { return m_set ? m_set->part(m_active) : nullptr; }

    // Painting only claims pixels of the character silhouette.
    bool insideSilhouette() const { return m_insideSilhouette; }
    void setInsideSilhouette(bool on);
    int wandTolerance() const { return m_tolerance; }
    void setWandTolerance(int t);
    bool wandContiguous() const { return m_contiguous; }
    void setWandContiguous(bool on);

    const std::optional<pose::Person> &skeleton() const { return m_skeleton; }
    void setSkeleton(const std::optional<pose::Person> &person);

    // Assigns (or removes) mask pixels to/from the active part as one undo step, respecting locked
    // parts and the silhouette option. With no active part, removal clears any unlocked part.
    void assign(const QImage &mask, bool remove, const QString &undoText);
    int labelAt(const QPointF &docPos) const;

signals:
    void setChanged();
    void activeLabelChanged(int label);
    void optionsChanged();
    void skeletonChanged();
    void extensionPreview(const QImage &mask, const QRect &dirty); // live extension painting
    void message(const QString &text);

private:
    QPointer<doc::PartSet> m_set;
    int m_active = 0;
    bool m_insideSilhouette = true;
    int m_tolerance = 28;
    bool m_contiguous = true;
    std::optional<pose::Person> m_skeleton;
};

class PartToolBase : public Tool
{
    Q_OBJECT
public:
    PartToolBase(EditorState *state, PartEditContext *context, QObject *parent = nullptr);

protected:
    PartEditContext *context() const { return m_context; }
    // Ctrl+click selects the part under the cursor. Returns true when handled.
    bool pickWithCtrl(const ToolEvent &ev);
    bool ensureActive();

private:
    PartEditContext *m_context;
};

// Paints part membership (Paint), removes it (Erase) or paints the extension area (Extend).
class PartBrushTool : public PartToolBase
{
    Q_OBJECT
public:
    enum class Mode { Paint, Erase, Extend };
    PartBrushTool(EditorState *state, PartEditContext *context, Mode mode, QObject *parent = nullptr);

    QString id() const override;
    QString name() const override;
    QString iconName() const override;
    QString shortcut() const override;
    QString hint() const override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    bool key(CanvasView *view, QKeyEvent *e) override;
    double brushRadius() const override;
    QWidget *createOptions(QWidget *parent) override;

private:
    void stampTo(const QPointF &pos);
    void stamp(const QPointF &pos);

    Mode m_mode;
    bool m_active = false;
    bool m_remove = false;
    QImage m_before;     // label map before the stroke
    QImage m_extension;  // working extension mask (Extend)
    QRect m_dirty;
    QPointF m_last;
    QVector<bool> m_locked;
};

class PartLassoTool : public PartToolBase
{
    Q_OBJECT
public:
    PartLassoTool(EditorState *state, PartEditContext *context, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("part-lasso"); }
    QString name() const override { return tr("Lasso"); }
    QString iconName() const override { return QStringLiteral("lasso"); }
    QString shortcut() const override { return QStringLiteral("L"); }
    QString hint() const override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;
    QWidget *createOptions(QWidget *parent) override;

private:
    QPolygonF m_points;
    bool m_active = false;
    bool m_remove = false;
};

class PartWandTool : public PartToolBase
{
    Q_OBJECT
public:
    PartWandTool(EditorState *state, PartEditContext *context, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("part-wand"); }
    QString name() const override { return tr("Magic wand"); }
    QString iconName() const override { return QStringLiteral("wand"); }
    QString shortcut() const override { return QStringLiteral("W"); }
    QString hint() const override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    QWidget *createOptions(QWidget *parent) override;
};

// Draw a line across a part to cut it into separate parts.
class PartCutTool : public PartToolBase
{
    Q_OBJECT
public:
    PartCutTool(EditorState *state, PartEditContext *context, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("part-cut"); }
    QString name() const override { return tr("Cut line"); }
    QString iconName() const override { return QStringLiteral("scissors"); }
    QString shortcut() const override { return QStringLiteral("C"); }
    QString hint() const override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;

private:
    QPolygonF m_points;
    bool m_active = false;
    bool m_busy = false;
};

// Click a part to select it; hover shows its name.
class PartPickTool : public PartToolBase
{
    Q_OBJECT
public:
    PartPickTool(EditorState *state, PartEditContext *context, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("part-pick"); }
    QString name() const override { return tr("Select part"); }
    QString iconName() const override { return QStringLiteral("target"); }
    QString shortcut() const override { return QStringLiteral("V"); }
    QString hint() const override;
    QCursor cursor() const override { return Qt::PointingHandCursor; }
    void press(CanvasView *view, const ToolEvent &ev) override;
    void hover(CanvasView *view, const ToolEvent &ev) override;

signals:
    void hovered(int label);
};

// Edits the body skeleton used by the skeleton segmenter and skeleton splits.
class SkeletonTool : public PartToolBase
{
    Q_OBJECT
public:
    SkeletonTool(EditorState *state, PartEditContext *context, QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("part-skeleton"); }
    QString name() const override { return tr("Skeleton"); }
    QString iconName() const override { return QStringLiteral("person"); }
    QString shortcut() const override { return QStringLiteral("K"); }
    QString hint() const override;
    QCursor cursor() const override { return Qt::ArrowCursor; }
    void activated(CanvasView *view) override;
    void press(CanvasView *view, const ToolEvent &ev) override;
    void move(CanvasView *view, const ToolEvent &ev) override;
    void release(CanvasView *view, const ToolEvent &ev) override;
    void paintOverlay(CanvasView *view, QPainter &p) override;

private:
    int jointAt(CanvasView *view, const QPointF &widgetPos) const;
    int m_joint = -1;
    bool m_translate = false;
    QPointF m_last;
};

// Paints the skeleton (bones and joints) in document coordinates.
void paintSkeleton(QPainter &p, const pose::Person &person, double zoom, int highlightJoint = -1);

} // namespace lumen::ui
