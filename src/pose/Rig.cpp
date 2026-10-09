#include "pose/Rig.h"

#include "pose/Anatomy.h"

#include <QJsonArray>
#include <QQuaternion>
#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace lumen::pose {

namespace {

constexpr double kDeg = M_PI / 180.0;

// Body proportions in units of total height (ground at y = 0). Character faces +z; their left is +x.
const QVector3D kPelvis(0.0f, 0.53f, 0.0f);
const QVector3D kNeck(0.0f, 0.815f, 0.0f);
const QVector3D kHeadPivot(0.0f, 0.845f, 0.0f);
const QVector3D kHeadCenterOffset(0.0f, 0.065f, 0.01f);
const QVector3D kHeadUnit(0.058f, 0.07f, 0.06f);
constexpr float kShoulderX = 0.115f, kShoulderY = 0.805f;
constexpr float kHipX = 0.06f, kHipY = 0.505f;
constexpr float kUpperArm = 0.165f, kForearm = 0.145f, kHand = 0.105f;
constexpr float kThigh = 0.245f, kShin = 0.245f;

QQuaternion rotY(double deg) { return QQuaternion::fromAxisAndAngle(0, 1, 0, float(deg)); }
QQuaternion rotX(double deg) { return QQuaternion::fromAxisAndAngle(1, 0, 0, float(deg)); }
QQuaternion rotZ(double deg) { return QQuaternion::fromAxisAndAngle(0, 0, 1, float(deg)); }

// yaw (+ = toward +x), pitch (+ = top toward +z), roll (+ = top toward +x).
QQuaternion orientation(double yaw, double pitch, double roll)
{
    return rotY(yaw) * rotX(pitch) * rotZ(-roll);
}

// Spherical limb direction shared by arms and legs (see LimbParams).
QVector3D limbDirection(const LimbParams &p, double side)
{
    const double r = p.raise * kDeg;
    const double t = p.direction * kDeg;
    return QVector3D(float(side * std::sin(r) * std::sin(t)), float(-std::cos(r)), float(std::sin(r) * std::cos(t)));
}

// Bending direction of the elbow/knee. The rest-pose bend direction (arms: forward, legs: backward)
// is carried along by the minimal rotation that takes the hanging limb to its current direction, so
// a sideways-raised arm still bends forward, a forward-raised arm bends upward and a forward thigh
// (sitting) bends its shin downward. bendTwist then rotates the bending plane around the limb.
QVector3D bendDirection(const QVector3D &d, const LimbParams &p, double side, bool leg)
{
    const QVector3D down(0, -1, 0);
    const QVector3D rest = leg ? QVector3D(0, 0, -1) : QVector3D(0, 0, 1);
    QQuaternion frame;
    if (QVector3D::dotProduct(down, d) < -0.9999f)
        frame = QQuaternion::fromAxisAndAngle(0, 0, 1, 180.0f); // straight up: swing through the side plane
    else
        frame = QQuaternion::rotationTo(down, d);
    QVector3D b = frame.rotatedVector(rest);
    b = (b - d * QVector3D::dotProduct(b, d)).normalized();
    const double t = p.bendTwist * side * kDeg;
    const QVector3D axis = QVector3D::crossProduct(d, b);
    return (b * float(std::cos(t)) + axis * float(std::sin(t))).normalized();
}

struct Joint3D
{
    QVector3D pos;
    bool visible = true;
};

double clampDeg(double v, double lo, double hi)
{
    return std::clamp(v, lo, hi);
}

LimbParams limbFromJson(const QJsonObject &o, const LimbParams &d)
{
    LimbParams p = d;
    p.raise = clampDeg(o.value(QStringLiteral("raise")).toDouble(d.raise), -45, 200);
    p.direction = clampDeg(o.value(QStringLiteral("direction")).toDouble(d.direction), -180, 180);
    p.bend = clampDeg(o.value(QStringLiteral("bend")).toDouble(d.bend), 0, 170);
    p.bendTwist = clampDeg(o.value(QStringLiteral("bend_twist")).toDouble(d.bendTwist), -180, 180);
    return p;
}

QJsonObject limbToJson(const LimbParams &p)
{
    return QJsonObject{{QStringLiteral("raise"), p.raise},
                       {QStringLiteral("direction"), p.direction},
                       {QStringLiteral("bend"), p.bend},
                       {QStringLiteral("bend_twist"), p.bendTwist}};
}

} // namespace

