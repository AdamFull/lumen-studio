#include "plugins/parts/PartsPage.h"

#include "ai/Director.h"
#include "ai/PartService.h"
#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/MaskOps.h"
#include "core/Psd.h"
#include "core/Settings.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "ui/Icons.h"
#include "ui/Shortcuts.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/Tools.h"
#include "live2d/CubismCore.h"
#include "plugins/parts/CubismPanel.h"
#include "plugins/parts/CubismView.h"
#include "plugins/parts/PartTools.h"
#include "plugins/parts/PuppetPreview.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "plugins/parts/ExpressionCheck.h"
#include "ui/widgets/CharacterChip.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ProviderCombo.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

const QString kSkeletonSegmenter = QStringLiteral("__skeleton__");

QPixmap checkerThumb(const QImage &image, const QSize &box)
{
    QPixmap pm(box);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const int cell = 6;
    for (int y = 0; y < box.height(); y += cell)
        for (int x = 0; x < box.width(); x += cell)
            p.fillRect(x, y, cell, cell, ((x / cell + y / cell) % 2) ? QColor(70, 70, 76) : QColor(56, 56, 62));
    if (!image.isNull()) {
        const QImage scaled = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p.drawImage((box.width() - scaled.width()) / 2, (box.height() - scaled.height()) / 2, scaled);
    }
    return pm;
}

QPushButton *primaryButton(const QString &text, const QString &icon, QWidget *parent)
{
    auto *b = new QPushButton(QIcon(Icons::pixmap(icon, 18, Theme::palette().accentText)), text, parent);
    b->setProperty("role", QStringLiteral("primary"));
    return b;
}

QString stepTitle(int step)
{
    switch (step) {
    case 0: return QObject::tr("1  Source");
    case 1: return QObject::tr("2  Masks");
    case 2: return QObject::tr("3  Generate");
    default: return QObject::tr("4  Preview && export");
    }
}

QString stepHint(int step)
{
    switch (step) {
    case 0: return QObject::tr("Pick a front-facing character image with a clean or transparent background.");
    case 1: return QObject::tr("Auto-segment, then fix the masks: paint, lasso, wand, split and merge until every part is right.");
    case 2: return QObject::tr("Generate the hidden areas of every part, the selected ones, or one at a time, then review.");
    default: return QObject::tr("Check the parts on a moving puppet and export a layered PSD for Live2D Cubism.");
    }
}

} // namespace

// ---- Review card -------------------------------------------------------------------------------------

// One part in the review list: result thumbnail, state, variants and per-part actions.
class PartCard : public QFrame
{
public:
    PartCard(QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("Card"));
        setFrameShape(QFrame::StyledPanel);
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(8, 8, 8, 8);
        h->setSpacing(10);
        m_thumb = new QLabel(this);
        m_thumb->setFixedSize(76, 76);
        m_thumb->setCursor(Qt::PointingHandCursor);
        h->addWidget(m_thumb);
        auto *v = new QVBoxLayout;
        v->setSpacing(4);
        m_name = new QLabel(this);
        m_name->setTextFormat(Qt::RichText);
        m_state = mutedLabel(QString(), this);
        m_state->setWordWrap(true);
        v->addWidget(m_name);
        v->addWidget(m_state);
        auto *row = new QHBoxLayout;
        row->setSpacing(2);
        m_prev = iconButton(QStringLiteral("chevron-left"), QObject::tr("Previous variant"), this);
        m_variant = mutedLabel(QString(), this);
        m_next = iconButton(QStringLiteral("chevron-right"), QObject::tr("Next variant"), this);
        m_generate = iconButton(QStringLiteral("sparkles"), QObject::tr("Generate this part again (adds a variant)"), this);
        m_approve = iconButton(QStringLiteral("check"), QObject::tr("Approve this result"), this, true);
        m_edit = iconButton(QStringLiteral("edit"), QObject::tr("Edit the mask of this part"), this);
        m_remove = iconButton(QStringLiteral("trash"), QObject::tr("Delete this variant"), this);
        row->addWidget(m_prev);
        row->addWidget(m_variant);
        row->addWidget(m_next);
        row->addStretch();
        row->addWidget(m_generate);
        row->addWidget(m_approve);
        row->addWidget(m_edit);
        row->addWidget(m_remove);
        v->addLayout(row);
        h->addLayout(v, 1);
        QObject::connect(m_prev, &QToolButton::clicked, this, [this] { if (onVariant) onVariant(-1); });
        QObject::connect(m_next, &QToolButton::clicked, this, [this] { if (onVariant) onVariant(+1); });
        QObject::connect(m_generate, &QToolButton::clicked, this, [this] { if (onGenerate) onGenerate(); });
        QObject::connect(m_approve, &QToolButton::clicked, this, [this](bool on) { if (onApprove) onApprove(on); });
        QObject::connect(m_edit, &QToolButton::clicked, this, [this] { if (onEdit) onEdit(); });
        QObject::connect(m_remove, &QToolButton::clicked, this, [this] { if (onRemove) onRemove(); });
    }

    void refresh(const doc::Part &p, const QImage &result, bool selected)
    {
        m_name->setText(QStringLiteral("<span style='color:%1'>●</span>&nbsp;<b>%2</b>").arg(p.color.name(), p.displayName().toHtmlEscaped()));
        QString state = doc::partStateLabel(p.state);
        if (p.state == doc::PartState::Failed && !p.error.isEmpty())
            state = QObject::tr("Failed: %1").arg(p.error);
        else if (p.state == doc::PartState::Masked)
            state = QObject::tr("Not generated yet");
        m_state->setText(state);
        m_state->setToolTip(p.error);
        const bool hasVariants = p.variants.size() > 1;
        m_prev->setVisible(hasVariants);
        m_next->setVisible(hasVariants);
        m_variant->setVisible(hasVariants);
        m_variant->setText(QStringLiteral("%1/%2").arg(p.chosen + 1).arg(p.variants.size()));
        m_prev->setEnabled(p.chosen > 0);
        m_next->setEnabled(p.chosen + 1 < p.variants.size());
        m_approve->setEnabled(p.hasResult());
        m_approve->setChecked(p.state == doc::PartState::Approved);
        m_remove->setVisible(p.hasResult());
        const bool busy = p.state == doc::PartState::Queued || p.state == doc::PartState::Generating;
        m_generate->setEnabled(!busy && (p.pixels > 0 || p.provenance.startsWith(QLatin1String("variant:"))));
        if (m_resultKey != p.chosenAssetId() + QString::number(result.cacheKey())) {
            m_resultKey = p.chosenAssetId() + QString::number(result.cacheKey());
            m_thumb->setPixmap(checkerThumb(result, m_thumb->size()));
        }
        setProperty("selected", selected);
        setStyleSheet(selected ? QStringLiteral("#Card { border: 1px solid %1; }").arg(Theme::palette().accent.name()) : QString());
    }

    std::function<void(int)> onVariant;
    std::function<void()> onGenerate;
    std::function<void(bool)> onApprove;
    std::function<void()> onEdit;
    std::function<void()> onRemove;
    std::function<void()> onSelect;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (onSelect)
            onSelect();
        QFrame::mousePressEvent(e);
    }

private:
    QLabel *m_thumb;
    QLabel *m_name;
    QLabel *m_state;
    QLabel *m_variant;
    QToolButton *m_prev, *m_next, *m_generate, *m_approve, *m_edit, *m_remove;
    QString m_resultKey;
};

// ---- Page -----------------------------------------------------------------------------------------------

