#include "ui/pages/PosePage.h"

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "core/Async.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "pose/Anatomy.h"
#include "pose/PosePresets.h"
#include "pose/PoseRenderer.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/pose/PoseCanvas.h"
#include "ui/widgets/Controls.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

class PoseStateCommand : public QUndoCommand
{
public:
    PoseStateCommand(PosePage *page, PosePage::State before, PosePage::State after, const QString &text)
        : QUndoCommand(text)
        , m_page(page)
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
    }
    void redo() override
    {
        if (m_first) {
            m_first = false;
            return;
        }
        m_page->setState(m_after, false);
    }
    void undo() override { m_page->setState(m_before, false); }

private:
    PosePage *m_page;
    PosePage::State m_before;
    PosePage::State m_after;
    bool m_first = true;
};

struct RigField
{
    QString key;
    QString group;
    QString label;
    double min, max;
    std::function<double &(pose::RigParams &)> ref;
};

QList<RigField> rigFields()
{
    using R = pose::RigParams;
    auto f = [](const QString &key, const QString &group, const QString &label, double min, double max, std::function<double &(R &)> ref) {
        return RigField{key, group, label, min, max, std::move(ref)};
    };
    auto limb = [&](const QString &prefix, const QString &group, std::function<pose::LimbParams &(R &)> l) {
        return QList<RigField>{
            f(prefix + QStringLiteral(".raise"), group, QObject::tr("Raise"), -45, 200, [l](R &r) -> double & { return l(r).raise; }),
            f(prefix + QStringLiteral(".direction"), group, QObject::tr("Direction"), -180, 180, [l](R &r) -> double & { return l(r).direction; }),
            f(prefix + QStringLiteral(".bend"), group, QObject::tr("Bend"), 0, 170, [l](R &r) -> double & { return l(r).bend; }),
            f(prefix + QStringLiteral(".twist"), group, QObject::tr("Bend plane"), -180, 180, [l](R &r) -> double & { return l(r).bendTwist; }),
        };
    };
    QList<RigField> fields = {
        f(QStringLiteral("yaw"), QObject::tr("Body & camera"), QObject::tr("Turn"), -180, 180, [](R &r) -> double & { return r.yaw; }),
        f(QStringLiteral("pitch"), QObject::tr("Body & camera"), QObject::tr("Lean to viewer"), -90, 90, [](R &r) -> double & { return r.pitch; }),
        f(QStringLiteral("roll"), QObject::tr("Body & camera"), QObject::tr("Tilt"), -180, 180, [](R &r) -> double & { return r.roll; }),
        f(QStringLiteral("camera"), QObject::tr("Body & camera"), QObject::tr("Camera height"), -80, 80, [](R &r) -> double & { return r.cameraPitch; }),
        f(QStringLiteral("perspective"), QObject::tr("Body & camera"), QObject::tr("Perspective"), 0, 1, [](R &r) -> double & { return r.perspective; }),
        f(QStringLiteral("torso.lean"), QObject::tr("Torso & head"), QObject::tr("Torso lean"), -60, 100, [](R &r) -> double & { return r.torsoLean; }),
        f(QStringLiteral("torso.side"), QObject::tr("Torso & head"), QObject::tr("Torso side"), -60, 60, [](R &r) -> double & { return r.torsoSide; }),
        f(QStringLiteral("torso.twist"), QObject::tr("Torso & head"), QObject::tr("Torso twist"), -90, 90, [](R &r) -> double & { return r.torsoTwist; }),
        f(QStringLiteral("head.yaw"), QObject::tr("Torso & head"), QObject::tr("Head turn"), -90, 90, [](R &r) -> double & { return r.headYaw; }),
        f(QStringLiteral("head.pitch"), QObject::tr("Torso & head"), QObject::tr("Head nod"), -60, 60, [](R &r) -> double & { return r.headPitch; }),
        f(QStringLiteral("head.roll"), QObject::tr("Torso & head"), QObject::tr("Head tilt"), -45, 45, [](R &r) -> double & { return r.headRoll; }),
    };
    fields += limb(QStringLiteral("larm"), QObject::tr("Left arm"), [](R &r) -> pose::LimbParams & { return r.leftArm; });
    fields += limb(QStringLiteral("rarm"), QObject::tr("Right arm"), [](R &r) -> pose::LimbParams & { return r.rightArm; });
    fields += limb(QStringLiteral("lleg"), QObject::tr("Left leg"), [](R &r) -> pose::LimbParams & { return r.leftLeg; });
    fields += limb(QStringLiteral("rleg"), QObject::tr("Right leg"), [](R &r) -> pose::LimbParams & { return r.rightLeg; });
    fields += {
        f(QStringLiteral("pos.x"), QObject::tr("Placement"), QObject::tr("Horizontal"), -0.5, 1.5, [](R &r) -> double & { return r.positionX; }),
        f(QStringLiteral("pos.y"), QObject::tr("Placement"), QObject::tr("Vertical"), -1, 1, [](R &r) -> double & { return r.positionY; }),
        f(QStringLiteral("pos.scale"), QObject::tr("Placement"), QObject::tr("Scale"), 0.2, 3, [](R &r) -> double & { return r.scale; }),
    };
    return fields;
}

