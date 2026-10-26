#include "ui/pages/StartScreen.h"

#include "core/Async.h"
#include "core/Paths.h"
#include "core/Psd.h"
#include "core/Settings.h"
#include "doc/Project.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lumen::ui {

namespace {

// Documents first (they keep their layers), then images, at most this many in all; the files
// opened from disk have a row of their own.
constexpr int kMaxRecent = 12;
constexpr int kMaxDocuments = 6;
constexpr int kMaxFiles = 6;
constexpr int kTileIcon = 112;

// A thumbnail centred in a square, so every tile's name sits at the same height. A document (it
// keeps its layers) gets a small "Layers" badge in its corner.
QIcon tileIcon(const QPixmap &thumbnail, bool document = false)
{
    QPixmap square(kTileIcon, kTileIcon);
    square.fill(Qt::transparent);
    const QPixmap scaled = thumbnail.scaled(kTileIcon, kTileIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&square);
    p.drawPixmap((kTileIcon - scaled.width()) / 2, (kTileIcon - scaled.height()) / 2, scaled);
    if (document) {
        const Palette &pal = Theme::palette();
        QFont font = p.font();
        font.setPointSizeF(7.5);
        font.setBold(true);
        p.setFont(font);
        const QString text = StartScreen::tr("Layers");
        const QRect badge(4, kTileIcon - 22, p.fontMetrics().horizontalAdvance(text) + 26, 18);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.accent);
        p.drawRoundedRect(badge, 5, 5);
        p.drawPixmap(badge.left() + 4, badge.top() + 2, Icons::pixmap(QStringLiteral("layers"), 14, pal.accentText));
        p.setPen(pal.accentText);
        p.drawText(badge.adjusted(21, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    p.end();
    return QIcon(square);
}

// A name on two lines at most, shortened with "…" in the second (at its end, or in its middle so
// a file name keeps its end and its extension).
QString twoLines(const QFontMetrics &fm, const QString &name, int width, Qt::TextElideMode elide)
{
    if (fm.horizontalAdvance(name) <= width)
        return name;
    // The first line takes whole words while they fit (or as much of one long word as fits).
    const QStringList words = name.split(u' ', Qt::SkipEmptyParts);
    QString first;
    int used = 0;
    for (const QString &w : words) {
        const QString next = first.isEmpty() ? w : first + u' ' + w;
        if (fm.horizontalAdvance(next) > width)
            break;
        first = next;
        ++used;
    }
    if (first.isEmpty()) {
        int n = name.size();
        while (n > 1 && fm.horizontalAdvance(name.left(n)) > width)
            --n;
        return name.left(n) + u'\n' + fm.elidedText(name.mid(n), elide, width);
    }
    return first + u'\n' + fm.elidedText(words.mid(used).join(u' '), elide, width);
}

QPushButton *actionButton(const QString &icon, const QString &text, const QString &tip, QWidget *parent)
{
    auto *b = new QPushButton(Icons::get(icon), text, parent);
    b->setIconSize(QSize(20, 20));
    b->setMinimumHeight(44);
    b->setToolTip(tip);
    return b;
}

} // namespace

StartScreen::StartScreen(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_thumbs(new ThumbnailCache(this))
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *content = new QWidget;
    auto *scroll = verticalScroll(content, this);
    outer->addWidget(scroll);
    // A column of readable width in the middle of the window.
    auto *center = new QHBoxLayout(content);
    center->setContentsMargins(24, 28, 24, 24);
    auto *column = new QWidget(content);
    column->setMaximumWidth(960);
    center->addStretch(1);
    center->addWidget(column, 100);
    center->addStretch(1);
    auto *v = new QVBoxLayout(column);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(14);

    auto *head = new QHBoxLayout;
    head->setSpacing(12);
    auto *logo = new QLabel(column);
    logo->setPixmap(Icons::pixmap(QStringLiteral("logo"), 40, Theme::palette().accent));
    head->addWidget(logo);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("Lumen Studio"), column);
    Theme::setRole(title, QStringLiteral("title"));
    titles->addWidget(title);
    titles->addWidget(mutedLabel(tr("Open a picture to change it with words, or start a new one."), column));
    head->addLayout(titles, 1);
    v->addLayout(head);

    auto *actions = new QWidget(column);
    auto *flow = new FlowLayout(actions, 8, 8);
    flow->setContentsMargins(0, 0, 0, 0);
    auto *open = actionButton(QStringLiteral("image"), tr("Open image…"), tr("Open a photo or picture from this computer (Ctrl+O)"), actions);
    Theme::setRole(open, QStringLiteral("primary"));
    auto *paste = actionButton(QStringLiteral("copy"), tr("Paste from clipboard"), tr("Open the picture you copied as a new document"), actions);
    auto *prompt = actionButton(QStringLiteral("sparkles"), tr("New from prompt…"), tr("Describe a picture and let the AI draw it"), actions);
    auto *blank = actionButton(QStringLiteral("plus"), tr("New blank canvas…"), tr("An empty canvas of the size you choose (Ctrl+N)"), actions);
    for (QPushButton *b : {open, paste, prompt, blank})
        flow->addWidget(b);
    v->addWidget(actions);

    // While no image engine is configured, editing with words cannot work yet.
    m_setupCard = card(column);
    m_setupCard->setObjectName(QStringLiteral("setUpAiCard"));
    auto *sh = new QHBoxLayout(m_setupCard);
    sh->setContentsMargins(16, 14, 16, 14);
    sh->setSpacing(14);
    auto *spark = new QLabel(m_setupCard);
    spark->setPixmap(Icons::pixmap(QStringLiteral("sparkles"), 28, Theme::palette().accent));
    sh->addWidget(spark, 0, Qt::AlignTop);
    auto *sv = new QVBoxLayout;
    sv->setSpacing(4);
    auto *setupTitle = new QLabel(tr("Set up AI"), m_setupCard);
    Theme::setRole(setupTitle, QStringLiteral("heading"));
    sv->addWidget(setupTitle);
    sv->addWidget(mutedLabel(tr("To change pictures with words, Lumen needs an image engine: a model that runs on this computer, "
                                "or an online service."),
                             m_setupCard));
    sh->addLayout(sv, 1);
    auto *setup = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Set up AI…"), m_setupCard);
    Theme::setRole(setup, QStringLiteral("primary"));
    sh->addWidget(setup, 0, Qt::AlignVCenter);
    v->addWidget(m_setupCard);

    // Pictures opened from disk or saved as (File → Open recent); a click opens one again.
    m_filesBox = new QWidget(column);
    m_filesBox->setObjectName(QStringLiteral("recentFiles"));
    auto *fv = new QVBoxLayout(m_filesBox);
    fv->setContentsMargins(0, 8, 0, 0);
    fv->setSpacing(8);
    fv->addWidget(sectionLabel(tr("Recent files"), m_filesBox));
    m_fileTilesHost = new QWidget(m_filesBox);
    m_fileTiles = new FlowLayout(m_fileTilesHost, 8, 8);
    m_fileTiles->setContentsMargins(0, 0, 0, 0);
    fv->addWidget(m_fileTilesHost);
    v->addWidget(m_filesBox);

    m_recentBox = new QWidget(column);
    auto *rv = new QVBoxLayout(m_recentBox);
    rv->setContentsMargins(0, 8, 0, 0);
    rv->setSpacing(8);
    auto *recentHead = new QHBoxLayout;
    recentHead->addWidget(sectionLabel(tr("Recent in this project"), m_recentBox));
    recentHead->addStretch();
    auto *browse = new QPushButton(Icons::get(QStringLiteral("gallery")), tr("Browse project images…"), m_recentBox);
    Theme::setRole(browse, QStringLiteral("ghost"));
    browse->setToolTip(tr("Every image of this project, with background removal and upscaling (Alt+6)"));
    recentHead->addWidget(browse);
    rv->addLayout(recentHead);
    m_tilesHost = new QWidget(m_recentBox);
    m_tiles = new FlowLayout(m_tilesHost, 8, 8);
    m_tiles->setContentsMargins(0, 0, 0, 0);
    rv->addWidget(m_tilesHost);
    m_noRecent = mutedLabel(tr("The documents and images of this project appear here."), m_recentBox);
    rv->addWidget(m_noRecent);
    v->addWidget(m_recentBox);
    v->addWidget(mutedLabel(tr("Tip: drop an image file here to open it."), column));
    v->addStretch();

    connect(open, &QPushButton::clicked, this, &StartScreen::openImageRequested);
    connect(paste, &QPushButton::clicked, this, &StartScreen::pasteRequested);
    connect(prompt, &QPushButton::clicked, this, &StartScreen::newFromPromptRequested);
    connect(blank, &QPushButton::clicked, this, &StartScreen::newCanvasRequested);
    connect(browse, &QPushButton::clicked, this, &StartScreen::browseRequested);
    connect(setup, &QPushButton::clicked, this, &StartScreen::setUpAiRequested);

    m_rebuild.setSingleShot(true);
    m_rebuild.setInterval(0);
    connect(&m_rebuild, &QTimer::timeout, this, &StartScreen::rebuildRecent);
    auto bindProject = [this] {
        doc::Project *project = m_studio->project();
        m_thumbs->setProject(project);
        if (project) {
            connect(project, &doc::Project::documentsChanged, this, &StartScreen::scheduleRebuild, Qt::UniqueConnection);
            connect(project, &doc::Project::assetAdded, this, &StartScreen::scheduleRebuild, Qt::UniqueConnection);
            connect(project, &doc::Project::assetChanged, this, &StartScreen::scheduleRebuild, Qt::UniqueConnection);
            connect(project, &doc::Project::assetsRemoved, this, &StartScreen::scheduleRebuild, Qt::UniqueConnection);
        }
        scheduleRebuild();
    };
    connect(studio, &Studio::projectOpened, this, bindProject);
    connect(studio, &Studio::projectClosed, this, &StartScreen::scheduleRebuild);
    connect(m_thumbs, &ThumbnailCache::ready, this, [this](const QString &id) {
        for (QToolButton *tile : recentTiles()) {
            if (tile->property("assetId").toString() == id) {
                const QPixmap pm = m_thumbs->get(id);
                if (!pm.isNull())
                    tile->setIcon(tileIcon(pm));
            }
        }
    });
    connect(studio->providers(), &ai::ProviderRegistry::providersChanged, this, &StartScreen::updateSetupCard);
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String("app/recentFiles"))
            scheduleRebuild();
    });
    // Files may have been deleted or changed meanwhile, in another program.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive)
            scheduleRebuild();
    });
    bindProject();
    updateSetupCard();
}

