#include "ui/filters/ParamWidgets.h"

#include "filters/Ops.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

using filters::PortType;

QColor portColor(PortType type)
{
    // Distinct hues that read on the dark and the light theme.
    switch (type) {
    case PortType::Image: return QColor(0xe0, 0x9a, 0x3c);
    case PortType::Mask: return QColor(0x9a, 0xa3, 0xad);
    case PortType::Number: return QColor(0x4f, 0x9c, 0xe8);
    case PortType::Color: return QColor(0xd2, 0x5c, 0xb8);
    case PortType::Text: return QColor(0x4c, 0xb8, 0x7c);
    case PortType::Choice: return QColor(0x9a, 0x7c, 0xe6);
    }
    return QColor(0x80, 0x80, 0x80);
}

QString unitSuffix(const QString &unit)
{
    if (unit.isEmpty())
        return {};
    // Degrees stick to the number; words and symbols such as px and % keep a space.
    return unit == QStringLiteral("°") ? unit : QLatin1Char(' ') + unit;
}

// ---- CurveEditor ------------------------------------------------------------------------------

namespace {
constexpr double kMargin = 6.0;
constexpr double kGrab = 8.0; // px around a point that pick it
} // namespace

CurveEditor::CurveEditor(QWidget *parent)
    : QWidget(parent)
    , m_points(filters::ops::parseCurve(QString()))
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(tr("Drag a point to change the curve. Click to add a point; double-click or right-click a "
                  "point to remove it."));
}

QString CurveEditor::text() const
{
    return filters::ops::curveText(m_points);
}

void CurveEditor::setText(const QString &text)
{
    if (m_dragged >= 0)
        return; // the drag owns the points until it ends
    m_points = filters::ops::parseCurve(text);
    update();
}

QRectF CurveEditor::plotRect() const
{
    const double side = std::min(width(), height()) - 2 * kMargin;
    return {(width() - side) / 2.0, kMargin, std::max(1.0, side), std::max(1.0, side)};
}

QPointF CurveEditor::toWidget(const QPointF &value) const
{
    const QRectF r = plotRect();
    return {r.left() + value.x() / 255.0 * r.width(), r.bottom() - value.y() / 255.0 * r.height()};
}

QPointF CurveEditor::toValue(const QPointF &widget) const
{
    const QRectF r = plotRect();
    return {std::clamp(std::round((widget.x() - r.left()) / r.width() * 255.0), 0.0, 255.0),
            std::clamp(std::round((r.bottom() - widget.y()) / r.height() * 255.0), 0.0, 255.0)};
}

