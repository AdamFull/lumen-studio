#include "ui/filters/FiltersPanel.h"

#include "ai/ProviderRegistry.h"
#include "core/Settings.h"
#include "filters/Graph.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/FilterRunner.h"
#include "ui/filters/NodeEditorWindow.h"
#include "ui/filters/ParamWidgets.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/FlowLayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

namespace {

constexpr int kPathRole = Qt::UserRole;
const QString kTargetKey = QStringLiteral("filters/target");
const QString kPreviewKey = QStringLiteral("filters/preview");

// An icon of the list's size: a thumbnail, or a symbol centred on a plain tile.
QIcon tile(const QImage &thumbnail, const QString &symbol = {})
{
    const QSize size = FilterCatalog::thumbnailSize();
    QPixmap pixmap(size * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const Palette &pal = Theme::palette();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 4, 4);
    p.setClipPath(clip);
    if (!thumbnail.isNull()) {
        p.drawImage(QRectF(0, 0, size.width(), size.height()), thumbnail);
    } else {
        p.fillRect(QRectF(0, 0, size.width(), size.height()), pal.surface);
        if (!symbol.isEmpty()) {
            const int s = 22;
            p.drawPixmap(QRect((size.width() - s) / 2, (size.height() - s) / 2, s, s),
                         Icons::pixmap(symbol, s, symbol == QLatin1String("warning") ? pal.warning : pal.icon));
        }
    }
    return QIcon(pixmap);
}

} // namespace

