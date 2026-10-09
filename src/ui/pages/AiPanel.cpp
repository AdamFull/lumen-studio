#include "ui/pages/AiPanel.h"

#include "ai/PromptStyle.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "pose/PoseRenderer.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/JobCard.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

AiPanel::AiPanel(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
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

    v->addWidget(sectionLabel(tr("Provider")));
    m_provider = new ProviderCombo(studio->providers(), ai::ProviderKind::Image, content);
    v->addWidget(m_provider);
    m_capsWarning = new QLabel(content);
    m_capsWarning->setProperty("role", QStringLiteral("muted"));
    m_capsWarning->setWordWrap(true);
    v->addWidget(m_capsWarning);

    v->addWidget(sectionLabel(tr("Action")));
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    m_modes = new QButtonGroup(this);
    const QList<std::tuple<Mode, QString, QString, QString>> modes = {
        {Mode::Inpaint, QStringLiteral("mask"), tr("Fill selection"), tr("Regenerate the selected / masked area from the prompt (inpainting).")},
        {Mode::Variation, QStringLiteral("variation"), tr("Variation"), tr("Re-imagine the image (or the selection) while keeping its structure.")},
        {Mode::Edit, QStringLiteral("wand"), tr("Instruct edit"), tr("Describe the change in words (e.g. 'make it night', 'add a red scarf').")},
        {Mode::Expand, QStringLiteral("outpaint"), tr("Expand"), tr("Extend the canvas and let the model paint the new area (outpainting).")},
        {Mode::Upscale, QStringLiteral("upscale"), tr("Upscale"), tr("Increase resolution with an AI upscaler (opens a new document).")},
        {Mode::RemoveBackground, QStringLiteral("scissors"), tr("Remove BG"), tr("Cut out the subject onto a transparent layer.")},
        {Mode::DetectPose, QStringLiteral("person"), tr("Detect pose"), tr("Extract an editable OpenPose skeleton from the image.")},
    };
    int i = 0;
    for (const auto &[mode, icon, label, tip] : modes) {
        auto *b = new QToolButton(content);
        b->setIcon(Icons::get(icon));
        b->setText(label);
        b->setToolTip(tip);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(20, 20));
        b->setCheckable(true);
        b->setMinimumSize(78, 54);
        b->setProperty("role", QStringLiteral("chip"));
        b->setStyleSheet(QStringLiteral("QToolButton { border-radius: 9px; font-size: 8.5pt; }"));
        m_modes->addButton(b, static_cast<int>(mode));
        grid->addWidget(b, i / 3, i % 3);
        ++i;
    }
    m_modes->button(0)->setChecked(true);
    v->addLayout(grid);
    m_modeHint = mutedLabel(QString(), content);
    v->addWidget(m_modeHint);

    m_promptBox = new QWidget(content);
    auto *pv = new QVBoxLayout(m_promptBox);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(6);
    pv->addWidget(sectionLabel(tr("Prompt")));
    m_prompt = new AutoTextEdit(m_promptBox, 3, 8);
    m_prompt->setPlaceholderText(tr("Describe what should appear…"));
    pv->addWidget(m_prompt);
    m_negative = new AutoTextEdit(m_promptBox, 1, 4);
    m_negative->setPlaceholderText(tr("Negative prompt (optional)"));
    pv->addWidget(m_negative);
    v->addWidget(m_promptBox);

    m_strength = new SliderField(tr("Strength"), 0.05, 1.0, 0.75, 2, QString(), content);
    m_strength->setToolTip(tr("How much the model may change the area (denoising strength)."));
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
    m_upscaleFactor->addItem(QStringLiteral("2×"), 2.0);
    m_upscaleFactor->addItem(QStringLiteral("3×"), 3.0);
    m_upscaleFactor->addItem(QStringLiteral("4×"), 4.0);
    uh->addWidget(m_upscaleFactor, 1);
    v->addWidget(m_upscaleBox);

    m_refsBox = new QWidget(content);
    auto *rv = new QVBoxLayout(m_refsBox);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(sectionLabel(tr("References")));
    m_refs = new ReferenceStrip(studio, m_refsBox);
    m_refs->setCompact(true);
    rv->addWidget(m_refs);
    m_usePose = new QCheckBox(tr("Use the document's control pose"), m_refsBox);
    m_usePose->setChecked(true);
    rv->addWidget(m_usePose);
    v->addWidget(m_refsBox);

    v->addWidget(sectionLabel(tr("Options")));
    auto *variantsRow = new QHBoxLayout;
    variantsRow->addWidget(new QLabel(tr("Variants"), content));
    m_variants = new QSpinBox(content);
    m_variants->setRange(1, 8);
    m_variants->setValue(2);
    variantsRow->addWidget(m_variants);
    variantsRow->addStretch();
    v->addLayout(variantsRow);
    m_padding = new SliderField(tr("Context"), 0, 512, 64, 0, QStringLiteral(" px"), content);
    m_padding->setToolTip(tr("Surrounding area sent to the model along with the selection."));
    v->addWidget(m_padding);
    m_feather = new SliderField(tr("Blend edge"), 0, 128, 16, 0, QStringLiteral(" px"), content);
    v->addWidget(m_feather);
    m_colorMatch = new QCheckBox(tr("Match colours to the surroundings"), content);
    m_colorMatch->setChecked(true);
    v->addWidget(m_colorMatch);

    m_run = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Generate"), content);
    Theme::setRole(m_run, QStringLiteral("primary"));
    m_run->setMinimumHeight(36);
    m_run->setShortcut(QKeySequence(QStringLiteral("Ctrl+Return")));
    v->addWidget(m_run);

    m_jobs = new QVBoxLayout;
    m_jobs->setSpacing(6);
    v->addLayout(m_jobs);
    v->addStretch();

    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { setMode(static_cast<Mode>(id)); });
    connect(m_run, &QPushButton::clicked, this, &AiPanel::run);
    connect(m_prompt, &AutoTextEdit::submitted, this, &AiPanel::run);
    connect(m_provider, &ProviderCombo::providerChanged, this, &AiPanel::updateUi);
    connect(studio, &Studio::promptRequested, this, [this](const QString &prompt, const QString &negative) {
        m_prompt->setPlainText(prompt);
        m_negative->setPlainText(negative);
    });
    setMode(Mode::Inpaint);
}

