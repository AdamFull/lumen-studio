#include "ui/pages/GeneratePage.h"

#include "ai/CharacterContext.h"
#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/PromptStyle.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "pose/PosePresets.h"
#include "pose/PoseRenderer.h"
#include "ui/Icons.h"
#include "ui/Shortcuts.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "ui/widgets/CharacterChip.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/JobCard.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

const QList<QPair<QString, double>> kAspects = {{QStringLiteral("1:1"), 1.0},          {QStringLiteral("2:3"), 2.0 / 3.0},
                                                {QStringLiteral("3:4"), 0.75},         {QStringLiteral("9:16"), 9.0 / 16.0},
                                                {QStringLiteral("3:2"), 1.5},          {QStringLiteral("4:3"), 4.0 / 3.0},
                                                {QStringLiteral("16:9"), 16.0 / 9.0}};

QWidget *labeled(const QString &label, QWidget *field, QWidget *parent)
{
    auto *w = new QWidget(parent);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    v->addWidget(mutedLabel(label, w));
    v->addWidget(field);
    return w;
}

} // namespace

GeneratePage::GeneratePage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ---- Settings column ----
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(450);
    auto *panel = new QWidget(scroll);
    panel->setObjectName(QStringLiteral("Panel"));
    scroll->setWidget(panel);
    auto *v = new QVBoxLayout(panel);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(12);

    auto *title = new QLabel(tr("Generate"), panel);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);

    auto *provRow = new QHBoxLayout;
    m_provider = new ProviderCombo(studio->providers(), ai::ProviderKind::Image, panel);
    m_model = new QComboBox(panel);
    m_model->setEditable(true);
    m_model->setObjectName(QStringLiteral("model"));
    m_model->lineEdit()->setPlaceholderText(tr("default model"));
    auto *refreshModels = iconButton(QStringLiteral("refresh"), tr("Load models from the provider"), panel);
    provRow->addWidget(labeled(tr("Provider"), m_provider, panel), 1);
    auto *modelBox = new QWidget(panel);
    auto *mh = new QHBoxLayout(modelBox);
    mh->setContentsMargins(0, 0, 0, 0);
    mh->addWidget(m_model, 1);
    mh->addWidget(refreshModels);
    provRow->addWidget(labeled(tr("Model"), modelBox, panel), 1);
    v->addLayout(provRow);
    m_capsLabel = mutedLabel(QString(), panel);
    v->addWidget(m_capsLabel);

    auto *promptHead = new QHBoxLayout;
    promptHead->addWidget(sectionLabel(tr("Prompt"), panel));
    promptHead->addStretch();
    m_character = new CharacterChip(studio, QStringLiteral("generate"), panel);
    promptHead->addWidget(m_character);
    auto *enhance = new QToolButton(panel);
    enhance->setIcon(Icons::get(QStringLiteral("wand")));
    enhance->setText(tr("Enhance"));
    enhance->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    enhance->setToolTip(tr("Let the prompt-writer model improve the prompt for the selected image model"));
    promptHead->addWidget(enhance);
    v->addLayout(promptHead);
    m_prompt = new AutoTextEdit(panel, 4, 12);
    m_prompt->setPlaceholderText(tr("A portrait of a silver-haired knight in ornate armour, dramatic rim light…"));
    v->addWidget(m_prompt);
    m_negative = new AutoTextEdit(panel, 1, 5);
    m_negative->setPlaceholderText(tr("Negative prompt (what to avoid)"));
    v->addWidget(m_negative);

    v->addWidget(sectionLabel(tr("Size"), panel));
    auto *aspectRow = new QGridLayout;
    aspectRow->setSpacing(4);
    m_aspects = new QButtonGroup(this);
    for (int i = 0; i < kAspects.size(); ++i) {
        auto *chip = new QToolButton(panel);
        chip->setText(kAspects[i].first);
        chip->setCheckable(true);
        chip->setProperty("role", QStringLiteral("chip"));
        m_aspects->addButton(chip, i);
        aspectRow->addWidget(chip, i / 4, i % 4);
    }
    m_aspects->button(1)->setChecked(true);
    v->addLayout(aspectRow);
    auto *sizeRow = new QHBoxLayout;
    m_customSize = new QCheckBox(tr("Custom"), panel);
    m_width = new QSpinBox(panel);
    m_height = new QSpinBox(panel);
    for (QSpinBox *s : {m_width, m_height}) {
        s->setRange(64, 4096);
        s->setSingleStep(64);
        s->setSuffix(QStringLiteral(" px"));
        s->setEnabled(false);
    }
    m_width->setValue(832);
    m_height->setValue(1216);
    m_sizeLabel = mutedLabel(QString(), panel);
    sizeRow->addWidget(m_customSize);
    sizeRow->addWidget(m_width);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), panel));
    sizeRow->addWidget(m_height);
    sizeRow->addStretch();
    v->addLayout(sizeRow);
    v->addWidget(m_sizeLabel);

    m_samplingBox = new QWidget(panel);
    auto *sg = new QGridLayout(m_samplingBox);
    sg->setContentsMargins(0, 0, 0, 0);
    sg->setHorizontalSpacing(10);
    sg->setVerticalSpacing(8);
    m_steps = new SliderField(tr("Steps"), 1, 150, 28, 0, QString(), m_samplingBox);
    m_cfg = new SliderField(tr("CFG"), 1, 20, 6, 1, QString(), m_samplingBox);
    m_cfg->setObjectName(QStringLiteral("cfg"));
    m_sampler = new QComboBox(m_samplingBox);
    m_sampler->setEditable(true);
    m_sampler->lineEdit()->setPlaceholderText(tr("default"));
    m_scheduler = new QComboBox(m_samplingBox);
    m_scheduler->setEditable(true);
    m_scheduler->lineEdit()->setPlaceholderText(tr("default"));
    sg->addWidget(m_steps, 0, 0, 1, 2);
    sg->addWidget(m_cfg, 1, 0, 1, 2);
    sg->addWidget(labeled(tr("Sampler"), m_sampler, m_samplingBox), 2, 0);
    sg->addWidget(labeled(tr("Scheduler"), m_scheduler, m_samplingBox), 2, 1);
    v->addWidget(m_samplingBox);

    auto *row = new QHBoxLayout;
    m_batch = new QSpinBox(panel);
    m_batch->setRange(1, 16);
    m_batch->setValue(2);
    m_seed = new QSpinBox(panel);
    m_seed->setRange(0, 2147483647);
    m_seed->setEnabled(false);
    m_randomSeed = new QCheckBox(tr("Random"), panel);
    m_randomSeed->setChecked(true);
    m_quality = new QComboBox(panel);
    m_quality->addItems({QStringLiteral("high"), QStringLiteral("medium"), QStringLiteral("low"), QStringLiteral("auto")});
    row->addWidget(labeled(tr("Images"), m_batch, panel));
    auto *seedBox = new QWidget(panel);
    auto *sh = new QHBoxLayout(seedBox);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(m_seed, 1);
    sh->addWidget(m_randomSeed);
    row->addWidget(labeled(tr("Seed"), seedBox, panel), 1);
    row->addWidget(labeled(tr("Quality"), m_quality, panel));
    v->addLayout(row);
    m_transparent = new QCheckBox(tr("Transparent background (if supported)"), panel);
    v->addWidget(m_transparent);

    v->addWidget(sectionLabel(tr("References"), panel));
    m_refs = new ReferenceStrip(studio, panel);
    v->addWidget(m_refs);

    v->addWidget(sectionLabel(tr("Pose control"), panel));
    auto *poseRow = new QHBoxLayout;
    m_poseThumb = new QLabel(panel);
    m_poseThumb->setFixedSize(72, 96);
    m_poseThumb->setAlignment(Qt::AlignCenter);
    m_poseThumb->setStyleSheet(QStringLiteral("background: %1; border-radius: 8px; color: %2;").arg(Theme::palette().canvas.name(), Theme::palette().textMuted.name()));
    poseRow->addWidget(m_poseThumb);
    auto *poseButtons = new QVBoxLayout;
    auto *presetBtn = new QPushButton(Icons::get(QStringLiteral("person")), tr("Preset…"), panel);
    auto *presetMenu = new QMenu(presetBtn);
    QHash<QString, QMenu *> categories;
    for (const pose::PosePreset &p : pose::builtInPresets()) {
        QMenu *&m = categories[p.category];
        if (!m)
            m = presetMenu->addMenu(p.category);
        m->addAction(p.name, this, [this, p] { setPose(p.toPose(requestedSize())); });
    }
    presetBtn->setMenu(presetMenu);
    auto *editPose = new QPushButton(Icons::get(QStringLiteral("edit")), tr("Edit…"), panel);
    auto *clearPose = new QPushButton(Icons::get(QStringLiteral("close")), tr("Clear"), panel);
    poseButtons->addWidget(presetBtn);
    poseButtons->addWidget(editPose);
    // The pose editor is a plugin page.
    editPose->setVisible(studio->hasPage(QStringLiteral("poses")));
    poseButtons->addWidget(clearPose);
    poseRow->addLayout(poseButtons);
    v->addLayout(poseRow);
    m_poseWeight = new SliderField(tr("Strength"), 0.1, 2.0, 1.0, 2, QString(), panel);
    v->addWidget(m_poseWeight);

    v->addWidget(sectionLabel(tr("Start from an image (img2img)"), panel));
    auto *initRow = new QHBoxLayout;
    m_initThumb = new QLabel(tr("none"), panel);
    m_initThumb->setFixedSize(72, 72);
    m_initThumb->setAlignment(Qt::AlignCenter);
    m_initThumb->setStyleSheet(m_poseThumb->styleSheet());
    initRow->addWidget(m_initThumb);
    auto *initButtons = new QVBoxLayout;
    auto *chooseInit = new QPushButton(Icons::get(QStringLiteral("image")), tr("Choose…"), panel);
    auto *clearInit = new QPushButton(Icons::get(QStringLiteral("close")), tr("Clear"), panel);
    initButtons->addWidget(chooseInit);
    initButtons->addWidget(clearInit);
    initRow->addLayout(initButtons);
    v->addLayout(initRow);
    m_denoise = new SliderField(tr("Denoise"), 0.05, 1.0, 0.6, 2, QString(), panel);
    v->addWidget(m_denoise);

    m_go = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Generate"), panel);
    Theme::setRole(m_go, QStringLiteral("primary"));
    m_go->setMinimumHeight(40);
    Shortcuts::instance().bindButton(m_go, QStringLiteral("generate.run"), tr("Generate"), QStringLiteral("generate"),
                                     QKeySequence(QStringLiteral("Ctrl+Return")), tr("Generate"));
    m_go->setToolTip(tr("Generate (Ctrl+Enter)"));
    v->addWidget(m_go);
    v->addStretch();

    // ---- Results column ----
    auto *results = new QWidget(splitter);
    auto *rv = new QVBoxLayout(results);
    rv->setContentsMargins(16, 16, 16, 16);
    rv->setSpacing(10);
    auto *rh = new QHBoxLayout;
    auto *rtitle = new QLabel(tr("Results"), results);
    Theme::setRole(rtitle, QStringLiteral("heading"));
    rh->addWidget(rtitle);
    rh->addStretch();
    auto *clearResults = new QToolButton(results);
    clearResults->setText(tr("Clear"));
    rh->addWidget(clearResults);
    rv->addLayout(rh);
    m_jobs = new QVBoxLayout;
    m_jobs->setSpacing(6);
    rv->addLayout(m_jobs);
    m_results = new QListWidget(results);
    m_results->setViewMode(QListView::IconMode);
    m_results->setIconSize(QSize(240, 240));
    m_results->setGridSize(QSize(256, 256));
    m_results->setResizeMode(QListView::Adjust);
    m_results->setMovement(QListView::Static);
    m_results->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_results->setContextMenuPolicy(Qt::CustomContextMenu);
    m_results->setStyleSheet(QStringLiteral("QListWidget { background: %1; border-radius: 10px; padding: 8px; }").arg(Theme::palette().window.name()));
    rv->addWidget(m_results, 1);
    auto *emptyHint = mutedLabel(tr("Results appear here. Write a prompt, optionally add references or a pose, and press Generate (Ctrl+Enter)."), results);
    emptyHint->setAlignment(Qt::AlignCenter);
    rv->addWidget(emptyHint);
    connect(m_results->model(), &QAbstractItemModel::rowsInserted, emptyHint, [emptyHint] { emptyHint->hide(); });
    rv->addWidget(mutedLabel(tr("Double-click a result to open it in the editor. All results are stored in the Library."), results));

    splitter->addWidget(scroll);
    splitter->addWidget(results);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({470, 1000});

    // ---- Wiring ----
    connect(m_go, &QPushButton::clicked, this, &GeneratePage::generate);
    connect(m_prompt, &AutoTextEdit::submitted, this, &GeneratePage::generate);
    connect(enhance, &QToolButton::clicked, this, &GeneratePage::enhancePrompt);
    connect(m_provider, &ProviderCombo::providerChanged, this, &GeneratePage::providerChanged);
    connect(m_model, &QComboBox::currentTextChanged, this, &GeneratePage::followRecommendedCfg);
    connect(refreshModels, &QToolButton::clicked, this, [this] {
        ai::ImageProvider *p = m_provider->imageProvider();
        if (!p)
            return;
        p->listModels().then(this, [this](QStringList models) {
            const QString cur = m_model->currentText();
            m_model->clear();
            m_model->addItem(QString());
            m_model->addItems(models);
            m_model->setCurrentText(cur);
            m_model->showPopup();
        }).onFailed(this, [this](const std::exception &e) { m_studio->notify(describeException(e), QStringLiteral("error")); });
        p->listSamplers().then(this, [this](QStringList s) {
            const QString cur = m_sampler->currentText();
            m_sampler->clear();
            m_sampler->addItem(QString());
            m_sampler->addItems(s);
            m_sampler->setCurrentText(cur);
        }).onFailed(this, [](const std::exception &) {});
        p->listSchedulers().then(this, [this](QStringList s) {
            const QString cur = m_scheduler->currentText();
            m_scheduler->clear();
            m_scheduler->addItem(QString());
            m_scheduler->addItems(s);
            m_scheduler->setCurrentText(cur);
        }).onFailed(this, [](const std::exception &) {});
    });
    connect(m_aspects, &QButtonGroup::idClicked, this, &GeneratePage::updateSizeLabel);
    connect(m_customSize, &QCheckBox::toggled, this, [this](bool on) {
        m_width->setEnabled(on);
        m_height->setEnabled(on);
        for (QAbstractButton *b : m_aspects->buttons())
            b->setEnabled(!on);
        updateSizeLabel();
    });
    connect(m_width, &QSpinBox::valueChanged, this, &GeneratePage::updateSizeLabel);
    connect(m_height, &QSpinBox::valueChanged, this, &GeneratePage::updateSizeLabel);
    connect(m_randomSeed, &QCheckBox::toggled, m_seed, [this](bool on) { m_seed->setEnabled(!on); });
    connect(editPose, &QPushButton::clicked, this, [this] {
        m_studio->editPose(m_pose.value_or(pose::findBuiltInPreset(QStringLiteral("standing"))->toPose(requestedSize())), tr("Generate"));
    });
    connect(clearPose, &QPushButton::clicked, this, [this] { setPose(std::nullopt); });
    connect(chooseInit, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Start image"), QString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
        if (f.isEmpty())
            return;
        QImageReader reader(f);
        reader.setAutoTransform(true);
        setInitImage(reader.read());
    });
    connect(clearInit, &QPushButton::clicked, this, [this] { setInitImage(QImage()); });
    connect(clearResults, &QToolButton::clicked, m_results, &QListWidget::clear);
    connect(m_results, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        m_studio->openAssetInEditor(item->data(Qt::UserRole).toString());
    });
    connect(m_results, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_results->itemAt(pos);
        if (!item)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(Icons::get(QStringLiteral("brush")), tr("Open in editor"), this, [this, id] { m_studio->openAssetInEditor(id); });
        menu.addAction(Icons::get(QStringLiteral("link")), tr("Use as reference"), this, [this, id] { m_refs->addAsset(id); });
        menu.addAction(Icons::get(QStringLiteral("variation")), tr("Use as start image"), this, [this, id] {
            if (doc::Project *p = m_studio->project())
                p->loadImage(id).then(this, [this, id](QImage img) { setInitImage(img, id); });
        });
        menu.addAction(Icons::get(QStringLiteral("refresh")), tr("Re-run with these settings"), this, [this, id] { rerun(id); });
        menu.exec(m_results->viewport()->mapToGlobal(pos));
    });

    connect(studio, &Studio::referenceRequested, m_refs, &ReferenceStrip::addAsset);
    connect(studio, &Studio::poseForGenerationRequested, this, [this](const pose::Pose &p) {
        setPose(p);
        m_studio->showPage(pages::Generate);
    });
    connect(studio, &Studio::promptRequested, this, [this](const QString &prompt, const QString &negative) {
        m_prompt->setPlainText(prompt);
        m_negative->setPlainText(negative);
    });
    connect(studio, &Studio::rerunRequested, this, &GeneratePage::rerun);
    providerChanged();
    setPose(std::nullopt);
}

