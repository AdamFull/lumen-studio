#include "ui/dialogs/LoraDatasetDialog.h"

#include "ai/Director.h"
#include "ai/StructuredChat.h"
#include "core/Async.h"
#include "core/Error.h"
#include "doc/CharacterLibrary.h"
#include "doc/Project.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/director/DirectorSession.h"
#include "ui/widgets/Controls.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
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

    auto *split = new QSplitter(Qt::Horizontal, this);
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
    v->addWidget(split, 1);

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
}

LoraDatasetDialog::~LoraDatasetDialog()
{
    m_scoreQueue.clear();
    if (m_critique)
        m_critique->cancel();
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
