#pragma once

#include "ui/ext/Extension.h"

namespace lumen::ui {

// The core pages of the navigation rail (generate, editor, library); plugins add theirs.
QList<ext::PageSpec> corePages();

} // namespace lumen::ui
