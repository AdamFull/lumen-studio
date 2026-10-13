#include "pose/CastLayout.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>

namespace lumen::pose {

namespace {

// The keypoints of a person mapped from the pose canvas to `size`.
Person scaledPerson(const Pose &pose, int index, const QSize &size)
{
    Pose scaled = pose;
    scaled.resizeCanvas(size, true);
    return scaled.people.value(index);
}

const Keypoint &joint(const Person &p, int index)
{
    return p.body[static_cast<size_t>(index)];
}

// The size of the head (nose to neck), the unit for limb widths and margins. Falls back to a
// share of the body's height when the head is not visible (a back view hides the nose).
double headSize(const Person &p)
{
    const Keypoint &nose = joint(p, Nose);
    const Keypoint &neck = joint(p, Neck);
    if (nose.visible && neck.visible) {
        const double d = QLineF(nose.pos, neck.pos).length();
        if (d > 1.0)
            return d;
    }
    return std::max(8.0, p.bounds().height() / 7.0);
}

// Where the top of the hair can be: above the nose (or the neck) by about two head sizes.
QPointF headCentre(const Person &p, double head)
{
    const Keypoint &nose = joint(p, Nose);
    if (nose.visible)
        return nose.pos - QPointF(0, head * 0.35);
    return joint(p, Neck).pos - QPointF(0, head * 1.2);
}

QRect regionBox(const Person &p, const QSize &size, double grow)
{
    const double head = headSize(p);
    QRectF box = p.bounds();
    const QPointF centre = headCentre(p, head);
    box = box.united(QRectF(centre.x() - head * 1.3, centre.y() - head * 1.5, head * 2.6, head * 2.6));
    const double margin = grow * std::max(box.height(), head * 4.0);
    box.adjust(-margin, -margin, margin, margin);
    return box.toAlignedRect().intersected(QRect(QPoint(0, 0), size));
}

} // namespace

QString arrangementKey(Arrangement a)
{
    switch (a) {
    case Arrangement::Close: return QStringLiteral("close");
    case Arrangement::FrontBack: return QStringLiteral("front_back");
    case Arrangement::SideBySide: break;
    }
    return QStringLiteral("side_by_side");
}

Arrangement arrangementFromKey(const QString &key, Arrangement fallback)
{
    if (key == QLatin1String("side_by_side"))
        return Arrangement::SideBySide;
    if (key == QLatin1String("close"))
        return Arrangement::Close;
    if (key == QLatin1String("front_back"))
        return Arrangement::FrontBack;
    return fallback;
}

QList<RigParams> arrangePeople(const QList<RigParams> &people, Arrangement arrangement)
{
    if (people.size() < 2)
        return people;
    QList<RigParams> out = people;
    const int n = int(out.size());
    for (int i = 0; i < n; ++i) {
        RigParams &r = out[i];
        switch (arrangement) {
        case Arrangement::SideBySide:
            // Evenly apart; small enough that two full bodies fit with room between them.
            r.positionX = (i + 1.0) / (n + 1.0) + (i == 0 ? -0.04 : i == n - 1 ? 0.04 : 0.0);
            r.scale = n == 2 ? 0.82 : 0.7;
            r.positionY = 0.0;
            break;
        case Arrangement::Close:
            r.positionX = 0.5 + (i - (n - 1) / 2.0) * (n == 2 ? 0.22 : 0.18);
            r.scale = n == 2 ? 0.88 : 0.76;
            r.positionY = 0.0;
            break;
        case Arrangement::FrontBack:
            // The first in front, nearer and bigger; the others behind, to the side and smaller.
            r.positionX = i == 0 ? 0.42 : 0.42 + 0.2 * i;
            r.scale = i == 0 ? 0.95 : 0.78;
            r.positionY = i == 0 ? 0.03 : -0.05;
            break;
        }
    }
    return out;
}

QImage personSilhouette(const Pose &pose, int person, const QSize &size)
{
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    const Person p = scaledPerson(pose, person, size);
    if (p.isEmpty())
        return mask;
    const double head = headSize(p);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, head * 0.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::white);
    // Limbs (without the face lines, which the head covers).
    for (const auto &[a, b] : bodyLimbs()) {
        if (a == Nose || b == Nose || a >= 14 || b >= 14)
            continue;
        const Keypoint &ka = joint(p, a);
        const Keypoint &kb = joint(p, b);
        if (ka.visible && kb.visible)
            painter.drawLine(ka.pos, kb.pos);
    }
    // The hands, past the wrists along the forearms.
    for (const auto &[elbow, wrist] : {std::pair{RElbow, RWrist}, std::pair{LElbow, LWrist}}) {
        const Keypoint &e = joint(p, elbow);
        const Keypoint &w = joint(p, wrist);
        if (!w.visible)
            continue;
        QPointF centre = w.pos;
        if (e.visible) {
            const QLineF forearm(e.pos, w.pos);
            if (forearm.length() > 1.0)
                centre += (w.pos - e.pos) * (head * 0.45 / forearm.length());
        }
        painter.drawEllipse(centre, head * 0.6, head * 0.6);
    }
    // The torso between shoulders and hips.
    QPolygonF torso;
    for (int j : {RShoulder, LShoulder, LHip, RHip}) {
        if (joint(p, j).visible)
            torso << joint(p, j).pos;
    }
    if (torso.size() >= 3)
        painter.drawPolygon(torso);
    // The head with hair.
    const QPointF centre = headCentre(p, head);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(centre, head * 1.1, head * 1.25);
    return mask;
}

