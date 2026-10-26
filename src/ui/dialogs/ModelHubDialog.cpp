#include "ui/dialogs/ModelHubDialog.h"

#include "core/Error.h"
#include "core/Job.h"
#include "core/Paths.h"
#include "core/SecretStore.h"
#include "net/HttpClient.h"
#include "runtime/ModelLibrary.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/ModelsPage.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lumen::ui {

using runtime::HubFile;
using runtime::HubModel;
using runtime::HubSource;
using runtime::HubVersion;

namespace {

QString countText(qint64 n)
{
    if (n >= 1000000)
        return QStringLiteral("%1M").arg(double(n) / 1e6, 0, 'f', 1);
    if (n >= 1000)
        return QStringLiteral("%1k").arg(double(n) / 1e3, 0, 'f', 0);
    return QString::number(n);
}

// Sorts the downloads and size columns by number.
class NumberItem : public QTreeWidgetItem
{
public:
    NumberItem(QTreeWidget *tree, const QStringList &texts, int numberColumn)
        : QTreeWidgetItem(tree, texts)
        , m_column(numberColumn)
    {
    }
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == m_column)
            return data(column, Qt::UserRole + 1).toDouble() < other.data(column, Qt::UserRole + 1).toDouble();
        return text(column).compare(other.text(column), Qt::CaseInsensitive) < 0;
    }

private:
    int m_column;
};

QString fitOf(const HubFile &f, const std::optional<runtime::HardwareInfo> &hw)
{
    runtime::ModelEntry e;
    e.size = f.size;
    if (f.kind == runtime::ModelKind::Llm)
        e.size += 1536LL << 20; // the header is not read yet: room for a 16k context and a projector
    e.info.kind = f.kind;
    e.info.precision = f.precision;
    return f.size > 0 ? ModelsPage::fitKey(e, hw) : QString();
}

} // namespace

int ModelHubDialog::recommendedFile(const QList<HubFile> &files, const std::optional<runtime::HardwareInfo> &hw)
{
    if (files.isEmpty())
        return -1;
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].primary)
            return i;
    }
    // GGUF: the best common quantisation that fits the GPU; without one, Q4_K_M; else the smallest.
    static const QStringList order{QStringLiteral("Q8_0"), QStringLiteral("Q6_K"), QStringLiteral("Q5_K_M"), QStringLiteral("Q4_K_M"),
                                   QStringLiteral("IQ4_XS"), QStringLiteral("Q4_K_S"), QStringLiteral("Q3_K_M")};
    if (hw && hw->maxVram() > 0) {
        for (const QString &quant : order) {
            for (int i = 0; i < files.size(); ++i) {
                const HubFile &f = files[i];
                if (f.kind == runtime::ModelKind::Llm && f.precision == quant && fitOf(f, hw) == QLatin1String("fits"))
                    return i;
            }
        }
    }
    for (int i = 0; i < files.size(); ++i) {
        if (files[i].kind == runtime::ModelKind::Llm && files[i].precision == QLatin1String("Q4_K_M"))
            return i;
    }
    int smallest = -1;
    for (int i = 0; i < files.size(); ++i) {
        const HubFile &f = files[i];
        if (f.kind == runtime::ModelKind::Llm && (smallest < 0 || f.size < files[smallest].size))
            smallest = i;
    }
    if (smallest >= 0)
        return smallest;
    for (int i = 0; i < files.size(); ++i) {
        if (!files[i].pickle())
            return i;
    }
    return 0;
}