QStringList framingNames()
{
    return {QStringLiteral("full_body"), QStringLiteral("knees_up"), QStringLiteral("cowboy_shot"),
            QStringLiteral("upper_body"), QStringLiteral("bust"), QStringLiteral("headshot")};
}

QJsonObject RigParams::toJson() const
{
    QJsonObject o;
    if (!name.isEmpty())
        o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("view"), QJsonObject{{QStringLiteral("yaw"), yaw},
                                                 {QStringLiteral("pitch"), pitch},
                                                 {QStringLiteral("roll"), roll},
                                                 {QStringLiteral("camera_pitch"), cameraPitch},
                                                 {QStringLiteral("perspective"), perspective}});
    o.insert(QStringLiteral("torso"), QJsonObject{{QStringLiteral("lean"), torsoLean},
                                                  {QStringLiteral("side"), torsoSide},
                                                  {QStringLiteral("twist"), torsoTwist}});
    o.insert(QStringLiteral("head"), QJsonObject{{QStringLiteral("yaw"), headYaw},
                                                 {QStringLiteral("pitch"), headPitch},
                                                 {QStringLiteral("roll"), headRoll}});
    o.insert(QStringLiteral("left_arm"), limbToJson(leftArm));
    o.insert(QStringLiteral("right_arm"), limbToJson(rightArm));
    o.insert(QStringLiteral("left_leg"), limbToJson(leftLeg));
    o.insert(QStringLiteral("right_leg"), limbToJson(rightLeg));
    o.insert(QStringLiteral("left_hand"), leftHand);
    o.insert(QStringLiteral("right_hand"), rightHand);
    o.insert(QStringLiteral("expression"), expression);
    o.insert(QStringLiteral("framing"), framing);
    o.insert(QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), positionX},
                                                     {QStringLiteral("y"), positionY},
                                                     {QStringLiteral("scale"), scale}});
    o.insert(QStringLiteral("include_hands"), includeHands);
    o.insert(QStringLiteral("include_face"), includeFace);
    return o;
}

RigParams RigParams::fromJson(const QJsonObject &json)
{
    return fromJson(json, RigParams());
}

