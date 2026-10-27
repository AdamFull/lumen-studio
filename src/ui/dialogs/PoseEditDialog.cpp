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
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lumen::ui {

PoseEditDialog::PoseEditDialog(Studio *studio, const pose::Pose &pose, const QList<std::optional<pose::RigParams>> &rigs,
                               const QString &context, const QStringList &names, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_rigs(rigs)
    , m_context(context)
{
    while (m_rigs.size() < std::max<qsizetype>(1, pose.people.size()))
        m_rigs << std::nullopt;
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
            const int index = std::clamp(m_canvas->selectedPerson(), 0, int(m_rigs.size()) - 1);
            pose::RigParams r = p.rigs.first();
            if (const std::optional<pose::RigParams> &was = m_rigs.at(index)) {
                r.framing = was->framing;
                r.expression = p.rigs.first().expression == QLatin1String("neutral") ? was->expression : r.expression;
            }
            applyRig(index, r);
        });
    }
    presets->setMenu(menu);
    bar->addWidget(presets);
    v->addLayout(bar);
    // Two people: whom a preset or a described change is for (a click on a person chooses too).
    if (pose.people.size() > 1) {
        auto *who = new QHBoxLayout;
        who->addWidget(new QLabel(tr("Changes apply to:"), this));
        auto *people = new QButtonGroup(this);
        for (int i = 0; i < pose.people.size(); ++i) {
            auto *b = new QToolButton(this);
            b->setObjectName(QStringLiteral("posePerson"));
            b->setText(i < names.size() && !names.at(i).isEmpty() ? names.at(i)
                       : i == 0                                     ? tr("The left person")
                                                                    : tr("The right person"));
            b->setCheckable(true);
            b->setProperty("role", QStringLiteral("chip"));
            people->addButton(b, i);
            who->addWidget(b);
        }
        who->addStretch();
        people->button(0)->setChecked(true);
        v->addLayout(who);
        connect(people, &QButtonGroup::idClicked, this, [this](int id) { m_canvas->setSelectedPerson(id); });
        m_people = people;
    }
    m_canvas = new PoseCanvas(this);
    m_canvas->setPose(pose);
    v->addWidget(m_canvas, 1);
    if (m_people) {
        connect(m_canvas, &PoseCanvas::personSelected, this, [this](int index) {
            if (QAbstractButton *b = m_people->button(index))
                b->setChecked(true);
        });
    }
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
    // Moved by hand: that person's rig no longer says where its joints are.
    connect(m_canvas, &PoseCanvas::editFinished, this, [this] {
        const int index = m_canvas->selectedPerson();
        if (index >= 0 && index < m_rigs.size())
            m_rigs[index].reset();
    });
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
    const int index = std::clamp(m_canvas->selectedPerson(), 0, int(m_rigs.size()) - 1);
    const pose::RigParams base = m_rigs.at(index).value_or(pose::RigParams());
    QJsonObject shot{{QStringLiteral("description"), m_context}, {QStringLiteral("pose"), base.toJson()}};
    ai::StructuredChatOperation *op = m_studio->director()->revisePose(shot, text);
    m_describeBtn->setEnabled(false);
    m_describeBtn->setText(tr("Thinking…"));
    QPointer<PoseEditDialog> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, base, index] {
        if (!self)
            return;
        self->m_describeBtn->setEnabled(true);
        self->m_describeBtn->setText(tr("Apply"));
        if (op->state() != ai::Operation::State::Succeeded) {
            if (op->state() == ai::Operation::State::Failed)
                self->m_studio->notify(op->errorString(), QStringLiteral("error"));
            return;
        }
        self->applyRig(index, pose::RigParams::fromJson(op->result(), base));
        self->m_describe->clear();
    });
}

void PoseEditDialog::applyRig(int index, pose::RigParams rig)
{
    pose::Pose next = m_canvas->pose();
    if (next.people.size() > 1) {
        // The person stays where it stands; only the pose changes.
        if (const std::optional<pose::RigParams> &was = m_rigs.value(index)) {
            rig.positionX = was->positionX;
            rig.positionY = was->positionY;
            rig.scale = was->scale;
        }
    }
    if (index >= 0 && index < next.people.size())
        next.people[index] = pose::solveRig(rig, next.canvas);
    else
        next.people = {pose::solveRig(rig, next.canvas)};
    if (index >= 0 && index < m_rigs.size())
        m_rigs[index] = rig;
    m_canvas->setPose(next);
    m_canvas->setSelectedPerson(index);
}

} // namespace lumen::ui
