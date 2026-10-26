#include "ui/pages/PromptBar.h"

#include "ai/PromptStyle.h"
#include "doc/Document.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/pages/AiOptions.h"
#include "ui/pages/AiPanel.h"
#include "ui/pages/VariantStrip.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

PromptBar::PromptBar(Studio *studio, AiOptions *options, AiPanel *ai, VariantStrip *strip, QWidget *parent)
    : QFrame(parent)
    , m_studio(studio)
    , m_options(options)
    , m_ai(ai)
    , m_strip(strip)
{
    setObjectName(QStringLiteral("promptBar"));
    const Palette &pal = Theme::palette();
    setStyleSheet(QStringLiteral("#promptBar { background: %1; border-top: 1px solid %2; }").arg(pal.panel.name(), pal.border.name()));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 6, 10, 8);
    v->setSpacing(4);

    // What Generate does now, or why it cannot, with the ways out.
    auto *hintRow = new QHBoxLayout;
    hintRow->setSpacing(6);
    m_icon = new QLabel(this);
    m_icon->setPixmap(Icons::pixmap(QStringLiteral("sparkles"), 14, pal.accent));
    hintRow->addWidget(m_icon);
    m_hint = new ElidedLabel(QString(), this);
    m_hint->setObjectName(QStringLiteral("promptHint"));
    m_hint->setProperty("role", QStringLiteral("muted"));
    hintRow->addWidget(m_hint, 1);
    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(tr("Progress"));
    m_progress->setTextVisible(false);
    m_progress->setFixedSize(140, 6);
    m_progress->hide();
    hintRow->addWidget(m_progress);
    auto offer = [this, hintRow](const QString &name, const QString &text, const QString &tip) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(name);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        Theme::setRole(b, QStringLiteral("ghost"));
        b->hide();
        hintRow->addWidget(b);
        return b;
    };
    m_setUp = offer(QStringLiteral("promptSetUpAi"), tr("Set up AI…"), tr("Connect an image engine on this computer or online"));
    m_selectArea = offer(QStringLiteral("promptSelectArea"), tr("Select an area"),
                         tr("Draw a selection around what should change; only that area is then changed (M)"));
    m_variation = offer(QStringLiteral("promptVariation"), tr("Make a variation"),
                        tr("Redraws the whole image from your words, keeping its layout. How much it may change is the "
                           "strength in the ⋯ menu."));
    v->addLayout(hintRow);

    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    m_prompt = new AutoTextEdit(this, 1, 3);
    m_prompt->setObjectName(QStringLiteral("promptText"));
    m_prompt->setAccessibleName(tr("Prompt"));
    m_prompt->setSubmitOnEnter(true);
    m_prompt->setToolTip(tr("Enter runs it; Shift+Enter starts a new line."));
    // A little less padding than other text fields, so one line is as high as the buttons.
    m_prompt->setStyleSheet(QStringLiteral("QPlainTextEdit { padding: 4px 6px; }"));
    row->addWidget(m_prompt, 1);
    m_generate = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Generate"), this);
    m_generate->setObjectName(QStringLiteral("promptGenerate"));
    m_generate->setMinimumHeight(36);
    Theme::setRole(m_generate, QStringLiteral("primary"));
    row->addWidget(m_generate, 0, Qt::AlignTop);
    m_remove = new QPushButton(Icons::get(QStringLiteral("eraser")), tr("Remove"), this);
    m_remove->setObjectName(QStringLiteral("promptRemove"));
    m_remove->setMinimumHeight(36);
    m_remove->setToolTip(tr("Remove what is in the selected area and fill it in from its surroundings"));
    row->addWidget(m_remove, 0, Qt::AlignTop);
    m_more = iconButton(QStringLiteral("dots"), tr("More options"), this);
    m_more->setObjectName(QStringLiteral("promptMore"));
    m_more->setMinimumHeight(36);
    row->addWidget(m_more, 0, Qt::AlignTop);
    v->addLayout(row);

    buildOptions();

    m_update.setSingleShot(true);
    m_update.setInterval(30);
    connect(&m_update, &QTimer::timeout, this, &PromptBar::updateUi);
    connect(m_prompt, &AutoTextEdit::submitted, this, &PromptBar::generate);
    connect(m_prompt, &QPlainTextEdit::textChanged, this, [this] {
        m_options->setPrompt(m_prompt->text());
        updateGenerate();
    });
    connect(m_generate, &QPushButton::clicked, this, [this] {
        if (m_strip->isRunning())
            cancel();
        else
            generate();
    });
    connect(m_remove, &QPushButton::clicked, this, &PromptBar::remove);
    connect(m_variation, &QPushButton::clicked, this, &PromptBar::variation);
    connect(m_selectArea, &QPushButton::clicked, this, &PromptBar::selectAreaRequested);
    connect(m_setUp, &QPushButton::clicked, this, &PromptBar::setUpAiRequested);
    connect(m_more, &QToolButton::clicked, this, &PromptBar::showOptions);
    connect(m_options, &AiOptions::changed, this, [this] {
        if (m_popup->isVisible())
            updateOptions();
        scheduleUpdate();
    });
    connect(m_strip, &VariantStrip::stateChanged, this, &PromptBar::updateUi);
    connect(studio->providers(), &ai::ProviderRegistry::providersChanged, this, &PromptBar::scheduleUpdate);
    connect(studio->providers(), &ai::ProviderRegistry::rolesChanged, this, &PromptBar::scheduleUpdate);
    connect(ai->references(), &ReferenceStrip::changed, this, [this] {
        if (m_popup->isVisible())
            updateOptions();
    });
    updateUi();
}

