#pragma once

#include "live2d/Animation.h"
#include "live2d/CubismModel.h"
#include "live2d/Motion.h"
#include "live2d/Physics.h"

#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <optional>

namespace lumen::ui {

// Preview of a rigged Live2D Cubism model (.model3.json) using the user's Cubism Core library and
// Lumen's software renderer. Animates the standard parameters (angles, breath, blink, lip-sync),
// plays the model's idle motion, simulates its physics (physics3.json) and follows the mouse.
class CubismView : public QWidget
{
    Q_OBJECT
public:
    explicit CubismView(QWidget *parent = nullptr);
    ~CubismView() override;

    bool loadModel(const QString &model3Path, QString *error = nullptr);
    void clear();
    bool hasModel() const { return m_model != nullptr; }
    live2d::CubismModel *model() const { return m_model.get(); }
    QString modelName() const { return m_name; }
    bool hasIdleMotion() const;
    bool hasPose() const { return m_pose.has_value(); }

    // Motion groups of the model ("Idle" first) and their motions.
    QStringList motionGroups() const;
    int motionCount(const QString &group) const;
    QString motionLabel(const QString &group, int index) const;
    // Plays a motion once (crossfading from the current one); idle motions resume afterwards.
    bool playMotion(const QString &group, int index, QString *error = nullptr);
    void stopMotions();
    QString currentMotionGroup() const { return m_motions.currentGroup(); }
    int currentMotionIndex() const { return m_motions.currentIndex(); }

    QStringList expressionNames() const;
    // Crossfades to the named expression; an empty name clears it.
    bool setExpression(const QString &name, QString *error = nullptr);
    QString currentExpression() const { return m_expressions.current(); }
    bool hasPhysics() const { return m_physics.has_value(); }
    const std::optional<live2d::Physics> &physics() const { return m_physics; }
    QString physicsError() const { return m_physicsError; }

    void setAutoIdle(bool on);
    void setFollowMouse(bool on);
    void setTalking(bool on);
    // Plays the Idle group (random motion after motion) whenever nothing else is playing.
    void setPlayMotion(bool on);
    void setPhysicsEnabled(bool on);
    // Wind for the physics (x, y in physics units; 0 = still air).
    void setWind(const QPointF &wind);
    // A manual value overrides the animation of that parameter until cleared.
    void setManual(int parameterIndex, float value);
    void clearManual();
    float value(int parameterIndex) const;
    // Synchronous render at `size` (screenshots, tests).
    QImage renderNow(const QSize &size);

signals:
    void modelChanged();
    void motionChanged(const QString &group, int index); // index -1: nothing playing
    void framePresented();

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    void tick();
    void applyAnimation(double t, double dt);
    void requestRender();
    void updateTimer();

    std::unique_ptr<live2d::CubismModel> m_model;
    std::shared_ptr<const live2d::Motion> motion(const QString &group, int index, QString *error);
    void startIdle(double now);

    live2d::MotionPlayer m_motions;
    live2d::ExpressionPlayer m_expressions;
    std::optional<live2d::Pose> m_pose;
    QHash<QString, std::shared_ptr<const live2d::Motion>> m_motionCache;      // by file
    QHash<QString, std::shared_ptr<const live2d::Expression>> m_expressionCache; // by name
    QString m_lastGroup;
    int m_lastIndex = -2;
    int m_lastIdle = -1;
    std::optional<live2d::Physics> m_physics;
    QString m_physicsError;
    bool m_physicsEnabled = true;
    QPointF m_wind;
    QString m_name;
    QHash<int, float> m_manual;
    QImage m_image;
    bool m_rendering = false;
    bool m_dirty = false;
    quint64 m_generation = 0;
    bool m_autoIdle = true;
    bool m_follow = false;
    bool m_talking = false;
    bool m_playMotion = true;
    QTimer m_timer;
    QElapsedTimer m_clock;
    double m_lastT = 0;
    double m_nextBlink = 2.0;
    double m_blinkStart = -1;
    // Smoothed procedural values.
    double m_angleX = 0, m_angleY = 0, m_angleZ = 0, m_bodyX = 0, m_eyeX = 0, m_eyeY = 0;
    QPointF m_mouse{-1, -1};
    double m_zoom = 1.0;
    QPointF m_pan;
    QPointF m_dragStart;
    bool m_dragging = false;
};

} // namespace lumen::ui