PartsPage::PartsPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_state(new EditorState(this))
    , m_context(new PartEditContext(this))
{
    setAcceptDrops(true);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- Header: session + steps -------------------------------------------------------------------------
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("Panel"));
    auto *hh = new QHBoxLayout(header);
    hh->setContentsMargins(12, 8, 12, 8);
    hh->setSpacing(8);
    m_sessions = new QComboBox(header);
    m_sessions->setMinimumWidth(200);
    m_sessions->setToolTip(tr("Slicing sessions of this project"));
    auto *newSession = iconButton(QStringLiteral("plus"), tr("New slicing session"), header);
    hh->addWidget(m_sessions);
    hh->addWidget(newSession);
    hh->addSpacing(16);
    m_steps = new QButtonGroup(this);
    m_steps->setExclusive(true);
    for (int i = 0; i < 4; ++i) {
        auto *b = new QToolButton(header);
        b->setText(stepTitle(i));
        b->setCheckable(true);
        b->setProperty("role", QStringLiteral("segment"));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setMinimumHeight(30);
        m_steps->addButton(b, i);
        hh->addWidget(b);
        if (i < 3) {
            auto *arrow = new QLabel(header);
            arrow->setPixmap(Icons::pixmap(QStringLiteral("chevron-right"), 14, Theme::palette().textMuted));
            hh->addWidget(arrow);
        }
    }
    hh->addSpacing(12);
    m_stepHint = new ElidedLabel(QString(), header);
    m_stepHint->setProperty("role", QStringLiteral("muted"));
    hh->addWidget(m_stepHint, 1);
    root->addWidget(header);
    root->addWidget(separator(Qt::Horizontal, this));

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    root->addWidget(splitter, 1);

    // ---- Left: part list ----------------------------------------------------------------------------------
    m_list = new PartListPanel(m_context, splitter);
    m_list->setObjectName(QStringLiteral("Panel"));
    m_list->setMinimumWidth(240);

    // ---- Center: tools + canvas / preview ---------------------------------------------------------------
    auto *center = new QWidget(splitter);
    auto *cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    auto *optionsBar = new QWidget(center);
    optionsBar->setObjectName(QStringLiteral("Panel"));
    optionsBar->setFixedHeight(46);
    auto *ob = new QHBoxLayout(optionsBar);
    ob->setContentsMargins(12, 4, 12, 4);
    m_optionsHost = new QHBoxLayout;
    ob->addLayout(m_optionsHost, 1);
    auto *undo = iconButton(QStringLiteral("undo"), tr("Undo mask edit (Ctrl+Z)"), optionsBar);
    auto *redo = iconButton(QStringLiteral("redo"), tr("Redo (Ctrl+Shift+Z)"), optionsBar);
    ob->addWidget(separator(Qt::Vertical, optionsBar));
    ob->addWidget(undo);
    ob->addWidget(redo);
    cv->addWidget(optionsBar);
    cv->addWidget(separator(Qt::Horizontal, center));

    auto *canvasRow = new QHBoxLayout;
    canvasRow->setSpacing(0);
    m_palette = new QWidget(center);
    m_palette->setObjectName(QStringLiteral("Panel"));
    m_palette->setFixedWidth(52);
    auto *pv = new QVBoxLayout(m_palette);
    pv->setContentsMargins(6, 8, 6, 8);
    pv->setSpacing(2);
    m_tools = {new PartPickTool(m_state, m_context, this),
               new PartBrushTool(m_state, m_context, PartBrushTool::Mode::Paint, this),
               new PartBrushTool(m_state, m_context, PartBrushTool::Mode::Erase, this),
               new PartLassoTool(m_state, m_context, this),
               new PartWandTool(m_state, m_context, this),
               new PartCutTool(m_state, m_context, this),
               new PartBrushTool(m_state, m_context, PartBrushTool::Mode::Extend, this),
               new SkeletonTool(m_state, m_context, this),
               new HandTool(m_state, this)};
    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);
    for (Tool *tool : std::as_const(m_tools)) {
        auto *action = new QAction(Icons::get(tool->iconName()), tool->name(), this);
        action->setCheckable(true);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        Shortcuts::instance().bindAction(action, QStringLiteral("parts.tool.") + tool->id(), tr("Parts tools"), QStringLiteral("parts"),
                                         {QKeySequence(tool->shortcut())}, tool->name(), true);
        m_toolGroup->addAction(action);
        addAction(action);
        auto *button = new QToolButton(m_palette);
        button->setDefaultAction(action);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(40, 36);
        pv->addWidget(button);
        m_toolActions.insert(tool->id(), action);
        connect(action, &QAction::triggered, this, [this, id = tool->id()] { selectTool(id); });
        connect(tool, &Tool::message, this, [this](const QString &msg) { m_studio->notify(msg, QStringLiteral("info")); });
    }
    pv->addStretch();
    canvasRow->addWidget(m_palette);

    m_center = new QStackedWidget(center);
    m_canvas = new CanvasView(m_state, m_center);
    m_preview = new PuppetPreview(m_center);
    m_cubism = new CubismView(m_center);
    m_center->addWidget(m_canvas);
    m_center->addWidget(m_preview);
    m_center->addWidget(m_cubism);
    canvasRow->addWidget(m_center, 1);
    cv->addLayout(canvasRow, 1);

    auto *status = new QWidget(center);
    status->setObjectName(QStringLiteral("Panel"));
    auto *sh = new QHBoxLayout(status);
    sh->setContentsMargins(10, 3, 10, 3);
    m_hintLabel = mutedLabel(QString(), status);
    m_hintLabel->setWordWrap(false);
    m_overlayMode = new QComboBox(status);
    m_overlayMode->addItem(tr("Show parts"), 0);
    m_overlayMode->addItem(tr("Show result"), 1);
    m_overlayMode->addItem(tr("Source only"), 2);
    m_overlayOpacity = new SliderField(tr("Overlay"), 0, 100, 55, 0, QStringLiteral(" %"), status);
    m_overlayOpacity->setCompact(true);
    m_overlayOpacity->setMaximumWidth(240);
    auto *zoomOut = iconButton(QStringLiteral("zoom-out"), tr("Zoom out"), status);
    auto *zoomIn = iconButton(QStringLiteral("zoom-in"), tr("Zoom in"), status);
    auto *fit = iconButton(QStringLiteral("fit"), tr("Fit to window (Ctrl+0)"), status);
    m_zoomLabel = new QLabel(QStringLiteral("100%"), status);
    m_zoomLabel->setMinimumWidth(48);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomControls = new QWidget(status);
    auto *zh = new QHBoxLayout(m_zoomControls);
    zh->setContentsMargins(0, 0, 0, 0);
    zh->setSpacing(4);
    zh->addWidget(zoomOut);
    zh->addWidget(m_zoomLabel);
    zh->addWidget(zoomIn);
    zh->addWidget(fit);
    sh->addWidget(m_hintLabel, 1);
    sh->addWidget(m_overlayMode);
    sh->addWidget(m_overlayOpacity);
    sh->addWidget(m_zoomControls);
    cv->addWidget(status);

    // ---- Right: step panels -------------------------------------------------------------------------------
    m_panels = new QStackedWidget(splitter);
    m_panels->setObjectName(QStringLiteral("Panel"));
    m_panels->setMinimumWidth(320);
    // Each step scrolls on its own, so the tallest step does not set the window's minimum height.
    m_panels->addWidget(verticalScroll(buildSourcePanel(), m_panels));
    m_panels->addWidget(verticalScroll(buildMasksPanel(), m_panels));
    m_panels->addWidget(verticalScroll(buildGeneratePanel(), m_panels));
    m_panels->addWidget(verticalScroll(buildExportPanel(), m_panels));

    splitter->addWidget(m_list);
    splitter->addWidget(center);
    splitter->addWidget(m_panels);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({270, 900, 350});

    // ---- Wiring ------------------------------------------------------------------------------------------
    m_extensionTimer.setSingleShot(true);
    m_extensionTimer.setInterval(250);
    connect(&m_extensionTimer, &QTimer::timeout, this, &PartsPage::updateExtensionOverlay);
    m_assembledTimer.setSingleShot(true);
    m_assembledTimer.setInterval(150);
    connect(&m_assembledTimer, &QTimer::timeout, this, &PartsPage::updateAssembled);

    connect(newSession, &QToolButton::clicked, this, &PartsPage::newSession);
    connect(m_sessions, &QComboBox::activated, this, [this](int index) {
        const QString id = m_sessions->itemData(index).toString();
        if (!id.isEmpty() && (!m_set || id != m_set->id()))
            openSession(id);
    });
    connect(m_steps, &QButtonGroup::idClicked, this, [this](int step) {
        m_holdPreview = false;
        setStep(step);
    });
    connect(undo, &QToolButton::clicked, this, [this] {
        if (m_set)
            m_set->undoStack()->undo();
    });
    connect(redo, &QToolButton::clicked, this, [this] {
        if (m_set)
            m_set->undoStack()->redo();
    });
    connect(zoomIn, &QToolButton::clicked, m_canvas, &CanvasView::zoomIn);
    connect(zoomOut, &QToolButton::clicked, m_canvas, &CanvasView::zoomOut);
    connect(fit, &QToolButton::clicked, m_canvas, &CanvasView::fitToView);
    connect(m_canvas, &CanvasView::zoomChanged, this, [this](double z) { m_zoomLabel->setText(QStringLiteral("%1%").arg(std::lround(z * 100))); });
    connect(m_overlayMode, &QComboBox::currentIndexChanged, this, &PartsPage::applyOverlayMode);
    connect(m_overlayOpacity, &SliderField::valueChanged, this, &PartsPage::applyOverlayMode);

    connect(m_context, &PartEditContext::activeLabelChanged, this, [this](int label) {
        updateOverlay(m_set ? m_set->labels().rect() : QRect());
        scheduleExtensionOverlay();
        for (auto it = m_cardWidgets.begin(); it != m_cardWidgets.end(); ++it)
            updateCard(it.key());
        if (m_set && m_set->part(label))
            m_hintLabel->setText(tr("Selected: %1").arg(m_set->part(label)->displayName()));
    });
    connect(m_context, &PartEditContext::message, this, [this](const QString &msg) { m_studio->notify(msg); });
    connect(m_context, &PartEditContext::extensionPreview, this, &PartsPage::paintExtensionPreview);
    connect(m_context, &PartEditContext::skeletonChanged, m_canvas, &CanvasView::updateAll);
    if (auto *pick = qobject_cast<PartPickTool *>(m_tools.first())) {
        connect(pick, &PartPickTool::hovered, this, [this](int label) {
            const doc::Part *p = m_set ? m_set->part(label) : nullptr;
            m_canvas->setToolTip(p ? p->displayName() : QString());
        });
    }
    connect(m_list, &PartListPanel::splitRequested, this, &PartsPage::splitPart);
    connect(m_list, &PartListPanel::generateRequested, this, &PartsPage::generate);
    connect(m_list, &PartListPanel::cutToolRequested, this, [this] {
        setStep(MasksStep);
        selectTool(QStringLiteral("part-cut"));
    });
    connect(m_list, &PartListPanel::showUnassignedChanged, this, [this](bool on) {
        m_showUnassigned = on;
        updateOverlay(m_set ? m_set->labels().rect() : QRect());
    });
    connect(m_list, &PartListPanel::soloChanged, this, [this](int label) {
        m_solo = label;
        updateOverlay(m_set ? m_set->labels().rect() : QRect());
        scheduleAssembled();
    });

    connect(studio, &Studio::projectOpened, this, [this] {
        attach(nullptr);
        loadSessions();
    });
    connect(studio, &Studio::sliceRequested, this, &PartsPage::openAsset);
    connect(studio->parts(), &ai::PartService::segmentationApplied, this,
            [this](doc::PartSet *set, const QStringList &created, const QStringList &missing) {
                if (set != m_set)
                    return;
                QString msg = created.isEmpty() ? tr("Masks updated.") : tr("Found %n part(s).", nullptr, int(created.size()));
                if (!missing.isEmpty())
                    msg += QLatin1Char(' ') + tr("Not found: %1.").arg(missing.join(QStringLiteral(", ")));
                m_studio->notify(msg, QStringLiteral("success"));
                if (!m_context->activePart() && !m_set->parts().isEmpty())
                    m_context->setActiveLabel(m_set->partsInDrawOrder().last()->label);
            });

    // Shortcuts (page scope).
    auto shortcut = [this](const QString &id, const QString &name, const QStringList &keys, std::function<void()> fn) {
        auto *a = new QAction(this);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(a, &QAction::triggered, this, std::move(fn));
        addAction(a);
        QList<QKeySequence> defaults;
        for (const QString &k : keys)
            defaults << QKeySequence(k);
        Shortcuts::instance().bindAction(a, QStringLiteral("parts.") + id, tr("Parts"), QStringLiteral("parts"), defaults, name);
    };
    shortcut(QStringLiteral("undo"), tr("Undo"), {QStringLiteral("Ctrl+Z")}, [this] { if (m_set) m_set->undoStack()->undo(); });
    shortcut(QStringLiteral("redo"), tr("Redo"), {QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Ctrl+Y")},
             [this] { if (m_set) m_set->undoStack()->redo(); });
    shortcut(QStringLiteral("fit"), tr("Fit to window"), {QStringLiteral("Ctrl+0")}, [this] { m_canvas->fitToView(); });
    shortcut(QStringLiteral("zoom-in"), tr("Zoom in"), {QStringLiteral("Ctrl+=")}, [this] { m_canvas->zoomIn(); });
    shortcut(QStringLiteral("zoom-out"), tr("Zoom out"), {QStringLiteral("Ctrl+-")}, [this] { m_canvas->zoomOut(); });
    shortcut(QStringLiteral("delete-parts"), tr("Delete selected parts"), {QStringLiteral("Del")}, [this] {
        const QList<int> sel = m_list->selectedLabels();
        if (m_set && !sel.isEmpty()) {
            m_context->setActiveLabel(0);
            m_set->removeParts(sel);
        }
    });
    shortcut(QStringLiteral("generate"), tr("Generate selected parts"), {QStringLiteral("Ctrl+G")}, [this] { generate(m_list->selectedLabels()); });

    selectTool(QStringLiteral("part-pick"));
    setStep(SourceStep);
    if (studio->isProjectOpen())
        loadSessions();
}

PartsPage::~PartsPage()
{
    if (m_set)
        m_set->saveNow();
    m_canvas->setDocument(nullptr);
    delete m_display;
}

// ---- Panels ------------------------------------------------------------------------------------------------