RigParams RigParams::fromJson(const QJsonObject &json, const RigParams &d)
{
    RigParams p = d;
    p.name = json.value(QStringLiteral("name")).toString(d.name);
    const QJsonObject view = json.value(QStringLiteral("view")).toObject();
    p.yaw = view.value(QStringLiteral("yaw")).toDouble(d.yaw);
    p.pitch = clampDeg(view.value(QStringLiteral("pitch")).toDouble(d.pitch), -180, 180);
    p.roll = clampDeg(view.value(QStringLiteral("roll")).toDouble(d.roll), -180, 180);
    p.cameraPitch = clampDeg(view.value(QStringLiteral("camera_pitch")).toDouble(d.cameraPitch), -80, 80);
    p.perspective = std::clamp(view.value(QStringLiteral("perspective")).toDouble(d.perspective), 0.0, 1.0);
    const QJsonObject torso = json.value(QStringLiteral("torso")).toObject();
    p.torsoLean = clampDeg(torso.value(QStringLiteral("lean")).toDouble(d.torsoLean), -60, 100);
    p.torsoSide = clampDeg(torso.value(QStringLiteral("side")).toDouble(d.torsoSide), -60, 60);
    p.torsoTwist = clampDeg(torso.value(QStringLiteral("twist")).toDouble(d.torsoTwist), -90, 90);
    const QJsonObject head = json.value(QStringLiteral("head")).toObject();
    p.headYaw = clampDeg(head.value(QStringLiteral("yaw")).toDouble(d.headYaw), -90, 90);
    p.headPitch = clampDeg(head.value(QStringLiteral("pitch")).toDouble(d.headPitch), -60, 60);
    p.headRoll = clampDeg(head.value(QStringLiteral("roll")).toDouble(d.headRoll), -45, 45);
    p.leftArm = limbFromJson(json.value(QStringLiteral("left_arm")).toObject(), d.leftArm);
    p.rightArm = limbFromJson(json.value(QStringLiteral("right_arm")).toObject(), d.rightArm);
    p.leftLeg = limbFromJson(json.value(QStringLiteral("left_leg")).toObject(), d.leftLeg);
    p.rightLeg = limbFromJson(json.value(QStringLiteral("right_leg")).toObject(), d.rightLeg);
    p.leftHand = json.value(QStringLiteral("left_hand")).toString(d.leftHand);
    p.rightHand = json.value(QStringLiteral("right_hand")).toString(d.rightHand);
    p.expression = json.value(QStringLiteral("expression")).toString(d.expression);
    const QString framing = json.value(QStringLiteral("framing")).toString(d.framing);
    p.framing = framingNames().contains(framing) ? framing : d.framing;
    const QJsonObject pos = json.value(QStringLiteral("position")).toObject();
    p.positionX = std::clamp(pos.value(QStringLiteral("x")).toDouble(d.positionX), -0.5, 1.5);
    p.positionY = std::clamp(pos.value(QStringLiteral("y")).toDouble(d.positionY), -1.0, 1.0);
    p.scale = std::clamp(pos.value(QStringLiteral("scale")).toDouble(d.scale), 0.1, 4.0);
    p.includeHands = json.value(QStringLiteral("include_hands")).toBool(d.includeHands);
    p.includeFace = json.value(QStringLiteral("include_face")).toBool(d.includeFace);
    return p;
}

