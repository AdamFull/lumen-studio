#include "pose/Skeleton.h"

#include <QJsonArray>
#include <QLineF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace lumen::pose {

namespace {

QList<Keypoint> *handList(Person &p, Part part)
{
    if (part == Part::LeftHand)
        return &p.leftHand;
    if (part == Part::RightHand)
        return &p.rightHand;
    if (part == Part::Face)
        return &p.face;
    return nullptr;
}

const QList<Keypoint> *handList(const Person &p, Part part)
{
    return handList(const_cast<Person &>(p), part);
}

QJsonArray pointsToJson(const Keypoint *pts, int count)
{
    QJsonArray arr;
    for (int i = 0; i < count; ++i) {
        const Keypoint &k = pts[i];
        arr.append(k.visible ? k.pos.x() : 0.0);
        arr.append(k.visible ? k.pos.y() : 0.0);
        arr.append(k.visible ? 1.0 : 0.0);
    }
    return arr;
}

QList<Keypoint> pointsFromJson(const QJsonArray &arr, int expected, double sx, double sy)
{
    QList<Keypoint> out;
    if (arr.size() < expected * 3)
        return out;
    out.reserve(expected);
    for (int i = 0; i < expected; ++i) {
        const double x = arr.at(i * 3).toDouble();
        const double y = arr.at(i * 3 + 1).toDouble();
        const double c = arr.at(i * 3 + 2).toDouble();
        Keypoint k;
        k.pos = QPointF(x * sx, y * sy);
        k.visible = c > 0.0 && !(x == 0.0 && y == 0.0);
        out.append(k);
    }
    return out;
}

double maxCoordinate(const QJsonArray &arr)
{
    double m = 0.0;
    for (qsizetype i = 0; i + 2 < arr.size(); i += 3)
        m = std::max({m, arr.at(i).toDouble(), arr.at(i + 1).toDouble()});
    return m;
}

} // namespace

// ---- Person ---------------------------------------------------------------------------------

Keypoint *Person::point(const JointRef &ref)
{
    if (ref.part == Part::Body)
        return ref.index >= 0 && ref.index < kBodyCount ? &body[static_cast<size_t>(ref.index)] : nullptr;
    QList<Keypoint> *list = handList(*this, ref.part);
    return list && ref.index >= 0 && ref.index < list->size() ? &(*list)[ref.index] : nullptr;
}

const Keypoint *Person::point(const JointRef &ref) const
{
    return const_cast<Person *>(this)->point(ref);
}

QList<JointRef> Person::allJoints(bool visibleOnly) const
{
    QList<JointRef> refs;
    for (int i = 0; i < kBodyCount; ++i) {
        if (!visibleOnly || body[static_cast<size_t>(i)].visible)
            refs.append({Part::Body, i});
    }
    for (Part part : {Part::LeftHand, Part::RightHand, Part::Face}) {
        const QList<Keypoint> *list = handList(*this, part);
        for (int i = 0; i < list->size(); ++i) {
            if (!visibleOnly || list->at(i).visible)
                refs.append({part, i});
        }
    }
    return refs;
}

QRectF Person::bounds() const
{
    QRectF r;
    bool first = true;
    auto add = [&](const Keypoint &k) {
        if (!k.visible)
            return;
        if (first) {
            r = QRectF(k.pos, QSizeF(0, 0));
            first = false;
        } else {
            r.setLeft(std::min(r.left(), k.pos.x()));
            r.setRight(std::max(r.right(), k.pos.x()));
            r.setTop(std::min(r.top(), k.pos.y()));
            r.setBottom(std::max(r.bottom(), k.pos.y()));
        }
    };
    for (const Keypoint &k : body)
        add(k);
    for (const Keypoint &k : leftHand)
        add(k);
    for (const Keypoint &k : rightHand)
        add(k);
    for (const Keypoint &k : face)
        add(k);
    return r;
}

bool Person::isEmpty() const
{
    return std::none_of(body.begin(), body.end(), [](const Keypoint &k) { return k.visible; });
}

void Person::translate(const QPointF &delta)
{
    for (Keypoint &k : body)
        k.pos += delta;
    for (QList<Keypoint> *list : {&leftHand, &rightHand, &face}) {
        for (Keypoint &k : *list)
            k.pos += delta;
    }
}

