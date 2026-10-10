#include "ui/dialogs/LoraDatasetDialog.h"

#include "ai/Director.h"
#include "ai/LoraCaptions.h"
#include "ai/ProviderRegistry.h"
#include "ai/StructuredChat.h"
#include "core/Async.h"
#include "core/Error.h"
#include "core/Job.h"
#include "doc/CharacterLibrary.h"
#include "doc/Project.h"
#include "runtime/LoraTraining.h"
#include "runtime/ModelLibrary.h"
#include "runtime/RuntimeStore.h"
#include "runtime/Trainer.h"
#include "ui/CharacterTraining.h"
#include "ui/Icons.h"
#include "ui/LoraEvaluation.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "ui/dialogs/LocalServersPage.h"
#include "ui/director/DirectorSession.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

constexpr int kThumb = 168;
constexpr int kMinImages = 15;   // fewer and the LoRA learns too little
constexpr int kGoodImages = 20;

} // namespace

int LoraDatasetDialog::datasetScore(const QJsonObject &critique)
{
    const int overall = critique.value(QStringLiteral("overall")).toInt(0);
    const int identity = critique.value(QStringLiteral("identity")).toInt(overall);
    return std::clamp(std::min(overall, identity), 0, 10);
}

LoraDatasetDialog::LoraDatasetDialog(Studio *studio, const QString &characterId, DirectorSession *session, std::function<void()> planShots,
                                     QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_characterId(characterId)
    , m_session(session)
    , m_planShots(std::move(planShots))
    , m_dataset(new doc::LoraDataset(studio->characters()->datasetDirectory(characterId), this))
{
    const auto entry = studio->characters()->entry(characterId);
    m_sheet = entry ? entry->sheet : QJsonObject();
    setWindowTitle(tr("LoRA training set — %1").arg(entry ? entry->name : characterId));
    resize(1080, 720);
    auto *v = new QVBoxLayout(this);
    v->addWidget(mutedLabel(tr("A LoRA learns the character from 20–40 varied images: different views, framings, expressions and "
                               "poses, but always the same character on simple backgrounds. Generate the planned shots, add them "
                               "here, let the Critic score them and keep only the ones that look exactly right."),
                            this));
    auto *bar = new QHBoxLayout;
    auto *plan = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Plan training shots"), this);
    plan->setToolTip(tr("Puts 24 varied shots into the Create page's plan; generate them there"));
    plan->setEnabled(bool(m_planShots));
    auto *fromSession = new QPushButton(Icons::get(QStringLiteral("gallery")), tr("Add session results"), this);
    fromSession->setToolTip(tr("Adds the images generated in the current Create session"));
    fromSession->setEnabled(m_session != nullptr);
    auto *files = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Add images…"), this);
    m_score = new QPushButton(Icons::get(QStringLiteral("eye")), tr("Score with Critic"), this);
    m_score->setToolTip(tr("A vision model compares each image with the character's references"));
    m_minimum = new QSpinBox(this);
    m_minimum->setRange(1, 10);
    m_minimum->setValue(6);
    m_minimum->setPrefix(tr("reject below "));
    m_minimum->setSuffix(QStringLiteral("/10"));
    auto *dupes = new QPushButton(Icons::get(QStringLiteral("copy")), tr("Find near-duplicates"), this);
    bar->addWidget(plan);
    bar->addWidget(fromSession);
    bar->addWidget(files);
    bar->addSpacing(12);
    bar->addWidget(m_score);
    bar->addWidget(m_minimum);
    bar->addWidget(dupes);
    bar->addStretch();
    v->addLayout(bar);

    m_tabs = new QTabWidget(this);
    auto *split = new QSplitter(Qt::Horizontal, m_tabs);
    m_grid = new QListWidget(split);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setIconSize(QSize(kThumb, kThumb));
    m_grid->setGridSize(QSize(kThumb + 18, kThumb + 40));
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setUniformItemSizes(true);
    m_details = new QLabel(split);
    m_details->setWordWrap(true);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_details->setTextFormat(Qt::RichText);
    m_details->setMinimumWidth(240);
    m_details->setContentsMargins(12, 0, 0, 0);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    m_tabs->addTab(split, tr("Images"));

    // Captions.
    auto *captionPage = new QWidget(m_tabs);
    auto *cv = new QVBoxLayout(captionPage);
    cv->addWidget(mutedLabel(tr("Each image gets tags for what changes (view, framing, pose, expression, background). Tags of the "
                                "character's fixed look are left out, so the LoRA ties them to the trigger word."),
                             captionPage));
    auto *crow = new QHBoxLayout;
    crow->addWidget(new QLabel(tr("Trigger word"), captionPage));
    m_trigger = new QLineEdit(captionPage);
    m_trigger->setMaximumWidth(220);
    m_trigger->setToolTip(tr("A made-up word the model does not know yet; prompts use it to call up the character"));
    crow->addWidget(m_trigger);
    m_outfit = new QCheckBox(tr("Outfit is part of the character"), captionPage);
    m_outfit->setToolTip(tr("On: the outfit always comes with the character. Off: outfit tags stay in the captions, so prompts "
                            "can change the clothes."));
    crow->addWidget(m_outfit);
    crow->addStretch();
    m_captionButton = new QPushButton(Icons::get(QStringLiteral("text")), tr("Write captions"), captionPage);
    m_captionButton->setToolTip(tr("The vision model tags every kept image that has no tags yet"));
    Theme::setRole(m_captionButton, QStringLiteral("primary"));
    crow->addWidget(m_captionButton);
    cv->addLayout(crow);
    m_captions = new QTableWidget(0, 2, captionPage);
    m_captions->setObjectName(QStringLiteral("captions"));
    m_captions->setHorizontalHeaderLabels({tr("Image"), tr("Caption")});
    m_captions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_captions->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_captions->setColumnWidth(0, 92);
    m_captions->verticalHeader()->setVisible(false);
    m_captions->verticalHeader()->setDefaultSectionSize(92);
    m_captions->setIconSize(QSize(84, 84));
    m_captions->setWordWrap(true);
    cv->addWidget(m_captions, 1);
    m_tagSummary = mutedLabel(QString(), captionPage);
    m_tagSummary->setWordWrap(true);
    cv->addWidget(m_tagSummary);
    auto *edits = new QHBoxLayout;
    auto *replace = new QPushButton(tr("Find and replace…"), captionPage);
    auto *addTag = new QPushButton(Icons::get(QStringLiteral("plus")), tr("Add tag to all…"), captionPage);
    auto *removeTag = new QPushButton(Icons::get(QStringLiteral("minus")), tr("Remove tag from all…"), captionPage);
    auto *rebuild = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Rebuild from tags"), captionPage);
    rebuild->setToolTip(tr("Writes the captions again from the tags; hand-edited captions only get the new trigger word"));
    edits->addWidget(replace);
    edits->addWidget(addTag);
    edits->addWidget(removeTag);
    edits->addStretch();
    edits->addWidget(rebuild);
    cv->addLayout(edits);
    m_tabs->addTab(captionPage, tr("Captions"));
    m_tabs->addTab(buildTrainTab(), tr("Train"));
    v->addWidget(m_tabs, 1);

    connect(m_captionButton, &QPushButton::clicked, this, [this] {
        if (isCaptioning()) {
            m_captionQueue.clear();
            if (m_tagging)
                m_tagging->cancel();
            return;
        }
        captionAll();
    });
    connect(m_trigger, &QLineEdit::editingFinished, this, &LoraDatasetDialog::applySettings);
    connect(m_outfit, &QCheckBox::toggled, this, &LoraDatasetDialog::applySettings);
    connect(rebuild, &QPushButton::clicked, this, [this] { rebuildCaptions(); });
    connect(replace, &QPushButton::clicked, this, &LoraDatasetDialog::askReplace);
    connect(addTag, &QPushButton::clicked, this, &LoraDatasetDialog::askAddTag);
    connect(removeTag, &QPushButton::clicked, this, &LoraDatasetDialog::askRemoveTag);
    connect(m_captions, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_fillingCaptions || item->column() != 1)
            return;
        const QString caption = ai::joinTags(ai::splitTags(item->text()));
        const QString id = item->data(Qt::UserRole).toString();
        // Later: saving rebuilds the table, which must not happen inside its own signal.
        QMetaObject::invokeMethod(this, [this, id, caption] { m_dataset->setCaption(id, caption, true); }, Qt::QueuedConnection);
    });

    auto *selection = new QHBoxLayout;
    auto *keep = new QPushButton(Icons::get(QStringLiteral("check")), tr("Keep"), this);
    auto *reject = new QPushButton(Icons::get(QStringLiteral("close")), tr("Reject"), this);
    auto *remove = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Remove"), this);
    Theme::setRole(remove, QStringLiteral("danger"));
    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::RichText);
    auto *exportButton = new QPushButton(Icons::get(QStringLiteral("save")), tr("Export for training…"), this);
    exportButton->setToolTip(tr("Writes the kept images cropped to training sizes, with their captions, to a folder"));
    auto *close = new QPushButton(tr("Close"), this);
    selection->addWidget(keep);
    selection->addWidget(reject);
    selection->addWidget(remove);
    selection->addSpacing(12);
    selection->addWidget(m_status, 1);
    selection->addWidget(exportButton);
    selection->addWidget(close);
    v->addLayout(selection);

    connect(plan, &QPushButton::clicked, this, [this] {
        if (m_planShots)
            m_planShots();
    });
    connect(fromSession, &QPushButton::clicked, this, [this] {
        addSessionResults([this](int n) {
            m_studio->notify(n > 0 ? tr("Added %n image(s) from the session.", nullptr, n) : tr("No new images in the session."));
        });
    });
    connect(files, &QPushButton::clicked, this, &LoraDatasetDialog::addFiles);
    connect(m_score, &QPushButton::clicked, this, [this] {
        if (isScoring()) {
            m_scoreQueue.clear();
            if (m_critique)
                m_critique->cancel();
            return;
        }
        scoreAll();
    });
    connect(dupes, &QPushButton::clicked, this, [this] {
        const int n = m_dataset->markDuplicates();
        m_studio->notify(n > 0 ? tr("%n near-duplicate(s) rejected.", nullptr, n) : tr("No near-duplicates."));
    });
    connect(keep, &QPushButton::clicked, this, [this] { setSelectedKept(true); });
    connect(reject, &QPushButton::clicked, this, [this] { setSelectedKept(false); });
    connect(remove, &QPushButton::clicked, this, &LoraDatasetDialog::removeSelected);
    connect(exportButton, &QPushButton::clicked, this, &LoraDatasetDialog::exportImages);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_grid, &QListWidget::currentItemChanged, this, &LoraDatasetDialog::showCurrent);
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (const auto i = m_dataset->item(id))
            m_dataset->setKept(id, !i->kept);
    });
    connect(m_dataset, &doc::LoraDataset::changed, this, &LoraDatasetDialog::reload);
    m_dataset->load();
    // A trigger word from the character's name the first time.
    doc::DatasetSettings settings = m_dataset->settings();
    if (settings.trigger.isEmpty()) {
        settings.trigger = ai::suggestTrigger(entry ? entry->name : QString());
        m_dataset->setSettings(settings);
    }
    {
        QSignalBlocker b1(m_trigger);
        QSignalBlocker b2(m_outfit);
        m_trigger->setText(settings.trigger);
        m_outfit->setChecked(settings.outfitIsIdentity);
    }
    reloadCaptions();
    refreshTrainTab();
    refreshEarlierRuns();
    if (CharacterTraining *running = CharacterTraining::running(m_characterId))
        bindTraining(running);
    if (LoraEvaluation *testing = LoraEvaluation::running(m_characterId))
        bindEvaluation(testing);
}

