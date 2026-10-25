#include "ui/dialogs/ModelsPage.h"

#include "core/Error.h"
#include "core/Paths.h"
#include "net/HttpClient.h"
#include "runtime/ServerProfile.h"
#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/LocalServersPage.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelPresets.h"
#include "ui/dialogs/ModelHubDialog.h"
#include "ui/widgets/Controls.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace lumen::ui {

using runtime::ModelEntry;
using runtime::ModelKind;

namespace {

// Sorts the size column by bytes, the rest by text.
class ModelItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == 4)
            return data(4, Qt::UserRole).toDouble() < other.data(4, Qt::UserRole).toDouble();
        return text(column).compare(other.text(column), Qt::CaseInsensitive) < 0;
    }
};

QString sizeText(qint64 bytes)
{
    return runtime::formatBytes(bytes);
}

QString paramsText(qint64 n)
{
    if (n >= 1000000000)
        return QStringLiteral("%1B").arg(double(n) / 1e9, 0, 'f', 1);
    if (n >= 1000000)
        return QStringLiteral("%1M").arg(double(n) / 1e6, 0, 'f', 0);
    return QString::number(n);
}

QString familyText(const runtime::ModelInfo &i)
{
    QString s = i.family;
    if (!i.variant.isEmpty())
        s += (s.isEmpty() ? QString() : QStringLiteral(" · ")) + i.variant;
    return s;
}

// Removes and deletes everything in a layout, including nested layouts and their widgets (a nested
// layout is its own layout item, so it must not be deleted twice).
void clearLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            delete w;
            delete item;
        } else if (QLayout *inner = item->layout()) {
            clearLayout(inner);
            delete inner;
        } else {
            delete item;
        }
    }
}

void revealInExplorer(const QString &path)
{
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

} // namespace

QList<ModelKind> modelKindsForOption(const QString &serverKind, const QString &optionKey)
{
    if (serverKind == QLatin1String("llama.cpp")) {
        if (optionKey == QLatin1String("model") || optionKey == QLatin1String("more_models"))
            return {ModelKind::Llm};
        if (optionKey == QLatin1String("mmproj"))
            return {ModelKind::Projector};
    } else if (serverKind == QLatin1String("sd.cpp")) {
        if (optionKey == QLatin1String("model"))
            return {ModelKind::Checkpoint};
        if (optionKey == QLatin1String("diffusion_model"))
            return {ModelKind::DiffusionModel};
        if (optionKey == QLatin1String("vae"))
            return {ModelKind::Vae};
        if (optionKey == QLatin1String("control_net"))
            return {ModelKind::ControlNet};
        if (optionKey == QLatin1String("ip_adapter"))
            return {ModelKind::IpAdapter};
        if (optionKey == QLatin1String("clip_vision"))
            return {ModelKind::ClipVision};
        if (optionKey == QLatin1String("clip_l") || optionKey == QLatin1String("clip_g") || optionKey == QLatin1String("t5xxl")
            || optionKey == QLatin1String("llm"))
            return {ModelKind::TextEncoder};
    }
    return {};
}

void fillModelMenu(QMenu *menu, runtime::ModelLibrary *library, const QList<ModelKind> &kinds, const std::function<void(const QString &)> &pick)
{
    menu->clear();
    int count = 0;
    for (ModelKind kind : kinds) {
        for (const ModelEntry &e : library->entries(kind)) {
            QString label = e.name();
            const QString family = familyText(e.info);
            QStringList details;
            if (!family.isEmpty())
                details << family;
            if (!e.info.precision.isEmpty())
                details << e.info.precision;
            details << sizeText(e.size);
            label += QStringLiteral("  —  ") + details.join(QStringLiteral(" · "));
            QAction *a = menu->addAction(label);
            a->setToolTip(QDir::toNativeSeparators(e.path));
            const QString path = e.path;
            QObject::connect(a, &QAction::triggered, menu, [pick, path] { pick(QDir::toNativeSeparators(path)); });
            ++count;
        }
    }
    if (count == 0) {
        QAction *none = menu->addAction(QCoreApplication::translate("lumen::ui::ModelsPage", "No matching models in the library"));
        none->setEnabled(false);
    }
    menu->setToolTipsVisible(true);
}