QWidget *PartsPage::buildSourcePanel()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(10);
    v->addWidget(sectionLabel(tr("Source image")));
    m_sourceThumb = new QLabel(w);
    m_sourceThumb->setFixedSize(292, 220);
    m_sourceThumb->setAlignment(Qt::AlignCenter);
    m_sourceThumb->setObjectName(QStringLiteral("Card"));
    m_sourceThumb->setText(tr("Drop an image here\nor drag one from Browse project images"));
    v->addWidget(m_sourceThumb, 0, Qt::AlignHCenter);
    m_sourceInfo = mutedLabel(QString(), w);
    v->addWidget(m_sourceInfo);
    auto *row = new QHBoxLayout;
    auto *open = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Open image..."), w);
    auto *editor = new QPushButton(Icons::get(QStringLiteral("brush")), tr("Use editor image"), w);
    editor->setToolTip(tr("Use the flattened active document of the editor"));
    row->addWidget(open);
    row->addWidget(editor);
    v->addLayout(row);
    auto *layered = new QPushButton(Icons::get(QStringLiteral("layers")), tr("Open layered PSD..."), w);
    layered->setToolTip(tr("Start from a layered PSD (see-through output, a previous export or hand-made parts): every visible "
                           "layer becomes a part with its complete drawing."));
    v->addWidget(layered);
    m_removeBg = new QPushButton(Icons::get(QStringLiteral("scissors")), tr("Remove background"), w);
    m_removeBg->setToolTip(tr("Makes the background transparent with a background-removal provider; the character "
                              "silhouette then guides every mask."));
    v->addWidget(m_removeBg);

    v->addSpacing(6);
    v->addWidget(sectionLabel(tr("Part layout")));
    m_taxonomy = new QComboBox(w);
    m_taxonomy->setAccessibleName(tr("Part layout"));
    for (const doc::PartTaxonomy &t : doc::partTaxonomies()) {
        m_taxonomy->addItem(t.name, t.id);
        m_taxonomy->setItemData(m_taxonomy->count() - 1, t.description, Qt::ToolTipRole);
    }
    v->addWidget(m_taxonomy);

    v->addSpacing(6);
    v->addWidget(sectionLabel(tr("Live2D-ready source art")));
    auto *tips = mutedLabel(tr("• Front view, neutral face, eyes open, mouth closed\n"
                               "• Arms held away from the body, open hands\n"
                               "• Hair off the face, flat lighting, plain background\n"
                               "• At least 1500 px tall"), w);
    v->addWidget(tips);
    auto *presets = new QPushButton(Icons::get(QStringLiteral("person")), tr("Live2D pose presets..."), w);
    presets->setToolTip(tr("Open the pose library (category Live2D) to generate rig-ready source art"));
    v->addWidget(presets);
    v->addStretch();
    auto *next = primaryButton(tr("Continue to masks"), QStringLiteral("chevron-right"), w);
    v->addWidget(next);

    connect(open, &QPushButton::clicked, this, &PartsPage::chooseSourceFile);
    connect(layered, &QPushButton::clicked, this, [this] { openLayeredPsd(); });
    connect(editor, &QPushButton::clicked, this, &PartsPage::useEditorImage);
    connect(m_removeBg, &QPushButton::clicked, this, &PartsPage::removeBackground);
    // The pose library is the Poses plugin's page.
    presets->setVisible(m_studio->hasPage(QStringLiteral("poses")));
    connect(presets, &QPushButton::clicked, this, [this] { m_studio->showPage(QStringLiteral("poses")); });
    connect(next, &QPushButton::clicked, this, [this] { setStep(MasksStep); });
    connect(m_taxonomy, &QComboBox::activated, this, [this](int i) {
        if (m_set)
            m_set->setTaxonomy(m_taxonomy->itemData(i).toString());
    });
    return w;
}

QWidget *PartsPage::buildMasksPanel()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(10);
    v->addWidget(sectionLabel(tr("Automatic masks")));
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    m_segmenter = new QComboBox(w);
    m_scope = new QComboBox(w);
    m_scope->addItem(tr("Replace all masks"), static_cast<int>(ai::parts::SegmentScope::ReplaceAll));
    m_scope->addItem(tr("Only the selected parts' kinds"), static_cast<int>(ai::parts::SegmentScope::SelectedKinds));
    m_scope->addItem(tr("Only unassigned pixels"), static_cast<int>(ai::parts::SegmentScope::FillUnassigned));
    m_scope->setToolTip(tr("Locked parts are never changed."));
    form->addRow(tr("Segmenter"), m_segmenter);
    form->addRow(tr("Apply to"), m_scope);
    v->addLayout(form);
    m_segment = primaryButton(tr("Auto-segment"), QStringLiteral("sparkles"), w);
    v->addWidget(m_segment);
    m_segmentHint = mutedLabel(QString(), w);
    m_segmentHint->setWordWrap(true);
    v->addWidget(m_segmentHint);

    auto *skeletonRow = new QHBoxLayout;
    auto *placeSkeleton = new QPushButton(Icons::get(QStringLiteral("person")), tr("Edit skeleton"), w);
    placeSkeleton->setToolTip(tr("Show the body skeleton used by the skeleton segmenter and skeleton splits (K)"));
    auto *detect = new QPushButton(Icons::get(QStringLiteral("target")), tr("Detect skeleton"), w);
    detect->setToolTip(tr("Detect the pose from the image with a pose-detection provider (DWPose/OpenPose)"));
    skeletonRow->addWidget(placeSkeleton);
    skeletonRow->addWidget(detect);
    v->addLayout(skeletonRow);

    v->addSpacing(8);
    v->addWidget(sectionLabel(tr("Fix the masks")));
    auto *help = mutedLabel(tr("Select a part in the list, then:\n"
                               "• Brush (B) / Eraser (E) - paint pixels in or out\n"
                               "• Lasso (L) and Magic wand (W) - grab areas quickly\n"
                               "• Cut line (C) - cut a part in two\n"
                               "• Split ▸ left/right, islands or by skeleton\n"
                               "• Hidden area brush (X) - where the part continues behind others\n"
                               "• Ctrl+click on the canvas picks a part"), w);
    help->setWordWrap(true);
    v->addWidget(help);
    v->addStretch();
    auto *next = primaryButton(tr("Continue to generation"), QStringLiteral("chevron-right"), w);
    v->addWidget(next);

    auto reloadSegmenters = [this] {
        const QString current = m_segmenter->currentData().toString();
        m_segmenter->clear();
        for (ai::ImageProvider *p : m_studio->parts()->segmenters())
            m_segmenter->addItem(Icons::get(p->type().icon), p->displayName(), p->instanceId());
        m_segmenter->addItem(Icons::get(QStringLiteral("person")), tr("Skeleton (local, no model)"), kSkeletonSegmenter);
        const int idx = m_segmenter->findData(current);
        m_segmenter->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    reloadSegmenters();
    connect(m_studio->providers(), &ai::ProviderRegistry::providersChanged, this, reloadSegmenters);
    auto updateHint = [this] {
        const bool skeleton = m_segmenter->currentData().toString() == kSkeletonSegmenter;
        m_segmentHint->setText(skeleton ? tr("Splits the silhouette by the nearest bone. Adjust the skeleton (K) first for best results.")
                                        : (m_studio->parts()->segmenters().isEmpty()
                                               ? tr("Add a Lumen Part Server (see-through), a ComfyUI segmentation workflow or Gemini in Settings → Providers for model-based masks.")
                                               : tr("Model segmentation runs on the provider; you can keep editing meanwhile.")));
    };
    updateHint();
    connect(m_segmenter, &QComboBox::currentIndexChanged, this, updateHint);
    connect(m_studio->providers(), &ai::ProviderRegistry::providersChanged, this, updateHint);
    connect(m_segment, &QPushButton::clicked, this, &PartsPage::autoSegment);
    connect(placeSkeleton, &QPushButton::clicked, this, [this] { selectTool(QStringLiteral("part-skeleton")); });
    connect(detect, &QPushButton::clicked, this, &PartsPage::detectSkeleton);
    connect(next, &QPushButton::clicked, this, [this] { setStep(GenerateStep); });
    return w;
}

QWidget *PartsPage::buildGeneratePanel()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(10);
    v->addWidget(sectionLabel(tr("Complete hidden areas")));
    auto *form = new QFormLayout;
    m_strategy = new QComboBox(w);
    m_strategy->addItem(tr("Generate (inpaint)"), ai::completionStrategyName(ai::CompletionStrategy::Inpaint));
    m_strategy->addItem(tr("Model layers (see-through)"), ai::completionStrategyName(ai::CompletionStrategy::Decomposition));
    m_strategy->addItem(tr("Cut out only (no AI)"), ai::completionStrategyName(ai::CompletionStrategy::CutOnly));
    m_strategy->addItem(tr("Transparent (LayerDiffuse, ComfyUI)"), ai::completionStrategyName(ai::CompletionStrategy::LayerDiffuse));
    m_strategy->setItemData(0, tr("Paints the parts of each part hidden behind others with an image provider."), Qt::ToolTipRole);
    m_strategy->setItemData(1, tr("Uses the complete layers returned by a see-through segmentation."), Qt::ToolTipRole);
    m_strategy->setItemData(2, tr("Keeps only the visible pixels; instant."), Qt::ToolTipRole);
    m_strategy->setItemData(3, tr("Generates the hidden areas with real transparency (ComfyUI-layerdiffuse nodes and an SDXL checkpoint)."),
                            Qt::ToolTipRole);
    m_provider = new ProviderCombo(m_studio->providers(), ai::ProviderKind::Image, w);
    m_variants = new QSpinBox(w);
    m_variants->setRange(1, 4);
    m_variants->setToolTip(tr("Results per part; pick the best one in the list below"));
    form->addRow(tr("Method"), m_strategy);
    form->addRow(tr("Provider"), m_provider);
    form->addRow(tr("Variants"), m_variants);
    v->addLayout(form);
    auto *characterRow = new QHBoxLayout;
    characterRow->addWidget(new QLabel(tr("Character"), w));
    characterRow->addStretch();
    m_characterChip = new CharacterChip(m_studio, QStringLiteral("parts"), w);
    characterRow->addWidget(m_characterChip);
    v->addLayout(characterRow);
    m_character = new AutoTextEdit(w, 2, 5);
    v->addWidget(m_character);
    connect(m_characterChip, &CharacterChip::characterChanged, this, &PartsPage::updateCharacterHint);
    updateCharacterHint();
    auto *buttons = new QHBoxLayout;
    m_generateAll = primaryButton(tr("Generate all"), QStringLiteral("sparkles"), w);
    m_generateSelected = new QPushButton(tr("Selected"), w);
    m_generateSelected->setToolTip(tr("Generate the parts selected in the list (Ctrl+G)"));
    m_cancel = new QPushButton(Icons::get(QStringLiteral("stop")), QString(), w);
    m_cancel->setToolTip(tr("Cancel running part jobs"));
    buttons->addWidget(m_generateAll, 1);
    buttons->addWidget(m_generateSelected);
    buttons->addWidget(m_cancel);
    v->addLayout(buttons);

    v->addWidget(sectionLabel(tr("Expression variants")));
    auto *exprRow = new QHBoxLayout;
    m_expression = new QComboBox(w);
    for (const ai::parts::ExpressionKind &k : ai::parts::expressionKinds())
        m_expression->addItem(k.name, k.id);
    m_expression->setToolTip(tr("Repaints the eye or mouth parts (or the selected parts) and stores the drawing as a hidden "
                                "layer for blinking and lip-sync."));
    m_expressionButton = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Add"), w);
    exprRow->addWidget(m_expression, 1);
    exprRow->addWidget(m_expressionButton);
    v->addLayout(exprRow);
    m_checkExpressions = new QCheckBox(tr("Check each variant with the Critic"), w);
    m_checkExpressions->setToolTip(tr("The vision model compares the face before and after; a variant that changes the eye colour or the "
                                      "face, or barely shows the expression, gets a warning in the part list"));
    m_checkExpressions->setChecked(Settings::instance().value(QStringLiteral("parts/checkExpressions"), true).toBool());
    connect(m_checkExpressions, &QCheckBox::toggled, this,
            [](bool on) { Settings::instance().setValue(QStringLiteral("parts/checkExpressions"), on); });
    v->addWidget(m_checkExpressions);

    v->addWidget(sectionLabel(tr("Review")));
    auto *scroll = new QScrollArea(w);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *list = new QWidget(scroll);
    m_cards = new QVBoxLayout(list);
    m_cards->setContentsMargins(0, 0, 4, 0);
    m_cards->setSpacing(6);
    m_cards->addStretch();
    scroll->setWidget(list);
    v->addWidget(scroll, 1);
    auto *next = primaryButton(tr("Preview && export"), QStringLiteral("chevron-right"), w);
    v->addWidget(next);

    auto updateVisibility = [this] {
        const ai::CompletionStrategy s = ai::completionStrategyFromName(m_strategy->currentData().toString());
        const bool generates = s == ai::CompletionStrategy::Inpaint || s == ai::CompletionStrategy::LayerDiffuse;
        m_provider->setEnabled(generates);
        m_variants->setEnabled(s == ai::CompletionStrategy::Inpaint);
        m_character->setEnabled(generates);
    };
    updateVisibility();
    connect(m_strategy, &QComboBox::currentIndexChanged, this, [this, updateVisibility] {
        updateVisibility();
        saveGenerationSettings();
    });
    connect(m_provider, &ProviderCombo::providerChanged, this, &PartsPage::saveGenerationSettings);
    connect(m_variants, &QSpinBox::valueChanged, this, &PartsPage::saveGenerationSettings);
    connect(m_character, &QPlainTextEdit::textChanged, this, &PartsPage::saveGenerationSettings);
    connect(m_generateAll, &QPushButton::clicked, this, [this] {
        if (!m_set)
            return;
        QList<int> labels;
        for (const doc::Part *p : m_set->partsInDrawOrder()) {
            if (p->state != doc::PartState::Approved)
                labels.append(p->label);
        }
        generate(labels);
    });
    connect(m_generateSelected, &QPushButton::clicked, this, [this] { generate(m_list->selectedLabels()); });
    connect(m_cancel, &QPushButton::clicked, this, &PartsPage::cancelGeneration);
    connect(m_expressionButton, &QPushButton::clicked, this, [this] { generateExpression(m_expression->currentData().toString()); });
    connect(next, &QPushButton::clicked, this, [this] { setStep(ExportStep); });
    return w;
}