LoraDatasetDialog::~LoraDatasetDialog()
{
    m_scoreQueue.clear();
    m_captionQueue.clear();
    if (m_critique)
        m_critique->cancel();
    if (m_tagging)
        m_tagging->cancel();
}

QPixmap LoraDatasetDialog::thumbnail(const QString &id)
{
    if (m_thumbs.contains(id))
        return m_thumbs.value(id);
    if (!m_loading.contains(id)) {
        m_loading << id;
        const QString path = m_dataset->imagePath(id);
        QPointer<LoraDatasetDialog> self(this);
        async::run([path] { return QImage(path).scaled(kThumb, kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation); })
            .then(this, [self, id](const QImage &img) {
                if (!self)
                    return;
                self->m_loading.removeAll(id);
                self->m_thumbs.insert(id, QPixmap::fromImage(img));
                self->reload();
            });
    }
    return {};
}

void LoraDatasetDialog::reload()
{
    const QString current = m_grid->currentItem() ? m_grid->currentItem()->data(Qt::UserRole).toString() : QString();
    const QStringList selected = selectedIds();
    QSignalBlocker block(m_grid);
    m_grid->clear();
    const QList<doc::DatasetItem> items = m_dataset->items();
    for (const doc::DatasetItem &i : items) {
        QString label = i.score >= 0 ? tr("%1/10").arg(i.score) : tr("not scored");
        if (!i.duplicateOf.isEmpty())
            label += QStringLiteral(" · ") + tr("duplicate");
        auto *item = new QListWidgetItem(label, m_grid);
        item->setData(Qt::UserRole, i.id);
        QPixmap pm = thumbnail(i.id);
        if (!pm.isNull() && !i.kept) {
            // Rejected images are dimmed and crossed out.
            QPixmap dim(pm.size());
            dim.fill(Qt::transparent);
            QPainter p(&dim);
            p.setOpacity(0.35);
            p.drawPixmap(0, 0, pm);
            p.setOpacity(1.0);
            p.setPen(QPen(Theme::palette().danger, 4));
            p.drawLine(8, 8, dim.width() - 8, dim.height() - 8);
            p.drawLine(dim.width() - 8, 8, 8, dim.height() - 8);
            p.end();
            pm = dim;
        }
        if (!pm.isNull())
            item->setIcon(QIcon(pm));
        if (!i.kept)
            item->setForeground(Theme::palette().textMuted);
        else if (i.score >= 0 && i.score < m_minimum->value())
            item->setForeground(Theme::palette().warning);
        item->setToolTip(i.shot.value(QStringLiteral("title")).toString());
        if (i.id == current)
            m_grid->setCurrentItem(item);
        if (selected.contains(i.id))
            item->setSelected(true);
    }
    block.unblock();
    const int kept = m_dataset->keptCount();
    const int scored = int(std::count_if(items.begin(), items.end(), [](const doc::DatasetItem &i) { return i.score >= 0; }));
    QString state;
    QColor color = Theme::palette().textMuted;
    if (kept >= kGoodImages) {
        state = tr("ready for training");
        color = Theme::palette().success;
    } else if (kept >= kMinImages) {
        state = tr("enough, more helps");
        color = Theme::palette().warning;
    } else {
        state = tr("%n more needed", nullptr, kMinImages - kept);
    }
    QString text = tr("<b>%1 kept</b> of %2 · %3 scored").arg(kept).arg(items.size()).arg(scored);
    text += QStringLiteral(" · <span style='color:%1'>%2</span>").arg(color.name(), state.toHtmlEscaped());
    if (isScoring())
        text += QStringLiteral(" · ") + tr("scoring, %n left", nullptr, int(m_scoreQueue.size()) + (m_scoring ? 1 : 0));
    m_status->setText(text);
    m_score->setText(isScoring() ? tr("Stop scoring") : tr("Score with Critic"));
    showCurrent();
    reloadCaptions();
}

