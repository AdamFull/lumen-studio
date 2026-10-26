#include "plugins/poses/PosesPlugin.h"

#include "plugins/poses/PosePage.h"

#include <QCoreApplication>

namespace lumen::plugins {

QString PosesPlugin::name() const
{
    return QCoreApplication::translate("lumen::plugins::PosesPlugin", "Poses");
}

QString PosesPlugin::description() const
{
    return QCoreApplication::translate("lumen::plugins::PosesPlugin",
                                       "Tools → Poses: a pose editor and library. Pose people with a 3D rig or joint by joint, detect "
                                       "a pose in an image, and start a new image in that pose.");
}

QList<ui::ext::PageSpec> PosesPlugin::pages() const
{
    ui::ext::PageSpec poses;
    poses.id = QStringLiteral("poses");
    poses.icon = QStringLiteral("person");
    poses.label = QCoreApplication::translate("lumen::ui::MainWindow", "Poses");
    poses.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "Pose editor and library");
    poses.order = 40;
    poses.shortcut = QKeySequence(QStringLiteral("Alt+4"));
    poses.create = [](ui::Studio *studio, QWidget *parent) -> QWidget * { return new ui::PosePage(studio, parent); };
    return {poses};
}

} // namespace lumen::plugins
