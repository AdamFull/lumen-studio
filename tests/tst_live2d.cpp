#include "live2d/CubismCore.h"
#include "live2d/CubismModel.h"
#include "live2d/CubismRenderer.h"
#include "live2d/Motion.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>
#include <cstring>

using namespace lumen::live2d;

// ---- A fake Cubism Core with seven quads -----------------------------------------------------------------
//  0 Back      [-1,1]^2, texture 0 (red top half, dark red bottom half), order 0
//  1 Eye       0.4 wide, moves with ParamAngleX, texture 1 (blue), order 3
//  2 Glow      additive blue over the back, order 1
//  3 Top band  green, clipped by mask 4 (left half)
//  4 Mask      invisible, x in [-1, 0]
//  5 Low band  yellow, clipped by mask 4 inverted (right half)
//  6 Hidden    visible flag off
namespace fake {

constexpr int kDrawables = 7;
float params[1] = {0.0f};
const float paramMin[1] = {-30.0f}, paramMax[1] = {30.0f}, paramDef[1] = {0.0f};
const char *paramIds[1] = {"ParamAngleX"};
const char *partIds[1] = {"PartRoot"};
float partOpacity[1] = {1.0f};
const char *drawableIds[kDrawables] = {"Back", "Eye", "Glow", "TopBand", "Mask", "LowBand", "Hidden"};
const unsigned char constantFlags[kDrawables] = {0, 0, CsmBlendAdditive, 0, 0, CsmIsInvertedMask, 0};
const unsigned char dynamicFlags[kDrawables] = {CsmIsVisible, CsmIsVisible, CsmIsVisible, CsmIsVisible, 0, CsmIsVisible, 0};
const int textureIndices[kDrawables] = {0, 1, 1, 2, 2, 3, 1};
const int renderOrders[kDrawables] = {0, 5, 1, 2, 6, 3, 4};
const float opacities[kDrawables] = {1, 1, 1, 1, 1, 1, 1};
const int maskCounts[kDrawables] = {0, 0, 0, 1, 0, 1, 0};
const int maskOfBands[1] = {4};
const int *masks[kDrawables] = {nullptr, nullptr, nullptr, maskOfBands, nullptr, maskOfBands, nullptr};
const int vertexCounts[kDrawables] = {4, 4, 4, 4, 4, 4, 4};
CsmVector2 positions[kDrawables][4];
const CsmVector2 *positionPtrs[kDrawables] = {positions[0], positions[1], positions[2], positions[3], positions[4], positions[5], positions[6]};
const CsmVector2 quadUv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
const CsmVector2 *uvPtrs[kDrawables] = {quadUv, quadUv, quadUv, quadUv, quadUv, quadUv, quadUv};
const int indexCounts[kDrawables] = {6, 6, 6, 6, 6, 6, 6};
const unsigned short quadIndices[6] = {0, 1, 2, 0, 2, 3};
const unsigned short *indexPtrs[kDrawables] = {quadIndices, quadIndices, quadIndices, quadIndices, quadIndices, quadIndices, quadIndices};
bool consistent = true;
int updates = 0;

void quad(int i, float x0, float y0, float x1, float y1)
{
    positions[i][0] = {x0, y0};
    positions[i][1] = {x1, y0};
    positions[i][2] = {x1, y1};
    positions[i][3] = {x0, y1};
}

unsigned getVersion() { return 0x05000000; }
int hasMocConsistency(void *, unsigned) { return consistent ? 1 : 0; }
void *reviveMocInPlace(void *address, unsigned size)
{
    if (reinterpret_cast<std::uintptr_t>(address) % kMocAlignment != 0 || size < 4 || std::memcmp(address, "MOC3", 4) != 0)
        return nullptr;
    return address;
}
unsigned getSizeofModel(const void *) { return 256; }
void *initializeModelInPlace(const void *, void *address, unsigned)
{
    return reinterpret_cast<std::uintptr_t>(address) % kModelAlignment == 0 ? address : nullptr;
}
void updateModel(void *)
{
    ++updates;
    quad(0, -1, -1, 1, 1);
    const float dx = params[0] / 30.0f * 0.5f;
    quad(1, -0.2f + dx, -0.2f, 0.2f + dx, 0.2f);
    quad(2, 0.5f, -0.2f, 0.9f, 0.2f);
    quad(3, -1, 0.6f, 1, 0.9f);
    quad(4, -1, -1, 0, 1);
    quad(5, -1, -0.9f, 1, -0.6f);
    quad(6, -1, -1, 1, 1);
}
void readCanvasInfo(const void *, CsmVector2 *size, CsmVector2 *origin, float *ppu)
{
    *size = {200, 200};
    *origin = {100, 100};
    *ppu = 100;
}
int getParameterCount(const void *) { return 1; }
const char **getParameterIds(const void *) { return paramIds; }
const float *getMin(const void *) { return paramMin; }
const float *getMax(const void *) { return paramMax; }
const float *getDef(const void *) { return paramDef; }
float *getValues(void *) { return params; }
int getPartCount(const void *) { return 1; }
const char **getPartIds(const void *) { return partIds; }
float *getPartOpacities(void *) { return partOpacity; }
int getDrawableCount(const void *) { return kDrawables; }
const char **getDrawableIds(const void *) { return drawableIds; }
const unsigned char *getConstantFlags(const void *) { return constantFlags; }
const unsigned char *getDynamicFlags(const void *) { return dynamicFlags; }
const int *getTextureIndices(const void *) { return textureIndices; }
const int *getRenderOrders(const void *) { return renderOrders; }
const float *getOpacities(const void *) { return opacities; }
const int *getMaskCounts(const void *) { return maskCounts; }
const int **getMasks(const void *) { return masks; }
const int *getVertexCounts(const void *) { return vertexCounts; }
const CsmVector2 **getPositions(const void *) { return positionPtrs; }
const CsmVector2 **getUvs(const void *) { return uvPtrs; }
const int *getIndexCounts(const void *) { return indexCounts; }
const unsigned short **getIndices(const void *) { return indexPtrs; }
void resetDynamicFlags(void *) {}

CoreApi api()
{
    CoreApi a;
    a.getVersion = getVersion;
    a.hasMocConsistency = hasMocConsistency;
    a.reviveMocInPlace = reviveMocInPlace;
    a.getSizeofModel = getSizeofModel;
    a.initializeModelInPlace = initializeModelInPlace;
    a.updateModel = updateModel;
    a.readCanvasInfo = readCanvasInfo;
    a.getParameterCount = getParameterCount;
    a.getParameterIds = getParameterIds;
    a.getParameterMinimumValues = getMin;
    a.getParameterMaximumValues = getMax;
    a.getParameterDefaultValues = getDef;
    a.getParameterValues = getValues;
    a.getPartCount = getPartCount;
    a.getPartIds = getPartIds;
    a.getPartOpacities = getPartOpacities;
    a.getDrawableCount = getDrawableCount;
    a.getDrawableIds = getDrawableIds;
    a.getDrawableConstantFlags = getConstantFlags;
    a.getDrawableDynamicFlags = getDynamicFlags;
    a.getDrawableTextureIndices = getTextureIndices;
    a.getDrawableRenderOrders = getRenderOrders;
    a.getDrawableOpacities = getOpacities;
    a.getDrawableMaskCounts = getMaskCounts;
    a.getDrawableMasks = getMasks;
    a.getDrawableVertexCounts = getVertexCounts;
    a.getDrawableVertexPositions = getPositions;
    a.getDrawableVertexUvs = getUvs;
    a.getDrawableIndexCounts = getIndexCounts;
    a.getDrawableIndices = getIndices;
    a.resetDrawableDynamicFlags = resetDynamicFlags;
    return a;
}

} // namespace fake

