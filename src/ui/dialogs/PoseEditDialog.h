#pragma once

#include "pose/Rig.h"

#include <QDialog>

#include <optional>

class QButtonGroup;
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
    // `rigs`: the rig of each person in `pose` where it is known (a planned shot), else nullopt (a
    // pose edited by hand). With two people (M16), `names` label the chips that choose whom a preset
    // or a described change applies to; each keeps where it stands.
    PoseEditDialog(Studio *studio, const pose::Pose &pose, const QList<std::optional<pose::RigParams>> &rigs, const QString &context,
                   const QStringList &names = {}, QWidget *parent = nullptr);
    pose::Pose pose() const;
    // Per person; nullopt once that person was moved by hand.
    const QList<std::optional<pose::RigParams>> &rigs() const { return m_rigs; }

private:
    void describe();
    // Puts `rig` in place of person `index`, where that person stands.
    void applyRig(int index, pose::RigParams rig);
    Studio *m_studio;
    PoseCanvas *m_canvas;
    QLineEdit *m_describe;
    QPushButton *m_describeBtn;
    QList<std::optional<pose::RigParams>> m_rigs;
    QButtonGroup *m_people = nullptr;
    QString m_context;
};

} // namespace lumen::ui
