#include "ui/widgets/ReferenceStrip.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

QPixmap roundedThumb(const QImage &image, int size)
{
    const QImage t = image.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, size, size), 8, 8);
    p.setClipPath(path);
    p.drawImage(QPoint((size - t.width()) / 2, (size - t.height()) / 2), t);
    return pm;
}

} // namespace

ReferenceStrip::ReferenceStrip(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    setAcceptDrops(true);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    m_container = new QWidget(scroll);
    m_items = new QHBoxLayout(m_container);
    m_items->setContentsMargins(0, 0, 0, 0);
    m_items->setSpacing(8);
    scroll->setWidget(m_container);
    outer->addWidget(scroll);
    rebuild();
}

void ReferenceStrip::setMaximum(int max)
{
    m_max = std::max(0, max);
    rebuild();
}

void ReferenceStrip::setCompact(bool compact)
{
    m_compact = compact;
    rebuild();
}

void ReferenceStrip::clear()
{
    m_refs.clear();
    rebuild();
    emit changed();
}

void ReferenceStrip::addImage(const QImage &image, const QString &assetId, ai::ReferenceRole role)
{
    if (image.isNull())
        return;
    ai::ReferenceImage r;
    r.image = img::thumbnail(img::normalized(image), 2048);
    r.assetId = assetId;
    r.role = role;
    r.weight = role == ai::ReferenceRole::Style ? 0.6 : 0.8;
    m_refs.append(r);
    rebuild();
    emit changed();
}

void ReferenceStrip::addAsset(const QString &assetId)
{
    doc::Project *project = m_studio->project();
    if (!project)
        return;
    for (const ai::ReferenceImage &r : std::as_const(m_refs)) {
        if (r.assetId == assetId)
            return;
    }
    QPointer<ReferenceStrip> self(this);
    project->loadImage(assetId)
        .then(this, [self, assetId](QImage image) {
            if (self)
                self->addImage(image, assetId);
        })
        .onFailed(this, [this](const std::exception &e) { m_studio->notify(describeException(e), QStringLiteral("error")); });
}

void ReferenceStrip::importFiles(const QStringList &files)
{
    doc::Project *project = m_studio->project();
    for (const QString &file : files) {
        if (project && project->isOpen()) {
            QPointer<ReferenceStrip> self(this);
            project->importFile(file, doc::AssetKind::Reference)
                .then(this, [self](doc::AssetInfo info) {
                    if (self)
                        self->addAsset(info.id);
                })
                .onFailed(this, [this](const std::exception &e) { m_studio->notify(describeException(e), QStringLiteral("error")); });
        } else {
            QImageReader reader(file);
            reader.setAutoTransform(true);
            addImage(reader.read());
        }
    }
}

void ReferenceStrip::browse()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add reference images"), QString(),
                                                            tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    importFiles(files);
}

