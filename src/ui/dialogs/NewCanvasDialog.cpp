#include "ui/dialogs/NewCanvasDialog.h"

#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

namespace lumen::ui {

NewCanvasDialog::NewCanvasDialog(Studio *studio, QWidget *parent)
    : QDialog(parent)
{
    // The texts were the editor page's before the dialog became a class of its own.
    setWindowTitle(QCoreApplication::translate("lumen::ui::EditorPage", "New blank canvas"));
    setObjectName(QStringLiteral("newCanvas"));
    auto *form = new QFormLayout(this);
    form->setContentsMargins(20, 18, 20, 18);
    form->setVerticalSpacing(10);
    m_name = new QLineEdit(QCoreApplication::translate("lumen::ui::EditorPage", "Untitled"), this);
    m_name->setAccessibleName(QCoreApplication::translate("lumen::ui::EditorPage", "Name"));
    m_preset = new QComboBox(this);
    m_preset->setAccessibleName(tr("Size"));
    QList<QPair<QString, QSize>> presets = {
        {QCoreApplication::translate("lumen::ui::EditorPage", "Square 1024"), QSize(1024, 1024)},
        {QCoreApplication::translate("lumen::ui::EditorPage", "Portrait 832 × 1216 (2:3)"), QSize(832, 1216)},
        {QCoreApplication::translate("lumen::ui::EditorPage", "Landscape 1216 × 832 (3:2)"), QSize(1216, 832)},
        {QCoreApplication::translate("lumen::ui::EditorPage", "Full HD 1920 × 1080"), QSize(1920, 1080)},
        {QCoreApplication::translate("lumen::ui::EditorPage", "4K 3840 × 2160"), QSize(3840, 2160)},
    };
    // A sheet for the Live2D parts plugin's characters, when it is on.
    if (studio->hasPage(QStringLiteral("parts")))
        presets.insert(3, {QCoreApplication::translate("lumen::ui::EditorPage", "Live2D sheet 2048 × 3072"), QSize(2048, 3072)});
    for (const auto &p : presets)
        m_preset->addItem(p.first, p.second);
    m_preset->addItem(tr("Custom size"), QSize());
    m_width = new QSpinBox(this);
    m_height = new QSpinBox(this);
    m_width->setAccessibleName(QCoreApplication::translate("lumen::ui::EditorPage", "Width"));
    m_height->setAccessibleName(QCoreApplication::translate("lumen::ui::EditorPage", "Height"));
    for (QSpinBox *s : {m_width, m_height}) {
        s->setRange(16, 16384);
        s->setSuffix(QStringLiteral(" px"));
        s->setValue(1024);
    }
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this] {
        const QSize s = m_preset->currentData().toSize();
        if (s.isEmpty())
            return;
        const QSignalBlocker bw(m_width), bh(m_height);
        m_width->setValue(s.width());
        m_height->setValue(s.height());
    });
    // Typing a size of its own makes it a custom one.
    auto custom = [this] {
        const QSize s(m_width->value(), m_height->value());
        if (m_preset->currentData().toSize() != s) {
            const int index = m_preset->findData(s);
            const QSignalBlocker b(m_preset);
            m_preset->setCurrentIndex(index >= 0 ? index : m_preset->count() - 1);
        }
    };
    connect(m_width, &QSpinBox::valueChanged, this, custom);
    connect(m_height, &QSpinBox::valueChanged, this, custom);
    m_background = new QComboBox(this);
    m_background->setAccessibleName(QCoreApplication::translate("lumen::ui::EditorPage", "Background"));
    m_background->addItem(QCoreApplication::translate("lumen::ui::EditorPage", "White"), QColor(Qt::white));
    m_background->addItem(QCoreApplication::translate("lumen::ui::EditorPage", "Black"), QColor(Qt::black));
    m_background->addItem(QCoreApplication::translate("lumen::ui::EditorPage", "Transparent"), QColor(Qt::transparent));
    m_background->addItem(QCoreApplication::translate("lumen::ui::EditorPage", "Grey"), QColor(0x808080));
    form->addRow(QCoreApplication::translate("lumen::ui::EditorPage", "Name"), m_name);
    form->addRow(tr("Size"), m_preset);
    form->addRow(QCoreApplication::translate("lumen::ui::EditorPage", "Width"), m_width);
    form->addRow(QCoreApplication::translate("lumen::ui::EditorPage", "Height"), m_height);
    form->addRow(QCoreApplication::translate("lumen::ui::EditorPage", "Background"), m_background);
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    create->setIcon(Icons::get(QStringLiteral("plus")));
    create->setDefault(true);
    Theme::setRole(create, QStringLiteral("primary"));
    buttons->addButton(QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_name->selectAll();
    m_name->setFocus();
}

QString NewCanvasDialog::name() const
{
    return m_name->text();
}

QSize NewCanvasDialog::canvasSize() const
{
    return QSize(m_width->value(), m_height->value());
}

QColor NewCanvasDialog::background() const
{
    return m_background->currentData().value<QColor>();
}

} // namespace lumen::ui
