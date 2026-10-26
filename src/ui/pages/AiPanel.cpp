#include "ui/pages/AiPanel.h"

#include "ai/PromptRules.h"
#include "ai/PromptStyle.h"
#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "pose/PoseRenderer.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "ui/pages/AiOptions.h"
#include "ui/widgets/RunSettingsEditor.h"
#include "ui/pages/PromptPlan.h"
#include "ui/widgets/CharacterChip.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/JobCard.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lumen::ui {

std::optional<ai::Utility> AiPanel::utilityOf(Mode mode)
{
    switch (mode) {
    case Mode::Upscale: return ai::Utility::Upscale;
    case Mode::RemoveBackground: return ai::Utility::RemoveBackground;
    case Mode::DetectPose: return ai::Utility::DetectPose;
    default: return std::nullopt;
    }
}

AiPanel::AiPanel(Studio *studio, AiOptions *options, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_options(options)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);

    // Every action here changes the picture: the engine for changes, as Settings → AI calls it.
    v->addWidget(sectionLabel(tr("Engine for changes")));
    m_provider = new ProviderCombo(studio->providers(), ai::ProviderKind::Image, content);
    m_provider->setUse(ProviderCombo::Use::Changes);
    m_provider->setAccessibleName(tr("Engine for changes"));
    v->addWidget(m_provider);
    m_capsWarning = new QLabel(content);
    m_capsWarning->setObjectName(QStringLiteral("aiCapsWarning"));
    m_capsWarning->setProperty("role", QStringLiteral("muted"));
    m_capsWarning->setWordWrap(true);
    v->addWidget(m_capsWarning);

    // The project's character: the Characters plugin's (it makes them), so only while it is on.
    m_characterRow = new QWidget(content);
    auto *characterRow = new QHBoxLayout(m_characterRow);
    characterRow->setContentsMargins(0, 0, 0, 0);
    characterRow->addWidget(sectionLabel(tr("Character"), m_characterRow));
    characterRow->addStretch();
    m_character = new CharacterChip(studio, QStringLiteral("editor"), m_characterRow);
    characterRow->addWidget(m_character);
    m_charactersOn = studio->hasPage(QStringLiteral("create"));
    m_characterRow->setVisible(m_charactersOn);
    v->addWidget(m_characterRow);

    v->addWidget(sectionLabel(tr("Action")));
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    m_modes = new QButtonGroup(this);
    // Changing an area or the whole image from a description is the prompt bar's; these are the rest.
    const QList<std::tuple<Mode, QString, QString, QString>> modes = {
        {Mode::Variation, QStringLiteral("variation"), tr("Variation"), tr("Re-imagine the image (or the selection) while keeping its structure.")},
        {Mode::Expand, QStringLiteral("outpaint"), tr("Expand"), tr("Make the canvas larger and let the image engine paint the new area.")},
        {Mode::Upscale, QStringLiteral("upscale"), tr("Upscale"), tr("A larger, sharper copy of the image, as a new document.")},
        {Mode::RemoveBackground, QStringLiteral("scissors"), tr("Remove background"), tr("Cut out the subject onto a transparent layer.")},
        {Mode::DetectPose, QStringLiteral("person"), tr("Detect pose"), tr("Find the pose of the people in the image.")},
    };
    // At 1280 × 720 a button is about 95 px wide and its padding takes 30: a longer name takes two
    // lines or a smaller font (the German "Hochskalieren" was cut at 72). One font size for all of
    // them, the smallest any name needs, so the row does not look uneven.
    double pointSize = 8.5;
    for (const auto &[mode, icon, label, tip] : modes) {
        double fitted = 8.5;
        fitLabel(label, 64, &fitted);
        pointSize = std::min(pointSize, fitted);
    }
    int i = 0;
    for (const auto &[mode, icon, label, tip] : modes) {
        auto *b = new QToolButton(content);
        b->setIcon(Icons::get(icon));
        double fitted = pointSize;
        const QString text = fitLabel(label, 64, &fitted);
        b->setText(text);
        b->setAccessibleName(label);
        b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(20, 20));
        b->setCheckable(true);
        b->setMinimumSize(78, text.contains(u'\n') ? 66 : 54);
        b->setProperty("role", QStringLiteral("chip"));
        b->setStyleSheet(QStringLiteral("QToolButton { border-radius: 9px; font-size: %1pt; }").arg(pointSize));
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_modes->addButton(b, static_cast<int>(mode));
        // Poses are the Poses plugin's: without it nothing shows or edits a detected pose.
        if (mode == Mode::DetectPose) {
            m_detectPose = b;
            if (!studio->hasPage(QStringLiteral("poses"))) {
                b->hide();
                continue;
            }
        }
        grid->addWidget(b, i / 3, i % 3);
        ++i;
    }
    for (int c = 0; c < 3; ++c)
        grid->setColumnStretch(c, 1);
    // One height for every action, also when one name takes two lines (the rows were uneven).
    int buttonHeight = 0;
    for (QAbstractButton *b : m_modes->buttons())
        buttonHeight = std::max(buttonHeight, b->minimumHeight());
    for (QAbstractButton *b : m_modes->buttons())
        b->setMinimumHeight(buttonHeight);
    v->addLayout(grid);
    m_modeHint = mutedLabel(QString(), content);
    v->addWidget(m_modeHint);

    // In per cent, as in the prompt bar's ⋯ menu.
    m_strength = new SliderField(tr("How much may change"), 5, 100, std::round(options->variationStrength() * 100), 0,
                                 QStringLiteral(" %"), content);
    m_strength->setToolTip(tr("How much a variation may change the image: a little keeps more of it."));
    v->addWidget(m_strength);

    m_expandBox = new QWidget(content);
    auto *eg = new QGridLayout(m_expandBox);
    eg->setContentsMargins(0, 0, 0, 0);
    m_expandLeft = new QCheckBox(tr("Left"), m_expandBox);
    m_expandRight = new QCheckBox(tr("Right"), m_expandBox);
    m_expandTop = new QCheckBox(tr("Top"), m_expandBox);
    m_expandBottom = new QCheckBox(tr("Bottom"), m_expandBox);
    m_expandLeft->setChecked(true);
    m_expandRight->setChecked(true);
    m_expandAmount = new QSpinBox(m_expandBox);
    m_expandAmount->setRange(32, 2048);
    m_expandAmount->setSingleStep(64);
    m_expandAmount->setValue(256);
    m_expandAmount->setSuffix(QStringLiteral(" px"));
    m_expandAmount->setAccessibleName(tr("Expand by"));
    eg->addWidget(m_expandTop, 0, 1);
    eg->addWidget(m_expandLeft, 1, 0);
    eg->addWidget(m_expandAmount, 1, 1);
    eg->addWidget(m_expandRight, 1, 2);
    eg->addWidget(m_expandBottom, 2, 1);
    v->addWidget(m_expandBox);

    m_upscaleBox = new QWidget(content);
    auto *uh = new QHBoxLayout(m_upscaleBox);
    uh->setContentsMargins(0, 0, 0, 0);
    uh->addWidget(new QLabel(tr("Factor"), m_upscaleBox));
    m_upscaleFactor = new QComboBox(m_upscaleBox);
    m_upscaleFactor->setAccessibleName(tr("Factor"));
    m_upscaleFactor->addItem(QStringLiteral("2×"), 2.0);
    m_upscaleFactor->addItem(QStringLiteral("3×"), 3.0);
    m_upscaleFactor->addItem(QStringLiteral("4×"), 4.0);
    uh->addWidget(m_upscaleFactor, 1);
    v->addWidget(m_upscaleBox);

    m_run = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Make a variation"), content);
    m_run->setObjectName(QStringLiteral("aiRun"));
    Theme::setRole(m_run, QStringLiteral("primary"));
    m_run->setMinimumHeight(36);
    v->addWidget(m_run);
    m_jobs = new QVBoxLayout;
    m_jobs->setSpacing(6);
    v->addLayout(m_jobs);

    // What the prompt bar uses too.
    v->addSpacing(6);
    v->addWidget(sectionLabel(tr("Options")));
    v->addWidget(mutedLabel(tr("These also apply to the prompt bar under the canvas."), content));
    m_refsBox = new QWidget(content);
    auto *rv = new QVBoxLayout(m_refsBox);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(new QLabel(tr("Reference images"), m_refsBox));
    m_refs = new ReferenceStrip(studio, m_refsBox);
    m_refs->setCompact(true);
    rv->addWidget(m_refs);
    v->addWidget(m_refsBox);
    m_usePose = new QCheckBox(tr("Follow the document's control pose"), content);
    m_usePose->setChecked(options->usePose());
    v->addWidget(m_usePose);

    auto *form = new QGridLayout;
    form->setHorizontalSpacing(8);
    form->addWidget(new QLabel(tr("Results per run"), content), 0, 0);
    m_variants = new QSpinBox(content);
    m_variants->setAccessibleName(tr("Results per run"));
    m_variants->setRange(1, AiOptions::kMaxVariants);
    m_variants->setToolTip(tr("How many results each run makes to choose from."));
    form->addWidget(m_variants, 0, 1);
    // The settings the engine's model takes (plan M18 §18.4), shared with the prompt bar's ⋯.
    m_settings = new RunSettingsEditor(options, content);
    form->addWidget(m_settings, 1, 0, 1, 2);
    form->addWidget(new QLabel(tr("Seed"), content), 2, 0);
    auto *seedRow = new QHBoxLayout;
    m_seed = new QSpinBox(content);
    m_seed->setAccessibleName(tr("Seed"));
    m_seed->setRange(0, 2147483647);
    m_seed->setToolTip(tr("The same seed, words and settings give the same result again."));
    m_randomSeed = new QCheckBox(tr("Random"), content);
    seedRow->addWidget(m_seed, 1);
    seedRow->addWidget(m_randomSeed);
    form->addLayout(seedRow, 2, 1);
    form->setColumnStretch(1, 1);
    v->addLayout(form);
    m_negativeBox = new QWidget(content);
    auto *nv = new QVBoxLayout(m_negativeBox);
    nv->setContentsMargins(0, 0, 0, 0);
    nv->addWidget(new QLabel(tr("Leave out"), m_negativeBox));
    m_negative = new AutoTextEdit(m_negativeBox, 2, 4);
    m_negative->setAccessibleName(tr("Negative prompt"));
    m_negative->setPlaceholderText(tr("Things that should not be in the result"));
    nv->addWidget(m_negative);
    v->addWidget(m_negativeBox);
    m_literal = new QCheckBox(tr("Use my words as they are"), content);
    m_literal->setObjectName(QStringLiteral("aiLiteralWords"));
    m_literal->setToolTip(literalWordsHelp());
    v->addWidget(m_literal);
    m_areaStrength = new SliderField(tr("Area may change"), 5, 100, std::round(options->regionStrength() * 100), 0,
                                     QStringLiteral(" %"), content);
    m_areaStrength->setToolTip(tr("How much a selected area may change when the prompt bar paints it anew: a little keeps more of "
                                  "what is there."));
    v->addWidget(m_areaStrength);
    m_padding = new SliderField(tr("Context"), 0, 512, options->padding(), 0, QStringLiteral(" px"), content);
    m_padding->setToolTip(tr("Surrounding area sent to the model along with the selection."));
    v->addWidget(m_padding);
    m_feather = new SliderField(tr("Blend edge"), 0, 128, options->feather(), 0, QStringLiteral(" px"), content);
    m_feather->setToolTip(tr("How wide the edge is over which a result blends into the image around the selection."));
    v->addWidget(m_feather);
    // The sliders start at one place, under one another (each label had its own width).
    SliderField::alignLabels({m_strength, m_areaStrength, m_padding, m_feather});
    m_colorMatch = new QCheckBox(tr("Match colours to the surroundings"), content);
    v->addWidget(m_colorMatch);
    v->addStretch();

    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { setMode(static_cast<Mode>(id)); });
    connect(m_run, &QPushButton::clicked, this, [this] { run(m_mode); });
    // Only a choice of the user's picks the engine (ProviderCombo::picked); one nobody chose follows
    // the engine for changes of Settings → AI.
    connect(m_provider, &ProviderCombo::picked, this, [this] {
        m_options->setProviderId(true, m_provider->pinnedProviderId());
        updateUi();
    });
    connect(studio->providers(), &ai::ProviderRegistry::providersChanged, this, &AiPanel::syncFromOptions);
    connect(studio->providers(), &ai::ProviderRegistry::rolesChanged, this, &AiPanel::syncFromOptions);
    connect(m_strength, &SliderField::valueChanged, this, [this](double percent) { m_options->setVariationStrength(percent / 100.0); });
    connect(m_areaStrength, &SliderField::valueChanged, this, [this](double percent) { m_options->setRegionStrength(percent / 100.0); });
    connect(m_usePose, &QCheckBox::toggled, m_options, &AiOptions::setUsePose);
    connect(m_literal, &QCheckBox::toggled, m_options, &AiOptions::setLiteralWords);
    connect(m_variants, &QSpinBox::valueChanged, m_options, &AiOptions::setVariants);
    connect(m_seed, &QSpinBox::valueChanged, this, [this](int seed) {
        if (!m_randomSeed->isChecked())
            m_options->setSeed(seed);
    });
    connect(m_randomSeed, &QCheckBox::toggled, this, [this](bool random) { m_options->setSeed(random ? -1 : m_seed->value()); });
    connect(m_negative, &QPlainTextEdit::textChanged, this, [this] { m_options->setNegative(m_negative->text()); });
    connect(m_padding, &SliderField::valueChanged, this, [this](double px) { m_options->setPadding(int(px)); });
    connect(m_feather, &SliderField::valueChanged, this, [this](double px) { m_options->setFeather(int(px)); });
    connect(m_colorMatch, &QCheckBox::toggled, m_options, &AiOptions::setColorMatch);
    connect(m_options, &AiOptions::changed, this, &AiPanel::syncFromOptions);
    // "Use as reference" (Browse project images) lands here.
    connect(studio, &Studio::referenceRequested, m_refs, &ReferenceStrip::addAsset);
    syncFromOptions();
    setMode(Mode::Variation);
}

