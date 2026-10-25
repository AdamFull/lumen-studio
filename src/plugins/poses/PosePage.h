#pragma once

#include "pose/PosePresets.h"

#include <QHash>
#include <QTimer>
#include <QUndoStack>
#include <QWidget>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace lumen::ui {

class PoseCanvas;
class SliderField;
class Studio;

// Pose editor: interactive skeletons, 3D rig sliders, presets, LLM pose instructions and detection.
class PosePage : public QWidget
{
    Q_OBJECT
public:
    explicit PosePage(Studio *studio, QWidget *parent = nullptr);

    struct State
    {
        pose::Pose pose;
        QList<std::optional<pose::RigParams>> rigs; // per person; nullopt = free-form keypoints
    };
    void setState(const State &state, bool undoable, const QString &text = QString());
    const State &state() const { return m_state; }

private:
    void buildRigPanel(QVBoxLayout *layout);
    void syncRigPanel();
    void rigChanged();
    void applyPreset(const pose::PosePreset &preset, bool addAsPerson);
    void reloadPresets();
    void setAspect(const QString &aspect);
    void describePose();
    void detectFromImage();
    void savePreset();
    void exportPng();
    void exportJson();
    void importJson();
    std::optional<pose::RigParams> currentRig() const;

    Studio *m_studio;
    State m_state;
    State m_rigEditStart;
    bool m_rigEditing = false;
    QTimer m_rigCommit;
    QUndoStack m_undo;
    PoseCanvas *m_canvas;
    QListWidget *m_presets;
    QComboBox *m_aspect;
    QComboBox *m_people;
    QWidget *m_rigPanel;
    QLabel *m_rigNote;
    QPushButton *m_convertToRig;
    QHash<QString, SliderField *> m_rigSliders;
    QComboBox *m_leftHand;
    QComboBox *m_rightHand;
    QComboBox *m_expression;
    QComboBox *m_framing;
    QLineEdit *m_describe;
    QPushButton *m_describeBtn;
    QButtonGroup *m_modes;
    bool m_syncing = false;
};

} // namespace lumen::ui