namespace {

QList<QToolButton *> tilesOf(const FlowLayout *layout)
{
    QList<QToolButton *> tiles;
    for (int i = 0; i < layout->count(); ++i) {
        if (auto *b = qobject_cast<QToolButton *>(layout->itemAt(i)->widget()))
            tiles << b;
    }
    return tiles;
}

} // namespace

QList<QToolButton *> StartScreen::recentTiles() const
{
    return tilesOf(m_tiles);
}

QList<QToolButton *> StartScreen::recentFileTiles() const
{
    return tilesOf(m_fileTiles);
}

QString StartScreen::thumbnailKey(const QString &path)
{
    return path + u'|' + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch());
}

void StartScreen::loadFileThumbnail(const QString &path)
{
    const QString key = thumbnailKey(path);
    if (m_fileThumbs.contains(key) || m_loading.contains(key))
        return;
    m_loading.insert(key);
    const int side = kTileIcon * 2;
    async::run([path, side] {
        QImage image;
        if (QFileInfo(path).suffix().compare(QLatin1String("psd"), Qt::CaseInsensitive) == 0) {
            if (const std::optional<psd::File> file = psd::read(path))
                image = file->composite;
        } else {
            // Decoded at about the tile's size: quick also for a large photo.
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.width() > side || size.height() > side)
                reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatio));
            image = reader.read();
        }
        if (!image.isNull() && (image.width() > side || image.height() > side))
            image = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return image;
    }).then(this, [this, path, key](const QImage &image) {
        m_loading.remove(key);
        const QPixmap pm = QPixmap::fromImage(image);
        m_fileThumbs.insert(key, pm);
        for (QToolButton *tile : recentFileTiles()) {
            if (tile->property("filePath").toString() == path && !pm.isNull())
                tile->setIcon(tileIcon(pm));
        }
    });
}

