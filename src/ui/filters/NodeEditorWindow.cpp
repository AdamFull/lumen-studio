#include "ui/filters/NodeEditorWindow.h"

#include "ai/FilterNodes.h"
#include "ai/GenerationService.h"
#include "core/Async.h"
#include "core/Error.h"
#include "filters/Registry.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/filters/FilterCatalog.h"
#include "ui/filters/GraphModel.h"
#include "ui/filters/NodeScene.h"
#include "ui/filters/ParamWidgets.h"
#include "ui/widgets/Controls.h"

#include <QAction>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

namespace {

constexpr int kPreviewSide = 480; // longest side the previews are computed at
constexpr int kPreviewHeight = 200;

QList<QPointer<NodeEditorWindow>> &windows()
{
    static QList<QPointer<NodeEditorWindow>> list;
    return list;
}

// The image on a checkerboard, so transparent parts read as such.
QPixmap previewPixmap(const QImage &image, const QSize &box)
{
    const QSize size = image.size().scaled(box, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    QPixmap pixmap(size);
    QPainter p(&pixmap);
    const int c = 8;
    for (int y = 0; y < size.height(); y += c) {
        for (int x = 0; x < size.width(); x += c)
            p.fillRect(x, y, c, c, ((x + y) / c) % 2 ? QColor(0x3a, 0x3d, 0x44) : QColor(0x2c, 0x2f, 0x35));
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRect(QPoint(0, 0), size), image);
    return pixmap;
}

QString valueText(const QVariant &v)
{
    if (v.typeId() == QMetaType::QColor)
        return v.value<QColor>().name(QColor::HexArgb);
    if (v.typeId() == QMetaType::Double)
        return QLocale().toString(v.toDouble(), 'g', 6);
    return v.toString();
}

} // namespace

NodeEditorWindow::NodeEditorWindow(FilterCatalog *catalog, QWidget *parent)
    : QMainWindow(parent, Qt::Window)
    , m_catalog(catalog)
    , m_model(new GraphModel(this))
    , m_evaluator(std::make_unique<filters::Evaluator>())
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("NodeEditorWindow"));
    windows().append(this);

    m_scene = new NodeScene(m_model, this);
    m_menu = new NodeMenu(filters::NodeRegistry::global(), this);

    // Toolbar.
    auto *bar = addToolBar(tr("Filter editor"));
    bar->setObjectName(QStringLiteral("NodeEditorToolbar"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->setIconSize(QSize(18, 18));
    QAction *add = bar->addAction(Icons::get(QStringLiteral("plus")), tr("Add node"));
    add->setShortcut(QKeySequence(QStringLiteral("Shift+A")));
    add->setToolTip(tr("Add a node (Shift+A, or right-click on an empty place)"));
    m_deleteAction = bar->addAction(Icons::get(QStringLiteral("trash")), tr("Delete"));
    m_deleteAction->setToolTip(tr("Delete the selected nodes and links (Del)"));
    m_deleteAction->setEnabled(false); // until something is selected
    QAction *frame = bar->addAction(Icons::get(QStringLiteral("fit")), tr("Show all"));
    frame->setToolTip(tr("Show every node (Home)"));
    frame->setShortcut(QKeySequence(Qt::Key_Home));
    bar->addSeparator();
    QAction *undo = m_model->undoStack()->createUndoAction(this, tr("Undo"));
    undo->setIcon(Icons::get(QStringLiteral("undo")));
    undo->setShortcut(QKeySequence::Undo);
    QAction *redo = m_model->undoStack()->createRedoAction(this, tr("Redo"));
    redo->setIcon(Icons::get(QStringLiteral("redo")));
    redo->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+Z")), QKeySequence(QStringLiteral("Ctrl+Y"))});
    // The actions' texts name the step ("Undo Connect"); the buttons stay short.
    for (QAction *a : {undo, redo}) {
        bar->addAction(a);
        if (auto *b = qobject_cast<QToolButton *>(bar->widgetForAction(a)))
            b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    bar->addSeparator();
    QAction *saveAction = bar->addAction(Icons::get(QStringLiteral("save")), tr("Save"));
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setToolTip(tr("Save into the filters folder (Ctrl+S)"));
    QAction *saveAsAction = bar->addAction(tr("Save as…"));
    saveAsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    saveAsAction->setToolTip(tr("Save as a new filter"));
    bar->addSeparator();
    m_runAction = bar->addAction(Icons::get(QStringLiteral("play")), tr("Run AI steps"));
    m_runAction->setToolTip(tr("AI steps take a while, so the preview runs them only when you ask"));

    // Canvas and side panel.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    m_view = new NodeView(m_scene, splitter);
    auto *side = new QWidget;
    side->setObjectName(QStringLiteral("Panel"));
    buildSidePanel(side);
    QScrollArea *scroll = verticalScroll(side, splitter);
    scroll->setMinimumWidth(300);
    splitter->addWidget(m_view);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setSizes({760, 340});
    setCentralWidget(splitter);
    statusBar()->setSizeGripEnabled(false);
    // How to work the canvas; messages (a refused link) show over it for a while.
    auto *hint = new ElidedLabel(tr("Drag from a port to another to connect them. Right-click to add a node. "
                                    "The wheel zooms; the middle button or Space and drag moves around."),
                                 statusBar());
    hint->setProperty("role", QStringLiteral("muted"));
    statusBar()->addWidget(hint, 1);

    // Wiring.
    connect(add, &QAction::triggered, this, [this, bar, add] {
        const QWidget *button = bar->widgetForAction(add);
        openNodeMenu(m_view->mapToScene(m_view->viewport()->rect().center()),
                     button ? button->mapToGlobal(QPoint(0, button->height())) : QCursor::pos());
    });
    connect(m_deleteAction, &QAction::triggered, this, &NodeEditorWindow::deleteSelection);
    connect(frame, &QAction::triggered, this, &NodeEditorWindow::frameAll);
    connect(saveAction, &QAction::triggered, this, [this] {
        QString error;
        if (!save(&error) && !error.isEmpty())
            showMessage(error);
    });
    connect(saveAsAction, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save filter as"), tr("Name:"), QLineEdit::Normal,
                                                   m_model->graph().name, &ok);
        QString error;
        if (ok && !saveAs(name, &error) && !error.isEmpty())
            showMessage(error);
    });
    connect(m_runAction, &QAction::triggered, this, [this] {
        if (m_running) {
            m_evaluator->cancel();
            return;
        }
        m_runRequested = true;
        startPreview();
    });
    connect(m_menu, &NodeMenu::picked, this, [this](const QString &typeId) { addNode(typeId, m_addAt); });
    connect(m_scene, &NodeScene::addRequested, this, &NodeEditorWindow::openNodeMenu);
    connect(m_scene, &NodeScene::refused, this, &NodeEditorWindow::showMessage);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &NodeEditorWindow::selectionChanged);
    connect(m_model, &GraphModel::changed, this, &NodeEditorWindow::modelChanged);
    connect(m_model, &GraphModel::modifiedChanged, this, &NodeEditorWindow::updateTitle);
    // AI steps say whether an engine can run them, and list the engines: both change when one is
    // added or removed.
    if (ai::GenerationService *service = ai::filterNodeService()) {
        connect(service->registry(), &ai::ProviderRegistry::providersChanged, this, [this] {
            m_scene->sync();
            updateProblems();
            rebuildNodeSection();
        });
    }

    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(60);
    connect(&m_previewTimer, &QTimer::timeout, this, &NodeEditorWindow::startPreview);

    m_saveQuestion = [this](const QString &name) {
        return QMessageBox::question(this, tr("Save the filter?"), tr("“%1” has changes that are not saved.").arg(name),
                                     QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    };

    // A size that fits the screen it opens on (the audit checks 1280 × 720).
    const QSize available = screen() ? screen()->availableGeometry().size() : QSize(1280, 720);
    resize(QSize(1180, 760).boundedTo(available - QSize(40, 60)));
    if (parent) {
        m_owner = parent->window();
        m_owner->installEventFilter(this);
    }
    rebuildNodeSection();
    setGraph(filters::Graph::createDefault());
}

