#pragma once

#include "pose/Rig.h"

#include <optional>

namespace lumen::pose {

struct PosePreset
{
    QString id;       // stable identifier (file name stem for user presets)
    QString name;     // display name
    QString category; // "Standing", "Action", "Sitting", "Framing", "Sheets", "User"...
    QString description;
    QList<RigParams> rigs;    // generated presets (one per person)
    std::optional<Pose> pose; // captured keypoints (user presets saved from the editor)
    bool builtIn = true;

    Pose toPose(const QSize &canvas) const;
    QJsonObject toJson() const;
    static std::optional<PosePreset> fromJson(const QJsonObject &json);
};

const QList<PosePreset> &builtInPresets();
std::optional<PosePreset> findBuiltInPreset(const QString &idOrName);

// User presets live in <app data>/presets/poses/*.json.
QString userPresetDirectory();
QList<PosePreset> loadUserPresets();
bool saveUserPreset(PosePreset preset, QString *error = nullptr);
bool deleteUserPreset(const QString &id);

} // namespace lumen::pose