void ReferenceStrip::rebuild()
{
    while (QLayoutItem *item = m_items->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const int size = m_compact ? 56 : 84;
    for (int i = 0; i < m_refs.size(); ++i) {
        auto *tile = new QFrame(m_container);
        tile->setProperty("card", QStringLiteral("true"));
        auto *v = new QVBoxLayout(tile);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(4);
        auto *thumb = new QLabel(tile);
        thumb->setPixmap(roundedThumb(m_refs[i].image, size));
        thumb->setFixedSize(size, size);
        thumb->setToolTip(tr("Reference %1 (%2×%3)").arg(i + 1).arg(m_refs[i].image.width()).arg(m_refs[i].image.height()));
        v->addWidget(thumb);
        auto *row = new QHBoxLayout;
        row->setSpacing(2);
        auto *role = new QToolButton(tile);
        role->setText(m_refs[i].role == ai::ReferenceRole::Character ? tr("Character")
                      : m_refs[i].role == ai::ReferenceRole::Style   ? tr("Style")
                                                                     : tr("Other"));
        role->setPopupMode(QToolButton::InstantPopup);
        role->setStyleSheet(QStringLiteral("font-size: 8pt; padding: 1px 3px;"));
        auto *menu = new QMenu(role);
        const QList<QPair<QString, ai::ReferenceRole>> roles = {{tr("Character"), ai::ReferenceRole::Character},
                                                                {tr("Style"), ai::ReferenceRole::Style},
                                                                {tr("Composition"), ai::ReferenceRole::Composition},
                                                                {tr("Other"), ai::ReferenceRole::Other}};
        for (const auto &rr : roles) {
            menu->addAction(rr.first, this, [this, i, rr] {
                if (i < m_refs.size()) {
                    m_refs[i].role = rr.second;
                    rebuild();
                    emit changed();
                }
            });
        }
        menu->addSeparator();
        for (double w : {0.4, 0.6, 0.8, 1.0}) {
            QAction *a = menu->addAction(tr("Weight %1").arg(w), this, [this, i, w] {
                if (i < m_refs.size()) {
                    m_refs[i].weight = w;
                    emit changed();
                }
            });
            a->setCheckable(true);
            a->setChecked(std::abs(m_refs[i].weight - w) < 0.01);
        }
        role->setMenu(menu);
        auto *remove = new QToolButton(tile);
        remove->setIcon(Icons::get(QStringLiteral("close")));
        remove->setIconSize(QSize(12, 12));
        remove->setToolTip(tr("Remove"));
        connect(remove, &QToolButton::clicked, this, [this, i] {
            if (i < m_refs.size()) {
                m_refs.removeAt(i);
                rebuild();
                emit changed();
            }
        });
        row->addWidget(role, 1);
        row->addWidget(remove);
        if (m_compact)
            role->hide();
        v->addLayout(row);
        m_items->addWidget(tile);
    }
    if (m_refs.size() < m_max) {
        auto *add = new QToolButton(m_container);
        add->setIcon(Icons::get(QStringLiteral("plus")));
        add->setIconSize(QSize(20, 20));
        add->setFixedSize(size + 10, size + (m_compact ? 10 : 34));
        add->setToolTip(tr("Add reference images (or drop/paste them here)"));
        add->setStyleSheet(QStringLiteral("QToolButton { border: 1px dashed %1; border-radius: 10px; }").arg(Theme::palette().borderStrong.name()));
        add->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(add);
        menu->addAction(Icons::get(QStringLiteral("folder")), tr("From files..."), this, &ReferenceStrip::browse);
        menu->addAction(Icons::get(QStringLiteral("copy")), tr("Paste from clipboard"), this, [this] {
            const QImage img = QApplication::clipboard()->image();
            if (img.isNull()) {
                m_studio->notify(tr("The clipboard does not contain an image."));
                return;
            }
            if (doc::Project *project = m_studio->project(); project && project->isOpen()) {
                QPointer<ReferenceStrip> self(this);
                project->addAsset(img, doc::AssetKind::Reference, tr("Pasted reference")).then(this, [self](doc::AssetInfo info) {
                    if (self)
                        self->addAsset(info.id);
                });
            } else {
                addImage(img);
            }
        });
        menu->addAction(Icons::get(QStringLiteral("gallery")), tr("From project images (drag images here)"), this, [this] {
            m_studio->showPage(pages::Library);
        });
        add->setMenu(menu);
        m_items->addWidget(add);
    }
    m_items->addStretch();
    setMinimumHeight(size + (m_compact ? 22 : 46));
}

void ReferenceStrip::dragEnterEvent(QDragEnterEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m->hasFormat(QString::fromLatin1(kAssetMime)) || m->hasUrls() || m->hasImage())
        e->acceptProposedAction();
}

void ReferenceStrip::dropEvent(QDropEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m->hasFormat(QString::fromLatin1(kAssetMime))) {
        for (const QByteArray &id : m->data(QString::fromLatin1(kAssetMime)).split('\n')) {
            if (!id.isEmpty())
                addAsset(QString::fromUtf8(id));
        }
    } else if (m->hasUrls()) {
        QStringList files;
        for (const QUrl &u : m->urls()) {
            if (u.isLocalFile())
                files << u.toLocalFile();
        }
        importFiles(files);
    } else if (m->hasImage()) {
        addImage(qvariant_cast<QImage>(m->imageData()));
    }
    e->acceptProposedAction();
}

} // namespace lumen::ui
