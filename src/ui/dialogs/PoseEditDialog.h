#pragma once

#include "pose/Rig.h"

#include <QDialog>

#include <optional>

class QLineEdit;
class QPushButton;

namespace lumen::ui {

class PoseCanvas;
class Studio;

// Compact pose editor used for planned shots: drag joints, apply presets or describe a change in words.
class PoseEditDialog : public QDialog
{
    Q_OBJECT
public:
    PoseEditDialog(Studio *studio, const pose::Pose &pose, const std::optional<pose::RigParams> &rig, const QString &context,
                   QWidget *parent = nullptr);
    pose::Pose pose() const;
    std::optional<pose::RigParams> rig() const { return m_rig; }

private:
    void describe();
    Studio *m_studio;
    PoseCanvas *m_canvas;
    QLineEdit *m_describe;
    QPushButton *m_describeBtn;
    std::optional<pose::RigParams> m_rig;
    QString m_context;
};

} // namespace lumen::ui
