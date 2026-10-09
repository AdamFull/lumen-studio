#include "ui/Theme.h"

#include "ui/Icons.h"

#include "core/Paths.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

namespace lumen::ui {

Theme &Theme::instance()
{
    static Theme t;
    return t;
}

Palette Theme::makeDark()
{
    Palette p;
    p.dark = true;
    p.window = QColor(0x121419);
    p.panel = QColor(0x181b21);
    p.surface = QColor(0x1f232b);
    p.raised = QColor(0x272c36);
    p.border = QColor(0x2a2f3a);
    p.borderStrong = QColor(0x3a4150);
    p.text = QColor(0xe7e9ef);
    p.textMuted = QColor(0x969db0);
    p.icon = QColor(0xb9bfcc);
    p.accent = QColor(0x8b6cff);
    p.accentHover = QColor(0x9d83ff);
    p.accentText = QColor(0xffffff);
    p.success = QColor(0x34c38f);
    p.warning = QColor(0xf2a93b);
    p.danger = QColor(0xf0566a);
    p.canvas = QColor(0x0d0f13);
    p.selection = QColor(139, 108, 255, 70);
    return p;
}

Palette Theme::makeLight()
{
    Palette p;
    p.dark = false;
    p.window = QColor(0xf3f4f7);
    p.panel = QColor(0xfbfbfd);
    p.surface = QColor(0xffffff);
    p.raised = QColor(0xeceef3);
    p.border = QColor(0xdfe2e8);
    p.borderStrong = QColor(0xc6cad4);
    p.text = QColor(0x1b1e25);
    p.textMuted = QColor(0x646b7c);
    p.icon = QColor(0x4a5060);
    p.accent = QColor(0x6d4cf5);
    p.accentHover = QColor(0x7d60f7);
    p.accentText = QColor(0xffffff);
    p.success = QColor(0x1f9d6f);
    p.warning = QColor(0xc77f12);
    p.danger = QColor(0xd93b51);
    p.canvas = QColor(0xe2e4ea);
    p.selection = QColor(109, 76, 245, 60);
    return p;
}

QColor Theme::mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

void Theme::setRole(QWidget *w, const QString &role)
{
    w->setProperty("role", role);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

void Theme::apply(QApplication &app, const QString &name)
{
    m_name = name == QLatin1String("light") ? QStringLiteral("light") : QStringLiteral("dark");
    m_palette = m_name == QLatin1String("light") ? makeLight() : makeDark();
    Icons::clearCache();
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette qp;
    const Palette &p = m_palette;
    qp.setColor(QPalette::Window, p.window);
    qp.setColor(QPalette::WindowText, p.text);
    qp.setColor(QPalette::Base, p.surface);
    qp.setColor(QPalette::AlternateBase, p.raised);
    qp.setColor(QPalette::ToolTipBase, p.raised);
    qp.setColor(QPalette::ToolTipText, p.text);
    qp.setColor(QPalette::PlaceholderText, p.textMuted);
    qp.setColor(QPalette::Text, p.text);
    qp.setColor(QPalette::Button, p.surface);
    qp.setColor(QPalette::ButtonText, p.text);
    qp.setColor(QPalette::BrightText, p.accentText);
    qp.setColor(QPalette::Highlight, p.accent);
    qp.setColor(QPalette::HighlightedText, p.accentText);
    qp.setColor(QPalette::Link, p.accent);
    qp.setColor(QPalette::Mid, p.border);
    qp.setColor(QPalette::Dark, p.window);
    qp.setColor(QPalette::Light, p.raised);
    qp.setColor(QPalette::Disabled, QPalette::Text, p.textMuted);
    qp.setColor(QPalette::Disabled, QPalette::WindowText, p.textMuted);
    qp.setColor(QPalette::Disabled, QPalette::ButtonText, p.textMuted);
    app.setPalette(qp);
    app.setStyleSheet(styleSheet());
    emit changed();
}

QString Theme::styleSheet() const
{
    const Palette &p = m_palette;
    QString css = QStringLiteral(R"CSS(
* { outline: none; }
QWidget { color: @text; font-size: 9.5pt; }
QMainWindow, QDialog { background: @window; }
QWidget#Panel, QFrame#Panel { background: @panel; }
QFrame[card="true"] { background: @surface; border: 1px solid @border; border-radius: 10px; }
QFrame[card="raised"] { background: @raised; border: 1px solid @border; border-radius: 10px; }
QLabel { background: transparent; }
QLabel[role="title"] { font-size: 13pt; font-weight: 600; }
QLabel[role="heading"] { font-size: 10.5pt; font-weight: 600; }
QLabel[role="section"] { color: @muted; font-size: 8pt; font-weight: 600; letter-spacing: 0.6px; text-transform: uppercase; }
QLabel[role="muted"] { color: @muted; }
QLabel[role="error"] { color: @danger; }
QLabel[role="success"] { color: @success; }
QLabel[role="badge"] { background: @raised; border: 1px solid @border; border-radius: 8px; padding: 1px 7px; color: @muted; font-size: 8pt; }

QPushButton { background: @surface; border: 1px solid @border; border-radius: 7px; padding: 6px 14px; min-height: 18px; }
QPushButton:hover { background: @raised; border-color: @borderStrong; }
QPushButton:pressed { background: @border; }
QPushButton:disabled { color: @muted; background: @panel; }
QPushButton:checked { background: @accentSoft; border-color: @accent; }
QPushButton[role="primary"] { background: @accent; color: @accentText; border: 1px solid @accent; font-weight: 600; }
QPushButton[role="primary"]:hover { background: @accentHover; border-color: @accentHover; }
QPushButton[role="primary"]:disabled { background: @accentDisabled; border-color: @accentDisabled; color: @accentTextDim; }
QPushButton[role="danger"] { color: @danger; }
QPushButton[role="danger"]:hover { background: @dangerSoft; border-color: @danger; }
QPushButton[role="ghost"] { background: transparent; border: 1px solid transparent; }
QPushButton[role="ghost"]:hover { background: @raised; }
QPushButton::menu-indicator { width: 0; }

QToolButton { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 4px; }
QToolButton:hover { background: @raised; }
QToolButton:pressed { background: @border; }
QToolButton:checked { background: @accentSoft; border-color: @accentBorder; }
QToolButton:disabled { color: @muted; }
QToolButton[role="nav"] { border-radius: 10px; padding: 4px 0px; margin: 1px 0px; }
QToolButton[role="nav"]:checked { background: @accentSoft; border: 1px solid @accentBorder; }
QToolButton[role="chip"] { background: @surface; border: 1px solid @border; border-radius: 12px; padding: 3px 10px; }
QToolButton[role="chip"]:checked { background: @accentSoft; border-color: @accent; color: @text; }
QToolButton::menu-indicator { image: none; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
  background: @input; border: 1px solid @border; border-radius: 7px; padding: 5px 8px; selection-background-color: @accent; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: @muted; background: @panel; }
QPlainTextEdit, QTextEdit { padding: 6px; }
QSpinBox, QDoubleSpinBox { padding-right: 18px; }
QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button { width: 16px; border: none; background: transparent; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@upArrow); width: 10px; height: 10px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@downArrow); width: 10px; height: 10px; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(@downArrow); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: @raised; border: 1px solid @borderStrong; border-radius: 8px; padding: 4px; selection-background-color: @accentSoft; selection-color: @text; }

QCheckBox, QRadioButton { spacing: 7px; background: transparent; }
QCheckBox::indicator, QRadioButton::indicator { width: 15px; height: 15px; border: 1px solid @borderStrong; background: @input; }
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(@check); }
QRadioButton::indicator:checked { background: @accent; border: 4px solid @input; outline: 1px solid @accent; }

QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal { width: 14px; height: 14px; margin: -6px 0; border-radius: 7px; background: @handle; border: 2px solid @accent; }
QSlider::handle:horizontal:hover { background: @accent; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @scroll; border-radius: 3px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @borderStrong; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @scroll; border-radius: 3px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }

QTabWidget::pane { border: none; }
QTabBar::tab { background: transparent; color: @muted; padding: 7px 14px; border: none; border-bottom: 2px solid transparent; margin-right: 2px; }
QTabBar::tab:selected { color: @text; border-bottom-color: @accent; }
QTabBar::tab:hover:!selected { color: @text; }
QTabBar[role="documents"]::tab { background: @panel; border: 1px solid @border; border-bottom: none; border-top-left-radius: 7px; border-top-right-radius: 7px; padding: 6px 10px; }
QTabBar[role="documents"]::tab:selected { background: @surface; color: @text; border-color: @borderStrong; }
QTabBar::close-button { image: url(@close); subcontrol-position: right; }
QTabBar::close-button:hover { background: @raised; border-radius: 4px; }

QSplitter::handle { background: @border; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QMenuBar { background: @window; border-bottom: 1px solid @border; }
QMenuBar::item { padding: 5px 10px; background: transparent; border-radius: 5px; }
QMenuBar::item:selected { background: @raised; }
QMenu { background: @raised; border: 1px solid @borderStrong; border-radius: 9px; padding: 5px; }
QMenu::item { padding: 6px 24px 6px 12px; border-radius: 6px; }
QMenu::item:selected { background: @accentSoft; }
QMenu::item:disabled { color: @muted; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
QMenu::icon { padding-left: 6px; }

QToolTip { background: @raised; color: @text; border: 1px solid @borderStrong; border-radius: 6px; padding: 5px 8px; }
QProgressBar { background: @border; border: none; border-radius: 3px; max-height: 6px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @accent; border-radius: 3px; }
QStatusBar { background: @window; border-top: 1px solid @border; color: @muted; }
QStatusBar::item { border: none; }
QListView, QTreeView, QListWidget, QTreeWidget, QTableView { background: @panel; border: none; selection-background-color: @accentSoft; selection-color: @text; }
QListView::item, QTreeView::item { border-radius: 6px; padding: 3px; }
QListView::item:hover, QTreeView::item:hover { background: @raised; }
QListView::item:selected, QTreeView::item:selected { background: @accentSoft; }
QHeaderView::section { background: @panel; color: @muted; border: none; border-bottom: 1px solid @border; padding: 5px; }
QGroupBox { border: 1px solid @border; border-radius: 9px; margin-top: 14px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @muted; }
QDockWidget { titlebar-close-icon: none; }
QUndoView { background: @panel; }
)CSS");
    const QColor accentSoft(p.accent.red(), p.accent.green(), p.accent.blue(), p.dark ? 52 : 34);
    const QColor accentBorder(p.accent.red(), p.accent.green(), p.accent.blue(), 120);
    const QColor dangerSoft(p.danger.red(), p.danger.green(), p.danger.blue(), 40);
    const QColor accentDisabled = mix(p.accent, p.panel, 0.6);
    auto rgba = [](const QColor &c) { return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()); };
    // Stylesheets can only reference files: render the few icons they need into the cache directory.
    const QString iconDir = paths::cache() + QStringLiteral("/theme-") + m_name;
    QDir().mkpath(iconDir);
    auto iconFile = [&iconDir](const QString &name, const QColor &color) {
        const QString path = iconDir + u'/' + name + u'-' + color.name().mid(1) + QStringLiteral(".png");
        if (!QFile::exists(path))
            Icons::pixmap(name, 16, color, 2.0).save(path);
        return path;
    };
    const QList<QPair<QString, QString>> vars = {
        {QStringLiteral("@accentTextDim"), rgba(QColor(255, 255, 255, 150))},
        {QStringLiteral("@accentDisabled"), accentDisabled.name()},
        {QStringLiteral("@accentHover"), p.accentHover.name()},
        {QStringLiteral("@accentText"), p.accentText.name()},
        {QStringLiteral("@accentSoft"), rgba(accentSoft)},
        {QStringLiteral("@accentBorder"), rgba(accentBorder)},
        {QStringLiteral("@accent"), p.accent.name()},
        {QStringLiteral("@dangerSoft"), rgba(dangerSoft)},
        {QStringLiteral("@danger"), p.danger.name()},
        {QStringLiteral("@borderStrong"), p.borderStrong.name()},
        {QStringLiteral("@border"), p.border.name()},
        {QStringLiteral("@window"), p.window.name()},
        {QStringLiteral("@panel"), p.panel.name()},
        {QStringLiteral("@surface"), p.surface.name()},
        {QStringLiteral("@raised"), p.raised.name()},
        {QStringLiteral("@input"), (p.dark ? mix(p.surface, p.window, 0.35) : p.surface).name()},
        {QStringLiteral("@handle"), p.surface.name()},
        {QStringLiteral("@scroll"), mix(p.border, p.borderStrong, 0.5).name()},
        {QStringLiteral("@muted"), p.textMuted.name()},
        {QStringLiteral("@text"), p.text.name()},
        {QStringLiteral("@upArrow"), iconFile(QStringLiteral("chevron-up"), p.textMuted)},
        {QStringLiteral("@downArrow"), iconFile(QStringLiteral("chevron-down"), p.textMuted)},
        {QStringLiteral("@check"), iconFile(QStringLiteral("check"), p.accentText)},
        {QStringLiteral("@close"), iconFile(QStringLiteral("close"), p.textMuted)},
    };
    for (const auto &v : vars)
        css.replace(v.first, v.second);
    return css;
}

} // namespace lumen::ui