void PromptBar::buildOptions()
{
    m_popup = new QFrame(this, Qt::Popup);
    m_popup->setObjectName(QStringLiteral("promptOptions"));
    m_popup->setFrameShape(QFrame::StyledPanel);
    m_popup->setMinimumWidth(360);
    m_form = new QFormLayout(m_popup);
    m_form->setContentsMargins(14, 12, 14, 12);
    m_form->setSpacing(8);
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_strength = new SliderField(tr("Strength"), 0.05, 1.0, m_options->regionStrength(), 2, QString(), m_popup);
    m_form->addRow(m_strength);
    m_steps = new QSpinBox(m_popup);
    m_steps->setAccessibleName(tr("Steps"));
    m_steps->setRange(0, 150);
    m_steps->setSpecialValueText(tr("Model's choice"));
    m_steps->setToolTip(tr("More steps can add detail and take longer. The model's choice is right for most models."));
    m_form->addRow(tr("Steps"), m_steps);
    m_seedRow = new QWidget(m_popup);
    auto *seedRow = new QHBoxLayout(m_seedRow);
    seedRow->setContentsMargins(0, 0, 0, 0);
    m_seed = new QSpinBox(m_seedRow);
    m_seed->setAccessibleName(tr("Seed"));
    m_seed->setRange(0, 2147483647);
    m_seed->setToolTip(tr("The same seed, words and settings give the same result again."));
    m_randomSeed = new QCheckBox(tr("Random"), m_seedRow);
    seedRow->addWidget(m_seed, 1);
    seedRow->addWidget(m_randomSeed);
    m_form->addRow(tr("Seed"), m_seedRow);
    m_variants = new QSpinBox(m_popup);
    m_variants->setAccessibleName(tr("Variants"));
    m_variants->setRange(1, AiOptions::kMaxVariants);
    m_variants->setToolTip(tr("How many results each run makes to choose from."));
    m_form->addRow(tr("Variants"), m_variants);
    m_negative = new AutoTextEdit(m_popup, 1, 3);
    m_negative->setAccessibleName(tr("Negative prompt"));
    m_negative->setPlaceholderText(tr("Things that should not be in the result"));
    m_form->addRow(tr("Leave out"), m_negative);
    m_references = new QPushButton(m_popup);
    m_references->setToolTip(tr("Pictures the engine follows for a character, a style or an object (in the AI panel)"));
    m_form->addRow(tr("References"), m_references);
    m_pose = new QCheckBox(tr("Follow the document's control pose"), m_popup);
    m_form->addRow(m_pose);
    m_engine = new ProviderCombo(m_studio->providers(), ai::ProviderKind::Image, m_popup);
    m_engine->setObjectName(QStringLiteral("promptEngine"));
    m_form->addRow(tr("Image engine"), m_engine);
    m_allOptions = new QPushButton(Icons::get(QStringLiteral("adjust")), tr("All AI options…"), m_popup);
    m_allOptions->setToolTip(tr("The AI panel: every option, and the actions that are not a description (upscale, expand, "
                                "remove background…)"));
    m_form->addRow(m_allOptions);

    connect(m_strength, &SliderField::valueChanged, this, [this](double v) {
        if (plan().action == PromptAction::CannotEditImage)
            m_options->setVariationStrength(v);
        else
            m_options->setRegionStrength(v);
    });
    connect(m_steps, &QSpinBox::valueChanged, m_options, &AiOptions::setSteps);
    connect(m_seed, &QSpinBox::valueChanged, this, [this](int seed) {
        if (!m_randomSeed->isChecked())
            m_options->setSeed(seed);
    });
    connect(m_randomSeed, &QCheckBox::toggled, this, [this](bool random) { m_options->setSeed(random ? -1 : m_seed->value()); });
    connect(m_variants, &QSpinBox::valueChanged, m_options, &AiOptions::setVariants);
    connect(m_negative, &QPlainTextEdit::textChanged, this, [this] { m_options->setNegative(m_negative->text()); });
    connect(m_pose, &QCheckBox::toggled, m_options, &AiOptions::setUsePose);
    connect(m_engine, &ProviderCombo::providerChanged, m_options, &AiOptions::setProviderId);
    connect(m_references, &QPushButton::clicked, this, [this] {
        m_popup->hide();
        emit allOptionsRequested();
    });
    connect(m_allOptions, &QPushButton::clicked, this, [this] {
        m_popup->hide();
        emit allOptionsRequested();
    });
}