QWidget *PartsPage::buildExportPanel()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(14, 14, 14, 14);
    v->setSpacing(10);
    m_previewTabs = new QTabWidget(w);
    m_previewTabs->setDocumentMode(true);
    auto *puppetTab = new QWidget;
    auto *pv = new QVBoxLayout(puppetTab);
    pv->setContentsMargins(0, 8, 0, 0);
    pv->setSpacing(6);
    auto *toggles = new QHBoxLayout;
    m_idle = new QCheckBox(tr("Idle motion"), w);
    m_idle->setChecked(true);
    m_follow = new QCheckBox(tr("Follow mouse"), w);
    m_talk = new QCheckBox(tr("Talk"), w);
    toggles->addWidget(m_idle);
    toggles->addWidget(m_follow);
    toggles->addWidget(m_talk);
    toggles->addStretch();
    pv->addLayout(toggles);
    struct Spec
    {
        QString label;
        double min, max;
        double PuppetPreview::Params::*field;
    };
    const QList<Spec> specs{{tr("Angle X"), -30, 30, &PuppetPreview::Params::angleX},
                            {tr("Angle Y"), -30, 30, &PuppetPreview::Params::angleY},
                            {tr("Angle Z"), -30, 30, &PuppetPreview::Params::angleZ},
                            {tr("Body X"), -10, 10, &PuppetPreview::Params::bodyX},
                            {tr("Eye open"), 0, 100, &PuppetPreview::Params::eyeOpen},
                            {tr("Mouth open"), 0, 100, &PuppetPreview::Params::mouthOpen},
                            {tr("Hair"), -100, 100, &PuppetPreview::Params::hair}};
    for (const Spec &s : specs) {
        const bool percent = s.max == 100;
        auto *slider = new SliderField(s.label, s.min, s.max, s.field == &PuppetPreview::Params::eyeOpen ? 100 : 0, 0,
                                       percent ? QStringLiteral(" %") : QStringLiteral("°"), w);
        slider->setCompact(true);
        pv->addWidget(slider);
        m_paramSliders.append(slider);
        const auto field = s.field;
        connect(slider, &SliderField::valueChanged, this, [this, field, percent](double value) {
            if (m_loading)
                return;
            m_idle->setChecked(false);
            m_follow->setChecked(false);
            PuppetPreview::Params p = m_preview->params();
            p.*field = percent ? value / 100.0 : value;
            m_preview->setParams(p);
        });
    }
    connect(m_preview, &PuppetPreview::paramsChanged, this, [this, specs] {
        m_loading = true;
        const PuppetPreview::Params &p = m_preview->params();
        for (int i = 0; i < specs.size(); ++i) {
            const double value = p.*(specs[i].field);
            m_paramSliders[i]->setValue(specs[i].max == 100 ? value * 100.0 : value);
        }
        m_loading = false;
    });
    connect(m_idle, &QCheckBox::toggled, m_preview, &PuppetPreview::setAutoIdle);
    connect(m_follow, &QCheckBox::toggled, m_preview, &PuppetPreview::setFollowMouse);
    connect(m_talk, &QCheckBox::toggled, m_preview, &PuppetPreview::setTalking);
    pv->addStretch();
    m_cubismPanel = new CubismPanel(m_studio, m_cubism, w);
    m_previewTabs->addTab(puppetTab, Icons::get(QStringLiteral("parts")), tr("Lumen puppet"));
    m_previewTabs->addTab(m_cubismPanel, Icons::get(QStringLiteral("person")), tr("Cubism model"));
    m_previewTabs->setTabToolTip(0, tr("2.5D preview of the sliced parts"));
    m_previewTabs->setTabToolTip(1, tr("Preview a model rigged in Live2D Cubism Editor (.model3.json)"));
    v->addWidget(m_previewTabs, 1);
    connect(m_previewTabs, &QTabWidget::currentChanged, this, &PartsPage::showPreviewWidget);

    v->addSpacing(8);
    v->addWidget(sectionLabel(tr("Export")));
    m_includeCutOuts = new QCheckBox(tr("Include parts without a generated result (cut-outs)"), w);
    m_includeCutOuts->setChecked(true);
    m_includeOriginal = new QCheckBox(tr("Add the original image as a hidden reference layer"), w);
    m_includeOriginal->setChecked(true);
    v->addWidget(m_includeCutOuts);
    v->addWidget(m_includeOriginal);
    auto *psd = primaryButton(tr("Export layered PSD..."), QStringLiteral("download"), w);
    psd->setToolTip(tr("One named layer per part in draw order - import it into Live2D Cubism Editor"));
    auto *pngs = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Export PNG folder..."), w);
    pngs->setToolTip(tr("Full-canvas PNG per part plus parts.json (draw order, bounds, depth) and a preview"));
    auto *editor = new QPushButton(Icons::get(QStringLiteral("layers")), tr("Open as layers in the editor"), w);
    v->addWidget(psd);
    v->addWidget(pngs);
    v->addWidget(editor);
    auto *note = mutedLabel(tr("In Cubism Editor use File ▸ Open (PSD); every part becomes an ArtMesh ready for rigging."), w);
    note->setWordWrap(true);
    v->addWidget(note);
    v->addStretch();
    connect(psd, &QPushButton::clicked, this, &PartsPage::exportPsd);
    connect(pngs, &QPushButton::clicked, this, &PartsPage::exportPngs);
    connect(editor, &QPushButton::clicked, this, &PartsPage::openInEditor);
    return w;
}

// ---- Sessions ------------------------------------------------------------------------------------------------

void PartsPage::loadSessions(const QString &select)
{
    doc::Project *project = m_studio->project();
    if (!project || !project->isOpen())
        return;
    QPointer<PartsPage> self(this);
    project->records(QString::fromLatin1(doc::PartSet::kRecordType)).then(this, [self, select](QList<doc::Record> records) {
        if (!self)
            return;
        std::sort(records.begin(), records.end(), [](const doc::Record &a, const doc::Record &b) { return a.modified > b.modified; });
        self->m_sessions->blockSignals(true);
        self->m_sessions->clear();
        for (const doc::Record &r : records)
            self->m_sessions->addItem(r.thumbnail.isNull() ? QIcon() : QIcon(QPixmap::fromImage(r.thumbnail)), r.name, r.id);
        if (self->m_set && self->m_sessions->findData(self->m_set->id()) < 0 && self->m_set->hasSource())
            self->m_sessions->insertItem(0, self->m_set->name().isEmpty() ? tr("Untitled parts") : self->m_set->name(), self->m_set->id());
        if (self->m_sessions->count() == 0)
            self->m_sessions->addItem(tr("No sessions yet"));
        const QString want = !select.isEmpty() ? select : (self->m_set ? self->m_set->id() : QString());
        const int idx = self->m_sessions->findData(want);
        self->m_sessions->setCurrentIndex(std::max(0, idx));
        self->m_sessions->blockSignals(false);
        // Open the most recent session on first load.
        if (!self->m_set && !records.isEmpty() && select.isEmpty())
            self->openSession(records.first().id);
    });
}

void PartsPage::openSession(const QString &recordId)
{
    doc::Project *project = m_studio->project();
    if (!project)
        return;
    QPointer<PartsPage> self(this);
    QPointer<doc::Project> guard(project);
    project->records(QString::fromLatin1(doc::PartSet::kRecordType)).then(this, [self, guard, recordId](QList<doc::Record> records) {
        if (!self || !guard)
            return;
        for (const doc::Record &r : records) {
            if (r.id != recordId)
                continue;
            const QJsonObject json = r.json;
            async::run([json] { return doc::PartSet::decode(json); }).then(self, [self, guard, recordId](doc::PartSet::Snapshot snap) {
                if (!self || !guard)
                    return;
                guard->loadImage(snap.sourceAssetId)
                    .then(self, [](QImage source) {
                        return async::run([source] {
                            const QImage normalized = img::normalized(source);
                            return std::make_pair(normalized, img::silhouetteMask(normalized));
                        });
                    })
                    .unwrap()
                    .then(self, [self, recordId, snap](std::pair<QImage, QImage> loaded) {
                        if (!self)
                            return;
                        auto *set = new doc::PartSet(self);
                        set->restore(recordId, snap, loaded.first, loaded.second);
                        self->attach(set);
                        self->setStep(self->m_holdPreview ? int(ExportStep) : snap.step);
                    })
                    .onFailed(self, [self](const std::exception &e) {
                        if (self)
                            self->m_studio->notify(tr("Cannot open the session: %1").arg(describeException(e)), QStringLiteral("error"));
                    });
            });
            return;
        }
    });
}

void PartsPage::newSession()
{
    auto *set = new doc::PartSet(this);
    set->setTaxonomy(m_taxonomy->currentData().toString());
    attach(set);
    setStep(SourceStep);
}