ModelHubDialog::ModelHubDialog(Studio *studio, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
{
    setWindowTitle(tr("Download models"));
    resize(1040, 680);
    auto *v = new QVBoxLayout(this);
    v->setSpacing(8);

    auto *bar = new QHBoxLayout;
    m_source = new QComboBox(this);
    m_source->addItem(runtime::hubSourceName(HubSource::HuggingFace), int(HubSource::HuggingFace));
    m_source->addItem(runtime::hubSourceName(HubSource::Civitai), int(HubSource::Civitai));
    m_category = new QComboBox(this);
    m_category->setObjectName(QStringLiteral("hubCategory"));
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search models…"));
    m_search->setClearButtonEnabled(true);
    auto *go = new QPushButton(tr("Search"), this);
    Theme::setRole(go, QStringLiteral("primary"));
    m_nsfw = new QCheckBox(tr("Adult content"), this);
    m_nsfw->setToolTip(tr("Include models Civitai marks as adult content"));
    auto *accounts = new QPushButton(Icons::get(QStringLiteral("link")), tr("Accounts…"), this);
    accounts->setToolTip(tr("Access tokens for gated Hugging Face models and Civitai downloads"));
    bar->addWidget(m_source);
    bar->addWidget(m_category);
    bar->addWidget(m_search, 1);
    bar->addWidget(go);
    bar->addWidget(m_nsfw);
    bar->addWidget(accounts);
    v->addLayout(bar);

    auto *split = new QSplitter(Qt::Horizontal, this);
    auto *left = new QWidget(split);
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    m_results = new QTreeWidget(left);
    m_results->setHeaderLabels({tr("Model"), tr("Author"), tr("Type"), tr("Downloads")});
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->setTextElideMode(Qt::ElideMiddle);
    m_results->setSortingEnabled(true);
    m_results->header()->setSortIndicator(-1, Qt::AscendingOrder); // the hub's order (most downloaded first)
    m_results->header()->setStretchLastSection(false);
    m_results->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_results->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_results->setColumnWidth(1, 130);
    m_results->setColumnWidth(2, 120);
    lv->addWidget(m_results, 1);
    m_more = new QPushButton(tr("Load more"), left);
    m_more->setVisible(false);
    lv->addWidget(m_more);

    auto *scroll = new QScrollArea(split);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_details = new QWidget(scroll);
    auto *dv = new QVBoxLayout(m_details);
    dv->setContentsMargins(12, 0, 0, 0);
    m_title = new QLabel(m_details);
    Theme::setRole(m_title, QStringLiteral("heading"));
    m_title->setWordWrap(true);
    m_title->setOpenExternalLinks(true);
    m_title->setTextFormat(Qt::RichText);
    m_meta = mutedLabel(QString(), m_details);
    m_meta->setTextFormat(Qt::RichText);
    m_notice = new QLabel(m_details);
    m_notice->setWordWrap(true);
    m_notice->setTextFormat(Qt::RichText);
    m_notice->setVisible(false);
    auto *top = new QHBoxLayout;
    auto *texts = new QVBoxLayout;
    texts->addWidget(m_title);
    texts->addWidget(m_meta);
    texts->addWidget(m_notice);
    texts->addStretch();
    m_preview = new QLabel(m_details);
    m_preview->setFixedSize(150, 150);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setVisible(false);
    top->addLayout(texts, 1);
    top->addWidget(m_preview, 0, Qt::AlignTop);
    dv->addLayout(top);
    m_description = mutedLabel(QString(), m_details);
    m_description->setMaximumHeight(110);
    dv->addWidget(m_description);
    auto *vrow = new QHBoxLayout;
    m_version = new QComboBox(m_details);
    vrow->addWidget(new QLabel(tr("Version"), m_details));
    vrow->addWidget(m_version, 1);
    dv->addLayout(vrow);
    m_files = new QTreeWidget(m_details);
    m_files->setHeaderLabels({tr("File"), tr("Precision"), tr("Size"), tr("GPU"), tr("On this computer")});
    m_files->setRootIsDecorated(false);
    m_files->setUniformRowHeights(true);
    m_files->setMinimumHeight(200);
    m_files->setTextElideMode(Qt::ElideMiddle);
    m_files->header()->setStretchLastSection(false);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 5; ++c)
        m_files->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    dv->addWidget(m_files, 1);
    m_projector = new QCheckBox(m_details);
    m_projector->setChecked(true);
    m_projector->setVisible(false);
    dv->addWidget(m_projector);
    m_companions = new QCheckBox(m_details);
    m_companions->setObjectName(QStringLiteral("companions"));
    m_companions->setChecked(true);
    m_companions->setVisible(false);
    dv->addWidget(m_companions);
    auto *drow = new QHBoxLayout;
    drow->addStretch();
    m_download = new QPushButton(Icons::get(QStringLiteral("download")), tr("Download"), m_details);
    Theme::setRole(m_download, QStringLiteral("primary"));
    m_download->setEnabled(false);
    drow->addWidget(m_download);
    dv->addLayout(drow);
    scroll->setWidget(m_details);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    v->addWidget(split, 1);

    m_status = mutedLabel(QString(), this);
    v->addWidget(m_status);
    v->addWidget(mutedLabel(tr("Files go into Lumen's models folder and are checked against the hub's SHA-256 checksums. Check each "
                               "model's licence before you use what you make with it."),
                            this));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    v->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_source, &QComboBox::currentIndexChanged, this, [this] {
        updateSourceControls();
        search();
    });
    connect(m_category, &QComboBox::currentIndexChanged, this, [this] { search(); });
    connect(m_nsfw, &QCheckBox::toggled, this, [this] { search(); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] { search(); });
    connect(go, &QPushButton::clicked, this, [this] { search(); });
    connect(m_more, &QPushButton::clicked, this, [this] { search(true); });
    connect(accounts, &QPushButton::clicked, this, &ModelHubDialog::editAccounts);
    connect(m_results, &QTreeWidget::currentItemChanged, this, &ModelHubDialog::showModel);
    connect(m_version, &QComboBox::currentIndexChanged, this, &ModelHubDialog::showVersion);
    connect(m_files, &QTreeWidget::currentItemChanged, this, [this] {
        const HubVersion *v = currentVersion();
        QTreeWidgetItem *item = m_files->currentItem();
        const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
        m_download->setEnabled(v && index >= 0 && index < v->files.size());
        const std::optional<HubFile> proj = v && index >= 0 && index < v->files.size() ? runtime::projectorFor(v->files, v->files[index]) : std::nullopt;
        m_projector->setVisible(proj.has_value());
        if (proj) {
            m_projector->setText(tr("Also download the vision projector (%1), so the model can see images").arg(runtime::formatBytes(proj->size)));
            m_projector->setToolTip(proj->path);
        }
        showCompanions();
    });
    connect(m_files, &QTreeWidget::itemDoubleClicked, this, &ModelHubDialog::downloadSelected);
    connect(m_download, &QPushButton::clicked, this, &ModelHubDialog::downloadSelected);
    connect(m_studio->models(), &runtime::ModelLibrary::changed, this, &ModelHubDialog::showVersion);

    QPointer<ModelHubDialog> self(this);
    runtime::probeHardware().then(this, [self](runtime::HardwareInfo hw) {
        if (!self)
            return;
        self->m_hw = hw;
        self->showVersion();
    });
    updateSourceControls();
    showModel();
}

