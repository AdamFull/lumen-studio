#pragma once

#include <QObject>
#include <QString>

class QWidget;

namespace lumen::ui {

// Gives every interactive widget a screen-reader name. Icon-only buttons, fields without a visible
// label and custom views otherwise announce nothing. Names come from, in order: an explicit
// accessibleName, the widget's text, the QAction text, a QFormLayout/buddy label, the placeholder
// text and the tool tip (without its "(shortcut)" suffix).
class Accessibility : public QObject
{
    Q_OBJECT
public:
    // Names widgets as they are polished (before they are first shown), application-wide.
    static void install();
    // Names `widget` now if it is interactive and unnamed; returns the name it ends up with.
    static QString ensureName(QWidget *widget);
    // Interactive widgets under `root` that still have no accessible name (for tests).
    static QList<QWidget *> unnamed(QWidget *root);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using QObject::QObject;
};

} // namespace lumen::ui