QWidget *LoraDatasetDialog::buildTrainTab()
{
    auto *page = new QWidget(m_tabs);
    auto *v = new QVBoxLayout(page);
    v->addWidget(mutedLabel(tr("Lumen trains the LoRA with kohya sd-scripts on this computer. The GPU is used for the whole time: "
                               "the servers stop and start again afterwards. On an 8 GB GPU a set of 25 images takes about two hours."),
                            page));
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *trainerRow = new QHBoxLayout;
    m_trainer = new QLabel(page);
    m_installTrainer = new QPushButton(Icons::get(QStringLiteral("download")), tr("Install the trainer…"), page);
    trainerRow->addWidget(m_trainer, 1);
    trainerRow->addWidget(m_installTrainer);
    form->addRow(tr("Trainer"), trainerRow);
    m_checkpoint = new QComboBox(page);
    m_checkpoint->setToolTip(tr("The LoRA works with this model and others of its family; use the model you generate with"));
    form->addRow(tr("Base model"), m_checkpoint);
    m_preset = new QComboBox(page);
    m_preset->addItem(tr("8 GB GPU: U-Net only, rank 16"), int(runtime::VramTier::Small));
    m_preset->addItem(tr("12 GB GPU: U-Net only, rank 32, batch 2"), int(runtime::VramTier::Medium));
    m_preset->addItem(tr("16 GB GPU or more: with text encoders, rank 32, batch 2"), int(runtime::VramTier::Large));
    form->addRow(tr("Settings"), m_preset);
    m_epochs = new QSpinBox(page);
    m_epochs->setRange(1, 40);
    m_epochs->setValue(10);
    m_epochs->setToolTip(tr("Each epoch goes through all images; a LoRA file is saved after every epoch"));
    form->addRow(tr("Epochs"), m_epochs);
    v->addLayout(form);
    m_trainSummary = mutedLabel(QString(), page);
    m_trainSummary->setTextFormat(Qt::RichText);
    v->addWidget(m_trainSummary);
    auto *row = new QHBoxLayout;
    m_trainButton = new QPushButton(Icons::get(QStringLiteral("play")), tr("Start training"), page);
    Theme::setRole(m_trainButton, QStringLiteral("primary"));
    m_resumeButton = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Continue the stopped training"), page);
    row->addWidget(m_trainButton);
    row->addWidget(m_resumeButton);
    row->addStretch();
    v->addLayout(row);
    m_trainBar = new QProgressBar(page);
    m_trainBar->setRange(0, 1000);
    m_trainBar->setVisible(false);
    v->addWidget(m_trainBar);
    m_trainStatus = new QLabel(page);
    m_trainStatus->setWordWrap(true);
    m_trainStatus->setTextFormat(Qt::RichText);
    v->addWidget(m_trainStatus);
    auto *runsRow = new QHBoxLayout;
    m_earlierRuns = mutedLabel(QString(), page);
    m_removeRuns = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Remove earlier trainings…"), page);
    m_removeRuns->setToolTip(tr("Removes the files of earlier trainings of this character. LoRAs in the model library stay."));
    runsRow->addWidget(m_earlierRuns);
    runsRow->addWidget(m_removeRuns);
    runsRow->addStretch();
    v->addLayout(runsRow);
    connect(m_removeRuns, &QPushButton::clicked, this, &LoraDatasetDialog::removeEarlierRuns);

    // Test the result: the newest epochs against a baseline without the LoRA.
    v->addSpacing(8);
    v->addWidget(sectionLabel(tr("Test the result"), page));
    v->addWidget(mutedLabel(tr("The newest three epochs draw four new scenes with the LoRA; for comparison the same scenes are drawn "
                               "without it, with the reference image through IP-Adapter. The Critic scores how well each image keeps the "
                               "character (0–10). Takes about 45 minutes on an 8 GB GPU."),
                            page));
    auto *evalRow = new QHBoxLayout;
    m_evalButton = new QPushButton(Icons::get(QStringLiteral("check")), tr("Test the LoRA"), page);
    m_applyEval = new QPushButton(page);
    Theme::setRole(m_applyEval, QStringLiteral("primary"));
    evalRow->addWidget(m_evalButton);
    evalRow->addWidget(m_applyEval);
    evalRow->addStretch();
    v->addLayout(evalRow);
    m_evalStatus = new QLabel(page);
    m_evalStatus->setWordWrap(true);
    m_evalStatus->setTextFormat(Qt::RichText);
    v->addWidget(m_evalStatus);
    m_evalTable = new QTableWidget(page);
    m_evalTable->setObjectName(QStringLiteral("loraTest"));
    m_evalTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_evalTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_evalTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_evalTable->setIconSize(QSize(72, 72));
    m_evalTable->verticalHeader()->setDefaultSectionSize(80);
    m_evalTable->setMinimumHeight(140);
    v->addWidget(m_evalTable, 1);

    connect(m_evalButton, &QPushButton::clicked, this, [this] {
        if (m_evaluation) {
            m_evaluation->cancel();
            return;
        }
        startEvaluation();
    });
    connect(m_applyEval, &QPushButton::clicked, this, &LoraDatasetDialog::applyEvaluation);
    connect(m_evalTable, &QTableWidget::itemSelectionChanged, this, &LoraDatasetDialog::refreshEvaluation);
    connect(m_evalTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (QTableWidgetItem *item = m_evalTable->item(row, column)) {
            const QString image = item->data(Qt::UserRole + 2).toString();
            if (!image.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(image));
        }
    });

    connect(m_installTrainer, &QPushButton::clicked, this, [this] {
        InstallRuntimeDialog dlg(m_studio, std::nullopt, QStringLiteral("kohya"), this);
        dlg.exec();
    });
    connect(m_studio->runtimes(), &runtime::RuntimeStore::changed, this, &LoraDatasetDialog::refreshTrainTab);
    connect(m_preset, &QComboBox::currentIndexChanged, this, &LoraDatasetDialog::refreshTrainTab);
    connect(m_epochs, &QSpinBox::valueChanged, this, &LoraDatasetDialog::refreshTrainTab);
    connect(m_checkpoint, &QComboBox::currentIndexChanged, this, &LoraDatasetDialog::refreshTrainTab);
    connect(m_trainButton, &QPushButton::clicked, this, [this] {
        if (m_trainingJob && !m_trainingJob->isFinished()) {
            m_trainingJob->cancel();
            return;
        }
        startTraining(false);
    });
    connect(m_resumeButton, &QPushButton::clicked, this, [this] { startTraining(true); });

    // The base model: what the default image generator uses, else the library's checkpoints.
    const QString current = checkpointFile(m_studio, m_studio->providers()->defaultImageProvider());
    for (const runtime::ModelEntry &e : m_studio->models()->entries(runtime::ModelKind::Checkpoint)) {
        if (e.info.family != QLatin1String("SDXL") && e.info.family != QLatin1String("SD 1.5"))
            continue;
        QString label = e.name() + QStringLiteral("  —  ") + e.info.family;
        if (!e.info.variant.isEmpty())
            label += QStringLiteral(" · ") + e.info.variant;
        m_checkpoint->addItem(label, e.path);
        m_checkpoint->setItemData(m_checkpoint->count() - 1, QDir::toNativeSeparators(e.path), Qt::ToolTipRole);
    }
    if (!current.isEmpty() && m_checkpoint->findData(current) < 0 && QFileInfo::exists(current))
        m_checkpoint->insertItem(0, QFileInfo(current).completeBaseName(), current);
    m_checkpoint->setCurrentIndex(std::max(0, m_checkpoint->findData(current)));
    QPointer<LoraDatasetDialog> self(this);
    runtime::probeHardware().then(this, [self](const runtime::HardwareInfo &hw) {
        if (!self)
            return;
        self->m_preset->setCurrentIndex(std::max(0, self->m_preset->findData(int(runtime::vramTierFor(hw.maxVram())))));
    });
    return page;
}

