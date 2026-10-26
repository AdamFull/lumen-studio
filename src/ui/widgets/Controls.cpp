#include "ui/widgets/Controls.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTextBlock>

#include <climits>
#include <cmath>

namespace lumen::ui {

SliderField::SliderField(const QString &label, double min, double max, double value, int decimals, const QString &suffix, QWidget *parent)
    : QWidget(parent)
    , m_scale(std::pow(10.0, decimals))
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    m_label = new QLabel(label, this);
    m_label->setProperty("role", QStringLiteral("muted"));
    m_label->setMinimumWidth(64);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(int(std::lround(min * m_scale)), int(std::lround(max * m_scale)));
    m_spin = new QDoubleSpinBox(this);
    m_spin->setDecimals(decimals);
    m_spin->setRange(min, max);
    m_spin->setSuffix(suffix);
    m_spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_spin->setFixedWidth(64);
    m_spin->setAlignment(Qt::AlignRight);
    m_spin->setKeyboardTracking(false);
    layout->addWidget(m_label);
    layout->addWidget(m_slider, 1);
    layout->addWidget(m_spin);
    if (label.isEmpty())
        m_label->hide();
    // Screen readers announce the slider and the number box by the field's label.
    m_slider->setAccessibleName(label);
    m_spin->setAccessibleName(label);
    setValue(value);
    connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        const double d = v / m_scale;
        if (std::abs(m_spin->value() - d) > 0.5 / m_scale) {
            QSignalBlocker b(m_spin);
            m_spin->setValue(d);
        }
        emit valueChanged(d);
    });
    connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double d) {
        QSignalBlocker b(m_slider);
        m_slider->setValue(int(std::lround(d * m_scale)));
        emit valueChanged(d);
    });
}

double SliderField::value() const
{
    return m_spin->value();
}

void SliderField::setValue(double v)
{
    QSignalBlocker b1(m_slider), b2(m_spin);
    m_spin->setValue(v);
    m_slider->setValue(int(std::lround(v * m_scale)));
}

void SliderField::setRange(double min, double max)
{
    QSignalBlocker b1(m_slider), b2(m_spin);
    m_spin->setRange(min, max);
    m_slider->setRange(int(std::lround(min * m_scale)), int(std::lround(max * m_scale)));
}

void SliderField::setCompact(bool compact)
{
    m_label->setMinimumWidth(compact ? 0 : 64);
    m_slider->setMinimumWidth(compact ? 80 : 120);
}

// ---- ColorSwatch --------------------------------------------------------------------------------------

ColorSwatch::ColorSwatch(const QColor &color, QWidget *parent)
    : QToolButton(parent)
    , m_color(color)
{
    setFixedSize(28, 28);
    setCursor(Qt::PointingHandCursor);
    connect(this, &QToolButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_color, this, tr("Choose colour"), QColorDialog::ShowAlphaChannel);
        if (c.isValid())
            setColor(c);
    });
}

void ColorSwatch::setColor(const QColor &c)
{
    if (c == m_color)
        return;
    m_color = c;
    setToolTip(c.name(QColor::HexArgb));
    update();
    emit colorChanged(c);
}

void ColorSwatch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
    QPainterPath path;
    path.addRoundedRect(r, 6, 6);
    p.setClipPath(path);
    // Checker under transparent colours.
    for (int y = 0; y < height(); y += 6)
        for (int x = 0; x < width(); x += 6)
            p.fillRect(x, y, 6, 6, ((x + y) / 6) % 2 ? QColor(200, 200, 200) : Qt::white);
    p.fillRect(r, m_color);
    p.setClipping(false);
    p.setPen(QPen(underMouse() ? Theme::palette().accent : Theme::palette().borderStrong, 1.5));
    p.drawPath(path);
}

// ---- AutoTextEdit ---------------------------------------------------------------------------------------