NodeEditorWindow::~NodeEditorWindow()
{
    *m_alive = false;
    // The scene and the history go after this destructor and signal while they do.
    m_scene->disconnect(this);
    m_model->disconnect(this);
    m_model->undoStack()->disconnect(this);
    m_previewFuture.cancel();
    windows().removeAll(this);
    windows().removeIf([](const QPointer<NodeEditorWindow> &w) { return w.isNull(); });
}

QList<NodeEditorWindow *> NodeEditorWindow::openWindows()
{
    QList<NodeEditorWindow *> out;
    for (const QPointer<NodeEditorWindow> &w : windows()) {
        if (w)
            out << w;
    }
    return out;
}

NodeEditorWindow *NodeEditorWindow::openFilter(FilterCatalog *catalog, const QString &path, QWidget *parent, QString *error)
{
    const QString clean = QFileInfo(path).absoluteFilePath();
    for (NodeEditorWindow *w : openWindows()) {
        if (!w->path().isEmpty() && QFileInfo(w->path()).absoluteFilePath() == clean) {
            w->show();
            w->raise();
            w->activateWindow();
            return w;
        }
    }
    if (FilterCatalog::isBuiltIn(path)) {
        if (error)
            *error = tr("Built-in filters cannot be changed. Duplicate one to edit the copy.");
        return nullptr;
    }
    auto *w = new NodeEditorWindow(catalog, parent);
    if (!w->open(path, error)) {
        delete w;
        return nullptr;
    }
    w->show();
    w->frameAll();
    return w;
}

