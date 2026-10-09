#include "pose/Anatomy.h"

#include <cmath>

namespace lumen::pose {

namespace {

constexpr double kDeg = M_PI / 180.0;

struct FingerSpec
{
    QPointF base;       // MCP position (u, v)
    double angle;       // in-plane direction (deg) relative to the hand axis, positive toward the thumb
    double seg[3];      // proximal, middle, distal lengths
};

const FingerSpec kFingers[4] = {
    {{0.45, 0.15}, 6.0, {0.21, 0.12, 0.10}},   // index
    {{0.47, 0.03}, 0.0, {0.23, 0.13, 0.11}},   // middle
    {{0.45, -0.08}, -6.0, {0.21, 0.12, 0.10}}, // ring
    {{0.40, -0.18}, -13.0, {0.16, 0.09, 0.08}} // pinky
};

struct Curl
{
    double a, b, c; // cumulative flexion (deg) at MCP, PIP, DIP
    double spread;  // extra in-plane spread multiplier
};

// Projected finger (palm facing the viewer): flexion folds the finger toward the viewer,
// which foreshortens each segment by cos(cumulative angle).
void finger(std::array<QPointF, 21> &out, int firstIndex, const FingerSpec &spec, const Curl &curl)
{
    out[static_cast<size_t>(firstIndex)] = spec.base;
    const double dirAngle = spec.angle * curl.spread * kDeg;
    const QPointF dir(std::cos(dirAngle), std::sin(dirAngle));
    double cumulative = 0.0;
    const double flex[3] = {curl.a, curl.b, curl.c};
    QPointF p = spec.base;
    for (int i = 0; i < 3; ++i) {
        cumulative += flex[i];
        p += dir * (spec.seg[i] * std::cos(cumulative * kDeg));
        out[static_cast<size_t>(firstIndex + 1 + i)] = p;
    }
}

void thumb(std::array<QPointF, 21> &out, double inPlaneDeg, double curlDeg)
{
    const QPointF cmc(0.10, 0.16);
    out[1] = cmc;
    const double seg[3] = {0.17, 0.13, 0.11};
    double angle = inPlaneDeg;
    QPointF p = cmc;
    for (int i = 0; i < 3; ++i) {
        const double a = angle * kDeg;
        p += QPointF(std::cos(a), std::sin(a)) * seg[i];
        out[static_cast<size_t>(2 + i)] = p;
        angle -= curlDeg; // curling bends the thumb across the palm
    }
}

} // namespace

QStringList handShapeNames()
{
    return {QStringLiteral("relaxed"), QStringLiteral("open"), QStringLiteral("fist"), QStringLiteral("point"),
            QStringLiteral("peace"), QStringLiteral("thumbs_up"), QStringLiteral("grip")};
}

QString handShapeName(HandShape shape)
{
    return handShapeNames().value(static_cast<int>(shape));
}

HandShape handShapeFromName(const QString &name, HandShape fallback)
{
    const int i = handShapeNames().indexOf(name.trimmed().toLower());
    return i >= 0 ? static_cast<HandShape>(i) : fallback;
}

std::array<QPointF, 21> handTemplate(HandShape shape)
{
    std::array<QPointF, 21> pts{};
    pts[0] = QPointF(0, 0);
    const Curl open{0, 0, 0, 1.6};
    const Curl relaxed{18, 38, 52, 1.0};
    const Curl fist{85, 185, 245, 0.6};
    const Curl grip{45, 95, 125, 0.8};
    Curl curls[4] = {relaxed, relaxed, relaxed, relaxed};
    double thumbAngle = 50.0, thumbCurl = 10.0;
    switch (shape) {
    case HandShape::Relaxed:
        thumbAngle = 42.0;
        thumbCurl = 12.0;
        break;
    case HandShape::Open:
        for (Curl &c : curls)
            c = open;
        thumbAngle = 62.0;
        thumbCurl = 4.0;
        break;
    case HandShape::Fist:
        for (Curl &c : curls)
            c = fist;
        thumbAngle = 20.0;
        thumbCurl = 55.0;
        break;
    case HandShape::Point:
        curls[0] = open;
        curls[1] = curls[2] = curls[3] = fist;
        thumbAngle = 22.0;
        thumbCurl = 50.0;
        break;
    case HandShape::Peace:
        curls[0] = curls[1] = Curl{0, 0, 0, 2.6};
        curls[2] = curls[3] = fist;
        thumbAngle = 20.0;
        thumbCurl = 55.0;
        break;
    case HandShape::ThumbsUp:
        for (Curl &c : curls)
            c = fist;
        thumbAngle = 95.0;
        thumbCurl = -5.0;
        break;
    case HandShape::Grip:
        for (Curl &c : curls)
            c = grip;
        thumbAngle = 35.0;
        thumbCurl = 30.0;
        break;
    }
    thumb(pts, thumbAngle, thumbCurl);
    for (int f = 0; f < 4; ++f)
        finger(pts, 5 + f * 4, kFingers[f], curls[f]);
    return pts;
}

QStringList expressionNames()
{
    return {QStringLiteral("neutral"), QStringLiteral("smile"), QStringLiteral("open_mouth"),
            QStringLiteral("surprised"), QStringLiteral("closed_eyes"), QStringLiteral("wink_left"),
            QStringLiteral("wink_right"), QStringLiteral("angry"), QStringLiteral("sad")};
}

QString expressionName(Expression e)
{
    return expressionNames().value(static_cast<int>(e));
}

Expression expressionFromName(const QString &name, Expression fallback)
{
    const int i = expressionNames().indexOf(name.trimmed().toLower());
    return i >= 0 ? static_cast<Expression>(i) : fallback;
}

namespace {

double surfaceDepth(double fx, double fy)
{
    // Front surface of a head-shaped ellipsoid (flatter at the sides).
    const double r = 1.0 - fx * fx * 0.92 - fy * fy * 0.35;
    return 0.85 * std::sqrt(std::max(0.04, r));
}

QVector3D onFace(double fx, double fy, double extraDepth = 0.0)
{
    return QVector3D(float(fx), float(fy), float(surfaceDepth(fx, fy) + extraDepth));
}

} // namespace

std::array<QVector3D, 70> faceTemplate(Expression e)
{
    std::array<QVector3D, 70> f{};
    // Jaw 0..16: starts at the character's right temple (x < 0), runs around the chin.
    for (int t = 0; t <= 16; ++t) {
        const double a = M_PI * t / 16.0;
        const double fx = -std::cos(a) * 0.96;
        const double fy = 0.12 - std::sin(a) * 0.98;
        f[static_cast<size_t>(t)] = onFace(fx, fy);
    }
    double browLift = 0.0, browInner = 0.0;
    if (e == Expression::Surprised)
        browLift = 0.09;
    if (e == Expression::Angry)
        browInner = -0.07;
    if (e == Expression::Sad)
        browInner = 0.06;
    for (int k = 0; k < 5; ++k) {
        const double fxR = -0.78 + k * 0.15; // right brow, outer → inner
        const double arch = 0.36 + 0.06 * std::sin(M_PI * k / 4.0);
        const double inner = browInner * (k / 4.0);
        f[static_cast<size_t>(17 + k)] = onFace(fxR, arch + browLift + inner, 0.03);
        const double fxL = 0.18 + k * 0.15; // left brow, inner → outer
        const double archL = 0.36 + 0.06 * std::sin(M_PI * (4 - k) / 4.0);
        const double innerL = browInner * ((4 - k) / 4.0);
        f[static_cast<size_t>(22 + k)] = onFace(fxL, archL + browLift + innerL, 0.03);
    }
    // Nose bridge and tip.
    const double bridge[4] = {0.22, 0.08, -0.05, -0.18};
    for (int k = 0; k < 4; ++k)
        f[static_cast<size_t>(27 + k)] = onFace(0.0, bridge[k], 0.05 + 0.07 * k);
    // Nostrils.
    for (int k = 0; k < 5; ++k) {
        const double fx = -0.18 + k * 0.09;
        f[static_cast<size_t>(31 + k)] = onFace(fx, -0.27 + (k == 2 ? -0.02 : 0.0), 0.12 - std::abs(fx) * 0.3);
    }
    // Eyes.
    // dlib right eye (36..41): 36 outer corner (x most negative), 39 inner corner.
    {
        const bool closed = e == Expression::ClosedEyes || e == Expression::WinkRight;
        const double open = closed ? 0.004 : (e == Expression::Surprised ? 0.075 : 0.05);
        const QPointF pts[6] = {{-0.52, 0.15}, {-0.43, 0.15 + open}, {-0.32, 0.15 + open},
                                {-0.24, 0.15}, {-0.32, 0.15 - open * 0.8}, {-0.43, 0.15 - open * 0.8}};
        for (int k = 0; k < 6; ++k)
            f[static_cast<size_t>(36 + k)] = onFace(pts[k].x(), pts[k].y(), -0.02);
    }
    {
        const bool closed = e == Expression::ClosedEyes || e == Expression::WinkLeft;
        const double open = closed ? 0.004 : (e == Expression::Surprised ? 0.075 : 0.05);
        const QPointF pts[6] = {{0.24, 0.15}, {0.32, 0.15 + open}, {0.43, 0.15 + open},
                                {0.52, 0.15}, {0.43, 0.15 - open * 0.8}, {0.32, 0.15 - open * 0.8}};
        for (int k = 0; k < 6; ++k)
            f[static_cast<size_t>(42 + k)] = onFace(pts[k].x(), pts[k].y(), -0.02);
    }
    // Mouth.
    double corner = 0.0, width = 1.0, open = 0.0, lowerDrop = 0.0;
    switch (e) {
    case Expression::Smile: corner = 0.06; width = 1.12; break;
    case Expression::OpenMouth: open = 0.12; break;
    case Expression::Surprised: open = 0.16; width = 0.8; break;
    case Expression::Angry: corner = -0.03; width = 0.95; break;
    case Expression::Sad: corner = -0.06; break;
    default: break;
    }
    lowerDrop = open;
    const double my = -0.5;
    const QPointF outer[12] = {
        {-0.30 * width, my + corner}, {-0.20 * width, my + 0.06}, {-0.08 * width, my + 0.09}, {0.0, my + 0.08},
        {0.08 * width, my + 0.09},    {0.20 * width, my + 0.06},  {0.30 * width, my + corner}, {0.20 * width, my - 0.08 - lowerDrop * 0.8},
        {0.08 * width, my - 0.11 - lowerDrop}, {0.0, my - 0.12 - lowerDrop}, {-0.08 * width, my - 0.11 - lowerDrop}, {-0.20 * width, my - 0.08 - lowerDrop * 0.8},
    };
    for (int k = 0; k < 12; ++k)
        f[static_cast<size_t>(48 + k)] = onFace(outer[k].x(), outer[k].y(), 0.04);
    const QPointF inner[8] = {
        {-0.26 * width, my + corner * 0.9}, {-0.08 * width, my + 0.03}, {0.0, my + 0.03}, {0.08 * width, my + 0.03},
        {0.26 * width, my + corner * 0.9},  {0.08 * width, my - 0.03 - open}, {0.0, my - 0.03 - open}, {-0.08 * width, my - 0.03 - open},
    };
    for (int k = 0; k < 8; ++k)
        f[static_cast<size_t>(60 + k)] = onFace(inner[k].x(), inner[k].y(), 0.03);
    // Pupils.
    f[68] = onFace(-0.38, 0.152, -0.01);
    f[69] = onFace(0.38, 0.152, -0.01);
    return f;
}

QVector3D headNoseLocal()
{
    return onFace(0.0, -0.18, 0.26);
}

QVector3D headEyeLocal(bool left)
{
    return onFace(left ? 0.38 : -0.38, 0.152, -0.01);
}

QVector3D headEarLocal(bool left)
{
    return QVector3D(left ? 1.0f : -1.0f, 0.08f, -0.25f);
}

} // namespace lumen::pose
