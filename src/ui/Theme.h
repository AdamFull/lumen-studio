#pragma once

#include <QColor>
#include <QObject>
#include <QString>

class QApplication;
class QWidget;

namespace lumen::ui {

struct Palette
{
    bool dark = true;
    QColor window;      // app background
    QColor panel;       // side panels
    QColor surface;     // cards, inputs
    QColor raised;      // hover / elevated
    QColor border;
    QColor borderStrong;
    QColor text;
    QColor textMuted;
    QColor icon;
    QColor accent;
    QColor accentHover;
    QColor accentText;  // text on accent
    QColor success;
    QColor warning;
    QColor danger;
    QColor canvas;      // editor background around the image
    QColor selection;   // translucent accent for selections
};

// Application-wide look: Fusion style + palette + stylesheet, light and dark variants.
class Theme : public QObject
{
    Q_OBJECT
public:
    static Theme &instance();
    static const Palette &palette() { return instance().m_palette; }

    void apply(QApplication &app, const QString &name);
    QString name() const { return m_name; }
    bool isDark() const { return m_palette.dark; }

    // Helpers for widgets that paint themselves.
    static QColor mix(const QColor &a, const QColor &b, double t);
    static void setRole(QWidget *w, const QString &role); // sets dynamic "role" property and repolishes

signals:
    void changed();

private:
    Theme() = default;
    static Palette makeDark();
    static Palette makeLight();
    QString styleSheet() const;

    Palette m_palette = makeDark();
    QString m_name = QStringLiteral("dark");
};

} // namespace lumen::ui
