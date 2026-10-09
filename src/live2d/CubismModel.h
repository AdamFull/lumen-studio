#pragma once

#include "live2d/Motion.h"

#include <QImage>
#include <QPointF>
#include <QSizeF>
#include <QVector>

#include <memory>

namespace lumen::live2d {

struct DrawableFrame
{
    QString id;
    int texture = 0;
    float opacity = 1.0f;
    bool visible = true;
    int renderOrder = 0;
    enum class Blend { Normal, Additive, Multiplicative } blend = Blend::Normal;
    bool invertedMask = false;
    QVector<int> masks;
    QVector<QPointF> positions; // model units, y up
    QVector<QPointF> uvs;       // 0..1, v up
    QVector<quint16> indices;
    float multiply[3] = {1.0f, 1.0f, 1.0f};
    float screen[3] = {0.0f, 0.0f, 0.0f};
};

// Immutable copy of a model's drawables after an update; rendered off the GUI thread.
struct ModelFrame
{
    QSizeF canvasSize;    // pixels
    QPointF origin;       // pixels
    double pixelsPerUnit = 1.0;
    QList<DrawableFrame> drawables;
};

// A Cubism model instance: moc + model memory owned by this object, parameters and textures.
// Requires CubismCore::instance() to be loaded. GUI thread (the Core is not used concurrently).
class CubismModel
{
public:
    ~CubismModel();
    CubismModel(const CubismModel &) = delete;
    CubismModel &operator=(const CubismModel &) = delete;

    static std::unique_ptr<CubismModel> load(const QString &model3Path, QString *error = nullptr);

    const ModelSettings &settings() const { return m_settings; }
    const QList<QImage> &textures() const { return m_textures; } // premultiplied ARGB32

    int parameterCount() const { return int(m_parameterIds.size()); }
    const QStringList &parameterIds() const { return m_parameterIds; }
    int parameterIndex(const QString &id) const { return m_parameterIds.indexOf(id); }
    float parameterMinimum(int index) const { return m_min.value(index); }
    float parameterMaximum(int index) const { return m_max.value(index); }
    float parameterDefault(int index) const { return m_default.value(index); }
    float parameter(int index) const;
    void setParameter(int index, float value);
    bool setParameter(const QString &id, float value);
    void resetParameters();

    const QStringList &partIds() const { return m_partIds; }
    bool setPartOpacity(const QString &id, float value);

    // Recomputes the drawables (csmUpdateModel) and returns a snapshot for rendering.
    ModelFrame update();

private:
    CubismModel() = default;

    ModelSettings m_settings;
    std::unique_ptr<char[]> m_mocBuffer;
    std::unique_ptr<char[]> m_modelBuffer;
    void *m_moc = nullptr;
    void *m_model = nullptr;
    QStringList m_parameterIds;
    QVector<float> m_min, m_max, m_default;
    QStringList m_partIds;
    QList<QImage> m_textures;
};

} // namespace lumen::live2d
