#include "ui/dialogs/SetupWizard.h"

#include "ai/GenerationService.h"
#include "core/Error.h"
#include "core/Job.h"
#include "runtime/ModelLibrary.h"
#include "runtime/RuntimeStore.h"
#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/LocalSetupDialog.h"
#include "ui/ext/Extension.h"
#include "ui/widgets/Controls.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lumen::ui {

namespace {

const QString kSd = QStringLiteral("sd.cpp");
const QString kLlama = QStringLiteral("llama.cpp");

// An online service of the second choice.
struct Service
{
    QString typeId;
    QString name;
    QString description;
    QString keyPage;
};

QList<Service> services()
{
    return {
        {QStringLiteral("fal"), QStringLiteral("fal.ai"),
         SetupWizard::tr("FLUX models: new images, changes from a description (FLUX Kontext) and filling a selected area."),
         QStringLiteral("https://fal.ai/dashboard/keys")},
        {QStringLiteral("openai_image"), QStringLiteral("OpenAI"),
         SetupWizard::tr("GPT Image: new images and changes from a description, with or without a selection."),
         QStringLiteral("https://platform.openai.com/api-keys")},
        {QStringLiteral("gemini_image"), QStringLiteral("Google Gemini"),
         SetupWizard::tr("Gemini image models: new images and changes from a description."),
         QStringLiteral("https://aistudio.google.com/apikey")},
    };
}

QString serviceName(const QString &typeId)
{
    for (const Service &s : services()) {
        if (s.typeId == typeId)
            return s.name;
    }
    return typeId;
}

// "about 20 minutes at 100 Mbit/s": how long a download takes on a fast home connection.
QString downloadTime(qint64 bytes)
{
    const int minutes = std::max(1, int(std::ceil(double(bytes) * 8 / 100e6 / 60)));
    return SetupWizard::tr("about %n minute(s) at 100 Mbit/s", nullptr, minutes);
}

// What a model can do and how fast it is on this computer, in plain words.
QString modelSummary(const runtime::ModelPreset &p, runtime::LocalRecommendation::Fit fit)
{
    using Fit = runtime::LocalRecommendation::Fit;
    QString what = p.edit ? SetupWizard::tr("Makes new images and changes them from a description: colorize a photo, remove or replace "
                                            "an object, keep everything else.")
                          : SetupWizard::tr("Makes new images and repaints a selected area. It follows edit instructions such as "
                                            "\"colorize this\" less well than FLUX.2 [klein], which needs an 8 GB graphics card.");
    const bool flux = p.familyFile == QLatin1String("flux2");
    QString speed;
    if (fit == Fit::NoGpu)
        speed = SetupWizard::tr("Without an NVIDIA or AMD graphics card it runs on the processor: several minutes per image.");
    else if (flux)
        speed = fit == Fit::Full ? SetupWizard::tr("About 20 to 40 seconds per image on an 8 GB graphics card.")
                                 : SetupWizard::tr("Your graphics card has less than 8 GB, so much of the model waits in the computer's "
                                                   "memory: a few minutes per image.");
    else
        speed = fit == Fit::Full ? SetupWizard::tr("About 30 to 60 seconds per image.")
                                 : SetupWizard::tr("About 30 to 60 seconds per image, longer on graphics cards with less than 6 GB.");
    return what + QLatin1Char(' ') + speed + QLatin1Char(' ')
           + SetupWizard::tr("The first image after a start takes a minute longer while the model loads.");
}

QString shortFolder(const QString &path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

QLabel *wrapped(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    return l;
}

// The provider a running server is already added as (same type and address), if any.
ai::Provider *providerAt(ai::ProviderRegistry *registry, const QString &typeId, const QUrl &url)
{
    const QString base = url.adjusted(QUrl::StripTrailingSlash).toString();
    for (ai::Provider *p : registry->providers()) {
        if (p->type().id == typeId && p->text(QStringLiteral("url")).startsWith(base))
            return p;
    }
    return nullptr;
}

QPointer<SetupWizard> g_open; // openSetupWizard: one at a time
QList<SetupWizard *> g_wizards; // every wizard, for the setup they show
QPointer<LocalSetup> g_running;  // the local setup that is downloading, also after its wizard closed

bool shownByAWizard(const LocalSetup *setup)
{
    for (const SetupWizard *w : std::as_const(g_wizards)) {
        if (w->localSetup() == setup)
            return true;
    }
    return false;
}

} // namespace

SetupWizard::SetupWizard(Studio *studio, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
{
    g_wizards << this;
    hardwareOverride = testHardware;
    searchFolders = testSearchFolders;
    // A setup that still downloads (its wizard was closed) is shown again rather than started twice.
    const bool resumed = g_running && g_running->isRunning() && g_running->studio() == studio;
    m_setup = resumed ? g_running.data() : new LocalSetup(studio, studio); // outlives the wizard while downloads run
    setWindowTitle(tr("Set up AI"));
    resize(780, 600);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(20, 16, 20, 16);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildChoosePage());
    m_pages->addWidget(buildLocalPage());
    m_pages->addWidget(buildOnlinePage());
    m_pages->addWidget(buildServerPage());
    m_pages->addWidget(buildResultPage());
    v->addWidget(m_pages, 1);

    connect(m_setup, &LocalSetup::jobsChanged, this, &SetupWizard::showLocalProgress);
    connect(m_setup, &LocalSetup::finished, this, &SetupWizard::localFinished);
    connect(m_setup, &LocalSetup::runtimesChanged, this, [this] {
        if (m_hardwareReady && m_foldersReady)
            refreshPlan();
    });
    if (!resumed) {
        // The setup object goes once it is done and no wizard shows it; then a message says how it went.
        connect(m_setup, &LocalSetup::finished, m_setup, [studio, setup = m_setup](bool ok, const QString &message) {
            if (!setup || shownByAWizard(setup))
                return;
            studio->notify(ok ? tr("Local AI is set up. %1").arg(message) : tr("The local AI setup did not finish: %1").arg(message),
                           ok ? QStringLiteral("success") : QStringLiteral("error"));
            setup->deleteLater();
        });
    } else {
        m_localStack->setCurrentIndex(1);
        showLocalProgress();
    }
    // New library files (a scan that was still running) can change the plan.
    connect(m_studio->models(), &runtime::ModelLibrary::scanFinished, this, [this] {
        if (m_hardwareReady && m_foldersReady && m_localStack->currentIndex() == 0)
            refreshPlan();
    });
}

SetupWizard::~SetupWizard()
{
    g_wizards.removeAll(this);
    if (m_setup && !m_setup->isRunning() && !shownByAWizard(m_setup))
        m_setup->deleteLater();
}

LocalSetup *SetupWizard::localSetup() const
{
    return m_setup;
}

void SetupWizard::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (!std::exchange(m_started, true)) {
        startLooking();
        detectServers();
    }
}

void SetupWizard::showPage(Page page)
{
    if (page != Page::Result)
        m_backTo = page;
    if (page == Page::Local)
        lookForFiles();
    m_pages->setCurrentIndex(int(page));
}

