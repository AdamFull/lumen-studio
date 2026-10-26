#include "ui/dialogs/ImageSizeDialog.h"

#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPoint>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace lumen::ui {

namespace {

// The canvas positions, in reading order: column and row from -1 to 1.
const QList<QPoint> kAnchors = {{0, 0}, {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

} // namespace

ImageSizeDialog::ImageSizeDialog(Kind kind, const QSize &current, QWidget *parent)
    : QDialog(parent)
    , m_kind(kind)
    , m_current(current)
{
    setWindowTitle(kind == Kind::Image ? tr("Image size") : tr("Canvas size"));
    setObjectName(kind == Kind::Image ? QStringLiteral("imageSize") : QStringLiteral("canvasSize"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(12);
    v->addWidget(mutedLabel(kind == Kind::Image
                                ? tr("Scales the whole picture, every layer. Now %1 × %2 px.").arg(current.width()).arg(current.height())
                                : tr("Cuts or extends the canvas; the picture keeps its size. Now %1 × %2 px.")
                                      .arg(current.width())
                                      .arg(current.height()),
                            this));
    auto *form = new QFormLayout;
    form->setVerticalSpacing(10);
    m_width = new QSpinBox(this);
    m_height = new QSpinBox(this);
    m_width->setAccessibleName(tr("Width"));
    m_height->setAccessibleName(tr("Height"));
    for (QSpinBox *s : {m_width, m_height}) {
        s->setRange(1, 16384);
        s->setSuffix(QStringLiteral(" px"));
    }
    m_width->setValue(current.width());
    m_height->setValue(current.height());
    form->addRow(tr("Width"), m_width);
    form->addRow(tr("Height"), m_height);
    if (kind == Kind::Image) {
        m_keep = new QCheckBox(tr("Keep the proportions"), this);
        m_keep->setChecked(true);
        form->addRow(QString(), m_keep);
        connect(m_width, &QSpinBox::valueChanged, this, &ImageSizeDialog::widthChanged);
        connect(m_height, &QSpinBox::valueChanged, this, &ImageSizeDialog::heightChanged);
    } else {
        m_anchor = new QComboBox(this);
        m_anchor->setAccessibleName(tr("Picture"));
        const QStringList names = {tr("In the middle"), tr("Top left"), tr("Top"), tr("Top right"), tr("Left"),
                                   tr("Right"),         tr("Bottom left"), tr("Bottom"), tr("Bottom right")};
        for (int i = 0; i < names.size(); ++i)
            m_anchor->addItem(names.at(i), kAnchors.at(i));
        form->addRow(tr("Picture"), m_anchor);
    }
    v->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

QSize ImageSizeDialog::newSize() const
{
    return {m_width->value(), m_height->value()};
}

void ImageSizeDialog::setNewSize(const QSize &size)
{
    const QSignalBlocker bw(m_width), bh(m_height);
    m_width->setValue(size.width());
    m_height->setValue(size.height());
}

void ImageSizeDialog::setAnchor(int column, int row)
{
    if (m_anchor)
        m_anchor->setCurrentIndex(std::max(0, m_anchor->findData(QPoint(column, row))));
}

QRect ImageSizeDialog::canvasRect() const
{
    const QSize size = newSize();
    const QPoint anchor = m_anchor ? m_anchor->currentData().toPoint() : QPoint();
    // Where the picture's top left lands in the new canvas, as Document::resizeCanvas takes it: the
    // canvas's origin in the picture's pixels.
    auto offset = [](int now, int next, int side) { return side < 0 ? 0 : side > 0 ? next - now : (next - now) / 2; };
    return {QPoint(-offset(m_current.width(), size.width(), anchor.x()), -offset(m_current.height(), size.height(), anchor.y())), size};
}

void ImageSizeDialog::widthChanged()
{
    if (!m_keep || !m_keep->isChecked() || m_current.isEmpty())
        return;
    const QSignalBlocker b(m_height);
    m_height->setValue(std::max(1, int(std::lround(m_width->value() * double(m_current.height()) / m_current.width()))));
}

void ImageSizeDialog::heightChanged()
{
    if (!m_keep || !m_keep->isChecked() || m_current.isEmpty())
        return;
    const QSignalBlocker b(m_width);
    m_width->setValue(std::max(1, int(std::lround(m_height->value() * double(m_current.width()) / m_current.height()))));
}

} // namespace lumen::ui
