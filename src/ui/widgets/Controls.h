#pragma once

#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QWidget>

#include <functional>

class QSlider;
class QDoubleSpinBox;
class QScrollArea;
class QHBoxLayout;
class QVBoxLayout;

namespace lumen::ui {

// Label + slider + spin box kept in sync. Works for integer and fractional values.
class SliderField : public QWidget
{
    Q_OBJECT
public:
    SliderField(const QString &label, double min, double max, double value, int decimals = 0, const QString &suffix = QString(),
                QWidget *parent = nullptr);
    double value() const;
    void setValue(double v);
    void setRange(double min, double max);
    void setCompact(bool compact);

signals:
    void valueChanged(double value);

private:
    QLabel *m_label;
    QSlider *m_slider;
    QDoubleSpinBox *m_spin;
    double m_scale;
};

// Colour swatch that opens a colour dialog.
class ColorSwatch : public QToolButton
{
    Q_OBJECT
public:
    explicit ColorSwatch(const QColor &color = Qt::black, QWidget *parent = nullptr);
    QColor color() const { return m_color; }
    void setColor(const QColor &c);

signals:
    void colorChanged(const QColor &c);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QColor m_color;
};

// Plain text edit that grows with its content between min and max lines; Ctrl+Enter emits submitted().
class AutoTextEdit : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit AutoTextEdit(QWidget *parent = nullptr, int minLines = 2, int maxLines = 10);
    QString text() const { return toPlainText(); }
    void setSubmitOnEnter(bool enter) { m_enterSubmits = enter; }

signals:
    void submitted();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void adjustHeight();
    int m_minLines;
    int m_maxLines;
    bool m_enterSubmits = false;
};

// One line of text that shortens itself with "…" to the width it gets, instead of setting the
// minimum width of everything around it; the full text is in the tooltip.
class ElidedLabel : public QLabel
{
    Q_OBJECT
public:
    explicit ElidedLabel(const QString &text = QString(), QWidget *parent = nullptr);
    void setFullText(const QString &text);
    QString fullText() const { return m_full; }
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateText();
    QString m_full;
};

// Small helpers to build consistent layouts.
QLabel *sectionLabel(const QString &text, QWidget *parent = nullptr);
QLabel *mutedLabel(const QString &text, QWidget *parent = nullptr);
QToolButton *iconButton(const QString &icon, const QString &tooltip, QWidget *parent = nullptr, bool checkable = false);
// Fits a label into `width` pixels at `pointSize` (labels of fixed-width text-under-icon buttons,
// which translations make longer): breaks it onto two lines at the space nearest its middle, and
// for a single long word lowers the point size (down to 7 pt). `pointSize` returns the size to use.
QString fitLabel(const QString &text, int width, double *pointSize);
QFrame *card(QWidget *parent = nullptr);
QFrame *separator(Qt::Orientation orientation, QWidget *parent = nullptr);
// Puts a side panel's content into a frameless scroll area that scrolls vertically only, so a tall
// panel does not set the window's minimum height. The area is as wide as the content needs.
QScrollArea *verticalScroll(QWidget *content, QWidget *parent = nullptr);

} // namespace lumen::ui
