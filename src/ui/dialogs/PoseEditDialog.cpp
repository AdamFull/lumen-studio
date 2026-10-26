#include "ui/dialogs/PoseEditDialog.h"

#include "ai/Director.h"
#include "pose/PosePresets.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/pose/PoseCanvas.h"
#include "ui/widgets/Controls.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

PoseEditDialog::PoseEditDialog(Studio *studio, const pose::Pose &pose, const std::optional<pose::RigParams> &rig, const QString &context,
                               QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_rig(rig)
    , m_context(context)
{
    setWindowTitle(tr("Edit pose"));
    // Tall for a standing figure, but within the screen (1080p at 150 %): the canvas takes what is left.
    resizeWithinScreen(this, QSize(760, 860));
    auto *v = new QVBoxLayout(this);
    if (!context.isEmpty())
        v->addWidget(mutedLabel(context, this));
    auto *bar = new QHBoxLayout;
    auto *modes = new QButtonGroup(this);
    const QList<std::pair<pose::DragMode, QString>> list = {{pose::DragMode::Rotate, tr("Rotate")},
                                                           {pose::DragMode::Chain, tr("IK")},
                                                           {pose::DragMode::Free, tr("Free")},
                                                           {pose::DragMode::Translate, tr("Move part")}};
    for (const auto &[mode, label] : list) {
        auto *b = new QToolButton(this);
        b->setText(label);
        b->setCheckable(true);
        b->setProperty("role", QStringLiteral("chip"));
        modes->addButton(b, static_cast<int>(mode));
        bar->addWidget(b);
    }
    modes->button(static_cast<int>(pose::DragMode::Rotate))->setChecked(true);
    bar->addStretch();
    auto *presets = new QPushButton(Icons::get(QStringLiteral("person")), tr("Preset"), this);
    auto *menu = new QMenu(presets);
    QHash<QString, QMenu *> cats;
    for (const pose::PosePreset &p : pose::builtInPresets()) {
        if (p.rigs.size() != 1)
            continue;
        QMenu *&m = cats[p.category];
        if (!m)
            m = menu->addMenu(p.category);
        m->addAction(p.name, this, [this, p] {
            pose::RigParams r = p.rigs.first();
            if (m_rig) {
                r.framing = m_rig->framing;
                r.expression = p.rigs.first().expression == QLatin1String("neutral") ? m_rig->expression : r.expression;
            }
            m_rig = r;
            pose::Pose next = m_canvas->pose();
            next.people = {pose::solveRig(r, next.canvas)};
            m_canvas->setPose(next);
        });
    }
    presets->setMenu(menu);
    bar->addWidget(presets);
    v->addLayout(bar);
    m_canvas = new PoseCanvas(this);
    m_canvas->setPose(pose);
    v->addWidget(m_canvas, 1);
    auto *row = new QHBoxLayout;
    m_describe = new QLineEdit(this);
    m_describe->setPlaceholderText(tr("Describe a change, e.g. 'raise the left hand to wave', 'turn a bit to the right'"));
    m_describeBtn = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Apply"), this);
    row->addWidget(m_describe, 1);
    row->addWidget(m_describeBtn);
    v->addLayout(row);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(modes, &QButtonGroup::idClicked, this, [this](int id) { m_canvas->setDragMode(static_cast<pose::DragMode>(id)); });
    connect(m_canvas, &PoseCanvas::editFinished, this, [this] { m_rig.reset(); });
    connect(m_describeBtn, &QPushButton::clicked, this, &PoseEditDialog::describe);
    connect(m_describe, &QLineEdit::returnPressed, this, &PoseEditDialog::describe);
}

pose::Pose PoseEditDialog::pose() const
{
    return m_canvas->pose();
}

void PoseEditDialog::describe()
{
    const QString text = m_describe->text().trimmed();
    if (text.isEmpty())
        return;
    const pose::RigParams base = m_rig.value_or(pose::RigParams());
    QJsonObject shot{{QStringLiteral("description"), m_context}, {QStringLiteral("pose"), base.toJson()}};
    ai::StructuredChatOperation *op = m_studio->director()->revisePose(shot, text);
    m_describeBtn->setEnabled(false);
    m_describeBtn->setText(tr("Thinking…"));
    QPointer<PoseEditDialog> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, base] {
        if (!self)
            return;
        self->m_describeBtn->setEnabled(true);
        self->m_describeBtn->setText(tr("Apply"));
        if (op->state() != ai::Operation::State::Succeeded) {
            if (op->state() == ai::Operation::State::Failed)
                self->m_studio->notify(op->errorString(), QStringLiteral("error"));
            return;
        }
        const pose::RigParams r = pose::RigParams::fromJson(op->result(), base);
        self->m_rig = r;
        pose::Pose next = self->m_canvas->pose();
        next.people = {pose::solveRig(r, next.canvas)};
        self->m_canvas->setPose(next);
        self->m_describe->clear();
    });
}

} // namespace lumen::ui
