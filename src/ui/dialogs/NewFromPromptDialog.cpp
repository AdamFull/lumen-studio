#include "ui/dialogs/NewFromPromptDialog.h"

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/PromptStyle.h"
#include "core/JobQueue.h"
#include "core/Settings.h"
#include "doc/Project.h"
#include "pose/PoseRenderer.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/pages/AiPanel.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/ProviderCombo.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaMethod>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

namespace {

const QList<QPair<QString, double>> kAspects = {{QStringLiteral("1:1"), 1.0},          {QStringLiteral("2:3"), 2.0 / 3.0},
                                                {QStringLiteral("3:4"), 0.75},         {QStringLiteral("9:16"), 9.0 / 16.0},
                                                {QStringLiteral("3:2"), 1.5},          {QStringLiteral("4:3"), 4.0 / 3.0},
                                                {QStringLiteral("16:9"), 16.0 / 9.0}};
// The last choices, for the next new image.
const QString kAspectKey = QStringLiteral("newFromPrompt/aspect");
const QString kVariantsKey = QStringLiteral("newFromPrompt/variants");

} // namespace

NewFromPromptDialog::NewFromPromptDialog(Studio *studio, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
{
    setWindowTitle(tr("New from prompt"));
    setMinimumWidth(520);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(10);

    v->addWidget(sectionLabel(tr("Describe the image"), this));
    m_prompt = new AutoTextEdit(this, 3, 8);
    m_prompt->setAccessibleName(tr("Prompt"));
    m_prompt->setPlaceholderText(tr("A lighthouse on a cliff at sunset, painted in watercolour…"));
    v->addWidget(m_prompt);

    v->addWidget(sectionLabel(tr("Size"), this));
    auto *chips = new QWidget(this);
    auto *flow = new FlowLayout(chips, 4, 4);
    flow->setContentsMargins(0, 0, 0, 0);
    m_aspects = new QButtonGroup(this);
    for (int i = 0; i < kAspects.size(); ++i) {
        auto *chip = new QToolButton(chips);
        chip->setText(kAspects[i].first);
        chip->setCheckable(true);
        chip->setProperty("role", QStringLiteral("chip"));
        chip->setToolTip(kAspects[i].second == 1.0 ? tr("Square")
                         : kAspects[i].second < 1.0 ? tr("Portrait (%1)").arg(kAspects[i].first)
                                                    : tr("Landscape (%1)").arg(kAspects[i].first));
        m_aspects->addButton(chip, i);
        flow->addWidget(chip);
    }
    const int aspect = std::clamp(Settings::instance().value(kAspectKey, 0).toInt(), 0, int(kAspects.size()) - 1);
    m_aspects->button(aspect)->setChecked(true);
    v->addWidget(chips);
    auto *sizeRow = new QHBoxLayout;
    m_custom = new QCheckBox(tr("Custom size"), this);
    m_width = new QSpinBox(this);
    m_height = new QSpinBox(this);
    m_width->setAccessibleName(tr("Width"));
    m_height->setAccessibleName(tr("Height"));
    for (QSpinBox *s : {m_width, m_height}) {
        s->setRange(64, 4096);
        s->setSingleStep(64);
        s->setSuffix(QStringLiteral(" px"));
        s->setValue(1024);
        s->setEnabled(false);
    }
    sizeRow->addWidget(m_custom);
    sizeRow->addWidget(m_width);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), this));
    sizeRow->addWidget(m_height);
    sizeRow->addStretch();
    v->addLayout(sizeRow);
    m_sizeLabel = mutedLabel(QString(), this);
    v->addWidget(m_sizeLabel);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 6, 0, 0);
    m_variants = new QSpinBox(this);
    m_variants->setRange(1, 4);
    m_variants->setValue(std::clamp(Settings::instance().value(kVariantsKey, 2).toInt(), 1, 4));
    m_variants->setToolTip(tr("How many images to make. They are shown over the new document one at a time, and you keep the one you like."));
    form->addRow(tr("Variants"), m_variants);
    m_engine = new ProviderCombo(studio->providers(), ai::ProviderKind::Image, this);
    m_engine->setObjectName(QStringLiteral("imageEngine"));
    m_engine->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addRow(tr("Image engine"), m_engine);
    v->addLayout(form);
    m_engineNote = mutedLabel(QString(), this);
    v->addWidget(m_engineNote);

    // A pose from the Poses plugin ("New image with this pose").
    m_poseBox = new QWidget(this);
    auto *ph = new QHBoxLayout(m_poseBox);
    ph->setContentsMargins(0, 0, 0, 0);
    m_poseThumb = new QLabel(m_poseBox);
    m_poseThumb->setFixedSize(48, 64);
    m_poseThumb->setAlignment(Qt::AlignCenter);
    m_poseThumb->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg(Theme::palette().canvas.name()));
    ph->addWidget(m_poseThumb);
    m_poseNote = mutedLabel(QString(), m_poseBox);
    ph->addWidget(m_poseNote, 1);
    auto *clearPose = new QPushButton(Icons::get(QStringLiteral("close")), tr("No pose"), m_poseBox);
    clearPose->setToolTip(tr("Make the image without this pose"));
    ph->addWidget(clearPose);
    m_poseBox->hide();
    v->addWidget(m_poseBox);

    // Settings most people never need.
    m_moreButton = new QToolButton(this);
    m_moreButton->setText(tr("More options"));
    m_moreButton->setCheckable(true);
    m_moreButton->setAutoRaise(true);
    m_moreButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_moreButton->setArrowType(Qt::RightArrow);
    v->addWidget(m_moreButton);
    m_more = new QWidget(this);
    auto *mv = new QVBoxLayout(m_more);
    mv->setContentsMargins(0, 0, 0, 0);
    m_negativeBox = new QWidget(m_more);
    auto *nv = new QVBoxLayout(m_negativeBox);
    nv->setContentsMargins(0, 0, 0, 0);
    nv->addWidget(sectionLabel(tr("Leave out"), m_negativeBox));
    m_negative = new AutoTextEdit(m_negativeBox, 2, 4);
    m_negative->setAccessibleName(tr("Negative prompt"));
    m_negative->setPlaceholderText(tr("Things that should not be in the image, e.g. text, watermark (negative prompt)"));
    nv->addWidget(m_negative);
    mv->addWidget(m_negativeBox);
    auto *seedRow = new QHBoxLayout;
    seedRow->addWidget(new QLabel(tr("Seed"), m_more));
    m_seed = new QSpinBox(m_more);
    m_seed->setRange(0, 2147483647);
    m_seed->setAccessibleName(tr("Seed"));
    m_seed->setToolTip(tr("The same seed, prompt and size give the same image again."));
    m_seed->setEnabled(false);
    m_randomSeed = new QCheckBox(tr("Random"), m_more);
    m_randomSeed->setChecked(true);
    seedRow->addWidget(m_seed, 1);
    seedRow->addWidget(m_randomSeed);
    mv->addLayout(seedRow);
    m_more->hide();
    v->addWidget(m_more);
    v->addStretch();

    auto *buttons = new QDialogButtonBox(this);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    m_create->setIcon(Icons::get(QStringLiteral("sparkles")));
    m_create->setToolTip(tr("Make a new document of this size and draw the image into it (Ctrl+Enter)"));
    m_create->setDefault(true);
    Theme::setRole(m_create, QStringLiteral("primary"));
    buttons->addButton(QDialogButtonBox::Cancel);
    v->addWidget(buttons);

    connect(m_aspects, &QButtonGroup::idClicked, this, &NewFromPromptDialog::updateSize);
    connect(m_custom, &QCheckBox::toggled, this, [this](bool on) {
        m_width->setEnabled(on);
        m_height->setEnabled(on);
        for (QAbstractButton *b : m_aspects->buttons())
            b->setEnabled(!on);
        updateSize();
    });
    connect(m_width, &QSpinBox::valueChanged, this, &NewFromPromptDialog::updateSize);
    connect(m_height, &QSpinBox::valueChanged, this, &NewFromPromptDialog::updateSize);
    connect(m_randomSeed, &QCheckBox::toggled, m_seed, [this](bool on) { m_seed->setEnabled(!on); });
    connect(m_moreButton, &QToolButton::toggled, this, &NewFromPromptDialog::showMore);
    connect(clearPose, &QPushButton::clicked, this, [this] { setPose(std::nullopt); });
    connect(m_engine, &ProviderCombo::providerChanged, this, [this] {
        updateUi();
        updateSize();
    });
    connect(m_prompt, &QPlainTextEdit::textChanged, this, &NewFromPromptDialog::updateUi);
    connect(m_prompt, &AutoTextEdit::submitted, m_create, &QPushButton::click);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (create())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateUi();
    updateSize();
    m_prompt->setFocus();
}