QSize GeneratePage::requestedSize() const
{
    if (m_customSize->isChecked())
        return QSize(m_width->value(), m_height->value());
    ai::ImageProvider *p = m_provider->imageProvider();
    const qint64 pixels = p ? p->capabilities().nativePixels : 1024 * 1024;
    const int multiple = p ? std::max(8, p->capabilities().sizeMultiple) : 64;
    const int id = std::max(0, m_aspects->checkedId());
    return ai::Director::sizeForAspect(kAspects[id].first, pixels, std::max(multiple, 64));
}

void GeneratePage::updateSizeLabel()
{
    const QSize s = requestedSize();
    QString text = tr("%1 × %2 px").arg(s.width()).arg(s.height());
    if (ai::ImageProvider *p = m_provider->imageProvider()) {
        const ai::ImageCapabilities caps = p->capabilities();
        if (!caps.fixedSizes.isEmpty())
            text += tr(" (the provider uses the closest supported size)");
        else if (!caps.aspectRatios.isEmpty())
            text += tr(" (the provider uses the closest aspect ratio)");
    }
    m_sizeLabel->setText(text);
    if (m_pose)
        setPose(m_pose);
}

void GeneratePage::followRecommendedCfg()
{
    const ai::PromptStyle style = ai::promptStyleFor(m_provider->imageProvider(), m_model->currentText());
    if (style.family == m_cfgFamily)
        return; // the user's value stays while the model family does
    m_cfgFamily = style.family;
    m_cfg->setValue(style.cfg);
}