void AiPanel::syncFromOptions()
{
    {
        // The engine runs go to: the chosen one, else the engine for changes.
        const QSignalBlocker b(m_provider);
        m_provider->setPinnedProviderId(m_options->providerId(true));
    }
    const QSignalBlocker b0(m_literal), b1(m_strength), b2(m_usePose), b3(m_variants), b5(m_seed), b6(m_randomSeed), b7(m_padding),
        b8(m_feather), b9(m_colorMatch), b10(m_areaStrength);
    m_strength->setValue(std::round(m_options->variationStrength() * 100));
    m_areaStrength->setValue(std::round(m_options->regionStrength() * 100));
    m_usePose->setChecked(m_options->usePose());
    m_literal->setChecked(m_options->literalWords());
    m_variants->setValue(m_options->variants());
    m_randomSeed->setChecked(m_options->seed() < 0);
    m_seed->setEnabled(m_options->seed() >= 0);
    if (m_options->seed() >= 0)
        m_seed->setValue(int(std::min<qint64>(m_options->seed(), m_seed->maximum())));
    m_padding->setValue(m_options->padding());
    m_feather->setValue(m_options->feather());
    m_colorMatch->setChecked(m_options->colorMatch());
    // Not blocked: the field fits its height to the text (setting the same words back changes nothing).
    if (m_negative->text() != m_options->negative())
        m_negative->setPlainText(m_options->negative());
    updateUi();
}

