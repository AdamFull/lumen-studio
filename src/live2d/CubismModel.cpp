#include "live2d/CubismModel.h"

#include "live2d/CubismCore.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <cstdint>
#include <cstring>

namespace lumen::live2d {

namespace {

// Allocates `size` bytes aligned to `alignment`; the returned buffer owns the memory.
char *alignedBlock(std::unique_ptr<char[]> &owner, size_t size, size_t alignment)
{
    owner = std::make_unique<char[]>(size + alignment);
    const auto raw = reinterpret_cast<std::uintptr_t>(owner.get());
    const std::uintptr_t aligned = (raw + alignment - 1) & ~(std::uintptr_t(alignment) - 1);
    return reinterpret_cast<char *>(aligned);
}

} // namespace

CubismModel::~CubismModel() = default;

std::unique_ptr<CubismModel> CubismModel::load(const QString &model3Path, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<CubismModel> {
        if (error)
            *error = message;
        return nullptr;
    };
    CubismCore &core = CubismCore::instance();
    if (!core.isLoaded())
        return fail(QObject::tr("The Live2D Cubism Core library is not loaded."));
    const CoreApi &api = core.api();

    std::unique_ptr<CubismModel> model(new CubismModel);
    QString e;
    const auto settings = ModelSettings::load(model3Path, &e);
    if (!settings)
        return fail(e);
    model->m_settings = *settings;

    QFile mocFile(settings->moc);
    if (!mocFile.open(QIODevice::ReadOnly))
        return fail(QObject::tr("Cannot open %1: %2").arg(QFileInfo(settings->moc).fileName(), mocFile.errorString()));
    const QByteArray moc = mocFile.readAll();
    if (moc.isEmpty())
        return fail(QObject::tr("%1 is empty").arg(QFileInfo(settings->moc).fileName()));
    char *mocMemory = alignedBlock(model->m_mocBuffer, size_t(moc.size()), kMocAlignment);
    std::memcpy(mocMemory, moc.constData(), size_t(moc.size()));
    const unsigned mocSize = unsigned(moc.size());
    if (api.getMocVersion && api.getLatestMocVersion && api.getMocVersion(mocMemory, mocSize) > api.getLatestMocVersion())
        return fail(QObject::tr("This model needs a newer Cubism Core (moc version %1, Core supports %2).")
                        .arg(api.getMocVersion(mocMemory, mocSize)).arg(api.getLatestMocVersion()));
    if (api.hasMocConsistency && !api.hasMocConsistency(mocMemory, mocSize))
        return fail(QObject::tr("%1 is damaged or not a .moc3 file.").arg(QFileInfo(settings->moc).fileName()));
    model->m_moc = api.reviveMocInPlace(mocMemory, mocSize);
    if (!model->m_moc)
        return fail(QObject::tr("The Cubism Core could not read %1.").arg(QFileInfo(settings->moc).fileName()));
    const unsigned modelSize = api.getSizeofModel(model->m_moc);
    char *modelMemory = alignedBlock(model->m_modelBuffer, modelSize, kModelAlignment);
    model->m_model = api.initializeModelInPlace(model->m_moc, modelMemory, modelSize);
    if (!model->m_model)
        return fail(QObject::tr("The Cubism Core could not initialise the model."));

    const int params = api.getParameterCount(model->m_model);
    const char **ids = api.getParameterIds(model->m_model);
    const float *mins = api.getParameterMinimumValues(model->m_model);
    const float *maxs = api.getParameterMaximumValues(model->m_model);
    const float *defs = api.getParameterDefaultValues(model->m_model);
    for (int i = 0; i < params; ++i) {
        model->m_parameterIds << QString::fromUtf8(ids[i]);
        model->m_min << mins[i];
        model->m_max << maxs[i];
        model->m_default << defs[i];
    }
    const int parts = api.getPartCount(model->m_model);
    const char **partIds = api.getPartIds(model->m_model);
    for (int i = 0; i < parts; ++i)
        model->m_partIds << QString::fromUtf8(partIds[i]);

    for (const QString &path : settings->textures) {
        QImageReader reader(path);
        QImage texture = reader.read();
        if (texture.isNull())
            return fail(QObject::tr("Cannot load texture %1: %2").arg(QFileInfo(path).fileName(), reader.errorString()));
        model->m_textures << texture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    return model;
}

float CubismModel::parameter(int index) const
{
    if (index < 0 || index >= parameterCount())
        return 0.0f;
    return CubismCore::instance().api().getParameterValues(m_model)[index];
}

void CubismModel::setParameter(int index, float value)
{
    if (index < 0 || index >= parameterCount())
        return;
    CubismCore::instance().api().getParameterValues(m_model)[index] = std::clamp(value, m_min[index], m_max[index]);
}

bool CubismModel::setParameter(const QString &id, float value)
{
    const int i = parameterIndex(id);
    if (i < 0)
        return false;
    setParameter(i, value);
    return true;
}

void CubismModel::resetParameters()
{
    float *values = CubismCore::instance().api().getParameterValues(m_model);
    for (int i = 0; i < parameterCount(); ++i)
        values[i] = m_default[i];
}

bool CubismModel::setPartOpacity(const QString &id, float value)
{
    const int i = m_partIds.indexOf(id);
    if (i < 0)
        return false;
    CubismCore::instance().api().getPartOpacities(m_model)[i] = std::clamp(value, 0.0f, 1.0f);
    return true;
}

ModelFrame CubismModel::update()
{
    const CoreApi &api = CubismCore::instance().api();
    api.updateModel(m_model);
    ModelFrame frame;
    CsmVector2 size{0, 0}, origin{0, 0};
    float ppu = 1.0f;
    api.readCanvasInfo(m_model, &size, &origin, &ppu);
    frame.canvasSize = QSizeF(size.x, size.y);
    frame.origin = QPointF(origin.x, origin.y);
    frame.pixelsPerUnit = ppu > 0 ? ppu : 1.0;

    const int count = api.getDrawableCount(m_model);
    const char **ids = api.getDrawableIds(m_model);
    const unsigned char *constant = api.getDrawableConstantFlags(m_model);
    const unsigned char *dynamic = api.getDrawableDynamicFlags(m_model);
    const int *textures = api.getDrawableTextureIndices(m_model);
    const float *opacities = api.getDrawableOpacities(m_model);
    const int *maskCounts = api.getDrawableMaskCounts(m_model);
    const int **masks = api.getDrawableMasks(m_model);
    const int *vertexCounts = api.getDrawableVertexCounts(m_model);
    const CsmVector2 **positions = api.getDrawableVertexPositions(m_model);
    const CsmVector2 **uvs = api.getDrawableVertexUvs(m_model);
    const int *indexCounts = api.getDrawableIndexCounts(m_model);
    const unsigned short **indices = api.getDrawableIndices(m_model);
    const int *orders = api.getDrawableRenderOrders ? api.getDrawableRenderOrders(m_model)
                        : api.getRenderOrders       ? api.getRenderOrders(m_model)
                                                    : api.getDrawableDrawOrders(m_model);
    const CsmVector4 *multiply = api.getDrawableMultiplyColors ? api.getDrawableMultiplyColors(m_model) : nullptr;
    const CsmVector4 *screen = api.getDrawableScreenColors ? api.getDrawableScreenColors(m_model) : nullptr;

    frame.drawables.reserve(count);
    for (int i = 0; i < count; ++i) {
        DrawableFrame d;
        d.id = QString::fromUtf8(ids[i]);
        d.texture = textures[i];
        d.opacity = opacities[i];
        d.visible = dynamic[i] & CsmIsVisible;
        d.renderOrder = orders ? orders[i] : i;
        d.blend = (constant[i] & CsmBlendAdditive)         ? DrawableFrame::Blend::Additive
                  : (constant[i] & CsmBlendMultiplicative) ? DrawableFrame::Blend::Multiplicative
                                                           : DrawableFrame::Blend::Normal;
        d.invertedMask = constant[i] & CsmIsInvertedMask;
        for (int m = 0; m < maskCounts[i]; ++m)
            d.masks << masks[i][m];
        if (d.visible && d.opacity > 0.0f) {
            d.positions.resize(vertexCounts[i]);
            d.uvs.resize(vertexCounts[i]);
            for (int v = 0; v < vertexCounts[i]; ++v) {
                d.positions[v] = QPointF(positions[i][v].x, positions[i][v].y);
                d.uvs[v] = QPointF(uvs[i][v].x, uvs[i][v].y);
            }
            d.indices.resize(indexCounts[i]);
            std::memcpy(d.indices.data(), indices[i], size_t(indexCounts[i]) * sizeof(quint16));
        }
        if (multiply) {
            d.multiply[0] = multiply[i].x;
            d.multiply[1] = multiply[i].y;
            d.multiply[2] = multiply[i].z;
        }
        if (screen) {
            d.screen[0] = screen[i].x;
            d.screen[1] = screen[i].y;
            d.screen[2] = screen[i].z;
        }
        frame.drawables.append(d);
    }
    // Masks need the geometry of their mask drawables even when those are hidden themselves.
    for (DrawableFrame &d : frame.drawables) {
        for (int m : std::as_const(d.masks)) {
            if (m < 0 || m >= count)
                continue;
            DrawableFrame &mask = frame.drawables[m];
            if (!mask.positions.isEmpty())
                continue;
            mask.positions.resize(vertexCounts[m]);
            mask.uvs.resize(vertexCounts[m]);
            for (int v = 0; v < vertexCounts[m]; ++v) {
                mask.positions[v] = QPointF(positions[m][v].x, positions[m][v].y);
                mask.uvs[v] = QPointF(uvs[m][v].x, uvs[m][v].y);
            }
            mask.indices.resize(indexCounts[m]);
            std::memcpy(mask.indices.data(), indices[m], size_t(indexCounts[m]) * sizeof(quint16));
        }
    }
    api.resetDrawableDynamicFlags(m_model);
    return frame;
}

} // namespace lumen::live2d
