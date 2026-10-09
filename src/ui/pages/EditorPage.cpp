#include "ui/pages/EditorPage.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/Psd.h"
#include "doc/PsdIO.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/Shortcuts.h"
#include "ui/canvas/CanvasView.h"
#include "ui/canvas/ContextBar.h"
#include "ui/canvas/EditorState.h"
#include "ui/canvas/Tools.h"
#include "ui/canvas/TransformTool.h"
#include "ui/pages/AiPanel.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/LayersPanel.h"
#include "ui/widgets/ReferenceStrip.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QSpinBox>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QUndoView>
#include <QVBoxLayout>

namespace lumen::ui {

EditorPage::EditorPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_state(new EditorState(this))
{
    setAcceptDrops(true);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Document tabs.
    auto *tabRow = new QWidget(this);
    tabRow->setObjectName(QStringLiteral("Panel"));
    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(8, 6, 8, 0);
    tabLayout->setSpacing(4);
    m_tabs = new QTabBar(tabRow);
    m_tabs->setProperty("role", QStringLiteral("documents"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(false);
    m_tabs->setExpanding(false);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    tabLayout->addWidget(m_tabs);
    auto *newDoc = iconButton(QStringLiteral("plus"), tr("New document (Ctrl+N)"), tabRow);
    tabLayout->addWidget(newDoc);
    tabLayout->addStretch();
    root->addWidget(tabRow);

    // Tool options bar.
    auto *optionsBar = new QWidget(this);
    optionsBar->setObjectName(QStringLiteral("Panel"));
    optionsBar->setFixedHeight(46);
    auto *ob = new QHBoxLayout(optionsBar);
    ob->setContentsMargins(12, 4, 12, 4);
    m_optionsHost = new QHBoxLayout;
    ob->addLayout(m_optionsHost, 1);
    auto *undo = iconButton(QStringLiteral("undo"), tr("Undo (Ctrl+Z)"), optionsBar);
    auto *redo = iconButton(QStringLiteral("redo"), tr("Redo (Ctrl+Shift+Z)"), optionsBar);
    ob->addWidget(separator(Qt::Vertical, optionsBar));
    ob->addWidget(undo);
    ob->addWidget(redo);
    root->addWidget(optionsBar);
    root->addWidget(separator(Qt::Horizontal, this));

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    root->addWidget(splitter, 1);

    // Tool palette.
    auto *palette = new QWidget(splitter);
    palette->setObjectName(QStringLiteral("Panel"));
    palette->setFixedWidth(52);
    auto *pv = new QVBoxLayout(palette);
    pv->setContentsMargins(6, 8, 6, 8);
    pv->setSpacing(2);
    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);
    m_tools = {new MoveTool(m_state, this),
               new TransformTool(m_state, this),
               new SelectionTool(m_state, SelectionTool::Shape::Rect, this),
               new SelectionTool(m_state, SelectionTool::Shape::Ellipse, this),
               new SelectionTool(m_state, SelectionTool::Shape::Lasso, this),
               new MaskBrushTool(m_state, this),
               new BrushTool(m_state, false, this),
               new BrushTool(m_state, true, this),
               new FillTool(m_state, this),
               new EyedropperTool(m_state, this),
               new CropTool(m_state, this),
               new HandTool(m_state, this)};
    for (Tool *tool : std::as_const(m_tools)) {
        auto *action = new QAction(Icons::get(tool->iconName()), tool->name(), this);
        action->setCheckable(true);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        Shortcuts::instance().bindAction(action, QStringLiteral("editor.tool.") + tool->id(), tr("Editor tools"), QStringLiteral("editor"),
                                         {QKeySequence(tool->shortcut())}, tool->name(), true);
        m_toolGroup->addAction(action);
        addAction(action);
        auto *button = new QToolButton(palette);
        button->setDefaultAction(action);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(40, 36);
        pv->addWidget(button);
        m_toolActions.insert(tool->id(), action);
        connect(action, &QAction::triggered, this, [this, id = tool->id()] { selectTool(id); });
        connect(tool, &Tool::message, this, [this](const QString &msg) { m_studio->notify(msg, QStringLiteral("warning")); });
    }
    pv->addStretch();
    m_fg = new ColorSwatch(m_state->foreground(), palette);
    m_bg = new ColorSwatch(m_state->background(), palette);
    m_fg->setToolTip(tr("Foreground colour"));
    m_bg->setToolTip(tr("Background colour"));
    auto *swap = iconButton(QStringLiteral("refresh"), tr("Swap colours (X)"), palette);
    swap->setIconSize(QSize(14, 14));
    pv->addWidget(m_fg, 0, Qt::AlignHCenter);
    pv->addWidget(m_bg, 0, Qt::AlignHCenter);
    pv->addWidget(swap, 0, Qt::AlignHCenter);
    connect(m_fg, &ColorSwatch::colorChanged, m_state, &EditorState::setForeground);
    connect(m_bg, &ColorSwatch::colorChanged, m_state, &EditorState::setBackground);
    connect(swap, &QToolButton::clicked, m_state, &EditorState::swapColors);
    connect(m_state, &EditorState::colorsChanged, this, [this] {
        m_fg->setColor(m_state->foreground());
        m_bg->setColor(m_state->background());
    });

    // Canvas column.
    auto *center = new QWidget(splitter);
    auto *cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    m_canvas = new CanvasView(m_state, center);
    cv->addWidget(m_canvas, 1);
    auto *status = new QWidget(center);
    status->setObjectName(QStringLiteral("Panel"));
    auto *sh = new QHBoxLayout(status);
    sh->setContentsMargins(10, 3, 10, 3);
    m_infoLabel = mutedLabel(QString(), status);
    m_infoLabel->setWordWrap(false);
    m_hintLabel = mutedLabel(QString(), status);
    m_hintLabel->setWordWrap(false);
    auto *zoomOut = iconButton(QStringLiteral("zoom-out"), tr("Zoom out (Ctrl+-)"), status);
    auto *zoomIn = iconButton(QStringLiteral("zoom-in"), tr("Zoom in (Ctrl+=)"), status);
    auto *fit = iconButton(QStringLiteral("fit"), tr("Fit to window (Ctrl+0)"), status);
    m_zoomLabel = new QLabel(QStringLiteral("100%"), status);
    m_zoomLabel->setMinimumWidth(48);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    sh->addWidget(m_infoLabel);
    sh->addSpacing(16);
    sh->addWidget(m_hintLabel, 1);
    sh->addWidget(zoomOut);
    sh->addWidget(m_zoomLabel);
    sh->addWidget(zoomIn);
    sh->addWidget(fit);
    cv->addWidget(status);

    // Side panel.
    m_side = new QTabWidget(splitter);
    m_side->setMinimumWidth(300);
    m_side->setDocumentMode(true);
    m_ai = new AiPanel(studio, m_side);
    m_layers = new LayersPanel(m_side);
    m_history = new QUndoView(m_side);
    m_history->setEmptyLabel(tr("Original"));
    m_side->addTab(m_ai, Icons::get(QStringLiteral("sparkles")), tr("AI"));
    m_side->addTab(m_layers, Icons::get(QStringLiteral("layers")), tr("Layers"));
    m_side->addTab(m_history, Icons::get(QStringLiteral("history")), tr("History"));

    splitter->addWidget(palette);
    splitter->addWidget(center);
    splitter->addWidget(m_side);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({52, 900, 340});

    // Wiring.
    connect(newDoc, &QToolButton::clicked, this, &EditorPage::newDocumentDialog);
    connect(undo, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->undoStack()->undo();
    });
    connect(redo, &QToolButton::clicked, this, [this] {
        if (m_doc)
            m_doc->undoStack()->redo();
    });
    // Contextual generative toolbar next to the selection.
    m_contextBar = new ContextBar(m_canvas);
    m_contextBar->setEnabledByUser(m_state->showContextBar());
    connect(m_contextBar, &ContextBar::generateRequested, this, [this](const QString &prompt) {
        m_ai->runFromCanvas(AiPanel::Mode::Inpaint, prompt);
    });
    connect(m_contextBar, &ContextBar::removeRequested, this, [this] {
        m_ai->runFromCanvas(AiPanel::Mode::Inpaint, QStringLiteral("empty background, seamless continuation of the surroundings"));
    });
    connect(m_contextBar, &ContextBar::moreRequested, this, [this] { m_side->setCurrentWidget(m_ai); });
    connect(m_ai, &AiPanel::runningJobsChanged, m_contextBar, &ContextBar::setRunning);
    connect(zoomIn, &QToolButton::clicked, m_canvas, &CanvasView::zoomIn);
    connect(zoomOut, &QToolButton::clicked, m_canvas, &CanvasView::zoomOut);
    connect(fit, &QToolButton::clicked, m_canvas, &CanvasView::fitToView);
    connect(m_canvas, &CanvasView::zoomChanged, this, [this](double z) { m_zoomLabel->setText(QStringLiteral("%1%").arg(std::lround(z * 100))); });
    connect(m_canvas, &CanvasView::cursorMoved, this, [this](const QPointF &p) {
        if (m_doc)
            m_infoLabel->setText(QStringLiteral("%1 × %2 px   ·   %3, %4").arg(m_doc->size().width()).arg(m_doc->size().height())
                                     .arg(int(std::floor(p.x()))).arg(int(std::floor(p.y()))));
    });
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_rebuildingTabs || index < 0 || index >= m_studio->documents().size())
            return;
        m_studio->setActiveDocument(m_studio->documents().at(index));
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index >= 0 && index < m_studio->documents().size())
            m_studio->closeDocument(m_studio->documents().at(index));
    });
    connect(studio, &Studio::documentsChanged, this, &EditorPage::rebuildTabs);
    connect(studio, &Studio::activeDocumentChanged, this, &EditorPage::setDocument);

    // Shortcuts.
    // Page shortcuts (customisable in Settings → Shortcuts).
    auto shortcut = [this](const QString &id, const QString &name, const QStringList &keys, std::function<void()> fn) {
        auto *a = new QAction(this);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(a, &QAction::triggered, this, std::move(fn));
        addAction(a);
        QList<QKeySequence> defaults;
        for (const QString &k : keys)
            defaults << QKeySequence(k);
        Shortcuts::instance().bindAction(a, QStringLiteral("editor.") + id, tr("Editor"), QStringLiteral("editor"), defaults, name);
    };
    shortcut(QStringLiteral("undo"), tr("Undo"), {QStringLiteral("Ctrl+Z")}, [this] { if (m_doc) m_doc->undoStack()->undo(); });
    shortcut(QStringLiteral("redo"), tr("Redo"), {QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Ctrl+Y")},
             [this] { if (m_doc) m_doc->undoStack()->redo(); });
    shortcut(QStringLiteral("deselect"), tr("Deselect"), {QStringLiteral("Ctrl+D")}, [this] { if (m_doc) m_doc->clearSelection(); });
    shortcut(QStringLiteral("select-all"), tr("Select all"), {QStringLiteral("Ctrl+A")}, [this] { if (m_doc) m_doc->selectAll(); });
    shortcut(QStringLiteral("invert-selection"), tr("Invert selection"), {QStringLiteral("Ctrl+Shift+I")},
             [this] { if (m_doc) m_doc->invertSelection(); });
    shortcut(QStringLiteral("delete-pixels"), tr("Delete selected pixels"), {QStringLiteral("Del")}, [this] { deleteSelectionPixels(); });
    shortcut(QStringLiteral("fit"), tr("Fit to window"), {QStringLiteral("Ctrl+0")}, [this] { m_canvas->fitToView(); });
    shortcut(QStringLiteral("actual-pixels"), tr("Actual pixels (100 %)"), {QStringLiteral("Ctrl+1")}, [this] { m_canvas->actualPixels(); });
    shortcut(QStringLiteral("zoom-in"), tr("Zoom in"), {QStringLiteral("Ctrl+="), QStringLiteral("Ctrl++")}, [this] { m_canvas->zoomIn(); });
    shortcut(QStringLiteral("zoom-out"), tr("Zoom out"), {QStringLiteral("Ctrl+-")}, [this] { m_canvas->zoomOut(); });
    shortcut(QStringLiteral("swap-colors"), tr("Swap colours"), {QStringLiteral("X")}, [this] { m_state->swapColors(); });
    shortcut(QStringLiteral("paste-layer"), tr("Paste as layer"), {QStringLiteral("Ctrl+V")}, [this] { pasteAsLayer(); });
    shortcut(QStringLiteral("new-layer"), tr("New layer"), {QStringLiteral("Ctrl+Shift+N")},
             [this] { if (m_doc) m_doc->addLayer(tr("Layer %1").arg(m_doc->layerCount() + 1)); });
    shortcut(QStringLiteral("duplicate-layer"), tr("Duplicate layer"), {QStringLiteral("Ctrl+J")},
             [this] { if (m_doc) m_doc->duplicateLayer(m_doc->activeLayerId()); });
    shortcut(QStringLiteral("merge-down"), tr("Merge down"), {QStringLiteral("Ctrl+E")},
             [this] { if (m_doc) m_doc->mergeDown(m_doc->activeLayerId()); });

    selectTool(QStringLiteral("mask"));
    rebuildTabs();
    setDocument(studio->activeDocument());
}