const QStringList kAspects = {QStringLiteral("2:3"), QStringLiteral("1:1"), QStringLiteral("3:4"), QStringLiteral("9:16"),
                              QStringLiteral("3:2"), QStringLiteral("4:3"), QStringLiteral("16:9"), QStringLiteral("2:1")};

} // namespace

PosePage::PosePage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ---- presets column ----
    auto *left = new QWidget(splitter);
    left->setObjectName(QStringLiteral("Panel"));
    left->setMinimumWidth(250);
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(12, 14, 12, 12);
    auto *title = new QLabel(tr("Poses"), left);
    Theme::setRole(title, QStringLiteral("title"));
    lv->addWidget(title);
    lv->addWidget(mutedLabel(tr("Click a preset to apply it to the selected person. Right-click for more."), left));
    m_presets = new QListWidget(left);
    m_presets->setViewMode(QListView::IconMode);
    m_presets->setIconSize(QSize(84, 112));
    m_presets->setGridSize(QSize(104, 150));
    m_presets->setResizeMode(QListView::Adjust);
    m_presets->setMovement(QListView::Static);
    m_presets->setWordWrap(true);
    m_presets->setContextMenuPolicy(Qt::CustomContextMenu);
    lv->addWidget(m_presets, 1);
    auto *presetButtons = new QHBoxLayout;
    auto *savePreset = new QPushButton(Icons::get(QStringLiteral("save")), tr("Save as preset"), left);
    presetButtons->addWidget(savePreset);
    lv->addLayout(presetButtons);

    // ---- canvas column ----
    auto *center = new QWidget(splitter);
    auto *cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    auto *toolbar = new QWidget(center);
    toolbar->setObjectName(QStringLiteral("Panel"));
    auto *th = new QHBoxLayout(toolbar);
    th->setContentsMargins(12, 6, 12, 6);
    th->setSpacing(6);
    m_modes = new QButtonGroup(this);
    const QList<std::tuple<pose::DragMode, QString, QString>> modes = {
        {pose::DragMode::Rotate, tr("Rotate"), tr("Rotate joints around their parent (keeps bone lengths)")},
        {pose::DragMode::Chain, tr("IK"), tr("Drag hands/feet and let elbows/knees follow (Ctrl)")},
        {pose::DragMode::Free, tr("Free"), tr("Move single points (Alt)")},
        {pose::DragMode::Translate, tr("Move part"), tr("Move a joint with everything attached to it (Shift)")},
    };
    for (const auto &[mode, label, tip] : modes) {
        auto *b = new QToolButton(toolbar);
        b->setText(label);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setProperty("role", QStringLiteral("chip"));
        m_modes->addButton(b, static_cast<int>(mode));
        th->addWidget(b);
    }
    m_modes->button(static_cast<int>(pose::DragMode::Rotate))->setChecked(true);
    th->addSpacing(10);
    th->addWidget(new QLabel(tr("Canvas"), toolbar));
    m_aspect = new QComboBox(toolbar);
    m_aspect->addItems(kAspects);
    th->addWidget(m_aspect);
    th->addSpacing(10);
    th->addWidget(new QLabel(tr("Person"), toolbar));
    m_people = new QComboBox(toolbar);
    th->addWidget(m_people);
    auto *addPerson = iconButton(QStringLiteral("plus"), tr("Add a person"), toolbar);
    auto *removePerson = iconButton(QStringLiteral("trash"), tr("Remove the selected person"), toolbar);
    auto *mirror = iconButton(QStringLiteral("flip"), tr("Mirror the selected person"), toolbar);
    th->addWidget(addPerson);
    th->addWidget(removePerson);
    th->addWidget(mirror);
    th->addStretch();
    auto *undo = iconButton(QStringLiteral("undo"), tr("Undo"), toolbar);
    auto *redo = iconButton(QStringLiteral("redo"), tr("Redo"), toolbar);
    th->addWidget(undo);
    th->addWidget(redo);
    cv->addWidget(toolbar);
    m_canvas = new PoseCanvas(center);
    cv->addWidget(m_canvas, 1);
    auto *bottom = new QWidget(center);
    bottom->setObjectName(QStringLiteral("Panel"));
    auto *bh = new QHBoxLayout(bottom);
    bh->setContentsMargins(12, 8, 12, 8);
    auto *hands = new QCheckBox(tr("Hands"), bottom);
    hands->setChecked(true);
    auto *face = new QCheckBox(tr("Face"), bottom);
    face->setChecked(true);
    auto *underlay = new QPushButton(Icons::get(QStringLiteral("image")), tr("Underlay…"), bottom);
    underlay->setToolTip(tr("Show a reference image under the skeleton"));
    auto *opacity = new SliderField(tr("Opacity"), 0, 100, 50, 0, QStringLiteral(" %"), bottom);
    opacity->setCompact(true);
    opacity->setFixedWidth(150);
    auto *detect = new QPushButton(Icons::get(QStringLiteral("target")), tr("Detect…"), bottom);
    detect->setToolTip(tr("Detect the pose of a person in an image"));
    bh->addWidget(hands);
    bh->addWidget(face);
    bh->addSpacing(12);
    bh->addWidget(underlay);
    bh->addWidget(opacity);
    bh->addWidget(detect);
    bh->addStretch();
    auto *more = new QPushButton(Icons::get(QStringLiteral("dots")), tr("File"), bottom);
    auto *fileMenu = new QMenu(more);
    fileMenu->addAction(Icons::get(QStringLiteral("download")), tr("Export pose map (PNG)…"), this, &PosePage::exportPng);
    fileMenu->addAction(Icons::get(QStringLiteral("download")), tr("Export OpenPose JSON…"), this, &PosePage::exportJson);
    fileMenu->addAction(Icons::get(QStringLiteral("upload")), tr("Import OpenPose JSON…"), this, &PosePage::importJson);
    more->setMenu(fileMenu);
    auto *toDoc = new QPushButton(Icons::get(QStringLiteral("brush")), tr("To document"), bottom);
    toDoc->setToolTip(tr("Use as the control pose of the active editor document"));
    auto *toGen = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Use in Generate"), bottom);
    Theme::setRole(toGen, QStringLiteral("primary"));
    bh->addWidget(more);
    bh->addWidget(toDoc);
    bh->addWidget(toGen);
    cv->addWidget(bottom);

    // ---- rig column ----
    auto *right = new QWidget(splitter);
    right->setObjectName(QStringLiteral("Panel"));
    right->setMinimumWidth(330);
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(12, 14, 12, 12);
    rv->setSpacing(8);
    rv->addWidget(sectionLabel(tr("Describe the pose"), right));
    auto *describeRow = new QHBoxLayout;
    m_describe = new QLineEdit(right);
    m_describe->setPlaceholderText(tr("e.g. sitting cross-legged, waving with the left hand"));
    m_describeBtn = new QPushButton(Icons::get(QStringLiteral("sparkles")), QString(), right);
    m_describeBtn->setToolTip(tr("Ask the director model to pose the selected person"));
    describeRow->addWidget(m_describe, 1);
    describeRow->addWidget(m_describeBtn);
    rv->addLayout(describeRow);
    auto *scroll = new QScrollArea(right);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rigPanel = new QWidget(scroll);
    scroll->setWidget(m_rigPanel);
    auto *rigLayout = new QVBoxLayout(m_rigPanel);
    rigLayout->setContentsMargins(0, 0, 6, 0);
    buildRigPanel(rigLayout);
    rv->addWidget(scroll, 1);

    splitter->addWidget(left);
    splitter->addWidget(center);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({270, 880, 360});

    // ---- wiring ----
    connect(m_modes, &QButtonGroup::idClicked, this, [this](int id) { m_canvas->setDragMode(static_cast<pose::DragMode>(id)); });
    connect(m_canvas, &PoseCanvas::poseEdited, this, [this](const pose::Pose &p) {
        m_state.pose = p;
        // Manual edits detach the edited person from the generation rig.
        const int sel = m_canvas->selectedPerson();
        if (sel >= 0 && sel < m_state.rigs.size() && m_state.rigs[sel]) {
            m_state.rigs[sel].reset();
            syncRigPanel();
        }
    });
    connect(m_canvas, &PoseCanvas::editFinished, this, [this](const pose::Pose &before, const pose::Pose &) {
        State prev = m_state;
        prev.pose = before;
        m_undo.push(new PoseStateCommand(this, prev, m_state, tr("Move joint")));
    });
    connect(m_canvas, &PoseCanvas::personSelected, this, [this](int i) {
        QSignalBlocker b(m_people);
        m_people->setCurrentIndex(i);
        syncRigPanel();
    });
    connect(m_people, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0)
            m_canvas->setSelectedPerson(i);
        syncRigPanel();
    });
    connect(m_aspect, &QComboBox::currentTextChanged, this, &PosePage::setAspect);
    connect(addPerson, &QToolButton::clicked, this, [this] {
        auto preset = pose::findBuiltInPreset(QStringLiteral("standing"));
        applyPreset(*preset, true);
    });
    connect(removePerson, &QToolButton::clicked, this, [this] {
        const int sel = m_canvas->selectedPerson();
        if (sel < 0 || sel >= m_state.pose.people.size() || m_state.pose.people.size() <= 1)
            return;
        State next = m_state;
        next.pose.people.removeAt(sel);
        next.rigs.removeAt(sel);
        setState(next, true, tr("Remove person"));
    });
    connect(mirror, &QToolButton::clicked, this, [this] {
        const int sel = m_canvas->selectedPerson();
        if (sel < 0 || sel >= m_state.pose.people.size())
            return;
        State next = m_state;
        const QRectF b = next.pose.people[sel].bounds();
        next.pose.people[sel].mirror(b.center().x());
        if (next.rigs[sel]) {
            pose::RigParams r = *next.rigs[sel];
            r.yaw = -r.yaw;
            r.roll = -r.roll;
            r.torsoSide = -r.torsoSide;
            r.torsoTwist = -r.torsoTwist;
            r.headYaw = -r.headYaw;
            r.headRoll = -r.headRoll;
            std::swap(r.leftArm, r.rightArm);
            std::swap(r.leftLeg, r.rightLeg);
            std::swap(r.leftHand, r.rightHand);
            next.rigs[sel] = r;
            next.pose.people[sel] = pose::solveRig(r, next.pose.canvas);
        }
        setState(next, true, tr("Mirror"));
    });
    connect(&m_undo, &QUndoStack::canUndoChanged, undo, &QToolButton::setEnabled);
    connect(&m_undo, &QUndoStack::canRedoChanged, redo, &QToolButton::setEnabled);
    undo->setEnabled(false);
    redo->setEnabled(false);
    connect(undo, &QToolButton::clicked, &m_undo, &QUndoStack::undo);
    connect(redo, &QToolButton::clicked, &m_undo, &QUndoStack::redo);
    connect(hands, &QCheckBox::toggled, m_canvas, &PoseCanvas::setShowHands);
    connect(face, &QCheckBox::toggled, m_canvas, &PoseCanvas::setShowFace);
    connect(opacity, &SliderField::valueChanged, this, [this](double v) { m_canvas->setUnderlayOpacity(v / 100.0); });
    connect(underlay, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Reference image"), QString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
        if (f.isEmpty())
            return;
        QImageReader reader(f);
        reader.setAutoTransform(true);
        m_canvas->setUnderlay(reader.read());
    });
    connect(detect, &QPushButton::clicked, this, &PosePage::detectFromImage);
    connect(savePreset, &QPushButton::clicked, this, &PosePage::savePreset);
    connect(m_describeBtn, &QPushButton::clicked, this, &PosePage::describePose);
    connect(m_describe, &QLineEdit::returnPressed, this, &PosePage::describePose);
    connect(toGen, &QPushButton::clicked, this, [this] { m_studio->usePoseForGeneration(m_state.pose); });
    connect(toDoc, &QPushButton::clicked, this, [this] {
        doc::Document *doc = m_studio->activeDocument();
        if (!doc) {
            m_studio->notify(tr("Open a document in the editor first."), QStringLiteral("warning"));
            return;
        }
        pose::Pose p = m_state.pose;
        p.resizeCanvas(doc->size(), true);
        doc->setControlPose(p);
        m_studio->notify(tr("Pose set as the control pose of '%1'.").arg(doc->name()), QStringLiteral("success"));
    });
    connect(m_presets, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        const bool user = item->data(Qt::UserRole + 1).toBool();
        if (user) {
            for (const pose::PosePreset &p : pose::loadUserPresets()) {
                if (p.id == id)
                    applyPreset(p, false);
            }
        } else if (auto p = pose::findBuiltInPreset(id)) {
            applyPreset(*p, false);
        }
    });
    connect(m_presets, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_presets->itemAt(pos);
        if (!item)
            return;
        const QString id = item->data(Qt::UserRole).toString();
        const bool user = item->data(Qt::UserRole + 1).toBool();
        QMenu menu(this);
        menu.addAction(tr("Add as new person"), this, [this, id, user] {
            if (!user) {
                if (auto p = pose::findBuiltInPreset(id))
                    applyPreset(*p, true);
            }
        })->setEnabled(!user);
        if (user)
            menu.addAction(Icons::get(QStringLiteral("trash")), tr("Delete preset"), this, [this, id] {
                pose::deleteUserPreset(id);
                reloadPresets();
            });
        menu.exec(m_presets->viewport()->mapToGlobal(pos));
    });
    connect(&m_rigCommit, &QTimer::timeout, this, [this] {
        if (m_rigEditing)
            m_undo.push(new PoseStateCommand(this, m_rigEditStart, m_state, tr("Adjust pose")));
        m_rigEditing = false;
    });
    m_rigCommit.setSingleShot(true);
    m_rigCommit.setInterval(500);
    connect(studio, &Studio::poseEditRequested, this, [this](const pose::Pose &p, const QString &) {
        State s;
        s.pose = p;
        for (int i = 0; i < p.people.size(); ++i)
            s.rigs.append(std::nullopt);
        setState(s, true, tr("Load pose"));
        m_studio->showPage(Page::Poses);
    });

    reloadPresets();
    State initial;
    const pose::PosePreset preset = *pose::findBuiltInPreset(QStringLiteral("a_pose"));
    initial.pose = preset.toPose(ai::Director::sizeForAspect(QStringLiteral("2:3"), 1024 * 1024, 64));
    initial.rigs = {preset.rigs.first()};
    setState(initial, false);
}

