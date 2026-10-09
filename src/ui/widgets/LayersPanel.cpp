#include "ui/widgets/LayersPanel.h"

#include "core/Async.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {
constexpr int kThumb = 38;
}

LayersPanel::LayersPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto *props = new QHBoxLayout;
    m_blend = new QComboBox(this);
    m_blend->addItems(doc::blendModeNames());
    m_blend->setToolTip(tr("Blend mode"));
    props->addWidget(m_blend, 1);
    layout->addLayout(props);
    m_opacity = new SliderField(tr("Opacity"), 0, 100, 100, 0, QStringLiteral(" %"), this);
    layout->addWidget(m_opacity);

    m_list = new QListWidget(this);
    m_list->setSpacing(1);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list, 1);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(2);
    auto *add = iconButton(QStringLiteral("plus"), tr("New layer"), this);
    auto *dup = iconButton(QStringLiteral("copy"), tr("Duplicate layer"), this);
    auto *merge = iconButton(QStringLiteral("merge"), tr("Merge down"), this);
    auto *up = iconButton(QStringLiteral("chevron-up"), tr("Move up"), this);
    auto *down = iconButton(QStringLiteral("chevron-down"), tr("Move down"), this);
    auto *del = iconButton(QStringLiteral("trash"), tr("Delete layer"), this);
    for (QToolButton *b : {add, dup, merge, up, down})
        bar->addWidget(b);
    bar->addStretch();
    bar->addWidget(del);
    layout->addLayout(bar);

    connect(add, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->addLayer(tr("Layer %1").arg(m_doc->layerCount() + 1));
    });
    connect(dup, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->duplicateLayer(m_doc->activeLayerId());
    });
    connect(merge, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->mergeDown(m_doc->activeLayerId());
    });
    connect(up, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->moveLayer(m_doc->activeLayerId(), m_doc->indexOf(m_doc->activeLayerId()) + 1);
    });
    connect(down, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->moveLayer(m_doc->activeLayerId(), m_doc->indexOf(m_doc->activeLayerId()) - 1);
    });
    connect(del, &QToolButton::clicked, this, [this] {
        if (m_doc && m_doc->layerCount() > 1)
            m_doc->removeLayer(m_doc->activeLayerId());
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_doc || m_rebuilding || row < 0)
            return;
        const int index = m_doc->layerCount() - 1 - row;
        if (const doc::LayerPtr l = m_doc->layerAt(index))
            m_doc->setActiveLayer(l->id);
    });
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, [this](const QModelIndex &, int start, int, const QModelIndex &, int dest) {
        if (!m_doc || m_rebuilding)
            return;
        const int n = m_doc->layerCount();
        const int to = dest > start ? dest - 1 : dest;
        const doc::LayerPtr layer = m_doc->layerAt(n - 1 - start);
        if (layer)
            QTimer::singleShot(0, this, [this, id = layer->id, target = n - 1 - to] {
                if (m_doc)
                    m_doc->moveLayer(id, target);
            });
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!m_doc)
            return;
        const doc::LayerPtr l = m_doc->findLayer(item->data(Qt::UserRole).toUuid());
        if (!l)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename layer"), tr("Name:"), QLineEdit::Normal, l->props.name, &ok);
        if (ok && !name.trimmed().isEmpty()) {
            doc::LayerProps p = l->props;
            p.name = name.trimmed();
            m_doc->setLayerProps(l->id, p, tr("Rename layer"));
        }
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_doc)
            return;
        QMenu menu(this);
        const QUuid id = m_doc->activeLayerId();
        menu.addAction(Icons::get(QStringLiteral("copy")), tr("Duplicate"), this, [this, id] { m_doc->duplicateLayer(id); });
        menu.addAction(Icons::get(QStringLiteral("merge")), tr("Merge down"), this, [this, id] { m_doc->mergeDown(id); });
        menu.addAction(tr("Flatten image"), this, [this] { m_doc->flattenImage(); });
        menu.addSeparator();
        if (const doc::LayerPtr l = m_doc->findLayer(id)) {
            QAction *alpha = menu.addAction(tr("Lock transparency"), this, [this, id](bool on) {
                if (const doc::LayerPtr ll = m_doc->findLayer(id)) {
                    doc::LayerProps p = ll->props;
                    p.alphaLocked = on;
                    m_doc->setLayerProps(id, p, tr("Lock transparency"));
                }
            });
            alpha->setCheckable(true);
            alpha->setChecked(l->props.alphaLocked);
        }
        menu.addSeparator();
        QAction *del = menu.addAction(Icons::get(QStringLiteral("trash")), tr("Delete"), this, [this, id] { m_doc->removeLayer(id); });
        del->setEnabled(m_doc->layerCount() > 1);
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_blend, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_doc || m_rebuilding)
            return;
        if (const doc::LayerPtr l = m_doc->activeLayer()) {
            doc::LayerProps p = l->props;
            p.blend = static_cast<doc::BlendMode>(i);
            m_doc->setLayerProps(l->id, p, tr("Blend mode"));
        }
    });
    connect(m_opacity, &SliderField::valueChanged, this, [this](double v) {
        if (!m_doc || m_rebuilding)
            return;
        if (const doc::LayerPtr l = m_doc->activeLayer()) {
            doc::LayerProps p = l->props;
            p.opacity = v / 100.0;
            m_doc->setLayerProps(l->id, p, tr("Layer opacity"));
        }
    });
    m_thumbTimer.setSingleShot(true);
    m_thumbTimer.setInterval(350);
    connect(&m_thumbTimer, &QTimer::timeout, this, &LayersPanel::updateThumbnails);
    setEnabled(false);
}