void PromptBar::setDocument(doc::Document *doc)
{
    if (m_doc)
        m_doc->disconnect(this);
    m_doc = doc;
    if (doc) {
        connect(doc, &doc::Document::selectionChanged, this, &PromptBar::scheduleUpdate);
        connect(doc, &doc::Document::layersChanged, this, &PromptBar::scheduleUpdate);
        connect(doc, &doc::Document::pixelsChanged, this, &PromptBar::scheduleUpdate);
        connect(doc, &doc::Document::sizeChanged, this, &PromptBar::scheduleUpdate);
        connect(doc, &doc::Document::controlPoseChanged, this, &PromptBar::scheduleUpdate);
    }
    updateUi();
}

void PromptBar::scheduleUpdate()
{
    m_update.start();
}

bool PromptBar::blank() const
{
    if (!m_doc)
        return false;
    if (m_blankDoc != m_doc || m_blankRevision != m_doc->revision()) {
        m_blankDoc = m_doc;
        m_blankRevision = m_doc->revision();
        m_blank = isBlankCanvas(*m_doc);
    }
    return m_blank;
}

PromptPlan PromptBar::plan() const
{
    if (!m_doc)
        return {};
    ai::ImageProvider *provider = m_options->provider(m_studio->providers());
    const ai::ImageCapabilities caps = provider ? provider->capabilities() : ai::ImageCapabilities();
    const bool selection = m_doc->hasSelection() && !m_doc->selectionBounds().isEmpty();
    return promptPlan(selection, blank(), provider ? &caps : nullptr);
}

QString PromptBar::hint() const
{
    return m_hint->fullText();
}

QString PromptBar::prompt() const
{
    return m_prompt->text();
}

void PromptBar::setPrompt(const QString &prompt, const QString &negative)
{
    m_prompt->setPlainText(prompt);
    m_options->setNegative(negative);
    focusPrompt();
}

void PromptBar::focusPrompt()
{
    m_prompt->setFocus();
    m_prompt->moveCursor(QTextCursor::End);
}

