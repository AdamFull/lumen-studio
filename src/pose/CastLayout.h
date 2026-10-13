#pragma once

#include "pose/Rig.h"
#include "pose/Skeleton.h"

#include <QImage>
#include <QList>
#include <QRect>

namespace lumen::pose {

// Two (or more) characters in one image (M16 §16.2): where each person stands, and the part of
// the image that is theirs, for the refine pass that draws that character.

// How the people of a scene stand. Side by side: apart, nothing overlaps. Close: near each other,
// arms and shoulders may overlap. Front and back: the first person in front, the second behind
// and smaller.
enum class Arrangement { SideBySide, Close, FrontBack };
QString arrangementKey(Arrangement a); // "side_by_side", "close", "front_back"
Arrangement arrangementFromKey(const QString &key, Arrangement fallback = Arrangement::SideBySide);

// The rigs placed for the arrangement (position and scale; the poses stay). The first person is on
// the left (or in front). One person is returned as it is.
QList<RigParams> arrangePeople(const QList<RigParams> &people, Arrangement arrangement);

// A person's body as a filled shape on an image of `size` (Grayscale8, white = body): thick limbs,
// the torso and the head with room for hair. `pose` gives the canvas the keypoints are in.
QImage personSilhouette(const Pose &pose, int person, const QSize &size);

struct PersonRegion
{
    int person = -1;         // index in Pose::people
    QRect box;               // the person's part of the image: keypoints, head and hair, grown
    QImage mask;             // image-sized Grayscale8: white = this person's region, without the
                             // bodies of the people in front of them
};

// Each person's region in an image of `imageSize`, in refine order: from back to front (the person
// lowest in the image is nearest), so the people in front are drawn last and stay whole.
// `grow` widens each box by that fraction of the person's height for hair and clothes.
QList<PersonRegion> personRegions(const Pose &pose, const QSize &imageSize, double grow = 0.08);

// The pose with only one of its people (the control map of that person's refine pass).
Pose onlyPerson(const Pose &pose, int index);

} // namespace lumen::pose
