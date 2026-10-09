#include "pose/PoseEditing.h"

#include "pose/Anatomy.h"

#include <QLineF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace lumen::pose {

namespace {

double angleOf(const QPointF &v)
{
    return std::atan2(v.y(), v.x()) * 180.0 / M_PI;
}

double length(const QPointF &v)
{
    return std::hypot(v.x(), v.y());
}

QPointF rotateAround(const QPointF &p, const QPointF &c, double degrees)
{
    const double a = degrees * M_PI / 180.0;
    const double cs = std::cos(a), sn = std::sin(a);
    const QPointF d = p - c;
    return c + QPointF(d.x() * cs - d.y() * sn, d.x() * sn + d.y() * cs);
}

int handParent(int i)
{
    if (i <= 0)
        return -1;
    return (i - 1) % 4 == 0 ? 0 : i - 1;
}

QList<int> handDescendants(int i)
{
    QList<int> out;
    if (i == 0) {
        for (int k = 1; k < kHandCount; ++k)
            out.append(k);
        return out;
    }
    // Remaining joints of the same finger.
    const int fingerEnd = ((i - 1) / 4) * 4 + 4;
    for (int k = i + 1; k <= fingerEnd; ++k)
        out.append(k);
    return out;
}

QList<Keypoint> *handOf(Person &p, Part part)
{
    return part == Part::LeftHand ? &p.leftHand : (part == Part::RightHand ? &p.rightHand : nullptr);
}

void moveRefs(Person &person, const QList<JointRef> &refs, const QPointF &delta)
{
    for (const JointRef &r : refs) {
        if (Keypoint *k = person.point(r))
            k->pos += delta;
    }
}

void rotateRefs(Person &person, const QList<JointRef> &refs, const QPointF &center, double degrees)
{
    for (const JointRef &r : refs) {
        if (Keypoint *k = person.point(r))
            k->pos = rotateAround(k->pos, center, degrees);
    }
}

// Re-attaches the hand to its wrist after the forearm changed: translate with the wrist and rotate by
// the change of forearm direction.
void followHand(Person &person, Part hand, const QPointF &oldElbow, const QPointF &oldWrist)
{
    QList<Keypoint> *pts = handOf(person, hand);
    if (!pts || pts->size() != kHandCount)
        return;
    const int elbow = hand == Part::LeftHand ? LElbow : RElbow;
    const int wrist = hand == Part::LeftHand ? LWrist : RWrist;
    const QPointF newElbow = person.body[elbow].pos;
    const QPointF newWrist = person.body[wrist].pos;
    const double delta = angleOf(newWrist - newElbow) - angleOf(oldWrist - oldElbow);
    for (Keypoint &k : *pts)
        k.pos = rotateAround(k.pos + (newWrist - oldWrist), newWrist, delta);
}

} // namespace

QList<JointRef> subtree(const Person &person, int bodyJoint, bool includeSelf)
{
    QList<JointRef> refs;
    QList<int> joints = bodyDescendants(bodyJoint);
    if (includeSelf)
        joints.prepend(bodyJoint);
    for (int j : joints)
        refs.append({Part::Body, j});
    auto addAll = [&refs](Part part, const QList<Keypoint> &list) {
        for (int i = 0; i < list.size(); ++i)
            refs.append({part, i});
    };
    if (joints.contains(LWrist))
        addAll(Part::LeftHand, person.leftHand);
    if (joints.contains(RWrist))
        addAll(Part::RightHand, person.rightHand);
    if (joints.contains(Nose))
        addAll(Part::Face, person.face);
    return refs;
}

void rotateSubtree(Person &person, int pivot, double degrees)
{
    rotateRefs(person, subtree(person, pivot, false), person.body[static_cast<size_t>(pivot)].pos, degrees);
}