void PosePage::buildRigPanel(QVBoxLayout *layout)
{
    m_rigNote = mutedLabel(QString(), m_rigPanel);
    layout->addWidget(m_rigNote);
    m_convertToRig = new QPushButton(tr("Rebuild as editable rig"), m_rigPanel);
    m_convertToRig->setToolTip(tr("Replace the free-form keypoints of this person with an adjustable rig (resets the pose to standing)."));
    layout->addWidget(m_convertToRig);
    connect(m_convertToRig, &QPushButton::clicked, this, [this] {
        const int sel = m_canvas->selectedPerson();
        if (sel < 0 || sel >= m_state.pose.people.size())
            return;
        State next = m_state;
        pose::RigParams r;
        r.positionX = std::clamp(next.pose.people[sel].bounds().center().x() / next.pose.canvas.width(), 0.0, 1.0);
        next.rigs[sel] = r;
        next.pose.people[sel] = pose::solveRig(r, next.pose.canvas);
        setState(next, true, tr("Convert to rig"));
    });

    auto *combos = new QGroupBox(tr("Hands, face && framing"), m_rigPanel);
    auto *cl = new QGridLayout(combos);
    m_leftHand = new QComboBox(combos);
    m_rightHand = new QComboBox(combos);
    QStringList hands = pose::handShapeNames();
    hands << QStringLiteral("none");
    m_leftHand->addItems(hands);
    m_rightHand->addItems(hands);
    m_expression = new QComboBox(combos);
    m_expression->addItems(pose::expressionNames());
    m_framing = new QComboBox(combos);
    m_framing->addItems(pose::framingNames());
    cl->addWidget(new QLabel(tr("Left hand"), combos), 0, 0);
    cl->addWidget(m_leftHand, 0, 1);
    cl->addWidget(new QLabel(tr("Right hand"), combos), 1, 0);
    cl->addWidget(m_rightHand, 1, 1);
    cl->addWidget(new QLabel(tr("Expression"), combos), 2, 0);
    cl->addWidget(m_expression, 2, 1);
    cl->addWidget(new QLabel(tr("Framing"), combos), 3, 0);
    cl->addWidget(m_framing, 3, 1);
    layout->addWidget(combos);
    for (QComboBox *c : {m_leftHand, m_rightHand, m_expression, m_framing})
        connect(c, &QComboBox::currentTextChanged, this, &PosePage::rigChanged);

    QHash<QString, QGroupBox *> groups;
    for (const RigField &f : rigFields()) {
        QGroupBox *&box = groups[f.group];
        if (!box) {
            box = new QGroupBox(QString(f.group).replace(u'&', QStringLiteral("&&")), m_rigPanel);
            box->setLayout(new QVBoxLayout);
            layout->addWidget(box);
        }
        const int decimals = f.max - f.min <= 4 ? 2 : 0;
        auto *slider = new SliderField(f.label, f.min, f.max, 0, decimals, decimals ? QString() : QStringLiteral("°"), box);
        slider->setCompact(true);
        box->layout()->addWidget(slider);
        m_rigSliders.insert(f.key, slider);
        connect(slider, &SliderField::valueChanged, this, &PosePage::rigChanged);
    }
    layout->addStretch();
}