SetupWizard::Page SetupWizard::currentPage() const
{
    return Page(m_pages->currentIndex());
}

QPushButton *SetupWizard::backButton(QWidget *parent)
{
    auto *back = new QPushButton(Icons::get(QStringLiteral("chevron-left")), tr("Back"), parent);
    connect(back, &QPushButton::clicked, this, [this] { showPage(Page::Choose); });
    return back;
}

// ---- Choose -----------------------------------------------------------------------------------

QWidget *SetupWizard::buildChoosePage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    auto *title = new QLabel(tr("Set up AI"), page);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(tr("Lumen changes and creates images from what you describe. For that it needs an image engine: "
                               "choose where it runs. You can change this later in Settings → AI."),
                            page));

    auto option = [this, page, v](const QString &icon, const QString &name, const QString &text, const QString &button, Page target,
                                  QLabel **hint) {
        auto *box = card(page);
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(14, 12, 14, 12);
        h->setSpacing(12);
        auto *pic = new QLabel(box);
        pic->setPixmap(Icons::pixmap(icon, 28, Theme::palette().accent));
        pic->setAlignment(Qt::AlignTop);
        h->addWidget(pic);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(4);
        auto *heading = new QLabel(name, box);
        Theme::setRole(heading, QStringLiteral("heading"));
        texts->addWidget(heading);
        texts->addWidget(mutedLabel(text, box));
        if (hint) {
            *hint = wrapped(QString(), box);
            texts->addWidget(*hint);
        }
        h->addLayout(texts, 1);
        auto *go = new QPushButton(button, box);
        go->setObjectName(QStringLiteral("choose-%1").arg(int(target)));
        Theme::setRole(go, QStringLiteral("primary"));
        h->addWidget(go, 0, Qt::AlignVCenter);
        connect(go, &QPushButton::clicked, this, [this, target] { showPage(target); });
        v->addWidget(box);
    };
    option(QStringLiteral("sparkles"), tr("Run on this computer"),
           tr("Free and private: the model runs on your graphics card and your images stay here. Lumen downloads the program "
              "and one model once (7 to 13 GB, 10 to 20 minutes on a fast connection) and sets everything up; after that an "
              "image takes under a minute."),
           tr("Set up here…"), Page::Local, &m_localHint);
    option(QStringLiteral("cloud"), tr("Use an online service"),
           tr("fal.ai, OpenAI or Google Gemini. You need an account there and paste its key; the service charges a few cents per "
              "image. Ready in two minutes, nothing to download."),
           tr("Choose a service…"), Page::Online, nullptr);
    option(QStringLiteral("server"), tr("Connect a server that is already running"),
           tr("ComfyUI, Forge / Stable Diffusion WebUI or stable-diffusion.cpp's sd-server, on this computer or another one. "
              "Lumen uses the models it has; nothing is downloaded."),
           tr("Connect…"), Page::Server, &m_serverHint);
    m_localHint->setText(tr("Checking this computer…"));
    m_serverHint->setText(tr("Looking for servers…"));
    v->addStretch();

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(mutedLabel(tr("Not now? Help → Set up AI… opens this again."), page), 1);
    auto *skip = new QPushButton(tr("Skip for now"), page);
    connect(skip, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(skip);
    v->addLayout(buttons);
    return page;
}

// ---- Run on this computer -----------------------------------------------------------------------