HubSource ModelHubDialog::source() const
{
    return HubSource(m_source->currentData().toInt());
}

void ModelHubDialog::setSource(HubSource s)
{
    m_source->setCurrentIndex(std::max(0, m_source->findData(int(s))));
}

void ModelHubDialog::setCategory(const QString &category)
{
    m_category->setCurrentIndex(std::max(0, m_category->findData(category)));
}

void ModelHubDialog::setSearchText(const QString &text)
{
    m_search->setText(text);
}

void ModelHubDialog::updateSourceControls()
{
    const bool first = m_category->count() == 0;
    const QString current = m_category->currentData().toString();
    QSignalBlocker block(m_category);
    m_category->clear();
    // Image models first: the editor's; language models are only for plugins.
    m_category->addItem(tr("Image models"), QStringLiteral("checkpoint"));
    m_category->addItem(tr("LoRAs"), QStringLiteral("lora"));
    m_category->addItem(tr("ControlNets"), QStringLiteral("controlnet"));
    if (source() == HubSource::Civitai)
        m_category->addItem(tr("Upscalers"), QStringLiteral("upscaler"));
    if (source() == HubSource::HuggingFace)
        m_category->addItem(tr("Language and vision (GGUF)"), QStringLiteral("llm"));
    m_category->addItem(tr("Everything"), QString());
    m_category->setCurrentIndex(first ? 0 : std::max(0, m_category->findData(current)));
    m_nsfw->setVisible(source() == HubSource::Civitai);
}