void EditorPage::selectTool(const QString &id)
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
        auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(tool->name()));
        m_options = new QWidget;
        auto *h = new QHBoxLayout(m_options);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(16);
        h->addWidget(title);
        if (QWidget *opts = tool->createOptions(m_options))
            h->addWidget(opts, 1);
        else
            h->addStretch();
        m_optionsHost->addWidget(m_options);
        m_hintLabel->setText(tool->hint());
        if (id == QLatin1String("mask") || id.startsWith(QLatin1String("select")) || id == QLatin1String("lasso"))
            m_side->setCurrentWidget(m_ai);
        return;
    }
}

void EditorPage::setDocument(doc::Document *doc)
{
    m_doc = doc;
    m_canvas->setDocument(doc);
    m_layers->setDocument(doc);
    m_ai->setDocument(doc);
    m_history->setStack(doc ? doc->undoStack() : nullptr);
    const int idx = int(m_studio->documents().indexOf(doc));
    if (idx >= 0 && m_tabs->currentIndex() != idx) {
        m_rebuildingTabs = true;
        m_tabs->setCurrentIndex(idx);
        m_rebuildingTabs = false;
    }
    updateStatus();
}

void EditorPage::rebuildTabs()
{
    m_rebuildingTabs = true;
    while (m_tabs->count() > 0)
        m_tabs->removeTab(0);
    for (doc::Document *d : m_studio->documents()) {
        const int i = m_tabs->addTab(d->name().isEmpty() ? tr("Untitled") : d->name());
        m_tabs->setTabToolTip(i, QStringLiteral("%1 × %2").arg(d->size().width()).arg(d->size().height()));
        connect(d, &doc::Document::nameChanged, this, &EditorPage::rebuildTabs, Qt::UniqueConnection);
    }
    const int idx = int(m_studio->documents().indexOf(m_studio->activeDocument()));
    m_tabs->setCurrentIndex(idx);
    m_tabs->setVisible(m_tabs->count() > 0);
    m_rebuildingTabs = false;
}