FiltersPanel::FiltersPanel(Studio *studio, CanvasView *canvas, QWidget *parent, FilterCatalog *catalog)
    : QWidget(parent)
    , m_studio(studio)
    , m_canvas(canvas)
    , m_catalog(catalog ? catalog : FilterCatalog::of(studio))
    , m_runner(new FilterRunner(this))
{
    setObjectName(QStringLiteral("FiltersPanel"));
    // AI steps that the filter editor ran on this layer are not run again by Apply.
    m_runner->setSharedResults(m_catalog->sharedResults());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_list = new QListWidget(this);
    m_list->setIconSize(FilterCatalog::thumbnailSize());
    m_list->setSpacing(1);
    m_list->setUniformItemSizes(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setAccessibleName(tr("Filters"));
    m_list->setMinimumHeight(140);

    auto *buttons = new QWidget(this);
    auto *flow = new FlowLayout(buttons, 4, 4);
    flow->setContentsMargins(0, 0, 0, 0);
    m_newButton = new QPushButton(Icons::get(QStringLiteral("plus")), tr("New filter…"), buttons);
    m_newButton->setToolTip(tr("Build a filter of your own in the filter editor"));
    m_editButton = new QPushButton(Icons::get(QStringLiteral("edit")), tr("Edit…"), buttons);
    m_duplicateButton = new QPushButton(Icons::get(QStringLiteral("copy")), tr("Duplicate"), buttons);
    m_duplicateButton->setToolTip(tr("Make a copy in your filters to change"));
    m_deleteButton = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Delete"), buttons);
    m_folderButton = iconButton(QStringLiteral("folder"), tr("Open filters folder"), buttons);
    for (QWidget *w : std::initializer_list<QWidget *>{m_newButton, m_editButton, m_duplicateButton, m_deleteButton, m_folderButton})
        flow->addWidget(w);

    // The chosen filter.
    m_details = new QWidget;
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(0, 4, 0, 0);
    details->setSpacing(8);
    m_title = new QLabel(m_details);
    m_title->setWordWrap(true);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_about = mutedLabel(QString(), m_details);
    m_about->setWordWrap(true);
    details->addWidget(m_title);
    details->addWidget(m_about);
    m_settingsLayout = new QVBoxLayout;
    m_settingsLayout->setSpacing(8);
    details->addLayout(m_settingsLayout);
    m_previewCheck = new QCheckBox(tr("Preview on the canvas"), m_details);
    m_previewCheck->setToolTip(tr("Shows the filter on the picture while you change its settings, at a lower resolution. "
                                  "Apply keeps it."));
    m_previewCheck->setChecked(Settings::instance().value(kPreviewKey, true).toBool());
    details->addWidget(m_previewCheck);
    auto *targetRow = new QHBoxLayout;
    targetRow->setSpacing(8);
    auto *targetLabel = mutedLabel(tr("Result on"), m_details);
    m_target = new QComboBox(m_details);
    m_target->addItem(tr("A new layer"), QStringLiteral("new"));
    m_target->addItem(tr("This layer"), QStringLiteral("layer"));
    m_target->setItemData(0, tr("The result goes on a new layer above, so the original stays underneath."), Qt::ToolTipRole);
    m_target->setItemData(1, tr("The result replaces the pixels of the layer (undo brings them back)."), Qt::ToolTipRole);
    m_target->setAccessibleName(tr("Put the result on"));
    m_target->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_target->setCurrentIndex(Settings::instance().value(kTargetKey).toString() == QLatin1String("layer") ? 1 : 0);
    targetRow->addWidget(targetLabel);
    targetRow->addWidget(m_target, 1);
    details->addLayout(targetRow);
    details->addStretch(1);

    // What the filter will do and the actions: always in view under the details, which scroll.
    m_footer = new QWidget(this);
    m_footer->setObjectName(QStringLiteral("FilterActions"));
    auto *footer = new QVBoxLayout(m_footer);
    footer->setContentsMargins(0, 4, 0, 0);
    footer->setSpacing(6);
    // AI results to choose from (the strip above the prompt bar) show on the canvas instead of the
    // preview; said where it stays in view.
    m_previewNote = mutedLabel(tr("The canvas shows the AI results to choose from. Keep or discard them to see the filter there."),
                               m_footer);
    m_previewNote->setObjectName(QStringLiteral("FilterPreviewNote"));
    m_previewNote->setWordWrap(true);
    m_previewNote->hide();
    footer->addWidget(m_previewNote);
    m_selectionHint = mutedLabel(QString(), m_footer);
    m_selectionHint->setObjectName(QStringLiteral("FilterSelectionHint"));
    m_selectionHint->setWordWrap(true);
    footer->addWidget(m_selectionHint);
    m_availability = new QLabel(m_footer);
    m_availability->setObjectName(QStringLiteral("FilterAvailability"));
    m_availability->setWordWrap(true);
    Theme::setRole(m_availability, QStringLiteral("error"));
    m_availability->hide();
    footer->addWidget(m_availability);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    m_runButton = new QPushButton(Icons::get(QStringLiteral("play")), tr("Run AI steps"), m_footer);
    m_runButton->setToolTip(tr("This filter has AI steps. They take a while, so the preview runs them only when you ask."));
    m_applyButton = new QPushButton(Icons::get(QStringLiteral("check")), tr("Apply"), m_footer);
    m_applyButton->setProperty("role", QStringLiteral("primary"));
    m_applyButton->setToolTip(tr("Apply the filter to the active layer (only the selected area changes)"));
    actions->addWidget(m_runButton);
    actions->addStretch(1);
    actions->addWidget(m_applyButton);
    footer->addLayout(actions);
    m_progressRow = new QWidget(m_footer);
    auto *progress = new QGridLayout(m_progressRow);
    progress->setContentsMargins(0, 0, 0, 0);
    progress->setHorizontalSpacing(6);
    progress->setVerticalSpacing(2);
    m_progress = new QProgressBar(m_progressRow);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(8);
    m_progressText = mutedLabel(QString(), m_progressRow);
    m_progressText->setWordWrap(true);
    m_cancelButton = new QPushButton(tr("Cancel"), m_progressRow);
    progress->addWidget(m_progress, 0, 0);
    progress->addWidget(m_cancelButton, 0, 1, 2, 1, Qt::AlignVCenter);
    progress->addWidget(m_progressText, 1, 0);
    progress->setColumnStretch(0, 1);
    m_progressRow->hide();
    footer->addWidget(m_progressRow);
    m_status = mutedLabel(QString(), m_footer);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer->addWidget(m_status);

    // The list over the details; the line between them can be dragged, and the details scroll.
    // The actions stay under both.
    auto *split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    auto *top = new QWidget(split);
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(6);
    topLayout->addWidget(m_list, 1);
    topLayout->addWidget(buttons);
    split->addWidget(top);
    QScrollArea *detailsScroll = verticalScroll(m_details, split);
    detailsScroll->setMinimumHeight(96); // the title and a setting or two
    split->addWidget(detailsScroll);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    layout->addWidget(separator(Qt::Horizontal, this));
    layout->addWidget(m_footer);

    m_confirm = [this](const QString &question) {
        return QMessageBox::question(this, tr("Delete filter"), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
    };

    // Wiring.
    connect(m_catalog, &FilterCatalog::changed, this, &FiltersPanel::rebuildList);
    connect(m_catalog, &FilterCatalog::thumbnailReady, this, &FiltersPanel::updateThumbnail);
    connect(m_catalog, &FilterCatalog::saved, this, [this](const QString &path) {
        if (path == m_path && !m_runner->isBusy())
            loadSelected();
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        const QString path = item ? item->data(kPathRole).toString() : QString();
        if (path != m_path) {
            m_path = path;
            loadSelected();
        }
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item->data(kPathRole).toString().isEmpty())
            editSelected();
    });
    connect(m_newButton, &QPushButton::clicked, this, [this] { newFilter(); });
    connect(m_editButton, &QPushButton::clicked, this, &FiltersPanel::editSelected);
    connect(m_duplicateButton, &QPushButton::clicked, this, &FiltersPanel::duplicateSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &FiltersPanel::deleteSelected);
    connect(m_folderButton, &QToolButton::clicked, this, &FiltersPanel::openFolder);
    connect(m_applyButton, &QPushButton::clicked, this, &FiltersPanel::applySelected);
    connect(m_runButton, &QPushButton::clicked, this, [this] {
        QString why;
        if (!m_runner->run(&why))
            setStatus(why, true);
        else
            setStatus({});
    });
    connect(m_cancelButton, &QPushButton::clicked, this, &FiltersPanel::cancel);
    connect(m_previewCheck, &QCheckBox::toggled, this, [this](bool on) {
        Settings::instance().setValue(kPreviewKey, on);
        updatePreviewState();
        if (on)
            m_runner->resumePreview(); // asked for: also right after Apply
    });
    connect(m_target, &QComboBox::currentIndexChanged, this, [this] {
        Settings::instance().setValue(kTargetKey, m_target->currentData());
        m_runner->setTarget(m_target->currentIndex() == 1 ? FilterRunner::Target::Layer : FilterRunner::Target::NewLayer);
    });
    m_runner->setTarget(m_target->currentIndex() == 1 ? FilterRunner::Target::Layer : FilterRunner::Target::NewLayer);
    connect(m_runner, &FilterRunner::previewChanged, this, [this](const QImage &image) {
        if (m_canvas)
            m_canvas->setPreview(CanvasView::Preview::Filter, image);
    });
    connect(m_runner, &FilterRunner::previewFailed, this, [this](const QString &message) { setStatus(message, true); });
    connect(m_runner, &FilterRunner::needsRunChanged, this, &FiltersPanel::updateButtons);
    connect(m_runner, &FilterRunner::busyChanged, this, [this](bool busy) {
        m_progressRow->setVisible(busy);
        if (busy) {
            m_progress->setRange(0, 0);
            m_progressText->setText(m_runner->isApplying() ? tr("Applying…") : tr("Running the AI steps…"));
        }
        updateButtons();
    });
    // The bar counts steps in hundredths, so an AI step's job moves it while it runs.
    connect(m_runner, &FilterRunner::progress, this, [this](int done, int total, const QString &step) {
        m_stepsDone = done;
        m_stepsTotal = std::max(1, total);
        m_stepName = step;
        m_progress->setRange(0, m_stepsTotal * 100);
        m_progress->setValue(done * 100);
        if (!step.isEmpty())
            m_progressText->setText(tr("%1 (step %2 of %3)").arg(step).arg(done + 1).arg(total));
    });
    connect(m_runner, &FilterRunner::stepProgress, this, [this](double fraction, const QString &status) {
        if (m_stepName.isEmpty())
            return;
        if (fraction >= 0.0) {
            m_progress->setRange(0, m_stepsTotal * 100);
            m_progress->setValue(m_stepsDone * 100 + int(std::lround(std::clamp(fraction, 0.0, 1.0) * 100)));
        }
        const QString step = tr("%1 (step %2 of %3)").arg(m_stepName).arg(m_stepsDone + 1).arg(m_stepsTotal);
        m_progressText->setText(status.isEmpty() ? step : tr("%1: %2").arg(step, status));
    });
    // A successful apply has said where the result went (applied); failures say what happened.
    connect(m_runner, &FilterRunner::finished, this, [this](bool ok, const QString &message) {
        if (!ok || !message.isEmpty())
            setStatus(message, !ok);
    });
    connect(m_runner, &FilterRunner::resultSizeChanged, this, &FiltersPanel::updateSelectionHint);
    // The filter that was applied: another may have been chosen while it ran.
    connect(m_runner, &FilterRunner::applied, this, [this] {
        const QString where = m_runner->target() == FilterRunner::Target::NewLayer ? tr("on a new layer") : tr("on the layer");
        QString text = tr("Applied “%1” %2.").arg(m_runner->appliedName(), where);
        if (!m_runner->appliedNote().isEmpty())
            text += QLatin1Char(' ') + m_runner->appliedNote();
        setStatus(text + QLatin1Char(' ') + tr("Undo (Ctrl+Z) takes it back."));
    });
    connect(studio, &Studio::activeDocumentChanged, this, &FiltersPanel::setDocument);
    if (m_canvas) {
        connect(m_canvas, &CanvasView::previewChanged, this, [this](CanvasView::Preview kind) {
            if (kind == CanvasView::Preview::Result)
                updatePreviewState();
        });
    }
    // Whether the AI steps can run depends on the image engines that are set up.
    connect(studio->providers(), &ai::ProviderRegistry::providersChanged, this, &FiltersPanel::updateAvailability);
    connect(studio->providers(), &ai::ProviderRegistry::rolesChanged, this, &FiltersPanel::updateAvailability);

    rebuildList();
    setDocument(studio->activeDocument());
    updatePreviewState();
    loadSelected();
}

FiltersPanel::~FiltersPanel()
{
    if (m_canvas)
        m_canvas->setPreview(CanvasView::Preview::Filter, {});
}

void FiltersPanel::setDocument(doc::Document *doc)
{
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    if (m_canvas)
        m_canvas->setPreview(CanvasView::Preview::Filter, {}); // the old document's preview must not show over the new one
    m_runner->setDocument(doc);
    if (doc) {
        connect(doc, &doc::Document::selectionChanged, this, &FiltersPanel::updateSelectionHint);
        connect(doc, &doc::Document::activeLayerChanged, this, &FiltersPanel::updateButtons);
        connect(doc, &doc::Document::layerPropsChanged, this, &FiltersPanel::updateButtons);
    }
    updateSelectionHint();
    updateButtons();
}

QListWidgetItem *FiltersPanel::itemFor(const QString &path) const
{
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(kPathRole).toString() == path)
            return m_list->item(i);
    }
    return nullptr;
}