void ModelHubDialog::search(bool more)
{
    if (!more) {
        m_models.clear();
        m_cursor.clear();
        m_current.reset();
        QSignalBlocker block(m_results);
        m_results->clear();
        showModel();
    }
    runtime::HubQuery q;
    q.text = m_search->text();
    q.category = m_category->currentData().toString();
    q.nsfw = source() == HubSource::Civitai && m_nsfw->isChecked();
    q.cursor = more ? m_cursor : QString();
    const int serial = ++m_searchSerial;
    m_more->setEnabled(false);
    m_status->setText(tr("Searching %1…").arg(runtime::hubSourceName(source())));
    QPointer<ModelHubDialog> self(this);
    m_studio->hub()
        ->search(source(), q)
        .then(this, [self, serial](const runtime::HubPage &page) {
            if (!self || serial != self->m_searchSerial)
                return;
            self->m_cursor = page.nextCursor;
            for (const HubModel &m : page.models) {
                const int index = int(self->m_models.size());
                self->m_models << m;
                auto *item = new NumberItem(self->m_results, {m.name, m.author, m.type, countText(m.downloads)}, 3);
                item->setData(0, Qt::UserRole, index);
                item->setData(3, Qt::UserRole + 1, double(m.downloads));
                item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
                item->setToolTip(0, m.source == HubSource::HuggingFace ? m.id : m.name);
                if (!m.gated.isEmpty()) {
                    item->setIcon(0, Icons::get(QStringLiteral("lock")));
                    item->setToolTip(0, tr("%1 — gated: needs an access token and accepted terms").arg(m.id));
                }
            }
            self->m_more->setVisible(!page.nextCursor.isEmpty());
            self->m_more->setEnabled(true);
            self->m_status->setText(self->m_models.isEmpty() ? tr("Nothing found.") : tr("%n model(s)", nullptr, int(self->m_models.size())));
            if (!self->m_results->currentItem() && self->m_results->topLevelItemCount() > 0)
                self->m_results->setCurrentItem(self->m_results->topLevelItem(0));
        })
        .onFailed(this, [self, serial](const std::exception &e) {
            if (!self || serial != self->m_searchSerial)
                return;
            self->m_more->setEnabled(true);
            self->m_status->setText(tr("Search failed: %1").arg(describeException(e)));
        });
}

void ModelHubDialog::showModel()
{
    QTreeWidgetItem *item = m_results->currentItem();
    const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
    if (index < 0 || index >= m_models.size()) {
        m_current.reset();
        m_title->setText(tr("Choose a model"));
        m_meta->clear();
        m_notice->setVisible(false);
        m_description->setText(source() == HubSource::HuggingFace
                                   ? tr("Hugging Face hosts language and vision models (GGUF files for llama.cpp) and many image models.")
                                   : tr("Civitai hosts image checkpoints, LoRAs and ControlNets made by the community."));
        m_preview->setVisible(false);
        showVersion();
        return;
    }
    const HubModel m = m_models[index];
    m_current = m;
    m_title->setText(QStringLiteral("<a href='%1'>%2</a>").arg(m.page.toString(), m.name.toHtmlEscaped()));
    QStringList meta;
    if (!m.author.isEmpty())
        meta << tr("by %1").arg(m.author.toHtmlEscaped());
    meta << tr("%1 downloads").arg(countText(m.downloads));
    if (m.source == HubSource::HuggingFace) {
        meta << tr("Licence: %1").arg(m.license.isEmpty() ? tr("not stated") : m.license.toHtmlEscaped());
    } else {
        QStringList perms;
        const QStringList commercial = m.permissions.value(QStringLiteral("allowCommercialUse")).toStringList();
        perms << (commercial.isEmpty() || commercial.contains(QStringLiteral("None")) ? tr("no commercial use")
                                                                                       : tr("commercial use: %1").arg(commercial.join(QStringLiteral(", "))));
        if (m.permissions.contains(QStringLiteral("allowNoCredit")))
            perms << (m.permissions.value(QStringLiteral("allowNoCredit")).toBool() ? tr("no credit needed") : tr("credit the creator"));
        meta << tr("Licence: %1").arg(perms.join(QStringLiteral(", ")).toHtmlEscaped());
    }
    m_meta->setText(meta.join(QStringLiteral(" · ")));
    QString notice;
    if (m.source == HubSource::HuggingFace && !m.gated.isEmpty() && m_studio->hub()->token(HubSource::HuggingFace).isEmpty())
        notice = tr("Gated model: accept its terms on huggingface.co, then add an access token under Accounts….");
    else if (m.source == HubSource::Civitai && m_studio->hub()->token(HubSource::Civitai).isEmpty())
        notice = tr("Civitai asks for an API key for most downloads: add one under Accounts….");
    m_notice->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().warning.name(), notice.toHtmlEscaped()));
    m_notice->setVisible(!notice.isEmpty());
    m_description->setText(m.description);
    m_preview->setVisible(false);
    m_preview->clear();
    if (!m.preview.isEmpty())
        loadPreview(m.preview, QStringLiteral("hub-%1").arg(m.id));

    if (m.detailed) {
        showVersion();
        return;
    }
    // Hugging Face: the files and the current commit come from a second request.
    {
        QSignalBlocker block(m_version);
        m_version->clear();
    }
    m_files->clear();
    m_download->setEnabled(false);
    m_projector->setVisible(false);
    m_status->setText(tr("Loading the files of %1…").arg(m.id));
    const int serial = ++m_detailSerial;
    QPointer<ModelHubDialog> self(this);
    m_studio->hub()
        ->details(m)
        .then(this, [self, serial, index](const HubModel &full) {
            if (!self || serial != self->m_detailSerial || index >= self->m_models.size())
                return;
            self->m_models[index] = full;
            self->m_current = full;
            self->m_status->clear();
            self->showModel(); // with the licence and files from the details
        })
        .onFailed(this, [self, serial](const std::exception &e) {
            if (!self || serial != self->m_detailSerial)
                return;
            self->m_status->setText(tr("Cannot list the files: %1").arg(describeException(e)));
        });
}

