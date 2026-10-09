#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>

#include <array>
#include <optional>

// OpenPose-compatible skeleton model (COCO-18 body, 21-point hands, 70-point face).
// Coordinates are pixels in the pose canvas (Pose::canvas), y pointing down.
namespace lumen::pose {

constexpr int kBodyCount = 18;
constexpr int kHandCount = 21;
constexpr int kFaceCount = 70;

enum BodyJoint {
    Nose = 0, Neck, RShoulder, RElbow, RWrist, LShoulder, LElbow, LWrist,
    RHip, RKnee, RAnkle, LHip, LKnee, LAnkle, REye, LEye, REar, LEar
};

enum class Part { Body, LeftHand, RightHand, Face };

struct Keypoint
{
    QPointF pos;
    bool visible = false;
    bool operator==(const Keypoint &o) const { return pos == o.pos && visible == o.visible; }
};

struct JointRef
{
    Part part = Part::Body;
    int index = -1;
    bool isValid() const { return index >= 0; }
    bool operator==(const JointRef &o) const { return part == o.part && index == o.index; }
    bool operator!=(const JointRef &o) const { return !(*this == o); }
};

struct Person
{
    std::array<Keypoint, kBodyCount> body;
    QList<Keypoint> leftHand;  // empty or kHandCount
    QList<Keypoint> rightHand; // empty or kHandCount
    QList<Keypoint> face;      // empty or kFaceCount
    QString label;

    Keypoint *point(const JointRef &ref);
    const Keypoint *point(const JointRef &ref) const;
    QList<JointRef> allJoints(bool visibleOnly = true) const;
    QRectF bounds() const;
    bool isEmpty() const;
    void translate(const QPointF &delta);
    void transform(const QPointF &center, double scale, double rotationDeg);
    void scaleAxes(double sx, double sy);
    // Mirrors horizontally around x = axisX, swapping left/right semantics so the result stays anatomically valid.
    void mirror(double axisX);
    bool operator==(const Person &o) const;
};

struct Pose
{
    QSize canvas{512, 768};
    QList<Person> people;

    bool isEmpty() const;
    // Changes canvas size; when scalePoints is true keypoints are scaled to keep their relative positions.
    void resizeCanvas(const QSize &size, bool scalePoints);

    // OpenPose JSON ("people": [{pose_keypoints_2d, hand_left_keypoints_2d, hand_right_keypoints_2d,
    // face_keypoints_2d}], canvas_width, canvas_height). Pixel coordinates.
    QJsonObject toOpenPoseJson() const;
    static std::optional<Pose> fromOpenPoseJson(const QJsonObject &json, QString *error = nullptr,
                                                const QSize &fallbackCanvas = QSize(512, 512));
    bool operator==(const Pose &o) const { return canvas == o.canvas && people == o.people; }
};

QString bodyJointName(int index);
QString jointName(const JointRef &ref);
// Parent in the editing hierarchy (Neck is the root; returns -1 for Neck).
int bodyParent(int index);
// Children list (direct) in the editing hierarchy.
QList<int> bodyChildren(int index);
// All descendants (body joints) of index.
QList<int> bodyDescendants(int index);
// Joint index on the opposite side (e.g. LShoulder <-> RShoulder); identity for centre joints.
int mirroredBodyJoint(int index);
// Mirrored index for 70-point face landmarks.
int mirroredFacePoint(int index);

// OpenPose rendering conventions (controlnet_aux).
const QList<QPair<int, int>> &bodyLimbs();   // 17 limbs, 0-based
QColor bodyColor(int index);                 // 18 colours (limb i and joint i)
const QList<QPair<int, int>> &handEdges();   // 20 edges
QColor handEdgeColor(int edgeIndex);

} // namespace lumen::pose