void EditorPage::updateStatus()
{
    if (m_doc)
        m_infoLabel->setText(QStringLiteral("%1 × %2 px").arg(m_doc->size().width()).arg(m_doc->size().height()));
    else
        m_infoLabel->setText(tr("No document"));
}

void EditorPage::deleteSelectionPixels()
{
    if (!m_doc || !m_doc->hasSelection())
        return;
    const doc::LayerPtr layer = m_doc->activeLayer();
    const QRect bounds = m_doc->selectionBounds();
    if (!layer || bounds.isEmpty() || layer->props.locked)
        return;
    QImage pixels = layer->image.copy(bounds);
    const QImage mask = m_doc->selection().copy(bounds);
    for (int y = 0; y < pixels.height(); ++y) {
        auto *px = reinterpret_cast<QRgb *>(pixels.scanLine(y));
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            const int keep = 255 - m[x];
            const QRgb p = px[x];
            px[x] = qRgba(qRed(p) * keep / 255, qGreen(p) * keep / 255, qBlue(p) * keep / 255, qAlpha(p) * keep / 255);
        }
    }
    m_doc->applyPixels(layer->id, bounds, pixels, tr("Delete pixels"));
}

void EditorPage::newDocumentDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("New document"));
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit(tr("Untitled"), &dlg);
    auto *preset = new QComboBox(&dlg);
    const QList<QPair<QString, QSize>> presets = {
        {tr("Square 1024"), QSize(1024, 1024)},   {tr("Portrait 832 × 1216 (2:3)"), QSize(832, 1216)},
        {tr("Landscape 1216 × 832 (3:2)"), QSize(1216, 832)}, {tr("Live2D sheet 2048 × 3072"), QSize(2048, 3072)},
        {tr("Full HD 1920 × 1080"), QSize(1920, 1080)}, {tr("4K 3840 × 2160"), QSize(3840, 2160)},
    };
    for (const auto &p : presets)
        preset->addItem(p.first, p.second);
    auto *w = new QSpinBox(&dlg);
    auto *h = new QSpinBox(&dlg);
    for (QSpinBox *s : {w, h}) {
        s->setRange(16, 16384);
        s->setSuffix(QStringLiteral(" px"));
    }
    w->setValue(1024);
    h->setValue(1024);
    connect(preset, &QComboBox::currentIndexChanged, &dlg, [preset, w, h] {
        const QSize s = preset->currentData().toSize();
        w->setValue(s.width());
        h->setValue(s.height());
    });
    auto *bg = new QComboBox(&dlg);
    bg->addItem(tr("White"), QColor(Qt::white));
    bg->addItem(tr("Black"), QColor(Qt::black));
    bg->addItem(tr("Transparent"), QColor(Qt::transparent));
    bg->addItem(tr("Grey"), QColor(0x808080));
    form->addRow(tr("Name"), name);
    form->addRow(tr("Preset"), preset);
    form->addRow(tr("Width"), w);
    form->addRow(tr("Height"), h);
    form->addRow(tr("Background"), bg);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted)
        m_studio->newDocument(QSize(w->value(), h->value()), name->text(), bg->currentData().value<QColor>());
}