void LayersPanel::setDocument(doc::Document *doc)
{
    if (m_doc == doc)
        return;
    if (m_doc)
        m_doc->disconnect(this);
    m_doc = doc;
    m_thumbs.clear();
    m_thumbRevision.clear();
    setEnabled(doc != nullptr);
    if (doc) {
        connect(doc, &doc::Document::layersChanged, this, &LayersPanel::rebuild);
        connect(doc, &doc::Document::layerPropsChanged, this, &LayersPanel::rebuild);
        connect(doc, &doc::Document::activeLayerChanged, this, &LayersPanel::syncActive);
        connect(doc, &doc::Document::pixelsChanged, this, &LayersPanel::scheduleThumbnails);
    }
    rebuild();
    updateThumbnails();
}

QWidget *LayersPanel::rowWidget(const doc::LayerPtr &layer)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(4, 3, 4, 3);
    h->setSpacing(8);
    auto *vis = new QToolButton(w);
    vis->setIcon(Icons::get(layer->props.visible ? QStringLiteral("eye") : QStringLiteral("eye-off")));
    vis->setIconSize(QSize(16, 16));
    vis->setToolTip(tr("Toggle visibility"));
    auto *thumb = new QLabel(w);
    thumb->setFixedSize(kThumb, kThumb);
    thumb->setObjectName(QStringLiteral("thumb"));
    thumb->setPixmap(m_thumbs.value(layer->id));
    thumb->setStyleSheet(QStringLiteral("border: 1px solid %1; border-radius: 4px;").arg(Theme::palette().border.name()));
    auto *name = new QLabel(layer->props.name, w);
    name->setTextInteractionFlags(Qt::NoTextInteraction);
    if (!layer->props.visible)
        name->setProperty("role", QStringLiteral("muted"));
    auto *info = new QLabel(layer->props.blend != doc::BlendMode::Normal || layer->props.opacity < 1.0
                                ? QStringLiteral("%1 · %2%").arg(doc::blendModeName(layer->props.blend)).arg(int(layer->props.opacity * 100))
                                : QString(),
                            w);
    info->setProperty("role", QStringLiteral("muted"));
    auto *texts = new QVBoxLayout;
    texts->setSpacing(0);
    texts->addWidget(name);
    if (!info->text().isEmpty())
        texts->addWidget(info);
    else
        info->hide();
    auto *lock = new QToolButton(w);
    lock->setIcon(Icons::get(layer->props.locked ? QStringLiteral("lock") : QStringLiteral("unlock")));
    lock->setIconSize(QSize(14, 14));
    lock->setToolTip(tr("Lock layer"));
    if (!layer->props.locked)
        lock->setStyleSheet(QStringLiteral("QToolButton { opacity: 0.4; }"));
    h->addWidget(vis);
    h->addWidget(thumb);
    h->addLayout(texts, 1);
    h->addWidget(lock);
    const QUuid id = layer->id;
    connect(vis, &QToolButton::clicked, this, [this, id] {
        if (const doc::LayerPtr l = m_doc ? m_doc->findLayer(id) : nullptr) {
            doc::LayerProps p = l->props;
            p.visible = !p.visible;
            m_doc->setLayerProps(id, p, p.visible ? tr("Show layer") : tr("Hide layer"));
        }
    });
    connect(lock, &QToolButton::clicked, this, [this, id] {
        if (const doc::LayerPtr l = m_doc ? m_doc->findLayer(id) : nullptr) {
            doc::LayerProps p = l->props;
            p.locked = !p.locked;
            m_doc->setLayerProps(id, p, p.locked ? tr("Lock layer") : tr("Unlock layer"));
        }
    });
    return w;
}