void GeneratePage::providerChanged()
{
    ai::ImageProvider *p = m_provider->imageProvider();
    if (!p) {
        m_capsLabel->setText(tr("No image provider configured — add one in Settings."));
        updateSizeLabel();
        return;
    }
    const ai::ImageCapabilities caps = p->capabilities();
    m_model->clear();
    m_model->addItem(QString());
    m_model->setCurrentText(QString());
    m_samplingBox->setVisible(caps.steps || caps.cfg || caps.samplers);
    m_negative->setVisible(caps.negativePrompt);
    m_quality->parentWidget()->setVisible(caps.qualityTiers);
    m_transparent->setVisible(caps.transparentBackground);
    m_seed->parentWidget()->parentWidget()->setVisible(caps.seed);
    m_refs->setMaximum(caps.maxReferences);
    m_refs->setEnabled(caps.maxReferences > 0);
    QStringList notes;
    if (caps.maxReferences > 0)
        notes << tr("up to %1 reference images").arg(caps.maxReferences);
    if (caps.poseControl)
        notes << tr("pose control");
    if (caps.edit)
        notes << tr("instruction edits");
    const ai::PromptStyle style = ai::promptStyleFor(p);
    notes << (style.dialect == ai::PromptStyle::Dialect::Tags ? tr("tag-style prompts") : tr("natural-language prompts"));
    followRecommendedCfg();
    m_capsLabel->setText(tr("Supports: %1").arg(notes.join(QStringLiteral(", "))));
    updateSizeLabel();
}

