#pragma once

#include "ui/ext/Extension.h"

namespace lumen::plugins {

// Poses: the pose editor and library page (OpenPose skeletons, 3D rig, Live2D presets). The pose
// canvas and the pose dialog stay in the core, which the director's shot cards also use.
class PosesPlugin : public ui::ext::Extension
{
public:
    QString id() const override { return QStringLiteral("poses"); }
    QString name() const override;
    QString description() const override;
    QList<ui::ext::PageSpec> pages() const override;
};

} // namespace lumen::plugins