void LoraDatasetDialog::refreshTrainTab()
{
    if (!m_trainer)
        return;
    const QList<runtime::InstalledRuntime> trainers = m_studio->runtimes()->runtimes(QStringLiteral("kohya"));
    m_trainer->setText(trainers.isEmpty() ? tr("Not installed (about 4 GB to download)") : trainers.last().versionInfo);
    m_installTrainer->setVisible(trainers.isEmpty());
    const int images = m_dataset->keptCount();
    const int repeats = runtime::repeatsFor(images);
    const runtime::TrainingPreset preset = runtime::trainingPreset(runtime::VramTier(m_preset->currentData().toInt()));
    const int steps = runtime::totalSteps(images, repeats, m_epochs->value(), preset.batch);
    // Measured on an 8 GB RTX 4060 (3.1 s per step); larger presets are estimates.
    const double secondsPerStep = m_preset->currentData().toInt() == int(runtime::VramTier::Small) ? 3.1 : 2.4;
    const int minutes = int(steps * secondsPerStep / 60.0) + 2;
    QString summary = tr("%1 images × %2 repeats × %3 epochs = <b>%4 steps</b>, about %5.")
                          .arg(images)
                          .arg(repeats)
                          .arg(m_epochs->value())
                          .arg(steps)
                          .arg(minutes >= 90 ? tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60) : tr("%1 min").arg(minutes));
    summary += QStringLiteral(" ") + tr("Trigger word: <b>%1</b>.").arg(m_dataset->settings().trigger.toHtmlEscaped());
    if (images < 15)
        summary += QStringLiteral("<br><span style='color:%1'>%2</span>")
                       .arg(Theme::palette().warning.name(), tr("Fewer than 15 images: the LoRA will learn little. Add more first.").toHtmlEscaped());
    m_trainSummary->setText(summary);
    const bool running = m_trainingJob && !m_trainingJob->isFinished();
    m_trainButton->setText(running ? tr("Stop") : tr("Start training"));
    m_trainButton->setEnabled(running || (!trainers.isEmpty() && images > 0 && m_checkpoint->count() > 0));
    m_resumeButton->setVisible(!running && CharacterTraining::canResume(CharacterTraining::lastRun(m_dataset->directory())));
    m_resumeButton->setEnabled(!trainers.isEmpty());
    refreshEvaluation();
}

void LoraDatasetDialog::refreshEarlierRuns()
{
    if (!m_earlierRuns)
        return;
    QStringList runs = CharacterTraining::runs(m_dataset->directory());
    if (!runs.isEmpty())
        runs.removeLast();
    qint64 bytes = 0;
    for (const QString &run : std::as_const(runs))
        bytes += CharacterTraining::folderSize(run);
    m_earlierRuns->setText(tr("%n earlier training(s), %1", nullptr, int(runs.size())).arg(QLocale().formattedDataSize(bytes)));
    m_earlierRuns->setVisible(!runs.isEmpty());
    m_removeRuns->setVisible(!runs.isEmpty());
}

void LoraDatasetDialog::removeEarlierRuns()
{
    const bool busy = (m_trainingJob && !m_trainingJob->isFinished()) || m_evaluation;
    if (busy
        || QMessageBox::question(this, tr("Remove earlier trainings"),
                                 tr("Remove the files of the earlier trainings (epochs, test images, logs)? The LoRAs in the model "
                                    "library and the newest training stay."))
               != QMessageBox::Yes)
        return;
    const qint64 freed = CharacterTraining::removeEarlierRuns(m_dataset->directory());
    m_studio->notify(tr("Earlier trainings removed: %1 freed.").arg(QLocale().formattedDataSize(freed)), QStringLiteral("success"));
    refreshEarlierRuns();
}