QWidget *SetupWizard::buildLocalPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    m_localStack = new QStackedWidget(page);
    outer->addWidget(m_localStack, 1);

    // The plan.
    auto *planPage = new QWidget(m_localStack);
    auto *v = new QVBoxLayout(planPage);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    auto *title = new QLabel(tr("Run on this computer"), planPage);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    m_hardware = mutedLabel(tr("Checking this computer and looking for models you already have…"), planPage);
    v->addWidget(m_hardware);
    auto *adviceRow = new QHBoxLayout;
    m_advice = wrapped(QString(), planPage);
    Theme::setRole(m_advice, QStringLiteral("error"));
    m_advice->setVisible(false);
    m_adviceOnline = new QPushButton(Icons::get(QStringLiteral("cloud")), tr("Use an online service"), planPage);
    m_adviceOnline->setVisible(false);
    connect(m_adviceOnline, &QPushButton::clicked, this, [this] { showPage(Page::Online); });
    adviceRow->addWidget(m_advice, 1);
    adviceRow->addWidget(m_adviceOnline, 0, Qt::AlignVCenter);
    v->addLayout(adviceRow);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_model = new QComboBox(planPage);
    m_model->setObjectName(QStringLiteral("setupModel"));
    m_model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_model->setMinimumContentsLength(20);
    form->addRow(tr("Model"), m_model);
    v->addLayout(form);
    m_modelInfo = mutedLabel(QString(), planPage);
    v->addWidget(m_modelInfo);
    m_language = new QCheckBox(planPage);
    m_language->setObjectName(QStringLiteral("setupLanguage"));
    m_language->setVisible(false);
    v->addWidget(m_language);
    m_languageInfo = mutedLabel(QString(), planPage);
    m_languageInfo->setContentsMargins(24, 0, 0, 0);
    m_languageInfo->setVisible(false);
    v->addWidget(m_languageInfo);
    m_planTree = new QTreeWidget(planPage);
    m_planTree->setObjectName(QStringLiteral("setupPlan"));
    m_planTree->setHeaderLabels({tr("What"), tr("Size"), tr("From"), tr("Licence")});
    m_planTree->setRootIsDecorated(false);
    m_planTree->setUniformRowHeights(true);
    m_planTree->header()->setStretchLastSection(true);
    v->addWidget(m_planTree, 1);
    m_summary = wrapped(QString(), planPage);
    m_summary->setObjectName(QStringLiteral("setupSummary"));
    m_summary->setTextFormat(Qt::RichText);
    v->addWidget(m_summary);
    // Where the files come from and go: under the plan, as they are about it.
    auto *folders = new QHBoxLayout;
    // Models in a folder Lumen does not look in (a portable ComfyUI, a second drive).
    auto *other = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Use models from a folder…"), planPage);
    other->setObjectName(QStringLiteral("setupUseFolder"));
    other->setToolTip(tr("Lumen looked in the model library, ComfyUI, WebUI, LM Studio and Downloads. Choose another folder with the "
                         "model files, such as a portable ComfyUI: they are used where they are."));
    connect(other, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder with model files"));
        if (!dir.isEmpty())
            useModelsFolder(dir);
    });
    folders->addWidget(other);
    auto *folder = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Open models folder"), planPage);
    folder->setToolTip(tr("Where Lumen saves models (Settings → Models can move it)"));
    connect(folder, &QPushButton::clicked, this, [this] {
        const QString dir = m_studio->models()->defaultFolder();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    folders->addWidget(folder);
    folders->addStretch();
    v->addLayout(folders);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(backButton(planPage));
    // The SDXL set with pose control and character references of the Characters plugin.
    if (!ext::Extensions::instance().needingLanguageModel().isEmpty()) {
        auto *characters = new QPushButton(tr("Model set for Characters (pose control)…"), planPage);
        characters->setToolTip(tr("Instead of this model: an SDXL model with pose control, character references, an upscaler and a "
                                  "language model, as the Characters plugin uses them (about 17 GB)"));
        connect(characters, &QPushButton::clicked, this, [this] {
            auto *dlg = new LocalSetupDialog(m_studio, this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
        });
        buttons->addWidget(characters);
    }
    buttons->addStretch();
    m_setUp = new QPushButton(Icons::get(QStringLiteral("download")), tr("Set up"), planPage);
    m_setUp->setObjectName(QStringLiteral("setupStart"));
    Theme::setRole(m_setUp, QStringLiteral("primary"));
    m_setUp->setEnabled(false);
    connect(m_setUp, &QPushButton::clicked, this, &SetupWizard::startLocal);
    buttons->addWidget(m_setUp);
    v->addLayout(buttons);
    m_localStack->addWidget(planPage);

    // Progress.
    auto *progress = new QWidget(m_localStack);
    auto *pv = new QVBoxLayout(progress);
    pv->setContentsMargins(0, 0, 0, 0);
    auto *ptitle = new QLabel(tr("Setting up…"), progress);
    Theme::setRole(ptitle, QStringLiteral("title"));
    pv->addWidget(ptitle);
    pv->addWidget(mutedLabel(tr("You can close this window: downloads continue in the Queue, and Lumen finishes the setup when "
                                "they are done. An interrupted download resumes where it stopped."),
                             progress));
    m_progress = new QTreeWidget(progress);
    m_progress->setHeaderLabels({tr("Step"), tr("Progress")});
    m_progress->setRootIsDecorated(false);
    pv->addWidget(m_progress, 1);
    m_progressStatus = mutedLabel(QString(), progress);
    pv->addWidget(m_progressStatus);
    auto *close = new QPushButton(tr("Close"), progress);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    auto *row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(close);
    pv->addLayout(row);
    m_localStack->addWidget(progress);

    connect(m_model, &QComboBox::currentIndexChanged, this, &SetupWizard::refreshPlan);
    connect(m_language, &QCheckBox::toggled, this, &SetupWizard::refreshPlan);
    return page;
}

const runtime::ModelPreset *SetupWizard::preset(const QString &id) const
{
    for (const runtime::ModelPreset &p : presets) {
        if (p.id == id)
            return &p;
    }
    return presets.isEmpty() ? runtime::presetById(id) : nullptr;
}

void SetupWizard::startLooking()
{
    auto ready = [this] {
        m_hardwareReady = true;
        showRecommendation();
        maybePlan();
    };
    // A setup that is running knows the computer and its programs already.
    if (m_setup->isRunning() && m_setup->hardware()) {
        ready();
        return;
    }
    // The plan does not wait for the program releases on GitHub; their rows fill in when the lookup
    // finishes (runtimesChanged). llama.cpp only while a plugin needs a language model.
    connect(m_setup, &LocalSetup::hardwareReady, this, ready, Qt::SingleShotConnection);
    QStringList programs{kSd};
    if (!ext::Extensions::instance().needingLanguageModel().isEmpty())
        programs << kLlama;
    m_setup->prepare(hardwareOverride, programs);
}

void SetupWizard::useModelsFolder(const QString &folder)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    if (clean.isEmpty())
        return;
    m_hardware->setText(tr("Reading %1…").arg(shortFolder(clean)));
    QPointer<SetupWizard> self(this);
    runtime::readFolders({clean}).then(this, [self, clean](QList<runtime::FoundFolder> found) {
        if (!self)
            return;
        // The folder replaces what was read of it before, and is preferred like a suggested one.
        self->m_found.removeIf([&clean](const runtime::FoundFolder &f) { return QDir::cleanPath(f.path) == clean; });
        self->m_found = found + self->m_found;
        if (self->m_setup && self->m_setup->hardware())
            self->m_hardware->setText(tr("This computer: %1").arg(self->m_setup->hardware()->summary()));
        int models = 0;
        for (const runtime::FoundFolder &f : std::as_const(found))
            models += int(f.entries.size());
        self->m_folderNote = models > 0 ? tr("%n model file(s) found in %1.", nullptr, models).arg(shortFolder(clean))
                                        : tr("No model files in %1 (Lumen reads its subfolders too).").arg(shortFolder(clean));
        self->m_foldersReady = true;
        self->refreshPlan();
        emit self->folderRead();
    });
}

void SetupWizard::lookForFiles()
{
    if (std::exchange(m_lookingForFiles, true))
        return;
    QPointer<SetupWizard> self(this);
    // Folders with models that are not in the library yet; the servers' model files count too.
    QStringList folders;
    if (searchFolders) {
        folders = *searchFolders;
    } else {
        QStringList used;
        for (const runtime::ServerProfile &p : m_studio->servers()->profiles()) {
            for (const char *key : {"model", "diffusion_model"})
                used << QDir::fromNativeSeparators(p.options.value(QLatin1String(key)).toString());
        }
        folders = m_studio->models()->suggestedRoots(m_studio->runtimes(), used);
    }
    runtime::readFolders(folders).then(this, [self](QList<runtime::FoundFolder> found) {
        if (!self)
            return;
        // After a folder chosen meanwhile (Use models from a folder…), which stays first.
        for (const runtime::FoundFolder &f : std::as_const(found)) {
            const bool known = std::any_of(self->m_found.cbegin(), self->m_found.cend(),
                                           [&f](const runtime::FoundFolder &k) { return QDir::cleanPath(k.path) == QDir::cleanPath(f.path); });
            if (!known)
                self->m_found << f;
        }
        self->m_foldersReady = true;
        self->maybePlan();
    });
}

void SetupWizard::maybePlan()
{
    if (!m_hardwareReady || !m_foldersReady || !m_setup)
        return;
    refreshPlan();
    emit localPlanReady();
}

void SetupWizard::showRecommendation()
{
    const runtime::HardwareInfo hw = m_setup->hardware().value_or(runtime::HardwareInfo());
    m_recommendation = runtime::recommendLocalModel(hw);
    const runtime::LocalRecommendation &r = *m_recommendation;
    const runtime::GpuInfo *gpu = runtime::imageGpu(hw);
    const runtime::ModelPreset *recommended = preset(r.presetId);
    const QString gpuText = gpu ? QStringLiteral("%1 (%2)").arg(gpu->name, runtime::formatBytes(qint64(gpu->dedicatedVram))) : QString();
    m_hardware->setText(tr("This computer: %1").arg(hw.summary()));

    switch (r.fit) {
    case runtime::LocalRecommendation::Fit::Full:
        m_localHint->setText(tr("Suggested for your %1: %2.").arg(gpuText, recommended ? recommended->name : r.presetId));
        break;
    case runtime::LocalRecommendation::Fit::Light:
        m_localHint->setText(tr("Suggested for your %1: %2, a lighter model. Changing images from a description works best with "
                                "8 GB or an online service.")
                                 .arg(gpuText, recommended ? recommended->name : r.presetId));
        m_advice->setText(tr("Your graphics card has less than 8 GB. %1 runs on it, but FLUX.2 [klein], which follows edit "
                             "instructions best, needs 8 GB; an online service can do that too.")
                              .arg(recommended ? recommended->name : r.presetId));
        break;
    case runtime::LocalRecommendation::Fit::NoGpu:
        m_localHint->setText(tr("This computer has no NVIDIA or AMD graphics card, so images would take many minutes each: an "
                                "online service is the better choice."));
        m_advice->setText(tr("No NVIDIA or AMD graphics card was found. The models run on the processor then, which takes many "
                             "minutes per image. An online service is the better choice here."));
        break;
    }
    if (m_setup->isRunning())
        m_localHint->setText(tr("Setting up: the downloads are running."));
    m_advice->setVisible(r.fit != runtime::LocalRecommendation::Fit::Full);
    m_adviceOnline->setVisible(r.fit != runtime::LocalRecommendation::Fit::Full);

    QSignalBlocker block(m_model);
    m_model->clear();
    for (const QString &id : {r.presetId, r.alternativeId}) {
        if (const runtime::ModelPreset *p = preset(id))
            m_model->addItem(id == r.presetId ? tr("%1 (suggested)").arg(p->name) : p->name, id);
    }
    block.unblock();

    // A language model only for plugins that need one, and on by default only where it fits next
    // to the image model.
    const QStringList needing = ext::Extensions::instance().needingLanguageModel();
    if (!needing.isEmpty()) {
        QSignalBlocker b(m_language);
        m_language->setChecked(r.languageModel);
        m_language->setVisible(true);
        m_languageInfo->setVisible(true);
    }
}

void SetupWizard::refreshPlan()
{
    if (!m_setup || !m_recommendation || !m_foldersReady)
        return;
    const runtime::ModelPreset *p = preset(m_model->currentData().toString());
    m_planTree->clear();
    if (!p) {
        m_setUp->setEnabled(false);
        return;
    }
    m_plan = runtime::planLocalModel(m_studio->models(), m_found, *p);
    m_modelInfo->setText(modelSummary(*p, m_recommendation->fit));

    LocalSetup::Options o;
    o.setId = m_setup->recommendedSet();
    o.language = !m_language->isHidden() && m_language->isChecked();
    o.images = true;
    o.model = m_plan;

    // The language model's size, for its check box.
    if (!m_language->isHidden()) {
        LocalSetup::Options lm;
        lm.setId = o.setId;
        lm.images = false;
        qint64 size = 0;
        QString name;
        for (const LocalSetup::FileStep &f : m_setup->fileSteps(lm)) {
            if (f.file.role == QLatin1String("language"))
                name = f.file.name;
            if (f.localPath.isEmpty())
                size += f.file.size;
        }
        const QStringList needing = ext::Extensions::instance().needingLanguageModel();
        m_language->setText(tr("Also set up a language model for %1").arg(needing.join(QStringLiteral(", "))));
        m_languageInfo->setText(size > 0 ? tr("%1, %2 more. It shares the graphics card with the image model, so images get slower; the "
                                              "editor works without it.")
                                               .arg(name, runtime::formatBytes(size))
                                         : tr("%1, already on this computer. It shares the graphics card with the image model, so "
                                              "images get slower; the editor works without it.")
                                               .arg(name));
    }

    bool blocked = !m_plan.ok();
    bool lookingUp = false;
    auto add = [this](const QString &what, qint64 size, const QString &from, const QString &licence, bool here) {
        auto *item = new QTreeWidgetItem(m_planTree, {what, size > 0 ? runtime::formatBytes(size) : QString(), from, licence});
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setForeground(2, here ? Theme::palette().success : Theme::palette().textMuted);
        return item;
    };
    QStringList kinds{kSd};
    if (o.language)
        kinds << kLlama;
    for (const QString &kind : kinds) {
        const LocalSetup::RuntimeStep s = m_setup->runtimeStep(kind);
        const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(kind);
        const QString name = spec ? spec->name : kind;
        if (!s.existingId.isEmpty()) {
            add(tr("%1 (the program)").arg(name), 0, tr("Installed"), spec ? spec->license : QString(), true);
        } else if (s.error.isEmpty() && s.plan.ok() && s.plan.tag.isEmpty()) {
            // GitHub has not answered yet: the release, and so its size, is not known (the row and
            // the total fill in when it answers).
            lookingUp = true;
            add(tr("%1 (the program)").arg(name), 0, tr("Looking up…"), spec ? spec->license : QString(), false);
        } else if (s.plan.ok()) {
            add(s.plan.tag.isEmpty() ? tr("%1 (the program)").arg(name) : tr("%1 %2 (the program)").arg(name, s.plan.tag),
                m_studio->runtimes()->pendingDownload(s.plan), tr("Download from GitHub"),
                spec ? spec->license : QString(), false);
        } else {
            QTreeWidgetItem *item = add(tr("%1 (the program)").arg(name), 0, s.error.isEmpty() ? tr("Looking up…") : s.error, QString(), false);
            item->setForeground(2, Theme::palette().danger);
            blocked = true;
        }
    }
    QSet<QString> licences;
    for (const runtime::PlannedFile &f : m_plan.files) {
        QTreeWidgetItem *item = nullptr;
        // "FLUX.2 VAE — flux2-vae.safetensors": what it is and which file.
        if (!f.missing()) {
            item = add(QStringLiteral("%1 — %2").arg(f.label, QFileInfo(f.path).fileName()), QFileInfo(f.path).size(),
                       f.folder.isEmpty() ? tr("On this computer") : tr("On this computer (found)"), QString(), true);
            item->setToolTip(0, shortFolder(f.path));
            item->setToolTip(2, shortFolder(f.path));
        } else if (f.file.isValid()) {
            item = add(QStringLiteral("%1 — %2").arg(f.label, f.file.fileName()), f.file.size, tr("Download from Hugging Face"), f.file.license, false);
            item->setToolTip(0, QStringLiteral("https://huggingface.co/%1 · %2").arg(f.file.repo, f.file.path));
            licences << f.file.license;
        } else {
            item = add(f.label, 0, f.required ? tr("Missing") : tr("Not needed"), QString(), false);
        }
    }
    if (o.language) {
        for (const LocalSetup::FileStep &f : m_setup->fileSteps(o)) {
            QTreeWidgetItem *item = add(f.file.name, f.file.size, f.localPath.isEmpty() ? tr("Download from Hugging Face") : tr("On this computer"),
                                        f.file.license, !f.localPath.isEmpty());
            item->setToolTip(0, f.localPath.isEmpty() ? QStringLiteral("https://huggingface.co/%1 · %2").arg(f.file.repo, f.file.path)
                                                      : shortFolder(f.localPath));
            if (f.localPath.isEmpty())
                licences << f.file.license;
        }
    }
    for (int c = 0; c < 3; ++c)
        m_planTree->resizeColumnToContents(c);

    const qint64 download = m_setup->downloadSize(o);
    const QStorageInfo storage(m_studio->models()->defaultFolder());
    QStringList lines;
    if (!m_plan.ok()) {
        lines << QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().danger.name(), m_plan.error.toHtmlEscaped());
    } else if (download > 0) {
        QString line = lookingUp ? tr("To download: <b>%1</b> and the program, %2.").arg(runtime::formatBytes(download), downloadTime(download))
                                 : tr("To download: <b>%1</b>, %2.").arg(runtime::formatBytes(download), downloadTime(download));
        if (storage.isValid())
            line += QLatin1Char(' ') + tr("Free on %1: %2.").arg(QDir::toNativeSeparators(storage.rootPath()), runtime::formatBytes(storage.bytesAvailable()));
        lines << line;
        lines << tr("Models are saved in %1.").arg(shortFolder(m_studio->models()->defaultFolder()).toHtmlEscaped());
    } else {
        lines << tr("<b>Everything is on this computer already</b>; nothing to download.");
    }
    if (!m_plan.foldersToAdd().isEmpty())
        lines << tr("Files are used where they are; %1 joins the model library (nothing there is moved or changed).")
                     .arg(m_plan.foldersToAdd().join(QStringLiteral(", ")).toHtmlEscaped());
    if (!m_folderNote.isEmpty())
        lines << m_folderNote.toHtmlEscaped();
    // A server of Lumen's that has this model already.
    if (m_plan.ok() && !m_plan.model()->missing()) {
        bool works = false;
        const QString server = m_setup->serverWithModel(m_plan.model()->path, m_plan.model()->option, &works);
        if (const auto profile = server.isEmpty() ? std::nullopt : m_studio->servers()->profile(server))
            lines << (works ? tr("Your server \"%1\" runs this model already; Lumen uses it as it is.").arg(profile->name.toHtmlEscaped())
                            : tr("Your server \"%1\" has this model with settings that cannot work; Lumen corrects them (the model "
                                 "in its slot, with its VAE and text encoder).")
                                  .arg(profile->name.toHtmlEscaped()));
    }
    if (storage.isValid() && download > 0 && storage.bytesAvailable() < download + (1LL << 30)) {
        lines << QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().danger.name(), tr("Not enough disk space."));
        blocked = true;
    }
    if (!licences.isEmpty())
        lines << tr("By downloading you accept the models' licences (see the Licence column).");
    lines << tr("Then Lumen starts the model and makes a test image.");
    m_summary->setText(lines.join(QStringLiteral("<br>")));
    m_setUp->setText(download > 0 ? tr("Download and set up") : tr("Set up"));
    m_setUp->setEnabled(!blocked && !m_setup->isRunning());
}