void AiPanel::setDocument(doc::Document *doc)
{
    if (m_doc)
        m_doc->disconnect(this);
    m_doc = doc;
    if (doc) {
        connect(doc, &doc::Document::selectionChanged, this, &AiPanel::updateUi);
        connect(doc, &doc::Document::controlPoseChanged, this, &AiPanel::updateUi);
    }
    updateUi();
}

void AiPanel::setMode(Mode mode)
{
    // Fill selection and instruction edits are the prompt bar's.
    if (mode == Mode::Inpaint || mode == Mode::Edit)
        mode = Mode::Variation;
    m_mode = mode;
    if (QAbstractButton *b = m_modes->button(static_cast<int>(mode)))
        b->setChecked(true);
    updateUi();
}

void AiPanel::withCharacter(ai::ImageRequest request, ai::CharacterUse use, bool skip, ai::ImageProvider *provider,
                            std::function<void(ai::ImageRequest, QJsonObject)> then)
{
    // Characters are the Characters plugin's: while it is off, none is drawn in.
    const QString id = skip || !m_charactersOn ? QString() : m_character->characterId();
    if (id.isEmpty() || !provider)
        return then(std::move(request), {});
    const ai::ImageCapabilities caps = provider->capabilities();
    const ai::PromptStyle style = ai::promptStyleFor(provider);
    const QString family = checkpointFamily(m_studio, provider);
    const QString providerName = provider->displayName();
    QPointer<AiPanel> self(this);
    m_studio->characterContext(id).then(this, [self, request, use, caps, style, family, providerName, then](const ai::CharacterContext &c) mutable {
        if (!self)
            return;
        if (c.isEmpty())
            return then(std::move(request), {});
        const ai::CharacterApplied applied = ai::applyCharacter(request, c, caps, use, style, family);
        const QString note = applied.fallbackNote(c, providerName);
        if (!note.isEmpty() && !self->m_notes.contains(note)) {
            self->m_notes.insert(note);
            self->m_studio->notify(note);
        }
        then(std::move(request), applied.any() ? applied.toJson(c) : QJsonObject());
    }).onFailed(this, [self, request, then](const std::exception &e) {
        // The character's pictures could not be read: the run goes on without it.
        if (!self)
            return;
        self->m_studio->notify(describeException(e), QStringLiteral("warning"));
        then(request, {});
    });
}

