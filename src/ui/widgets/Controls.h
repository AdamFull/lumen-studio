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

// Small helpers to build consistent layouts.
QLabel *sectionLabel(const QString &text, QWidget *parent = nullptr);
QLabel *mutedLabel(const QString &text, QWidget *parent = nullptr);
QToolButton *iconButton(const QString &icon, const QString &tooltip, QWidget *parent = nullptr, bool checkable = false);
QFrame *card(QWidget *parent = nullptr);
QFrame *separator(Qt::Orientation orientation, QWidget *parent = nullptr);

} // namespace lumen::ui
