#include "ui/dialogs/PlanChangeDialog.h"

#include "ai/Provider.h"
#include "ai/StructuredChat.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace lumen::ui {

PlanChangeDialog::PlanChangeDialog(ai::LlmProvider *planner, const QString &model, const QImage &picture, const ai::PlanContext &context,
                                   QWidget *parent)
    : QDialog(parent)
    , m_context(context)
{
    setWindowTitle(tr("Plan this change"));
    setMinimumWidth(460);
    auto *v = new QVBoxLayout(this);
    auto *words = new QLabel(tr("The change: %1").arg(context.words), this);
    words->setWordWrap(true);
    v->addWidget(words);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("planStatus"));
    m_status->setWordWrap(true);
    m_status->setProperty("role", QStringLiteral("muted"));
    v->addWidget(m_status);

    m_fields = new QWidget(this);
    auto *form = new QFormLayout(m_fields);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_select = new QLineEdit(m_fields);
    m_select->setObjectName(QStringLiteral("planSelect"));
    m_select->setAccessibleName(tr("Select first"));
    m_select->setPlaceholderText(context.hasSelection ? tr("The selection as it is") : tr("The whole picture"));
    m_select->setToolTip(tr("What is selected before the change, as in Select → By description… (\"the black lines\", \"the cup\")."));
    form->addRow(tr("Select first"), m_select);
    m_prompt = new AutoTextEdit(m_fields, 2, 6);
    m_prompt->setObjectName(QStringLiteral("planPrompt"));
    m_prompt->setAccessibleName(tr("Request for the model"));
    m_prompt->setToolTip(tr("Sent to the image model exactly as it is here."));
    form->addRow(tr("Request for the model"), m_prompt);
    m_negativeRow = new QWidget(m_fields);
    auto *nv = new QVBoxLayout(m_negativeRow);
    nv->setContentsMargins(0, 0, 0, 0);
    m_negative = new AutoTextEdit(m_negativeRow, 1, 3);
    m_negative->setObjectName(QStringLiteral("planNegative"));
    m_negative->setAccessibleName(tr("Leave out"));
    nv->addWidget(m_negative);
    form->addRow(tr("Leave out"), m_negativeRow);
    form->setRowVisible(m_negativeRow, context.negative);
    m_lorasRow = new QWidget(m_fields);
    m_loras = new QVBoxLayout(m_lorasRow);
    m_loras->setContentsMargins(0, 0, 0, 0);
    m_loras->setSpacing(2);
    form->addRow(tr("LoRAs"), m_lorasRow);
    form->setRowVisible(m_lorasRow, false);
    m_note = new QLabel(m_fields);
    m_note->setObjectName(QStringLiteral("planNote"));
    m_note->setWordWrap(true);
    form->addRow(m_note);
    m_fields->hide();
    v->addWidget(m_fields);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_run = buttons->addButton(tr("Run"), QDialogButtonBox::AcceptRole);
    m_run->setObjectName(QStringLiteral("planRun"));
    m_run->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    if (!planner) {
        fail(tr("Planning needs a language model that sees pictures. Choose one for \"Vision analysis\" in Settings → Roles."));
        return;
    }
    m_status->setText(tr("%1 looks at the picture… (a local model may first take a while to load)").arg(planner->displayName()));
    ai::ChatRequest request = ai::changePlanRequest(picture, context);
    request.model = model;
    m_op = ai::requestJson(planner, request);
    QPointer<PlanChangeDialog> self(this);
    ai::StructuredChatOperation *op = m_op;
    connect(op, &ai::Operation::finished, this, [self, op] {
        if (!self)
            return;
        if (op->state() == ai::Operation::State::Succeeded)
            self->showPlan(ai::parseChangePlan(op->result(), self->m_context));
        else if (op->state() == ai::Operation::State::Failed)
            self->fail(tr("No plan: %1").arg(op->errorString()));
    });
}

PlanChangeDialog::~PlanChangeDialog()
{
    if (m_op && !m_op->isFinished())
        m_op->cancel();
}

void PlanChangeDialog::showPlan(const ai::ChangePlan &plan)
{
    m_ready = true;
    m_status->setText(tr("Check the plan and change what you like; nothing happens before Run."));
    m_status->setProperty("role", QStringLiteral("muted"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    qDeleteAll(m_loraBoxes);
    m_loraBoxes.clear();
    m_select->setText(plan.select);
    m_prompt->setPlainText(plan.prompt);
    m_negative->setPlainText(plan.negative);
    // Every LoRA that fits, ticked where the plan uses it.
    for (const QString &name : std::as_const(m_context.loras)) {
        auto *box = new QCheckBox(name, m_lorasRow);
        box->setObjectName(QStringLiteral("planLora"));
        box->setProperty("lora", name);
        box->setChecked(plan.loras.contains(name));
        m_loras->addWidget(box);
        m_loraBoxes << box;
    }
    if (auto *form = qobject_cast<QFormLayout *>(m_fields->layout()))
        form->setRowVisible(m_lorasRow, !m_loraBoxes.isEmpty());
    m_note->setText(plan.note);
    m_note->setVisible(!plan.note.isEmpty());
    m_fields->show();
    m_run->setEnabled(true);
    m_run->setDefault(true);
    adjustSize();
    emit planned();
}

void PlanChangeDialog::fail(const QString &message)
{
    m_ready = true;
    m_status->setText(message);
    m_status->setProperty("role", QStringLiteral("error"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    emit planned();
}

ai::ChangePlan PlanChangeDialog::plan() const
{
    ai::ChangePlan p;
    p.select = m_select->text().simplified();
    p.prompt = m_prompt->text().trimmed();
    p.negative = m_context.negative ? m_negative->text().trimmed() : QString();
    for (QCheckBox *box : m_loraBoxes) {
        if (box->isChecked())
            p.loras << box->property("lora").toString();
    }
    p.note = m_note->text();
    return p;
}

} // namespace lumen::ui