void AiPanel::setDocument(doc::Document *doc)
{
    if (m_doc)
        m_doc->disconnect(this);
    m_doc = doc;
    if (doc)
        connect(doc, &doc::Document::selectionChanged, this, &AiPanel::updateUi);
    updateUi();
}

void AiPanel::setMode(Mode mode)
{
    m_mode = mode;
    if (QAbstractButton *b = m_modes->button(static_cast<int>(mode)))
        b->setChecked(true);
    updateUi();
}

void AiPanel::updateUi()
{
    const bool prompt = m_mode == Mode::Inpaint || m_mode == Mode::Variation || m_mode == Mode::Edit || m_mode == Mode::Expand;
    m_promptBox->setVisible(prompt);
    m_strength->setVisible(m_mode == Mode::Inpaint || m_mode == Mode::Variation);
    m_expandBox->setVisible(m_mode == Mode::Expand);
    m_upscaleBox->setVisible(m_mode == Mode::Upscale);
    m_refsBox->setVisible(prompt);
    m_padding->setVisible(m_mode == Mode::Inpaint || m_mode == Mode::Variation || m_mode == Mode::Edit);
    m_feather->setVisible(prompt);
    m_colorMatch->setVisible(prompt);
    m_variants->parentWidget()->setVisible(true);
    m_variants->setEnabled(prompt);
    m_strength->setValue(m_mode == Mode::Variation ? 0.45 : (m_strength->value() < 0.5 ? 0.75 : m_strength->value()));
    m_prompt->setPlaceholderText(m_mode == Mode::Edit ? tr("Describe the change, e.g. 'turn the jacket red'") : tr("Describe what should appear…"));
    QStringList hints;
    hints << m_modes->button(static_cast<int>(m_mode))->toolTip();
    const bool hasSel = m_doc && m_doc->hasSelection() && !m_doc->selectionBounds().isEmpty();
    if (m_mode == Mode::Inpaint && !hasSel)
        hints << tr("Paint a mask (Q) or make a selection first.");
    m_modeHint->setText(hints.join(u' '));

    ai::ImageProvider *provider = m_provider->imageProvider();
    QString warning;
    if (provider) {
        const ai::ImageCapabilities caps = provider->capabilities();
        if (m_mode == Mode::Edit && !caps.edit)
            warning = tr("%1 has no instruction editing; the change will be done by inpainting.").arg(provider->displayName());
        else if ((m_mode == Mode::Upscale && !provider->supportsUtility(ai::Utility::Upscale))
                 || (m_mode == Mode::RemoveBackground && !provider->supportsUtility(ai::Utility::RemoveBackground))
                 || (m_mode == Mode::DetectPose && !provider->supportsUtility(ai::Utility::DetectPose))) {
            ai::ImageProvider *other = m_studio->generation()->providerForUtility(
                m_mode == Mode::Upscale ? ai::Utility::Upscale : m_mode == Mode::RemoveBackground ? ai::Utility::RemoveBackground : ai::Utility::DetectPose);
            warning = other ? tr("Will use %1 for this action.").arg(other->displayName())
                            : tr("No configured provider supports this action.");
        } else if (!caps.inpaint && !caps.edit && m_mode == Mode::Inpaint) {
            warning = tr("%1 has no native inpainting; Lumen blends an img2img result into the selection.").arg(provider->displayName());
        }
        if (caps.maxReferences == 0)
            m_refs->setEnabled(false);
        else
            m_refs->setEnabled(true);
        m_refs->setMaximum(std::max(1, caps.maxReferences));
    }
    m_capsWarning->setText(warning);
    m_capsWarning->setVisible(!warning.isEmpty());
    m_run->setEnabled(m_doc != nullptr && (m_mode != Mode::Inpaint || hasSel));
    const QString runText = m_mode == Mode::Upscale ? tr("Upscale")
                          : m_mode == Mode::RemoveBackground ? tr("Remove background")
                          : m_mode == Mode::DetectPose ? tr("Detect pose")
                          : m_mode == Mode::Expand ? tr("Expand canvas")
                                                   : tr("Generate");
    m_run->setText(runText);
}