void NodeEditorWindow::buildSidePanel(QWidget *side)
{
    auto *layout = new QVBoxLayout(side);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // The selected node.
    m_nodeBox = new QWidget(side);
    auto *nodeOuter = new QVBoxLayout(m_nodeBox);
    nodeOuter->setContentsMargins(0, 0, 0, 0);
    nodeOuter->setSpacing(6);
    m_nodeTitle = new QLabel(m_nodeBox);
    m_nodeTitle->setWordWrap(true);
    QFont bold = m_nodeTitle->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() + 1);
    m_nodeTitle->setFont(bold);
    m_nodeHelp = mutedLabel(QString(), m_nodeBox);
    m_nodeHelp->setWordWrap(true);
    nodeOuter->addWidget(m_nodeTitle);
    nodeOuter->addWidget(m_nodeHelp);
    m_nodeLayout = new QVBoxLayout;
    m_nodeLayout->setSpacing(8);
    nodeOuter->addLayout(m_nodeLayout);
    nodeOuter->addWidget(separator(Qt::Horizontal, m_nodeBox));
    layout->addWidget(m_nodeBox);

    // The preview of the selected node.
    m_previewTitle = sectionLabel(tr("Preview"), side);
    layout->addWidget(m_previewTitle);
    m_previewLabel = new QLabel(side);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumHeight(kPreviewHeight);
    m_previewLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_previewLabel->setAccessibleName(tr("Preview"));
    layout->addWidget(m_previewLabel);
    m_previewInfo = mutedLabel(QString(), side);
    m_previewInfo->setWordWrap(true);
    layout->addWidget(m_previewInfo);
    m_previewRun = new QPushButton(Icons::get(QStringLiteral("play")), tr("Run AI steps"), side);
    m_previewRun->setToolTip(m_runAction->toolTip());
    layout->addWidget(m_previewRun, 0, Qt::AlignLeft);
    connect(m_previewRun, &QPushButton::clicked, m_runAction, &QAction::trigger);
    layout->addWidget(separator(Qt::Horizontal, side));

    // Problems.
    layout->addWidget(sectionLabel(tr("Problems"), side));
    m_noProblems = mutedLabel(tr("None: the filter can be used."), side);
    m_noProblems->setWordWrap(true);
    layout->addWidget(m_noProblems);
    m_problems = new QListWidget(side);
    m_problems->setWordWrap(true);
    m_problems->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_problems->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_problems->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    m_problems->setAccessibleName(tr("Problems"));
    layout->addWidget(m_problems);
    connect(m_problems, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString node = item->data(Qt::UserRole).toString();
        if (!node.isEmpty())
            selectNode(node);
    });
    layout->addWidget(separator(Qt::Horizontal, side));

    // The filter as a whole.
    layout->addWidget(sectionLabel(tr("Filter"), side));
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_name = new QLineEdit(side);
    m_name->setAccessibleName(tr("Filter name"));
    m_description = new QLineEdit(side);
    m_description->setAccessibleName(tr("What the filter does"));
    m_description->setPlaceholderText(tr("What the filter does, in a sentence"));
    form->addRow(tr("Name"), m_name);
    form->addRow(tr("About"), m_description);
    layout->addLayout(form);
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_model->change(tr("Rename filter"), [&](filters::Graph &g) {
            g.name = text;
            return true;
        }, QStringLiteral("name"));
    });
    connect(m_description, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_model->change(tr("Change the description"), [&](filters::Graph &g) {
            g.description = text;
            return true;
        }, QStringLiteral("description"));
    });
    auto *publishedTitle = mutedLabel(tr("Settings shown in the Filters panel"), side);
    publishedTitle->setWordWrap(true);
    layout->addWidget(publishedTitle);
    m_publishedLayout = new QVBoxLayout;
    m_publishedLayout->setSpacing(4);
    layout->addLayout(m_publishedLayout);
    m_noPublished = mutedLabel(tr("None yet. Select a node and click the slider button next to a setting."), side);
    m_noPublished->setWordWrap(true);
    layout->addWidget(m_noPublished);
    layout->addStretch(1);
}

