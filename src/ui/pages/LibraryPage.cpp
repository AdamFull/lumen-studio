#include "ui/pages/LibraryPage.h"

#include "ai/GenerationService.h"
#include "core/Error.h"
#include "core/JsonUtil.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/ReferenceStrip.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

constexpr int kDocumentsFilter = 100;
constexpr int kAllFilter = 99; // QButtonGroup reserves -1 for automatic ids

class AssetDelegate : public QStyledItemDelegate
{
public:
    AssetDelegate(int *size, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_size(size)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(*m_size + 12, *m_size + 12); }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        const Palette &pal = Theme::palette();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF cell = QRectF(opt.rect).adjusted(6, 6, -6, -6);
        QPainterPath clip;
        clip.addRoundedRect(cell, 10, 10);
        p->fillPath(clip, pal.surface);
        const QPixmap pm = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!pm.isNull()) {
            p->setClipPath(clip);
            const QSizeF s = QSizeF(pm.size()).scaled(cell.size(), Qt::KeepAspectRatioByExpanding);
            p->drawPixmap(QRectF(cell.center().x() - s.width() / 2, cell.center().y() - s.height() / 2, s.width(), s.height()), pm,
                          QRectF(pm.rect()));
            p->setClipping(false);
        } else {
            p->setPen(pal.textMuted);
            p->drawText(cell, Qt::AlignCenter, QObject::tr("Loading…"));
        }
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        if (selected || hover) {
            p->setPen(QPen(selected ? pal.accent : pal.borderStrong, selected ? 2.5 : 1.5));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(cell, 10, 10);
        }
        if (index.data(AssetModel::FavoriteRole).toBool()) {
            const QRectF star(cell.right() - 26, cell.top() + 6, 20, 20);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 120));
            p->drawEllipse(star.adjusted(-2, -2, 2, 2));
            p->drawPixmap(star.toRect(), Icons::pixmap(QStringLiteral("star-filled"), 20, QColor(0xf5c542)));
        }
        if (hover) {
            const QString kind = index.data(AssetModel::KindRole).toString();
            QFont f = opt.font;
            f.setPointSizeF(f.pointSizeF() * 0.85);
            p->setFont(f);
            const QFontMetrics fm(f);
            const QRectF badge(cell.left() + 6, cell.bottom() - fm.height() - 10, fm.horizontalAdvance(kind) + 14, fm.height() + 4);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 150));
            p->drawRoundedRect(badge, 6, 6);
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, kind);
        }
        p->restore();
    }

private:
    int *m_size;
};

int s_tileSize = 180;

} // namespace

// ---- AssetModel -------------------------------------------------------------------------------------------

AssetModel::AssetModel(ThumbnailCache *thumbs, QObject *parent)
    : QAbstractListModel(parent)
    , m_thumbs(thumbs)
{
    connect(thumbs, &ThumbnailCache::ready, this, [this](const QString &id) {
        const QModelIndex idx = indexOf(id);
        if (idx.isValid())
            emit dataChanged(idx, idx, {Qt::DecorationRole});
    });
}

void AssetModel::setProject(doc::Project *project)
{
    if (m_project)
        m_project->disconnect(this);
    m_project = project;
    if (project) {
        connect(project, &doc::Project::opened, this, &AssetModel::reload);
        connect(project, &doc::Project::assetAdded, this, [this](const doc::AssetInfo &info) {
            beginInsertRows({}, 0, 0);
            m_assets.prepend(info);
            endInsertRows();
        });
        connect(project, &doc::Project::assetChanged, this, [this](const doc::AssetInfo &info) {
            for (int i = 0; i < m_assets.size(); ++i) {
                if (m_assets[i].id == info.id) {
                    m_assets[i] = info;
                    emit dataChanged(index(i), index(i));
                }
            }
        });
        connect(project, &doc::Project::assetsRemoved, this, [this](const QStringList &ids) {
            for (int i = int(m_assets.size()) - 1; i >= 0; --i) {
                if (ids.contains(m_assets[i].id)) {
                    beginRemoveRows({}, i, i);
                    m_assets.removeAt(i);
                    endRemoveRows();
                }
            }
        });
    }
    reload();
}