QList<PersonRegion> personRegions(const Pose &pose, const QSize &imageSize, double grow)
{
    QList<PersonRegion> regions;
    for (int i = 0; i < pose.people.size(); ++i) {
        const Person p = scaledPerson(pose, i, imageSize);
        if (p.isEmpty())
            continue;
        PersonRegion r;
        r.person = i;
        r.box = regionBox(p, imageSize, grow);
        regions << r;
    }
    // Back to front: the bigger the head, the nearer (feet and box bottoms say nothing when the
    // framing cuts the people off); for heads of about the same size, the lower feet.
    QHash<int, std::pair<double, double>> depth; // person -> (head size, lowest keypoint)
    for (const PersonRegion &r : std::as_const(regions)) {
        const Person p = scaledPerson(pose, r.person, imageSize);
        depth.insert(r.person, {headSize(p), p.bounds().bottom()});
    }
    std::stable_sort(regions.begin(), regions.end(), [&depth](const PersonRegion &a, const PersonRegion &b) {
        const auto [ha, fa] = depth.value(a.person);
        const auto [hb, fb] = depth.value(b.person);
        if (std::abs(ha - hb) > 0.08 * std::max(ha, hb))
            return ha < hb;
        return fa + 4 < fb;
    });
    // The other people's bodies stay as they are: all of a body in front, and of a body behind
    // whatever this person's own body does not cover (a hand reaching into this region).
    QList<QImage> bodies;
    for (const PersonRegion &r : std::as_const(regions))
        bodies << personSilhouette(pose, r.person, imageSize);
    for (int k = 0; k < regions.size(); ++k) {
        PersonRegion &r = regions[k];
        r.mask = QImage(imageSize, QImage::Format_Grayscale8);
        r.mask.fill(0);
        {
            QPainter painter(&r.mask);
            painter.fillRect(r.box, Qt::white);
        }
        const QImage &own = bodies[k];
        for (int y = r.box.top(); y <= r.box.bottom(); ++y) {
            uchar *line = r.mask.scanLine(y);
            const uchar *mine = own.constScanLine(y);
            for (int other = 0; other < regions.size(); ++other) {
                if (other == k)
                    continue;
                const uchar *theirs = bodies[other].constScanLine(y);
                const bool inFront = other > k;
                for (int x = r.box.left(); x <= r.box.right(); ++x) {
                    if (theirs[x] > 127 && (inFront || mine[x] <= 127))
                        line[x] = 0;
                }
            }
        }
    }
    return regions;
}

Pose onlyPerson(const Pose &pose, int index)
{
    Pose out;
    out.canvas = pose.canvas;
    if (index >= 0 && index < pose.people.size())
        out.people << pose.people[index];
    return out;
}

} // namespace lumen::pose