const HubVersion *ModelHubDialog::currentVersion() const
{
    if (!m_current || !m_current->detailed || m_current->versions.isEmpty())
        return nullptr;
    const int i = std::clamp(m_version->currentIndex(), 0, int(m_current->versions.size()) - 1);
    return &m_current->versions[i];
}

void ModelHubDialog::showVersion()
{
    // Versions (Civitai: one per release; Hugging Face: the current commit).
    if (m_current && m_current->detailed && m_version->count() != m_current->versions.size()) {
        QSignalBlocker block(m_version);
        m_version->clear();
        for (const HubVersion &v : m_current->versions) {
            QString label = v.name;
            if (!v.baseModel.isEmpty())
                label += QStringLiteral(" — ") + v.baseModel;
            if (v.published.isValid())
                label += QStringLiteral(" · ") + QLocale().toString(v.published.date(), QLocale::ShortFormat);
            m_version->addItem(label, v.id);
        }
    }
    m_version->setEnabled(m_version->count() > 1);
    const HubVersion *v = currentVersion();
    const QString selectedPath = m_files->currentItem() ? m_files->currentItem()->text(0) : QString();
    QSignalBlocker block(m_files);
    m_files->clear();
    m_download->setEnabled(false);
    m_projector->setVisible(false);
    m_companions->setVisible(false);
    if (!v)
        return;
    if (!v->trainedWords.isEmpty())
        m_description->setText(m_current->description + (m_current->description.isEmpty() ? QString() : QStringLiteral("\n\n"))
                               + tr("Trigger words: %1").arg(v->trainedWords.join(QStringLiteral(", "))));
    runtime::ModelLibrary *lib = m_studio->models();
    for (int i = 0; i < v->files.size(); ++i) {
        const HubFile &f = v->files[i];
        const QString fit = fitOf(f, m_hw);
        const auto local = lib->find(f.sha256, f.fileName(), f.size);
        auto *item = new QTreeWidgetItem(m_files, {f.path, f.precision, f.size > 0 ? runtime::formatBytes(f.size) : QString(), ModelsPage::fitText(fit),
                                                   local ? tr("In the library") : QString()});
        item->setData(0, Qt::UserRole, i);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        if (!fit.isEmpty())
            item->setForeground(3, ModelsPage::fitColor(fit));
        if (local) {
            item->setForeground(4, Theme::palette().success);
            item->setToolTip(4, QDir::toNativeSeparators(local->path));
        }
        if (f.pickle()) {
            item->setIcon(0, Icons::get(QStringLiteral("warning")));
            item->setToolTip(0, tr("Pickle format: it can contain code that runs when Python tools load it."));
        }
        if (f.path == selectedPath)
            m_files->setCurrentItem(item);
    }
    if (!m_files->currentItem()) {
        const int r = recommendedFile(v->files, m_hw);
        if (r >= 0)
            m_files->setCurrentItem(m_files->topLevelItem(r));
    }
    block.unblock();
    emit m_files->currentItemChanged(m_files->currentItem(), nullptr);
}

