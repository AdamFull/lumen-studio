#pragma once

#include "pose/Skeleton.h"

#include <QJsonObject>
#include <QString>

namespace lumen::pose {

// Limb orientation in anatomical terms (degrees).
//  raise:      0 = hanging straight down, 90 = horizontal, 180 = straight up.
//  direction:  where a raised limb points: 0 = forward (toward where the body faces), 90 = out to the
//              character's own side, 180 = backward, -90 = across the body.
//  bend:       elbow/knee flexion, 0 = straight .. ~150 = fully bent.
//  bendTwist:  rotates the bending plane. Arms: 0 = forearm bends forward, +90 = toward the body midline,
//              -90 = away from the body / upward when the arm is out to the side. Legs: 0 = knee bends
//              backward (natural), +90 = shin moves toward the midline.
struct LimbParams
{
    double raise = 0.0;
    double direction = 0.0;
    double bend = 0.0;
    double bendTwist = 0.0;
};

// Parameters of the 3D forward-kinematics generation rig. Designed to be produced by LLMs (all values
// are intuitive angles) and by preset definitions; solved into editable 2D OpenPose keypoints.
struct RigParams
{
    QString name;
    // Whole body orientation. yaw: 0 = facing viewer, + = turns toward the viewer's right
    // (90 = profile facing right, 180 = back view). pitch: + = leans toward viewer. roll: + = tilts to viewer's right.
    double yaw = 0.0, pitch = 0.0, roll = 0.0;
    // Upper body relative to hips. lean: + = bends forward. side: + = bends toward viewer's right. twist: like yaw.
    double torsoLean = 0.0, torsoSide = 0.0, torsoTwist = 0.0;
    // Head relative to torso. yaw: + = looks toward viewer's right. pitch: + = looks down. roll: + = tilts toward viewer's right.
    double headYaw = 0.0, headPitch = 0.0, headRoll = 0.0;
    LimbParams leftArm{8.0, 90.0, 8.0, 0.0};
    LimbParams rightArm{8.0, 90.0, 8.0, 0.0};
    LimbParams leftLeg{2.0, 90.0, 0.0, 0.0};
    LimbParams rightLeg{2.0, 90.0, 0.0, 0.0};
    QString leftHand = QStringLiteral("relaxed");  // see handShapeNames()
    QString rightHand = QStringLiteral("relaxed");
    QString expression = QStringLiteral("neutral"); // see expressionNames()
    // Framing: full_body, knees_up, cowboy_shot, upper_body, bust, headshot.
    QString framing = QStringLiteral("full_body");
    double positionX = 0.5;  // horizontal centre in the canvas (0..1)
    double positionY = 0.0;  // vertical offset as fraction of canvas height (+ = down)
    double scale = 1.0;      // relative size multiplier
    double cameraPitch = 0.0;   // + = camera above looking down
    double perspective = 0.15;  // 0 = orthographic .. 1 = strong perspective
    bool includeHands = true;
    bool includeFace = true;

    QJsonObject toJson() const;
    static RigParams fromJson(const QJsonObject &json);
    static RigParams fromJson(const QJsonObject &json, const RigParams &defaults);
};

QStringList framingNames();

// Solves the rig into a person on the given canvas.
Person solveRig(const RigParams &params, const QSize &canvas);

// Builds a pose with one person per parameter set.
Pose solvePose(const QList<RigParams> &people, const QSize &canvas);

// JSON schema describing RigParams for LLM structured output.
QJsonObject rigParamsSchema();

// Human/LLM-readable documentation of all rig parameters (used in prompts).
QString rigParamsGuide();

} // namespace lumen::pose