Person solveRig(const RigParams &params, const QSize &canvas)
{
    const QQuaternion body = orientation(params.yaw, params.pitch, params.roll);
    const QQuaternion torso = orientation(params.torsoTwist, params.torsoLean, params.torsoSide);
    const QQuaternion head = orientation(params.headYaw, params.headPitch, params.headRoll);
    const QQuaternion upper = body * torso;
    const QQuaternion headWorld = upper * head;

    auto lowerToWorld = [&](const QVector3D &p) { return kPelvis + body.rotatedVector(p - kPelvis); };
    auto upperToWorld = [&](const QVector3D &p) { return kPelvis + upper.rotatedVector(p - kPelvis); };
    auto headToWorld = [&](const QVector3D &local) {
        const QVector3D scaled(local.x() * kHeadUnit.x(), local.y() * kHeadUnit.y(), local.z() * kHeadUnit.z());
        const QVector3D inUpper = kHeadPivot + head.rotatedVector(kHeadCenterOffset + scaled);
        return upperToWorld(inUpper);
    };
    auto facesCamera = [&](const QVector3D &localNormal, float threshold) {
        return headWorld.rotatedVector(localNormal.normalized()).z() > threshold;
    };

    std::array<Joint3D, kBodyCount> j{};
    j[Neck].pos = upperToWorld(kNeck);
    j[Nose] = {headToWorld(headNoseLocal()), facesCamera(QVector3D(0, -0.1f, 1), -0.15f)};
    j[REye] = {headToWorld(headEyeLocal(false)), facesCamera(QVector3D(-0.45f, 0.1f, 1), -0.05f)};
    j[LEye] = {headToWorld(headEyeLocal(true)), facesCamera(QVector3D(0.45f, 0.1f, 1), -0.05f)};
    j[REar] = {headToWorld(headEarLocal(false)), facesCamera(QVector3D(-1, 0.08f, -0.25f), -0.45f)};
    j[LEar] = {headToWorld(headEarLocal(true)), facesCamera(QVector3D(1, 0.08f, -0.25f), -0.45f)};

    // Arms (in torso frame).
    auto arm = [&](const LimbParams &lp, double side, int sh, int el, int wr) {
        const QVector3D shoulder(float(side * kShoulderX), kShoulderY, -0.005f);
        const QVector3D d = limbDirection(lp, side);
        const QVector3D elbow = shoulder + d * kUpperArm;
        const QVector3D b = bendDirection(d, lp, side, false);
        const double bend = lp.bend * kDeg;
        const QVector3D fore = (d * float(std::cos(bend)) + b * float(std::sin(bend))).normalized();
        const QVector3D wrist = elbow + fore * kForearm;
        j[static_cast<size_t>(sh)].pos = upperToWorld(shoulder);
        j[static_cast<size_t>(el)].pos = upperToWorld(elbow);
        j[static_cast<size_t>(wr)].pos = upperToWorld(wrist);
    };
    arm(params.leftArm, 1.0, LShoulder, LElbow, LWrist);
    arm(params.rightArm, -1.0, RShoulder, RElbow, RWrist);

    auto leg = [&](const LimbParams &lp, double side, int hp, int kn, int an) {
        const QVector3D hip(float(side * kHipX), kHipY, 0.0f);
        const QVector3D d = limbDirection(lp, side);
        const QVector3D knee = hip + d * kThigh;
        const QVector3D b = bendDirection(d, lp, side, true);
        const double bend = lp.bend * kDeg;
        const QVector3D shin = (d * float(std::cos(bend)) + b * float(std::sin(bend))).normalized();
        const QVector3D ankle = knee + shin * kShin;
        j[static_cast<size_t>(hp)].pos = lowerToWorld(hip);
        j[static_cast<size_t>(kn)].pos = lowerToWorld(knee);
        j[static_cast<size_t>(an)].pos = lowerToWorld(ankle);
    };
    leg(params.leftLeg, 1.0, LHip, LKnee, LAnkle);
    leg(params.rightLeg, -1.0, RHip, RKnee, RAnkle);

    // Face landmarks.
    std::array<Joint3D, kFaceCount> face{};
    if (params.includeFace) {
        const auto tpl = faceTemplate(expressionFromName(params.expression));
        for (int i = 0; i < kFaceCount; ++i) {
            const QVector3D &l = tpl[static_cast<size_t>(i)];
            face[static_cast<size_t>(i)] = {headToWorld(l), facesCamera(QVector3D(l.x() * 1.1f, l.y() * 0.4f, l.z() + 0.15f), 0.0f)};
        }
    }

    // ---- Camera ----
    const QQuaternion camera = rotX(params.cameraPitch);
    const double persp = std::clamp(params.perspective, 0.0, 1.0);
    const double distance = persp > 1e-3 ? 1.4 / persp : 0.0;
    auto project = [&](const QVector3D &p) {
        QVector3D v = camera.rotatedVector(p - kPelvis);
        double f = 1.0;
        if (distance > 0.0)
            f = distance / std::max(0.2, distance - double(v.z()));
        return QPointF(v.x() * f, (v.y() * f) + kPelvis.y()); // y up
    };

    std::array<QPointF, kBodyCount> p2{};
    for (int i = 0; i < kBodyCount; ++i)
        p2[static_cast<size_t>(i)] = project(j[static_cast<size_t>(i)].pos);
    std::array<QPointF, kFaceCount> f2{};
    for (int i = 0; i < kFaceCount; ++i)
        f2[static_cast<size_t>(i)] = project(face[static_cast<size_t>(i)].pos);
    const QPointF headTop = project(upperToWorld(kHeadPivot + head.rotatedVector(kHeadCenterOffset + QVector3D(0, 0.085f, 0))));

    // ---- Framing ----
    const double W = canvas.width();
    const double H = canvas.height();
    const double margin = 0.06;
    auto avgY = [&](int a, int b) { return (p2[static_cast<size_t>(a)].y() + p2[static_cast<size_t>(b)].y()) / 2.0; };
    const double hipY = avgY(LHip, RHip);
    const double kneeY = avgY(LKnee, RKnee);
    const double neckY = p2[Neck].y();

    double pixelsPerUnit = 1.0;
    QPointF anchor;   // body-space point that maps to `target`
    QPointF target;
    if (params.framing == QLatin1String("full_body")) {
        double minX = headTop.x(), maxX = headTop.x(), minY = headTop.y(), maxY = headTop.y();
        auto include = [&](const QPointF &p) {
            minX = std::min(minX, p.x());
            maxX = std::max(maxX, p.x());
            minY = std::min(minY, p.y());
            maxY = std::max(maxY, p.y());
        };
        for (const QPointF &p : p2)
            include(p);
        include(p2[LAnkle] - QPointF(0, 0.025));
        include(p2[RAnkle] - QPointF(0, 0.025));
        // Hands extend beyond the wrists along the forearm.
        for (auto [el, wr] : {std::pair{LElbow, LWrist}, std::pair{RElbow, RWrist}}) {
            const QPointF fore = p2[static_cast<size_t>(wr)] - p2[static_cast<size_t>(el)];
            const double len = std::hypot(fore.x(), fore.y());
            if (len > 1e-6 && params.includeHands)
                include(p2[static_cast<size_t>(wr)] + fore / len * kHand);
        }
        const double bw = std::max(0.05, maxX - minX + 0.06);
        const double bh = std::max(0.05, maxY - minY);
        pixelsPerUnit = std::min(H * (1.0 - 2 * margin) / bh, W * (1.0 - 2 * margin) / bw) * params.scale;
        anchor = QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
        target = QPointF(params.positionX * W, H / 2.0 + params.positionY * H);
    } else {
        double bottom = hipY;
        if (params.framing == QLatin1String("knees_up"))
            bottom = kneeY - 0.04;
        else if (params.framing == QLatin1String("cowboy_shot"))
            bottom = (hipY + kneeY) / 2.0;
        else if (params.framing == QLatin1String("upper_body"))
            bottom = hipY + 0.02;
        else if (params.framing == QLatin1String("bust"))
            bottom = neckY - 0.17;
        else if (params.framing == QLatin1String("headshot"))
            bottom = neckY - 0.06;
        const double top = headTop.y() + 0.015;
        const double regionH = std::max(0.05, top - bottom);
        pixelsPerUnit = H * (1.0 - margin) / regionH * params.scale;
        anchor = QPointF(p2[Neck].x(), top);
        target = QPointF(params.positionX * W, margin * H + params.positionY * H);
    }
    auto toCanvas = [&](const QPointF &p) {
        return QPointF(target.x() + (p.x() - anchor.x()) * pixelsPerUnit, target.y() + (anchor.y() - p.y()) * pixelsPerUnit);
    };

    Person person;
    person.label = params.name;
    for (int i = 0; i < kBodyCount; ++i) {
        person.body[static_cast<size_t>(i)].pos = toCanvas(p2[static_cast<size_t>(i)]);
        person.body[static_cast<size_t>(i)].visible = j[static_cast<size_t>(i)].visible;
    }
    if (params.includeFace) {
        person.face.resize(kFaceCount);
        for (int i = 0; i < kFaceCount; ++i) {
            person.face[i].pos = toCanvas(f2[static_cast<size_t>(i)]);
            person.face[i].visible = face[static_cast<size_t>(i)].visible;
        }
        // A face seen from behind is not detectable at all.
        if (!j[Nose].visible)
            person.face.clear();
    }

    if (params.includeHands) {
        auto makeHand = [&](bool left) {
            const int el = left ? LElbow : RElbow;
            const int wr = left ? LWrist : RWrist;
            const QPointF e = person.body[static_cast<size_t>(el)].pos;
            const QPointF w = person.body[static_cast<size_t>(wr)].pos;
            QPointF axis = w - e;
            const double projLen = std::hypot(axis.x(), axis.y());
            const double fullLen = kForearm * pixelsPerUnit;
            QList<Keypoint> pts;
            if (projLen < 1e-3)
                return pts;
            axis /= projLen;
            const double foreshortening = std::clamp(projLen / fullLen, 0.45, 1.0);
            const double handLen = kHand * pixelsPerUnit * foreshortening;
            // Seen from behind the palm side flips.
            const bool backView = std::cos(params.yaw * kDeg) < 0.0;
            QPointF thumbDir = left ? QPointF(axis.y(), -axis.x()) : QPointF(-axis.y(), axis.x());
            if (backView)
                thumbDir = -thumbDir;
            const auto tpl = handTemplate(handShapeFromName(left ? params.leftHand : params.rightHand));
            for (const QPointF &t : tpl) {
                Keypoint k;
                k.pos = w + axis * (t.x() * handLen) + thumbDir * (t.y() * handLen);
                k.visible = true;
                pts.append(k);
            }
            return pts;
        };
        if (params.leftHand != QLatin1String("none"))
            person.leftHand = makeHand(true);
        if (params.rightHand != QLatin1String("none"))
            person.rightHand = makeHand(false);
    }
    return person;
}