void EditorPage::exportImage()
{
    if (!m_doc)
        return;
    QString filter;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export image"), m_doc->name() + QStringLiteral(".png"),
                                                      tr("PNG (*.png);;JPEG (*.jpg *.jpeg);;WebP (*.webp);;Layered Photoshop (*.psd)"), &filter);
    if (path.isEmpty())
        return;
    const doc::DocumentSnapshot snap = m_doc->snapshot();
    const QSize size = m_doc->size();
    const QString suffix = QFileInfo(path).suffix().toLower();
    async::run([snap, size, path, suffix] {
        if (suffix == QLatin1String("psd")) {
            QString error;
            if (!psd::write(path, doc::exportPsd(snap, size), &error))
                throw Error(error);
            return;
        }
        const QImage img = doc::flatten(snap, size);
        const QByteArray fmt = suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") ? QByteArray("JPEG") : suffix.toUpper().toLatin1();
        const QByteArray bytes = img::encode(img, fmt.isEmpty() ? "PNG" : fmt.constData(), fmt == "PNG" ? 6 : 95);
        if (bytes.isEmpty())
            throw Error(QObject::tr("This format is not supported (install the Qt Image Formats plugin for WebP)."));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
            throw Error(f.errorString());
    }).then(this, [this, path] { m_studio->notify(tr("Exported %1").arg(QFileInfo(path).fileName()), QStringLiteral("success")); })
      .onFailed(this, [this](const std::exception &e) { m_studio->notify(describeException(e), QStringLiteral("error")); });
}