void PartsPage::attach(doc::PartSet *set)
{
    if (m_set == set)
        return;
    cancelGeneration();
    if (m_set) {
        doc::PartSet *old = m_set;
        old->disconnect(this);
        // Save and drop the old session once written.
        old->saveNow().then(this, [old] { old->deleteLater(); });
    }
    m_set = set;
    m_solo = 0;
    m_context->setSet(set);
    if (set) {
        set->bind(m_studio->project());
        connect(set, &doc::PartSet::labelsChanged, this, [this](const QRect &r) {
            updateOverlay(r);
            scheduleExtensionOverlay();
        });
        connect(set, &doc::PartSet::partsChanged, this, [this] {
            updateOverlay(m_set->labels().rect());
            scheduleExtensionOverlay();
            rebuildCards();
            scheduleAssembled();
            updateStepAvailability();
            if (m_context->activeLabel() && !m_set->part(m_context->activeLabel()))
                m_context->setActiveLabel(0);
        });
        connect(set, &doc::PartSet::extensionChanged, this, &PartsPage::scheduleExtensionOverlay);
        connect(set, &doc::PartSet::partStateChanged, this, &PartsPage::updateCard);
        connect(set, &doc::PartSet::resultChanged, this, [this](int label) {
            updateCard(label);
            scheduleAssembled();
        });
        connect(set, &doc::PartSet::sourceChanged, this, [this] {
            rebuildDisplay();
            refreshSourceInfo();
            updateStepAvailability();
        });
        connect(set, &doc::PartSet::taxonomyChanged, this, [this] {
            const int idx = m_taxonomy->findData(m_set->taxonomyId());
            if (idx >= 0)
                m_taxonomy->setCurrentIndex(idx);
        });
        connect(set, &doc::PartSet::saved, this, [this] {
            if (m_sessions->findData(m_set->id()) < 0)
                loadSessions(m_set->id());
        });
        // Generation settings stored with the session.
        m_loading = true;
        const QJsonObject s = set->settings();
        m_strategy->setCurrentIndex(std::max(0, m_strategy->findData(s.value(QStringLiteral("strategy")).toString(QStringLiteral("inpaint")))));
        if (s.contains(QStringLiteral("provider")))
            m_provider->setProviderId(s.value(QStringLiteral("provider")).toString());
        m_variants->setValue(s.value(QStringLiteral("variants")).toInt(1));
        m_character->setPlainText(s.value(QStringLiteral("character")).toString());
        const int t = m_taxonomy->findData(set->taxonomyId());
        if (t >= 0)
            m_taxonomy->setCurrentIndex(t);
        m_loading = false;
        m_studio->parts()->loadResults(set);
    }
    rebuildDisplay();
    refreshSourceInfo();
    rebuildCards();
    updateStepAvailability();
    loadSessions(set ? set->id() : QString());
}

void PartsPage::setStep(int step)
{
    // The preview step also hosts the Cubism model preview, which needs no slicing session.
    if ((!m_set || !m_set->hasSource()) && step != ExportStep)
        step = SourceStep;
    step = std::clamp(step, 0, 3);
    if (QAbstractButton *b = m_steps->button(step))
        b->setChecked(true);
    m_panels->setCurrentIndex(step);
    m_stepHint->setFullText(stepHint(step));
    const bool preview = step == ExportStep;
    if (preview)
        showPreviewWidget();
    else
        m_center->setCurrentWidget(m_canvas);
    m_palette->setVisible(step == MasksStep || step == GenerateStep);
    m_overlayMode->setVisible(step != SourceStep && !preview);
    m_overlayOpacity->setVisible(step != SourceStep && !preview);
    m_zoomControls->setVisible(!preview);
    if (m_set)
        m_set->setStep(step);
    if (step == SourceStep) {
        m_overlayMode->setCurrentIndex(2);
        selectTool(QStringLiteral("hand"));
    } else if (step == MasksStep) {
        m_overlayMode->setCurrentIndex(0);
        if (m_canvas->tool() && m_canvas->tool()->id() == QLatin1String("hand"))
            selectTool(QStringLiteral("part-pick"));
    } else if (step == GenerateStep) {
        m_overlayMode->setCurrentIndex(1);
        rebuildCards();
    } else {
        refreshPreview();
    }
    applyOverlayMode();
}

void PartsPage::updateStepAvailability()
{
    const bool hasSource = m_set && m_set->hasSource();
    const bool hasParts = hasSource && !m_set->parts().isEmpty();
    m_steps->button(MasksStep)->setEnabled(hasSource);
    m_steps->button(GenerateStep)->setEnabled(hasParts);
    m_steps->button(ExportStep)->setEnabled(true);
    m_segment->setEnabled(hasSource);
    m_generateAll->setEnabled(hasParts);
    m_generateSelected->setEnabled(hasParts);
}

// ---- Source -------------------------------------------------------------------------------------------------

void PartsPage::openAsset(const QString &assetId)
{
    newSession();
    setSourceAsset(assetId, true);
}

void PartsPage::automate(bool segment, int step)
{
    m_automation = Automation{true, segment, step};
}

void PartsPage::runAutomation()
{
    const Automation a = m_automation;
    m_automation.active = false;
    if (!m_set || !a.segment) {
        setStep(a.step);
        return;
    }
    const pose::Person skeleton = ai::parts::fitSkeleton(m_set->silhouette());
    m_context->setSkeleton(skeleton);
    Job *job = m_studio->parts()->segmentWithSkeleton(m_set, skeleton, ai::parts::SegmentOptions{});
    QPointer<PartsPage> self(this);
    connect(job, &Job::finished, this, [self, a](Job *j) {
        if (!self || !self->m_set || j->state() != Job::State::Succeeded)
            return;
        QList<int> labels;
        for (const doc::Part &p : self->m_set->parts())
            labels.append(p.label);
        if (a.step >= GenerateStep) {
            self->m_strategy->setCurrentIndex(2); // cut-only: no provider needed
            self->generate(labels);
        }
        if (!labels.isEmpty())
            self->m_context->setActiveLabel(labels.first());
        self->setStep(a.step);
    });
}

void PartsPage::chooseSourceFile()
{
    doc::Project *project = m_studio->project();
    if (!project)
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose a character image"), dir,
                                                      tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (file.isEmpty())
        return;
    QPointer<PartsPage> self(this);
    project->importFile(file, doc::AssetKind::Reference)
        .then(this, [self](doc::AssetInfo info) {
            if (self)
                self->setSourceAsset(info.id, true);
        })
        .onFailed(this, [self](const std::exception &e) {
            if (self)
                self->m_studio->notify(describeException(e), QStringLiteral("error"));
        });
}

void PartsPage::openLayeredPsd(const QString &pathIn)
{
    doc::Project *project = m_studio->project();
    if (!project)
        return;
    QString path = pathIn;
    if (path.isEmpty()) {
        const QString dir = Settings::instance().value(QStringLiteral("parts/exportDir"),
                                                       QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
        path = QFileDialog::getOpenFileName(this, tr("Open layered PSD"), dir, tr("Layered Photoshop (*.psd)"));
        if (path.isEmpty())
            return;
    }
    struct Loaded
    {
        QImage composite;
        QImage silhouette;
        QList<ai::parts::SegmentLayer> layers;
        int skipped = 0;
    };
    m_studio->notify(tr("Reading %1...").arg(QFileInfo(path).fileName()));
    QPointer<PartsPage> self(this);
    QPointer<doc::Project> guard(project);
    const QString name = QFileInfo(path).completeBaseName();
    async::run([path] {
        QString error;
        const std::optional<psd::File> file = psd::read(path, &error);
        if (!file)
            throw Error(QObject::tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), error));
        Loaded l;
        l.layers = ai::parts::layersFromPsd(*file, &l.skipped);
        if (l.layers.isEmpty())
            throw Error(QObject::tr("%1 has no visible pixel layers.").arg(QFileInfo(path).fileName()));
        // The parts define the character: composite of the imported (visible) layers.
        l.composite = QImage(file->size, QImage::Format_ARGB32_Premultiplied);
        l.composite.fill(Qt::transparent);
        QPainter p(&l.composite);
        for (const ai::parts::SegmentLayer &s : std::as_const(l.layers))
            p.drawImage(0, 0, s.image);
        p.end();
        l.silhouette = img::silhouetteMask(l.composite);
        return l;
    }).then(this, [self, guard, name](Loaded loaded) {
        if (!self || !guard)
            return;
        guard->addAsset(loaded.composite, doc::AssetKind::Reference, name, QJsonObject{{QStringLiteral("source"), QStringLiteral("psd")}})
            .then(self, [self, loaded, name](doc::AssetInfo info) {
                if (!self)
                    return;
                self->newSession();
                doc::PartSet *set = self->m_set;
                set->setName(name);
                set->setSource(loaded.composite, info.id, loaded.silhouette);
                ai::parts::SegmentOptions options;
                options.clipToSilhouette = false;
                options.minIsland = 4;
                options.provenance = QStringLiteral("psd");
                Job *job = self->m_studio->parts()->applyLayers(set, loaded.layers, options);
                const int skipped = loaded.skipped;
                QPointer<doc::PartSet> setGuard(set);
                connect(job, &Job::finished, self, [self, setGuard, skipped](Job *j) {
                    if (!self || !setGuard)
                        return;
                    if (j->state() != Job::State::Succeeded) {
                        if (j->state() == Job::State::Failed)
                            self->m_studio->notify(j->errorString(), QStringLiteral("error"));
                        return;
                    }
                    // Every layer is already a complete drawing: turn the layers into results.
                    QList<int> labels;
                    for (const doc::Part &p : setGuard->parts())
                        labels.append(p.label);
                    ai::CompletionOptions completion;
                    completion.strategy = ai::CompletionStrategy::Decomposition;
                    self->m_studio->parts()->complete(setGuard, labels, completion);
                    QString msg = tr("Imported %n part layer(s).", nullptr, int(labels.size()));
                    if (skipped)
                        msg += QLatin1Char(' ') + tr("%n hidden layer(s) skipped.", nullptr, skipped);
                    self->m_studio->notify(msg, QStringLiteral("success"));
                    self->setStep(GenerateStep);
                });
                self->setStep(MasksStep);
                self->m_canvas->fitToView();
            });
    }).onFailed(this, [self](const std::exception &e) {
        if (self)
            self->m_studio->notify(describeException(e), QStringLiteral("error"));
    });
}

void PartsPage::useEditorImage()
{
    doc::Document *d = m_studio->activeDocument();
    doc::Project *project = m_studio->project();
    if (!d || !project) {
        m_studio->notify(tr("Open an image in the editor first."));
        return;
    }
    const QImage flat = d->flattened();
    const QString name = d->name();
    QPointer<PartsPage> self(this);
    project->addAsset(flat, doc::AssetKind::Edit, name.isEmpty() ? tr("Editor image") : name)
        .then(this, [self](doc::AssetInfo info) {
            if (self)
                self->setSourceAsset(info.id, true);
        });
}

void PartsPage::setSourceAsset(const QString &assetId, bool keepSession)
{
    doc::Project *project = m_studio->project();
    if (!project)
        return;
    if (!m_set || !keepSession)
        newSession();
    // A session with masks keeps its parts only when the new image has the same size.
    if (m_set->hasSource() && !m_set->parts().isEmpty()) {
        const auto answer = QMessageBox::question(this, tr("Replace source"),
                                                  tr("Start a new slicing session for this image? Choose No to replace the "
                                                     "image of the current session (masks are kept only if the size matches)."),
                                                  QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Yes)
            newSession();
    }
    const auto info = project->asset(assetId);
    adoptCharacterOf(assetId);
    QPointer<PartsPage> self(this);
    QPointer<doc::PartSet> set(m_set);
    m_sourceInfo->setText(tr("Loading..."));
    project->loadImage(assetId)
        .then(this, [](QImage image) {
            return async::run([image] {
                const QImage normalized = img::normalized(image);
                return std::make_pair(normalized, img::silhouetteMask(normalized));
            });
        })
        .unwrap()
        .then(this, [self, set, assetId, info](std::pair<QImage, QImage> loaded) {
            if (!self || !set)
                return;
            if (set->name().isEmpty())
                set->setName(info ? info->name : tr("Character"));
            set->setSource(loaded.first, assetId, loaded.second);
            self->setStep(MasksStep);
            self->m_canvas->fitToView();
            if (self->m_automation.active)
                self->runAutomation();
        })
        .onFailed(this, [self](const std::exception &e) {
            if (self)
                self->m_studio->notify(describeException(e), QStringLiteral("error"));
        });
}