Pose solvePose(const QList<RigParams> &people, const QSize &canvas)
{
    Pose pose;
    pose.canvas = canvas;
    for (const RigParams &p : people)
        pose.people.append(solveRig(p, canvas));
    return pose;
}

QJsonObject rigParamsSchema()
{
    auto num = [](double lo, double hi, const QString &desc) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("number")},
                           {QStringLiteral("minimum"), lo},
                           {QStringLiteral("maximum"), hi},
                           {QStringLiteral("description"), desc}};
    };
    auto limb = [&](const QString &what) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("description"), what},
            {QStringLiteral("properties"),
             QJsonObject{{QStringLiteral("raise"), num(-45, 200, QStringLiteral("0 down, 90 horizontal, 180 up"))},
                         {QStringLiteral("direction"), num(-180, 180, QStringLiteral("0 forward, 90 own side, 180 back, -90 across body"))},
                         {QStringLiteral("bend"), num(0, 170, QStringLiteral("elbow/knee flexion"))},
                         {QStringLiteral("bend_twist"), num(-180, 180, QStringLiteral("rotates bending plane"))}}},
            {QStringLiteral("required"), QJsonArray{QStringLiteral("raise"), QStringLiteral("direction"), QStringLiteral("bend"), QStringLiteral("bend_twist")}},
            {QStringLiteral("additionalProperties"), false}};
    };
    auto obj = [](const QJsonObject &props) {
        QJsonArray req;
        for (auto it = props.begin(); it != props.end(); ++it)
            req.append(it.key());
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                           {QStringLiteral("properties"), props},
                           {QStringLiteral("required"), req},
                           {QStringLiteral("additionalProperties"), false}};
    };
    auto enumOf = [](const QStringList &values) {
        QJsonArray arr;
        for (const QString &v : values)
            arr.append(v);
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), arr}};
    };
    QStringList hands = handShapeNames();
    hands << QStringLiteral("none");
    QJsonObject props{
        {QStringLiteral("view"), obj(QJsonObject{{QStringLiteral("yaw"), num(-180, 180, QStringLiteral("body turn"))},
                                                 {QStringLiteral("pitch"), num(-180, 180, QStringLiteral("lean toward viewer"))},
                                                 {QStringLiteral("roll"), num(-180, 180, QStringLiteral("tilt"))},
                                                 {QStringLiteral("camera_pitch"), num(-80, 80, QStringLiteral("camera above (+) / below (-)"))},
                                                 {QStringLiteral("perspective"), num(0, 1, QStringLiteral("0 flat .. 1 strong"))}})},
        {QStringLiteral("torso"), obj(QJsonObject{{QStringLiteral("lean"), num(-60, 100, QStringLiteral("bend forward"))},
                                                  {QStringLiteral("side"), num(-60, 60, QStringLiteral("bend sideways"))},
                                                  {QStringLiteral("twist"), num(-90, 90, QStringLiteral("shoulders turn"))}})},
        {QStringLiteral("head"), obj(QJsonObject{{QStringLiteral("yaw"), num(-90, 90, QStringLiteral("look right(+)/left(-)"))},
                                                 {QStringLiteral("pitch"), num(-60, 60, QStringLiteral("look down(+)/up(-)"))},
                                                 {QStringLiteral("roll"), num(-45, 45, QStringLiteral("tilt"))}})},
        {QStringLiteral("left_arm"), limb(QStringLiteral("character's left arm"))},
        {QStringLiteral("right_arm"), limb(QStringLiteral("character's right arm"))},
        {QStringLiteral("left_leg"), limb(QStringLiteral("character's left leg"))},
        {QStringLiteral("right_leg"), limb(QStringLiteral("character's right leg"))},
        {QStringLiteral("left_hand"), enumOf(hands)},
        {QStringLiteral("right_hand"), enumOf(hands)},
        {QStringLiteral("expression"), enumOf(expressionNames())},
        {QStringLiteral("framing"), enumOf(framingNames())},
        {QStringLiteral("position"), obj(QJsonObject{{QStringLiteral("x"), num(-0.5, 1.5, QStringLiteral("horizontal centre 0..1"))},
                                                     {QStringLiteral("y"), num(-1, 1, QStringLiteral("vertical offset"))},
                                                     {QStringLiteral("scale"), num(0.1, 4, QStringLiteral("size multiplier"))}})},
    };
    return obj(props);
}