void LoraDatasetDialog::startEvaluation()
{
    const QString run = CharacterTraining::lastRun(m_dataset->directory());
    QString error;
    LoraEvaluation *e = run.isEmpty() ? nullptr : LoraEvaluation::start(m_studio, m_characterId, run, &error);
    if (!e) {
        m_evalStatus->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                                  .arg(Theme::palette().danger.name(), (error.isEmpty() ? tr("Train the LoRA first.") : error).toHtmlEscaped()));
        return;
    }
    bindEvaluation(e);
}

void LoraDatasetDialog::bindEvaluation(LoraEvaluation *evaluation)
{
    m_evaluation = evaluation;
    m_evalRun = evaluation->runDir();
    connect(evaluation, &LoraEvaluation::changed, this, &LoraDatasetDialog::refreshEvaluation);
    QPointer<LoraDatasetDialog> self(this);
    connect(evaluation, &LoraEvaluation::finished, this, [self](bool ok, const QString &message) {
        if (!self)
            return;
        self->m_evaluation = nullptr;
        self->refreshEvaluation();
        const QColor colour = ok ? Theme::palette().success : Theme::palette().danger;
        self->m_evalStatus->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(colour.name(), message.toHtmlEscaped()));
    });
    refreshEvaluation();
}

void LoraDatasetDialog::refreshEvaluation()
{
    if (!m_evalTable)
        return;
    const QString run = m_evaluation ? m_evaluation->runDir() : CharacterTraining::lastRun(m_dataset->directory());
    const bool training = m_trainingJob && !m_trainingJob->isFinished();
    const bool hasFiles = !run.isEmpty() && !LoraEvaluation::epochFiles(run).isEmpty();
    m_evalButton->setText(m_evaluation ? tr("Stop testing") : tr("Test the LoRA"));
    m_evalButton->setEnabled(m_evaluation || (hasFiles && !training));

    // The cells: of the running test, else of the last saved one.
    struct Row
    {
        int epoch;
        double weight;
    };
    QList<LoraEvaluation::Cell> cells;
    QList<QPair<QString, QString>> shots; // id, title
    int bestEpoch = 0;
    double bestWeight = 0.8;
    bool finished = false;
    if (m_evaluation) {
        cells = m_evaluation->cells();
        for (const QJsonObject &s : m_evaluation->shots())
            shots.append({s.value(QStringLiteral("id")).toString(), s.value(QStringLiteral("title")).toString()});
        if (!m_evaluation->isFinished())
            m_evalStatus->setText(m_evaluation->status().toHtmlEscaped());
    } else {
        const QJsonObject saved = run.isEmpty() ? QJsonObject() : LoraEvaluation::saved(run);
        for (const QJsonValue &v : saved.value(QStringLiteral("cells")).toArray()) {
            const QJsonObject o = v.toObject();
            LoraEvaluation::Cell c;
            c.epoch = o.value(QStringLiteral("epoch")).toInt();
            c.weight = o.value(QStringLiteral("weight")).toDouble();
            c.shot = o.value(QStringLiteral("shot")).toString();
            c.image = o.value(QStringLiteral("image")).toString();
            c.score = o.value(QStringLiteral("score")).toDouble(-1);
            c.critique = o.value(QStringLiteral("critique")).toObject();
            c.error = o.value(QStringLiteral("error")).toString();
            cells << c;
        }
        for (const QJsonValue &v : saved.value(QStringLiteral("shots")).toArray())
            shots.append({v[QStringLiteral("id")].toString(), v[QStringLiteral("title")].toString()});
        bestEpoch = saved.value(QStringLiteral("bestEpoch")).toInt();
        bestWeight = saved.value(QStringLiteral("bestWeight")).toDouble(0.8);
        finished = saved.value(QStringLiteral("finished")).toBool();
        if (m_evalRun != run) {
            m_evalRun = run;
            m_evalStatus->setText(finished ? saved.value(QStringLiteral("status")).toString().toHtmlEscaped() : QString());
        }
    }

    QList<Row> rows;
    for (const LoraEvaluation::Cell &c : std::as_const(cells)) {
        bool seen = false;
        for (const Row &r : std::as_const(rows))
            seen = seen || (r.epoch == c.epoch && qFuzzyCompare(r.weight + 1, c.weight + 1));
        if (!seen)
            rows.append({c.epoch, c.weight});
    }
    const int selected = m_evalTable->currentRow();
    {
        QSignalBlocker block(m_evalTable);
        m_evalTable->setVisible(!rows.isEmpty());
        m_evalTable->setColumnCount(int(shots.size()) + 1);
        QStringList headers;
        for (const auto &s : std::as_const(shots))
            headers << s.second;
        headers << tr("Mean");
        m_evalTable->setHorizontalHeaderLabels(headers);
        m_evalTable->setRowCount(int(rows.size()));
        QStringList rowHeaders;
        for (int r = 0; r < rows.size(); ++r) {
            const Row row = rows[r];
            rowHeaders << (row.epoch == 0 ? tr("Without LoRA") : tr("Epoch %1 · %2").arg(row.epoch).arg(row.weight));
            double sum = 0;
            int n = 0;
            for (int s = 0; s < shots.size(); ++s) {
                auto *item = new QTableWidgetItem;
                item->setTextAlignment(Qt::AlignCenter);
                for (const LoraEvaluation::Cell &c : std::as_const(cells)) {
                    if (c.epoch != row.epoch || !qFuzzyCompare(c.weight + 1, row.weight + 1) || c.shot != shots[s].first)
                        continue;
                    if (!c.image.isEmpty()) {
                        const QImage img(c.image);
                        if (!img.isNull())
                            item->setIcon(QIcon(QPixmap::fromImage(img.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
                        item->setData(Qt::UserRole + 2, c.image);
                    }
                    if (c.score >= 0) {
                        item->setText(QString::number(c.score, 'f', 1));
                        sum += c.score;
                        ++n;
                        QStringList tip{tr("Identity %1 · pose %2")
                                            .arg(c.critique.value(QStringLiteral("identity")).toDouble(), 0, 'f', 1)
                                            .arg(c.critique.value(QStringLiteral("pose")).toInt())};
                        // The checklist: ✓ match, ≈ close, ✗ different, – not visible.
                        const QJsonArray names = c.critique.value(QStringLiteral("checklist")).toArray();
                        const QJsonObject verdicts = c.critique.value(QStringLiteral("features")).toObject();
                        for (int f = 0; f < names.size(); ++f) {
                            const QString v = verdicts.value(QStringLiteral("f%1").arg(f + 1)).toString();
                            const QString mark = v == QLatin1String("match")       ? QStringLiteral("✓")
                                                 : v == QLatin1String("close")     ? QStringLiteral("≈")
                                                 : v == QLatin1String("different") ? QStringLiteral("✗")
                                                                                   : QStringLiteral("–");
                            tip << mark + QLatin1Char(' ') + names[f].toString();
                        }
                        for (const QJsonValue &i : c.critique.value(QStringLiteral("issues")).toArray())
                            tip << QStringLiteral("• ") + i.toString();
                        item->setToolTip(tip.join(QLatin1Char('\n')));
                    } else if (!c.error.isEmpty()) {
                        item->setText(tr("failed"));
                        item->setToolTip(c.error);
                    }
                }
                m_evalTable->setItem(r, s, item);
            }
            auto *mean = new QTableWidgetItem(n > 0 ? QString::number(sum / n, 'f', 1) : QString());
            mean->setTextAlignment(Qt::AlignCenter);
            QFont f = mean->font();
            f.setBold(true);
            mean->setFont(f);
            if (finished && row.epoch == bestEpoch && qFuzzyCompare(row.weight + 1, bestWeight + 1))
                mean->setForeground(Theme::palette().success);
            m_evalTable->setItem(r, int(shots.size()), mean);
        }
        m_evalTable->setVerticalHeaderLabels(rowHeaders);
        m_evalTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        if (selected >= 0 && selected < rows.size())
            m_evalTable->selectRow(selected);
    }

    // Apply: the selected row, else the recommendation.
    int epoch = finished ? bestEpoch : 0;
    double weight = bestWeight;
    const int current = m_evalTable->currentRow();
    if (current >= 0 && current < rows.size() && rows[current].epoch > 0 && m_evalTable->selectionModel()->hasSelection()) {
        epoch = rows[current].epoch;
        weight = rows[current].weight;
    }
    m_applyEval->setVisible(!m_evaluation && epoch > 0);
    m_applyEval->setText(epoch == bestEpoch && qFuzzyCompare(weight + 1, bestWeight + 1)
                             ? tr("Use epoch %1 at weight %2 (recommended)").arg(epoch).arg(weight)
                             : tr("Use epoch %1 at weight %2").arg(epoch).arg(weight));
    m_applyEval->setProperty("epoch", epoch);
    m_applyEval->setProperty("weight", weight);
}

void LoraDatasetDialog::applyEvaluation()
{
    const int epoch = m_applyEval->property("epoch").toInt();
    const double weight = m_applyEval->property("weight").toDouble();
    QString error;
    const QString file = LoraEvaluation::apply(m_studio, m_characterId, m_evalRun, epoch, weight, &error);
    if (file.isEmpty()) {
        m_evalStatus->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().danger.name(), error.toHtmlEscaped()));
        return;
    }
    m_evalStatus->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                              .arg(Theme::palette().success.name(),
                                   tr("Epoch %1 is attached to the character at weight %2: %3")
                                       .arg(epoch)
                                       .arg(weight)
                                       .arg(QDir::toNativeSeparators(file))
                                       .toHtmlEscaped()));
}