void SetupWizard::startLocal()
{
    if (!m_setup || !m_plan.ok() || m_setup->isRunning())
        return;
    LocalSetup::Options o;
    o.setId = m_setup->recommendedSet();
    o.language = !m_language->isHidden() && m_language->isChecked();
    o.images = true;
    o.model = m_plan;
    m_localStack->setCurrentIndex(1);
    g_running = m_setup.data();
    m_setup->run(o);
}

void SetupWizard::showLocalProgress()
{
    if (!m_setup)
        return;
    m_progress->clear();
    for (const QPointer<Job> &job : m_setup->jobs()) {
        if (!job)
            continue;
        auto *item = new QTreeWidgetItem(m_progress, {job->title()});
        auto *bar = new QProgressBar(m_progress);
        bar->setRange(0, 1000);
        bar->setTextVisible(true);
        m_progress->setItemWidget(item, 1, bar);
        auto update = [job, bar, item] {
            if (!job)
                return;
            bar->setValue(job->state() == Job::State::Succeeded ? 1000 : int(std::max(0.0, job->progress()) * 1000));
            const QString status = job->state() == Job::State::Failed ? job->errorString()
                                   : job->state() == Job::State::Succeeded ? tr("Done")
                                   : job->state() == Job::State::Queued ? tr("Waiting")
                                                                        : job->statusText();
            bar->setFormat(status.isEmpty() ? QStringLiteral("%p%") : status);
            item->setToolTip(0, status);
        };
        connect(job, &Job::progressChanged, bar, update);
        connect(job, &Job::stateChanged, bar, update);
        update();
    }
    m_progress->setColumnWidth(0, 320);
    m_progressStatus->setText(m_setup->jobs().isEmpty() ? tr("Setting up the server…") : tr("%n step(s)", nullptr, int(m_setup->jobs().size())));
}