void FiltersPanel::rebuildList()
{
    const QString keep = m_path;
    QSignalBlocker block(m_list);
    m_list->clear();
    auto header = [this](const QString &text) {
        auto *item = new QListWidgetItem(text, m_list);
        item->setFlags(Qt::NoItemFlags);
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        item->setForeground(Theme::palette().textMuted);
    };
    const QList<FilterCatalog::Entry> entries = m_catalog->entries();
    bool savedHeader = false;
    bool aiHeader = false;
    header(tr("Built in"));
    for (const FilterCatalog::Entry &e : entries) {
        // The built-in filters with AI steps follow the others under a heading of their own.
        if (e.builtIn && e.usesAi && !aiHeader) {
            header(tr("Built in, with AI"));
            aiHeader = true;
        }
        if (!e.builtIn && !savedHeader) {
            header(tr("Your filters"));
            savedHeader = true;
        }
        auto *item = new QListWidgetItem(e.name, m_list);
        item->setData(kPathRole, e.path);
        QString tip = e.description;
        if (e.usesAi)
            tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Uses AI: the preview runs when you ask.");
        if (!e.error.isEmpty())
            tip = e.error;
        item->setToolTip(tip);
        const QImage thumb = m_catalog->thumbnail(e.path);
        item->setIcon(tile(thumb, !e.error.isEmpty() ? QStringLiteral("warning") : e.usesAi ? QStringLiteral("sparkles") : QString()));
        if (!e.error.isEmpty())
            item->setForeground(Theme::palette().textMuted);
    }
    if (!savedHeader) {
        header(tr("Your filters"));
        auto *none = new QListWidgetItem(tr("None yet: “New filter…” starts one."), m_list);
        none->setFlags(Qt::NoItemFlags);
        none->setForeground(Theme::palette().textMuted);
    }
    QListWidgetItem *current = itemFor(keep);
    if (!current) {
        for (int i = 0; i < m_list->count() && !current; ++i) {
            if (!m_list->item(i)->data(kPathRole).toString().isEmpty())
                current = m_list->item(i);
        }
    }
    m_list->setCurrentItem(current);
    const QString path = current ? current->data(kPathRole).toString() : QString();
    const std::optional<FilterCatalog::Entry> entry = m_catalog->entry(path);
    // The chosen filter was saved again (the node editor): its settings may have changed.
    if (path != m_path || (entry && entry->modified != m_loadedStamp)) {
        m_path = path;
        loadSelected();
    } else {
        updateButtons();
    }
}