void Person::transform(const QPointF &center, double scale, double rotationDeg)
{
    QTransform t;
    t.translate(center.x(), center.y());
    t.rotate(rotationDeg);
    t.scale(scale, scale);
    t.translate(-center.x(), -center.y());
    for (Keypoint &k : body)
        k.pos = t.map(k.pos);
    for (QList<Keypoint> *list : {&leftHand, &rightHand, &face}) {
        for (Keypoint &k : *list)
            k.pos = t.map(k.pos);
    }
}

void Person::scaleAxes(double sx, double sy)
{
    auto apply = [sx, sy](Keypoint &k) { k.pos = QPointF(k.pos.x() * sx, k.pos.y() * sy); };
    for (Keypoint &k : body)
        apply(k);
    for (QList<Keypoint> *list : {&leftHand, &rightHand, &face}) {
        for (Keypoint &k : *list)
            apply(k);
    }
}

void Person::mirror(double axisX)
{
    auto flip = [axisX](Keypoint &k) { k.pos.setX(2.0 * axisX - k.pos.x()); };
    std::array<Keypoint, kBodyCount> mirrored;
    for (int i = 0; i < kBodyCount; ++i) {
        Keypoint k = body[static_cast<size_t>(i)];
        flip(k);
        mirrored[static_cast<size_t>(mirroredBodyJoint(i))] = k;
    }
    body = mirrored;
    for (Keypoint &k : leftHand)
        flip(k);
    for (Keypoint &k : rightHand)
        flip(k);
    std::swap(leftHand, rightHand);
    if (face.size() == kFaceCount) {
        QList<Keypoint> f(kFaceCount);
        for (int i = 0; i < kFaceCount; ++i) {
            Keypoint k = face.at(i);
            flip(k);
            f[mirroredFacePoint(i)] = k;
        }
        face = f;
    } else {
        for (Keypoint &k : face)
            flip(k);
    }
}

bool Person::operator==(const Person &o) const
{
    return body == o.body && leftHand == o.leftHand && rightHand == o.rightHand && face == o.face && label == o.label;
}

// ---- Pose -----------------------------------------------------------------------------------

bool Pose::isEmpty() const
{
    return std::all_of(people.begin(), people.end(), [](const Person &p) { return p.isEmpty(); });
}

void Pose::resizeCanvas(const QSize &size, bool scalePoints)
{
    if (size.isEmpty() || size == canvas)
        return;
    if (scalePoints && !canvas.isEmpty()) {
        // Uniform scale + centring keeps proportions intact when the aspect ratio changes.
        const double s = std::min(double(size.width()) / canvas.width(), double(size.height()) / canvas.height());
        const QPointF offset((size.width() - canvas.width() * s) / 2.0, (size.height() - canvas.height() * s) / 2.0);
        for (Person &p : people) {
            p.scaleAxes(s, s);
            p.translate(offset);
        }
    }
    canvas = size;
}

QJsonObject Pose::toOpenPoseJson() const
{
    QJsonArray peopleArr;
    for (const Person &p : people) {
        QJsonObject o;
        o.insert(QStringLiteral("pose_keypoints_2d"), pointsToJson(p.body.data(), kBodyCount));
        if (p.leftHand.size() == kHandCount)
            o.insert(QStringLiteral("hand_left_keypoints_2d"), pointsToJson(p.leftHand.constData(), kHandCount));
        if (p.rightHand.size() == kHandCount)
            o.insert(QStringLiteral("hand_right_keypoints_2d"), pointsToJson(p.rightHand.constData(), kHandCount));
        if (p.face.size() == kFaceCount)
            o.insert(QStringLiteral("face_keypoints_2d"), pointsToJson(p.face.constData(), kFaceCount));
        peopleArr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("people"), peopleArr);
    root.insert(QStringLiteral("canvas_width"), canvas.width());
    root.insert(QStringLiteral("canvas_height"), canvas.height());
    return root;
}