bool NodeEditorWindow::open(const QString &path, QString *error)
{
    const std::optional<filters::Graph> graph = m_catalog->load(path, error);
    if (!graph)
        return false;
    m_path = path;
    m_model->reset(*graph);
    m_scene->clearSelection();
    updateTitle();
    return true;
}

void NodeEditorWindow::setGraph(const filters::Graph &graph)
{
    m_path.clear();
    m_model->reset(graph);
    m_scene->clearSelection();
    updateTitle();
}

void NodeEditorWindow::setSource(const QImage &image, const QImage &selection)
{
    m_source = image;
    m_selection = selection;
    schedulePreview();
}

QString NodeEditorWindow::addNode(const QString &typeId, std::optional<QPointF> scenePos)
{
    const filters::NodeTypePtr type = filters::NodeRegistry::global().find(typeId);
    if (!type)
        return {};
    const QPointF centre = scenePos ? *scenePos : m_view->mapToScene(m_view->viewport()->rect().center());
    const QPointF place = m_scene->freePlace(centre - QPointF(NodeItem::kWidth / 2, 30));
    QString id;
    m_model->change(tr("Add “%1”").arg(type->info().name), [&](filters::Graph &g) {
        id = g.addNode(type, QPointF(std::round(place.x()), std::round(place.y())));
        return !id.isEmpty();
    });
    m_addAt.reset();
    if (!id.isEmpty())
        selectNode(id);
    return id;
}

bool NodeEditorWindow::connectPorts(const QString &fromNode, const QString &fromPort, const QString &toNode,
                                    const QString &toPort)
{
    return m_scene->connectPorts(fromNode, fromPort, toNode, toPort);
}

bool NodeEditorWindow::deleteSelection()
{
    return m_scene->deleteSelection();
}

void NodeEditorWindow::selectNode(const QString &id)
{
    m_scene->selectNode(id);
    if (NodeItem *item = m_scene->nodeItem(id))
        m_view->ensureVisible(item, 40, 40);
}

bool NodeEditorWindow::setParam(const QString &node, const QString &param, const QVariant &value)
{
    const filters::Node *n = m_model->graph().node(node);
    const filters::ParamSpec *spec = n && n->type ? n->type->info().param(param) : nullptr;
    if (!spec)
        return false;
    return m_model->change(tr("Change “%1”").arg(spec->label), [&](filters::Graph &g) { return g.setParam(node, param, value); },
                           node + QLatin1Char('/') + param);
}

bool NodeEditorWindow::publish(const QString &node, const QString &param, const QString &label, double minimum,
                               double maximum)
{
    const filters::Node *n = m_model->graph().node(node);
    const filters::ParamSpec *spec = n && n->type ? n->type->info().param(param) : nullptr;
    if (!spec)
        return false;
    const QString text = label.trimmed().isEmpty() ? spec->label : label.trimmed();
    return m_model->change(tr("Show “%1” in the Filters panel").arg(text), [&](filters::Graph &g) {
        // A published setting keeps its place in the list when it is published again.
        QList<filters::PublishedParam> list = g.published();
        const filters::PublishedParam p{node, param, text, minimum, maximum};
        const auto it = std::find_if(list.begin(), list.end(),
                                     [&](const filters::PublishedParam &q) { return q.node == node && q.param == param; });
        if (it != list.end())
            *it = p;
        else
            list.append(p);
        g.setPublished(list);
        return true;
    });
}

