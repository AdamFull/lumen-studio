#include "FakeCubismCore.h"
#include "live2d/CubismCore.h"
#include "live2d/CubismModel.h"
#include "live2d/CubismRenderer.h"
#include "live2d/Motion.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <cstring>

using namespace lumen::live2d;

class TestLive2D : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void modelSettings();
    void motionCurves();
    void coreLoadErrors();
    void loadModel();
    void renderDrawables();
    void renderPerformance();

private:
    QTemporaryDir m_dir;
    QString m_model3;
};

void TestLive2D::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_model3 = fake::writeModel(m_dir.path());
    QVERIFY(!m_model3.isEmpty());
}

void TestLive2D::cleanupTestCase()
{
    CubismCore::instance().unload();
}

void TestLive2D::modelSettings()
{
    QString error;
    const auto s = ModelSettings::load(m_model3, &error);
    QVERIFY2(s.has_value(), qPrintable(error));
    QCOMPARE(QFileInfo(s->moc).fileName(), QStringLiteral("fake.moc3"));
    QVERIFY(QFileInfo(s->moc).isAbsolute());
    QCOMPARE(s->textures.size(), 4);
    QVERIFY(s->textures[1].endsWith(QStringLiteral("tex/blue.png")));
    QCOMPARE(QFileInfo(s->idleMotion()).fileName(), QStringLiteral("idle.motion3.json"));
    QCOMPARE(s->eyeBlinkIds, (QStringList{QStringLiteral("ParamEyeLOpen"), QStringLiteral("ParamEyeROpen")}));
    QVERIFY(!ModelSettings::fromJson(QJsonObject{}, m_dir.path(), &error).has_value());
    QVERIFY(!error.isEmpty());
}

void TestLive2D::motionCurves()
{
    // Linear 0->10 over [0,1], bezier 10->0 over [1,2], stepped to 5 at 3, inverse stepped to 7 at 4.
    const QJsonObject json{
        {QStringLiteral("Meta"), QJsonObject{{QStringLiteral("Duration"), 4.0}, {QStringLiteral("Loop"), true}}},
        {QStringLiteral("Curves"), QJsonArray{QJsonObject{
            {QStringLiteral("Target"), QStringLiteral("Parameter")},
            {QStringLiteral("Id"), QStringLiteral("P")},
            {QStringLiteral("Segments"), QJsonArray{0, 0, 0, 1, 10, 1, 1.333, 10, 1.667, 0, 2, 0, 2, 3, 5, 3, 4, 7}}}}}};
    QString error;
    const auto motion = Motion::fromJson(json, &error);
    QVERIFY2(motion.has_value(), qPrintable(error));
    const MotionCurve &c = motion->curves().first();
    QCOMPARE(c.segments.size(), 4);
    QCOMPARE(c.valueAt(-1), 0.0);
    QCOMPARE(c.valueAt(0.5), 5.0);
    QVERIFY(std::abs(c.valueAt(1.5) - 5.0) < 0.2); // symmetric bezier passes the midpoint
    QVERIFY(c.valueAt(1.1) > c.valueAt(1.9));
    QCOMPARE(c.valueAt(2.5), 0.0);  // stepped holds the start value
    QCOMPARE(c.valueAt(3.0), 5.0);
    QCOMPARE(c.valueAt(3.5), 7.0);  // inverse stepped jumps to the end value
    QCOMPARE(c.valueAt(10.0), 7.0);
    double seen = -1;
    motion->evaluate(4.5, [&seen](MotionCurve::Target, const QString &id, double v) {
        if (id == QLatin1String("P"))
            seen = v;
    });
    QCOMPARE(seen, 5.0); // loops back to t = 0.5
    QVERIFY(!Motion::fromJson(QJsonObject{{QStringLiteral("Curves"), QJsonArray{QJsonObject{
                                  {QStringLiteral("Segments"), QJsonArray{0, 0, 1, 1}}}}}}, &error).has_value());
}