void SetupWizard::localFinished(bool ok, const QString &message)
{
    m_localStack->setCurrentIndex(0);
    if (!ok) {
        m_backTo = Page::Local;
        m_pages->setCurrentIndex(int(Page::Result));
        m_resultTitle->setText(tr("The setup did not finish"));
        m_resultText->setText(message.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("<br><br>")
                              + tr("Back shows the plan again; files that were downloaded are kept."));
        m_resultImage->clear();
        m_retry->setVisible(false);
        m_testProgress->setVisible(false);
        m_stopTest->setVisible(false);
        refreshPlan();
        return;
    }
    ai::Provider *provider = m_setup ? m_studio->providerForServer(m_setup->imageServer()) : nullptr;
    if (!provider) {
        localFinished(false, tr("The image server was set up, but it has no provider."));
        return;
    }
    m_backTo = Page::Local;
    const QString name = m_setup->options().model ? m_setup->options().model->preset.name : provider->displayName();
    runTestImage(provider->instanceId(), tr("%1 is set up").arg(name),
                 tr("Lumen starts the image engine and makes a test image. The first start loads the model, which takes a minute "
                    "or two."));
    refreshPlan();
}

// ---- Use an online service ----------------------------------------------------------------------

QWidget *SetupWizard::buildOnlinePage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    auto *title = new QLabel(tr("Use an online service"), page);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(tr("Lumen sends your description and the image (or the selected part) to the service, which makes the "
                               "result and charges your account a few cents per image. The key is kept in the Windows Credential "
                               "Manager, never in a file."),
                            page));
    m_services = new QButtonGroup(page);
    for (const Service &s : services()) {
        auto *radio = new QRadioButton(s.name, page);
        radio->setObjectName(QStringLiteral("service-%1").arg(s.typeId));
        radio->setProperty("typeId", s.typeId);
        m_services->addButton(radio);
        v->addWidget(radio);
        auto *info = mutedLabel(s.description, page);
        info->setContentsMargins(24, 0, 0, 4);
        v->addWidget(info);
    }
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_key = new QLineEdit(page);
    m_key->setObjectName(QStringLiteral("serviceKey"));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(tr("Paste the key here"));
    form->addRow(tr("API key"), m_key);
    v->addLayout(form);
    m_keyLink = wrapped(QString(), page);
    m_keyLink->setTextFormat(Qt::RichText);
    m_keyLink->setOpenExternalLinks(true);
    v->addWidget(m_keyLink);
    m_onlineStatus = wrapped(QString(), page);
    m_onlineStatus->setObjectName(QStringLiteral("serviceStatus"));
    v->addWidget(m_onlineStatus);
    v->addStretch();
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(backButton(page));
    buttons->addStretch();
    m_connectService = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Connect and make a test image"), page);
    m_connectService->setObjectName(QStringLiteral("connectService"));
    m_connectService->setToolTip(tr("Checks the key, makes the service Lumen's image engine and makes one test image (a few cents)"));
    Theme::setRole(m_connectService, QStringLiteral("primary"));
    buttons->addWidget(m_connectService);
    v->addLayout(buttons);

    connect(m_services, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *b) { selectService(b->property("typeId").toString()); });
    connect(m_connectService, &QPushButton::clicked, this, &SetupWizard::connectService);
    connect(m_key, &QLineEdit::returnPressed, this, &SetupWizard::connectService);
    m_services->buttons().first()->setChecked(true);
    selectService(services().first().typeId);
    return page;
}