bool NodeEditorWindow::unpublish(const QString &node, const QString &param)
{
    return m_model->change(tr("Remove from the Filters panel"), [&](filters::Graph &g) {
        const qsizetype before = g.published().size();
        g.unpublish(node, param);
        return g.published().size() != before;
    });
}

void NodeEditorWindow::frameAll()
{
    m_view->frameAll();
}

bool NodeEditorWindow::isModified() const
{
    return m_model->isModified();
}

bool NodeEditorWindow::save(QString *error)
{
    if (m_path.isEmpty() || FilterCatalog::isBuiltIn(m_path)) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save filter"), tr("Name:"), QLineEdit::Normal,
                                                   m_model->graph().name, &ok);
        return ok && saveAs(name, error);
    }
    const QString path = m_catalog->save(m_model->graph(), m_path, error);
    if (path.isEmpty())
        return false;
    m_model->markSaved();
    updateTitle();
    showMessage(tr("Saved “%1”.").arg(m_model->graph().name));
    emit saved(path);
    return true;
}

bool NodeEditorWindow::saveAs(const QString &name, QString *error)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty()) {
        if (error)
            *error = tr("Give the filter a name.");
        return false;
    }
    if (clean != m_model->graph().name) {
        m_model->change(tr("Rename filter"), [&](filters::Graph &g) {
            g.name = clean;
            return true;
        });
    }
    const QString path = m_catalog->save(m_model->graph(), {}, error);
    if (path.isEmpty())
        return false;
    m_path = path;
    m_model->markSaved();
    updateTitle();
    showMessage(tr("Saved “%1”.").arg(clean));
    emit saved(path);
    return true;
}

QStringList NodeEditorWindow::problems() const
{
    QStringList out;
    for (int i = 0; i < m_problems->count(); ++i)
        out << m_problems->item(i)->text();
    return out;
}

QString NodeEditorWindow::previewText() const
{
    return m_previewText;
}

bool NodeEditorWindow::isPreviewPending() const
{
    // Until the result is on screen: the evaluation's future ends before its continuation runs.
    return m_previewTimer.isActive() || m_pendingAfterRun || m_shownGeneration != m_previewGeneration;
}

void NodeEditorWindow::showMessage(const QString &message)
{
    m_message = message;
    statusBar()->showMessage(message, 12000);
}

void NodeEditorWindow::updateTitle()
{
    const QString name = m_model->graph().name.isEmpty() ? tr("Untitled filter") : m_model->graph().name;
    setWindowTitle(tr("%1[*] — Filter editor").arg(name));
    setWindowModified(m_model->isModified());
}

void NodeEditorWindow::openNodeMenu(const QPointF &scenePos, const QPoint &screenPos)
{
    m_addAt = scenePos;
    m_menu->popup(screenPos);
}

// ---- Keeping the side panel in step --------------------------------------------------------------

void NodeEditorWindow::modelChanged()
{
    if (!m_selected.isEmpty() && !m_model->graph().node(m_selected)) {
        m_selected.clear();
        rebuildNodeSection();
    } else {
        updateNodeValues();
    }
    updateFilterSection();
    updateProblems();
    updateTitle();
    m_runAction->setVisible(m_model->graph().needsExplicitRun());
    m_previewRun->setVisible(m_model->graph().needsExplicitRun());
    schedulePreview();
}

void NodeEditorWindow::selectionChanged()
{
    const QStringList nodes = m_scene->selectedNodes();
    const QString selected = nodes.size() == 1 ? nodes.first() : QString();
    m_deleteAction->setEnabled(!m_scene->selectedItems().isEmpty());
    if (selected == m_selected)
        return;
    m_selected = selected;
    rebuildNodeSection();
    updatePreviewTitle();
    schedulePreview();
}

void NodeEditorWindow::updatePreviewTitle()
{
    const filters::Node *node = m_model->graph().node(m_selected);
    m_previewTitle->setText(node && node->type ? tr("Preview of “%1”").arg(node->type->info().name)
                                               : tr("Preview of the result"));
}

