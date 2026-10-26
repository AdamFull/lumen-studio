#include "ui/canvas/EditorState.h"

#include "core/Settings.h"

namespace lumen::ui {

namespace {
QString key(const char *k)
{
    return QStringLiteral("editor/") + QLatin1String(k);
}
} // namespace

EditorState::EditorState(QObject *parent)
    : QObject(parent)
{
    const Settings &s = Settings::instance();
    m_fg = QColor(s.value(key("fg"), QStringLiteral("#000000")).toString());
    m_bg = QColor(s.value(key("bg"), QStringLiteral("#ffffff")).toString());
    m_brushSize = s.value(key("brushSize"), m_brushSize).toDouble();
    m_hardness = s.value(key("hardness"), m_hardness).toDouble();
    m_opacity = s.value(key("opacity"), m_opacity).toDouble();
    m_flow = s.value(key("flow"), m_flow).toDouble();
    m_pressureSize = s.value(key("pressureSize"), m_pressureSize).toBool();
    m_pressureOpacity = s.value(key("pressureOpacity"), m_pressureOpacity).toBool();
    m_maskSize = s.value(key("maskSize"), m_maskSize).toDouble();
    m_maskHardness = s.value(key("maskHardness"), m_maskHardness).toDouble();
    m_tolerance = s.value(key("tolerance"), m_tolerance).toInt();
    m_sampleMerged = s.value(key("sampleMerged"), m_sampleMerged).toBool();
    m_feather = s.value(key("selectionFeather"), m_feather).toInt();
    // The switch of the floating generative toolbar, which the prompt bar replaced.
    Settings::instance().remove(key("contextBar"));
}

void EditorState::persist()
{
    Settings &s = Settings::instance();
    s.setValue(key("fg"), m_fg.name());
    s.setValue(key("bg"), m_bg.name());
    s.setValue(key("brushSize"), m_brushSize);
    s.setValue(key("hardness"), m_hardness);
    s.setValue(key("opacity"), m_opacity);
    s.setValue(key("flow"), m_flow);
    s.setValue(key("pressureSize"), m_pressureSize);
    s.setValue(key("pressureOpacity"), m_pressureOpacity);
    s.setValue(key("maskSize"), m_maskSize);
    s.setValue(key("maskHardness"), m_maskHardness);
    s.setValue(key("tolerance"), m_tolerance);
    s.setValue(key("sampleMerged"), m_sampleMerged);
    s.setValue(key("selectionFeather"), m_feather);
    emit changed();
}

void EditorState::setForeground(const QColor &c)
{
    if (c == m_fg || !c.isValid())
        return;
    m_fg = c;
    persist();
    emit colorsChanged();
}

void EditorState::setBackground(const QColor &c)
{
    if (c == m_bg || !c.isValid())
        return;
    m_bg = c;
    persist();
    emit colorsChanged();
}

void EditorState::swapColors()
{
    std::swap(m_fg, m_bg);
    persist();
    emit colorsChanged();
}

#define LUMEN_SETTER(Name, member)        \
    void EditorState::Name(decltype(member) v) \
    {                                     \
        if (member == v)                  \
            return;                       \
        member = v;                       \
        persist();                        \
    }

LUMEN_SETTER(setBrushSize, m_brushSize)
LUMEN_SETTER(setBrushHardness, m_hardness)
LUMEN_SETTER(setBrushOpacity, m_opacity)
LUMEN_SETTER(setBrushFlow, m_flow)
LUMEN_SETTER(setPressureSize, m_pressureSize)
LUMEN_SETTER(setPressureOpacity, m_pressureOpacity)
LUMEN_SETTER(setMaskBrushSize, m_maskSize)
LUMEN_SETTER(setMaskHardness, m_maskHardness)
LUMEN_SETTER(setFillTolerance, m_tolerance)
LUMEN_SETTER(setSampleMerged, m_sampleMerged)
LUMEN_SETTER(setSelectionFeather, m_feather)
LUMEN_SETTER(setShowSelectionOverlay, m_overlay)
LUMEN_SETTER(setShowPoseOverlay, m_poseOverlay)

#undef LUMEN_SETTER

void EditorState::setSelectionMode(SelectionMode mode)
{
    if (mode == m_selectionMode)
        return;
    m_selectionMode = mode;
    emit changed();
}

} // namespace lumen::ui