void SetupWizard::selectService(const QString &typeId)
{
    m_service = typeId;
    for (const Service &s : services()) {
        if (s.typeId != typeId)
            continue;
        const QString shown = QUrl(s.keyPage).host() + QUrl(s.keyPage).path();
        m_keyLink->setText(tr("No key yet? Create one at <a href=\"%1\">%2</a> (you need an account with %3).")
                               .arg(s.keyPage, shown.toHtmlEscaped(), s.name.toHtmlEscaped()));
    }
    m_onlineStatus->clear();
}

void SetupWizard::connectService()
{
    // Enter in the key field while a key is checked would check it again and pay for a second
    // test image.
    if (m_checkingKey)
        return;
    const QString key = m_key->text().trimmed();
    if (key.isEmpty()) {
        m_onlineStatus->setText(tr("Paste the key first."));
        Theme::setRole(m_onlineStatus, QStringLiteral("error"));
        return;
    }
    ai::ProviderRegistry *registry = m_studio->providers();
    // The service's provider if there is one already (a new key replaces the old), else a new one.
    // The key is checked first on a provider that is not saved, so a mistyped one changes nothing:
    // the old key stays in the Credential Manager and no engine is added.
    QString existing;
    for (ai::Provider *p : registry->providers()) {
        if (p->type().id == m_service) {
            existing = p->instanceId();
            break;
        }
    }
    QVariantMap settings = existing.isEmpty() ? QVariantMap() : registry->settingsFor(existing);
    settings.insert(QStringLiteral("api_key"), key);
    if (serviceUrls.contains(m_service))
        settings.insert(QStringLiteral("url"), serviceUrls.value(m_service));
    ai::Provider *check = registry->createUnsaved(m_service, settings, this);
    if (!check) {
        m_onlineStatus->setText(tr("This service is not available in this version of Lumen."));
        Theme::setRole(m_onlineStatus, QStringLiteral("error"));
        return;
    }
    m_checkingKey = true;
    m_onlineStatus->setText(tr("Checking the key…"));
    Theme::setRole(m_onlineStatus, QStringLiteral("muted"));
    m_connectService->setEnabled(false);
    QPointer<SetupWizard> self(this);
    const QString service = m_service;
    check->testConnection()
        .then(this, [self, check, existing, service, settings](const QString &) {
            check->deleteLater();
            if (!self)
                return;
            self->m_checkingKey = false;
            self->m_connectService->setEnabled(true);
            ai::ProviderRegistry *registry = self->m_studio->providers();
            ai::Provider *provider = existing.isEmpty() ? nullptr : registry->provider(existing);
            if (provider)
                registry->updateProvider(existing, provider->displayName(), settings);
            else
                provider = registry->addProvider(service, serviceName(service), settings);
            if (!provider) {
                self->m_onlineStatus->setText(tr("This service is not available in this version of Lumen."));
                Theme::setRole(self->m_onlineStatus, QStringLiteral("error"));
                return;
            }
            const QString id = provider->instanceId();
            registry->setEnabled(id, true);
            registry->setDefaultImageProvider(id);
            self->m_onlineStatus->setText(tr("Connected."));
            Theme::setRole(self->m_onlineStatus, QStringLiteral("success"));
            self->m_backTo = Page::Online;
            self->runTestImage(id, tr("%1 is connected").arg(serviceName(service)),
                               tr("Lumen asks the service for a test image; this takes a few seconds."));
        })
        .onFailed(this, [self, check](const std::exception &e) {
            check->deleteLater();
            if (!self)
                return;
            self->m_checkingKey = false;
            self->m_connectService->setEnabled(true);
            self->m_onlineStatus->setText(tr("The service did not accept the key: %1").arg(describeException(e)));
            Theme::setRole(self->m_onlineStatus, QStringLiteral("error"));
        });
}

// ---- Connect a running server ------------------------------------------------------------------

QWidget *SetupWizard::buildServerPage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    auto *title = new QLabel(tr("Connect a server that is already running"), page);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(tr("Lumen works with ComfyUI, Forge / Stable Diffusion WebUI (started with --api) and stable-diffusion.cpp's "
                               "sd-server, and uses the model loaded there. Nothing is downloaded; the server keeps running on its own."),
                            page));
    v->addWidget(sectionLabel(tr("Found on this computer"), page));
    auto *list = new QWidget(page);
    m_foundList = new QVBoxLayout(list);
    m_foundList->setContentsMargins(0, 0, 0, 0);
    m_foundList->setSpacing(6);
    v->addWidget(verticalScroll(list, page), 1);
    auto *again = new QHBoxLayout;
    m_foundStatus = wrapped(tr("Looking for servers…"), page);
    Theme::setRole(m_foundStatus, QStringLiteral("muted"));
    again->addWidget(m_foundStatus, 1);
    auto *search = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Search again"), page);
    connect(search, &QPushButton::clicked, this, &SetupWizard::detectServers);
    again->addWidget(search, 0, Qt::AlignTop);
    v->addLayout(again);
    v->addWidget(sectionLabel(tr("Or by address"), page));
    auto *row = new QHBoxLayout;
    m_address = new QLineEdit(page);
    m_address->setObjectName(QStringLiteral("serverAddress"));
    m_address->setPlaceholderText(QStringLiteral("http://192.168.1.20:8188"));
    auto *connectButton = new QPushButton(tr("Connect"), page);
    connectButton->setObjectName(QStringLiteral("connectAddress"));
    row->addWidget(m_address, 1);
    row->addWidget(connectButton);
    v->addLayout(row);
    m_serverStatus = wrapped(QString(), page);
    m_serverStatus->setObjectName(QStringLiteral("serverStatus"));
    v->addWidget(m_serverStatus);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(backButton(page));
    buttons->addStretch();
    v->addLayout(buttons);
    connect(connectButton, &QPushButton::clicked, this, &SetupWizard::connectAddress);
    connect(m_address, &QLineEdit::returnPressed, this, &SetupWizard::connectAddress);
    return page;
}

