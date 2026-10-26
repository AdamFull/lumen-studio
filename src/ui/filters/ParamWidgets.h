#pragma once

#include "filters/NodeType.h"

#include <QList>
#include <QPointF>
#include <QVariant>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;

namespace lumen::ui {

class ColorSwatch;
class SliderField;

// Colour of a port type in the node editor: its dot and the links that leave it.
QColor portColor(filters::PortType type);

// The curve of a Curves node, as points from 0 to 255 ("x y, x y", as the node stores it). Drag a
// point to move it, click elsewhere to add one, double-click or right-click a point to remove it
// (two always stay). The curve drawn is the one the node applies.
class CurveEditor : public QWidget
{
    Q_OBJECT
public:
    explicit CurveEditor(QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text); // without emitting
    QList<QPointF> points() const { return m_points; }

    QSize sizeHint() const override { return {220, 220}; }
    QSize minimumSizeHint() const override { return {140, 140}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return width; }

signals:
    void textChanged(const QString &text); // also while a point is dragged
    void editingFinished();               // a drag ended, a point was added or removed

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
    QRectF plotRect() const;
    QPointF toWidget(const QPointF &value) const;
    QPointF toValue(const QPointF &widget) const;
    int pointAt(const QPointF &widget) const;
    void removePoint(int index);
    void moveDragged(const QPointF &widget);

    QList<QPointF> m_points;
    int m_dragged = -1;
};

// The control for one node setting, chosen by its spec: a slider with a number box, a list of
// choices, a colour swatch, the curve editor (with the points as text), a text field, a field of
// several lines for a prompt, or the list of image engines (an AI step's "Image engine"). Shared by
// the node editor and the Filters panel (published settings).
class ParamField : public QWidget
{
    Q_OBJECT
public:
    // `label` replaces the setting's own label; `minimum` < `maximum` narrows a number's range (a
    // published slider's range).
    ParamField(const filters::ParamSpec &spec, const QVariant &value, const QString &label = {}, double minimum = 0.0,
               double maximum = 0.0, QWidget *parent = nullptr);

    const filters::ParamSpec &spec() const { return m_spec; }
    QVariant value() const;
    void setValue(const QVariant &value); // without emitting

signals:
    void valueChanged(const QVariant &value);

private:
    filters::ParamSpec m_spec;
    SliderField *m_slider = nullptr;
    QComboBox *m_combo = nullptr;
    ColorSwatch *m_color = nullptr;
    CurveEditor *m_curve = nullptr;
    QLineEdit *m_text = nullptr;
    QPlainTextEdit *m_prompt = nullptr;
    bool m_engines = false; // m_combo lists image engines
};

// The suffix a number box shows after the value of a setting ("px" becomes " px").
QString unitSuffix(const QString &unit);

} // namespace lumen::ui