std::optional<pose::RigParams> PosePage::currentRig() const
{
    const int sel = m_canvas->selectedPerson();
    if (sel < 0 || sel >= m_state.rigs.size())
        return std::nullopt;
    return m_state.rigs[sel];
}

void PosePage::syncRigPanel()
{
    m_syncing = true;
    const auto rig = currentRig();
    const bool hasRig = rig.has_value();
    for (SliderField *s : std::as_const(m_rigSliders))
        s->setEnabled(hasRig);
    for (QComboBox *c : {m_leftHand, m_rightHand, m_expression, m_framing})
        c->setEnabled(hasRig);
    m_convertToRig->setVisible(!hasRig && !m_state.pose.people.isEmpty());
    m_rigNote->setText(hasRig ? tr("Adjust the 3D rig of the selected person. Dragging joints switches the person to free-form editing.")
                              : tr("This person uses free-form keypoints (edited, detected or imported). Drag joints on the canvas."));
    if (hasRig) {
        pose::RigParams r = *rig;
        for (const RigField &f : rigFields()) {
            if (SliderField *s = m_rigSliders.value(f.key))
                s->setValue(f.ref(r));
        }
        m_leftHand->setCurrentText(r.leftHand);
        m_rightHand->setCurrentText(r.rightHand);
        m_expression->setCurrentText(r.expression);
        m_framing->setCurrentText(r.framing);
    }
    m_syncing = false;
}

