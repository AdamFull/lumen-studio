#include "ui/widgets/RunSettingsEditor.h"

#include "ai/Provider.h"
#include "ui/pages/AiOptions.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>

namespace lumen::ui {

namespace {

QString number(double v)
{
    return QLocale().toString(v, 'g', 3);
}

} // namespace

RunSettingsEditor::RunSettingsEditor(AiOptions *options, QWidget *parent)
    : QWidget(parent)
    , m_options(options)
{
    setObjectName(QStringLiteral("runSettings"));
    m_form = new QFormLayout(this);
    m_form->setContentsMargins(0, 0, 0, 0);
    m_form->setSpacing(6);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_model = new QLabel(this);
    m_model->setObjectName(QStringLiteral("runSettingsModel"));
    m_model->setWordWrap(true);
    m_model->setProperty("role", QStringLiteral("muted"));
    m_form->addRow(m_model);

    m_steps = new QSpinBox(this);
    m_steps->setAccessibleName(tr("Steps"));
    m_steps->setRange(0, 150);
    m_steps->setToolTip(tr("More steps can add detail and take longer. The model's choice is right for most models."));
    m_form->addRow(tr("Steps"), m_steps);
    m_cfg = new QDoubleSpinBox(this);
    m_cfg->setAccessibleName(tr("Follow the words"));
    m_cfg->setRange(0, 30);
    m_cfg->setSingleStep(0.5);
    m_cfg->setDecimals(1);
    m_cfg->setToolTip(tr("How closely the image follows the description (CFG). Higher follows it more strictly but can look harsh; "
                         "lower is freer and softer."));
    m_form->addRow(tr("Follow the words"), m_cfg);
    m_distilled = new QLabel(tr("This model is distilled and made for CFG 1 (the model's choice). A higher CFG makes it slower and "
                                "can burn the image; the klein base models do take it."),
                             this);
    m_distilled->setObjectName(QStringLiteral("runSettingsDistilled"));
    m_distilled->setWordWrap(true);
    m_distilled->setProperty("role", QStringLiteral("muted"));
    m_form->addRow(m_distilled);
    m_guidance = new QDoubleSpinBox(this);
    m_guidance->setAccessibleName(tr("Guidance"));
    m_guidance->setRange(0, 10);
    m_guidance->setSingleStep(0.5);
    m_guidance->setDecimals(1);
    m_guidance->setToolTip(tr("FLUX.1 dev's built-in guidance (distilled guidance): how closely it follows the description. 3.5 is usual; "
                              "lower looks more natural."));
    m_form->addRow(tr("Guidance"), m_guidance);
    m_imageCfg = new QDoubleSpinBox(this);
    m_imageCfg->setAccessibleName(tr("Keep the picture"));
    m_imageCfg->setRange(0, 10);
    m_imageCfg->setSingleStep(0.5);
    m_imageCfg->setDecimals(1);
    m_imageCfg->setToolTip(tr("How closely an edit keeps the picture it changes (image guidance)."));
    m_form->addRow(tr("Keep the picture"), m_imageCfg);
    m_flowShift = new QDoubleSpinBox(this);
    m_flowShift->setAccessibleName(tr("Flow shift"));
    m_flowShift->setRange(0, 20);
    m_flowShift->setSingleStep(0.5);
    m_flowShift->setDecimals(1);
    m_flowShift->setToolTip(tr("Where the model spends its steps (flow shift): higher puts more into the overall layout, lower into "
                               "fine detail."));
    m_form->addRow(tr("Flow shift"), m_flowShift);
    m_sampler = new QComboBox(this);
    m_sampler->setAccessibleName(tr("Sampler"));
    m_sampler->setToolTip(tr("The method that turns noise into the image, step by step. The model's choice suits it best."));
    m_form->addRow(tr("Sampler"), m_sampler);
    m_scheduler = new QComboBox(this);
    m_scheduler->setAccessibleName(tr("Scheduler"));
    m_scheduler->setToolTip(tr("How the steps are spread from rough to fine (scheduler)."));
    m_form->addRow(tr("Scheduler"), m_scheduler);
    m_clipSkip = new QSpinBox(this);
    m_clipSkip->setAccessibleName(tr("CLIP skip"));
    m_clipSkip->setRange(0, 4);
    m_clipSkip->setToolTip(tr("Leaves out the text encoder's last layers (CLIP skip). Pony and Illustrious models usually want 2."));
    m_form->addRow(tr("CLIP skip"), m_clipSkip);
    m_reset = new QPushButton(tr("Back to the model's choice"), this);
    m_reset->setObjectName(QStringLiteral("runSettingsReset"));
    m_reset->setToolTip(tr("Every setting here goes back to the model's own value"));
    m_form->addRow(m_reset);

    for (QSpinBox *s : {m_steps, m_clipSkip})
        connect(s, &QSpinBox::valueChanged, this, &RunSettingsEditor::store);
    for (QDoubleSpinBox *s : {m_cfg, m_guidance, m_imageCfg, m_flowShift})
        connect(s, &QDoubleSpinBox::valueChanged, this, &RunSettingsEditor::store);
    for (QComboBox *c : {m_sampler, m_scheduler})
        connect(c, &QComboBox::activated, this, &RunSettingsEditor::store);
    connect(m_reset, &QPushButton::clicked, this, [this] { m_options->setRunSettings(m_spec.key, {}); });
    connect(m_options, &AiOptions::changed, this, &RunSettingsEditor::showValues);
    setEngine(nullptr);
}

void RunSettingsEditor::setEngine(ai::ImageProvider *provider)
{
    const ai::RunSettingsSpec spec = ai::runSettingsFor(provider);
    const bool engineChanged = provider != m_engine;
    m_engine = provider;
    m_spec = spec;
    m_model->setText(spec.modelName.isEmpty() ? QString() : tr("Settings for %1").arg(spec.modelName));
    m_form->setRowVisible(m_model, !spec.modelName.isEmpty());
    m_form->setRowVisible(m_steps, spec.steps);
    m_form->setRowVisible(m_cfg, spec.cfg);
    m_form->setRowVisible(m_distilled, spec.distilled);
    m_form->setRowVisible(m_guidance, spec.guidance);
    m_form->setRowVisible(m_imageCfg, spec.imageCfg);
    m_form->setRowVisible(m_flowShift, spec.flowShift);
    m_form->setRowVisible(m_sampler, spec.sampler);
    m_form->setRowVisible(m_scheduler, spec.sampler);
    m_form->setRowVisible(m_clipSkip, spec.clipSkip);
    m_form->setRowVisible(m_reset, spec.steps || spec.cfg || spec.guidance || spec.imageCfg || spec.flowShift || spec.sampler || spec.clipSkip);
    // "Model's choice" with the model's own value, where it is known.
    auto choice = [this](double v) { return v > 0 ? tr("Model's choice (%1)").arg(number(v)) : tr("Model's choice"); };
    m_steps->setSpecialValueText(choice(spec.modelSteps));
    m_cfg->setSpecialValueText(choice(spec.modelCfg));
    m_guidance->setSpecialValueText(choice(spec.modelGuidance));
    m_imageCfg->setSpecialValueText(choice(spec.modelImageCfg));
    m_flowShift->setSpecialValueText(choice(spec.modelFlowShift));
    m_clipSkip->setSpecialValueText(tr("Model's choice"));
    if (engineChanged || m_sampler->count() == 0)
        fillLists();
    showValues();
}

void RunSettingsEditor::fillLists()
{
    auto reset = [this](QComboBox *box, const QString &model) {
        const QSignalBlocker block(box);
        box->clear();
        box->addItem(model.isEmpty() ? tr("Model's choice") : tr("Model's choice (%1)").arg(model), QString());
    };
    reset(m_sampler, m_spec.modelSampler);
    reset(m_scheduler, m_spec.modelScheduler);
    if (!m_engine || !m_spec.sampler)
        return;
    const int request = ++m_listRequest;
    QPointer<RunSettingsEditor> self(this);
    auto fill = [self, request](QComboBox *box, const QStringList &names) {
        if (!self || request != self->m_listRequest)
            return;
        {
            const QSignalBlocker block(box);
            for (const QString &n : names)
                box->addItem(n, n);
        }
        self->showValues();
    };
    m_engine->listSamplers().then(this, [fill, this](const QStringList &names) { fill(m_sampler, names); });
    m_engine->listSchedulers().then(this, [fill, this](const QStringList &names) { fill(m_scheduler, names); });
}

void RunSettingsEditor::showValues()
{
    const ai::RunSettings s = m_options->runSettings(m_spec.key);
    const QSignalBlocker b1(m_steps), b2(m_cfg), b3(m_guidance), b4(m_imageCfg), b5(m_flowShift), b6(m_sampler), b7(m_scheduler),
        b8(m_clipSkip);
    m_steps->setValue(s.steps);
    m_cfg->setValue(s.cfg);
    m_guidance->setValue(s.guidance);
    m_imageCfg->setValue(s.imageCfg);
    m_flowShift->setValue(s.flowShift);
    m_clipSkip->setValue(s.clipSkip);
    auto select = [](QComboBox *box, const QString &value) {
        int i = box->findData(value);
        if (i < 0 && !value.isEmpty()) {
            // Chosen before but not listed now (the server is not running): still shown.
            box->addItem(value, value);
            i = box->count() - 1;
        }
        box->setCurrentIndex(std::max(0, i));
    };
    select(m_sampler, s.sampler);
    select(m_scheduler, s.scheduler);
    m_reset->setEnabled(!s.isEmpty());
}

void RunSettingsEditor::store()
{
    if (m_spec.key.isEmpty())
        return;
    ai::RunSettings s = m_options->runSettings(m_spec.key);
    s.steps = m_steps->value();
    s.cfg = m_cfg->value();
    s.guidance = m_guidance->value();
    s.imageCfg = m_imageCfg->value();
    s.flowShift = m_flowShift->value();
    s.clipSkip = m_clipSkip->value();
    s.sampler = m_sampler->currentData().toString();
    s.scheduler = m_scheduler->currentData().toString();
    m_options->setRunSettings(m_spec.key, s);
}

} // namespace lumen::ui
