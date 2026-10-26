#include "ui/pages/StartScreen.h"

#include "doc/Project.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/FlowLayout.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ThumbnailCache.h"

#include <QHBoxLayout>
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

// Documents first (they keep their layers), then images, at most this many in all.
constexpr int kMaxRecent = 12;
constexpr int kMaxDocuments = 6;
constexpr int kTileIcon = 112;

// A thumbnail centred in a square, so every tile's name sits at the same height.
QIcon tileIcon(const QPixmap &thumbnail)
{
    QPixmap square(kTileIcon, kTileIcon);
    square.fill(Qt::transparent);
    const QPixmap scaled = thumbnail.scaled(kTileIcon, kTileIcon, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&square);
    p.drawPixmap((kTileIcon - scaled.width()) / 2, (kTileIcon - scaled.height()) / 2, scaled);
    p.end();
    return QIcon(square);
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
    bindProject();
    updateSetupCard();
}

QList<QToolButton *> StartScreen::recentTiles() const
{
    QList<QToolButton *> tiles;
    for (int i = 0; i < m_tiles->count(); ++i) {
        if (auto *b = qobject_cast<QToolButton *>(m_tiles->itemAt(i)->widget()))
            tiles << b;
    }
    return tiles;
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

QToolButton *StartScreen::addTile(const QString &name, const QString &tooltip, const QPixmap &thumbnail)
{
    auto *tile = new QToolButton(m_tilesHost);
    tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tile->setIconSize(QSize(kTileIcon, kTileIcon));
    tile->setFixedSize(kTileIcon + 24, kTileIcon + 40);
    tile->setAutoRaise(true);
    const QString shown = name.isEmpty() ? tr("Untitled") : name;
    // The name shortened to what the button has room for besides its padding (measured with a text
    // wider than the icon).
    const QString wide(40, u'M');
    tile->setText(wide);
    const int padding = tile->sizeHint().width() - tile->fontMetrics().horizontalAdvance(wide);
    tile->setText(tile->fontMetrics().elidedText(shown, Qt::ElideRight, tile->width() - padding - 2));
    tile->setAccessibleName(shown);
    tile->setToolTip(QStringLiteral("%1\n%2").arg(shown, tooltip));
    tile->setIcon(thumbnail.isNull() ? Icons::get(QStringLiteral("image")) : tileIcon(thumbnail));
    m_tiles->addWidget(tile);
    return tile;
}

void StartScreen::rebuildRecent()
{
    m_dirty = false;
    while (QLayoutItem *item = m_tiles->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    doc::Project *project = m_studio->project();
    if (project && project->isOpen()) {
        QList<doc::DocumentInfo> documents = project->documents();
        std::sort(documents.begin(), documents.end(), [](const doc::DocumentInfo &a, const doc::DocumentInfo &b) { return a.modified > b.modified; });
        for (const doc::DocumentInfo &d : documents.mid(0, kMaxDocuments)) {
            const QString tip = tr("Document with layers · %1 × %2 px · %3")
                                    .arg(d.size.width())
                                    .arg(d.size.height())
                                    .arg(QLocale().toString(d.modified, QLocale::ShortFormat));
            QToolButton *tile = addTile(d.name, tip, QPixmap::fromImage(d.thumbnail));
            tile->setProperty("documentId", d.id);
            const QString id = d.id;
            connect(tile, &QToolButton::clicked, this, [this, id] { m_studio->openStoredDocument(id); });
        }
        // Images, newest first; sliced character parts are intermediate results.
        QList<doc::AssetInfo> images = project->assets();
        images.removeIf([](const doc::AssetInfo &a) { return a.kind == doc::AssetKind::Part; });
        std::sort(images.begin(), images.end(), [](const doc::AssetInfo &a, const doc::AssetInfo &b) { return a.created > b.created; });
        for (const doc::AssetInfo &a : images.mid(0, kMaxRecent - int(m_tiles->count()))) {
            const QString tip = tr("%1 · %2 × %3 px · %4")
                                    .arg(doc::assetKindLabel(a.kind))
                                    .arg(a.size.width())
                                    .arg(a.size.height())
                                    .arg(QLocale().toString(a.created, QLocale::ShortFormat));
            QToolButton *tile = addTile(a.name, a.prompt().isEmpty() ? tip : tip + u'\n' + a.prompt().left(200), m_thumbs->get(a.id));
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
