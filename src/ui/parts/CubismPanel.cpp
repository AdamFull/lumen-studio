#include "ui/parts/CubismPanel.h"

#include "core/Settings.h"
#include "live2d/CubismCore.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/parts/CubismView.h"
#include "ui/widgets/Controls.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {

const QString kCoreSetting = QStringLiteral("live2d/coreLibrary");
const QString kModelSetting = QStringLiteral("live2d/lastModel");

} // namespace

CubismPanel::CubismPanel(Studio *studio, CubismView *view, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
    , m_view(view)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 8, 0, 0);
    v->setSpacing(8);
    m_status = mutedLabel(QString(), this);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    v->addWidget(m_status);
    auto *row = new QHBoxLayout;
    m_locate = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Locate Cubism Core..."), this);
    m_locate->setToolTip(tr("Choose %1 from the Core folder of the Live2D Cubism SDK for Native").arg(live2d::CubismCore::libraryFileName()));
    m_open = new QPushButton(Icons::get(QStringLiteral("person")), tr("Open model..."), this);
    m_open->setToolTip(tr("Open a .model3.json exported from Cubism Editor"));
    row->addWidget(m_locate);
    row->addWidget(m_open);
    v->addLayout(row);

    auto *toggles = new QHBoxLayout;
    m_idle = new QCheckBox(tr("Idle"), this);
    m_idle->setChecked(true);
    m_motion = new QCheckBox(tr("Idle motion"), this);
    m_motion->setChecked(true);
    m_motion->setToolTip(tr("Play the model's Idle motion group"));
    m_follow = new QCheckBox(tr("Follow mouse"), this);
    m_talk = new QCheckBox(tr("Talk"), this);
    toggles->addWidget(m_idle);
    toggles->addWidget(m_motion);
    toggles->addWidget(m_follow);
    toggles->addWidget(m_talk);
    toggles->addStretch();
    v->addLayout(toggles);

    auto *paramHeader = new QHBoxLayout;
    paramHeader->addWidget(sectionLabel(tr("Parameters"), this));
    paramHeader->addStretch();
    m_reset = new QPushButton(tr("Reset"), this);
    m_reset->setToolTip(tr("Release every parameter you moved back to the animation"));
    paramHeader->addWidget(m_reset);
    v->addLayout(paramHeader);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_paramHost = new QWidget(scroll);
    m_params = new QVBoxLayout(m_paramHost);
    m_params->setContentsMargins(0, 0, 6, 0);
    m_params->setSpacing(2);
    m_params->addStretch();
    scroll->setWidget(m_paramHost);
    v->addWidget(scroll, 1);
    auto *note = mutedLabel(tr("Physics are not simulated. Live2D Cubism Core is provided by Live2D Inc. under its own licence; "
                               "Lumen only loads your copy."), this);
    note->setWordWrap(true);
    v->addWidget(note);

    m_sync.setInterval(200);
    connect(&m_sync, &QTimer::timeout, this, &CubismPanel::syncSliders);
    connect(m_locate, &QPushButton::clicked, this, &CubismPanel::locateCore);
    connect(m_open, &QPushButton::clicked, this, &CubismPanel::chooseModel);
    connect(m_idle, &QCheckBox::toggled, m_view, &CubismView::setAutoIdle);
    connect(m_motion, &QCheckBox::toggled, m_view, &CubismView::setPlayMotion);
    connect(m_follow, &QCheckBox::toggled, m_view, &CubismView::setFollowMouse);
    connect(m_talk, &QCheckBox::toggled, m_view, &CubismView::setTalking);
    connect(m_reset, &QPushButton::clicked, m_view, &CubismView::clearManual);
    connect(m_view, &CubismView::modelChanged, this, [this] {
        rebuildParameters();
        refreshStatus();
    });
    refreshStatus();
}

void CubismPanel::activate()
{
    if (!m_activated) {
        m_activated = true;
        live2d::CubismCore::instance().loadDefault();
        const QString last = Settings::instance().value(kModelSetting).toString();
        if (live2d::CubismCore::instance().isLoaded() && !m_view->hasModel() && QFileInfo::exists(last))
            openModel(last);
        refreshStatus();
    }
    m_sync.start();
}