void NodeEditorWindow::rebuildNodeSection()
{
    while (QLayoutItem *item = m_nodeLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->setParent(nullptr); // gone from the panel now; deleted once its signals are done
            w->deleteLater();
        }
        delete item;
    }
    m_fields.clear();
    m_publishButtons.clear();
    const filters::Node *node = m_model->graph().node(m_selected);
    m_nodeBox->setVisible(node != nullptr);
    if (!node || !node->type)
        return;
    const filters::NodeTypeInfo &info = node->type->info();
    m_nodeTitle->setText(info.name);
    m_nodeHelp->setText(info.description);
    if (info.params.isEmpty()) {
        auto *none = mutedLabel(tr("This node has no settings."), m_nodeBox);
        none->setWordWrap(true);
        m_nodeLayout->addWidget(none);
    }
    const QString nodeId = node->id;
    for (const filters::ParamSpec &spec : info.params) {
        auto *row = new QWidget(m_nodeBox);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        auto *field = new ParamField(spec, node->param(spec.id), {}, 0.0, 0.0, row);
        field->setObjectName(QStringLiteral("param:%1").arg(spec.id));
        h->addWidget(field, 1);
        auto *publish = iconButton(QStringLiteral("adjust"), tr("Show “%1” in the Filters panel").arg(spec.label), row, true);
        publish->setObjectName(QStringLiteral("publish:%1").arg(spec.id));
        h->addWidget(publish, 0, spec.editor == QLatin1String("curve") ? Qt::AlignTop : Qt::AlignVCenter);
        m_nodeLayout->addWidget(row);
        m_fields << field;
        m_publishButtons << publish;
        const QString paramId = spec.id;
        connect(field, &ParamField::valueChanged, this, [this, nodeId, paramId](const QVariant &v) { setParam(nodeId, paramId, v); });
        connect(publish, &QToolButton::clicked, this, [this, nodeId, paramId](bool on) {
            if (on)
                askToPublish(nodeId, paramId);
            else
                unpublish(nodeId, paramId);
            updateNodeValues(); // the button follows the graph, also when the question was cancelled
        });
    }
    updateNodeValues();
}

void NodeEditorWindow::updateNodeValues()
{
    const filters::Graph &g = m_model->graph();
    const filters::Node *node = g.node(m_selected);
    if (!node || !node->type)
        return;
    const filters::NodeTypeInfo &info = node->type->info();
    for (int i = 0; i < m_fields.size() && i < info.params.size(); ++i) {
        const filters::ParamSpec &spec = info.params.at(i);
        m_fields[i]->setValue(node->param(spec.id));
        // A connected input of the same name sets the value instead.
        const bool driven = info.input(spec.id) && g.linkInto(node->id, spec.id);
        m_fields[i]->setEnabled(!driven);
        m_fields[i]->setToolTip(driven ? tr("Set by what is connected to the “%1” input.").arg(spec.label) : spec.help);
        const bool published = std::any_of(g.published().begin(), g.published().end(), [&](const filters::PublishedParam &p) {
            return p.node == node->id && p.param == spec.id;
        });
        QSignalBlocker block(m_publishButtons[i]);
        m_publishButtons[i]->setChecked(published);
        m_publishButtons[i]->setToolTip(published ? tr("Shown in the Filters panel; click to remove it from there")
                                                  : tr("Show “%1” in the Filters panel").arg(spec.label));
    }
}