QString ModelsPage::fitKey(const ModelEntry &e, const std::optional<runtime::HardwareInfo> &hw)
{
    if (!hw || hw->maxVram() == 0)
        return {};
    const ModelKind k = e.info.kind;
    if (k != ModelKind::Llm && k != ModelKind::Checkpoint && k != ModelKind::DiffusionModel)
        return {};
    qint64 need = e.info.estimatedMemory(e.size);
    // sd.cpp converts 16-bit diffusion weights to q8_0 by default on small GPUs: half the size.
    if (k != ModelKind::Llm && (e.info.precision == QLatin1String("F16") || e.info.precision == QLatin1String("BF16")) && hw->maxVram() < 14ull << 30)
        need = e.info.estimatedMemory(e.size / 2);
    const double vram = double(hw->maxVram());
    if (double(need) <= vram * 0.95)
        return QStringLiteral("fits");
    if (k == ModelKind::Llm && double(need) <= vram * 1.8)
        return QStringLiteral("partial"); // llama.cpp keeps the rest of the layers on the CPU
    if (k != ModelKind::Llm && double(need) <= vram * 1.6)
        return QStringLiteral("partial"); // sd.cpp streams weights from RAM
    return QStringLiteral("too large");
}

QString ModelsPage::fitText(const QString &key)
{
    if (key == QLatin1String("fits"))
        return tr("Fits");
    if (key == QLatin1String("partial"))
        return tr("Partly");
    return key.isEmpty() ? QString() : tr("Too large");
}

QColor ModelsPage::fitColor(const QString &key)
{
    if (key == QLatin1String("fits"))
        return Theme::palette().success;
    if (key == QLatin1String("partial"))
        return Theme::palette().warning;
    return Theme::palette().danger;
}

ModelsPage::ModelsPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(8);
    auto *title = new QLabel(tr("Models"), this);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(tr("Model files on this computer: Lumen's models folder and the folders you add (ComfyUI, WebUI, LM Studio, "
                               "Downloads…). Files stay where they are; Lumen reads their headers to tell what they are."),
                            this));
    auto *bar = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search models…"));
    m_search->setClearButtonEnabled(true);
    m_kind = new QComboBox(this);
    m_kind->addItem(tr("All types"), QString());
    for (ModelKind k : runtime::allModelKinds())
        m_kind->addItem(runtime::modelKindLabel(k), runtime::modelKindKey(k));
    m_folders = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Folders"), this);
    m_folders->setMenu(new QMenu(m_folders));
    connect(m_folders->menu(), &QMenu::aboutToShow, this, &ModelsPage::buildFoldersMenu);
    auto *rescan = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Rescan"), this);
    auto *download = new QPushButton(Icons::get(QStringLiteral("download")), tr("Download…"), this);
    Theme::setRole(download, QStringLiteral("primary"));
    download->setToolTip(tr("Find and download models from Hugging Face and Civitai"));
    connect(download, &QPushButton::clicked, this, [this] {
        auto *dlg = new ModelHubDialog(m_studio, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        dlg->search();
    });
    bar->addWidget(download);
    bar->addWidget(m_search, 1);
    bar->addWidget(m_kind);
    bar->addWidget(m_folders);
    bar->addWidget(rescan);
    v->addLayout(bar);

    auto *split = new QSplitter(Qt::Horizontal, this);
    m_list = new QTreeWidget(split);
    m_list->setHeaderLabels({tr("Name"), tr("Type"), tr("Family"), tr("Precision"), tr("Size"), tr("GPU"), tr("Folder")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(0, Qt::AscendingOrder);
    auto *scroll = new QScrollArea(split);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_details = new QWidget(scroll);
    m_detailsLayout = new QVBoxLayout(m_details);
    m_detailsLayout->setContentsMargins(12, 0, 0, 0);
    scroll->setWidget(m_details);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);
    m_status = mutedLabel(QString(), this);
    v->addWidget(m_status);

    runtime::ModelLibrary *lib = m_studio->models();
    connect(lib, &runtime::ModelLibrary::changed, this, &ModelsPage::reload);
    connect(lib, &runtime::ModelLibrary::scanFinished, this, &ModelsPage::reload);
    connect(rescan, &QPushButton::clicked, lib, &runtime::ModelLibrary::rescan);
    connect(m_search, &QLineEdit::textChanged, this, &ModelsPage::reload);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &ModelsPage::reload);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &ModelsPage::showDetails);
    QPointer<ModelsPage> self(this);
    runtime::probeHardware().then(this, [self](runtime::HardwareInfo hw) {
        if (!self)
            return;
        self->m_hardware = hw;
        self->reload();
    });
    reload();
    showDetails();
}