void PromptBar::updateUi()
{
    m_update.stop();
    m_plan = plan();
    const PromptPlan &p = m_plan;
    const bool running = m_strip->isRunning();
    QString hint;
    QString placeholder = tr("Describe the change, e.g. “colorize it” or “replace the cup with a candle”");
    switch (p.action) {
    case PromptAction::NoEngine:
        hint = tr("No image engine is set up yet, so nothing can be drawn or changed.");
        break;
    case PromptAction::EditArea:
        hint = tr("Changes the selected area. Describe the change; the rest of the image stays as it is.");
        break;
    case PromptAction::InpaintArea:
        hint = tr("Changes the selected area. Describe what should be there; the rest of the image stays as it is.");
        placeholder = tr("Describe what should be there, e.g. “a red scarf” or “an empty table”");
        break;
    case PromptAction::RedrawArea:
        hint = tr("Changes the selected area by redrawing it. Describe what should be there; the rest stays as it is.");
        placeholder = tr("Describe what should be there, e.g. “a red scarf” or “an empty table”");
        break;
    case PromptAction::EditImage:
        hint = tr("Changes the whole image. Describe the change, or select an area to change only that.");
        placeholder = tr("Describe the change, e.g. “colorize this photo” or “make it night”");
        break;
    case PromptAction::CannotEditImage:
        hint = tr("This image engine cannot change the whole image from a description.");
        placeholder = tr("Describe the image for a variation, or select an area to change");
        break;
    case PromptAction::NewImage:
        hint = tr("Creates an image of %1 × %2 px. Describe what it should show.").arg(m_doc->size().width()).arg(m_doc->size().height());
        placeholder = tr("Describe the image, e.g. “a lighthouse on a cliff at dusk”");
        break;
    case PromptAction::CannotCreate:
        hint = tr("This image engine cannot create an image from a description.");
        break;
    case PromptAction::CannotChangeImages:
        hint = tr("This image engine only creates new images; it cannot change this one.");
        break;
    }
    if (running) {
        const double progress = m_strip->progress();
        const QString status = m_strip->status();
        const int pending = m_strip->pending();
        hint = pending > 1 ? tr("Working on %1 results…").arg(pending) : tr("Working on it…");
        if (!status.isEmpty())
            hint += u' ' + status;
        m_progress->setRange(0, progress >= 0 ? 100 : 0);
        m_progress->setValue(progress >= 0 ? int(progress * 100) : 0);
    }
    m_hint->setFullText(hint);
    m_progress->setVisible(running);
    m_prompt->setPlaceholderText(placeholder);
    m_setUp->setVisible(!running && p.action == PromptAction::NoEngine);
    m_selectArea->setVisible(!running && p.action == PromptAction::CannotEditImage);
    m_variation->setVisible(!running && p.action == PromptAction::CannotEditImage && p.variation);

    m_generate->setText(running ? tr("Cancel") : tr("Generate"));
    m_generate->setIcon(Icons::get(running ? QStringLiteral("stop") : QStringLiteral("sparkles")));
    m_generate->setToolTip(running ? tr("Stop the run") : tr("Run it (Enter)"));
    if (m_generate->property("role").toString() != (running ? QString() : QStringLiteral("primary")))
        Theme::setRole(m_generate, running ? QString() : QStringLiteral("primary"));
    updateGenerate();
    m_remove->setVisible(p.region);
    m_remove->setEnabled(!running);
    if (m_popup->isVisible())
        updateOptions();
}

void PromptBar::updateGenerate()
{
    // Words are needed except to cancel.
    m_generate->setEnabled(m_strip->isRunning() || (m_plan.canRun() && !m_prompt->text().trimmed().isEmpty()));
}

