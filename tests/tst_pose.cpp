#include "pose/PoseEditing.h"
#include "pose/PosePresets.h"
#include "pose/PoseRenderer.h"
#include "pose/Rig.h"

#include "core/JsonSchema.h"

#include <QDir>
#include <QPainter>
#include <QtTest>

#include <cmath>

using namespace lumen::pose;

class TestPose : public QObject
{
    Q_OBJECT
private slots:
    void openPoseJsonRoundTrip();
    void normalisedJsonImport();
    void rotateKeepsBoneLength();
    void ikReachesTarget();
    void mirrorSwapsSides();
    void rigFrontViewIsSymmetric();
    void rigProfileHidesFarEye();
    void rigJsonRoundTrip();
    void rigSchemaAcceptsDefaults();
    void rendererUsesOpenPoseColours();
    void presetsRender();
};

static double dist(const QPointF &a, const QPointF &b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

void TestPose::openPoseJsonRoundTrip()
{
    Pose pose = solvePose({RigParams()}, QSize(512, 768));
    const QJsonObject json = pose.toOpenPoseJson();
    auto back = Pose::fromOpenPoseJson(json);
    QVERIFY(back.has_value());
    QCOMPARE(back->canvas, pose.canvas);
    QCOMPARE(back->people.size(), 1);
    for (int i = 0; i < kBodyCount; ++i) {
        QVERIFY(dist(back->people[0].body[i].pos, pose.people[0].body[i].pos) < 1e-6);
        QCOMPARE(back->people[0].body[i].visible, pose.people[0].body[i].visible);
    }
    QCOMPARE(back->people[0].face.size(), kFaceCount);
    QCOMPARE(back->people[0].leftHand.size(), kHandCount);
}

void TestPose::normalisedJsonImport()
{
    QJsonArray body;
    for (int i = 0; i < kBodyCount; ++i) {
        body.append(0.5);
        body.append(0.1 + i * 0.04);
        body.append(1.0);
    }
    QJsonObject person{{QStringLiteral("pose_keypoints_2d"), body}};
    QJsonObject doc{{QStringLiteral("people"), QJsonArray{person}},
                    {QStringLiteral("canvas_width"), 400},
                    {QStringLiteral("canvas_height"), 800}};
    auto pose = Pose::fromOpenPoseJson(doc);
    QVERIFY(pose.has_value());
    QCOMPARE(pose->people[0].body[0].pos, QPointF(200, 80));
}

void TestPose::rotateKeepsBoneLength()
{
    Person p = solveRig(RigParams(), QSize(512, 768));
    const double upper = dist(p.body[LShoulder].pos, p.body[LElbow].pos);
    const double fore = dist(p.body[LElbow].pos, p.body[LWrist].pos);
    dragJoint(p, {Part::Body, LElbow}, p.body[LShoulder].pos + QPointF(100, -20), DragMode::Rotate);
    QVERIFY(std::abs(dist(p.body[LShoulder].pos, p.body[LElbow].pos) - upper) < 1e-6);
    QVERIFY(std::abs(dist(p.body[LElbow].pos, p.body[LWrist].pos) - fore) < 1e-6);
    // Hand moved along with the wrist.
    QVERIFY(dist(p.leftHand[0].pos, p.body[LWrist].pos) < 1e-6);
}

void TestPose::ikReachesTarget()
{
    Person p = solveRig(RigParams(), QSize(512, 768));
    const double upper = dist(p.body[RShoulder].pos, p.body[RElbow].pos);
    const double fore = dist(p.body[RElbow].pos, p.body[RWrist].pos);
    const QPointF target = p.body[RShoulder].pos + QPointF(-upper * 0.6, -fore * 0.9);
    dragJoint(p, {Part::Body, RWrist}, target, DragMode::Chain);
    QVERIFY(dist(p.body[RWrist].pos, target) < 0.01);
    QVERIFY(std::abs(dist(p.body[RShoulder].pos, p.body[RElbow].pos) - upper) < 1e-6);
    QVERIFY(std::abs(dist(p.body[RElbow].pos, p.body[RWrist].pos) - fore) < 1e-6);
    QVERIFY(dist(p.rightHand[0].pos, p.body[RWrist].pos) < 1e-6);
}

void TestPose::mirrorSwapsSides()
{
    RigParams r;
    r.rightArm = LimbParams{100, 90, 70, -90};
    Person p = solveRig(r, QSize(512, 768));
    const QPointF rightWrist = p.body[RWrist].pos;
    const double axis = 256.0;
    p.mirror(axis);
    QVERIFY(dist(p.body[LWrist].pos, QPointF(2 * axis - rightWrist.x(), rightWrist.y())) < 1e-6);
}

void TestPose::rigFrontViewIsSymmetric()
{
    RigParams r;
    r.perspective = 0.0;
    Person p = solveRig(r, QSize(512, 768));
    const double cx = p.body[Neck].pos.x();
    QVERIFY(std::abs((p.body[LShoulder].pos.x() - cx) + (p.body[RShoulder].pos.x() - cx)) < 0.5);
    // Character's left appears on the viewer's right.
    QVERIFY(p.body[LShoulder].pos.x() > p.body[RShoulder].pos.x());
    // Head above neck above hips above ankles.
    QVERIFY(p.body[Nose].pos.y() < p.body[Neck].pos.y());
    QVERIFY(p.body[Neck].pos.y() < p.body[LHip].pos.y());
    QVERIFY(p.body[LHip].pos.y() < p.body[LAnkle].pos.y());
    // Fits in canvas.
    const QRectF b = p.bounds();
    QVERIFY(b.top() >= 0 && b.bottom() <= 768 && b.left() >= 0 && b.right() <= 512);
}

void TestPose::rigProfileHidesFarEye()
{
    RigParams r;
    r.yaw = 90;
    Person p = solveRig(r, QSize(512, 768));
    QVERIFY(p.body[REye].visible);
    QVERIFY(!p.body[LEye].visible);
    QVERIFY(!p.body[LEar].visible);
    r.yaw = 180;
    Person back = solveRig(r, QSize(512, 768));
    QVERIFY(!back.body[Nose].visible);
    QVERIFY(back.face.isEmpty());
}

void TestPose::rigJsonRoundTrip()
{
    RigParams r;
    r.yaw = 33;
    r.leftArm = LimbParams{40, 80, 30, 15};
    r.expression = QStringLiteral("smile");
    r.framing = QStringLiteral("bust");
    const RigParams back = RigParams::fromJson(r.toJson());
    QCOMPARE(back.yaw, 33.0);
    QCOMPARE(back.leftArm.direction, 80.0);
    QCOMPARE(back.leftArm.bendTwist, 15.0);
    QCOMPARE(back.expression, QStringLiteral("smile"));
    QCOMPARE(back.framing, QStringLiteral("bust"));
}

void TestPose::rigSchemaAcceptsDefaults()
{
    lumen::json::SchemaValidator v(rigParamsSchema());
    QJsonObject json = RigParams().toJson();
    json.remove(QStringLiteral("include_hands"));
    json.remove(QStringLiteral("include_face"));
    QStringList errors;
    QVERIFY2(v.validate(json, &errors), qPrintable(errors.join(QLatin1Char('\n'))));
    json.insert(QStringLiteral("framing"), QStringLiteral("nonsense"));
    QVERIFY(!v.validate(json));
}

void TestPose::rendererUsesOpenPoseColours()
{
    Pose pose = solvePose({RigParams()}, QSize(512, 768));
    QImage img = renderOpenPose(pose, QSize(512, 768));
    const QPoint neck = pose.people[0].body[Neck].pos.toPoint();
    QCOMPARE(img.pixelColor(neck), bodyColor(Neck));
    QCOMPARE(img.pixelColor(2, 2), QColor(Qt::black));
}

void TestPose::presetsRender()
{
    const QString outDir = QDir::temp().filePath(QStringLiteral("lumen_pose_presets"));
    QDir().mkpath(outDir);
    const auto &presets = builtInPresets();
    QVERIFY(presets.size() >= 20);
    // Contact sheet for visual inspection.
    const int cols = 8;
    const QSize cell(256, 384);
    const int rows = int((presets.size() + cols - 1) / cols);
    QImage sheet(cell.width() * cols, cell.height() * rows, QImage::Format_RGB32);
    sheet.fill(Qt::black);
    QPainter p(&sheet);
    for (int i = 0; i < presets.size(); ++i) {
        const PosePreset &preset = presets[i];
        const QSize canvas = preset.category == QLatin1String("Sheets") ? QSize(1024, 512) : QSize(512, 768);
        const Pose pose = preset.toPose(canvas);
        QVERIFY(!pose.isEmpty());
        const QImage img = renderOpenPose(pose, canvas);
        img.save(outDir + u'/' + preset.id + QStringLiteral(".png"));
        const QRect cellRect((i % cols) * cell.width(), (i / cols) * cell.height(), cell.width(), cell.height());
        const QImage scaled = img.scaled(cell, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage(cellRect.topLeft() + QPoint((cell.width() - scaled.width()) / 2, 0), scaled);
        p.setPen(Qt::white);
        p.drawRect(cellRect.adjusted(0, 0, -1, -1));
        p.drawText(cellRect.adjusted(4, 0, -4, -4), Qt::AlignBottom | Qt::AlignLeft, preset.name);
    }
    p.end();
    sheet.save(outDir + QStringLiteral("/_contact_sheet.png"));
}

QTEST_MAIN(TestPose)
#include "tst_pose.moc"