void LoraDatasetDialog::startTraining(bool resume)
{
    const QList<runtime::InstalledRuntime> trainers = m_studio->runtimes()->runtimes(QStringLiteral("kohya"));
    if (trainers.isEmpty())
        return;
    CharacterTraining::Settings s;
    s.characterId = m_characterId;
    s.runtimeId = trainers.last().id;
    s.checkpoint = m_checkpoint->currentData().toString();
    s.tier = runtime::VramTier(m_preset->currentData().toInt());
    s.epochs = m_epochs->value();
    s.resume = resume;
    QString error;
    CharacterTraining *t = CharacterTraining::start(m_studio, s, &error);
    if (!t) {
        m_trainStatus->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().danger.name(), error.toHtmlEscaped()));
        return;
    }
    bindTraining(t);
}

void LoraDatasetDialog::bindTraining(CharacterTraining *training)
{
    m_trainStatus->setText(tr("Preparing the images…"));
    m_trainBar->setVisible(true);
    m_trainBar->setValue(0);
    QPointer<LoraDatasetDialog> self(this);
    auto attach = [self](Job *job) {
        if (!self)
            return;
        self->m_trainingJob = job;
        connect(job, &Job::progressChanged, self, &LoraDatasetDialog::showTrainingJob);
        connect(job, &Job::stateChanged, self, &LoraDatasetDialog::showTrainingJob);
        self->showTrainingJob();
        self->refreshTrainTab();
    };
    if (training->job())
        attach(training->job());
    connect(training, &CharacterTraining::started, this, attach);
    connect(training, &CharacterTraining::finished, this, [self](bool ok, const QString &message, const QString &lora) {
        if (!self)
            return;
        self->m_trainBar->setVisible(ok);
        if (ok)
            self->m_trainBar->setValue(1000);
        const QColor colour = ok ? Theme::palette().success : Theme::palette().danger;
        QString text = QStringLiteral("<span style='color:%1'>%2</span>").arg(colour.name(), message.toHtmlEscaped());
        if (ok)
            text += QStringLiteral("<br>") + tr("File: %1").arg(QDir::toNativeSeparators(lora).toHtmlEscaped());
        self->m_trainStatus->setText(text);
        self->refreshTrainTab();
        self->refreshEarlierRuns();
    });
}

void LoraDatasetDialog::showTrainingJob()
{
    if (!m_trainingJob)
        return;
    const double p = m_trainingJob->progress();
    if (p < 0) {
        m_trainBar->setRange(0, 0);
    } else {
        m_trainBar->setRange(0, 1000);
        m_trainBar->setValue(int(p * 1000));
    }
    if (!m_trainingJob->isFinished())
        m_trainStatus->setText(m_trainingJob->statusText().toHtmlEscaped());
    refreshTrainTab();
}