std::optional<Pose> Pose::fromOpenPoseJson(const QJsonObject &json, QString *error, const QSize &fallbackCanvas)
{
    // Some tools wrap the document in a list: [{"people": ...}].
    if (!json.contains(QStringLiteral("people"))) {
        if (error)
            *error = QStringLiteral("not an OpenPose document (missing \"people\")");
        return std::nullopt;
    }
    Pose pose;
    const int w = json.value(QStringLiteral("canvas_width")).toInt(fallbackCanvas.width());
    const int h = json.value(QStringLiteral("canvas_height")).toInt(fallbackCanvas.height());
    pose.canvas = QSize(std::max(64, w), std::max(64, h));
    for (const QJsonValue &pv : json.value(QStringLiteral("people")).toArray()) {
        const QJsonObject po = pv.toObject();
        const QJsonArray bodyArr = po.value(QStringLiteral("pose_keypoints_2d")).toArray();
        // Normalised coordinates (all <= 1) are scaled to the canvas.
        const bool normalised = maxCoordinate(bodyArr) <= 1.0;
        const double sx = normalised ? pose.canvas.width() : 1.0;
        const double sy = normalised ? pose.canvas.height() : 1.0;
        Person person;
        QList<Keypoint> body = pointsFromJson(bodyArr, kBodyCount, sx, sy);
        if (body.size() != kBodyCount) {
            // BODY_25 → COCO-18 conversion (BODY_25 has MidHip at 8 and feet at 19-24).
            if (bodyArr.size() >= 25 * 3) {
                const QList<Keypoint> b25 = pointsFromJson(bodyArr, 25, sx, sy);
                static const int map[kBodyCount] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
                body.clear();
                for (int i : map)
                    body.append(b25.at(i));
            } else {
                continue;
            }
        }
        for (int i = 0; i < kBodyCount; ++i)
            person.body[static_cast<size_t>(i)] = body.at(i);
        person.leftHand = pointsFromJson(po.value(QStringLiteral("hand_left_keypoints_2d")).toArray(), kHandCount, sx, sy);
        person.rightHand = pointsFromJson(po.value(QStringLiteral("hand_right_keypoints_2d")).toArray(), kHandCount, sx, sy);
        person.face = pointsFromJson(po.value(QStringLiteral("face_keypoints_2d")).toArray(), kFaceCount, sx, sy);
        auto allHidden = [](const QList<Keypoint> &l) {
            return std::none_of(l.begin(), l.end(), [](const Keypoint &k) { return k.visible; });
        };
        if (allHidden(person.leftHand))
            person.leftHand.clear();
        if (allHidden(person.rightHand))
            person.rightHand.clear();
        if (allHidden(person.face))
            person.face.clear();
        pose.people.append(person);
    }
    return pose;
}

// ---- Static tables --------------------------------------------------------------------------

QString bodyJointName(int index)
{
    static const char *names[kBodyCount] = {"Nose", "Neck", "Right shoulder", "Right elbow", "Right wrist",
                                            "Left shoulder", "Left elbow", "Left wrist", "Right hip", "Right knee",
                                            "Right ankle", "Left hip", "Left knee", "Left ankle", "Right eye",
                                            "Left eye", "Right ear", "Left ear"};
    return index >= 0 && index < kBodyCount ? QString::fromLatin1(names[index]) : QString();
}

QString jointName(const JointRef &ref)
{
    switch (ref.part) {
    case Part::Body: return bodyJointName(ref.index);
    case Part::LeftHand: return QStringLiteral("Left hand %1").arg(ref.index);
    case Part::RightHand: return QStringLiteral("Right hand %1").arg(ref.index);
    case Part::Face: return QStringLiteral("Face %1").arg(ref.index);
    }
    return {};
}

int bodyParent(int index)
{
    static const int parents[kBodyCount] = {
        Neck,      // Nose
        -1,        // Neck (root)
        Neck,      // RShoulder
        RShoulder, // RElbow
        RElbow,    // RWrist
        Neck,      // LShoulder
        LShoulder, // LElbow
        LElbow,    // LWrist
        Neck,      // RHip
        RHip,      // RKnee
        RKnee,     // RAnkle
        Neck,      // LHip
        LHip,      // LKnee
        LKnee,     // LAnkle
        Nose,      // REye
        Nose,      // LEye
        REye,      // REar
        LEye,      // LEar
    };
    return index >= 0 && index < kBodyCount ? parents[index] : -1;
}

QList<int> bodyChildren(int index)
{
    QList<int> out;
    for (int i = 0; i < kBodyCount; ++i) {
        if (bodyParent(i) == index)
            out.append(i);
    }
    return out;
}