ai::ImageRequest AiPanel::baseRequest() const
{
    ai::ImageRequest r;
    ai::ImageProvider *provider = m_provider->imageProvider();
    const ai::PromptStyle style = ai::promptStyleFor(provider);
    r.prompt = m_prompt->text().trimmed();
    if (m_mode != Mode::Edit)
        r.prompt = style.apply(r.prompt);
    r.negativePrompt = m_negative->text().trimmed();
    if (r.negativePrompt.isEmpty() && style.supportsNegative)
        r.negativePrompt = style.defaultNegative;
    r.denoise = m_strength->value();
    r.batchCount = m_variants->value();
    r.references = m_refs->references();
    if (provider) {
        const ai::ImageCapabilities caps = provider->capabilities();
        r.references = r.references.mid(0, caps.maxReferences);
        if (m_usePose->isChecked() && m_doc && m_doc->controlPose() && caps.poseControl) {
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
    if (!m_doc)
        return;
    const doc::DocumentSnapshot snap = m_doc->snapshot();
    const QSize size = m_doc->size();
    async::run([snap, size] { return doc::flatten(snap, size); }).then(this, std::move(fn));
}

void AiPanel::addResultLayers(doc::Document *doc, const QList<QImage> &images, const QString &name, const QJsonObject &meta)
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
        m_studio->notify(tr("%1 variants were added as layers - toggle their visibility to compare.").arg(images.size()));
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

void AiPanel::run()
{
    if (!m_doc)
        return;
    const QString providerId = m_provider->providerId();
    if (providerId.isEmpty()) {
        m_studio->notify(tr("Add an image provider in Settings → Providers first."), QStringLiteral("warning"));
        return;
    }
    QPointer<doc::Document> doc = m_doc;
    const QString prompt = m_prompt->text().trimmed();
    const QString title = prompt.isEmpty() ? m_run->text() : prompt.left(60);
    ai::GenerationService *gen = m_studio->generation();

    switch (m_mode) {
    case Mode::Inpaint:
    case Mode::Variation:
    case Mode::Edit: {
        const bool hasSel = doc->hasSelection() && !doc->selectionBounds().isEmpty();
        if (m_mode == Mode::Inpaint && !hasSel) {
            m_studio->notify(tr("Paint a mask or make a selection first."), QStringLiteral("warning"));
            return;
        }
        if (m_mode == Mode::Edit && prompt.isEmpty()) {
            m_studio->notify(tr("Describe the change you want."), QStringLiteral("warning"));
            return;
        }
        ai::ImageRequest req = baseRequest();
        const QImage selection = hasSel ? doc->selection() : QImage();
        const Mode mode = m_mode;
        const int padding = int(m_padding->value());
        const int feather = int(m_feather->value());
        const bool colorMatch = m_colorMatch->isChecked();
        ai::ImageProvider *provider = m_provider->imageProvider();
        const ai::ImageCapabilities caps = provider ? provider->capabilities() : ai::ImageCapabilities();
        submitWithComposite([=](QImage composite) {
            if (!doc)
                return;
            ai::GenerationSpec spec;
            spec.providerId = providerId;
            spec.request = req;
            spec.title = title;
            spec.assetKind = doc::AssetKind::Edit;
            spec.meta = QJsonObject{{QStringLiteral("document"), doc->id()}, {QStringLiteral("action"), static_cast<int>(mode)}};
            if (!selection.isNull()) {
                spec.region = ai::RegionInput{composite, selection, padding, 6, feather, colorMatch};
            } else {
                // Whole image: img2img / instruction edit at the model's working resolution.
                const QSize workSize = caps.fixedSizes.isEmpty()
                                           ? img::fitToPixelBudget(composite.size(), std::min(caps.nativePixels, caps.maxPixels), caps.sizeMultiple)
                                           : composite.size();
                spec.request.mode = mode == Mode::Edit && caps.edit ? ai::GenMode::Edit : ai::GenMode::ImageToImage;
                spec.request.initImage = img::normalized(img::scaled(img::flatten(composite, Qt::white), workSize, true));
                spec.request.size = workSize;
            }
            Job *job = gen->submit(spec);
            const QString name = mode == Mode::Edit ? tr("Edit: %1").arg(prompt.left(32)) : (prompt.isEmpty() ? tr("AI variation") : prompt.left(40));
            trackJob(job, [this, name](doc::Document *d, const ai::GenerationOutput &out) {
                QJsonObject meta{{QStringLiteral("generated"), true}};
                if (!out.assets.isEmpty())
                    meta.insert(QStringLiteral("asset"), out.assets.first().id);
                addResultLayers(d, out.images, name, meta);
            });
        });
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
        ai::ImageRequest req = baseRequest();
        req.denoise = 1.0;
        const int feather = int(m_feather->value());
        const bool colorMatch = m_colorMatch->isChecked();
        submitWithComposite([=](QImage composite) {
            if (!doc)
                return;
            ai::GenerationSpec spec;
            spec.providerId = providerId;
            spec.request = req;
            spec.title = tr("Expand: %1").arg(title);
            spec.assetKind = doc::AssetKind::Edit;
            spec.region = ai::RegionInput{composite, mask, 64, 4, feather, colorMatch};
            Job *job = gen->submit(spec);
            trackJob(job, [this](doc::Document *d, const ai::GenerationOutput &out) { addResultLayers(d, out.images, tr("Expansion"), {}); });
        });
        break;
    }
    case Mode::Upscale: {
        const double factor = m_upscaleFactor->currentData().toDouble();
        submitWithComposite([=](QImage composite) {
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
        submitWithComposite([=](QImage composite) {
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
        submitWithComposite([=](QImage composite) {
            Job *job = gen->submitUtility(providerId, ai::Utility::DetectPose, img::flatten(composite, Qt::white), {}, tr("Detect pose"));
            trackJob(job, [this](doc::Document *d, const ai::GenerationOutput &out) {
                const QJsonObject json = out.info.value(QStringLiteral("openpose")).toObject();
                if (auto pose = pose::Pose::fromOpenPoseJson(json, nullptr, d->size()); pose && !pose->isEmpty()) {
                    d->setControlPose(*pose);
                    m_studio->notify(tr("Pose detected - it is now the document's control pose. Open the Poses page to edit it."));
                    m_studio->editPose(*pose, d->name());
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