void AssetModel::reload()
{
    beginResetModel();
    m_assets.clear();
    if (m_project) {
        m_assets = m_project->assets();
        std::reverse(m_assets.begin(), m_assets.end());
    }
    endResetModel();
}

int AssetModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_assets.size());
}

QModelIndex AssetModel::indexOf(const QString &id) const
{
    for (int i = 0; i < m_assets.size(); ++i) {
        if (m_assets[i].id == id)
            return index(i);
    }
    return {};
}

QVariant AssetModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_assets.size())
        return {};
    const doc::AssetInfo &a = m_assets[index.row()];
    switch (role) {
    case Qt::DecorationRole: return m_thumbs->get(a.id);
    case Qt::DisplayRole: return a.name;
    case Qt::ToolTipRole:
        return QStringLiteral("<b>%1</b><br>%2 · %3×%4<br>%5").arg(a.name.toHtmlEscaped(), doc::assetKindLabel(a.kind))
            .arg(a.size.width()).arg(a.size.height()).arg(a.prompt().left(200).toHtmlEscaped());
    case IdRole: return a.id;
    case KindRole: return doc::assetKindLabel(a.kind);
    case FavoriteRole: return a.favorite;
    case CreatedRole: return a.created;
    case SearchRole: return a.name + u' ' + a.prompt() + u' ' + a.tags.join(u' ') + u' ' + a.metadata.value(QStringLiteral("provider")).toString();
    case InfoRole: return QVariant::fromValue(static_cast<int>(a.kind));
    default: return {};
    }
}

Qt::ItemFlags AssetModel::flags(const QModelIndex &index) const
{
    return QAbstractListModel::flags(index) | Qt::ItemIsDragEnabled;
}

QStringList AssetModel::mimeTypes() const
{
    return {QString::fromLatin1(kAssetMime)};
}

QMimeData *AssetModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData;
    QByteArray ids;
    for (const QModelIndex &i : indexes)
        ids += i.data(IdRole).toString().toUtf8() + '\n';
    mime->setData(QString::fromLatin1(kAssetMime), ids);
    return mime;
}

// ---- AssetFilter --------------------------------------------------------------------------------------------

void AssetFilter::setKind(int kind)
{
    m_kind = kind;
    invalidateFilter();
}

void AssetFilter::setFavoritesOnly(bool on)
{
    m_favorites = on;
    invalidateFilter();
}

void AssetFilter::setSearch(const QString &text)
{
    m_search = text.trimmed();
    invalidateFilter();
}

bool AssetFilter::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const QModelIndex idx = sourceModel()->index(row, 0, parent);
    const int kind = idx.data(AssetModel::InfoRole).toInt();
    if (m_kind >= 0 && kind != m_kind)
        return false;
    // Sliced character parts are intermediate results with their own filter.
    if (m_kind < 0 && kind == static_cast<int>(doc::AssetKind::Part))
        return false;
    if (m_favorites && !idx.data(AssetModel::FavoriteRole).toBool())
        return false;
    if (!m_search.isEmpty()) {
        const QString hay = idx.data(AssetModel::SearchRole).toString();
        for (const QString &word : m_search.split(u' ', Qt::SkipEmptyParts)) {
            if (!hay.contains(word, Qt::CaseInsensitive))
                return false;
        }
    }
    return true;
}

// ---- LibraryPage ---------------------------------------------------------------------------------------------