void LoraDatasetDialog::reloadCaptions()
{
    if (!m_captions)
        return;
    m_fillingCaptions = true;
    const QList<doc::DatasetItem> items = m_dataset->items();
    QList<doc::DatasetItem> kept;
    for (const doc::DatasetItem &i : items) {
        if (i.kept)
            kept << i;
    }
    m_captions->setRowCount(int(kept.size()));
    QHash<QString, int> counts;
    int missing = 0;
    for (int row = 0; row < kept.size(); ++row) {
        const doc::DatasetItem &i = kept[row];
        auto *image = new QTableWidgetItem;
        image->setFlags(Qt::ItemIsEnabled);
        const QPixmap pm = thumbnail(i.id);
        if (!pm.isNull())
            image->setIcon(QIcon(pm));
        image->setToolTip(i.shot.value(QStringLiteral("title")).toString());
        m_captions->setItem(row, 0, image);
        auto *caption = new QTableWidgetItem(i.caption);
        caption->setData(Qt::UserRole, i.id);
        caption->setToolTip(i.tags.isEmpty() ? tr("Not tagged yet") : tr("All tags: %1").arg(i.tags.join(QStringLiteral(", "))));
        if (i.caption.isEmpty())
            ++missing;
        m_captions->setItem(row, 1, caption);
        for (const QString &t : ai::splitTags(i.caption))
            counts[t] += 1;
    }
    m_fillingCaptions = false;
    // The most common tags show what the captions say overall.
    QList<QPair<int, QString>> common;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        if (it.key() != m_dataset->settings().trigger)
            common.append({it.value(), it.key()});
    }
    std::sort(common.begin(), common.end(), [](const auto &a, const auto &b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    QStringList top;
    for (int k = 0; k < std::min<int>(14, int(common.size())); ++k)
        top << QStringLiteral("%1 (%2)").arg(common[k].second).arg(common[k].first);
    QString summary = missing > 0 ? tr("%n image(s) without a caption.", nullptr, missing) + QLatin1Char(' ') : QString();
    if (!top.isEmpty())
        summary += tr("Most common tags: %1").arg(top.join(QStringLiteral(", ")));
    if (isCaptioning())
        summary = tr("Writing captions, %n left…", nullptr, int(m_captionQueue.size()) + (m_captioning ? 1 : 0)) + QLatin1Char(' ') + summary;
    m_tagSummary->setText(summary);
    m_captionButton->setText(isCaptioning() ? tr("Stop") : tr("Write captions"));
}

void LoraDatasetDialog::applySettings()
{
    doc::DatasetSettings s = m_dataset->settings();
    const QString previous = s.trigger;
    const QString trigger = ai::normalizeTag(m_trigger->text()).replace(QLatin1Char(' '), QLatin1Char('_'));
    if (trigger.isEmpty()) {
        m_trigger->setText(previous);
        return;
    }
    if (trigger == previous && m_outfit->isChecked() == s.outfitIsIdentity)
        return;
    s.trigger = trigger;
    s.outfitIsIdentity = m_outfit->isChecked();
    m_trigger->setText(trigger);
    m_dataset->setSettings(s);
    rebuildCaptions(previous);
}

void LoraDatasetDialog::rebuildCaptions(const QString &previousTrigger)
{
    const doc::DatasetSettings s = m_dataset->settings();
    for (const doc::DatasetItem &i : m_dataset->items()) {
        if (i.captionEdited) {
            QStringList tags = ai::splitTags(i.caption);
            if (!previousTrigger.isEmpty())
                tags.removeAll(ai::normalizeTag(previousTrigger));
            tags.removeAll(s.trigger);
            tags.prepend(s.trigger);
            m_dataset->setCaption(i.id, ai::joinTags(tags), true);
        } else if (!i.tags.isEmpty()) {
            m_dataset->setCaption(i.id, ai::buildCaption(i.tags, m_sheet, s.trigger, s.outfitIsIdentity), false);
        }
    }
}

void LoraDatasetDialog::captionAll()
{
    m_captionQueue.clear();
    for (const doc::DatasetItem &i : m_dataset->items()) {
        if (i.kept && i.tags.isEmpty())
            m_captionQueue << i.id;
    }
    if (m_captionQueue.isEmpty()) {
        rebuildCaptions();
        m_studio->notify(tr("Every kept image is tagged; the captions were rebuilt."));
        return;
    }
    captionNext();
}

void LoraDatasetDialog::captionNext()
{
    if (m_captioning)
        return;
    if (m_captionQueue.isEmpty()) {
        reloadCaptions();
        return;
    }
    const QString id = m_captionQueue.takeFirst();
    if (!m_dataset->item(id)) {
        captionNext();
        return;
    }
    m_captioning = true;
    reloadCaptions();
    ai::StructuredChatOperation *op = m_studio->director()->tagImage(QImage(m_dataset->imagePath(id)));
    m_tagging = op;
    QPointer<LoraDatasetDialog> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, id] {
        if (!self)
            return;
        self->m_captioning = false;
        if (op->state() == ai::Operation::State::Succeeded) {
            QStringList tags;
            for (const QJsonValue &v : op->result().value(QStringLiteral("tags")).toArray()) {
                const QString t = ai::modelTag(v.toString());
                if (!t.isEmpty() && !tags.contains(t))
                    tags << t;
            }
            const doc::DatasetSettings s = self->m_dataset->settings();
            self->m_dataset->setTags(id, tags);
            self->m_dataset->setCaption(id, ai::buildCaption(tags, self->m_sheet, s.trigger, s.outfitIsIdentity), false);
        } else if (op->state() == ai::Operation::State::Failed) {
            self->m_captionQueue.clear();
            self->m_studio->notify(tr("Writing captions failed: %1").arg(op->errorString()), QStringLiteral("error"));
        }
        self->captionNext();
    });
}

void LoraDatasetDialog::askReplace()
{
    bool ok = false;
    const QString from = ai::normalizeTag(QInputDialog::getText(this, tr("Find and replace"), tr("Find tag:"), QLineEdit::Normal, QString(), &ok));
    if (!ok || from.isEmpty())
        return;
    const QString to = ai::normalizeTag(QInputDialog::getText(this, tr("Find and replace"), tr("Replace \"%1\" with (empty removes it):").arg(from),
                                                              QLineEdit::Normal, QString(), &ok));
    if (!ok)
        return;
    m_dataset->updateCaptions([from, to](const doc::DatasetItem &i) {
        QStringList tags = ai::splitTags(i.caption);
        const qsizetype at = tags.indexOf(from);
        if (at < 0)
            return i.caption;
        if (to.isEmpty() || tags.contains(to))
            tags.removeAt(at);
        else
            tags[at] = to;
        return ai::joinTags(tags);
    });
}

void LoraDatasetDialog::askAddTag()
{
    bool ok = false;
    const QString tag = ai::normalizeTag(QInputDialog::getText(this, tr("Add tag"), tr("Tag to add to every caption:"), QLineEdit::Normal, QString(), &ok));
    if (!ok || tag.isEmpty())
        return;
    m_dataset->updateCaptions([tag](const doc::DatasetItem &i) {
        QStringList tags = ai::splitTags(i.caption);
        if (!tags.contains(tag))
            tags << tag;
        return ai::joinTags(tags);
    });
}

void LoraDatasetDialog::askRemoveTag()
{
    bool ok = false;
    const QString tag = ai::normalizeTag(QInputDialog::getText(this, tr("Remove tag"), tr("Tag to remove from every caption:"), QLineEdit::Normal, QString(), &ok));
    if (!ok || tag.isEmpty())
        return;
    m_dataset->updateCaptions([tag](const doc::DatasetItem &i) {
        QStringList tags = ai::splitTags(i.caption);
        tags.removeAll(tag);
        return ai::joinTags(tags);
    });
}

