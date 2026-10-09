#pragma once

// A fake Live2D Cubism Core (function table) with a small synthetic model, for tests that must run
// without the proprietary library.

#include "live2d/CubismCore.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <cstdint>
#include <cstring>

using lumen::live2d::CoreApi;
using lumen::live2d::CsmVector2;
using lumen::live2d::kMocAlignment;
using lumen::live2d::kModelAlignment;
using lumen::live2d::CsmBlendAdditive;
using lumen::live2d::CsmIsInvertedMask;
using lumen::live2d::CsmIsVisible;

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
// ParamHairFront has no geometry; it is driven by the physics in fake.physics3.json.
float params[2] = {0.0f, 0.0f};
const float paramMin[2] = {-30.0f, -1.0f}, paramMax[2] = {30.0f, 1.0f}, paramDef[2] = {0.0f, 0.0f};
const char *paramIds[2] = {"ParamAngleX", "ParamHairFront"};
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
int getParameterCount(const void *) { return 2; }
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

namespace fake {

inline bool writeJson(const QString &path, const QJsonObject &o)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson()) > 0;
}

inline QImage solid(const QColor &c)
{
    QImage im(16, 16, QImage::Format_ARGB32);
    im.fill(c);
    return im;
}

// One strand: ParamAngleX moves its root sideways, the swing drives ParamHairFront.
inline QJsonObject physicsJson(double fps = 30.0, double scale = 1.5, double weight = 100.0)
{
    auto vertex = [](double mobility, double delay, double acceleration, double radius) {
        return QJsonObject{{QStringLiteral("Position"), QJsonObject{{QStringLiteral("X"), 0}, {QStringLiteral("Y"), radius}}},
                           {QStringLiteral("Mobility"), mobility}, {QStringLiteral("Delay"), delay},
                           {QStringLiteral("Acceleration"), acceleration}, {QStringLiteral("Radius"), radius}};
    };
    auto range = [] { return QJsonObject{{QStringLiteral("Minimum"), -10}, {QStringLiteral("Default"), 0}, {QStringLiteral("Maximum"), 10}}; };
    const QJsonObject forces{{QStringLiteral("Gravity"), QJsonObject{{QStringLiteral("X"), 0}, {QStringLiteral("Y"), -1}}},
                             {QStringLiteral("Wind"), QJsonObject{{QStringLiteral("X"), 0}, {QStringLiteral("Y"), 0}}}};
    const QJsonObject meta{{QStringLiteral("PhysicsSettingCount"), 1},
                           {QStringLiteral("Fps"), fps},
                           {QStringLiteral("EffectiveForces"), forces},
                           {QStringLiteral("PhysicsDictionary"), QJsonArray{QJsonObject{{QStringLiteral("Id"), QStringLiteral("PhysicsSetting1")},
                                                                                         {QStringLiteral("Name"), QStringLiteral("Front hair")}}}}};
    const QJsonObject input{{QStringLiteral("Source"), QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                                   {QStringLiteral("Id"), QStringLiteral("ParamAngleX")}}},
                            {QStringLiteral("Weight"), 100},
                            {QStringLiteral("Type"), QStringLiteral("X")},
                            {QStringLiteral("Reflect"), false}};
    const QJsonObject output{{QStringLiteral("Destination"), QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                                         {QStringLiteral("Id"), QStringLiteral("ParamHairFront")}}},
                             {QStringLiteral("VertexIndex"), 1},
                             {QStringLiteral("Scale"), scale},
                             {QStringLiteral("Weight"), weight},
                             {QStringLiteral("Type"), QStringLiteral("Angle")},
                             {QStringLiteral("Reflect"), false}};
    const QJsonObject setting{{QStringLiteral("Id"), QStringLiteral("PhysicsSetting1")},
                              {QStringLiteral("Input"), QJsonArray{input}},
                              {QStringLiteral("Output"), QJsonArray{output}},
                              {QStringLiteral("Vertices"), QJsonArray{vertex(1, 1, 1, 0), vertex(0.95, 0.9, 1.5, 15)}},
                              {QStringLiteral("Normalization"), QJsonObject{{QStringLiteral("Position"), range()}, {QStringLiteral("Angle"), range()}}}};
    return QJsonObject{{QStringLiteral("Version"), 3}, {QStringLiteral("Meta"), meta}, {QStringLiteral("PhysicsSettings"), QJsonArray{setting}}};
}