LibraryPage::LibraryPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    m_thumbs = new ThumbnailCache(this);
    m_model = new AssetModel(m_thumbs, this);
    m_filter = new AssetFilter(this);
    m_filter->setSourceModel(m_model);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Toolbar.
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("Panel"));
    auto *bh = new FlowLayout(bar);
    bh->setContentsMargins(16, 10, 16, 10);
    auto *title = new QLabel(tr("Library"), bar);
    Theme::setRole(title, QStringLiteral("title"));
    bh->addWidget(title);
    m_kinds = new QButtonGroup(this);
    const QList<QPair<QString, int>> kinds = {
        {tr("All"), kAllFilter},
        {tr("Generated"), static_cast<int>(doc::AssetKind::Generation)},
        {tr("Edited"), static_cast<int>(doc::AssetKind::Edit)},
        {tr("References"), static_cast<int>(doc::AssetKind::Reference)},
        {tr("Imported"), static_cast<int>(doc::AssetKind::Import)},
        {tr("Poses"), static_cast<int>(doc::AssetKind::Pose)},
        {tr("Exports"), static_cast<int>(doc::AssetKind::Export)},
        {tr("Parts"), static_cast<int>(doc::AssetKind::Part)},
        {tr("Documents"), kDocumentsFilter},
    };
    for (const auto &k : kinds) {
        auto *chip = new QToolButton(bar);
        chip->setText(k.first);
        chip->setCheckable(true);
        chip->setProperty("role", QStringLiteral("chip"));
        m_kinds->addButton(chip, k.second);
        bh->addWidget(chip);
    }
    m_kinds->button(kAllFilter)->setChecked(true);
    m_favorites = iconButton(QStringLiteral("star"), tr("Favourites only"), bar, true);
    bh->addWidget(m_favorites);
    m_search = new QLineEdit(bar);
    m_search->setPlaceholderText(tr("Search prompts, names, tags…"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(260);
    m_size = new QSlider(Qt::Horizontal, bar);
    m_size->setRange(100, 360);
    m_size->setValue(s_tileSize);
    m_size->setFixedWidth(100);
    m_size->setToolTip(tr("Thumbnail size"));
    auto *import = new QPushButton(Icons::get(QStringLiteral("upload")), tr("Import"), bar);
    bh->setAlignment(bh->addGroup({m_search, m_size, import}), Qt::AlignRight);
    root->addWidget(bar);
    root->addWidget(separator(Qt::Horizontal, this));

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter, 1);
    m_center = new QStackedWidget(splitter);
    m_view = new QListView(m_center);
    m_view->setModel(m_filter);
    m_view->setViewMode(QListView::IconMode);
    m_view->setResizeMode(QListView::Adjust);
    m_view->setMovement(QListView::Static);
    m_view->setUniformItemSizes(true);
    m_view->setSpacing(0);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setDragEnabled(true);
    m_view->setDragDropMode(QAbstractItemView::DragOnly);
    m_view->setMouseTracking(true);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setItemDelegate(new AssetDelegate(&s_tileSize, m_view));
    m_view->setStyleSheet(QStringLiteral("QListView { background: %1; padding: 10px; }").arg(Theme::palette().window.name()));
    auto *gallery = new QWidget(m_center);
    auto *gv = new QVBoxLayout(gallery);
    gv->setContentsMargins(0, 0, 0, 0);
    gv->addWidget(m_view, 1);
    auto *empty = new QLabel(tr("<h3>No images yet</h3><p>Generated images, edits, references and imports of this project appear here.<br>"
                                "Drag images in, use Import, or create something on the Generate and Create pages.</p>"), gallery);
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::palette().textMuted.name()));
    gv->addWidget(empty, 1);
    auto updateEmpty = [this, empty] {
        const bool none = m_filter->rowCount() == 0;
        empty->setVisible(none);
        m_view->setVisible(!none);
    };
    connect(m_filter, &QAbstractItemModel::rowsInserted, this, updateEmpty);
    connect(m_filter, &QAbstractItemModel::rowsRemoved, this, updateEmpty);
    connect(m_filter, &QAbstractItemModel::modelReset, this, updateEmpty);
    connect(m_filter, &QAbstractItemModel::layoutChanged, this, updateEmpty);
    QTimer::singleShot(0, this, updateEmpty);
    m_center->addWidget(gallery);
    m_docsView = new QWidget(m_center);
    m_center->addWidget(m_docsView);

    // Inspector.
    // Inspector: scrolls when the window is too short for the preview and every field.
    auto *inspector = new QWidget;
    inspector->setMinimumWidth(300);
    auto *iv = new QVBoxLayout(inspector);
    iv->setContentsMargins(14, 14, 14, 14);
    iv->setSpacing(10);
    m_preview = new QLabel(inspector);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(200);
    m_preview->setStyleSheet(QStringLiteral("background: %1; border-radius: 10px;").arg(Theme::palette().canvas.name()));
    iv->addWidget(m_preview);
    auto *nameRow = new QHBoxLayout;
    m_name = new QLineEdit(inspector);
    m_name->setAccessibleName(tr("Image name"));
    m_name->setPlaceholderText(tr("Name"));
    m_star = iconButton(QStringLiteral("star"), tr("Favourite"), inspector, true);
    nameRow->addWidget(m_name, 1);
    nameRow->addWidget(m_star);
    iv->addLayout(nameRow);
    m_meta = mutedLabel(QString(), inspector);
    m_meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    iv->addWidget(m_meta);
    iv->addWidget(sectionLabel(tr("Prompt")));
    m_prompt = new QPlainTextEdit(inspector);
    m_prompt->setReadOnly(true);
    m_prompt->setAccessibleName(tr("Prompt"));
    m_prompt->setMaximumHeight(110);
    iv->addWidget(m_prompt);
    iv->addWidget(sectionLabel(tr("Parameters")));
    m_params = new QPlainTextEdit(inspector);
    m_params->setReadOnly(true);
    m_params->setAccessibleName(tr("Parameters"));
    iv->addWidget(m_params, 1);
    auto *actions = new QGridLayout;
    actions->setSpacing(6);
    auto *open = new QPushButton(Icons::get(QStringLiteral("brush")), tr("Open in editor"), inspector);
    Theme::setRole(open, QStringLiteral("primary"));
    auto *asRef = new QPushButton(Icons::get(QStringLiteral("link")), tr("Use as reference"), inspector);
    auto *usePrompt = new QPushButton(Icons::get(QStringLiteral("text")), tr("Reuse prompt"), inspector);
    auto *rerun = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Re-run"), inspector);
    auto *exportBtn = new QPushButton(Icons::get(QStringLiteral("download")), tr("Export…"), inspector);
    auto *del = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Delete"), inspector);
    Theme::setRole(del, QStringLiteral("danger"));
    actions->addWidget(open, 0, 0, 1, 2);
    actions->addWidget(asRef, 1, 0);
    actions->addWidget(usePrompt, 1, 1);
    actions->addWidget(rerun, 2, 0);
    actions->addWidget(exportBtn, 2, 1);
    actions->addWidget(del, 3, 0, 1, 2);
    iv->addLayout(actions);
    splitter->addWidget(m_center);
    m_inspector = verticalScroll(inspector, splitter);
    m_inspector->setObjectName(QStringLiteral("Panel"));
    splitter->addWidget(m_inspector);
    splitter->setStretchFactor(0, 1);
    splitter->setSizes({1000, 360});

    // Wiring.
    connect(m_kinds, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == kDocumentsFilter) {
            rebuildDocuments();
            m_center->setCurrentWidget(m_docsView);
            m_inspector->hide();
            return;
        }
        m_center->setCurrentIndex(0);
        m_inspector->show();
        m_filter->setKind(id == kAllFilter ? -1 : id);
    });
    connect(m_favorites, &QToolButton::toggled, m_filter, &AssetFilter::setFavoritesOnly);
    connect(m_search, &QLineEdit::textChanged, m_filter, &AssetFilter::setSearch);
    connect(m_size, &QSlider::valueChanged, this, [this](int v) {
        s_tileSize = v;
        m_view->setGridSize(QSize(v + 12, v + 12));
        m_view->doItemsLayout();
    });
    m_view->setGridSize(QSize(s_tileSize + 12, s_tileSize + 12));
    connect(import, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import images"), QString(),
                                                                tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
        for (const QString &f : files) {
            if (m_studio->project())
                m_studio->project()->importFile(f).then([](doc::AssetInfo) {}).onFailed(this, [this](const std::exception &e) {
                    m_studio->notify(describeException(e), QStringLiteral("error"));
                });
        }
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryPage::showInspector);
    connect(m_view, &QListView::doubleClicked, this, [this](const QModelIndex &idx) {
        m_studio->openAssetInEditor(idx.data(AssetModel::IdRole).toString());
    });
    connect(open, &QPushButton::clicked, this, [this] {
        for (const QString &id : selectedIds())
            m_studio->openAssetInEditor(id);
    });
    connect(asRef, &QPushButton::clicked, this, [this] {
        for (const QString &id : selectedIds())
            m_studio->useAsReference(id);
        m_studio->notify(tr("Added to the references of the Generate page."));
    });
    connect(usePrompt, &QPushButton::clicked, this, [this] {
        const auto info = m_studio->project() ? m_studio->project()->asset(m_currentId) : std::nullopt;
        if (info) {
            m_studio->usePrompt(info->prompt(), info->metadata.value(QStringLiteral("negative_prompt")).toString());
            m_studio->showPage(pages::Generate);
        }
    });
    connect(rerun, &QPushButton::clicked, this, [this] {
        if (!m_currentId.isEmpty()) {
            m_studio->rerunAsset(m_currentId);
            m_studio->showPage(pages::Generate);
        }
    });
    connect(exportBtn, &QPushButton::clicked, this, &LibraryPage::exportSelected);
    connect(del, &QPushButton::clicked, this, &LibraryPage::deleteSelected);
    connect(m_star, &QToolButton::toggled, this, [this](bool on) {
        doc::Project *p = m_studio->project();
        if (!p || m_currentId.isEmpty())
            return;
        if (auto info = p->asset(m_currentId); info && info->favorite != on) {
            info->favorite = on;
            p->updateAsset(*info);
        }
        m_star->setIcon(Icons::get(on ? QStringLiteral("star-filled") : QStringLiteral("star")));
    });
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        doc::Project *p = m_studio->project();
        if (!p || m_currentId.isEmpty())
            return;
        if (auto info = p->asset(m_currentId); info && info->name != m_name->text()) {
            info->name = m_name->text();
            p->updateAsset(*info);
        }
    });
    connect(m_view, &QListView::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (selectedIds().isEmpty())
            return;
        QMenu menu(this);
        menu.addAction(Icons::get(QStringLiteral("brush")), tr("Open in editor"), this, [this] {
            for (const QString &id : selectedIds())
                m_studio->openAssetInEditor(id);
        });
        menu.addAction(Icons::get(QStringLiteral("link")), tr("Use as reference"), this, [this] {
            for (const QString &id : selectedIds())
                m_studio->useAsReference(id);
        });
        if (m_studio->hasPage(QStringLiteral("parts"))) {
            menu.addAction(Icons::get(QStringLiteral("parts")), tr("Slice into parts (Live2D)"), this, [this] {
                m_studio->sliceAsset(selectedIds().first());
            });
        }
        const int count = int(selectedIds().size());
        const QString suffix = count > 1 ? tr(" (%n images)", nullptr, count) : QString();
        menu.addAction(Icons::get(QStringLiteral("scissors")), tr("Remove background") + suffix, this, [this] {
            runBatchUtility(ai::Utility::RemoveBackground, {}, tr("Remove background"));
        });
        menu.addAction(Icons::get(QStringLiteral("upscale")), tr("Upscale 2×") + suffix, this, [this] {
            runBatchUtility(ai::Utility::Upscale, QJsonObject{{QStringLiteral("factor"), 2}}, tr("Upscale 2×"));
        });
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("copy")), tr("Copy image"), this, [this] {
            if (doc::Project *p = m_studio->project())
                p->loadImage(selectedIds().first()).then(this, [](QImage img) { QApplication::clipboard()->setImage(img); });
        });
        menu.addAction(Icons::get(QStringLiteral("download")), tr("Export…"), this, &LibraryPage::exportSelected);
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("trash")), tr("Delete"), this, &LibraryPage::deleteSelected);
        menu.exec(m_view->viewport()->mapToGlobal(pos));
    });

    auto bindProject = [this] {
        m_thumbs->setProject(m_studio->project());
        m_model->setProject(m_studio->project());
        if (doc::Project *p = m_studio->project())
            connect(p, &doc::Project::documentsChanged, this, [this] {
                if (m_center->currentWidget() == m_docsView)
                    rebuildDocuments();
            });
    };
    connect(studio, &Studio::projectOpened, this, bindProject);
    bindProject();
    showInspector();
}