void LayersPanel::rebuild()
{
    m_rebuilding = true;
    m_list->clear();
    if (m_doc) {
        for (int i = m_doc->layerCount() - 1; i >= 0; --i) {
            const doc::LayerPtr l = m_doc->layerAt(i);
            auto *item = new QListWidgetItem(m_list);
            item->setData(Qt::UserRole, l->id);
            item->setSizeHint(QSize(0, kThumb + 8));
            m_list->setItemWidget(item, rowWidget(l));
        }
    }
    m_rebuilding = false;
    syncActive();
    scheduleThumbnails();
}

void LayersPanel::syncActive()
{
    if (!m_doc)
        return;
    m_rebuilding = true;
    const int index = m_doc->indexOf(m_doc->activeLayerId());
    m_list->setCurrentRow(index < 0 ? -1 : m_doc->layerCount() - 1 - index);
    if (const doc::LayerPtr l = m_doc->activeLayer()) {
        m_blend->setCurrentIndex(static_cast<int>(l->props.blend));
        m_opacity->setValue(l->props.opacity * 100.0);
    }
    m_rebuilding = false;
}

void LayersPanel::scheduleThumbnails()
{
    if (!m_thumbTimer.isActive())
        m_thumbTimer.start();
}

void LayersPanel::updateThumbnails()
{
    if (!m_doc)
        return;
    QList<QPair<QUuid, QImage>> work;
    for (const doc::LayerPtr &l : m_doc->layers()) {
        if (m_thumbRevision.value(l->id, ~quint64(0)) != l->revision) {
            work.append({l->id, l->image});
            m_thumbRevision.insert(l->id, l->revision);
        }
    }
    if (work.isEmpty())
        return;
    QPointer<LayersPanel> self(this);
    async::run([work] {
        QList<QPair<QUuid, QImage>> out;
        for (const auto &w : work) {
            const QImage scaled = w.second.scaled(kThumb * 2, kThumb * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QImage thumb(kThumb * 2, kThumb * 2, QImage::Format_ARGB32_Premultiplied);
            thumb.fill(Qt::transparent);
            QPainter p(&thumb);
            for (int y = 0; y < thumb.height(); y += 8)
                for (int x = 0; x < thumb.width(); x += 8)
                    p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(70, 72, 78) : QColor(52, 54, 60));
            p.drawImage((thumb.width() - scaled.width()) / 2, (thumb.height() - scaled.height()) / 2, scaled);
            p.end();
            out.append({w.first, thumb});
        }
        return out;
    }).then(this, [self](QList<QPair<QUuid, QImage>> thumbs) {
        if (!self)
            return;
        for (const auto &t : thumbs) {
            QPixmap pm = QPixmap::fromImage(t.second);
            pm.setDevicePixelRatio(2.0);
            self->m_thumbs.insert(t.first, pm);
        }
        for (int row = 0; row < self->m_list->count(); ++row) {
            QListWidgetItem *item = self->m_list->item(row);
            if (QWidget *w = self->m_list->itemWidget(item)) {
                if (auto *label = w->findChild<QLabel *>(QStringLiteral("thumb")))
                    label->setPixmap(self->m_thumbs.value(item->data(Qt::UserRole).toUuid()));
            }
        }
    });
}

} // namespace lumen::ui