void NewFromPromptDialog::setPrompt(const QString &prompt, const QString &negative)
{
    m_prompt->setPlainText(prompt);
    m_negative->setPlainText(negative);
    if (!negative.isEmpty())
        m_moreButton->setChecked(true);
}

void NewFromPromptDialog::fillFrom(const doc::AssetInfo &asset)
{
    const QJsonObject m = asset.metadata;
    // The words the user wrote (without what Lumen added) when known.
    setPrompt(m.value(m.contains(QStringLiteral("user_prompt")) ? QStringLiteral("user_prompt") : QStringLiteral("prompt")).toString(),
              m.value(QStringLiteral("negative_prompt")).toString());
    const int w = m.value(QStringLiteral("width")).toInt(asset.size.width());
    const int h = m.value(QStringLiteral("height")).toInt(asset.size.height());
    if (w > 0 && h > 0) {
        m_custom->setChecked(true);
        m_width->setValue(w);
        m_height->setValue(h);
    }
    const qint64 seed = qint64(m.value(QStringLiteral("seed")).toDouble(-1));
    if (seed >= 0) {
        m_randomSeed->setChecked(false);
        m_seed->setValue(int(std::min<qint64>(seed, m_seed->maximum())));
        m_moreButton->setChecked(true);
    }
}