void ModelHubDialog::showCompanions()
{
    const HubVersion *v = currentVersion();
    QTreeWidgetItem *item = m_files->currentItem();
    const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
    const QList<HubFile> companions = v && index >= 0 && index < v->files.size() ? runtime::companionFiles(v->files, v->files[index]) : QList<HubFile>{};
    QStringList missing;
    QStringList here;
    bool required = false;
    qint64 bytes = 0;
    for (const HubFile &c : companions) {
        const QString what = QStringLiteral("%1 (%2)").arg(c.fileName(), runtime::modelKindLabel(c.kind));
        if (m_studio->models()->find(c.sha256, c.fileName(), c.size)) {
            here << what;
            continue;
        }
        missing << what;
        required = required || c.required;
        bytes += std::max<qint64>(0, c.size);
    }
    m_companions->setVisible(!companions.isEmpty());
    if (companions.isEmpty())
        return;
    if (missing.isEmpty()) {
        m_companions->setChecked(true);
        m_companions->setEnabled(false);
        m_companions->setText(tr("The files it needs are in the library: %1").arg(here.join(QStringLiteral(", "))));
        m_companions->setToolTip(QString());
        return;
    }
    // Files the creator marks as required always come along; the others are recommended.
    m_companions->setEnabled(!required);
    if (required)
        m_companions->setChecked(true);
    m_companions->setText(required ? tr("Also downloads the files it needs (%1): %2").arg(runtime::formatBytes(bytes), missing.join(QStringLiteral(", ")))
                                   : tr("Also download the files that come with it (%1): %2").arg(runtime::formatBytes(bytes), missing.join(QStringLiteral(", "))));
    m_companions->setToolTip(required ? tr("The creator marks them as required: the model does not work without them.")
                                      : tr("The model's version includes them; it may need them to work."));
}

void ModelHubDialog::downloadSelected()
{
    const HubVersion *v = currentVersion();
    QTreeWidgetItem *item = m_files->currentItem();
    if (!v || !item || !m_current)
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= v->files.size())
        return;
    const HubFile file = v->files[index];
    bool allowPickle = false;
    if (file.pickle()) {
        if (QMessageBox::warning(this, tr("Pickle file"),
                                 tr("%1 is in a pickle format. Such files can contain code that runs when Python tools (ComfyUI, WebUI) "
                                    "load them. Lumen's own servers do not run it, but prefer a .safetensors version when there is one.\n\n"
                                    "Download it anyway?")
                                     .arg(file.fileName()),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
        allowPickle = true;
    }
    QList<HubFile> files = runtime::splitParts(v->files, file);
    if (m_projector->isVisible() && m_projector->isChecked()) {
        if (const auto proj = runtime::projectorFor(v->files, file))
            files << *proj;
    }
    // The model's VAE / text encoder: recorded with the model; downloaded when missing (always
    // when required).
    const QList<HubFile> companions = runtime::companionFiles(v->files, file);
    for (const HubFile &c : companions) {
        if (c.required || m_companions->isChecked())
            files << c;
    }
    const HubModel &m = *m_current;
    QStringList queued;
    for (const HubFile &f : std::as_const(files)) {
        if (const auto local = m_studio->models()->find(f.sha256, f.fileName(), f.size)) {
            // A model downloaded before its companions were known learns them now.
            if (f.path == file.path && !companions.isEmpty() && runtime::recordCompanions(local->path, companions))
                m_studio->models()->addFile(local->path);
            continue; // already here
        }
        runtime::HubDownload d;
        d.source = m.source;
        d.modelId = m.id;
        d.modelName = m.name;
        d.versionId = v->id;
        d.versionName = v->name;
        d.file = f;
        d.license = m.license;
        d.permissions = m.permissions;
        d.baseModel = v->baseModel;
        d.trainedWords = v->trainedWords;
        d.nsfw = m.nsfw;
        d.page = m.page;
        d.allowPickle = allowPickle;
        if (f.path == file.path)
            d.companions = companions;
        m_studio->hub()->download(d);
        queued << f.fileName();
    }
    if (queued.isEmpty()) {
        m_status->setText(tr("Already in the library."));
        return;
    }
    m_studio->notify(tr("Downloading %1 — progress is shown in the Queue.").arg(queued.join(QStringLiteral(", "))));
    m_status->setText(tr("Queued: %1").arg(queued.join(QStringLiteral(", "))));
}

void ModelHubDialog::editAccounts()
{
    HubAccountsDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted)
        showModel();
}