QList<int> bodyDescendants(int index)
{
    QList<int> out;
    QList<int> stack = bodyChildren(index);
    while (!stack.isEmpty()) {
        const int j = stack.takeLast();
        out.append(j);
        stack.append(bodyChildren(j));
    }
    return out;
}

int mirroredBodyJoint(int index)
{
    static const int map[kBodyCount] = {Nose, Neck, LShoulder, LElbow, LWrist, RShoulder, RElbow, RWrist,
                                        LHip, LKnee, LAnkle, RHip, RKnee, RAnkle, LEye, REye, LEar, REar};
    return index >= 0 && index < kBodyCount ? map[index] : index;
}

int mirroredFacePoint(int i)
{
    if (i >= 0 && i <= 16)
        return 16 - i; // jaw
    if (i >= 17 && i <= 26)
        return 43 - i; // brows 17..21 <-> 26..22
    if (i >= 27 && i <= 30)
        return i; // nose bridge
    if (i >= 31 && i <= 35)
        return 66 - i; // nostrils
    static const int eyes[12] = {45, 44, 43, 42, 47, 46, 39, 38, 37, 36, 41, 40};
    if (i >= 36 && i <= 47)
        return eyes[i - 36];
    static const int outer[12] = {54, 53, 52, 51, 50, 49, 48, 59, 58, 57, 56, 55};
    if (i >= 48 && i <= 59)
        return outer[i - 48];
    static const int inner[8] = {64, 63, 62, 61, 60, 67, 66, 65};
    if (i >= 60 && i <= 67)
        return inner[i - 60];
    if (i == 68)
        return 69;
    if (i == 69)
        return 68;
    return i;
}

const QList<QPair<int, int>> &bodyLimbs()
{
    // controlnet_aux limbSeq (1-based) converted to 0-based, first 17 entries.
    static const QList<QPair<int, int>> limbs = {
        {1, 2}, {1, 5}, {2, 3}, {3, 4}, {5, 6}, {6, 7}, {1, 8}, {8, 9}, {9, 10},
        {1, 11}, {11, 12}, {12, 13}, {1, 0}, {0, 14}, {14, 16}, {0, 15}, {15, 17},
    };
    return limbs;
}

QColor bodyColor(int index)
{
    static const int colors[kBodyCount][3] = {
        {255, 0, 0}, {255, 85, 0}, {255, 170, 0}, {255, 255, 0}, {170, 255, 0}, {85, 255, 0},
        {0, 255, 0}, {0, 255, 85}, {0, 255, 170}, {0, 255, 255}, {0, 170, 255}, {0, 85, 255},
        {0, 0, 255}, {85, 0, 255}, {170, 0, 255}, {255, 0, 255}, {255, 0, 170}, {255, 0, 85},
    };
    if (index < 0 || index >= kBodyCount)
        return Qt::white;
    return QColor(colors[index][0], colors[index][1], colors[index][2]);
}

const QList<QPair<int, int>> &handEdges()
{
    static const QList<QPair<int, int>> edges = {
        {0, 1}, {1, 2}, {2, 3}, {3, 4}, {0, 5}, {5, 6}, {6, 7}, {7, 8}, {0, 9}, {9, 10},
        {10, 11}, {11, 12}, {0, 13}, {13, 14}, {14, 15}, {15, 16}, {0, 17}, {17, 18}, {18, 19}, {19, 20},
    };
    return edges;
}

QColor handEdgeColor(int edgeIndex)
{
    // matplotlib.colors.hsv_to_rgb([ie / len(edges), 1.0, 1.0]) * 255
    const double h = double(edgeIndex) / double(handEdges().size());
    const double hh = h * 6.0;
    const int sector = static_cast<int>(std::floor(hh)) % 6;
    const double f = hh - std::floor(hh);
    const double q = 1.0 - f;
    double r = 0, g = 0, b = 0;
    switch (sector) {
    case 0: r = 1; g = f; b = 0; break;
    case 1: r = q; g = 1; b = 0; break;
    case 2: r = 0; g = 1; b = f; break;
    case 3: r = 0; g = q; b = 1; break;
    case 4: r = f; g = 0; b = 1; break;
    default: r = 1; g = 0; b = q; break;
    }
    return QColor(int(r * 255), int(g * 255), int(b * 255));
}

} // namespace lumen::pose