void FiltersPanel::updateThumbnail(const QString &path)
{
    if (QListWidgetItem *item = itemFor(path))
        item->setIcon(tile(m_catalog->thumbnail(path)));
}

bool FiltersPanel::selectFilter(const QString &path)
{
    QListWidgetItem *item = itemFor(path);
    if (!item)
        return false;
    m_list->setCurrentItem(item);
    return true;
}

ParamField *FiltersPanel::field(const QString &label) const
{
    for (ParamField *f : m_fields) {
        if (f->property("label").toString() == label)
            return f;
    }
    return nullptr;
}

void FiltersPanel::loadSelected()
{
    const std::optional<FilterCatalog::Entry> entry = m_catalog->entry(m_path);
    m_loadedStamp = entry ? entry->modified : QDateTime();
    std::optional<filters::Graph> graph;
    QString error;
    if (entry && entry->error.isEmpty())
        graph = m_catalog->load(m_path, &error);
    m_runner->setGraph(graph, entry ? entry->name : QString());
    m_title->setText(entry ? entry->name : tr("No filter chosen"));
    m_about->setText(entry ? (entry->error.isEmpty() ? entry->description : entry->error) : tr("Choose a filter in the list."));
    m_about->setVisible(!m_about->text().isEmpty());
    setStatus(entry && !entry->error.isEmpty() ? tr("This filter cannot be used.") : error, !error.isEmpty() || (entry && !entry->error.isEmpty()));
    rebuildSettings();
    updateAvailability();
    updateButtons();
}