void TestLive2D::coreLoadErrors()
{
    CubismCore &core = CubismCore::instance();
    core.unload();
    QString error;
    QVERIFY(!core.load(m_dir.filePath(QStringLiteral("missing/Live2DCubismCore.dll")), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!core.isLoaded());
    QVERIFY(!CubismModel::load(m_model3, &error));
    QVERIFY(error.contains(QStringLiteral("not loaded")));
    CoreApi incomplete;
    QVERIFY(incomplete.missingRequired().contains(QStringLiteral("csmUpdateModel")));
    QVERIFY(fake::api().missingRequired().isEmpty());
}

void TestLive2D::loadModel()
{
    CubismCore::instance().setApiForTesting(fake::api(), QStringLiteral("fake"));
    QCOMPARE(CubismCore::instance().versionString(), QStringLiteral("5.0.0"));
    QString error;
    fake::consistent = false;
    QVERIFY(!CubismModel::load(m_model3, &error));
    QVERIFY(error.contains(QStringLiteral("damaged")));
    fake::consistent = true;
    auto model = CubismModel::load(m_model3, &error);
    QVERIFY2(model, qPrintable(error));
    QCOMPARE(model->parameterIds(), QStringList{QStringLiteral("ParamAngleX")});
    QCOMPARE(model->textures().size(), 4);
    QVERIFY(model->setParameter(QStringLiteral("ParamAngleX"), 99.0f));
    QCOMPARE(model->parameter(0), 30.0f); // clamped to the maximum
    QVERIFY(!model->setParameter(QStringLiteral("ParamMissing"), 1.0f));
    model->resetParameters();
    QCOMPARE(model->parameter(0), 0.0f);
    QVERIFY(model->setPartOpacity(QStringLiteral("PartRoot"), 0.5f));
    const int before = fake::updates;
    const ModelFrame frame = model->update();
    QCOMPARE(fake::updates, before + 1);
    QCOMPARE(frame.drawables.size(), fake::kDrawables);
    QCOMPARE(frame.canvasSize, QSizeF(200, 200));
    QCOMPARE(frame.drawables[2].blend, DrawableFrame::Blend::Additive);
    QVERIFY(frame.drawables[5].invertedMask);
    QVERIFY(!frame.drawables[4].visible);
    QCOMPARE(frame.drawables[4].positions.size(), 4); // geometry kept because it is a mask
    QVERIFY(frame.drawables[6].positions.isEmpty());  // hidden and unused: skipped
}

void TestLive2D::renderDrawables()
{
    CubismCore::instance().setApiForTesting(fake::api(), QStringLiteral("fake"));
    QString error;
    auto model = CubismModel::load(m_model3, &error);
    QVERIFY2(model, qPrintable(error));
    RenderOptions options;
    options.size = QSize(200, 200);
    ModelFrame frame = model->update();
    const QImage img = renderFrame(frame, model->textures(), options);
    const QTransform t = frameTransform(frame, options);
    auto at = [&](const QImage &im, double x, double y) { return im.pixelColor(t.map(QPointF(x, y)).toPoint()); };
    QCOMPARE(at(img, -0.6, 0.3), QColor(255, 0, 0));   // back, upper half of its texture (v points up)
    QCOMPARE(at(img, -0.6, -0.3), QColor(120, 0, 0));  // back, lower half
    QCOMPARE(at(img, 0.0, 0.0), QColor(0, 0, 255));    // eye on top
    QCOMPARE(at(img, 0.7, 0.1), QColor(255, 0, 255));  // additive glow: red + blue
    QCOMPARE(at(img, -0.5, 0.75), QColor(0, 255, 0));  // band clipped to the left half
    QCOMPARE(at(img, 0.5, 0.75), QColor(255, 0, 0));   // outside the mask: back shows through
    QCOMPARE(at(img, 0.5, -0.75), QColor(255, 255, 0)); // inverted mask: right half
    QCOMPARE(at(img, -0.5, -0.75), QColor(120, 0, 0));
    QCOMPARE(img.pixelColor(1, 1).alpha(), 0);         // margin around the canvas

    // Parameters drive the geometry.
    model->setParameter(QStringLiteral("ParamAngleX"), 30.0f);
    frame = model->update();
    const QImage moved = renderFrame(frame, model->textures(), options);
    QCOMPARE(at(moved, 0.0, 0.1), QColor(255, 0, 0));  // the eye left the centre
    QCOMPARE(at(moved, 0.5, 0.0), QColor(0, 0, 255));

    // Idle motion from the model settings animates the same parameter.
    const auto idle = Motion::load(model->settings().idleMotion(), &error);
    QVERIFY2(idle.has_value(), qPrintable(error));
    idle->evaluate(0.5, [&model](MotionCurve::Target target, const QString &id, double v) {
        if (target == MotionCurve::Target::Parameter)
            model->setParameter(id, float(v));
    });
    QCOMPARE(model->parameter(0), -15.0f);

    // Background colour and zoom.
    options.background = Qt::white;
    options.zoom = 0.5;
    const QImage small = renderFrame(model->update(), model->textures(), options);
    QCOMPARE(small.pixelColor(5, 5), QColor(Qt::white));
    QCOMPARE(small.pixelColor(100, 60), QColor(255, 0, 0));
}

void TestLive2D::renderPerformance()
{
    // A model-sized load: 300 drawables with 10x10 grid meshes (60k triangles) over the canvas.
    ModelFrame frame;
    frame.canvasSize = QSizeF(2000, 2000);
    frame.origin = QPointF(1000, 1000);
    frame.pixelsPerUnit = 1000;
    QImage tex(512, 512, QImage::Format_ARGB32_Premultiplied);
    tex.fill(QColor(200, 150, 120, 220));
    for (int d = 0; d < 300; ++d) {
        DrawableFrame dr;
        dr.renderOrder = d;
        const double cx = std::sin(d * 1.7) * 0.6, cy = std::cos(d * 1.3) * 0.6, r = 0.15 + (d % 7) * 0.03;
        for (int y = 0; y <= 10; ++y)
            for (int x = 0; x <= 10; ++x) {
                dr.positions << QPointF(cx - r + 2 * r * x / 10.0, cy - r + 2 * r * y / 10.0);
                dr.uvs << QPointF(x / 10.0, y / 10.0);
            }
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) {
                const quint16 i = quint16(y * 11 + x);
                dr.indices << i << quint16(i + 1) << quint16(i + 12) << i << quint16(i + 12) << quint16(i + 11);
            }
        if (d % 10 == 5)
            dr.masks << d - 1;
        frame.drawables << dr;
    }
    RenderOptions options;
    options.size = QSize(800, 800);
    QElapsedTimer timer;
    timer.start();
    const QImage img = renderFrame(frame, {tex}, options);
    qInfo() << "300 drawables / 60k triangles at 800x800:" << timer.elapsed() << "ms";
    QVERIFY(!img.isNull());
    QVERIFY(img.pixelColor(400, 400).alpha() > 0);
}

QTEST_MAIN(TestLive2D)
#include "tst_live2d.moc"