void PosePage::rigChanged()
{
    if (m_syncing)
        return;
    const int sel = m_canvas->selectedPerson();
    if (sel < 0 || sel >= m_state.rigs.size() || !m_state.rigs[sel])
        return;
    if (!m_rigEditing) {
        m_rigEditStart = m_state;
        m_rigEditing = true;
    }
    pose::RigParams r = *m_state.rigs[sel];
    for (const RigField &f : rigFields()) {
        if (SliderField *s = m_rigSliders.value(f.key))
            f.ref(r) = s->value();
    }
    r.leftHand = m_leftHand->currentText();
    r.rightHand = m_rightHand->currentText();
    r.expression = m_expression->currentText();
    r.framing = m_framing->currentText();
    m_state.rigs[sel] = r;
    m_state.pose.people[sel] = pose::solveRig(r, m_state.pose.canvas);
    m_canvas->setPose(m_state.pose);
    m_rigCommit.start();
}

void PosePage::setState(const State &state, bool undoable, const QString &text)
{
    if (undoable)
        m_undo.push(new PoseStateCommand(this, m_state, state, text));
    m_state = state;
    while (m_state.rigs.size() < m_state.pose.people.size())
        m_state.rigs.append(std::nullopt);
    const int sel = std::clamp(m_canvas->selectedPerson(), 0, std::max(0, int(m_state.pose.people.size()) - 1));
    m_canvas->setPose(m_state.pose);
    {
        QSignalBlocker b(m_people);
        m_people->clear();
        for (int i = 0; i < m_state.pose.people.size(); ++i)
            m_people->addItem(tr("Person %1").arg(i + 1));
        m_people->setCurrentIndex(sel);
    }
    {
        // Reflect the canvas aspect in the combo without triggering a resize.
        QSignalBlocker b(m_aspect);
        const QSize c = m_state.pose.canvas;
        for (const QString &a : kAspects) {
            const QSize s = ai::Director::sizeForAspect(a, qint64(c.width()) * c.height(), 8);
            if (std::abs(double(s.width()) / s.height() - double(c.width()) / c.height()) < 0.02) {
                m_aspect->setCurrentText(a);
                break;
            }
        }
    }
    m_canvas->setSelectedPerson(sel);
    syncRigPanel();
}