void SetupWizard::detectServers()
{
    m_foundStatus->setText(tr("Looking for servers…"));
    QPointer<SetupWizard> self(this);
    // Only the last search fills the list: "Search again" while the first search still waits for
    // its ports would list every server twice.
    const int search = ++m_serverSearch;
    m_studio->providers()->detectLocalServers().then(this, [self, search](QList<ai::DetectedServer> found) {
        if (!self || search != self->m_serverSearch)
            return;
        clearLayout(self->m_foundList);
        // Image servers only: language model servers are set up elsewhere.
        static const QSet<QString> image{QStringLiteral("comfyui"), QStringLiteral("sdwebui"), QStringLiteral("sdcpp")};
        found.removeIf([](const ai::DetectedServer &s) { return !image.contains(s.typeId); });
        // An sd-server also answers WebUI's API, so on a port both could use (7861) it would be
        // listed twice: once, as sd-server, whose own API Lumen uses.
        QSet<QString> sdServers;
        for (const ai::DetectedServer &s : std::as_const(found)) {
            if (s.typeId == QLatin1String("sdcpp"))
                sdServers.insert(s.url.adjusted(QUrl::StripTrailingSlash).toString());
        }
        found.removeIf([&sdServers](const ai::DetectedServer &s) {
            return s.typeId == QLatin1String("sdwebui") && sdServers.contains(s.url.adjusted(QUrl::StripTrailingSlash).toString());
        });
        self->m_servers = found;
        QStringList names;
        for (const ai::DetectedServer &s : std::as_const(found)) {
            auto *row = card(self);
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(12, 8, 12, 8);
            auto *label = new QLabel(QStringLiteral("<b>%1</b><br><span style='color:%4'>%2 %3</span>")
                                         .arg(s.name.toHtmlEscaped(), s.url.toString().toHtmlEscaped(), s.detail.toHtmlEscaped(),
                                              Theme::palette().textMuted.name()),
                                     row);
            label->setWordWrap(true);
            h->addWidget(label, 1);
            const bool added = providerAt(self->m_studio->providers(), s.typeId, s.url);
            auto *use = new QPushButton(added ? tr("Use it again") : tr("Use this"), row);
            use->setObjectName(QStringLiteral("useServer"));
            Theme::setRole(use, QStringLiteral("primary"));
            h->addWidget(use, 0, Qt::AlignVCenter);
            QObject::connect(use, &QPushButton::clicked, self, [self, s] { self->useServer(s); });
            self->m_foundList->addWidget(row);
            names << QStringLiteral("%1 (%2)").arg(s.name, s.url.authority());
        }
        self->m_foundList->addStretch();
        if (found.isEmpty()) {
            self->m_foundStatus->setText(tr("None found on the usual ports (ComfyUI 8188 and 8000, WebUI 7860, Forge 7861, sd-server "
                                            "1234 and 7861). Start the server, then search again, or type its address below."));
            self->m_serverHint->setText(tr("None running on the usual ports right now; you can type an address."));
        } else {
            self->m_foundStatus->setText(tr("Found %n server(s).", nullptr, int(found.size())));
            self->m_serverHint->setText(tr("Found: %1.").arg(names.join(QStringLiteral(", "))));
        }
        emit self->serversFound();
    });
}

void SetupWizard::useServer(const ai::DetectedServer &server)
{
    ai::ProviderRegistry *registry = m_studio->providers();
    ai::Provider *provider = providerAt(registry, server.typeId, server.url);
    if (!provider)
        provider = registry->addProvider(server.typeId, server.name, {{QStringLiteral("url"), server.url.adjusted(QUrl::StripTrailingSlash).toString()}});
    if (!provider) {
        m_serverStatus->setText(tr("%1 is not available in this version of Lumen.").arg(server.name));
        Theme::setRole(m_serverStatus, QStringLiteral("error"));
        return;
    }
    registry->setEnabled(provider->instanceId(), true);
    registry->setDefaultImageProvider(provider->instanceId());
    m_backTo = Page::Server;
    const QString id = provider->instanceId();
    QPointer<SetupWizard> self(this);
    auto test = [self, id, name = server.name] {
        if (self)
            self->runTestImage(id, tr("%1 is connected").arg(name), tr("Lumen asks the server for a test image with its model."));
    };
    // ComfyUI's built-in graph needs a checkpoint: the first one it has, until one is chosen.
    if (provider->type().id == QLatin1String("comfyui") && provider->text(QStringLiteral("model")).isEmpty()) {
        m_serverStatus->setText(tr("Asking %1 for its models…").arg(server.name));
        Theme::setRole(m_serverStatus, QStringLiteral("muted"));
        provider->listModels()
            .then(this, [self, id, test](const QStringList &models) {
                if (!self)
                    return;
                ai::ProviderRegistry *registry = self->m_studio->providers();
                if (ai::Provider *p = registry->provider(id); p && !models.isEmpty()) {
                    QVariantMap settings = registry->settingsFor(id);
                    settings.insert(QStringLiteral("model"), models.first());
                    registry->updateProvider(id, p->displayName(), settings);
                }
                test();
            })
            .onFailed(this, [test](const std::exception &) { test(); });
        return;
    }
    test();
}

void SetupWizard::connectAddress()
{
    const QUrl url = QUrl::fromUserInput(m_address->text().trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        m_serverStatus->setText(tr("Type the server's address, for example http://127.0.0.1:8188."));
        Theme::setRole(m_serverStatus, QStringLiteral("error"));
        return;
    }
    m_serverStatus->setText(tr("Asking %1…").arg(url.toString()));
    Theme::setRole(m_serverStatus, QStringLiteral("muted"));
    QPointer<SetupWizard> self(this);
    m_studio->providers()->probeAddress(url).then(this, [self, url](QList<ai::DetectedServer> found) {
        if (!self)
            return;
        if (found.isEmpty()) {
            self->m_serverStatus->setText(tr("No ComfyUI, WebUI / Forge or sd-server answers at %1. Is it running, and started with "
                                             "--listen (ComfyUI) or --api (WebUI) when it is on another computer?")
                                              .arg(url.toString()));
            Theme::setRole(self->m_serverStatus, QStringLiteral("error"));
            return;
        }
        self->m_serverStatus->setText(tr("Found %1.").arg(found.first().name));
        Theme::setRole(self->m_serverStatus, QStringLiteral("success"));
        self->useServer(found.first());
    });
}

// ---- Result: the test image ---------------------------------------------------------------------