void FiltersPanel::updateAvailability()
{
    QStringList reasons;
    if (const std::optional<filters::Graph> &graph = m_runner->graph()) {
        for (const QString &id : graph->evaluationOrder(graph->outputNode())) {
            const filters::Node *node = graph->node(id);
            const QString reason = node && node->type ? node->type->unavailableReason(node->params) : QString();
            if (!reason.isEmpty() && !reasons.contains(reason))
                reasons << reason;
        }
    }
    m_availability->setText(reasons.join(QLatin1Char('\n')));
    m_availability->setVisible(!reasons.isEmpty());
}

void FiltersPanel::rebuildSettings()
{
    while (QLayoutItem *item = m_settingsLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->setParent(nullptr); // gone from the panel now; deleted once its signals are done
            w->deleteLater();
        }
        delete item;
    }
    m_fields.clear();
    const std::optional<filters::Graph> &graph = m_runner->graph();
    if (!graph)
        return;
    for (const filters::PublishedParam &p : graph->published()) {
        const filters::Node *node = graph->node(p.node);
        const filters::ParamSpec *spec = node && node->type ? node->type->info().param(p.param) : nullptr;
        if (!spec)
            continue;
        auto *field = new ParamField(*spec, node->param(p.param), p.label, p.minimum, p.maximum, m_details);
        field->setProperty("label", p.label);
        field->setObjectName(QStringLiteral("setting:%1/%2").arg(p.node, p.param));
        m_settingsLayout->addWidget(field);
        m_fields << field;
        const QString nodeId = p.node, paramId = p.param;
        connect(field, &ParamField::valueChanged, this, [this, nodeId, paramId](const QVariant &v) {
            m_runner->setParam(nodeId, paramId, v);
            setStatus({});
        });
    }
    if (m_fields.isEmpty()) {
        auto *none = mutedLabel(tr("This filter has no settings to change here."), m_details);
        none->setWordWrap(true);
        m_settingsLayout->addWidget(none);
    }
}

