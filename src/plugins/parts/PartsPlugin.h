#pragma once

#include "ui/ext/Extension.h"

namespace lumen::plugins {

// Live2D parts: slices a character illustration into named, ordered layers for Live2D Cubism,
// completes the hidden areas, previews the result as a puppet and exports a layered PSD.
class PartsPlugin : public ui::ext::Extension
{
public:
    QString id() const override { return QStringLiteral("parts"); }
    QString name() const override;
    QString description() const override;
    QList<ui::ext::PageSpec> pages() const override;
};

} // namespace lumen::plugins