void LoraDatasetDialog::showCurrent()
{
    QListWidgetItem *item = m_grid->currentItem();
    const auto i = item ? m_dataset->item(item->data(Qt::UserRole).toString()) : std::nullopt;
    if (!i) {
        m_details->setText(m_dataset->items().isEmpty()
                               ? tr("No images yet.<br><br>1. <b>Plan training shots</b> fills the Create page with 24 shots.<br>"
                                    "2. Generate them there (two variants per shot work well).<br>3. <b>Add session results</b>.<br>"
                                    "4. <b>Score with Critic</b>, then check the rest yourself.")
                               : tr("Select an image. Double-click keeps or rejects it."));
        return;
    }
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg((i->shot.value(QStringLiteral("title")).toString().isEmpty() ? tr("Imported image")
                                                                                                      : i->shot.value(QStringLiteral("title")).toString())
                                                  .toHtmlEscaped());
    lines << tr("%1 × %2 px").arg(i->size.width()).arg(i->size.height());
    lines << (i->kept ? tr("Kept for training") : tr("Rejected"));
    if (!i->duplicateOf.isEmpty())
        lines << tr("Nearly the same as a better image.");
    if (i->score >= 0) {
        lines << tr("Critic: %1/10 (identity %2, overall %3)")
                     .arg(i->score)
                     .arg(i->critique.value(QStringLiteral("identity")).toInt())
                     .arg(i->critique.value(QStringLiteral("overall")).toInt());
        QStringList issues;
        for (const QJsonValue &v : i->critique.value(QStringLiteral("issues")).toArray())
            issues << QStringLiteral("• ") + v.toString().toHtmlEscaped();
        if (!issues.isEmpty())
            lines << issues.join(QStringLiteral("<br>"));
    }
    m_details->setText(lines.join(QStringLiteral("<br>")));
}

QStringList LoraDatasetDialog::selectedIds() const
{
    QStringList out;
    for (QListWidgetItem *item : m_grid->selectedItems())
        out << item->data(Qt::UserRole).toString();
    return out;
}

void LoraDatasetDialog::setSelectedKept(bool kept)
{
    for (const QString &id : selectedIds())
        m_dataset->setKept(id, kept);
}

void LoraDatasetDialog::removeSelected()
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty()
        || QMessageBox::question(this, tr("Remove"), tr("Remove %n image(s) from the training set?", nullptr, int(ids.size()))) != QMessageBox::Yes)
        return;
    for (const QString &id : ids) {
        m_thumbs.remove(id);
        m_dataset->remove(id);
    }
}

void LoraDatasetDialog::addSessionResults(std::function<void(int)> done)
{
    doc::Project *project = m_studio->project();
    if (!m_session || !project) {
        if (done)
            done(0);
        return;
    }
    struct Pending
    {
        QString asset;
        QJsonObject shot;
        QJsonObject critique;
    };
    QList<Pending> pending;
    for (const ShotState &s : m_session->shots()) {
        for (const QString &asset : s.results) {
            if (!m_dataset->hasAsset(asset))
                pending.append({asset, s.shot, s.critiques.value(asset).toObject()});
        }
    }
    if (pending.isEmpty()) {
        if (done)
            done(0);
        return;
    }
    auto remaining = std::make_shared<int>(int(pending.size()));
    auto added = std::make_shared<int>(0);
    QPointer<LoraDatasetDialog> self(this);
    const QString sessionId = m_session->id();
    for (const Pending &p : std::as_const(pending)) {
        project->loadImage(p.asset).then(this, [self, p, remaining, added, done, sessionId](const QImage &image) {
            if (!self)
                return;
            auto finish = [self, remaining, added, done] {
                if (--*remaining == 0 && done)
                    done(*added);
            };
            if (image.isNull()) {
                finish();
                return;
            }
            const QJsonObject source{{QStringLiteral("asset"), p.asset}, {QStringLiteral("session"), sessionId},
                                     {QStringLiteral("shot"), p.shot.value(QStringLiteral("id"))}};
            self->m_dataset->add(image, source, p.shot).then(self, [self, p, added, finish](const doc::DatasetItem &item) {
                // A Critic review from the Create page counts here too.
                if (self && !p.critique.isEmpty())
                    self->m_dataset->setCritique(item.id, datasetScore(p.critique), p.critique);
                ++*added;
                finish();
            });
        });
    }
}

void LoraDatasetDialog::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add images"), QString(), tr("Images (*.png *.jpg *.jpeg *.webp)"));
    for (const QString &f : files) {
        const QImage image(f);
        if (!image.isNull())
            m_dataset->add(image, {{QStringLiteral("file"), f}});
    }
}

void LoraDatasetDialog::scoreAll()
{
    m_scoreQueue.clear();
    for (const doc::DatasetItem &i : m_dataset->items()) {
        if (i.score < 0 && i.kept)
            m_scoreQueue << i.id;
    }
    if (m_scoreQueue.isEmpty()) {
        m_studio->notify(tr("Every kept image has a score."));
        return;
    }
    if (!m_referencesLoaded) {
        QPointer<LoraDatasetDialog> self(this);
        m_scoring = true;
        reload();
        m_studio->characters()->loadReferences(m_characterId).then(this, [self](const QList<QImage> &refs) {
            if (!self)
                return;
            self->m_references = refs;
            self->m_referencesLoaded = true;
            self->m_scoring = false;
            self->scoreNext();
        });
        return;
    }
    scoreNext();
}

void LoraDatasetDialog::scoreNext()
{
    if (m_scoring)
        return;
    if (m_scoreQueue.isEmpty()) {
        reload();
        return;
    }
    const QString id = m_scoreQueue.takeFirst();
    const auto item = m_dataset->item(id);
    if (!item) {
        scoreNext();
        return;
    }
    m_scoring = true;
    reload();
    const QImage image(m_dataset->imagePath(id));
    ai::StructuredChatOperation *op = m_studio->director()->critiqueShot(image, item->shot, m_sheet, m_references);
    m_critique = op;
    QPointer<LoraDatasetDialog> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, id] {
        if (!self)
            return;
        self->m_scoring = false;
        if (op->state() == ai::Operation::State::Succeeded) {
            const QJsonObject critique = op->result();
            const int score = datasetScore(critique);
            self->m_dataset->setCritique(id, score, critique);
            if (score < self->m_minimum->value())
                self->m_dataset->setKept(id, false);
        } else if (op->state() == ai::Operation::State::Failed) {
            self->m_scoreQueue.clear();
            self->m_studio->notify(tr("The Critic failed: %1").arg(op->errorString()), QStringLiteral("error"));
        }
        self->scoreNext();
    });
}

void LoraDatasetDialog::exportImages()
{
    if (m_dataset->keptCount() == 0) {
        m_studio->notify(tr("Keep some images first."), QStringLiteral("warning"));
        return;
    }
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Export training images to"));
    if (folder.isEmpty())
        return;
    QPointer<LoraDatasetDialog> self(this);
    m_dataset->exportForTraining(folder, QStringLiteral("SDXL"))
        .then(this, [self, folder](int n) {
            if (self)
                self->m_studio->notify(tr("Exported %n training image(s) to %1.", nullptr, n).arg(QDir::toNativeSeparators(folder)),
                                       QStringLiteral("success"));
        })
        .onFailed(this, [self](const std::exception &e) {
            if (self)
                self->m_studio->notify(describeException(e), QStringLiteral("error"));
        });
}

} // namespace lumen::ui