void GeneratePage::setPose(const std::optional<pose::Pose> &p)
{
    m_pose = p;
    if (!p) {
        m_poseThumb->setPixmap(QPixmap());
        m_poseThumb->setText(tr("none"));
        return;
    }
    pose::Pose fitted = *p;
    fitted.resizeCanvas(requestedSize(), true);
    m_pose = fitted;
    const QImage map = pose::renderOpenPose(fitted, fitted.canvas);
    m_poseThumb->setPixmap(QPixmap::fromImage(map.scaled(m_poseThumb->size() * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation))
                               .scaled(m_poseThumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void GeneratePage::setInitImage(const QImage &image, const QString &assetId)
{
    Q_UNUSED(assetId);
    m_init = img::normalized(image);
    if (m_init.isNull()) {
        m_initThumb->setPixmap(QPixmap());
        m_initThumb->setText(tr("none"));
    } else {
        m_initThumb->setPixmap(QPixmap::fromImage(m_init.scaled(m_initThumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
}

void GeneratePage::generate()
{
    ai::ImageProvider *provider = m_provider->imageProvider();
    if (!provider) {
        m_studio->notify(tr("Add an image provider in Settings → Providers first."), QStringLiteral("warning"));
        return;
    }
    const QString prompt = m_prompt->text().trimmed();
    if (prompt.isEmpty() && m_init.isNull()) {
        m_studio->notify(tr("Write a prompt first."), QStringLiteral("warning"));
        return;
    }
    const ai::ImageCapabilities caps = provider->capabilities();
    const ai::PromptStyle style = ai::promptStyleFor(provider, m_model->currentText());
    ai::GenerationSpec spec;
    spec.providerId = provider->instanceId();
    ai::ImageRequest &r = spec.request;
    r.prompt = style.apply(prompt);
    r.negativePrompt = m_negative->text().trimmed();
    if (r.negativePrompt.isEmpty() && style.supportsNegative)
        r.negativePrompt = style.defaultNegative;
    r.size = requestedSize();
    r.model = m_model->currentText().trimmed();
    r.batchCount = m_batch->value();
    r.seed = m_randomSeed->isChecked() ? qint64(QRandomGenerator::global()->bounded(2147483647)) : m_seed->value();
    r.steps = int(m_steps->value());
    r.cfg = m_cfg->value();
    r.cfgChosen = true;
    r.sampler = m_sampler->currentText().trimmed();
    r.scheduler = m_scheduler->currentText().trimmed();
    if (caps.qualityTiers)
        r.quality = m_quality->currentText();
    r.transparentBackground = caps.transparentBackground && m_transparent->isChecked();
    r.references = m_refs->references().mid(0, caps.maxReferences);
    if (m_pose && caps.poseControl) {
        ai::ControlInput c;
        c.type = ai::ControlType::OpenPose;
        c.image = pose::renderOpenPose(*m_pose, r.size);
        c.weight = m_poseWeight->value();
        r.controls << c;
    }
    if (!m_init.isNull()) {
        r.mode = ai::GenMode::ImageToImage;
        r.initImage = img::normalized(m_init.scaled(r.size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                                          .copy(QRect(QPoint(0, 0), r.size)));
        r.denoise = m_denoise->value();
    }
    spec.title = prompt.left(60);
    spec.meta = QJsonObject{{QStringLiteral("source"), QStringLiteral("generate")}, {QStringLiteral("user_prompt"), prompt}};
    if (!m_randomSeed->isChecked())
        m_seed->setValue(int(r.seed));
    const QString characterId = m_character->characterId();
    if (characterId.isEmpty())
        return submit(spec, prompt);
    // The project's character: its LoRA (if this model family can load it), else its reference,
    // and its description.
    const QString family = checkpointFamily(m_studio, provider);
    const QString providerName = provider->displayName();
    QPointer<GeneratePage> self(this);
    m_studio->characterContext(characterId).then(this, [self, spec, caps, style, family, providerName, prompt](const ai::CharacterContext &c) mutable {
        if (!self)
            return;
        if (!c.isEmpty()) {
            const ai::CharacterApplied applied = ai::applyCharacter(spec.request, c, caps, ai::CharacterUse::Full, style, family);
            spec.meta.insert(QStringLiteral("character"), applied.toJson(c));
            self->noteFallback(applied.fallbackNote(c, providerName));
        }
        self->submit(spec, prompt);
    });
}

void GeneratePage::noteFallback(const QString &note)
{
    if (note.isEmpty() || m_notes.contains(note))
        return;
    m_notes.insert(note);
    m_studio->notify(note);
}

void GeneratePage::submit(const ai::GenerationSpec &spec, const QString &prompt)
{
    Job *job = m_studio->generation()->submit(spec);
    auto *card = new JobCard(job, m_studio->jobs(), this);
    m_jobs->insertWidget(0, card);
    connect(card, &JobCard::dismissed, card, &QObject::deleteLater);
    QPointer<JobCard> guard(card);
    connect(job, &Job::finished, this, [this, guard, prompt](Job *j) {
        if (j->state() == Job::State::Succeeded) {
            const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
            for (int i = 0; i < out.images.size(); ++i)
                addResult(i < out.assets.size() ? out.assets[i].id : QString(), out.images[i], prompt);
            if (guard)
                QTimer::singleShot(1500, guard, &QObject::deleteLater);
        } else if (j->state() == Job::State::Failed) {
            m_studio->notify(j->errorString(), QStringLiteral("error"));
        }
    });
}

void GeneratePage::addResult(const QString &assetId, const QImage &image, const QString &prompt)
{
    auto *item = new QListWidgetItem;
    item->setIcon(QIcon(QPixmap::fromImage(image.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
    item->setData(Qt::UserRole, assetId);
    item->setToolTip(prompt);
    m_results->insertItem(0, item);
}

void GeneratePage::enhancePrompt()
{
    const QString current = m_prompt->text().trimmed();
    if (current.isEmpty()) {
        m_studio->notify(tr("Write a short idea first, then enhance it."));
        return;
    }
    const ai::PromptStyle style = ai::promptStyleFor(m_provider->imageProvider(), m_model->currentText());
    QJsonObject shot{{QStringLiteral("prompt"), current}, {QStringLiteral("negative_prompt"), m_negative->text()}};
    ai::StructuredChatOperation *op = m_studio->director()->rewritePrompt(shot, {}, QString(), style);
    m_prompt->setEnabled(false);
    m_prompt->setPlainText(tr("Enhancing…"));
    QPointer<GeneratePage> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, current] {
        if (!self)
            return;
        self->m_prompt->setEnabled(true);
        if (op->state() == ai::Operation::State::Succeeded) {
            self->m_prompt->setPlainText(op->result().value(QStringLiteral("prompt")).toString());
            const QString neg = op->result().value(QStringLiteral("negative_prompt")).toString();
            if (!neg.isEmpty())
                self->m_negative->setPlainText(neg);
        } else {
            self->m_prompt->setPlainText(current);
            if (op->state() == ai::Operation::State::Failed)
                self->m_studio->notify(op->errorString(), QStringLiteral("error"));
        }
    });
}

void GeneratePage::rerun(const QString &assetId)
{
    doc::Project *p = m_studio->project();
    const auto info = p ? p->asset(assetId) : std::nullopt;
    if (!info)
        return;
    const QJsonObject m = info->metadata;
    // The words the user wrote (without the character's trigger and description) when known.
    m_prompt->setPlainText(m.value(m.contains(QStringLiteral("user_prompt")) ? QStringLiteral("user_prompt") : QStringLiteral("prompt")).toString());
    m_negative->setPlainText(m.value(QStringLiteral("negative_prompt")).toString());
    const int w = m.value(QStringLiteral("width")).toInt(), h = m.value(QStringLiteral("height")).toInt();
    if (w > 0 && h > 0) {
        m_customSize->setChecked(true);
        m_width->setValue(w);
        m_height->setValue(h);
    }
    // The model first: its recommended CFG must not replace the image's.
    m_model->setCurrentText(m.value(QStringLiteral("model")).toString());
    if (m.contains(QStringLiteral("steps")))
        m_steps->setValue(m.value(QStringLiteral("steps")).toDouble());
    if (m.contains(QStringLiteral("cfg")))
        m_cfg->setValue(m.value(QStringLiteral("cfg")).toDouble());
    m_sampler->setCurrentText(m.value(QStringLiteral("sampler")).toString());
    m_scheduler->setCurrentText(m.value(QStringLiteral("scheduler")).toString());
    const qint64 seed = qint64(m.value(QStringLiteral("seed")).toDouble(-1));
    if (seed >= 0) {
        m_randomSeed->setChecked(false);
        m_seed->setValue(int(seed));
    }
    // The character the image was drawn with, or none.
    QString note;
    const QJsonObject character = m.value(QStringLiteral("character")).toObject();
    const QString characterId = character.value(QStringLiteral("character")).toString();
    if (!characterId.isEmpty()) {
        if (const auto entry = m_studio->characters()->entry(characterId)) {
            m_studio->setActiveCharacter(characterId);
            m_character->setUsedHere(true);
            const QString lora = character.value(QStringLiteral("lora")).toString();
            if (!lora.isEmpty() && ai::CharacterLora::fromJson(entry->lora).file != lora)
                note = tr("The LoRA of %1 has changed since; the current one is used.").arg(entry->name);
        } else {
            note = tr("%1 is no longer in the character library.").arg(character.value(QStringLiteral("name")).toString());
        }
    } else if (!m_character->characterId().isEmpty()) {
        m_character->setUsedHere(false);
        note = tr("The image was drawn without a character, so the character is switched off here.");
    }
    m_studio->notify(note.isEmpty() ? tr("Settings restored from '%1'.").arg(info->name)
                                    : tr("Settings restored from '%1'. %2").arg(info->name, note));
}

} // namespace lumen::ui