void LibraryPage::runBatchUtility(ai::Utility utility, const QJsonObject &params, const QString &title)
{
    ai::GenerationService *gen = m_studio->generation();
    if (!gen->providerForUtility(utility)) {
        m_studio->notify(tr("No configured provider can run \"%1\". Add SD WebUI (rembg extension), ComfyUI, Stability AI or fal.ai "
                            "in Settings → Providers.")
                             .arg(title),
                         QStringLiteral("warning"));
        return;
    }
    const QStringList ids = selectedIds();
    doc::Project *project = m_studio->project();
    for (const QString &id : ids) {
        const auto info = project ? project->asset(id) : std::nullopt;
        const QString name = info ? QStringLiteral("%1: %2").arg(title, info->name) : title;
        gen->submitUtilityForAsset(QString(), utility, id, params, name);
    }
    m_studio->notify(tr("Queued %n job(s). The results appear in the library.", nullptr, int(ids.size())), QStringLiteral("info"));
}

QStringList LibraryPage::selectedIds() const
{
    QStringList ids;
    for (const QModelIndex &i : m_view->selectionModel()->selectedIndexes())
        ids << i.data(AssetModel::IdRole).toString();
    return ids;
}

void LibraryPage::showInspector()
{
    const QStringList ids = selectedIds();
    doc::Project *project = m_studio->project();
    m_currentId = ids.isEmpty() ? QString() : ids.first();
    const auto info = project && !m_currentId.isEmpty() ? project->asset(m_currentId) : std::nullopt;
    for (QWidget *w : m_inspector->findChildren<QPushButton *>())
        w->setEnabled(info.has_value());
    m_name->setEnabled(info.has_value());
    m_star->setEnabled(info.has_value());
    if (!info) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(tr("Select an image"));
        m_name->clear();
        m_meta->clear();
        m_prompt->clear();
        m_params->clear();
        return;
    }
    m_name->setText(info->name);
    {
        QSignalBlocker b(m_star);
        m_star->setChecked(info->favorite);
        m_star->setIcon(Icons::get(info->favorite ? QStringLiteral("star-filled") : QStringLiteral("star")));
    }
    QStringList meta;
    meta << doc::assetKindLabel(info->kind) << QStringLiteral("%1 × %2").arg(info->size.width()).arg(info->size.height())
         << QLocale().toString(info->created, QLocale::ShortFormat);
    const QString provider = info->metadata.value(QStringLiteral("provider")).toString();
    if (!provider.isEmpty())
        meta << provider;
    if (ids.size() > 1)
        meta << tr("%1 selected").arg(ids.size());
    m_meta->setText(meta.join(QStringLiteral("  ·  ")));
    m_prompt->setPlainText(info->prompt());
    QJsonObject params = info->metadata;
    params.remove(QStringLiteral("prompt"));
    params.remove(QStringLiteral("parameters"));
    m_params->setPlainText(info->metadata.value(QStringLiteral("parameters")).toString().isEmpty()
                               ? json::pretty(params)
                               : info->metadata.value(QStringLiteral("parameters")).toString());
    m_preview->setText(tr("Loading…"));
    const quint64 generation = ++m_previewGeneration;
    QPointer<LibraryPage> self(this);
    const QSize box = m_preview->size();
    project->loadImage(info->id).then(this, [self, generation, box](QImage img) {
        if (!self || generation != self->m_previewGeneration)
            return;
        const QPixmap pm = QPixmap::fromImage(img.scaled(box * self->devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        QPixmap scaled = pm;
        scaled.setDevicePixelRatio(self->devicePixelRatioF());
        self->m_preview->setPixmap(scaled);
    });
}

void LibraryPage::exportSelected()
{
    doc::Project *project = m_studio->project();
    const QStringList ids = selectedIds();
    if (!project || ids.isEmpty())
        return;
    if (ids.size() == 1) {
        const auto info = project->asset(ids.first());
        const QString path = QFileDialog::getSaveFileName(this, tr("Export image"), (info ? info->name : QStringLiteral("image")) + QStringLiteral(".png"),
                                                          tr("PNG (*.png);;JPEG (*.jpg);;WebP (*.webp)"));
        if (path.isEmpty())
            return;
        project->exportAsset(ids.first(), path)
            .then(this, [this] { m_studio->notify(tr("Exported."), QStringLiteral("success")); })
            .onFailed(this, [this](const std::exception &e) { m_studio->notify(describeException(e), QStringLiteral("error")); });
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export %n images to folder", nullptr, int(ids.size())));
    if (dir.isEmpty())
        return;
    int n = 0;
    for (const QString &id : ids) {
        const auto info = project->asset(id);
        QString base = info ? info->name : id;
        base.replace(QRegularExpression(QStringLiteral("[^\\w\\- ]+")), QStringLiteral("_"));
        project->exportAsset(id, QStringLiteral("%1/%2_%3.png").arg(dir, base.left(40)).arg(++n));
    }
    m_studio->notify(tr("Exporting %n image(s)…", nullptr, int(ids.size())));
}

void LibraryPage::deleteSelected()
{
    doc::Project *project = m_studio->project();
    const QStringList ids = selectedIds();
    if (!project || ids.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Delete"), tr("Delete %n image(s) from the project? This cannot be undone.", nullptr, int(ids.size())))
        != QMessageBox::Yes)
        return;
    project->removeAssets(ids);
}

void LibraryPage::rebuildDocuments()
{
    delete m_docsView->layout();
    qDeleteAll(m_docsView->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly));
    auto *v = new QVBoxLayout(m_docsView);
    v->setContentsMargins(16, 16, 16, 16);
    auto *list = new QListWidget(m_docsView);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(200, 200));
    list->setGridSize(QSize(230, 250));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setWordWrap(true);
    v->addWidget(mutedLabel(tr("Layered documents saved in this project. Double-click to open."), m_docsView));
    v->addWidget(list, 1);
    if (doc::Project *p = m_studio->project()) {
        for (const doc::DocumentInfo &d : p->documents()) {
            auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(d.thumbnail)),
                                             QStringLiteral("%1\n%2 × %3").arg(d.name.isEmpty() ? tr("Untitled") : d.name).arg(d.size.width()).arg(d.size.height()),
                                             list);
            item->setData(Qt::UserRole, d.id);
            item->setToolTip(QLocale().toString(d.modified, QLocale::ShortFormat));
        }
    }
    connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        m_studio->openStoredDocument(item->data(Qt::UserRole).toString());
    });
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QListWidget::customContextMenuRequested, this, [this, list](const QPoint &pos) {
        QListWidgetItem *item = list->itemAt(pos);
        if (!item)
            return;
        QMenu menu(this);
        menu.addAction(tr("Open"), this, [this, item] { m_studio->openStoredDocument(item->data(Qt::UserRole).toString()); });
        menu.addAction(Icons::get(QStringLiteral("trash")), tr("Delete document"), this, [this, item] {
            if (QMessageBox::question(this, tr("Delete document"), tr("Delete this layered document?")) == QMessageBox::Yes)
                m_studio->project()->removeDocument(item->data(Qt::UserRole).toString());
        });
        menu.exec(list->viewport()->mapToGlobal(pos));
    });
}

} // namespace lumen::ui