void FiltersPanel::updateButtons()
{
    const std::optional<FilterCatalog::Entry> entry = m_catalog->entry(m_path);
    const bool busy = m_runner->isBusy();
    const bool usable = entry && entry->error.isEmpty() && m_runner->graph();
    const doc::LayerPtr layer = m_doc ? m_doc->activeLayer() : nullptr;
    m_editButton->setEnabled(entry.has_value() && !busy);
    m_editButton->setText(entry && entry->builtIn ? tr("Edit a copy…") : tr("Edit…"));
    m_editButton->setToolTip(entry && entry->builtIn ? tr("Built-in filters stay as they are: this edits a copy in your filters")
                                                     : tr("Open the filter in the filter editor"));
    m_duplicateButton->setEnabled(entry && entry->error.isEmpty());
    m_deleteButton->setEnabled(entry && !entry->builtIn && !busy);
    m_deleteButton->setToolTip(entry && entry->builtIn ? tr("Built-in filters cannot be deleted") : tr("Delete the filter"));
    m_applyButton->setEnabled(usable && layer && !busy);
    m_runButton->setVisible(usable && m_runner->needsRun());
    m_runButton->setEnabled(!busy && layer);
    m_target->setEnabled(!busy);
    for (ParamField *f : std::as_const(m_fields))
        f->setEnabled(!busy || !m_runner->isApplying());
}

void FiltersPanel::updateSelectionHint()
{
    const QSize size = m_runner->resultSize();
    if (!m_doc) {
        m_selectionHint->setText(tr("Open an image to use a filter."));
    } else if (size.isValid()) {
        // The filter makes the picture larger or smaller (an upscale): the whole picture changes size.
        const QString becomes = m_runner->isResultSizeExact()
                                    ? tr("The picture becomes %1 × %2 pixels.").arg(size.width()).arg(size.height())
                                    : tr("The picture becomes about %1 × %2 pixels.").arg(size.width()).arg(size.height());
        m_selectionHint->setText(m_doc->hasSelection() ? tr("%1 Only the selected area gets the filter; the rest is only resized.").arg(becomes)
                                                       : becomes);
    } else if (m_doc->hasSelection()) {
        m_selectionHint->setText(tr("Only the selected area changes."));
    } else {
        m_selectionHint->setText(tr("Nothing is selected, so the whole layer changes."));
    }
}

void FiltersPanel::setStatus(const QString &text, bool problem)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
    Theme::setRole(m_status, problem ? QStringLiteral("error") : QStringLiteral("muted"));
}

QString FiltersPanel::statusText() const
{
    return m_status->text();
}