QString ModelsPage::selectedPath() const
{
    QTreeWidgetItem *item = m_list->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void ModelsPage::reload()
{
    const QString current = selectedPath();
    const QString needle = m_search->text().trimmed();
    const QString kindKey = m_kind->currentData().toString();
    QSignalBlocker block(m_list);
    m_list->setSortingEnabled(false);
    m_list->clear();
    qint64 total = 0;
    int shown = 0;
    const QList<ModelEntry> all = m_studio->models()->entries();
    for (const ModelEntry &e : all) {
        if (!kindKey.isEmpty() && runtime::modelKindKey(e.info.kind) != kindKey)
            continue;
        const QString family = familyText(e.info);
        if (!needle.isEmpty() && !e.name().contains(needle, Qt::CaseInsensitive) && !family.contains(needle, Qt::CaseInsensitive)
            && !e.civitai.value(QStringLiteral("name")).toString().contains(needle, Qt::CaseInsensitive))
            continue;
        const QString fit = fitKey(e, m_hardware);
        // Which scanned folder it is in (copies of the same file in two places are common).
        QString folder;
        for (const runtime::ModelRoot &r : m_studio->models()->roots()) {
            if (e.path.startsWith(r.path + QLatin1Char('/'), Qt::CaseInsensitive) && r.path.size() >= folder.size())
                folder = r.label;
        }
        auto *item = new ModelItem(m_list, QStringList{e.name(), runtime::modelKindLabel(e.info.kind), family, e.info.precision, sizeText(e.size), fitText(fit), folder});
        item->setData(0, Qt::UserRole, e.path);
        item->setData(4, Qt::UserRole, double(e.size));
        item->setToolTip(0, QDir::toNativeSeparators(e.path));
        item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        if (!fit.isEmpty())
            item->setForeground(5, fitColor(fit));
        if (e.info.unsafeFormat)
            item->setIcon(0, Icons::get(QStringLiteral("warning")));
        if (e.path == current)
            m_list->setCurrentItem(item);
        total += e.size;
        ++shown;
    }
    m_list->setSortingEnabled(true);
    for (int c = 1; c < 6; ++c)
        m_list->resizeColumnToContents(c);
    m_list->setColumnWidth(0, std::max(220, m_list->columnWidth(0)));
    const QString hw = m_hardware ? tr(" · GPU: %1").arg(sizeText(qint64(m_hardware->maxVram()))) : QString();
    m_status->setText(m_studio->models()->isScanning()
                          ? tr("Scanning…")
                          : tr("%n model(s) shown, %1", nullptr, shown).arg(sizeText(total)) + tr(" · %n folder(s)", nullptr, int(m_studio->models()->roots().size())) + hw);
    block.unblock();
    if (m_list->currentItem() == nullptr || selectedPath() != current)
        showDetails();
}

QStringList ModelsPage::usedFiles() const
{
    QStringList out;
    for (const runtime::ServerProfile &p : m_studio->servers()->profiles()) {
        for (auto it = p.options.cbegin(); it != p.options.cend(); ++it) {
            if (it.value().typeId() == QMetaType::QString && QFileInfo(it.value().toString()).isFile())
                out << it.value().toString();
        }
        for (const runtime::LlamaModel &m : runtime::llamaModels(p))
            out << m.model << m.mmproj;
    }
    return out;
}

QStringList ModelsPage::serversUsing(const QString &path) const
{
    QStringList names;
    for (const runtime::ServerProfile &p : m_studio->servers()->profiles()) {
        bool uses = false;
        for (auto it = p.options.cbegin(); it != p.options.cend(); ++it) {
            if (it.value().typeId() == QMetaType::QString && QFileInfo(it.value().toString()) == QFileInfo(path))
                uses = true;
        }
        for (const runtime::LlamaModel &m : runtime::llamaModels(p))
            uses |= QFileInfo(m.model) == QFileInfo(path) || QFileInfo(m.mmproj) == QFileInfo(path);
        // Folders handed to servers (LoRAs, upscalers).
        for (const QString &key : {QStringLiteral("lora_dir"), QStringLiteral("upscalers_dir")}) {
            const QString dir = p.options.value(key).toString();
            if (!dir.isEmpty() && QDir::cleanPath(QFileInfo(path).absolutePath()).compare(QDir::cleanPath(dir), Qt::CaseInsensitive) == 0)
                uses = true;
        }
        if (uses)
            names << p.name;
    }
    return names;
}

void ModelsPage::buildFoldersMenu()
{
    QMenu *menu = m_folders->menu();
    menu->clear();
    runtime::ModelLibrary *lib = m_studio->models();
    menu->addSection(tr("Scanned folders"));
    for (const runtime::ModelRoot &r : lib->roots()) {
        QMenu *sub = menu->addMenu(Icons::get(QStringLiteral("folder")), QStringLiteral("%1 — %2").arg(r.label, QDir::toNativeSeparators(r.path)));
        const QString path = r.path;
        sub->addAction(tr("Open"), this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
        QAction *remove = sub->addAction(tr("Stop scanning this folder"), this, [lib, path] { lib->removeRoot(path); });
        remove->setEnabled(!r.builtin);
    }
    const QStringList suggestions = lib->suggestedRoots(m_studio->runtimes(), usedFiles());
    if (!suggestions.isEmpty()) {
        menu->addSection(tr("Found on this computer"));
        for (const QString &s : suggestions)
            menu->addAction(Icons::get(QStringLiteral("plus")), QDir::toNativeSeparators(s), this, [lib, s] { lib->addRoot(s); });
    }
    menu->addSeparator();
    menu->addAction(Icons::get(QStringLiteral("plus")), tr("Add folder…"), this, [this, lib] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Add a folder with models"));
        if (!dir.isEmpty())
            lib->addRoot(dir);
    });
}

void ModelsPage::showDetails()
{
    clearLayout(m_detailsLayout);
    const auto entry = m_studio->models()->entry(selectedPath());
    if (!entry) {
        m_detailsLayout->addWidget(mutedLabel(m_studio->models()->entries().isEmpty()
                                                  ? tr("No models yet. Add the folders where your models are with Folders → Found on this computer "
                                                       "or Add folder…, or put files into Lumen's models folder.")
                                                  : tr("Select a model to see its details."),
                                              m_details));
        m_detailsLayout->addStretch();
        return;
    }
    const ModelEntry e = *entry;
    auto *name = new QLabel(e.name(), m_details);
    Theme::setRole(name, QStringLiteral("title"));
    name->setWordWrap(true);
    m_detailsLayout->addWidget(name);
    auto *path = new QLabel(QDir::toNativeSeparators(e.path), m_details);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Theme::setRole(path, QStringLiteral("muted"));
    m_detailsLayout->addWidget(path);

    QStringList facts;
    facts << runtime::modelKindLabel(e.info.kind);
    if (!familyText(e.info).isEmpty())
        facts << familyText(e.info);
    if (!e.info.precision.isEmpty())
        facts << e.info.precision;
    if (e.info.parameters > 0)
        facts << tr("%1 parameters").arg(paramsText(e.info.parameters));
    facts << sizeText(e.size);
    if (e.info.contextLength > 0)
        facts << tr("context %1").arg(QLocale().toString(e.info.contextLength));
    m_detailsLayout->addWidget(new QLabel(facts.join(QStringLiteral(" · ")), m_details));
    const QString fit = fitKey(e, m_hardware);
    if (!fit.isEmpty()) {
        const qint64 need = e.info.estimatedMemory(e.size);
        auto *fitLabel = new QLabel(fit == QLatin1String("fits") ? tr("Fits in the GPU's memory (about %1 needed).").arg(sizeText(need))
                                    : fit == QLatin1String("partial")
                                        ? tr("Runs partly from system memory: slower (about %1 needed, %2 VRAM).").arg(sizeText(need), sizeText(qint64(m_hardware->maxVram())))
                                        : tr("Too large for this GPU (about %1 needed, %2 VRAM).").arg(sizeText(need), sizeText(qint64(m_hardware->maxVram()))),
                                    m_details);
        fitLabel->setWordWrap(true);
        m_detailsLayout->addWidget(fitLabel);
    }
    if (!e.info.triggerWords.isEmpty())
        m_detailsLayout->addWidget(new QLabel(tr("Trigger words: %1").arg(e.info.triggerWords.join(QStringLiteral(", "))), m_details));
    if (e.info.unsafeFormat) {
        auto *warn = new QLabel(tr("Pickle format (.ckpt/.pt/.pth/.bin): it can contain code that runs when Python tools load it. Prefer "
                                   ".safetensors versions from trusted sources."),
                                m_details);
        warn->setWordWrap(true);
        Theme::setRole(warn, QStringLiteral("error"));
        m_detailsLayout->addWidget(warn);
    }
    if (!e.info.error.isEmpty()) {
        auto *err = new QLabel(e.info.error, m_details);
        Theme::setRole(err, QStringLiteral("error"));
        m_detailsLayout->addWidget(err);
    }
    const QStringList users = serversUsing(e.path);
    m_detailsLayout->addWidget(mutedLabel(users.isEmpty() ? tr("Not used by a server.") : tr("Used by: %1").arg(users.join(QStringLiteral(", "))), m_details));

    // Downloaded by Lumen: where from, which version, under which licence.
    if (!e.source.isEmpty() && e.source.value(QStringLiteral("source")).toString() == QLatin1String("huggingface")) {
        const QString repo = e.source.value(QStringLiteral("model")).toString();
        QString text = tr("Downloaded from <a href='%1'>%2</a> (version %3)")
                           .arg(QStringLiteral("https://huggingface.co/") + repo, repo.toHtmlEscaped(), e.source.value(QStringLiteral("version")).toString().left(7));
        const QString licence = e.source.value(QStringLiteral("license")).toString();
        if (!licence.isEmpty())
            text += QStringLiteral("<br>") + tr("Licence: %1").arg(licence.toHtmlEscaped());
        auto *src = new QLabel(text, m_details);
        src->setOpenExternalLinks(true);
        src->setWordWrap(true);
        m_detailsLayout->addWidget(src);
    }

    // Civitai.
    m_detailsLayout->addWidget(sectionLabel(tr("Civitai"), m_details));
    const QJsonObject c = e.civitai;
    if (c.value(QStringLiteral("notFound")).toBool()) {
        m_detailsLayout->addWidget(mutedLabel(tr("Not found on Civitai."), m_details));
    } else if (!c.isEmpty()) {
        auto *info = new QLabel(QStringLiteral("<a href='%1'>%2</a> — %3<br>%4%5")
                                    .arg(c.value(QStringLiteral("page")).toString(), c.value(QStringLiteral("name")).toString().toHtmlEscaped(),
                                         c.value(QStringLiteral("version")).toString().toHtmlEscaped(),
                                         tr("Base model: %1").arg(c.value(QStringLiteral("baseModel")).toString().toHtmlEscaped()),
                                         c.value(QStringLiteral("trainedWords")).toArray().isEmpty()
                                             ? QString()
                                             : QStringLiteral("<br>") + tr("Trained words: %1").arg(QVariant(c.value(QStringLiteral("trainedWords")).toArray().toVariantList()).toStringList().join(QStringLiteral(", ")).toHtmlEscaped())),
                                m_details);
        info->setOpenExternalLinks(true);
        info->setWordWrap(true);
        m_detailsLayout->addWidget(info);
        const QString preview = c.value(QStringLiteral("preview")).toString();
        if (!preview.isEmpty()) {
            auto *img = new QLabel(m_details);
            img->setMinimumHeight(40);
            m_detailsLayout->addWidget(img);
            // Previews are cached by version.
            const QString cacheFile = QDir(paths::cache()).filePath(QStringLiteral("civitai/%1.img").arg(qint64(c.value(QStringLiteral("versionId")).toDouble())));
            QPointer<QLabel> target(img);
            auto show = [target](const QByteArray &bytes) {
                QPixmap pm;
                if (target && pm.loadFromData(bytes))
                    target->setPixmap(pm.scaled(QSize(260, 260), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            };
            QFile cached(cacheFile);
            if (cached.open(QIODevice::ReadOnly)) {
                show(cached.readAll());
            } else {
                net::CallOptions o;
                o.quiet = true;
                o.maxRetries = 0;
                net::HttpCall *call = m_studio->http()->get(net::HttpClient::makeRequest(QUrl(preview)), o);
                connect(call, &net::HttpCall::finished, img, [call, show, cacheFile] {
                    if (!call->ok())
                        return;
                    QDir().mkpath(QFileInfo(cacheFile).absolutePath());
                    QFile f(cacheFile);
                    if (f.open(QIODevice::WriteOnly))
                        f.write(call->body());
                    show(call->body());
                });
            }
        }
    } else {
        m_detailsLayout->addWidget(mutedLabel(tr("Looking a file up sends its SHA-256 checksum to civitai.com."), m_details));
    }
    // The files the model needs from its Civitai version (VAE, text encoder).
    const QList<runtime::ModelCompanion> companions = runtime::companionsOf(m_studio->models(), e.path);
    if (!companions.isEmpty()) {
        QStringList lines;
        QList<runtime::HubFile> missing;
        qint64 bytes = 0;
        for (const runtime::ModelCompanion &comp : companions) {
            QString line = QStringLiteral("%1 (%2%3)").arg(comp.file.fileName().toHtmlEscaped(), runtime::modelKindLabel(comp.file.kind),
                                                         comp.file.required ? QStringLiteral(", ") + tr("required") : QString());
            if (comp.missing()) {
                missing << comp.file;
                bytes += std::max<qint64>(0, comp.file.size);
                line = QStringLiteral("<span style='color:%1'>%2 — %3</span>").arg(Theme::palette().warning.name(), line, tr("missing"));
            } else {
                line += QStringLiteral(" — ") + tr("in the library");
            }
            lines << line;
        }
        auto *needs = new QLabel(tr("Needs: %1").arg(lines.join(QStringLiteral("<br>"))), m_details);
        needs->setObjectName(QStringLiteral("companions"));
        needs->setWordWrap(true);
        needs->setTextFormat(Qt::RichText);
        m_detailsLayout->addWidget(needs);
        if (!missing.isEmpty()) {
            auto *get = new QPushButton(Icons::get(QStringLiteral("download")), tr("Download missing files (%1)").arg(runtime::formatBytes(bytes)), m_details);
            get->setObjectName(QStringLiteral("downloadCompanions"));
            Theme::setRole(get, QStringLiteral("primary"));
            m_detailsLayout->addWidget(get, 0, Qt::AlignLeft);
            connect(get, &QPushButton::clicked, this, [this, e, missing, get] {
                QStringList names;
                for (const runtime::HubFile &f : missing) {
                    m_studio->hub()->download(runtime::companionDownload(e, f));
                    names << f.fileName();
                }
                get->setEnabled(false);
                m_studio->notify(tr("Downloading %1 — progress is shown in the Queue.").arg(names.join(QStringLiteral(", "))));
            });
        }
    }

    auto *buttons = new QHBoxLayout;
    auto *lookup = new QPushButton(Icons::get(QStringLiteral("info")), c.isEmpty() ? tr("Look up on Civitai") : tr("Look up again"), m_details);
    auto *use = new QPushButton(Icons::get(QStringLiteral("server")), tr("Use with a server"), m_details);
    auto *reveal = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Show in folder"), m_details);
    auto *trash = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Move to recycle bin"), m_details);
    Theme::setRole(trash, QStringLiteral("danger"));
    use->setEnabled(e.info.kind == ModelKind::Llm || e.info.kind == ModelKind::Checkpoint || e.info.kind == ModelKind::DiffusionModel);
    buttons->addWidget(lookup);
    buttons->addWidget(use);
    m_detailsLayout->addLayout(buttons);
    auto *buttons2 = new QHBoxLayout;
    buttons2->addWidget(reveal);
    buttons2->addWidget(trash);
    m_detailsLayout->addLayout(buttons2);
    if (!e.sha256.isEmpty()) {
        auto *sha = new QLabel(QStringLiteral("SHA-256: %1").arg(e.sha256), m_details);
        sha->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sha->setWordWrap(true);
        Theme::setRole(sha, QStringLiteral("muted"));
        m_detailsLayout->addWidget(sha);
    }
    m_detailsLayout->addStretch();

    const QString filePath = e.path;
    connect(lookup, &QPushButton::clicked, this, [this, filePath, lookup] {
        lookup->setEnabled(false);
        lookup->setText(tr("Hashing and looking up…"));
        QPointer<ModelsPage> self(this);
        m_studio->models()->lookupCivitai(filePath).onFailed(this, [self](const std::exception &ex) {
            if (self) {
                self->m_studio->notify(describeException(ex), QStringLiteral("error"));
                self->showDetails();
            }
            return QJsonObject();
        });
    });
    connect(reveal, &QPushButton::clicked, this, [filePath] { revealInExplorer(filePath); });
    connect(use, &QPushButton::clicked, this, [this, e] { useWithServer(e); });
    connect(trash, &QPushButton::clicked, this, [this, filePath, users] {
        QString question = tr("Move %1 to the recycle bin?").arg(QFileInfo(filePath).fileName());
        if (!users.isEmpty())
            question += QStringLiteral("\n\n") + tr("It is used by: %1.").arg(users.join(QStringLiteral(", ")));
        if (QMessageBox::question(this, tr("Move to recycle bin"), question) != QMessageBox::Yes)
            return;
        QString error;
        if (!m_studio->models()->trash(filePath, &error))
            QMessageBox::warning(this, tr("Move to recycle bin"), error);
    });
}

void ModelsPage::warnMissingCompanions(const ModelEntry &e)
{
    QStringList missing;
    for (const runtime::ModelCompanion &c : runtime::companionsOf(m_studio->models(), e.path)) {
        if (c.missing())
            missing << c.file.fileName();
    }
    if (!missing.isEmpty())
        m_studio->notify(tr("%1 needs %2, which is not in the library yet: download it from the model's details.")
                             .arg(e.name(), missing.join(QStringLiteral(", "))),
                         QStringLiteral("warning"));
}

namespace {

// Server options from modelServerOptions: an empty model slot is removed, the rest set.
void applyModelOptions(QVariantMap *options, const QVariantMap &model)
{
    for (auto it = model.begin(); it != model.end(); ++it) {
        if (it.value().typeId() == QMetaType::QString && it.value().toString().isEmpty())
            options->remove(it.key());
        else
            options->insert(it.key(), it.value());
    }
}

} // namespace

void ModelsPage::useWithServer(const ModelEntry &e)
{
    // Offer servers of the matching kind, or a new one.
    const QString kind = e.info.kind == ModelKind::Llm ? QStringLiteral("llama.cpp") : QStringLiteral("sd.cpp");
    const QString key = e.info.kind == ModelKind::DiffusionModel ? QStringLiteral("diffusion_model") : QStringLiteral("model");
    QMenu menu(this);
    runtime::ServerSupervisor *servers = m_studio->servers();
    for (const runtime::ServerProfile &p : servers->profiles()) {
        if (servers->kindOf(p.id) != kind)
            continue;
        menu.addAction(Icons::get(QStringLiteral("server")), tr("Use in %1").arg(p.name), this, [this, p, key, e, kind] {
            runtime::ServerProfile changed = p;
            if (kind == QLatin1String("llama.cpp")) {
                // Added as a further model, so the current one stays available.
                QVariantList more = changed.options.value(QStringLiteral("more_models")).toList();
                more << QVariantMap{{QStringLiteral("model"), QDir::toNativeSeparators(e.path)},
                                    {QStringLiteral("mmproj"), QDir::toNativeSeparators(runtime::findProjector(e.path))}};
                changed.options.insert(QStringLiteral("more_models"), more);
            } else {
                // The previous model's VAE / text encoder go with it.
                for (const QString &previousKey : {QStringLiteral("model"), QStringLiteral("diffusion_model")}) {
                    const QString previous = changed.options.value(previousKey).toString();
                    if (previous.isEmpty())
                        continue;
                    const QVariantMap old = runtime::companionServerOptions(m_studio->models(), QDir::fromNativeSeparators(previous));
                    for (auto it = old.begin(); it != old.end(); ++it) {
                        if (changed.options.value(it.key()).toString() == it.value().toString())
                            changed.options.remove(it.key());
                    }
                }
                // The preset puts it in its slot with its components (ModelPresets.h).
                applyModelOptions(&changed.options, runtime::modelServerOptions(m_studio->models(), e.path));
                warnMissingCompanions(e);
                // What still does not fit (a ControlNet of another family) is named now, not at the start.
                const QList<runtime::ProfileProblem> problems = runtime::checkSdProfile(m_studio->models(), changed);
                for (const runtime::ProfileProblem &problem : problems) {
                    if (!problem.missing) {
                        m_studio->notify(problem.message, QStringLiteral("warning"));
                        break;
                    }
                }
            }
            m_studio->servers()->updateProfile(changed);
            m_studio->notify(m_studio->servers()->needsRestart(p.id) ? tr("%1 uses it after a restart.").arg(p.name) : tr("%1 updated.").arg(p.name),
                             QStringLiteral("success"));
        });
    }
    const auto runtimes = m_studio->runtimes()->runtimes(kind);
    QAction *create = menu.addAction(Icons::get(QStringLiteral("plus")), tr("New server with this model…"), this, [this, e, kind, key, runtimes] {
        runtime::ServerProfile p = m_studio->servers()->createProfile(runtimes.last().id, e.name());
        if (kind == QLatin1String("llama.cpp")) {
            p.options.insert(key, QDir::toNativeSeparators(e.path));
            p.options.insert(QStringLiteral("mmproj"), QDir::toNativeSeparators(runtime::findProjector(e.path)));
        } else {
            applyModelOptions(&p.options, runtime::modelServerOptions(m_studio->models(), e.path));
            warnMissingCompanions(e);
        }
        ServerProfileDialog dlg(m_studio, p, true, this);
        if (dlg.exec() == QDialog::Accepted)
            addServer(m_studio, dlg.profile(), dlg.createProvider());
    });
    create->setEnabled(!runtimes.isEmpty());
    if (runtimes.isEmpty())
        create->setToolTip(tr("Install or add %1 under Local servers first.").arg(kind));
    menu.exec(QCursor::pos());
}

} // namespace lumen::ui