QString rigParamsGuide()
{
    return QStringLiteral(
        "Pose rig parameters (all angles in degrees; 'left'/'right' are the CHARACTER's own sides):\n"
        "- view.yaw: 0 faces the viewer, +35 three-quarter view turned to the viewer's right, 90 right-facing profile, "
        "-90 left-facing profile, 180 back view. view.pitch: + leans the whole body toward the viewer. view.roll: tilts the whole body "
        "(90 = lying horizontally). view.camera_pitch: + camera above looking down, - camera below looking up. "
        "view.perspective: 0 flat, 0.15 natural, 0.6 dramatic.\n"
        "- torso.lean: + bends the upper body forward. torso.side: + bends toward the viewer's right. torso.twist: turns the shoulders like yaw.\n"
        "- head.yaw: + looks toward the viewer's right. head.pitch: + looks down, - looks up. head.roll: + tilts toward the viewer's right.\n"
        "- Limbs (left_arm, right_arm, left_leg, right_leg): raise = 0 hanging down, 90 horizontal, 180 straight up. "
        "direction = where the raised limb points: 0 forward, 90 out to the character's own side, 180 backward, -90 across the body. "
        "bend = elbow/knee flexion (0 straight, 90 right angle, 150 fully bent). bend_twist (arms) = 0 forearm bends forward, "
        "+90 toward the body midline (hands on hips, arms crossed), -90 upward/outward (waving, flexing). "
        "Legs: knees bend backward with bend_twist 0.\n"
        "- Examples: T-pose arms raise 90 direction 90; A-pose arms raise 35 direction 90; arms down raise 5-10 direction 90; "
        "pointing forward raise 90 direction 0; waving right_arm raise 100 direction 90 bend 70 bend_twist -90; "
        "hands on hips raise 40 direction 90 bend 110 bend_twist 90; sitting on a chair legs raise 90 direction 0 bend 90; "
        "kneeling legs raise 0 bend 90; walking left_leg raise 20 direction 0, right_leg raise 15 direction 180 bend 20.\n"
        "- left_hand/right_hand: relaxed, open, fist, point, peace, thumbs_up, grip, none. expression: neutral, smile, open_mouth, "
        "surprised, closed_eyes, wink_left, wink_right, angry, sad.\n"
        "- framing: full_body, knees_up, cowboy_shot, upper_body, bust, headshot. position.x: horizontal centre (0..1), "
        "position.scale: size multiplier (1 = fill frame).");
}

} // namespace lumen::pose