void PartsPage::removeBackground()
{
    if (!m_set || !m_set->hasSource())
        return;
    ai::ImageProvider *provider = m_studio->generation()->providerForUtility(ai::Utility::RemoveBackground);
    if (!provider) {
        m_studio->notify(tr("No configured provider can remove backgrounds (SD WebUI with rembg, Stability...)."), QStringLiteral("warning"));
        return;
    }
    Job *job = m_studio->generation()->submitUtility(provider->instanceId(), ai::Utility::RemoveBackground, m_set->source(), {},
                                                     tr("Remove background"));
    QPointer<PartsPage> self(this);
    QPointer<doc::PartSet> set(m_set);
    m_removeBg->setEnabled(false);
    connect(job, &Job::finished, this, [self, set](Job *j) {
        if (!self)
            return;
        self->m_removeBg->setEnabled(true);
        if (j->state() != Job::State::Succeeded || !set)
            return;
        const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
        if (!out.assets.isEmpty())
            self->setSourceAsset(out.assets.first().id, true);
    });
}

void PartsPage::refreshSourceInfo()
{
    if (!m_set || !m_set->hasSource()) {
        m_sourceThumb->setPixmap(QPixmap());
        m_sourceThumb->setText(tr("Drop an image here\nor drag one from Browse project images"));
        m_sourceInfo->clear();
        return;
    }
    m_sourceThumb->setPixmap(checkerThumb(m_set->source(), m_sourceThumb->size()));
    const QSize s = m_set->size();
    QString info = tr("%1 × %2 px").arg(s.width()).arg(s.height());
    if (s.height() < 1200)
        info += QStringLiteral(" · ") + tr("small for Live2D - consider upscaling");
    m_sourceInfo->setText(info);
}

void PartsPage::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(QString::fromLatin1(kAssetMime)) || e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void PartsPage::dropEvent(QDropEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (mime->hasFormat(QString::fromLatin1(kAssetMime))) {
        const QStringList ids = QString::fromUtf8(mime->data(QString::fromLatin1(kAssetMime))).split(u'\n', Qt::SkipEmptyParts);
        if (!ids.isEmpty())
            setSourceAsset(ids.first(), true);
        e->acceptProposedAction();
        return;
    }
    if (mime->hasUrls() && m_studio->project()) {
        const QString file = mime->urls().first().toLocalFile();
        if (file.isEmpty())
            return;
        if (file.endsWith(QLatin1String(".psd"), Qt::CaseInsensitive)) {
            openLayeredPsd(file);
            e->acceptProposedAction();
            return;
        }
        QPointer<PartsPage> self(this);
        m_studio->project()->importFile(file, doc::AssetKind::Reference).then(this, [self](doc::AssetInfo info) {
            if (self)
                self->setSourceAsset(info.id, true);
        });
        e->acceptProposedAction();
    }
}

void PartsPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_center->currentWidget() == m_preview)
        refreshPreview();
}

// ---- Masks --------------------------------------------------------------------------------------------------

void PartsPage::autoSegment()
{
    if (!m_set || !m_set->hasSource())
        return;
    ai::parts::SegmentOptions options;
    options.scope = static_cast<ai::parts::SegmentScope>(m_scope->currentData().toInt());
    if (options.scope == ai::parts::SegmentScope::SelectedKinds) {
        for (int label : m_list->selectedLabels()) {
            if (const doc::Part *p = m_set->part(label); p && !p->tag.isEmpty())
                options.kinds.append(p->tag);
        }
        if (options.kinds.isEmpty()) {
            m_studio->notify(tr("Select the parts whose kinds should be segmented again."), QStringLiteral("warning"));
            return;
        }
    }
    const QString segmenter = m_segmenter->currentData().toString();
    Job *job = nullptr;
    if (segmenter == kSkeletonSegmenter) {
        if (!m_context->skeleton()) {
            selectTool(QStringLiteral("part-skeleton"));
            m_studio->notify(tr("Adjust the skeleton to the character, then press Auto-segment again."));
            return;
        }
        job = m_studio->parts()->segmentWithSkeleton(m_set, *m_context->skeleton(), options);
    } else {
        job = m_studio->parts()->segment(m_set, segmenter, options);
    }
    m_segment->setEnabled(false);
    m_segment->setText(tr("Segmenting..."));
    QPointer<PartsPage> self(this);
    connect(job, &Job::progressChanged, this, [self](double, const QString &status) {
        if (self && !status.isEmpty())
            self->m_segmentHint->setText(status);
    });
    connect(job, &Job::finished, this, [self](Job *j) {
        if (!self)
            return;
        self->m_segment->setEnabled(true);
        self->m_segment->setText(tr("Auto-segment"));
        if (j->state() == Job::State::Failed)
            self->m_studio->notify(j->errorString(), QStringLiteral("error"));
        self->m_segmentHint->setText(j->state() == Job::State::Succeeded ? j->result().toString() : j->errorString());
    });
}

void PartsPage::detectSkeleton()
{
    if (!m_set || !m_set->hasSource())
        return;
    if (!m_studio->generation()->providerForUtility(ai::Utility::DetectPose)) {
        m_studio->notify(tr("No provider can detect poses (SD WebUI with ControlNet, or ComfyUI with controlnet_aux). "
                            "Place the skeleton by hand instead."), QStringLiteral("warning"));
        selectTool(QStringLiteral("part-skeleton"));
        return;
    }
    Job *job = m_studio->generation()->submitUtility(QString(), ai::Utility::DetectPose, img::flatten(m_set->source(), Qt::white), {},
                                                     tr("Detect skeleton"), false);
    QPointer<PartsPage> self(this);
    const QSize size = m_set->size();
    connect(job, &Job::finished, this, [self, size](Job *j) {
        if (!self)
            return;
        if (j->state() != Job::State::Succeeded) {
            if (j->state() == Job::State::Failed)
                self->m_studio->notify(j->errorString(), QStringLiteral("error"));
            return;
        }
        const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
        const auto pose = pose::Pose::fromOpenPoseJson(out.info.value(QStringLiteral("openpose")).toObject(), nullptr, size);
        if (!pose || pose->people.isEmpty()) {
            self->m_studio->notify(tr("The detector returned no keypoints. Place the skeleton by hand (K)."), QStringLiteral("warning"));
            self->selectTool(QStringLiteral("part-skeleton"));
            return;
        }
        // Keypoints come in the detector's canvas; map them to the source size.
        pose::Pose scaled = *pose;
        scaled.resizeCanvas(size, true);
        // The largest detected person is the character.
        pose::Person best = scaled.people.first();
        for (const pose::Person &p : scaled.people) {
            if (p.bounds().width() * p.bounds().height() > best.bounds().width() * best.bounds().height())
                best = p;
        }
        self->m_context->setSkeleton(best);
        self->selectTool(QStringLiteral("part-skeleton"));
        self->m_studio->notify(tr("Skeleton detected - adjust the joints if needed."), QStringLiteral("success"));
    });
}

void PartsPage::splitPart(int label, PartListPanel::Split mode)
{
    if (!m_set || !m_set->part(label))
        return;
    const doc::Part part = *m_set->part(label);
    const QImage mask = m_set->partMask(label);
    QPointer<doc::PartSet> set(m_set);
    QPointer<PartsPage> self(this);
    if (mode == PartListPanel::Split::Skeleton && !m_context->skeleton()) {
        setStep(MasksStep);
        selectTool(QStringLiteral("part-skeleton"));
        m_studio->notify(tr("Place the skeleton on the character first (drag the joints), then split again."));
        return;
    }
    // Centre line: skeleton neck, else the character silhouette centre.
    double centerX = m_set->size().width() / 2.0;
    if (m_context->skeleton() && m_context->skeleton()->body[pose::Neck].visible)
        centerX = m_context->skeleton()->body[pose::Neck].pos.x();
    else if (!m_set->silhouette().isNull())
        centerX = QRectF(img::maskBounds(m_set->silhouette(), 128)).center().x();
    const std::optional<pose::Person> skeleton = m_context->skeleton();
    const QString taxonomy = m_set->taxonomyId();
    struct Result
    {
        QList<QImage> pieces;
        QStringList names, sides, tags;
        QString error;
    };
    async::run([mode, mask, part, centerX, skeleton, taxonomy] {
        Result r;
        if (mode == PartListPanel::Split::LeftRight) {
            const auto [left, right] = img::splitByVerticalLine(mask, centerX);
            if (img::maskIsEmpty(left, 128) || img::maskIsEmpty(right, 128)) {
                r.error = QObject::tr("The part lies on one side of the centre line.");
                return r;
            }
            // The image's right half is the character's left side.
            r.pieces = {right, left};
            r.names = {part.name, part.name};
            r.sides = {QStringLiteral("L"), QStringLiteral("R")};
        } else if (mode == PartListPanel::Split::Islands) {
            const qint64 minArea = std::max<qint64>(16, qint64(mask.width()) * mask.height() / 20000);
            r.pieces = img::splitComponents(mask, minArea);
            if (r.pieces.size() < 2) {
                r.error = QObject::tr("The part is a single connected area.");
                return r;
            }
            for (int i = 0; i < r.pieces.size(); ++i)
                r.names << (i == 0 ? part.name : QStringLiteral("%1 %2").arg(part.name).arg(i + 1));
        } else {
            const QString t = part.tag;
            const bool arms = t.contains(QLatin1String("arm")) || t == QLatin1String("handwear") || t == QLatin1String("hand");
            const bool legs = t.contains(QLatin1String("leg")) || t == QLatin1String("thigh") || t == QLatin1String("shin")
                              || t == QLatin1String("footwear");
            const QList<ai::parts::BoneGroup> groups = (arms || legs) ? ai::parts::limbGroups(*skeleton, taxonomy, arms, legs)
                                                                      : ai::parts::skeletonGroups(*skeleton, taxonomy);
            const QList<QImage> masks = ai::parts::splitBySkeleton(mask, groups);
            for (int i = 0; i < groups.size(); ++i) {
                if (img::maskIsEmpty(masks[i], 128))
                    continue;
                r.pieces << masks[i];
                r.names << groups[i].name;
                r.sides << groups[i].side;
                r.tags << groups[i].tag;
            }
            if (r.pieces.size() < 2)
                r.error = QObject::tr("The skeleton assigns the whole part to one bone - adjust the joints.");
        }
        return r;
    }).then(this, [self, set, label](Result r) {
        if (!self || !set)
            return;
        if (!r.error.isEmpty()) {
            self->m_studio->notify(r.error, QStringLiteral("warning"));
            return;
        }
        const QList<int> labels = set->splitPart(label, r.pieces, r.names, r.sides, tr("Split part"), r.tags);
        self->m_studio->notify(tr("Split into %n parts.", nullptr, int(labels.size())), QStringLiteral("success"));
    });
}

void PartsPage::selectTool(const QString &id)
{
    for (Tool *tool : std::as_const(m_tools)) {
        if (tool->id() != id)
            continue;
        if (QAction *a = m_toolActions.value(id))
            a->setChecked(true);
        m_canvas->setTool(tool);
        if (m_options) {
            m_options->deleteLater();
            m_options = nullptr;
        }
        m_options = new QWidget;
        auto *h = new QHBoxLayout(m_options);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(16);
        h->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tool->name())));
        if (QWidget *opts = tool->createOptions(m_options))
            h->addWidget(opts, 1);
        else
            h->addStretch();
        m_optionsHost->addWidget(m_options);
        m_hintLabel->setText(tool->hint());
        // Hidden-area painting shows the hatched overlay.
        scheduleExtensionOverlay();
        return;
    }
}

// ---- Generation ----------------------------------------------------------------------------------------------