bool AiPanel::takesNegative(const ai::ImageProvider *provider) const
{
    // Only models that take one (FLUX and most online services do not).
    return provider && provider->capabilities().negativePrompt && ai::promptStyleFor(provider).supportsNegative;
}

void AiPanel::updateUi()
{
    const bool prompt = m_mode == Mode::Variation || m_mode == Mode::Expand;
    m_strength->setVisible(m_mode == Mode::Variation);
    m_expandBox->setVisible(m_mode == Mode::Expand);
    m_upscaleBox->setVisible(m_mode == Mode::Upscale);
    QStringList hints;
    hints << m_modes->button(static_cast<int>(m_mode))->toolTip();
    if (prompt)
        hints << tr("It uses the words in the prompt bar under the canvas, if any.");
    m_modeHint->setText(hints.join(u' '));

    // Every action here changes the picture: the engine for changes unless one is chosen.
    ai::ImageProvider *provider = m_options->provider(m_studio->providers(), true);
    const ai::ImageCapabilities caps = provider ? provider->capabilities() : ai::ImageCapabilities();
    QString warning;
    bool runnable = true;
    if (provider) {
        if (const std::optional<ai::Utility> utility = utilityOf(m_mode); utility && !provider->supportsUtility(*utility)) {
            // Another engine may do it; else what would help (Set up AI would only give another
            // engine that cannot either).
            ai::ImageProvider *other = m_studio->generation()->providerForUtility(*utility);
            warning = other ? tr("Will use %1 for this action.").arg(imageEngineName(other)) : m_studio->generation()->utilityUnavailable(*utility);
            runnable = other != nullptr;
        } else if (m_mode == Mode::Variation && !caps.imageToImage && !caps.inpaint && !caps.edit) {
            warning = tr("%1 only makes new images; it cannot make a variation of this one.").arg(imageEngineName(provider));
        }
        m_refs->setEnabled(caps.maxReferences > 0);
        m_refs->setMaximum(std::max(1, caps.maxReferences));
    }
    m_capsWarning->setText(warning);
    m_capsWarning->setVisible(!warning.isEmpty());
    m_negativeBox->setVisible(takesNegative(provider));
    m_usePose->setVisible(caps.poseControl && m_doc && m_doc->controlPose());
    m_settings->setEngine(provider);
    m_settings->setVisible(m_settings->hasSettings());
    m_run->setEnabled(m_doc != nullptr && runnable);
    const QString runText = m_mode == Mode::Upscale ? tr("Upscale")
                          : m_mode == Mode::RemoveBackground ? tr("Remove background")
                          : m_mode == Mode::DetectPose ? tr("Detect pose")
                          : m_mode == Mode::Expand ? tr("Expand canvas")
                                                   : tr("Make a variation");
    m_run->setText(runText);
}

