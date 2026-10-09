#pragma once

#include <QColor>
#include <QObject>

namespace lumen::ui {

// Tool settings shared by the editor (persisted between sessions).
class EditorState : public QObject
{
    Q_OBJECT
public:
    explicit EditorState(QObject *parent = nullptr);

    QColor foreground() const { return m_fg; }
    QColor background() const { return m_bg; }
    void setForeground(const QColor &c);
    void setBackground(const QColor &c);
    void swapColors();

    double brushSize() const { return m_brushSize; }
    double brushHardness() const { return m_hardness; }
    double brushOpacity() const { return m_opacity; }
    double brushFlow() const { return m_flow; }
    bool pressureSize() const { return m_pressureSize; }
    bool pressureOpacity() const { return m_pressureOpacity; }
    double maskBrushSize() const { return m_maskSize; }
    double maskHardness() const { return m_maskHardness; }
    int fillTolerance() const { return m_tolerance; }
    bool sampleMerged() const { return m_sampleMerged; }
    bool showSelectionOverlay() const { return m_overlay; }
    bool showPoseOverlay() const { return m_poseOverlay; }
    bool showContextBar() const { return m_contextBar; }

    void setBrushSize(double v);
    void setBrushHardness(double v);
    void setBrushOpacity(double v);
    void setBrushFlow(double v);
    void setPressureSize(bool v);
    void setPressureOpacity(bool v);
    void setMaskBrushSize(double v);
    void setMaskHardness(double v);
    void setFillTolerance(int v);
    void setSampleMerged(bool v);
    void setShowSelectionOverlay(bool v);
    void setShowPoseOverlay(bool v);
    void setShowContextBar(bool v);

signals:
    void changed();
    void colorsChanged();

private:
    void persist();
    QColor m_fg = Qt::black;
    QColor m_bg = Qt::white;
    double m_brushSize = 24;
    double m_hardness = 0.7;
    double m_opacity = 1.0;
    double m_flow = 1.0;
    bool m_pressureSize = true;
    bool m_pressureOpacity = false;
    double m_maskSize = 60;
    double m_maskHardness = 0.6;
    int m_tolerance = 32;
    bool m_sampleMerged = true;
    bool m_overlay = true;
    bool m_poseOverlay = true;
    bool m_contextBar = true;
};

} // namespace lumen::ui