AutoTextEdit::AutoTextEdit(QWidget *parent, int minLines, int maxLines)
    : QPlainTextEdit(parent)
    , m_minLines(minLines)
    , m_maxLines(maxLines)
{
    setTabChangesFocus(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(this, &QPlainTextEdit::textChanged, this, &AutoTextEdit::adjustHeight);
    adjustHeight();
}

void AutoTextEdit::adjustHeight()
{
    const QFontMetrics fm(font());
    int lines = 0;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        lines += std::max(1, b.layout() ? b.layout()->lineCount() : 1);
    lines = std::clamp(lines, m_minLines, m_maxLines);
    const int h = lines * fm.lineSpacing() + 18;
    if (h != height())
        setFixedHeight(h);
}

void AutoTextEdit::keyPressEvent(QKeyEvent *e)
{
    const bool enter = e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
    if (enter && ((e->modifiers() & Qt::ControlModifier) || (m_enterSubmits && !(e->modifiers() & Qt::ShiftModifier)))) {
        emit submitted();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
    adjustHeight();
}

// ---- helpers ---------------------------------------------------------------------------------------------

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text.toUpper(), parent);
    l->setProperty("role", QStringLiteral("section"));
    return l;
}

// ---- ElidedLabel --------------------------------------------------------------------------------------

ElidedLabel::ElidedLabel(const QString &text, QWidget *parent)
    : QLabel(parent)
{
    setWordWrap(false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setFullText(text);
}

void ElidedLabel::setFullText(const QString &text)
{
    m_full = text;
    setToolTip(text);
    updateText();
    updateGeometry();
}

QSize ElidedLabel::minimumSizeHint() const
{
    return QSize(0, QLabel::minimumSizeHint().height());
}

QSize ElidedLabel::sizeHint() const
{
    const QMargins m = contentsMargins();
    return QSize(fontMetrics().horizontalAdvance(m_full) + m.left() + m.right() + 2 * margin(), QLabel::sizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent *e)
{
    QLabel::resizeEvent(e);
    updateText();
}

void ElidedLabel::updateText()
{
    setText(fontMetrics().elidedText(m_full, Qt::ElideRight, contentsRect().width()));
}

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setProperty("role", QStringLiteral("muted"));
    l->setWordWrap(true);
    return l;
}

QString fitLabel(const QString &text, int width, double *pointSize)
{
    auto widthAt = [](const QString &line, double pt) {
        QFont f = QApplication::font();
        f.setPointSizeF(pt);
        return QFontMetrics(f).horizontalAdvance(line);
    };
    auto widest = [&](const QString &t, double pt) {
        int w = 0;
        for (const QString &line : t.split(u'\n'))
            w = std::max(w, widthAt(line, pt));
        return w;
    };
    QString out = text;
    if (widest(out, *pointSize) > width && text.contains(u' ')) {
        // Break at the space that gives the narrowest two lines.
        int best = -1, bestWidth = INT_MAX;
        for (int i = text.indexOf(u' '); i >= 0; i = text.indexOf(u' ', i + 1)) {
            const int w = std::max(widthAt(text.left(i), *pointSize), widthAt(text.mid(i + 1), *pointSize));
            if (w < bestWidth) {
                bestWidth = w;
                best = i;
            }
        }
        out = text.left(best) + u'\n' + text.mid(best + 1);
    }
    while (widest(out, *pointSize) > width && *pointSize > 7.0)
        *pointSize -= 0.5;
    return out;
}

QToolButton *iconButton(const QString &icon, const QString &tooltip, QWidget *parent, bool checkable)
{
    auto *b = new QToolButton(parent);
    b->setIcon(Icons::get(icon));
    b->setIconSize(QSize(18, 18));
    b->setToolTip(tooltip);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QFrame *card(QWidget *parent)
{
    auto *f = new QFrame(parent);
    f->setProperty("card", QStringLiteral("true"));
    return f;
}

QFrame *separator(Qt::Orientation orientation, QWidget *parent)
{
    auto *f = new QFrame(parent);
    f->setFrameShape(orientation == Qt::Horizontal ? QFrame::HLine : QFrame::VLine);
    f->setStyleSheet(QStringLiteral("color: %1; background: %1; border: none;").arg(Theme::palette().border.name()));
    if (orientation == Qt::Horizontal)
        f->setFixedHeight(1);
    else
        f->setFixedWidth(1);
    return f;
}

QScrollArea *verticalScroll(QWidget *content, QWidget *parent)
{
    // Grows its minimum width with the content's (plus the scroll bar), never its minimum height.
    class Area : public QScrollArea
    {
    public:
        using QScrollArea::QScrollArea;
        QSize minimumSizeHint() const override
        {
            const int content = widget() ? widget()->minimumSizeHint().width() : 0;
            return QSize(content + verticalScrollBar()->sizeHint().width() + 2 * frameWidth(), 60);
        }
    };
    auto *area = new Area(parent);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    // The panel background of the content shows through.
    area->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    return area;
}

} // namespace lumen::ui