void ModelHubDialog::loadPreview(const QUrl &url, const QString &cacheKey)
{
    if (url.scheme() != QLatin1String("https"))
        return;
    const QString cacheFile = QDir(paths::cache()).filePath(QStringLiteral("civitai/%1.img").arg(cacheKey));
    QPointer<QLabel> target(m_preview);
    auto show = [target](const QByteArray &bytes) {
        QPixmap pm;
        if (target && pm.loadFromData(bytes)) {
            target->setPixmap(pm.scaled(target->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            target->setVisible(true);
        }
    };
    QFile cached(cacheFile);
    if (cached.open(QIODevice::ReadOnly)) {
        show(cached.readAll());
        return;
    }
    net::CallOptions o;
    o.quiet = true;
    o.maxRetries = 0;
    o.timeoutMs = 20000;
    net::HttpCall *call = m_studio->http()->get(net::HttpClient::makeRequest(url), o);
    const QString expected = m_current ? m_current->id : QString();
    QPointer<ModelHubDialog> self(this);
    connect(call, &net::HttpCall::finished, m_preview, [self, call, show, cacheFile, expected] {
        if (!call->ok() || call->body().size() > 8 * 1024 * 1024)
            return;
        QDir().mkpath(QFileInfo(cacheFile).absolutePath());
        QFile f(cacheFile);
        if (f.open(QIODevice::WriteOnly))
            f.write(call->body());
        if (self && self->m_current && self->m_current->id == expected)
            show(call->body());
    });
}

// ---- HubAccountsDialog ------------------------------------------------------------------------------

HubAccountsDialog::HubAccountsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Model hub accounts"));
    resize(520, 300);
    auto *v = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto field = [this, form](const QString &label, const QString &key, bool *forget) {
        auto *row = new QHBoxLayout;
        auto *edit = new QLineEdit(this);
        edit->setEchoMode(QLineEdit::Password);
        const bool saved = !SecretStore::read(key).isEmpty();
        edit->setPlaceholderText(saved ? tr("Saved — type a new one to replace it") : tr("Not set"));
        row->addWidget(edit, 1);
        auto *remove = new QToolButton(this);
        remove->setIcon(Icons::get(QStringLiteral("trash")));
        remove->setToolTip(tr("Forget the saved token"));
        remove->setEnabled(saved);
        connect(remove, &QToolButton::clicked, this, [edit, remove, forget] {
            *forget = true;
            edit->clear();
            edit->setPlaceholderText(tr("Not set"));
            remove->setEnabled(false);
        });
        row->addWidget(remove);
        form->addRow(label, row);
        return edit;
    };
    m_hf = field(tr("Hugging Face token"), runtime::ModelHub::tokenKey(HubSource::HuggingFace), &m_forgetHf);
    form->addRow(QString(), mutedLabel(tr("For gated models (for example FLUX.1 dev): accept the model's terms on its page, then create a "
                                          "read-only token at <a href='https://huggingface.co/settings/tokens'>huggingface.co/settings/tokens</a>."),
                                       this));
    m_civitai = field(tr("Civitai API key"), runtime::ModelHub::tokenKey(HubSource::Civitai), &m_forgetCivitai);
    form->addRow(QString(), mutedLabel(tr("Civitai asks for one for most downloads: create it under <a href='https://civitai.com/user/account'>"
                                          "civitai.com/user/account</a> → API Keys."),
                                       this));
    for (QLabel *l : findChildren<QLabel *>()) {
        l->setOpenExternalLinks(true);
        l->setTextFormat(Qt::AutoText);
    }
    v->addLayout(form);
    v->addStretch();
    v->addWidget(mutedLabel(SecretStore::isNativeBackend() ? tr("Tokens are kept in the Windows Credential Manager and are only sent to their own site.")
                                                           : tr("Tokens are kept in a private file in Lumen's data folder and are only sent to their own site."),
                            this));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &HubAccountsDialog::save);
}

void HubAccountsDialog::save()
{
    auto apply = [this](QLineEdit *edit, HubSource s, bool forget) {
        const QString key = runtime::ModelHub::tokenKey(s);
        const QString value = edit->text().trimmed();
        if (!value.isEmpty()) {
            if (!SecretStore::write(key, value)) {
                QMessageBox::warning(this, windowTitle(), tr("Cannot save the token: %1").arg(SecretStore::lastError()));
                return false;
            }
        } else if (forget) {
            SecretStore::remove(key);
        }
        return true;
    };
    if (apply(m_hf, HubSource::HuggingFace, m_forgetHf) && apply(m_civitai, HubSource::Civitai, m_forgetCivitai))
        accept();
}

} // namespace lumen::ui