void solveTwoBone(Person &person, int root, int mid, int end, const QPointF &target)
{
    const QPointF r = person.body[static_cast<size_t>(root)].pos;
    const QPointF m = person.body[static_cast<size_t>(mid)].pos;
    const QPointF e = person.body[static_cast<size_t>(end)].pos;
    const double a = length(m - r);
    const double b = length(e - m);
    if (a < 1e-6 || b < 1e-6)
        return;
    // Keep the bend on the same side as before.
    const QPointF rm = m - r, re = e - r;
    const double cross = rm.x() * re.y() - rm.y() * re.x();
    const double side = cross >= 0 ? -1.0 : 1.0;

    QPointF toTarget = target - r;
    double d = length(toTarget);
    const double minD = std::abs(a - b) + 1e-3;
    const double maxD = a + b - 1e-3;
    if (d < 1e-6) {
        toTarget = re;
        d = length(re);
    }
    const double clamped = std::clamp(d, minD, maxD);
    const QPointF dir = toTarget / d;
    const double cosA = std::clamp((a * a + clamped * clamped - b * b) / (2.0 * a * clamped), -1.0, 1.0);
    const double angA = std::acos(cosA) * side;
    const double cs = std::cos(angA), sn = std::sin(angA);
    const QPointF midDir(dir.x() * cs - dir.y() * sn, dir.x() * sn + dir.y() * cs);
    const QPointF newMid = r + midDir * a;
    const QPointF newEnd = r + dir * clamped;
    person.body[static_cast<size_t>(mid)].pos = newMid;
    person.body[static_cast<size_t>(end)].pos = newEnd;
}

void dragJoint(Person &person, const JointRef &ref, const QPointF &target, DragMode mode)
{
    Keypoint *kp = person.point(ref);
    if (!kp)
        return;

    if (ref.part == Part::Face) {
        kp->pos = target;
        return;
    }

    if (ref.part == Part::LeftHand || ref.part == Part::RightHand) {
        QList<Keypoint> *hand = handOf(person, ref.part);
        if (ref.index == 0) {
            // Dragging the hand root moves the whole hand; the wrist follows.
            const QPointF delta = target - kp->pos;
            for (Keypoint &k : *hand)
                k.pos += delta;
            const int wrist = ref.part == Part::LeftHand ? LWrist : RWrist;
            person.body[static_cast<size_t>(wrist)].pos = hand->at(0).pos;
            return;
        }
        if (mode == DragMode::Free) {
            kp->pos = target;
            return;
        }
        const int parent = handParent(ref.index);
        const QPointF pivot = hand->at(parent).pos;
        if (mode == DragMode::Translate) {
            const QPointF delta = target - kp->pos;
            kp->pos = target;
            for (int d : handDescendants(ref.index))
                (*hand)[d].pos += delta;
            return;
        }
        const double delta = angleOf(target - pivot) - angleOf(kp->pos - pivot);
        kp->pos = rotateAround(kp->pos, pivot, delta);
        for (int d : handDescendants(ref.index))
            (*hand)[d].pos = rotateAround(hand->at(d).pos, pivot, delta);
        return;
    }

    const int j = ref.index;
    const QPointF oldLElbow = person.body[LElbow].pos, oldLWrist = person.body[LWrist].pos;
    const QPointF oldRElbow = person.body[RElbow].pos, oldRWrist = person.body[RWrist].pos;
    auto syncHands = [&] {
        followHand(person, Part::LeftHand, oldLElbow, oldLWrist);
        followHand(person, Part::RightHand, oldRElbow, oldRWrist);
    };

    switch (mode) {
    case DragMode::Free: {
        const QPointF delta = target - kp->pos;
        kp->pos = target;
        // Attached parts follow their anchor.
        if (j == LWrist || j == RWrist) {
            QList<Keypoint> *hand = handOf(person, j == LWrist ? Part::LeftHand : Part::RightHand);
            for (Keypoint &k : *hand)
                k.pos += delta;
        } else if (j == Nose) {
            for (Keypoint &k : person.face)
                k.pos += delta;
        }
        return;
    }
    case DragMode::Translate:
        moveRefs(person, subtree(person, j, true), target - kp->pos);
        return;
    case DragMode::Chain:
        if (j == LWrist || j == RWrist || j == LAnkle || j == RAnkle) {
            const int mid = bodyParent(j);
            const int root = bodyParent(mid);
            solveTwoBone(person, root, mid, j, target);
            syncHands();
            return;
        }
        [[fallthrough]];
    case DragMode::Rotate: {
        const int parent = bodyParent(j);
        if (parent < 0) {
            moveRefs(person, subtree(person, j, true), target - kp->pos);
            return;
        }
        const QPointF pivot = person.body[static_cast<size_t>(parent)].pos;
        if (length(target - pivot) < 1e-6)
            return;
        const double delta = angleOf(target - pivot) - angleOf(kp->pos - pivot);
        rotateRefs(person, subtree(person, j, true), pivot, delta);
        return;
    }
    }
}