void EditorPage::saveToLibrary()
{
    if (!m_doc || !m_studio->project())
        return;
    const doc::DocumentSnapshot snap = m_doc->snapshot();
    const QSize size = m_doc->size();
    const QString name = m_doc->name();
    QPointer<doc::Project> project = m_studio->project();
    async::run([snap, size] { return doc::flatten(snap, size); }).then(this, [this, project, name](QImage img) {
        if (project)
            project->addAsset(img, doc::AssetKind::Export, name).then(this, [this](doc::AssetInfo) {
                m_studio->notify(tr("Saved to the library."), QStringLiteral("success"));
            });
    });
}

void EditorPage::copyMerged()
{
    if (!m_doc)
        return;
    const QRect r = m_doc->hasSelection() ? m_doc->selectionBounds() : m_doc->bounds();
    if (r.isEmpty())
        return;
    QApplication::clipboard()->setImage(m_doc->flattened(r));
    m_studio->notify(tr("Copied %1 × %2 px").arg(r.width()).arg(r.height()));
}

void EditorPage::pasteAsLayer()
{
    const QImage img = QApplication::clipboard()->image();
    if (img.isNull())
        return;
    if (!m_doc) {
        m_studio->openImageAsDocument(img, tr("Pasted image"));
        return;
    }
    QImage placed(m_doc->size(), QImage::Format_ARGB32_Premultiplied);
    placed.fill(Qt::transparent);
    QPainter p(&placed);
    const QImage scaled = img.width() > m_doc->size().width() || img.height() > m_doc->size().height()
                              ? img.scaled(m_doc->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              : img;
    p.drawImage((m_doc->size().width() - scaled.width()) / 2, (m_doc->size().height() - scaled.height()) / 2, scaled);
    p.end();
    m_doc->addLayer(tr("Pasted"), placed, {}, -1, tr("Paste"));
}

void EditorPage::importAsLayer()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Import image as layer"), QString(),
                                                      tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    if (file.isEmpty())
        return;
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        m_studio->notify(tr("Cannot read %1: %2").arg(QFileInfo(file).fileName(), reader.errorString()), QStringLiteral("error"));
        return;
    }
    if (!m_doc) {
        m_studio->openImageAsDocument(img, QFileInfo(file).completeBaseName());
        return;
    }
    QApplication::clipboard()->setImage(img);
    pasteAsLayer();
}

void EditorPage::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasImage() || e->mimeData()->hasFormat(QString::fromLatin1(kAssetMime)))
        e->acceptProposedAction();
}

void EditorPage::dropEvent(QDropEvent *e)
{
    const QMimeData *m = e->mimeData();
    if (m->hasFormat(QString::fromLatin1(kAssetMime))) {
        for (const QByteArray &id : m->data(QString::fromLatin1(kAssetMime)).split('\n')) {
            if (!id.isEmpty())
                m_studio->openAssetInEditor(QString::fromUtf8(id));
        }
    } else if (m->hasUrls()) {
        for (const QUrl &url : m->urls()) {
            if (!url.isLocalFile())
                continue;
            m_studio->openFile(url.toLocalFile());
        }
    } else if (m->hasImage()) {
        m_studio->openImageAsDocument(qvariant_cast<QImage>(m->imageData()), tr("Dropped image"));
    }
    e->acceptProposedAction();
}

} // namespace lumen::ui