void NodeEditorWindow::updateFilterSection()
{
    const filters::Graph &g = m_model->graph();
    if (m_name->text() != g.name)
        m_name->setText(g.name);
    if (m_description->text() != g.description)
        m_description->setText(g.description);
    while (QLayoutItem *item = m_publishedLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    for (const filters::PublishedParam &p : g.published()) {
        const filters::Node *n = g.node(p.node);
        const filters::ParamSpec *spec = n && n->type ? n->type->info().param(p.param) : nullptr;
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        auto *label = new QPushButton(row);
        label->setProperty("role", QStringLiteral("ghost"));
        label->setStyleSheet(QStringLiteral("text-align: left;"));
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        const QString where = n && spec ? QStringLiteral("%1 → %2").arg(n->type->info().name, spec->label) : p.param;
        label->setText(p.label);
        label->setToolTip(tr("%1 (%2). Click to select the node.").arg(p.label, where));
        auto *remove = iconButton(QStringLiteral("close"), tr("Remove “%1” from the Filters panel").arg(p.label), row);
        h->addWidget(label, 1);
        h->addWidget(remove);
        m_publishedLayout->addWidget(row);
        const QString node = p.node, param = p.param;
        connect(label, &QPushButton::clicked, this, [this, node] { selectNode(node); });
        connect(remove, &QToolButton::clicked, this, [this, node, param] { unpublish(node, param); });
    }
    m_noPublished->setVisible(g.published().isEmpty());
}

void NodeEditorWindow::updateProblems()
{
    m_problems->clear();
    const Palette &pal = Theme::palette();
    // Steps that cannot run now (an AI step without a capable engine) are listed with the rest.
    for (const filters::Issue &issue : m_model->graph().validate() + m_model->graph().unavailable()) {
        const bool error = issue.severity == filters::Issue::Severity::Error;
        auto *item = new QListWidgetItem(QIcon(Icons::pixmap(QStringLiteral("warning"), 16, error ? pal.danger : pal.warning)),
                                         issue.message);
        item->setData(Qt::UserRole, issue.node);
        item->setToolTip(issue.node.isEmpty() ? issue.message : tr("%1\nClick to select the node.").arg(issue.message));
        m_problems->addItem(item);
    }
    m_problems->setVisible(m_problems->count() > 0);
    m_noProblems->setVisible(m_problems->count() == 0);
    // As tall as its rows: the side panel scrolls, the list does not.
    int height = 4;
    for (int i = 0; i < m_problems->count(); ++i)
        height += m_problems->sizeHintForRow(i) + 2;
    m_problems->setFixedHeight(std::max(24, height));
}

void NodeEditorWindow::askToPublish(const QString &node, const QString &param)
{
    const filters::Node *n = m_model->graph().node(node);
    const filters::ParamSpec *spec = n && n->type ? n->type->info().param(param) : nullptr;
    if (!spec)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Show in the Filters panel"));
    auto *form = new QFormLayout(&dialog);
    auto *intro = new QLabel(tr("People using the filter see this setting as a slider (or a list), without opening the editor."),
                             &dialog);
    intro->setWordWrap(true);
    form->addRow(intro);
    auto *label = new QLineEdit(spec->label, &dialog);
    form->addRow(tr("Label"), label);
    QDoubleSpinBox *low = nullptr;
    QDoubleSpinBox *high = nullptr;
    if (spec->type == filters::PortType::Number) {
        low = new QDoubleSpinBox(&dialog);
        high = new QDoubleSpinBox(&dialog);
        for (QDoubleSpinBox *s : {low, high}) {
            s->setRange(spec->minimum, spec->maximum);
            s->setDecimals(spec->decimals);
            s->setSuffix(unitSuffix(spec->unit));
        }
        low->setValue(spec->minimum);
        high->setValue(spec->maximum);
        form->addRow(tr("Lowest"), low);
        form->addRow(tr("Highest"), high);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (low && high && low->value() >= high->value()) {
            QMessageBox::information(&dialog, dialog.windowTitle(), tr("The lowest value must be below the highest."));
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    double minimum = 0.0, maximum = 0.0;
    if (low && high && (low->value() != spec->minimum || high->value() != spec->maximum)) {
        minimum = low->value();
        maximum = high->value();
    }
    publish(node, param, label->text(), minimum, maximum);
}

// ---- The preview of the selected node ----------------------------------------------------------

void NodeEditorWindow::schedulePreview()
{
    if (m_running) {
        // AI steps are not cut short by a change; the preview follows when they end.
        m_pendingAfterRun = true;
        return;
    }
    m_previewTimer.start();
}

void NodeEditorWindow::startPreview()
{
    m_previewTimer.stop();
    if (m_previewFuture.isRunning())
        m_previewFuture.cancel();
    const int generation = ++m_previewGeneration;
    const filters::Graph &g = m_model->graph();
    const QString target = m_selected.isEmpty() ? g.outputNode() : m_selected;
    const filters::Node *node = g.node(target);
    updatePreviewTitle();
    if (!node) {
        m_shownGeneration = generation;
        showPreview({}, tr("Add an “Output” node to see the result."));
        return;
    }
    const QImage source = m_source.isNull() ? filterSampleImage() : m_source;
    filters::EvalOptions options;
    options.scale = std::min(1.0, double(kPreviewSide) / std::max(source.width(), source.height()));
    options.target = m_selected.isEmpty() ? QString() : m_selected;
    options.runExpensive = m_runRequested;
    const bool run = m_runRequested;
    m_runRequested = false;
    if (run) {
        setRunning(true);
        showPreview(m_previewImage, tr("Running the AI steps…"));
        // The AI step's job tells how far it is ("Step 2 / 4").
        options.stepProgress = [self = QPointer<NodeEditorWindow>(this), generation](double, const QString &status) {
            QMetaObject::invokeMethod(qApp, [self, generation, status] {
                if (self && self->m_running && generation == self->m_previewGeneration && !status.isEmpty()) {
                    self->m_previewInfo->setText(tr("Running the AI steps… %1").arg(status));
                    self->m_previewInfo->show();
                }
            }, Qt::QueuedConnection);
        };
    }
    const filters::NodeTypePtr type = node->type;
    m_previewFuture = m_evaluator->evaluate(g, {source, m_source.isNull() ? QImage() : m_selection}, options);
    m_previewFuture
        .then(this, [this, alive = m_alive, generation, type](const filters::EvalResult &r) {
            if (!*alive || generation != m_previewGeneration)
                return;
            m_shownGeneration = generation;
            setRunning(false);
            if (r.needsRun) {
                showPreview({}, tr("This filter has AI steps. They take a while, so they run when you click “Run AI steps”."));
                return;
            }
            // The first picture the node gives; values as text.
            QImage image;
            QStringList values;
            for (const filters::PortSpec &port : type->info().outputs) {
                const QVariant v = r.outputs.value(port.id);
                if (port.type == filters::PortType::Image || port.type == filters::PortType::Mask) {
                    if (image.isNull())
                        image = v.value<QImage>();
                } else {
                    values << QStringLiteral("%1: %2").arg(port.label, valueText(v));
                }
            }
            // The output node gives the filter's result rather than outputs of its own.
            if (image.isNull() && type->info().isOutput)
                image = r.image;
            showPreview(image, values.join(QLatin1Char('\n')));
        })
        .onFailed(this, [this, alive = m_alive, generation](const std::exception &e) {
            if (!*alive || generation != m_previewGeneration)
                return;
            m_shownGeneration = generation;
            setRunning(false);
            showPreview({}, describeException(e));
        })
        .onCanceled(this, [this, alive = m_alive, generation] {
            if (!*alive || generation != m_previewGeneration)
                return;
            m_shownGeneration = generation;
            setRunning(false);
            showPreview({}, tr("Stopped."));
        });
}

void NodeEditorWindow::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    m_runAction->setText(running ? tr("Stop AI steps") : tr("Run AI steps"));
    m_runAction->setIcon(Icons::get(running ? QStringLiteral("stop") : QStringLiteral("play")));
    m_previewRun->setText(m_runAction->text());
    m_previewRun->setIcon(m_runAction->icon());
    if (!running && m_pendingAfterRun) {
        m_pendingAfterRun = false;
        schedulePreview();
    }
}

void NodeEditorWindow::showPreview(const QImage &image, const QString &text)
{
    m_previewImage = image;
    m_previewText = text;
    const int width = std::max(120, m_previewLabel->width());
    if (image.isNull()) {
        m_previewLabel->setPixmap(QPixmap());
        m_previewLabel->setText(tr("No picture"));
    } else {
        m_previewLabel->setPixmap(previewPixmap(image, QSize(width, kPreviewHeight)));
    }
    m_previewInfo->setText(text);
    m_previewInfo->setVisible(!text.isEmpty());
    m_previewRun->setVisible(m_model->graph().needsExplicitRun());
    emit previewUpdated();
}

bool NodeEditorWindow::confirmClose()
{
    if (!m_model->isModified() || !m_saveQuestion)
        return true;
    const QString name = m_model->graph().name.isEmpty() ? tr("Untitled filter") : m_model->graph().name;
    switch (m_saveQuestion(name)) {
    case QMessageBox::Save: {
        QString error;
        if (!save(&error)) {
            if (!error.isEmpty())
                showMessage(error);
            return false;
        }
        return true;
    }
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

void NodeEditorWindow::closeEvent(QCloseEvent *e)
{
    if (!confirmClose()) {
        e->ignore();
        return;
    }
    m_evaluator->cancel();
    e->accept();
}

bool NodeEditorWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_owner && event->type() == QEvent::Close && m_model->isModified()) {
        show();
        raise();
        activateWindow();
        if (!confirmClose()) {
            event->ignore();
            return true; // the owner stays open
        }
        m_model->markSaved(); // saved, or the changes are given up: the owner closes with this window
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace lumen::ui
