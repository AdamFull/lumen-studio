#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <array>

// Procedural templates for hands (21 OpenPose points) and faces (70 OpenPose points).
namespace lumen::pose {

enum class HandShape { Relaxed, Open, Fist, Point, Peace, ThumbsUp, Grip };

QStringList handShapeNames();
QString handShapeName(HandShape shape);
HandShape handShapeFromName(const QString &name, HandShape fallback = HandShape::Relaxed);

// Hand points in a local 2D frame: u along the hand axis (wrist → middle finger), v toward the thumb.
// Unit = hand length (wrist to middle fingertip of an open hand ≈ 1).
std::array<QPointF, 21> handTemplate(HandShape shape);

enum class Expression { Neutral, Smile, OpenMouth, Surprised, ClosedEyes, WinkLeft, WinkRight, Angry, Sad };

QStringList expressionNames();
QString expressionName(Expression e);
Expression expressionFromName(const QString &name, Expression fallback = Expression::Neutral);

// Face landmarks (dlib-68 + 2 pupils) in a head-local frame: x toward the character's left,
// y up, z toward where the face looks. x unit = half face width, y unit = see kFaceYScale.
std::array<QVector3D, 70> faceTemplate(Expression e);

// Body keypoints of the head in the same head-local frame.
QVector3D headNoseLocal();
QVector3D headEyeLocal(bool left);
QVector3D headEarLocal(bool left);

} // namespace lumen::pose