void NewFromPromptDialog::setPose(const std::optional<pose::Pose> &pose)
{
    m_pose = pose;
    if (pose && pose->canvas.width() > 0 && pose->canvas.height() > 0 && !m_custom->isChecked()) {
        // The aspect ratio closest to the pose's canvas, so the people keep their proportions.
        const double ratio = double(pose->canvas.width()) / pose->canvas.height();
        int best = 0;
        for (int i = 1; i < kAspects.size(); ++i) {
            if (std::abs(std::log(kAspects[i].second / ratio)) < std::abs(std::log(kAspects[best].second / ratio)))
                best = i;
        }
        m_aspects->button(best)->setChecked(true);
    }
    updateSize();
    updateUi();
}

QSize NewFromPromptDialog::requestedSize() const
{
    if (m_custom->isChecked())
        return QSize(m_width->value(), m_height->value());
    ai::ImageProvider *p = m_engine->imageProvider();
    const qint64 pixels = p ? p->capabilities().nativePixels : 1024 * 1024;
    const int multiple = p ? std::max(8, p->capabilities().sizeMultiple) : 64;
    const int id = std::max(0, m_aspects->checkedId());
    return ai::Director::sizeForAspect(kAspects[id].first, pixels, std::max(multiple, 64));
}

void NewFromPromptDialog::updateSize()
{
    const QSize s = requestedSize();
    // The fields show the size of the chosen shape, and start from it for a custom size.
    if (!m_custom->isChecked()) {
        const QSignalBlocker bw(m_width), bh(m_height);
        m_width->setValue(s.width());
        m_height->setValue(s.height());
    }
    QString text;
    if (ai::ImageProvider *p = m_engine->imageProvider()) {
        const ai::ImageCapabilities caps = p->capabilities();
        if (!caps.fixedSizes.isEmpty())
            text = tr("The engine draws the closest size it supports; the image is scaled to this size.");
        else if (!caps.aspectRatios.isEmpty())
            text = tr("The engine draws the closest shape it supports; the image is scaled to this size.");
    }
    m_sizeLabel->setText(text);
    m_sizeLabel->setVisible(!text.isEmpty());
    if (m_pose) {
        pose::Pose fitted = *m_pose;
        fitted.resizeCanvas(s, true);
        const QImage map = pose::renderOpenPose(fitted, fitted.canvas);
        m_poseThumb->setPixmap(QPixmap::fromImage(map.scaled(m_poseThumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
}

void NewFromPromptDialog::showMore(bool on)
{
    m_moreButton->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    m_more->setVisible(on);
    adjustSize();
}

void NewFromPromptDialog::updateUi()
{
    ai::ImageProvider *provider = m_engine->imageProvider();
    const ai::ImageCapabilities caps = provider ? provider->capabilities() : ai::ImageCapabilities();
    m_negativeBox->setVisible(takesNegative());
    m_poseBox->setVisible(m_pose.has_value());
    if (m_pose)
        m_poseNote->setText(provider && !caps.poseControl
                                ? tr("This engine cannot follow a pose; the pose is kept with the new document.")
                                : tr("The image follows this pose."));
    QString note;
    if (!provider)
        note = tr("No image engine is set up yet. Help → Set up AI… connects one on this computer or online.");
    m_engineNote->setText(note);
    m_engineNote->setVisible(!note.isEmpty());
    m_create->setEnabled(provider && !m_prompt->text().trimmed().isEmpty());
}

bool NewFromPromptDialog::takesNegative() const
{
    // Only models that take one (FLUX and most online services do not).
    ai::ImageProvider *provider = m_engine->imageProvider();
    return provider && provider->capabilities().negativePrompt && ai::promptStyleFor(provider).supportsNegative;
}

doc::Document *NewFromPromptDialog::create()
{
    ai::ImageProvider *provider = m_engine->imageProvider();
    const QString prompt = m_prompt->text().trimmed();
    if (!provider || prompt.isEmpty())
        return nullptr;
    const ai::ImageCapabilities caps = provider->capabilities();
    const ai::PromptStyle style = ai::promptStyleFor(provider);
    const QSize size = requestedSize();
    if (!m_custom->isChecked())
        Settings::instance().setValue(kAspectKey, m_aspects->checkedId());
    Settings::instance().setValue(kVariantsKey, m_variants->value());

    ai::GenerationSpec spec;
    spec.providerId = provider->instanceId();
    ai::ImageRequest &r = spec.request;
    r.prompt = style.apply(prompt);
    r.negativePrompt = takesNegative() ? m_negative->text().trimmed() : QString();
    if (r.negativePrompt.isEmpty() && style.supportsNegative)
        r.negativePrompt = style.defaultNegative;
    r.size = size;
    r.batchCount = m_variants->value();
    r.seed = m_randomSeed->isChecked() ? qint64(QRandomGenerator::global()->bounded(2147483647)) : m_seed->value();
    // Steps, guidance and the sampler come from the model (its preset) and the provider.
    std::optional<pose::Pose> fitted = m_pose;
    if (fitted) {
        fitted->resizeCanvas(size, true);
        if (caps.poseControl) {
            ai::ControlInput c;
            c.type = ai::ControlType::OpenPose;
            c.image = pose::renderOpenPose(*fitted, size);
            r.controls << c;
        }
    }
    spec.title = prompt.left(60);
    spec.meta = QJsonObject{{QStringLiteral("source"), QStringLiteral("new_from_prompt")}, {QStringLiteral("user_prompt"), prompt}};

    const QString name = prompt.simplified().left(40);
    doc::Document *doc = m_studio->newDocument(size, name);
    if (fitted)
        doc->setControlPose(fitted);
    Job *job = m_studio->generation()->submit(spec);
    if (isSignalConnected(QMetaMethod::fromSignal(&NewFromPromptDialog::generationStarted))) {
        emit generationStarted(doc, job, name, r.batchCount);
        return doc;
    }
    QPointer<doc::Document> target(doc);
    Studio *studio = m_studio;
    connect(job, &Job::finished, studio, [studio, target, name](Job *j) {
        if (j->state() == Job::State::Succeeded) {
            if (!target)
                return;
            const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
            QJsonObject meta{{QStringLiteral("generated"), true}};
            if (!out.assets.isEmpty())
                meta.insert(QStringLiteral("asset"), out.assets.first().id);
            AiPanel::addResultLayers(studio, target, out.images, name, meta);
        } else if (j->state() == Job::State::Failed) {
            studio->notify(j->errorString(), QStringLiteral("error"));
        }
    });
    return doc;
}

} // namespace lumen::ui
