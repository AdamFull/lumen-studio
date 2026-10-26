#include "ui/pages/PromptBar.h"

#include "ai/PromptStyle.h"
#include "doc/Document.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/pages/AiOptions.h"
#include "ui/widgets/LoraPicker.h"
#include "ui/widgets/RunSettingsEditor.h"
#include "ui/dialogs/SelectByDescriptionDialog.h"
#include "ui/SmartSelect.h"
#include "core/Finders.h"
#include "core/ImageOps.h"
#include "ui/pages/AiPanel.h"
#include "ui/pages/VariantStrip.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QtConcurrent/QtConcurrentRun>
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

#include <cmath>

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

    // What Generate does now, or why it cannot, with the way out.
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
    // A button with a border, so it does not read as part of the hint.
    m_selectArea = new QPushButton(Icons::get(QStringLiteral("select-rect")), tr("Select an area"), this);
    m_selectArea->setObjectName(QStringLiteral("promptSelectArea"));
    m_selectArea->setToolTip(tr("Draw a selection around what should change; only that area is then changed (M)"));
    m_selectArea->hide();
    hintRow->addWidget(m_selectArea);
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
    m_remove = new QPushButton(Icons::get(QStringLiteral("eraser")), tr("Remove object"), this);
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
    connect(m_selectArea, &QPushButton::clicked, this, &PromptBar::selectAreaRequested);
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

    // In per cent, from "a little" to "all of it".
    m_strength = new SliderField(tr("How much may change"), 5, 100, m_options->regionStrength() * 100, 0, QStringLiteral(" %"), m_popup);
    m_form->addRow(m_strength);
    m_variants = new QSpinBox(m_popup);
    m_variants->setAccessibleName(tr("Results per run"));
    m_variants->setRange(1, AiOptions::kMaxVariants);
    m_variants->setToolTip(tr("How many results each run makes to choose from."));
    m_form->addRow(tr("Results per run"), m_variants);
    m_negative = new AutoTextEdit(m_popup, 1, 3);
    m_negative->setAccessibleName(tr("Negative prompt"));
    m_negative->setPlaceholderText(tr("Things that should not be in the result"));
    m_form->addRow(tr("Leave out"), m_negative);
    // Chosen here rather than typed into the description (plan M18 §18.3).
    m_loras = new LoraPicker(m_studio, m_options, m_popup);
    m_form->addRow(tr("LoRAs"), m_loras);
    connect(m_loras, &LoraPicker::listed, this, [this] {
        if (m_popup->isVisible())
            m_popup->adjustSize();
    });
    connect(m_options, &AiOptions::changed, this, [this] {
        if (m_popup->isVisible())
            QMetaObject::invokeMethod(m_popup, [this] { m_popup->adjustSize(); }, Qt::QueuedConnection);
    });
    m_references = new QPushButton(m_popup);
    m_references->setToolTip(tr("Pictures the engine follows for a character, a style or an object (in the AI tab)"));
    m_form->addRow(tr("References"), m_references);
    m_pose = new QCheckBox(tr("Follow the document's control pose"), m_popup);
    m_form->addRow(m_pose);
    m_engine = new ProviderCombo(m_studio->providers(), ai::ProviderKind::Image, m_popup);
    m_engine->setObjectName(QStringLiteral("promptEngine"));
    m_form->addRow(tr("Image engine"), m_engine);
    // What changes a result in ways only experienced users ask for.
    m_advanced = new QToolButton(m_popup);
    m_advanced->setObjectName(QStringLiteral("promptAdvanced"));
    m_advanced->setText(tr("Advanced"));
    m_advanced->setCheckable(true);
    m_advanced->setAutoRaise(true);
    m_advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_advanced->setArrowType(Qt::RightArrow);
    m_form->addRow(m_advanced);
    // The settings the engine's model takes (plan M18 §18.4), shared with the AI tab.
    m_settings = new RunSettingsEditor(m_options, m_popup);
    m_form->addRow(m_settings);
    m_literal = new QCheckBox(tr("Use my words as they are"), m_popup);
    m_literal->setObjectName(QStringLiteral("promptLiteralWords"));
    m_literal->setToolTip(literalWordsHelp());
    m_form->addRow(m_literal);
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
    m_allOptions = new QPushButton(Icons::get(QStringLiteral("adjust")), tr("All AI options…"), m_popup);
    m_allOptions->setToolTip(tr("The AI tab: every option, and the actions that are not a description (upscale, expand, "
                                "remove background…)"));
    m_form->addRow(m_allOptions);

    connect(m_strength, &SliderField::valueChanged, this, [this](double percent) {
        if (plan().action == PromptAction::CannotEditImage)
            m_options->setVariationStrength(percent / 100.0);
        else
            m_options->setRegionStrength(percent / 100.0);
    });
    connect(m_seed, &QSpinBox::valueChanged, this, [this](int seed) {
        if (!m_randomSeed->isChecked())
            m_options->setSeed(seed);
    });
    connect(m_randomSeed, &QCheckBox::toggled, this, [this](bool random) { m_options->setSeed(random ? -1 : m_seed->value()); });
    connect(m_variants, &QSpinBox::valueChanged, m_options, &AiOptions::setVariants);
    connect(m_negative, &QPlainTextEdit::textChanged, this, [this] { m_options->setNegative(m_negative->text()); });
    connect(m_pose, &QCheckBox::toggled, m_options, &AiOptions::setUsePose);
    connect(m_literal, &QCheckBox::toggled, m_options, &AiOptions::setLiteralWords);
    // Only a choice of the user's picks the engine (ProviderCombo::picked), for the use the list
    // shows; choosing the one Settings → AI names leaves it to follow that one.
    connect(m_engine, &ProviderCombo::picked, this, [this] {
        m_options->setProviderId(m_engine->use() == ProviderCombo::Use::Changes, m_engine->pinnedProviderId());
    });
    connect(m_advanced, &QToolButton::toggled, this, [this](bool on) {
        m_advanced->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        updateOptions();
        m_popup->adjustSize();
    });
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