void PromptBar::updateOptions()
{
    const PromptPlan p = plan();
    ai::ImageProvider *provider = m_options->provider(m_studio->providers());
    const ai::ImageCapabilities caps = provider ? provider->capabilities() : ai::ImageCapabilities();
    const bool variation = p.action == PromptAction::CannotEditImage;
    {
        const QSignalBlocker b1(m_strength), b2(m_steps), b3(m_seed), b4(m_randomSeed), b5(m_variants), b6(m_pose);
        m_strength->setValue(variation ? m_options->variationStrength() : m_options->regionStrength());
        m_strength->setToolTip(variation ? tr("How much a variation may change the image: low keeps more of it.")
                                         : tr("How much the selected area may change: low keeps more of what is there."));
        m_steps->setValue(m_options->steps());
        m_randomSeed->setChecked(m_options->seed() < 0);
        m_seed->setEnabled(m_options->seed() >= 0);
        if (m_options->seed() >= 0)
            m_seed->setValue(int(std::min<qint64>(m_options->seed(), m_seed->maximum())));
        m_variants->setValue(m_options->variants());
        m_pose->setChecked(m_options->usePose());
    }
    // Not blocked: the field fits its height to the text (setting the same words back changes nothing).
    if (m_negative->text() != m_options->negative())
        m_negative->setPlainText(m_options->negative());
    if (!m_options->providerId().isEmpty()) {
        const QSignalBlocker b(m_engine);
        m_engine->setProviderId(m_options->providerId());
    }
    const int refs = m_ai->references()->count();
    m_references->setText(refs == 0 ? tr("Add…") : refs == 1 ? tr("1 picture…") : tr("%1 pictures…").arg(refs));
    // Only what the request uses and the engine takes.
    m_form->setRowVisible(m_strength, p.strength);
    m_form->setRowVisible(m_steps, !provider || caps.steps);
    m_form->setRowVisible(m_seedRow, !provider || caps.seed);
    const bool negative = provider && caps.negativePrompt && ai::promptStyleFor(provider).supportsNegative;
    m_form->setRowVisible(m_negative, negative);
    m_form->setRowVisible(m_references, caps.maxReferences > 0);
    m_form->setRowVisible(m_pose, caps.poseControl && m_doc && m_doc->controlPose());
}

void PromptBar::showOptions()
{
    updateOptions();
    m_popup->adjustSize();
    // Above the button: the bar is at the bottom of the window.
    QPoint pos = m_more->mapToGlobal(QPoint(m_more->width() - m_popup->width(), -m_popup->height() - 4));
    if (const QScreen *screen = m_more->screen()) {
        const QRect area = screen->availableGeometry();
        pos.setX(std::clamp(pos.x(), area.left(), std::max(area.left(), area.right() - m_popup->width())));
        pos.setY(std::clamp(pos.y(), area.top(), std::max(area.top(), area.bottom() - m_popup->height())));
    }
    m_popup->move(pos);
    m_popup->show();
}

void PromptBar::submit(const PromptPlan &plan, ai::GenMode mode, const QString &words, double strength, bool withCharacter,
                       const QString &name)
{
    AiPanel::PromptRun run;
    run.mode = mode;
    run.region = plan.region;
    run.prompt = words;
    run.strength = strength;
    run.withCharacter = withCharacter;
    run.name = name;
    if (plan.region)
        run.action = mode == ai::GenMode::Edit ? AiPanel::Mode::Edit : AiPanel::Mode::Inpaint;
    else if (mode == ai::GenMode::Edit)
        run.action = AiPanel::Mode::Edit;
    else if (mode == ai::GenMode::ImageToImage)
        run.action = AiPanel::Mode::Variation;
    m_ai->submitPrompt(run);
    updateUi();
}

void PromptBar::generate()
{
    if (!m_doc || m_strip->isRunning())
        return;
    const PromptPlan p = plan();
    const QString words = m_prompt->text().trimmed();
    if (p.action == PromptAction::NoEngine) {
        emit setUpAiRequested();
        return;
    }
    if (!p.canRun()) {
        m_studio->notify(hint(), QStringLiteral("warning"));
        return;
    }
    if (words.isEmpty()) {
        focusPrompt();
        return;
    }
    submit(p, p.mode, words, p.strength ? m_options->regionStrength() : 1.0, true, words.simplified().left(40));
}

void PromptBar::remove()
{
    if (!m_doc || m_strip->isRunning())
        return;
    const PromptPlan p = plan();
    if (!p.region)
        return;
    // Filling an area from its surroundings never draws the project's character into it.
    const RemovalRequest r = removalRequest(p);
    submit(p, r.mode, r.prompt, r.strength, false, tr("Removed"));
}

void PromptBar::variation()
{
    if (!m_doc || m_strip->isRunning())
        return;
    PromptPlan p = plan();
    p.region = false;
    const QString words = m_prompt->text().trimmed();
    submit(p, ai::GenMode::ImageToImage, words, m_options->variationStrength(), true,
           words.isEmpty() ? tr("Variation") : words.simplified().left(40));
}

void PromptBar::cancel()
{
    m_strip->stop();
}

} // namespace lumen::ui