int CurveEditor::pointAt(const QPointF &widget) const
{
    int best = -1;
    double bestDistance = kGrab;
    for (int i = 0; i < m_points.size(); ++i) {
        const double d = QLineF(toWidget(m_points[i]), widget).length();
        if (d <= bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    return best;
}

void CurveEditor::removePoint(int index)
{
    if (index < 0 || m_points.size() <= 2)
        return;
    m_points.removeAt(index);
    update();
    emit textChanged(text());
    emit editingFinished();
}

void CurveEditor::moveDragged(const QPointF &widget)
{
    QPointF v = toValue(widget);
    // A point stays between its neighbours, so the order (and the curve) cannot fold over.
    const double low = m_dragged > 0 ? m_points[m_dragged - 1].x() + 1 : 0.0;
    const double high = m_dragged + 1 < m_points.size() ? m_points[m_dragged + 1].x() - 1 : 255.0;
    v.setX(std::clamp(v.x(), low, std::max(low, high)));
    if (v == m_points[m_dragged])
        return;
    m_points[m_dragged] = v;
    update();
    emit textChanged(text());
}

void CurveEditor::mousePressEvent(QMouseEvent *e)
{
    const int hit = pointAt(e->position());
    if (e->button() == Qt::RightButton) {
        removePoint(hit);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (hit >= 0) {
        m_dragged = hit;
        return;
    }
    // A new point where the click is, inserted in order, then dragged until the button is released.
    const QPointF v = toValue(e->position());
    int at = 0;
    while (at < m_points.size() && m_points[at].x() < v.x())
        ++at;
    if (at < m_points.size() && m_points[at].x() == v.x())
        m_points[at] = v;
    else
        m_points.insert(at, v);
    m_dragged = at;
    update();
    emit textChanged(text());
}

void CurveEditor::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragged >= 0 && (e->buttons() & Qt::LeftButton))
        moveDragged(e->position());
    else
        setCursor(pointAt(e->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_dragged >= 0) {
        m_dragged = -1;
        emit editingFinished();
    }
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent *e)
{
    m_dragged = -1;
    removePoint(pointAt(e->position()));
}

void CurveEditor::paintEvent(QPaintEvent *)
{
    const Palette &pal = Theme::palette();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(r, pal.surface);
    p.setPen(QPen(pal.border, 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    p.setPen(QPen(pal.borderStrong, 1, Qt::DashLine));
    p.drawLine(r.bottomLeft(), r.topRight());
    p.setPen(QPen(pal.borderStrong, 1));
    p.drawRect(r);

    const QList<int> table = filters::ops::curveTable(m_points);
    QPainterPath curve;
    for (int x = 0; x < table.size(); ++x) {
        const QPointF w = toWidget(QPointF(x, table[x]));
        if (x == 0)
            curve.moveTo(w);
        else
            curve.lineTo(w);
    }
    p.setPen(QPen(pal.text, 1.8));
    p.drawPath(curve);
    for (int i = 0; i < m_points.size(); ++i) {
        p.setBrush(i == m_dragged ? pal.accent : pal.panel);
        p.setPen(QPen(pal.accent, 1.6));
        p.drawEllipse(toWidget(m_points[i]), 4.5, 4.5);
    }
}

// ---- ParamField -------------------------------------------------------------------------------

ParamField::ParamField(const filters::ParamSpec &spec, const QVariant &value, const QString &label, double minimum,
                       double maximum, QWidget *parent)
    : QWidget(parent)
    , m_spec(spec)
{
    const QString text = label.isEmpty() ? spec.label : label;
    setToolTip(spec.help);
    auto labelWidget = [this, &text] {
        auto *l = new QLabel(text, this);
        l->setProperty("role", QStringLiteral("muted"));
        l->setMinimumWidth(64);
        l->setWordWrap(true);
        return l;
    };
    switch (spec.type) {
    case PortType::Number: {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        const bool narrowed = minimum < maximum;
        m_slider = new SliderField(text, narrowed ? minimum : spec.minimum, narrowed ? maximum : spec.maximum,
                                   value.toDouble(), spec.decimals, unitSuffix(spec.unit), this);
        row->addWidget(m_slider);
        connect(m_slider, &SliderField::valueChanged, this, [this](double v) { emit valueChanged(v); });
        break;
    }
    case PortType::Choice: {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        m_combo = new QComboBox(this);
        // Long choices ("How bright the eye sees each colour") shorten instead of widening the panel.
        m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_combo->setMinimumContentsLength(8);
        m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        for (const filters::ChoiceOption &o : spec.choices) {
            m_combo->addItem(o.label, o.id);
            m_combo->setItemData(m_combo->count() - 1, o.label, Qt::ToolTipRole);
        }
        m_combo->setAccessibleName(text);
        row->addWidget(labelWidget());
        row->addWidget(m_combo, 1);
        setValue(value);
        connect(m_combo, &QComboBox::currentIndexChanged, this, [this] { emit valueChanged(m_combo->currentData()); });
        break;
    }
    case PortType::Color: {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        m_color = new ColorSwatch(value.value<QColor>(), this);
        m_color->setAccessibleName(text);
        row->addWidget(labelWidget(), 1);
        row->addWidget(m_color);
        connect(m_color, &ColorSwatch::colorChanged, this, [this](const QColor &c) { emit valueChanged(c); });
        break;
    }
    case PortType::Text:
    case PortType::Image:
    case PortType::Mask: {
        auto *column = new QVBoxLayout(this);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        column->addWidget(labelWidget());
        if (spec.editor == QLatin1String("curve")) {
            m_curve = new CurveEditor(this);
            m_curve->setAccessibleName(text);
            column->addWidget(m_curve);
            connect(m_curve, &CurveEditor::textChanged, this, [this](const QString &t) {
                QSignalBlocker block(m_text);
                m_text->setText(t);
                emit valueChanged(t);
            });
        }
        m_text = new QLineEdit(this);
        m_text->setAccessibleName(text);
        column->addWidget(m_text);
        setValue(value);
        connect(m_text, &QLineEdit::textEdited, this, [this](const QString &t) {
            if (m_curve)
                m_curve->setText(t);
            emit valueChanged(t);
        });
        break;
    }
    }
}

QVariant ParamField::value() const
{
    if (m_slider)
        return m_slider->value();
    if (m_combo)
        return m_combo->currentData();
    if (m_color)
        return m_color->color();
    if (m_text)
        return m_text->text();
    return {};
}

void ParamField::setValue(const QVariant &value)
{
    if (m_slider) {
        QSignalBlocker block(m_slider);
        m_slider->setValue(value.toDouble());
    } else if (m_combo) {
        QSignalBlocker block(m_combo);
        const int i = m_combo->findData(value.toString());
        m_combo->setCurrentIndex(std::max(0, i));
    } else if (m_color) {
        QSignalBlocker block(m_color);
        m_color->setColor(value.value<QColor>());
    } else if (m_text) {
        const QString t = value.toString();
        if (m_text->text() != t) {
            QSignalBlocker block(m_text);
            m_text->setText(t);
        }
        if (m_curve) {
            QSignalBlocker block(m_curve);
            m_curve->setText(t);
        }
    }
}

} // namespace lumen::ui