void CubismPanel::refreshStatus()
{
    live2d::CubismCore &core = live2d::CubismCore::instance();
    QString text;
    if (!core.isLoaded()) {
        text = tr("<b>Cubism Core not found.</b> Download the <i>Cubism SDK for Native</i> from live2d.com, accept its "
                  "licence, then locate %1 in its Core folder.").arg(live2d::CubismCore::libraryFileName());
    } else if (!m_view->hasModel()) {
        text = tr("Cubism Core %1 loaded. Open a .model3.json to preview a rigged model.").arg(core.versionString());
    } else {
        live2d::CubismModel *m = m_view->model();
        text = tr("<b>%1</b> · %n parameter(s) · Core %2", nullptr, m->parameterCount()).arg(m_view->modelName().toHtmlEscaped(), core.versionString());
    }
    m_status->setText(text);
    m_open->setEnabled(core.isLoaded());
    m_motion->setEnabled(m_view->hasIdleMotion());
    m_reset->setEnabled(m_view->hasModel());
}

void CubismPanel::locateCore()
{
    const QString start = QFileInfo(Settings::instance().value(kCoreSetting).toString()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Locate Live2D Cubism Core"), start,
                                                      tr("Cubism Core (%1);;Libraries (*.dll *.so *.dylib)").arg(live2d::CubismCore::libraryFileName()));
    if (path.isEmpty())
        return;
    QString error;
    if (!live2d::CubismCore::instance().load(path, &error)) {
        m_studio->notify(error, QStringLiteral("error"));
        return;
    }
    Settings::instance().setValue(kCoreSetting, path);
    m_studio->notify(tr("Cubism Core %1 loaded.").arg(live2d::CubismCore::instance().versionString()), QStringLiteral("success"));
    refreshStatus();
}

void CubismPanel::chooseModel()
{
    const QString start = QFileInfo(Settings::instance().value(kModelSetting).toString()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Cubism model"), start, tr("Cubism model (*.model3.json)"));
    if (!path.isEmpty())
        openModel(path);
}

bool CubismPanel::openModel(const QString &path)
{
    QString error;
    if (!m_view->loadModel(path, &error)) {
        m_studio->notify(error, QStringLiteral("error"));
        return false;
    }
    Settings::instance().setValue(kModelSetting, path);
    m_view->setAutoIdle(m_idle->isChecked());
    m_view->setPlayMotion(m_motion->isChecked());
    m_view->setFollowMouse(m_follow->isChecked());
    m_view->setTalking(m_talk->isChecked());
    return true;
}

void CubismPanel::rebuildParameters()
{
    for (const QPointer<SliderField> &s : std::as_const(m_sliders)) {
        if (s)
            s->deleteLater();
    }
    m_sliders.clear();
    live2d::CubismModel *model = m_view->model();
    if (!model)
        return;
    for (int i = 0; i < model->parameterCount(); ++i) {
        auto *slider = new SliderField(model->parameterIds().at(i), model->parameterMinimum(i), model->parameterMaximum(i),
                                       model->parameterDefault(i), 2, QString(), m_paramHost);
        slider->setCompact(true);
        m_params->insertWidget(m_params->count() - 1, slider);
        m_sliders.append(slider);
        connect(slider, &SliderField::valueChanged, this, [this, i](double value) {
            if (!m_syncing)
                m_view->setManual(i, float(value));
        });
    }
}

void CubismPanel::syncSliders()
{
    if (!isVisible() || !m_view->hasModel()) {
        if (!isVisible())
            m_sync.stop();
        return;
    }
    m_syncing = true;
    for (int i = 0; i < m_sliders.size(); ++i) {
        if (m_sliders[i] && !m_sliders[i]->underMouse())
            m_sliders[i]->setValue(m_view->value(i));
    }
    m_syncing = false;
}

} // namespace lumen::ui
