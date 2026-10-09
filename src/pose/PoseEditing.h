#pragma once

#include "pose/Skeleton.h"

namespace lumen::pose {

enum class DragMode {
    Free,     // moves only the joint itself
    Rotate,   // forward kinematics: rotates the joint (and everything below) around its parent, keeping bone length
    Chain,    // inverse kinematics: two-bone IK for wrists/ankles, FK fallback elsewhere
    Translate // moves the joint with its whole sub-tree
};

// Moves a joint to `target` according to the drag mode. Hands and face follow their anchor
// joints (wrists, nose) when those move.
void dragJoint(Person &person, const JointRef &ref, const QPointF &target, DragMode mode);

// Rotates the sub-tree below `pivot` (exclusive) around the pivot by `degrees`.
void rotateSubtree(Person &person, int pivot, double degrees);

// Two-bone IK: places `end` at target keeping lengths root-mid and mid-end. `bendSign` keeps the
// knee/elbow on the same side as before when 0.
void solveTwoBone(Person &person, int root, int mid, int end, const QPointF &target);

// Points belonging to a joint's sub-tree: body descendants + attached hand/face points.
QList<JointRef> subtree(const Person &person, int bodyJoint, bool includeSelf);

// Rotates an attached hand (all 21 points) around the wrist.
void rotateHand(Person &person, Part hand, double degrees);
void scaleHand(Person &person, Part hand, double factor);

// Copies arm/leg/hand pose from one side to the other (mirrored around the body centre line).
enum class Side { Left, Right };
void copySide(Person &person, Side from);

// Generates a default hand attached to the wrist (relaxed open hand along the forearm).
void attachDefaultHand(Person &person, Part hand);
void removeHand(Person &person, Part hand);

} // namespace lumen::pose