void PartsPage::saveGenerationSettings()
{
    if (m_loading || !m_set)
        return;
    QJsonObject s = m_set->settings();
    s.insert(QStringLiteral("strategy"), m_strategy->currentData().toString());
    s.insert(QStringLiteral("provider"), m_provider->providerId());
    s.insert(QStringLiteral("variants"), m_variants->value());
    s.insert(QStringLiteral("character"), m_character->text());
    m_set->setSettings(s);
}

void PartsPage::generate(const QList<int> &labels)
{
    if (!m_set || labels.isEmpty())
        return;
    ai::CompletionOptions options;
    options.strategy = ai::completionStrategyFromName(m_strategy->currentData().toString());
    options.providerId = m_provider->providerId();
    options.variants = m_variants->value();
    options.characterSummary = m_character->text().trimmed();
    QList<int> todo;
    for (int label : labels) {
        const doc::Part *p = m_set->part(label);
        if (!p || p->state == doc::PartState::Queued || p->state == doc::PartState::Generating)
            continue;
        if (p->provenance.startsWith(QLatin1String("variant:"))) {
            // Expression variants are regenerated from the face, not completed from a mask.
            if (labels.size() == 1)
                generateExpression(p->provenance.mid(8));
            continue;
        }
        if (p->pixels > 0)
            todo.append(label);
    }
    if (todo.isEmpty()) {
        if (labels.size() > 1 || !m_set->part(labels.first()) || !m_set->part(labels.first())->provenance.startsWith(QLatin1String("variant:")))
            m_studio->notify(tr("Nothing to generate: the parts are empty or already running."));
        return;
    }
    if (m_panels->currentIndex() != GenerateStep)
        setStep(GenerateStep);
    QPointer<PartsPage> self(this);
    QPointer<doc::PartSet> set(m_set);
    withCharacter(options.providerId, [self, set, todo, options](const ai::CharacterContext &character, const QString &family) mutable {
        if (!self || !set || set != self->m_set)
            return;
        options.character = character;
        options.modelFamily = family;
        if (options.characterSummary.isEmpty())
            options.characterSummary = ai::Director::appearanceTags(character.sheet);
        const QList<Job *> jobs = self->m_studio->parts()->complete(set, todo, options);
        for (Job *j : jobs)
            self->m_running.append(j);
    });
}

void PartsPage::withCharacter(const QString &providerId, std::function<void(const ai::CharacterContext &, const QString &)> then)
{
    const QString id = m_characterChip->characterId();
    if (id.isEmpty())
        return then(ai::CharacterContext{}, QString());
    const QString family = checkpointFamily(m_studio, m_studio->providers()->imageProvider(providerId));
    m_studio->characterContext(id).then(this, [then, family](const ai::CharacterContext &c) { then(c, family); });
}

void PartsPage::updateCharacterHint()
{
    const auto entry = m_studio->characters()->entry(m_characterChip->characterId());
    if (!entry) {
        m_character->setPlaceholderText(tr("Character description (optional): hair colour, outfit, art style..."));
        return;
    }
    const bool lora = ai::CharacterLora::fromJson(entry->lora).isValid();
    m_character->setPlaceholderText(lora ? tr("Empty: %1's LoRA and description are used (%2)").arg(entry->name, ai::Director::appearanceTags(entry->sheet))
                                         : tr("Empty: %1's description is used (%2)").arg(entry->name, ai::Director::appearanceTags(entry->sheet)));
}

void PartsPage::adoptCharacterOf(const QString &assetId)
{
    doc::Project *project = m_studio->project();
    const auto info = project ? project->asset(assetId) : std::nullopt;
    if (!info)
        return;
    QPointer<PartsPage> self(this);
    auto adopt = [self](const QString &id) {
        if (!self || id.isEmpty() || id == self->m_studio->activeCharacter())
            return;
        if (const auto entry = self->m_studio->characters()->entry(id)) {
            self->m_studio->setActiveCharacter(id);
            self->m_studio->notify(tr("%1 is the character of this image and now the project's character.").arg(entry->name));
        }
    };
    // Images since M14 name their character; older Create images name their director session.
    const QString id = info->metadata.value(QStringLiteral("character")).toObject().value(QStringLiteral("character")).toString();
    if (!id.isEmpty())
        return adopt(id);
    const QString session = info->metadata.value(QStringLiteral("director_session")).toString();
    if (session.isEmpty())
        return;
    project->records(QStringLiteral("director")).then(this, [adopt, session](const QList<doc::Record> &records) {
        for (const doc::Record &r : records) {
            if (r.id == session)
                adopt(r.json.value(QStringLiteral("library_character")).toString());
        }
    });
}

void PartsPage::generateExpression(const QString &kind)
{
    if (!m_set || kind.isEmpty())
        return;
    const ai::parts::ExpressionKind *k = ai::parts::findExpression(kind);
    if (!k)
        return;
    // Selected regular parts define the area; otherwise the parts matching the expression.
    QList<int> base;
    for (int label : m_list->selectedLabels()) {
        const doc::Part *p = m_set->part(label);
        if (p && p->pixels > 0 && !p->provenance.startsWith(QLatin1String("variant:")))
            base.append(label);
    }
    ai::ExpressionOptions options;
    options.kind = kind;
    options.providerId = m_provider->providerId();
    options.characterSummary = m_character->text().trimmed();
    QPointer<PartsPage> self(this);
    QPointer<doc::PartSet> set(m_set);
    withCharacter(options.providerId, [self, set, base, options](const ai::CharacterContext &character, const QString &family) mutable {
        if (!self || !set || set != self->m_set)
            return;
        options.character = character;
        options.modelFamily = family;
        if (options.characterSummary.isEmpty())
            options.characterSummary = ai::Director::appearanceTags(character.sheet);
        Job *job = self->m_studio->parts()->generateExpression(set, base, options);
        self->m_running.append(job);
        connect(job, &Job::finished, self, [self, set, options](Job *j) {
            if (!self)
                return;
            if (j->state() == Job::State::Failed)
                self->m_studio->notify(j->errorString(), QStringLiteral("error"));
            if (j->state() != Job::State::Succeeded || !set || !self->m_checkExpressions->isChecked())
                return;
            const ai::parts::ExpressionKind *kind = ai::parts::findExpression(options.kind);
            for (const doc::Part &p : set->parts()) {
                if (kind && p.tag == kind->tag)
                    checkExpressionVariant(self->m_studio, set, p.label, options.character, options.characterSummary, kind->prompt);
            }
        });
    });
}

void PartsPage::cancelGeneration()
{
    for (const QPointer<Job> &j : std::as_const(m_running)) {
        if (j && !j->isFinished())
            j->cancel();
    }
    m_running.clear();
}

void PartsPage::rebuildCards()
{
    QList<const doc::Part *> order = m_set ? m_set->partsInDrawOrder() : QList<const doc::Part *>();
    std::reverse(order.begin(), order.end());
    QList<int> labels;
    for (const doc::Part *p : order)
        labels.append(p->label);
    // Mask edits change parts often; only rebuild when the list itself changed.
    if (labels == m_cardOrder && m_cardWidgets.size() == labels.size()) {
        for (int label : labels)
            updateCard(label);
        return;
    }
    m_cardOrder = labels;
    for (auto it = m_cardWidgets.begin(); it != m_cardWidgets.end(); ++it) {
        if (it.value())
            it.value()->deleteLater();
    }
    m_cardWidgets.clear();
    if (!m_set)
        return;
    int index = 0;
    for (const doc::Part *p : order) {
        auto *card = new PartCard(m_cards->parentWidget());
        const int label = p->label;
        card->onSelect = [this, label] { m_context->setActiveLabel(label); };
        card->onGenerate = [this, label] { generate({label}); };
        card->onEdit = [this, label] {
            m_context->setActiveLabel(label);
            setStep(MasksStep);
            selectTool(QStringLiteral("part-brush"));
        };
        card->onVariant = [this, label](int delta) {
            if (const doc::Part *p = m_set ? m_set->part(label) : nullptr) {
                m_set->chooseVariant(label, p->chosen + delta);
                m_studio->parts()->loadResults(m_set);
            }
        };
        card->onApprove = [this, label](bool on) {
            if (const doc::Part *p = m_set ? m_set->part(label) : nullptr; p && p->hasResult())
                m_set->setPartState(label, on ? doc::PartState::Approved : doc::PartState::Ready);
        };
        card->onRemove = [this, label] {
            if (const doc::Part *p = m_set ? m_set->part(label) : nullptr; p && p->hasResult())
                m_set->removeVariant(label, p->chosen);
        };
        m_cards->insertWidget(index++, card);
        m_cardWidgets.insert(label, card);
        updateCard(label);
    }
}

void PartsPage::updateCard(int label)
{
    PartCard *card = m_cardWidgets.value(label);
    const doc::Part *p = m_set ? m_set->part(label) : nullptr;
    if (!card || !p)
        return;
    card->refresh(*p, m_set->resultImage(label), label == m_context->activeLabel());
}

// ---- Preview & export ------------------------------------------------------------------------------------------

void PartsPage::previewCubismModel(const QString &model3Path)
{
    m_holdPreview = true;
    setStep(ExportStep);
    m_previewTabs->setCurrentIndex(1);
    showPreviewWidget();
    if (!model3Path.isEmpty() && live2d::CubismCore::instance().isLoaded())
        m_cubismPanel->openModel(model3Path);
}

void PartsPage::showPreviewWidget()
{
    if (m_panels->currentIndex() != ExportStep)
        return;
    const bool cubism = m_previewTabs && m_previewTabs->currentIndex() == 1;
    m_center->setCurrentWidget(cubism ? static_cast<QWidget *>(m_cubism) : m_preview);
    if (cubism)
        m_cubismPanel->activate();
}

void PartsPage::refreshPreview()
{
    if (!m_set || !m_set->hasSource()) {
        m_preview->setLayers({}, QSize());
        return;
    }
    const doc::PartSet::Snapshot snap = m_set->snapshot();
    const QImage source = m_set->source();
    QHash<QString, QImage> results;
    for (const doc::Part &p : snap.parts) {
        if (p.hasResult() && m_set->hasResultImage(p.chosenAssetId()))
            results.insert(p.chosenAssetId(), m_set->resultImage(p.label));
    }
    QPointer<PartsPage> self(this);
    async::run([snap, source, results] { return ai::parts::exportLayers(snap, source, results, true); })
        .then(this, [self, size = source.size()](QList<ai::parts::ExportLayer> layers) {
            if (!self)
                return;
            QList<PuppetLayer> puppet;
            for (const ai::parts::ExportLayer &l : layers)
                puppet.append(PuppetLayer{l.name, l.tag, l.group, l.side, l.image, l.rect, l.depth, l.visible});
            self->m_preview->setLayers(puppet, size);
        });
}