ai::ImageRequest AiPanel::baseRequest(const ai::ImageProvider *provider, const QString &prompt, ai::GenMode mode, bool area) const
{
    ai::ImageRequest r;
    const ai::PromptStyle style = ai::promptStyleFor(provider);
    const ai::FittedPrompt fitted = ai::fitPrompt(style, mode, area, prompt, takesNegative(provider) ? m_options->negative() : QString(),
                                                  m_options->literalWords());
    r.prompt = fitted.prompt;
    r.negativePrompt = fitted.negative;
    r.batchCount = m_options->variants();
    r.seed = m_options->seed();
    r.references = m_refs->references();
    if (provider) {
        const ai::ImageCapabilities caps = provider->capabilities();
        // What was set for this model (steps, how closely to follow the words, sampler…).
        const ai::RunSettingsSpec spec = ai::runSettingsFor(provider);
        ai::applyRunSettings(r, m_options->runSettings(spec.key), spec);
        r.references = r.references.mid(0, caps.maxReferences);
        // The LoRAs chosen for this engine that are on (the picker turns off those that do not fit
        // its model), and the words that call them up.
        if (caps.loras) {
            r.loras << m_options->loraRefs(provider->instanceId());
            r.prompt = m_options->withTriggerWords(provider->instanceId(), r.prompt);
        }
        if (m_options->usePose() && m_doc && m_doc->controlPose() && caps.poseControl) {
            ai::ControlInput c;
            c.type = ai::ControlType::OpenPose;
            c.image = pose::renderOpenPose(*m_doc->controlPose(), m_doc->size());
            r.controls << c;
        }
    }
    return r;
}

void AiPanel::submitWithComposite(std::function<void(QImage)> fn)
{
    if (m_doc)
        submitWithComposite(m_doc, std::move(fn));
}

