#pragma once

#include "ui/Studio.h"

#include <QWidget>

namespace lumen::ui {

// Creates the widget implementing a main page.
QWidget *createPage(Page page, Studio *studio, QWidget *parent);

} // namespace lumen::ui