void rotateHand(Person &person, Part hand, double degrees)
{
    QList<Keypoint> *pts = handOf(person, hand);
    if (!pts || pts->isEmpty())
        return;
    const QPointF c = pts->at(0).pos;
    for (Keypoint &k : *pts)
        k.pos = rotateAround(k.pos, c, degrees);
}

void scaleHand(Person &person, Part hand, double factor)
{
    QList<Keypoint> *pts = handOf(person, hand);
    if (!pts || pts->isEmpty())
        return;
    const QPointF c = pts->at(0).pos;
    for (Keypoint &k : *pts)
        k.pos = c + (k.pos - c) * factor;
}

void copySide(Person &person, Side from)
{
    const double axis = person.body[Neck].visible ? person.body[Neck].pos.x() : person.bounds().center().x();
    auto mirrorPt = [axis](QPointF p) { return QPointF(2.0 * axis - p.x(), p.y()); };
    const int left[] = {LShoulder, LElbow, LWrist, LHip, LKnee, LAnkle, LEye, LEar};
    const int right[] = {RShoulder, RElbow, RWrist, RHip, RKnee, RAnkle, REye, REar};
    for (size_t i = 0; i < std::size(left); ++i) {
        const int src = from == Side::Left ? left[i] : right[i];
        const int dst = from == Side::Left ? right[i] : left[i];
        Keypoint k = person.body[static_cast<size_t>(src)];
        k.pos = mirrorPt(k.pos);
        person.body[static_cast<size_t>(dst)] = k;
    }
    QList<Keypoint> &srcHand = from == Side::Left ? person.leftHand : person.rightHand;
    QList<Keypoint> &dstHand = from == Side::Left ? person.rightHand : person.leftHand;
    dstHand = srcHand;
    for (Keypoint &k : dstHand)
        k.pos = mirrorPt(k.pos);
}

void attachDefaultHand(Person &person, Part hand)
{
    QList<Keypoint> *pts = handOf(person, hand);
    if (!pts)
        return;
    const bool left = hand == Part::LeftHand;
    const Keypoint &elbow = person.body[left ? LElbow : RElbow];
    const Keypoint &wrist = person.body[left ? LWrist : RWrist];
    if (!elbow.visible || !wrist.visible)
        return;
    QPointF axis = wrist.pos - elbow.pos;
    const double forearm = length(axis);
    if (forearm < 1e-3)
        return;
    axis /= forearm;
    // Palm facing the viewer: thumb toward screen-right for the left hand when the arm hangs down.
    const QPointF thumbDir = left ? QPointF(axis.y(), -axis.x()) : QPointF(-axis.y(), axis.x());
    const double handLen = forearm * 0.72;
    const auto tpl = handTemplate(HandShape::Relaxed);
    pts->clear();
    for (const QPointF &t : tpl) {
        Keypoint k;
        k.pos = wrist.pos + axis * (t.x() * handLen) + thumbDir * (t.y() * handLen);
        k.visible = true;
        pts->append(k);
    }
}

void removeHand(Person &person, Part hand)
{
    if (QList<Keypoint> *pts = handOf(person, hand))
        pts->clear();
}

} // namespace lumen::pose