void AiPanel::submitWithComposite(doc::Document *doc, std::function<void(QImage)> fn)
{
    const doc::DocumentSnapshot snap = doc->snapshot();
    const QSize size = doc->size();
    async::run([snap, size] { return doc::flatten(snap, size); }).then(this, std::move(fn));
}

void AiPanel::addResultLayers(Studio *studio, doc::Document *doc, const QList<QImage> &images, const QString &name, const QJsonObject &meta)
{
    if (images.isEmpty())
        return;
    doc->undoStack()->beginMacro(tr("AI result: %1").arg(name));
    QList<QUuid> ids;
    for (int i = 0; i < images.size(); ++i) {
        QImage img = images[i];
        if (img.size() != doc->size())
            img = img::normalized(img.scaled(doc->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        const QString layerName = images.size() > 1 ? QStringLiteral("%1 #%2").arg(name).arg(i + 1) : name;
        ids << doc->addLayer(layerName, img, meta, -1, tr("Add AI result"));
    }
    // Only the first variant stays visible; the others can be toggled in the layer panel.
    for (int i = 1; i < ids.size(); ++i) {
        if (const doc::LayerPtr l = doc->findLayer(ids[i])) {
            doc::LayerProps p = l->props;
            p.visible = false;
            doc->setLayerProps(ids[i], p, QString());
        }
    }
    doc->undoStack()->endMacro();
    if (!ids.isEmpty())
        doc->setActiveLayer(ids.first());
    if (images.size() > 1)
        studio->notify(tr("%1 variants were added as layers - toggle their visibility to compare.").arg(images.size()));
}

void AiPanel::trackJob(Job *job, std::function<void(doc::Document *, const ai::GenerationOutput &)> apply)
{
    auto *card = new JobCard(job, m_studio->jobs(), this);
    m_jobs->insertWidget(0, card);
    connect(card, &JobCard::dismissed, card, &QObject::deleteLater);
    QPointer<doc::Document> doc = m_doc;
    QPointer<JobCard> cardGuard(card);
    connect(job, &Job::finished, this, [this, doc, apply, cardGuard](Job *j) {
        if (j->state() == Job::State::Succeeded && doc)
            apply(doc, ai::GenerationService::outputOf(j));
        else if (j->state() == Job::State::Failed)
            m_studio->notify(j->errorString(), QStringLiteral("error"));
        // Successful cards fade away after a short moment; failures stay for inspection/retry.
        if (j->state() == Job::State::Succeeded && cardGuard)
            QTimer::singleShot(4000, cardGuard, &QObject::deleteLater);
    });
    // Keep at most 6 cards.
    while (m_jobs->count() > 6) {
        QLayoutItem *item = m_jobs->takeAt(m_jobs->count() - 1);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void AiPanel::submitPrompt(const PromptRun &run)
{
    if (!m_doc)
        return;
    // The engine the bar planned with, else the editor's for this kind of run: a new image goes to
    // the engine for new images, a change to the engine for changes (Settings → AI).
    ai::ImageProvider *provider = run.providerId.isEmpty()
                                      ? m_options->provider(m_studio->providers(), run.mode != ai::GenMode::TextToImage)
                                      : m_studio->providers()->imageProvider(run.providerId);
    if (!provider) {
        m_studio->notify(tr("Set up an image engine first: Help → Set up AI…"), QStringLiteral("warning"));
        return;
    }
    QPointer<doc::Document> doc = m_doc;
    if (m_preparing)
        m_preparing(doc);
    // Told once the jobs are out, or that there are none (the document closed meanwhile).
    QPointer<AiPanel> self(this);
    auto giveUp = [self, doc, name = run.name] {
        if (self && self->m_results)
            self->m_results(doc, {}, name, 0);
    };
    const QString providerId = provider->instanceId();
    const ai::ImageCapabilities caps = provider->capabilities();
    const bool region = run.region && doc->hasSelection() && !doc->selectionBounds().isEmpty();
    const QImage selection = region ? doc->selection() : QImage();
    const int padding = m_options->padding();
    const int feather = m_options->feather();
    const bool colorMatch = m_options->colorMatch();
    const int variants = m_options->variants();
    const ai::GenMode mode = run.mode;
    ai::ImageRequest base = baseRequest(provider, run.prompt, mode, region);
    base.mode = mode;
    base.denoise = run.strength;
    // A region gets the character's LoRA (or reference) only; the prompt describes the region.
    const ai::CharacterUse use = mode == ai::GenMode::Edit ? ai::CharacterUse::Edit : region ? ai::CharacterUse::Region : ai::CharacterUse::Full;
    withCharacter(base, use, !run.withCharacter, provider, [=, this](const ai::ImageRequest &req, const QJsonObject &character) {
        if (!doc)
            return giveUp();
        submitWithComposite(doc, [=, this](QImage composite) {
            if (!doc)
                return giveUp();
            ai::GenerationSpec spec;
            spec.providerId = providerId;
            spec.request = req;
            spec.title = run.prompt.isEmpty() ? run.name : run.prompt.left(60);
            spec.assetKind = mode == ai::GenMode::TextToImage ? doc::AssetKind::Generation : doc::AssetKind::Edit;
            spec.meta = QJsonObject{{QStringLiteral("document"), doc->id()}, {QStringLiteral("user_prompt"), run.prompt}};
            if (run.action)
                spec.meta.insert(QStringLiteral("action"), static_cast<int>(*run.action));
            if (!character.isEmpty())
                spec.meta.insert(QStringLiteral("character"), character);
            if (region) {
                spec.region = ai::RegionInput{composite, selection, padding, 6, feather, colorMatch};
            } else {
                // The whole image, or a new one, at the model's working resolution; the results are
                // scaled back to the canvas.
                if (mode == ai::GenMode::TextToImage) {
                    spec.request.size = workingSize(composite.size(), caps);
                } else {
                    spec.request.mode = mode == ai::GenMode::Edit && caps.edit ? ai::GenMode::Edit : ai::GenMode::ImageToImage;
                    spec.request.initImage = ai::wholeImageInput(composite, caps);
                    spec.request.size = spec.request.initImage.size();
                    // The results come back at the picture's size with its fine detail where the engine
                    // kept its shapes (a colorized photo stays as sharp as it was).
                    spec.detailSource = composite;
                }
            }
            // Engines that make one image per request get one job per variant, each with its own seed.
            spec.request.batchCount = variants;
            const QList<Job *> jobs = m_studio->generation()->submitVariants(spec);
            if (m_results) {
                m_results(doc, jobs, run.name, variants);
                return;
            }
            for (Job *job : std::as_const(jobs)) {
                trackJob(job, [this, name = run.name](doc::Document *d, const ai::GenerationOutput &out) {
                    QJsonObject meta{{QStringLiteral("generated"), true}};
                    if (!out.assets.isEmpty())
                        meta.insert(QStringLiteral("asset"), out.assets.first().id);
                    addResultLayers(d, out.images, name, meta);
                });
            }
        });
    });
}

void AiPanel::upscale(double factor)
{
    const int index = m_upscaleFactor->findData(factor);
    if (index >= 0)
        m_upscaleFactor->setCurrentIndex(index);
    run(Mode::Upscale);
}

void AiPanel::run(Mode mode)
{
    if (!m_doc)
        return;
    ai::ImageProvider *provider = m_options->provider(m_studio->providers(), true);
    if (!provider) {
        m_studio->notify(tr("Set up an image engine first: Help → Set up AI…"), QStringLiteral("warning"));
        return;
    }
    // The AI menu's Upscale and Remove background are disabled then; from elsewhere it says why.
    if (const std::optional<ai::Utility> utility = utilityOf(mode)) {
        if (const QString why = m_studio->generation()->utilityUnavailable(*utility); !why.isEmpty()) {
            m_studio->notify(why, QStringLiteral("warning"));
            return;
        }
    }
    QPointer<doc::Document> doc = m_doc;
    const QString providerId = provider->instanceId();
    const QString prompt = m_options->prompt().trimmed();
    const QString title = prompt.isEmpty() ? m_run->text() : prompt.left(60);
    ai::GenerationService *gen = m_studio->generation();

    switch (mode) {
    case Mode::Inpaint:
    case Mode::Edit:
    case Mode::Variation: {
        // A variation of the selection (inpainted at a low strength), else of the whole image.
        const bool hasSel = doc->hasSelection() && !doc->selectionBounds().isEmpty();
        PromptRun run;
        run.mode = ai::GenMode::ImageToImage;
        run.region = hasSel;
        run.prompt = prompt;
        run.strength = m_options->variationStrength();
        run.name = prompt.isEmpty() ? tr("AI variation") : prompt.simplified().left(40);
        run.action = Mode::Variation;
        submitPrompt(run);
        break;
    }
    case Mode::Expand: {
        const int amount = m_expandAmount->value();
        const QSize size = doc->size();
        QRect newRect(QPoint(0, 0), size);
        if (m_expandLeft->isChecked())
            newRect.setLeft(-amount);
        if (m_expandRight->isChecked())
            newRect.setRight(size.width() - 1 + amount);
        if (m_expandTop->isChecked())
            newRect.setTop(-amount);
        if (m_expandBottom->isChecked())
            newRect.setBottom(size.height() - 1 + amount);
        if (newRect.size() == size) {
            m_studio->notify(tr("Choose at least one direction."), QStringLiteral("warning"));
            return;
        }
        doc->resizeCanvas(newRect, tr("Expand canvas"));
        // Mask: the new area plus a small overlap into the original image for seamless blending.
        const int overlap = 24;
        QImage mask = img::createMask(doc->size(), 255);
        {
            QPainter p(&mask);
            const QRect original(-newRect.topLeft(), size);
            p.fillRect(original.adjusted(m_expandLeft->isChecked() ? overlap : 0, m_expandTop->isChecked() ? overlap : 0,
                                         m_expandRight->isChecked() ? -overlap : 0, m_expandBottom->isChecked() ? -overlap : 0),
                       Qt::black);
        }
        // The words describe the new area.
        ai::ImageRequest base = baseRequest(provider, prompt, ai::GenMode::Inpaint, false);
        base.denoise = 1.0;
        const int feather = m_options->feather();
        const bool colorMatch = m_options->colorMatch();
        withCharacter(base, ai::CharacterUse::Region, false, provider, [=, this](const ai::ImageRequest &req, const QJsonObject &character) {
            submitWithComposite([=, this](QImage composite) {
                if (!doc)
                    return;
                ai::GenerationSpec spec;
                spec.providerId = providerId;
                spec.request = req;
                spec.title = tr("Expand: %1").arg(title);
                spec.assetKind = doc::AssetKind::Edit;
                spec.region = ai::RegionInput{composite, mask, 64, 4, feather, colorMatch};
                if (!character.isEmpty())
                    spec.meta.insert(QStringLiteral("character"), character);
                Job *job = gen->submit(spec);
                trackJob(job, [this](doc::Document *d, const ai::GenerationOutput &out) { addResultLayers(d, out.images, tr("Expansion"), {}); });
            });
        });
        break;
    }
    case Mode::Upscale: {
        const double factor = m_upscaleFactor->currentData().toDouble();
        submitWithComposite([=, this](QImage composite) {
            Job *job = gen->submitUtility(providerId, ai::Utility::Upscale, composite, QJsonObject{{QStringLiteral("factor"), factor}},
                                          tr("Upscale %1×").arg(factor));
            trackJob(job, [this](doc::Document *d, const ai::GenerationOutput &out) {
                if (!out.images.isEmpty())
                    m_studio->openImageAsDocument(out.images.first(), tr("%1 (upscaled)").arg(d->name()));
            });
        });
        break;
    }
    case Mode::RemoveBackground: {
        submitWithComposite([=, this](QImage composite) {
            Job *job = gen->submitUtility(providerId, ai::Utility::RemoveBackground, img::flatten(composite, Qt::white), {}, tr("Remove background"));
            trackJob(job, [this](doc::Document *d, const ai::GenerationOutput &out) {
                if (out.images.isEmpty())
                    return;
                d->undoStack()->beginMacro(tr("Remove background"));
                for (const doc::LayerPtr &l : d->layers()) {
                    doc::LayerProps p = l->props;
                    p.visible = false;
                    d->setLayerProps(l->id, p, QString());
                }
                addResultLayers(d, {out.images.first()}, tr("Cut-out"), {});
                d->undoStack()->endMacro();
            });
        });
        break;
    }
    case Mode::DetectPose: {
        submitWithComposite([=, this](QImage composite) {
            Job *job = gen->submitUtility(providerId, ai::Utility::DetectPose, img::flatten(composite, Qt::white), {}, tr("Detect pose"));
            trackJob(job, [this](doc::Document *d, const ai::GenerationOutput &out) {
                const QJsonObject json = out.info.value(QStringLiteral("openpose")).toObject();
                if (auto pose = pose::Pose::fromOpenPoseJson(json, nullptr, d->size()); pose && !pose->isEmpty()) {
                    d->setControlPose(*pose);
                    if (m_studio->hasPage(QStringLiteral("poses"))) {
                        m_studio->notify(tr("Pose detected - it is now the document's control pose, and open in the Poses window to edit."));
                        m_studio->editPose(*pose, d->name());
                    } else {
                        m_studio->notify(tr("Pose detected - it is now the document's control pose."));
                    }
                } else if (!out.images.isEmpty()) {
                    addResultLayers(d, {out.images.first()}, tr("Pose map"), {});
                    m_studio->notify(tr("The server returned only a pose image (no keypoints); it was added as a layer."));
                }
            });
        });
        break;
    }
    }
}

} // namespace lumen::ui