void FiltersPanel::updatePreviewState()
{
    // AI results to choose from win over the filter's preview: it waits until they are kept (it
    // then shows the filter on the new picture) or discarded.
    const bool results = m_canvas && !m_canvas->preview(CanvasView::Preview::Result).isNull();
    m_previewNote->setVisible(results && m_previewCheck->isChecked());
    const bool on = m_shown && m_previewCheck->isChecked() && !results;
    if (m_canvas && on) {
        // About the canvas's size on the screen: sharp enough, still quick.
        const double dpr = m_canvas->devicePixelRatioF();
        const int side = int(std::max(m_canvas->width(), m_canvas->height()) * dpr);
        m_runner->setPreviewSize(std::clamp(side, 512, 2048));
    }
    m_runner->setPreviewEnabled(on);
    if (!on && m_canvas)
        m_canvas->setPreview(CanvasView::Preview::Filter, {});
}

void FiltersPanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    m_shown = true;
    updatePreviewState();
}

void FiltersPanel::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_shown = false;
    updatePreviewState();
}

// ---- Buttons ------------------------------------------------------------------------------------

void FiltersPanel::sendSourceTo(NodeEditorWindow *window) const
{
    const doc::LayerPtr layer = m_doc ? m_doc->activeLayer() : nullptr;
    window->setSource(layer ? layer->image : QImage(), m_doc ? m_doc->selection() : QImage());
}

QString FiltersPanel::newFilter(const QString &name)
{
    QString chosen = name;
    if (chosen.isEmpty()) {
        bool ok = false;
        chosen = QInputDialog::getText(this, tr("New filter"), tr("Name:"), QLineEdit::Normal, tr("My filter"), &ok);
        if (!ok || chosen.trimmed().isEmpty())
            return {};
    }
    QString error;
    const QString path = m_catalog->create(chosen, &error);
    if (path.isEmpty()) {
        setStatus(error, true);
        return {};
    }
    selectFilter(path);
    if (NodeEditorWindow *w = NodeEditorWindow::openFilter(m_catalog, path, window(), &error))
        sendSourceTo(w);
    else
        setStatus(error, true);
    return path;
}

NodeEditorWindow *FiltersPanel::editSelected()
{
    const std::optional<FilterCatalog::Entry> entry = m_catalog->entry(m_path);
    if (!entry)
        return nullptr;
    QString path = entry->path;
    QString error;
    if (entry->builtIn) {
        // The copy made last time while it is unchanged, so looking at a built-in filter in the
        // editor does not add another copy each time.
        const QString copy = m_catalog->unchangedCopyOf(path);
        path = copy.isEmpty() ? m_catalog->duplicate(path, &error) : copy;
        if (path.isEmpty()) {
            setStatus(error, true);
            return nullptr;
        }
        selectFilter(path);
    }
    NodeEditorWindow *w = NodeEditorWindow::openFilter(m_catalog, path, window(), &error);
    if (!w) {
        setStatus(error, true);
        return nullptr;
    }
    sendSourceTo(w);
    return w;
}

QString FiltersPanel::duplicateSelected()
{
    QString error;
    const QString path = m_catalog->duplicate(m_path, &error);
    if (path.isEmpty()) {
        setStatus(error, true);
        return {};
    }
    selectFilter(path);
    return path;
}

bool FiltersPanel::deleteSelected()
{
    const std::optional<FilterCatalog::Entry> entry = m_catalog->entry(m_path);
    if (!entry || entry->builtIn)
        return false;
    if (m_confirm && !m_confirm(tr("Delete the filter “%1”? Its file is removed from the filters folder.").arg(entry->name)))
        return false;
    QString error;
    if (!m_catalog->remove(entry->path, &error)) {
        setStatus(error, true);
        return false;
    }
    return true;
}

void FiltersPanel::openFolder()
{
    QString error;
    if (!m_catalog->ensureFolder(&error)) {
        setStatus(error, true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_catalog->folder()));
}

bool FiltersPanel::applySelected()
{
    QString why;
    if (!m_runner->apply(&why)) {
        setStatus(why, true);
        return false;
    }
    setStatus({});
    return true;
}

void FiltersPanel::cancel()
{
    m_runner->cancel();
}

} // namespace lumen::ui
