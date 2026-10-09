#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace lumen::ui {

// Theme-tinted stroke icons (24×24 SVG). Icons are cached per name and theme.
class Icons
{
public:
    static QIcon get(const QString &name);
    static QPixmap pixmap(const QString &name, int size, const QColor &color, qreal dpr = 2.0);
    static void clearCache();
    static QStringList names();
};

} // namespace lumen::ui
