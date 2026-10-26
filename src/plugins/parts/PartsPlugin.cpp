#include "plugins/parts/PartsPlugin.h"

#include "plugins/parts/PartsPage.h"

#include <QCoreApplication>

namespace lumen::plugins {

QString PartsPlugin::name() const
{
    return QCoreApplication::translate("lumen::plugins::PartsPlugin", "Live2D parts");
}

QString PartsPlugin::description() const
{
    return QCoreApplication::translate("lumen::plugins::PartsPlugin",
                                       "Tools → Parts: slices a character illustration into parts for Live2D Cubism, completes the "
                                       "hidden areas of every part, previews the result as a puppet and exports a layered PSD.");
}

QList<ui::ext::PageSpec> PartsPlugin::pages() const
{
    ui::ext::PageSpec parts;
    parts.id = QStringLiteral("parts");
    parts.icon = QStringLiteral("parts");
    parts.label = QCoreApplication::translate("lumen::ui::MainWindow", "Parts");
    parts.tooltip = QCoreApplication::translate("lumen::ui::MainWindow", "Slice a character into parts for Live2D");
    parts.order = 50;
    parts.shortcut = QKeySequence(QStringLiteral("Alt+5"));
    parts.create = [](ui::Studio *studio, QWidget *parent) -> QWidget * { return new ui::PartsPage(studio, parent); };
    return {parts};
}

} // namespace lumen::plugins