void StartScreen::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_dirty)
        rebuildRecent();
}

void StartScreen::scheduleRebuild()
{
    m_dirty = true;
    // Built when it is seen: while documents are open, the editor covers it.
    if (isVisible())
        m_rebuild.start();
}

void StartScreen::updateSetupCard()
{
    m_setupCard->setVisible(!imageEngineInUse(m_studio->providers()));
}

QToolButton *StartScreen::addTile(FlowLayout *tiles, const QString &name, const QString &tooltip, const QPixmap &thumbnail, bool document,
                                   Qt::TextElideMode elide)
{
    auto *tile = new QToolButton(tiles->parentWidget());
    tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tile->setIconSize(QSize(kTileIcon, kTileIcon));
    tile->setFixedSize(kTileIcon + 24, kTileIcon + 40 + tile->fontMetrics().lineSpacing());
    tile->setAutoRaise(true);
    const QString shown = name.isEmpty() ? tr("Untitled") : name;
    // The name shortened to what the button has room for besides its padding (measured with a text
    // wider than the icon).
    const QString wide(40, u'M');
    tile->setText(wide);
    const int padding = tile->sizeHint().width() - tile->fontMetrics().horizontalAdvance(wide);
    tile->setText(twoLines(tile->fontMetrics(), shown, tile->width() - padding - 2, elide));
    tile->setAccessibleName(shown);
    tile->setToolTip(QStringLiteral("%1\n%2").arg(shown, tooltip));
    tile->setIcon(thumbnail.isNull() ? Icons::get(QStringLiteral("image")) : tileIcon(thumbnail, document));
    tiles->addWidget(tile);
    return tile;
}