void PosePage::applyPreset(const pose::PosePreset &preset, bool addAsPerson)
{
    State next = m_state;
    const QSize canvas = next.pose.canvas;
    if (preset.rigs.size() > 1 || preset.pose) {
        // Multi-person and captured presets replace the whole pose.
        next.pose = preset.toPose(canvas);
        next.rigs.clear();
        for (int i = 0; i < next.pose.people.size(); ++i)
            next.rigs.append(preset.pose ? std::nullopt : std::optional<pose::RigParams>(preset.rigs.value(i)));
        setState(next, true, tr("Apply preset %1").arg(preset.name));
        return;
    }
    pose::RigParams r = preset.rigs.first();
    const int sel = m_canvas->selectedPerson();
    if (addAsPerson || next.pose.people.isEmpty()) {
        r.positionX = next.pose.people.isEmpty() ? 0.5 : std::min(0.85, 0.3 + 0.2 * next.pose.people.size());
        r.scale = next.pose.people.isEmpty() ? 1.0 : 0.8;
        next.pose.people.append(pose::solveRig(r, canvas));
        next.rigs.append(r);
        setState(next, true, tr("Add person"));
        m_canvas->setSelectedPerson(int(next.pose.people.size()) - 1);
        return;
    }
    // Keep the placement of the person being replaced.
    if (sel >= 0 && sel < next.rigs.size() && next.rigs[sel]) {
        r.positionX = next.rigs[sel]->positionX;
        r.positionY = next.rigs[sel]->positionY;
        r.scale = next.rigs[sel]->scale;
    }
    next.pose.people[sel] = pose::solveRig(r, canvas);
    next.rigs[sel] = r;
    setState(next, true, tr("Apply preset %1").arg(preset.name));
}

