#pragma once

#include "pose/PoseEditing.h"
#include "pose/Skeleton.h"

#include <QImage>
#include <QWidget>

namespace lumen::ui {

// Interactive OpenPose skeleton editor surface.
class PoseCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit PoseCanvas(QWidget *parent = nullptr);

    const pose::Pose &pose() const { return m_pose; }
    void setPose(const pose::Pose &pose);
    int selectedPerson() const { return m_person; }
    void setSelectedPerson(int index);
    void setDragMode(pose::DragMode mode) { m_mode = mode; }
    pose::DragMode dragMode() const { return m_mode; }
    void setUnderlay(const QImage &image);
    void setUnderlayOpacity(double opacity);
    void setShowFace(bool on);
    void setShowHands(bool on);

signals:
    // Emitted continuously while dragging (live) and once when an edit gesture ends (commit).
    void poseEdited(const lumen::pose::Pose &pose);
    void editFinished(const lumen::pose::Pose &before, const lumen::pose::Pose &after);
    void personSelected(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    QRectF canvasRect() const;
    QPointF toCanvas(const QPointF &widget) const;
    QPointF toWidget(const QPointF &canvas) const;
    struct Hit
    {
        int person = -1;
        pose::JointRef joint;
    };
    Hit hitTest(const QPointF &widgetPos) const;

    pose::Pose m_pose;
    pose::Pose m_before;
    int m_person = 0;
    Hit m_hover;
    Hit m_drag;
    bool m_dragPerson = false;
    QPointF m_lastCanvasPos;
    pose::DragMode m_mode = pose::DragMode::Rotate;
    QImage m_underlay;
    double m_underlayOpacity = 0.5;
    bool m_showFace = true;
    bool m_showHands = true;
};

} // namespace lumen::ui