void StartScreen::rebuildRecent()
{
    m_dirty = false;
    for (FlowLayout *layout : {m_tiles, m_fileTiles}) {
        while (QLayoutItem *item = layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    }
    // The files first, those that are still there (File → Open recent shows the others greyed).
    QStringList files;
    for (const QString &path : Settings::instance().recentFiles()) {
        if (files.size() < kMaxFiles && QFileInfo::exists(path))
            files << path;
    }
    for (const QString &path : std::as_const(files)) {
        const QFileInfo info(path);
        const QString tip = tr("%1\nChanged %2").arg(QDir::toNativeSeparators(path), QLocale().toString(info.lastModified(), QLocale::ShortFormat));
        // Names of photos often differ only near the end (IMG_20261026_101523.jpg).
        QToolButton *tile = addTile(m_fileTiles, info.fileName(), tip, m_fileThumbs.value(thumbnailKey(path)), false, Qt::ElideMiddle);
        tile->setProperty("filePath", path);
        connect(tile, &QToolButton::clicked, this, [this, path] { m_studio->openFile(path); });
        loadFileThumbnail(path);
    }
    m_filesBox->setVisible(!files.isEmpty());

    doc::Project *project = m_studio->project();
    if (project && project->isOpen()) {
        QList<doc::DocumentInfo> documents = project->documents();
        std::sort(documents.begin(), documents.end(), [](const doc::DocumentInfo &a, const doc::DocumentInfo &b) { return a.modified > b.modified; });
        // A document that is just a file above (unchanged since it was saved to it or read from it)
        // opens from that file's tile: Studio::openFile takes the document with its layers.
        documents.removeIf([&files](const doc::DocumentInfo &d) {
            if (d.fileModified || !d.file.isUnchangedOnDisk())
                return false;
            return std::any_of(files.cbegin(), files.cend(), [&d](const QString &f) { return paths::sameFile(f, d.file.path); });
        });
        for (const doc::DocumentInfo &d : documents.mid(0, kMaxDocuments)) {
            const QString tip = tr("Document with layers · %1 × %2 px · %3")
                                    .arg(d.size.width())
                                    .arg(d.size.height())
                                    .arg(QLocale().toString(d.modified, QLocale::ShortFormat));
            QToolButton *tile = addTile(m_tiles, d.name, tip, QPixmap::fromImage(d.thumbnail), true);
            tile->setProperty("documentId", d.id);
            const QString id = d.id;
            connect(tile, &QToolButton::clicked, this, [this, id] { m_studio->openStoredDocument(id); });
        }
        // Images, newest first; sliced character parts are intermediate results, and so are the
        // results of a prompt run nobody kept (they wait in Browse project images).
        QList<doc::AssetInfo> images = project->assets();
        images.removeIf([](const doc::AssetInfo &a) {
            const bool unkept = a.metadata.contains(QStringLiteral("user_prompt")) && !a.metadata.value(QStringLiteral("kept")).toBool();
            return a.kind == doc::AssetKind::Part || unkept;
        });
        std::sort(images.begin(), images.end(), [](const doc::AssetInfo &a, const doc::AssetInfo &b) { return a.created > b.created; });
        for (const doc::AssetInfo &a : images.mid(0, kMaxRecent - int(m_tiles->count()))) {
            const QString tip = tr("%1 · %2 × %3 px · %4")
                                    .arg(doc::assetKindLabel(a.kind))
                                    .arg(a.size.width())
                                    .arg(a.size.height())
                                    .arg(QLocale().toString(a.created, QLocale::ShortFormat));
            QToolButton *tile = addTile(m_tiles, a.name, a.prompt().isEmpty() ? tip : tip + u'\n' + a.prompt().left(200), m_thumbs->get(a.id));
            tile->setProperty("assetId", a.id);
            const QString id = a.id;
            connect(tile, &QToolButton::clicked, this, [this, id] { m_studio->openAssetInEditor(id); });
        }
    }
    const bool any = m_tiles->count() > 0;
    m_tilesHost->setVisible(any);
    m_noRecent->setVisible(!any);
}

} // namespace lumen::ui