bool PromptBar::changesPicture() const
{
    // A selection changes an area, also of a blank canvas (promptPlan).
    return m_doc && ((m_doc->hasSelection() && !m_doc->selectionBounds().isEmpty()) || !blank());
}

ai::ImageProvider *PromptBar::engine() const
{
    return m_options->provider(m_studio->providers(), changesPicture());
}

bool PromptBar::engineChosenInEditor() const
{
    const ai::ImageProvider *p = engine();
    return p && p->instanceId() == m_options->providerId(changesPicture());
}

PromptPlan PromptBar::plan() const
{
    if (!m_doc)
        return {};
    ai::ImageProvider *provider = engine();
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

bool PromptBar::offersVariation(const PromptPlan &p)
{
    return p.action == PromptAction::CannotEditImage && p.variation;
}

void PromptBar::updateUi()
{
    m_update.stop();
    m_plan = plan();
    const ai::ImageProvider *now = engine();
    const QString engineId = now ? now->instanceId() : QString();
    if (engineId != m_engineId) {
        m_engineId = engineId;
        emit engineChanged();
    }
    const PromptPlan &p = m_plan;
    const bool running = m_strip->isRunning();
    QString hint;
    QString tip;
    QString placeholder = tr("Describe the change, e.g. “colorize it” or “replace the cup with a candle”");
    switch (p.action) {
    case PromptAction::NoEngine:
        hint = tr("AI editing needs an image engine. Set one up to change this picture with words.");
        placeholder = tr("Set up an image engine first (Set up AI…)");
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
        hint = p.variation ? tr("This engine cannot change the whole picture from words: make a variation, or select an area to change.")
                           : tr("This engine cannot change the whole picture from words: select an area to change.");
        tip = tr("To change the whole picture from a description, use an engine with an edit model, such as FLUX.2 [klein] "
                 "(Help → Set up AI…).");
        placeholder = p.variation ? tr("Optional: words that guide the variation, e.g. “evening light”")
                                  : tr("Select an area first, then describe what should be there");
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
        tip.clear();
        m_progress->setRange(0, progress >= 0 ? 100 : 0);
        m_progress->setValue(progress >= 0 ? int(progress * 100) : 0);
    }
    m_hint->setFullText(hint);
    if (!tip.isEmpty())
        m_hint->setToolTip(hint + u'\n' + tip);
    m_progress->setVisible(running);
    m_prompt->setPlaceholderText(placeholder);
    m_selectArea->setVisible(!running && p.action == PromptAction::CannotEditImage);

    // The main button says what it does: Generate, Make a variation, Set up AI…, or Cancel.
    QString text = tr("Generate");
    QString icon = QStringLiteral("sparkles");
    QString tooltip = tr("Run it (Enter)");
    if (running) {
        text = tr("Cancel");
        icon = QStringLiteral("stop");
        tooltip = tr("Stop the run");
    } else if (p.action == PromptAction::NoEngine) {
        text = tr("Set up AI…");
        tooltip = tr("Connect an image engine on this computer or online");
    } else if (offersVariation(p)) {
        text = tr("Make a variation");
        icon = QStringLiteral("variation");
        tooltip = tr("Redraws the whole image, keeping its layout, guided by your words if any. How much it may change is in the ⋯ "
                     "menu (Enter).");
    }
    m_generate->setText(text);
    m_generate->setIcon(Icons::get(icon));
    m_generate->setToolTip(tooltip);
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
    // Words are needed except to cancel, to set up an engine and for a variation.
    const bool words = !m_prompt->text().trimmed().isEmpty();
    m_generate->setEnabled(m_strip->isRunning() || m_plan.action == PromptAction::NoEngine || offersVariation(m_plan)
                           || (m_plan.canRun() && words));
}

void PromptBar::updateOptions()
{
    const PromptPlan p = plan();
    ai::ImageProvider *provider = engine();
    const ai::ImageCapabilities caps = provider ? provider->capabilities() : ai::ImageCapabilities();
    const bool variation = p.action == PromptAction::CannotEditImage;
    // The engine list shows the engine this run goes to: the one for new images on a blank canvas,
    // else the one for changes, unless one is chosen for that use; named as in Settings → AI.
    const bool changes = changesPicture();
    m_engine->setUse(changes ? ProviderCombo::Use::Changes : ProviderCombo::Use::NewImages);
    if (auto *label = qobject_cast<QLabel *>(m_form->labelForField(m_engine)))
        label->setText(changes ? tr("Engine for changes") : tr("Image engine"));
    m_engine->setAccessibleName(changes ? tr("Engine for changes") : tr("Image engine"));
    {
        const QSignalBlocker b1(m_strength), b3(m_seed), b4(m_randomSeed), b5(m_variants), b6(m_pose), b7(m_engine), b8(m_literal);
        m_strength->setValue(std::round((variation ? m_options->variationStrength() : m_options->regionStrength()) * 100));
        m_strength->setToolTip(variation ? tr("How much a variation may change the image: a little keeps more of it.")
                                         : tr("How much the selected area may change: a little keeps more of what is there."));
        m_randomSeed->setChecked(m_options->seed() < 0);
        m_seed->setEnabled(m_options->seed() >= 0);
        if (m_options->seed() >= 0)
            m_seed->setValue(int(std::min<qint64>(m_options->seed(), m_seed->maximum())));
        m_variants->setValue(m_options->variants());
        m_pose->setChecked(m_options->usePose());
        m_literal->setChecked(m_options->literalWords());
        m_engine->setPinnedProviderId(m_options->providerId(changes));
    }
    // Not blocked: the field fits its height to the text (setting the same words back changes nothing).
    if (m_negative->text() != m_options->negative())
        m_negative->setPlainText(m_options->negative());
    const int refs = m_ai->references()->count();
    m_references->setText(refs == 0 ? tr("Add in the AI tab…") : tr("%n picture(s) in the AI tab…", nullptr, refs));
    // Only what the request uses and the engine takes.
    m_form->setRowVisible(m_strength, p.strength);
    const bool advanced = m_advanced->isChecked();
    m_settings->setEngine(provider);
    m_form->setRowVisible(m_advanced, !provider || caps.seed || m_settings->hasSettings());
    m_form->setRowVisible(m_settings, advanced && m_settings->hasSettings());
    m_form->setRowVisible(m_seedRow, advanced && (!provider || caps.seed));
    m_form->setRowVisible(m_literal, advanced);
    const bool negative = provider && caps.negativePrompt && ai::promptStyleFor(provider).supportsNegative;
    m_form->setRowVisible(m_negative, negative);
    m_loras->setEngine(provider);
    m_loras->setWords(m_prompt->text());
    m_form->setRowVisible(m_loras, provider && caps.loras);
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
    // The engine the plan was made for.
    if (const ai::ImageProvider *provider = engine())
        run.providerId = provider->instanceId();
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
    const QString words = m_prompt->text().trimmed();
    // "select: …" finds the area instead of changing the picture, and needs no engine.
    if (const QString what = selectionWords(words); !what.isEmpty()) {
        selectByWords(what);
        return;
    }
    const PromptPlan p = plan();
    if (p.action == PromptAction::NoEngine) {
        emit setUpAiRequested();
        return;
    }
    // The engine cannot change the whole picture from words: what the main button offers instead.
    if (offersVariation(p)) {
        variation();
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

void PromptBar::selectByWords(const QString &description)
{
    const img::FinderRequest request = img::parseFinderRequest(description);
    if (request.kind == img::FinderRequest::Kind::None) {
        // An object by name: the detection models find it, once they are downloaded, or an image
        // engine that can.
        SmartSelect *finder = m_studio->smartSelect();
        if (!finder->canFindObjects()) {
            m_studio->notify(tr("Selecting objects by name needs the detection models (about %1 MB, once), or an image engine that "
                                "finds objects: ComfyUI, fal.ai or Google Gemini. Select → By description… offers the download.")
                                 .arg(QLocale().toString(finder->objectsMissingSize() / 1e6, 'f', 0)),
                             QStringLiteral("warning"));
            return;
        }
        QPointer<doc::Document> doc(m_doc);
        QPointer<PromptBar> self(this);
        finder->findObjects(m_doc->flattened(), description)
            .then(this, [self, doc, description](const SmartSelect::FoundArea &area) {
                if (!self || !doc)
                    return;
                if (area.mask.isNull()) {
                    self->m_studio->notify(area.error, QStringLiteral("warning"));
                    return;
                }
                doc->applySelection(area.mask, tr("Select: %1").arg(description.left(40)));
                if (doc == self->m_doc)
                    self->m_prompt->clear();
                self->m_studio->notify(area.foundBy.isEmpty()
                                           ? tr("Selected %n object(s). Now say what should change there, or press Remove object.",
                                                nullptr, area.objects)
                                           : tr("Selected what %1 found. Now say what should change there, or press Remove object.")
                                                 .arg(area.foundBy),
                                       QStringLiteral("success"));
            });
        return;
    }
    QPointer<doc::Document> doc(m_doc);
    QPointer<PromptBar> self(this);
    QtConcurrent::run([picture = m_doc->flattened(), request] { return img::runFinder(picture, request); })
        .then(this, [self, doc, request, description](const QImage &mask) {
            if (!self || !doc)
                return;
            if (mask.isNull() || img::maskIsEmpty(mask)) {
                self->m_studio->notify(tr("Nothing like \"%1\" was found. Select → By description… has settings to find fainter "
                                          "or larger ones.")
                                           .arg(description),
                                       QStringLiteral("warning"));
                return;
            }
            doc->applySelection(mask, tr("Select: %1").arg(description.left(40)));
            // The words asked to select; what comes next is the change.
            if (doc == self->m_doc)
                self->m_prompt->clear();
            self->m_studio->notify(tr("Selected: %1 Now say what should change there, or press Remove object.").arg(finderDescription(request)),
                                   QStringLiteral("success"));
        });
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