namespace {

void writeJson(const QString &path, const QJsonObject &o)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(o).toJson());
}

QImage solid(const QColor &c)
{
    QImage im(16, 16, QImage::Format_ARGB32);
    im.fill(c);
    return im;
}

} // namespace

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

private:
    void makeModel(const QString &dir);
    QTemporaryDir m_dir;
    QString m_model3;
};

void TestLive2D::makeModel(const QString &dir)
{
    QDir().mkpath(dir + QStringLiteral("/tex"));
    QFile moc(dir + QStringLiteral("/fake.moc3"));
    QVERIFY(moc.open(QIODevice::WriteOnly));
    moc.write("MOC3 fake model data");
    moc.close();
    QImage back(16, 16, QImage::Format_ARGB32);
    back.fill(QColor(120, 0, 0));
    QPainter p(&back);
    p.fillRect(QRect(0, 0, 16, 8), QColor(255, 0, 0)); // top half bright
    p.end();
    QVERIFY(back.save(dir + QStringLiteral("/tex/back.png")));
    QVERIFY(solid(QColor(0, 0, 255)).save(dir + QStringLiteral("/tex/blue.png")));
    QVERIFY(solid(QColor(0, 255, 0)).save(dir + QStringLiteral("/tex/green.png")));
    QVERIFY(solid(QColor(255, 255, 0)).save(dir + QStringLiteral("/tex/yellow.png")));
    writeJson(dir + QStringLiteral("/idle.motion3.json"),
              QJsonObject{{QStringLiteral("Version"), 3},
                          {QStringLiteral("Meta"), QJsonObject{{QStringLiteral("Duration"), 2.0}, {QStringLiteral("Loop"), true}}},
                          {QStringLiteral("Curves"), QJsonArray{QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                                            {QStringLiteral("Id"), QStringLiteral("ParamAngleX")},
                                                                            {QStringLiteral("Segments"), QJsonArray{0, -30, 0, 2, 30}}}}}});
    m_model3 = dir + QStringLiteral("/fake.model3.json");
    writeJson(m_model3, QJsonObject{
        {QStringLiteral("Version"), 3},
        {QStringLiteral("FileReferences"), QJsonObject{
            {QStringLiteral("Moc"), QStringLiteral("fake.moc3")},
            {QStringLiteral("Textures"), QJsonArray{QStringLiteral("tex/back.png"), QStringLiteral("tex/blue.png"),
                                                    QStringLiteral("tex/green.png"), QStringLiteral("tex/yellow.png")}},
            {QStringLiteral("Physics"), QStringLiteral("fake.physics3.json")},
            {QStringLiteral("Motions"), QJsonObject{{QStringLiteral("Idle"), QJsonArray{QJsonObject{{QStringLiteral("File"), QStringLiteral("idle.motion3.json")}}}}}}}},
        {QStringLiteral("Groups"), QJsonArray{QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                          {QStringLiteral("Name"), QStringLiteral("EyeBlink")},
                                                          {QStringLiteral("Ids"), QJsonArray{QStringLiteral("ParamEyeLOpen"), QStringLiteral("ParamEyeROpen")}}}}}});
}

void TestLive2D::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    makeModel(m_dir.path());
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

QTEST_MAIN(TestLive2D)
#include "tst_live2d.moc"