void PosePage::reloadPresets()
{
    m_presets->clear();
    struct Entry
    {
        QString id, name, category;
        pose::Pose pose;
        bool user;
    };
    QList<Entry> entries;
    for (const pose::PosePreset &p : pose::builtInPresets()) {
        const QSize canvas = p.category == QLatin1String("Sheets") ? QSize(768, 512) : QSize(384, 512);
        entries.append({p.id, p.name, p.category, p.toPose(canvas), false});
    }
    for (const pose::PosePreset &p : pose::loadUserPresets())
        entries.append({p.id, p.name, tr("My presets"), p.toPose(QSize(384, 512)), true});
    for (const Entry &e : entries) {
        auto *item = new QListWidgetItem(e.name, m_presets);
        item->setData(Qt::UserRole, e.id);
        item->setData(Qt::UserRole + 1, e.user);
        item->setToolTip(QStringLiteral("%1 — %2").arg(e.category, e.name));
    }
    // Thumbnails are rendered off the GUI thread.
    QPointer<PosePage> self(this);
    async::run([entries] {
        QList<QImage> thumbs;
        for (const Entry &e : entries) {
            pose::RenderOptions opts;
            opts.thickness = 1.4;
            thumbs.append(pose::renderOpenPose(e.pose, e.pose.canvas, opts).scaled(168, 224, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        return thumbs;
    }).then(this, [self](QList<QImage> thumbs) {
        if (!self)
            return;
        for (int i = 0; i < thumbs.size() && i < self->m_presets->count(); ++i)
            self->m_presets->item(i)->setIcon(QIcon(QPixmap::fromImage(thumbs[i])));
    });
}

void PosePage::setAspect(const QString &aspect)
{
    const QSize canvas = ai::Director::sizeForAspect(aspect, 1024 * 1024, 64);
    State next = m_state;
    next.pose.resizeCanvas(canvas, true);
    for (int i = 0; i < next.rigs.size(); ++i) {
        if (next.rigs[i])
            next.pose.people[i] = pose::solveRig(*next.rigs[i], canvas);
    }
    setState(next, true, tr("Canvas size"));
}

void PosePage::describePose()
{
    const QString text = m_describe->text().trimmed();
    if (text.isEmpty())
        return;
    const int sel = m_canvas->selectedPerson();
    if (sel < 0 || sel >= m_state.pose.people.size())
        return;
    const pose::RigParams base = m_state.rigs.value(sel).value_or(pose::RigParams());
    QJsonObject shot{{QStringLiteral("description"), text}, {QStringLiteral("pose"), base.toJson()}};
    ai::StructuredChatOperation *op = m_studio->director()->revisePose(shot, text);
    m_describeBtn->setEnabled(false);
    m_describe->setEnabled(false);
    QPointer<PosePage> self(this);
    connect(op, &ai::Operation::finished, this, [self, op, sel, base] {
        if (!self)
            return;
        self->m_describeBtn->setEnabled(true);
        self->m_describe->setEnabled(true);
        if (op->state() != ai::Operation::State::Succeeded) {
            if (op->state() == ai::Operation::State::Failed)
                self->m_studio->notify(op->errorString(), QStringLiteral("error"));
            return;
        }
        pose::RigParams r = pose::RigParams::fromJson(op->result(), base);
        r.includeHands = base.includeHands;
        r.includeFace = base.includeFace;
        State next = self->m_state;
        if (sel >= next.pose.people.size())
            return;
        next.rigs[sel] = r;
        next.pose.people[sel] = pose::solveRig(r, next.pose.canvas);
        self->setState(next, true, tr("Describe pose"));
    });
}

void PosePage::detectFromImage()
{
    const QString f = QFileDialog::getOpenFileName(this, tr("Detect pose from image"), QString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (f.isEmpty())
        return;
    QImageReader reader(f);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull())
        return;
    m_canvas->setUnderlay(image);
    Job *job = m_studio->generation()->submitUtility(QString(), ai::Utility::DetectPose, img::flatten(image, Qt::white), {}, tr("Detect pose"), false);
    m_studio->notify(tr("Detecting pose…"));
    QPointer<PosePage> self(this);
    const QSize imgSize = image.size();
    connect(job, &Job::finished, this, [self, imgSize](Job *j) {
        if (!self)
            return;
        if (j->state() != Job::State::Succeeded) {
            if (j->state() == Job::State::Failed)
                self->m_studio->notify(j->errorString(), QStringLiteral("error"));
            return;
        }
        const ai::GenerationOutput out = ai::GenerationService::outputOf(j);
        auto p = pose::Pose::fromOpenPoseJson(out.info.value(QStringLiteral("openpose")).toObject(), nullptr, imgSize);
        if (!p || p->isEmpty()) {
            self->m_studio->notify(tr("The detector returned no keypoints (only an image). Use a provider with DWPose/OpenPose JSON output."),
                                   QStringLiteral("warning"));
            return;
        }
        State s;
        s.pose = *p;
        for (int i = 0; i < p->people.size(); ++i)
            s.rigs.append(std::nullopt);
        self->setState(s, true, tr("Detect pose"));
        self->m_studio->notify(tr("Detected %n person(s).", nullptr, int(p->people.size())), QStringLiteral("success"));
    });
}

void PosePage::savePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save pose preset"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    pose::PosePreset preset;
    preset.name = name.trimmed();
    preset.category = tr("My presets");
    const bool allRig = std::all_of(m_state.rigs.begin(), m_state.rigs.end(), [](const auto &r) { return r.has_value(); });
    if (allRig) {
        for (const auto &r : m_state.rigs)
            preset.rigs.append(*r);
    } else {
        preset.pose = m_state.pose;
    }
    QString error;
    if (!pose::saveUserPreset(preset, &error))
        m_studio->notify(tr("Could not save the preset: %1").arg(error), QStringLiteral("error"));
    else
        reloadPresets();
}

void PosePage::exportPng()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export pose map"), QStringLiteral("pose.png"), tr("PNG (*.png)"));
    if (path.isEmpty())
        return;
    if (!pose::renderOpenPose(m_state.pose, m_state.pose.canvas).save(path))
        m_studio->notify(tr("Could not write %1").arg(path), QStringLiteral("error"));
}

void PosePage::exportJson()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export OpenPose JSON"), QStringLiteral("pose.json"), tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(m_state.pose.toOpenPoseJson()).toJson(QJsonDocument::Indented));
}

void PosePage::importJson()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import OpenPose JSON"), QString(), tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QJsonObject obj = doc.isArray() ? doc.array().at(0).toObject() : doc.object();
    QString error;
    auto p = pose::Pose::fromOpenPoseJson(obj, &error, m_state.pose.canvas);
    if (!p) {
        m_studio->notify(tr("Not a valid OpenPose file: %1").arg(error), QStringLiteral("error"));
        return;
    }
    State s;
    s.pose = *p;
    for (int i = 0; i < p->people.size(); ++i)
        s.rigs.append(std::nullopt);
    setState(s, true, tr("Import pose"));
}

} // namespace lumen::ui