QWidget *SetupWizard::buildResultPage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    m_resultTitle = new QLabel(page);
    m_resultTitle->setObjectName(QStringLiteral("resultTitle"));
    Theme::setRole(m_resultTitle, QStringLiteral("title"));
    v->addWidget(m_resultTitle);
    m_resultText = wrapped(QString(), page);
    m_resultText->setObjectName(QStringLiteral("resultText"));
    m_resultText->setTextFormat(Qt::RichText);
    v->addWidget(m_resultText);
    m_resultImage = new QLabel(page);
    m_resultImage->setObjectName(QStringLiteral("resultImage"));
    m_resultImage->setAlignment(Qt::AlignCenter);
    m_resultImage->setMinimumSize(1, 1);
    m_resultImage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    v->addWidget(m_resultImage, 1);
    // While the test image is made: how far it is (starting the server, loading the model, the
    // steps), and a way to stop it.
    auto *progressRow = new QHBoxLayout;
    m_testProgress = new QProgressBar(page);
    m_testProgress->setObjectName(QStringLiteral("testProgress"));
    m_testProgress->setRange(0, 1000);
    m_testProgress->setTextVisible(false);
    progressRow->addWidget(m_testProgress, 1);
    m_stopTest = new QPushButton(tr("Stop"), page);
    m_stopTest->setObjectName(QStringLiteral("stopTest"));
    m_stopTest->setToolTip(tr("Stops the test image; the engine stays set up"));
    connect(m_stopTest, &QPushButton::clicked, this, [this] {
        if (m_testJob)
            m_testJob->cancel();
    });
    progressRow->addWidget(m_stopTest);
    m_testProgress->setVisible(false);
    m_stopTest->setVisible(false);
    v->addLayout(progressRow);
    auto *buttons = new QHBoxLayout;
    auto *back = new QPushButton(Icons::get(QStringLiteral("chevron-left")), tr("Back"), page);
    connect(back, &QPushButton::clicked, this, [this] { showPage(m_backTo); });
    buttons->addWidget(back);
    buttons->addStretch();
    m_retry = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Make another test image"), page);
    connect(m_retry, &QPushButton::clicked, this, [this] {
        runTestImage(m_testedProvider, m_resultTitle->text(), tr("Lumen makes another test image."));
    });
    buttons->addWidget(m_retry);
    auto *done = new QPushButton(tr("Done"), page);
    done->setObjectName(QStringLiteral("setupDone"));
    Theme::setRole(done, QStringLiteral("primary"));
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(done);
    v->addLayout(buttons);
    return page;
}

void SetupWizard::runTestImage(const QString &providerId, const QString &title, const QString &intro)
{
    m_testedProvider = providerId;
    m_pages->setCurrentIndex(int(Page::Result));
    m_resultTitle->setText(title);
    m_resultText->setText(intro.toHtmlEscaped());
    m_resultImage->setText(tr("Making a test image…"));
    m_retry->setVisible(false);
    m_testProgress->setValue(0);
    m_testProgress->setVisible(true);
    m_stopTest->setVisible(true);
    ai::GenerationSpec spec;
    spec.providerId = providerId;
    spec.request.prompt = QStringLiteral("A cup of coffee on a wooden table by a window, morning light, photo");
    spec.request.size = QSize(1024, 1024);
    spec.request.quality = QStringLiteral("low"); // where a service charges by quality, the test is cheap
    spec.title = tr("Test image");
    spec.saveAssets = false;
    Job *job = m_studio->generation()->submit(spec);
    m_testJob = job;
    QPointer<SetupWizard> self(this);
    auto progress = [self, job] {
        if (!self || self->m_testJob != job || job->isFinished())
            return;
        self->m_testProgress->setValue(int(std::clamp(job->progress(), 0.0, 1.0) * 1000));
        // What it is doing (starting the server, loading the model, the steps) under the heading:
        // the theme's progress bars show no text.
        const QString status = job->state() == Job::State::Queued ? tr("Waiting for the engine") : job->statusText();
        self->m_resultImage->setText(status.isEmpty() ? tr("Making a test image…") : tr("Making a test image…") + QLatin1Char('\n') + status);
    };
    connect(job, &Job::progressChanged, this, progress);
    connect(job, &Job::stateChanged, this, progress);
    progress();
    connect(job, &Job::finished, this, [self, job] {
        if (!self || self->m_testJob != job)
            return;
        self->m_retry->setVisible(true);
        self->m_testProgress->setVisible(false);
        self->m_stopTest->setVisible(false);
        const ai::GenerationOutput out = ai::GenerationService::outputOf(job);
        const bool ok = job->state() == Job::State::Succeeded && !out.images.isEmpty();
        if (job->state() == Job::State::Cancelled) {
            self->m_resultText->setText(tr("The test image was stopped. The engine is set up; Make another test image tries again.").toHtmlEscaped());
            self->m_resultImage->clear();
        } else if (!ok) {
            self->m_resultText->setText(tr("The test image did not work: %1").arg(job->errorString()).toHtmlEscaped() + QStringLiteral("<br>")
                                        + tr("Back lets you check the settings; Settings → AI shows the engine and its status.").toHtmlEscaped());
            self->m_resultImage->clear();
        } else {
            self->m_resultText->setText(tr("It works. Open an image, select an area and describe what should change, or start a new "
                                           "image from a description. Settings → AI shows and changes the engine.")
                                            .toHtmlEscaped());
            const QSize box = self->m_resultImage->size().boundedTo(QSize(420, 420)).expandedTo(QSize(64, 64));
            self->m_resultImage->setPixmap(QPixmap::fromImage(out.images.first()).scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        emit self->testImageFinished(ok);
    });
}

namespace {

// The window the wizard belongs to. Over a modal dialog (Settings, which runs exec()) it must be
// that dialog's child, or the dialog blocks it; when that dialog closes, the wizard moves to the
// window under it instead of closing with it.
void adoptWizard(SetupWizard *wizard, QWidget *owner)
{
    if (wizard->parentWidget() == owner)
        return;
    const bool shown = wizard->isVisible();
    wizard->setParent(owner, wizard->windowFlags()); // hides it
    if (shown)
        wizard->show();
    if (auto *dialog = qobject_cast<QDialog *>(owner)) {
        QObject::connect(dialog, &QDialog::finished, wizard, [wizard, dialog] {
            if (wizard->parentWidget() == dialog)
                adoptWizard(wizard, dialog->parentWidget() ? dialog->parentWidget()->window() : nullptr);
        });
    }
}

} // namespace

SetupWizard *openSetupWizard(Studio *studio, QWidget *parent, SetupWizard::Page page)
{
    QWidget *modal = QApplication::activeModalWidget();
    if (!g_open) {
        // Not over a modal dialog: the main window, so the wizard stays when the page or dialog
        // that opened it closes.
        QWidget *owner = modal;
        if (!owner) {
            owner = parent;
            while (owner && owner->parentWidget())
                owner = owner->parentWidget();
        }
        g_open = new SetupWizard(studio);
        g_open->setAttribute(Qt::WA_DeleteOnClose);
        adoptWizard(g_open, owner);
    } else if (modal && modal != g_open && !g_open->isAncestorOf(modal)) {
        // Open already (from the first start or Help) and now asked for from Settings: over it.
        adoptWizard(g_open, modal);
    }
    g_open->showPage(page);
    g_open->show();
    g_open->raise();
    g_open->activateWindow();
    return g_open;
}

} // namespace lumen::ui