void PartsPage::exportPsd()
{
    if (!m_set || m_set->parts().isEmpty())
        return;
    const QString dir = Settings::instance().value(QStringLiteral("parts/exportDir"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString name = m_set->name().isEmpty() ? tr("character") : m_set->name();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export layered PSD"), dir + u'/' + name + QStringLiteral(".psd"),
                                                      tr("Photoshop document (*.psd)"));
    if (path.isEmpty())
        return;
    Settings::instance().setValue(QStringLiteral("parts/exportDir"), QFileInfo(path).absolutePath());
    ai::ExportOptions options;
    options.includeCutOuts = m_includeCutOuts->isChecked();
    options.includeSource = m_includeOriginal->isChecked();
    Job *job = m_studio->parts()->exportPsd(m_set, path, options);
    QPointer<PartsPage> self(this);
    connect(job, &Job::finished, this, [self, path](Job *j) {
        if (!self)
            return;
        if (j->state() == Job::State::Succeeded)
            self->m_studio->notify(tr("Exported %1").arg(QFileInfo(path).fileName()), QStringLiteral("success"));
        else if (j->state() == Job::State::Failed)
            self->m_studio->notify(j->errorString(), QStringLiteral("error"));
    });
}

void PartsPage::exportPngs()
{
    if (!m_set || m_set->parts().isEmpty())
        return;
    const QString base = Settings::instance().value(QStringLiteral("parts/exportDir"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export part PNGs into folder"), base);
    if (dir.isEmpty())
        return;
    Settings::instance().setValue(QStringLiteral("parts/exportDir"), dir);
    ai::ExportOptions options;
    options.includeCutOuts = m_includeCutOuts->isChecked();
    Job *job = m_studio->parts()->exportPngFolder(m_set, dir, options);
    QPointer<PartsPage> self(this);
    connect(job, &Job::finished, this, [self, dir](Job *j) {
        if (!self)
            return;
        if (j->state() == Job::State::Succeeded)
            self->m_studio->notify(tr("Exported parts to %1").arg(QDir::toNativeSeparators(dir)), QStringLiteral("success"));
        else if (j->state() == Job::State::Failed)
            self->m_studio->notify(j->errorString(), QStringLiteral("error"));
    });
}

void PartsPage::openInEditor()
{
    if (!m_set || !m_set->hasSource())
        return;
    const doc::PartSet::Snapshot snap = m_set->snapshot();
    const QImage source = m_set->source();
    QHash<QString, QImage> results;
    for (const doc::Part &p : snap.parts) {
        if (p.hasResult() && m_set->hasResultImage(p.chosenAssetId()))
            results.insert(p.chosenAssetId(), m_set->resultImage(p.label));
    }
    const bool cutOuts = m_includeCutOuts->isChecked();
    const QString name = m_set->name().isEmpty() ? tr("Parts") : tr("%1 (parts)").arg(m_set->name());
    QPointer<PartsPage> self(this);
    async::run([snap, source, results, cutOuts] { return ai::parts::exportLayers(snap, source, results, cutOuts); })
        .then(this, [self, source, name](QList<ai::parts::ExportLayer> layers) {
            if (!self)
                return;
            doc::Document *d = self->m_studio->openImageAsDocument(source, name);
            if (!d)
                return;
            if (const doc::LayerPtr original = d->layerAt(0)) {
                doc::LayerProps props = original->props;
                props.name = tr("Original");
                props.visible = false;
                d->setLayerPropsRaw(original->id, props);
            }
            for (const ai::parts::ExportLayer &l : layers) {
                QImage full(source.size(), QImage::Format_ARGB32_Premultiplied);
                full.fill(Qt::transparent);
                QPainter p(&full);
                p.drawImage(l.rect.topLeft(), l.image);
                p.end();
                d->addLayer(l.name, full, QJsonObject{{QStringLiteral("part_tag"), l.tag}}, d->layerCount());
            }
            d->undoStack()->clear();
        });
}

// ---- Canvas display ----------------------------------------------------------------------------------------------

void PartsPage::rebuildDisplay()
{
    m_canvas->setDocument(nullptr);
    delete m_display;
    m_display = nullptr;
    if (!m_set || !m_set->hasSource()) {
        m_canvas->updateAll();
        return;
    }
    const QSize size = m_set->size();
    m_display = new doc::Document(size);
    m_display->setName(m_set->name());
    m_sourceLayer = m_display->addLayer(tr("Source"), m_set->source());
    m_resultLayer = m_display->addLayer(tr("Result"));
    m_overlayLayer = m_display->addLayer(tr("Parts"));
    m_extLayer = m_display->addLayer(tr("Hidden area"));
    m_display->undoStack()->clear();
    m_canvas->setDocument(m_display);
    updateOverlay(QRect(QPoint(0, 0), size));
    applyOverlayMode();
    scheduleAssembled();
    scheduleExtensionOverlay();
}

QVector<QRgb> PartsPage::overlayPalette() const
{
    QVector<QRgb> palette(256, 0);
    if (!m_set)
        return palette;
    const int active = m_context->activeLabel();
    for (const doc::Part &p : m_set->parts()) {
        if (!p.visible || (m_solo && p.label != m_solo))
            continue;
        const int a = (active == 0 || p.label == active) ? 255 : 150;
        palette[p.label] = qRgba(p.color.red() * a / 255, p.color.green() * a / 255, p.color.blue() * a / 255, a);
    }
    return palette;
}

void PartsPage::updateOverlay(const QRect &rectIn)
{
    if (!m_display || !m_set || m_set->labels().isNull())
        return;
    const QRect rect = rectIn.intersected(m_set->labels().rect());
    if (rect.isEmpty())
        return;
    const QImage &labels = m_set->labels();
    QImage overlay = img::colorizeLabels(labels, overlayPalette(), rect);
    const int active = m_context->activeLabel();
    // Unassigned character pixels (optional highlight).
    if (m_showUnassigned && !m_set->silhouette().isNull()) {
        const QImage &sil = m_set->silhouette();
        const QRgb hi = qRgba(255, 0, 160, 255);
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            const uchar *l = labels.constScanLine(y);
            const uchar *s = sil.constScanLine(y);
            QRgb *dst = reinterpret_cast<QRgb *>(overlay.scanLine(y - rect.top()));
            for (int x = rect.left(); x <= rect.right(); ++x) {
                if (!l[x] && s[x] >= 128 && ((x + y) / 4) % 2 == 0)
                    dst[x - rect.left()] = hi;
            }
        }
    }
    // Part boundaries, brighter around the selected part.
    const QRect grown = rect.adjusted(-1, -1, 1, 1).intersected(labels.rect());
    QImage edges = img::labelEdges(labels, grown, qRgba(20, 20, 24, 230));
    if (active) {
        for (int y = grown.top(); y <= grown.bottom(); ++y) {
            const uchar *l = labels.constScanLine(y);
            QRgb *e = reinterpret_cast<QRgb *>(edges.scanLine(y - grown.top()));
            for (int x = grown.left(); x <= grown.right(); ++x) {
                if (l[x] == active && qAlpha(e[x - grown.left()]))
                    e[x - grown.left()] = qRgba(255, 255, 255, 255);
            }
        }
    }
    QPainter p(&overlay);
    p.drawImage(grown.topLeft() - rect.topLeft(), edges);
    p.end();
    m_display->writePixelsRaw(m_overlayLayer, rect, overlay);
}

void PartsPage::scheduleExtensionOverlay()
{
    m_extensionTimer.start();
}

void PartsPage::updateExtensionOverlay()
{
    if (!m_display || !m_set)
        return;
    const int label = m_context->activeLabel();
    const bool show = label && m_set->part(label) && m_panels->currentIndex() != SourceStep
                      && (m_canvas->tool() && (m_canvas->tool()->id() == QLatin1String("part-extend") || m_overlayMode->currentIndex() == 0));
    const quint64 generation = ++m_extensionGeneration;
    if (!show) {
        QImage clear(m_set->size(), QImage::Format_ARGB32_Premultiplied);
        clear.fill(Qt::transparent);
        m_display->writePixelsRaw(m_extLayer, clear.rect(), clear);
        return;
    }
    const doc::PartSet::Snapshot snap = m_set->snapshot();
    const QImage sil = m_set->silhouette();
    const QColor color = m_set->part(label)->color;
    QPointer<PartsPage> self(this);
    async::run([snap, sil, label, color] {
        const QImage ext = doc::PartSet::extensionMaskFor(snap, sil, label);
        const QImage own = img::maskForLabel(snap.labels, label);
        QImage out(ext.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        const QRgb hatch = qRgba(color.red(), color.green(), color.blue(), 255);
        for (int y = 0; y < ext.height(); ++y) {
            const uchar *e = ext.constScanLine(y);
            const uchar *o = own.constScanLine(y);
            QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < ext.width(); ++x) {
                if (e[x] >= 128 && o[x] < 128)
                    dst[x] = ((x + y) / 5) % 2 == 0 ? hatch : qRgba(0, 0, 0, 90);
            }
        }
        return out;
    }).then(this, [self, generation](QImage hatch) {
        if (!self || generation != self->m_extensionGeneration || !self->m_display)
            return;
        self->m_display->writePixelsRaw(self->m_extLayer, hatch.rect(), hatch);
    });
}

void PartsPage::paintExtensionPreview(const QImage &mask, const QRect &rectIn)
{
    if (!m_display || !m_set)
        return;
    ++m_extensionGeneration; // supersede pending async renders
    const QRect rect = rectIn.intersected(mask.rect());
    const int label = m_context->activeLabel();
    const doc::Part *part = m_set->part(label);
    if (!part || rect.isEmpty())
        return;
    const QRgb hatch = qRgba(part->color.red(), part->color.green(), part->color.blue(), 255);
    const QImage &labels = m_set->labels();
    QImage out(rect.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        const uchar *m = mask.constScanLine(y);
        const uchar *l = labels.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y - rect.top()));
        for (int x = rect.left(); x <= rect.right(); ++x) {
            if (m[x] >= 128 && l[x] != label)
                dst[x - rect.left()] = ((x + y) / 5) % 2 == 0 ? hatch : qRgba(0, 0, 0, 90);
        }
    }
    m_display->writePixelsRaw(m_extLayer, rect, out);
}

void PartsPage::scheduleAssembled()
{
    m_assembledTimer.start();
}

void PartsPage::updateAssembled()
{
    if (!m_display || !m_set)
        return;
    const doc::PartSet::Snapshot snap = m_set->snapshot();
    const QImage source = m_set->source();
    QHash<QString, QImage> results;
    for (const doc::Part &p : snap.parts) {
        if (p.hasResult() && m_set->hasResultImage(p.chosenAssetId()))
            results.insert(p.chosenAssetId(), m_set->resultImage(p.label));
    }
    const int solo = m_solo;
    const quint64 generation = ++m_assembledGeneration;
    QPointer<PartsPage> self(this);
    async::run([snap, source, results, solo] {
        QImage out(source.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter p(&out);
        for (const ai::parts::ExportLayer &l : ai::parts::exportLayers(snap, source, results, true)) {
            if (!l.visible)
                continue;
            if (solo) {
                bool match = false;
                for (const doc::Part &part : snap.parts)
                    match |= part.label == solo && part.displayName() == l.name;
                if (!match)
                    continue;
            }
            p.drawImage(l.rect.topLeft(), l.image);
        }
        p.end();
        return out;
    }).then(this, [self, generation](QImage assembled) {
        if (!self || generation != self->m_assembledGeneration || !self->m_display)
            return;
        self->m_display->writePixelsRaw(self->m_resultLayer, assembled.rect(), assembled);
        if (self->m_panels->currentIndex() == ExportStep)
            self->refreshPreview();
    });
}

void PartsPage::applyOverlayMode()
{
    if (!m_display)
        return;
    const int mode = m_overlayMode->currentIndex();
    auto setProps = [this](const QUuid &id, bool visible, double opacity) {
        const doc::LayerPtr layer = m_display->findLayer(id);
        if (!layer)
            return;
        doc::LayerProps props = layer->props;
        props.visible = visible;
        props.opacity = opacity;
        props.locked = true;
        m_display->setLayerPropsRaw(id, props);
    };
    const double opacity = m_overlayOpacity->value() / 100.0;
    // Result mode shows the assembled parts over a dimmed source.
    setProps(m_sourceLayer, true, mode == 1 ? 0.25 : 1.0);
    setProps(m_resultLayer, mode == 1, 1.0);
    setProps(m_overlayLayer, mode == 0, opacity);
    setProps(m_extLayer, mode == 0, std::min(1.0, opacity + 0.25));
    scheduleExtensionOverlay();
}

} // namespace lumen::ui