// Writes fake.model3.json (moc, four textures, an Idle motion, EyeBlink group); returns its path.
inline QString writeModel(const QString &dir)
{
    QDir().mkpath(dir + QStringLiteral("/tex"));
    QFile moc(dir + QStringLiteral("/fake.moc3"));
    if (!moc.open(QIODevice::WriteOnly))
        return {};
    moc.write("MOC3 fake model data");
    moc.close();
    QImage back(16, 16, QImage::Format_ARGB32);
    back.fill(QColor(120, 0, 0));
    QPainter p(&back);
    p.fillRect(QRect(0, 0, 16, 8), QColor(255, 0, 0)); // top half bright
    p.end();
    bool ok = back.save(dir + QStringLiteral("/tex/back.png"));
    ok = ok && solid(QColor(0, 0, 255)).save(dir + QStringLiteral("/tex/blue.png"));
    ok = ok && solid(QColor(0, 255, 0)).save(dir + QStringLiteral("/tex/green.png"));
    ok = ok && solid(QColor(255, 255, 0)).save(dir + QStringLiteral("/tex/yellow.png"));
    ok = ok && writeJson(dir + QStringLiteral("/idle.motion3.json"),
                         QJsonObject{{QStringLiteral("Version"), 3},
                                     {QStringLiteral("Meta"), QJsonObject{{QStringLiteral("Duration"), 2.0}, {QStringLiteral("Loop"), true}}},
                                     {QStringLiteral("Curves"), QJsonArray{QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                                                       {QStringLiteral("Id"), QStringLiteral("ParamAngleX")},
                                                                                       {QStringLiteral("Segments"), QJsonArray{0, -30, 0, 2, 30}}}}}});
    ok = ok && writeJson(dir + QStringLiteral("/fake.physics3.json"), physicsJson());
    auto constantMotion = [](double value, double duration, bool loop) {
        return QJsonObject{{QStringLiteral("Version"), 3},
                           {QStringLiteral("Meta"), QJsonObject{{QStringLiteral("Duration"), duration}, {QStringLiteral("Loop"), loop}}},
                           {QStringLiteral("Curves"), QJsonArray{QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                                             {QStringLiteral("Id"), QStringLiteral("ParamAngleX")},
                                                                             {QStringLiteral("Segments"), QJsonArray{0, value, 0, duration, value}}}}}};
    };
    ok = ok && writeJson(dir + QStringLiteral("/idle2.motion3.json"), constantMotion(-5, 2.0, true));
    ok = ok && writeJson(dir + QStringLiteral("/tap.motion3.json"), constantMotion(20, 1.0, false));
    ok = ok && writeJson(dir + QStringLiteral("/smile.exp3.json"),
                         QJsonObject{{QStringLiteral("Type"), QStringLiteral("Live2D Expression")},
                                     {QStringLiteral("FadeInTime"), 0.1},
                                     {QStringLiteral("FadeOutTime"), 0.1},
                                     {QStringLiteral("Parameters"), QJsonArray{QJsonObject{{QStringLiteral("Id"), QStringLiteral("ParamAngleX")},
                                                                                           {QStringLiteral("Value"), 5},
                                                                                           {QStringLiteral("Blend"), QStringLiteral("Add")}}}}});
    ok = ok && writeJson(dir + QStringLiteral("/fake.pose3.json"),
                         QJsonObject{{QStringLiteral("Type"), QStringLiteral("Live2D Pose")},
                                     {QStringLiteral("Groups"), QJsonArray{QJsonArray{QJsonObject{{QStringLiteral("Id"), QStringLiteral("PartRoot")},
                                                                                                  {QStringLiteral("Link"), QJsonArray{}}}}}}});
    const QString model3 = dir + QStringLiteral("/fake.model3.json");
    ok = ok && writeJson(model3, QJsonObject{
        {QStringLiteral("Version"), 3},
        {QStringLiteral("FileReferences"), QJsonObject{
            {QStringLiteral("Moc"), QStringLiteral("fake.moc3")},
            {QStringLiteral("Textures"), QJsonArray{QStringLiteral("tex/back.png"), QStringLiteral("tex/blue.png"),
                                                    QStringLiteral("tex/green.png"), QStringLiteral("tex/yellow.png")}},
            {QStringLiteral("Physics"), QStringLiteral("fake.physics3.json")},
            {QStringLiteral("Pose"), QStringLiteral("fake.pose3.json")},
            {QStringLiteral("Expressions"), QJsonArray{QJsonObject{{QStringLiteral("Name"), QStringLiteral("smile")},
                                                                   {QStringLiteral("File"), QStringLiteral("smile.exp3.json")}}}},
            {QStringLiteral("Motions"), QJsonObject{
                {QStringLiteral("TapBody"), QJsonArray{QJsonObject{{QStringLiteral("File"), QStringLiteral("tap.motion3.json")},
                                                                   {QStringLiteral("FadeInTime"), 0.1},
                                                                   {QStringLiteral("FadeOutTime"), 0.1}}}},
                {QStringLiteral("Idle"), QJsonArray{QJsonObject{{QStringLiteral("File"), QStringLiteral("idle.motion3.json")}},
                                                    QJsonObject{{QStringLiteral("File"), QStringLiteral("idle2.motion3.json")}}}}}}}},
        {QStringLiteral("Groups"), QJsonArray{QJsonObject{{QStringLiteral("Target"), QStringLiteral("Parameter")},
                                                          {QStringLiteral("Name"), QStringLiteral("EyeBlink")},
                                                          {QStringLiteral("Ids"), QJsonArray{QStringLiteral("ParamEyeLOpen"), QStringLiteral("ParamEyeROpen")}}}}}});
    return ok ? model3 : QString();
}

} // namespace fake
