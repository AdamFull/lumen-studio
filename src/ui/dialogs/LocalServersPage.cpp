#include "ui/dialogs/LocalServersPage.h"
#include "ui/dialogs/ModelsPage.h"
#include "ui/dialogs/SetupWizard.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelPresets.h"

#include "ai/ProviderRegistry.h"
#include "core/Error.h"
#include "core/Job.h"
#include "core/Settings.h"
#include "runtime/Hardware.h"
#include "runtime/ServerProfile.h"
#include "runtime/RuntimeStore.h"
#include "runtime/Supervisor.h"
#include "runtime/Trainer.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace lumen::ui {

using runtime::OptionField;
using runtime::ServerState;

namespace {

QString runtimeName(const QString &kind)
{
    const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(kind);
    return spec ? spec->name : kind;
}

QString buildLabel(const runtime::InstalledRuntime &rt)
{
    if (!rt.managed)
        return QCoreApplication::translate("lumen::ui::LocalServersPage", "Existing install");
    const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(rt.kind);
    const runtime::BackendSpec *b = spec ? spec->backend(rt.backend) : nullptr;
    return b ? b->label : rt.backend;
}

QString startLabel(runtime::StartPolicy p)
{
    switch (p) {
    case runtime::StartPolicy::AtStartup:
        return QCoreApplication::translate("lumen::ui::LocalServersPage", "With Lumen");
    case runtime::StartPolicy::Manual:
        return QCoreApplication::translate("lumen::ui::LocalServersPage", "Manually");
    case runtime::StartPolicy::OnDemand:
        break;
    }
    return QCoreApplication::translate("lumen::ui::LocalServersPage", "When needed");
}

QColor stateColor(ServerState s)
{
    switch (s) {
    case ServerState::Ready:
        return Theme::palette().success;
    case ServerState::Starting:
    case ServerState::Stopping:
        return Theme::palette().warning;
    case ServerState::Failed:
        return Theme::palette().danger;
    case ServerState::Stopped:
        break;
    }
    return Theme::palette().textMuted;
}

// Editor for a list of llama.cpp models: name, model file, optional vision projector.
class ModelListEditor : public QWidget
{
public:
    ModelListEditor(const QVariantList &value, const QString &filter, std::function<void()> changed, QWidget *parent,
                    runtime::ModelLibrary *library = nullptr)
        : QWidget(parent)
        , m_filter(filter)
        , m_changed(std::move(changed))
    {
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        m_table = new QTableWidget(0, 3, this);
        m_table->setHorizontalHeaderLabels({QCoreApplication::translate("lumen::ui::ServerProfileDialog", "Name"),
                                            QCoreApplication::translate("lumen::ui::ServerProfileDialog", "Model file"),
                                            QCoreApplication::translate("lumen::ui::ServerProfileDialog", "Vision projector")});
        m_table->horizontalHeader()->setStretchLastSection(true);
        m_table->verticalHeader()->setVisible(false);
        m_table->setMinimumHeight(110);
        v->addWidget(m_table);
        auto *bar = new QHBoxLayout;
        auto *add = new QPushButton(Icons::get(QStringLiteral("plus")), QCoreApplication::translate("lumen::ui::ServerProfileDialog", "Add model…"), this);
        auto *remove = new QPushButton(Icons::get(QStringLiteral("trash")), QCoreApplication::translate("lumen::ui::ServerProfileDialog", "Remove"), this);
        bar->addWidget(add);
        if (library) {
            auto *fromLibrary = new QPushButton(Icons::get(QStringLiteral("layers")),
                                                QCoreApplication::translate("lumen::ui::ServerProfileDialog", "From library"), this);
            auto *menu = new QMenu(fromLibrary);
            fromLibrary->setMenu(menu);
            connect(menu, &QMenu::aboutToShow, this, [this, menu, library] {
                fillModelMenu(menu, library, {runtime::ModelKind::Llm}, [this](const QString &path) {
                    addRow(runtime::llamaModelName(path), path, QDir::toNativeSeparators(runtime::findProjector(path)));
                    m_changed();
                });
            });
            bar->addWidget(fromLibrary);
        }
        bar->addWidget(remove);
        bar->addStretch();
        v->addLayout(bar);
        for (const QVariant &item : value) {
            const QVariantMap m = item.toMap();
            addRow(m.value(QStringLiteral("name")).toString(), m.value(QStringLiteral("model")).toString(), m.value(QStringLiteral("mmproj")).toString());
        }
        connect(add, &QPushButton::clicked, this, [this] {
            const QString start = Settings::instance().value(QStringLiteral("servers/lastModelDir")).toString();
            const QString path = QFileDialog::getOpenFileName(this, QCoreApplication::translate("lumen::ui::ServerProfileDialog", "Add model"), start,
                                                              m_filter);
            if (path.isEmpty())
                return;
            Settings::instance().setValue(QStringLiteral("servers/lastModelDir"), QFileInfo(path).absolutePath());
            addRow(runtime::llamaModelName(path), QDir::toNativeSeparators(path), QDir::toNativeSeparators(runtime::findProjector(path)));
            m_changed();
        });
        connect(remove, &QPushButton::clicked, this, [this] {
            if (m_table->currentRow() >= 0) {
                m_table->removeRow(m_table->currentRow());
                m_changed();
            }
        });
        connect(m_table, &QTableWidget::itemChanged, this, [this] { m_changed(); });
    }

    QVariantList value() const
    {
        QVariantList out;
        for (int r = 0; r < m_table->rowCount(); ++r) {
            auto text = [this, r](int c) { return m_table->item(r, c) ? m_table->item(r, c)->text().trimmed() : QString(); };
            if (text(1).isEmpty())
                continue;
            out << QVariantMap{{QStringLiteral("name"), text(0)}, {QStringLiteral("model"), text(1)}, {QStringLiteral("mmproj"), text(2)}};
        }
        return out;
    }

private:
    void addRow(const QString &name, const QString &model, const QString &mmproj)
    {
        QSignalBlocker block(m_table);
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        m_table->setItem(r, 0, new QTableWidgetItem(name));
        m_table->setItem(r, 1, new QTableWidgetItem(model));
        m_table->setItem(r, 2, new QTableWidgetItem(mmproj));
        m_table->resizeColumnToContents(0);
    }

    QTableWidget *m_table;
    QString m_filter;
    std::function<void()> m_changed;
};

QTreeWidget *makeTree(const QStringList &headers, QWidget *parent)
{
    auto *t = new QTreeWidget(parent);
    t->setHeaderLabels(headers);
    t->setRootIsDecorated(false);
    t->setUniformRowHeights(true);
    t->setAlternatingRowColors(false);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->header()->setStretchLastSection(true);
    t->setMinimumHeight(130);
    return t;
}

} // namespace

// ---- LocalServersPage -----------------------------------------------------------------------------

LocalServersPage::LocalServersPage(Studio *studio, QWidget *parent)
    : QWidget(parent)
    , m_studio(studio)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(8);
    auto *title = new QLabel(tr("Local AI servers"), this);
    Theme::setRole(title, QStringLiteral("title"));
    v->addWidget(title);
    v->addWidget(mutedLabel(tr("Lumen can download and run llama.cpp (language and vision models) and stable-diffusion.cpp "
                               "(image models) on this computer, or use copies you already have. Servers start when a provider "
                               "needs them and stop when Lumen closes."),
                            this));
    m_hardwareLabel = mutedLabel(tr("Checking this computer…"), this);
    auto *setupRow = new QHBoxLayout;
    setupRow->addWidget(m_hardwareLabel, 1);
    auto *setup = new QPushButton(Icons::get(QStringLiteral("sparkles")), tr("Set up local AI…"), this);
    Theme::setRole(setup, QStringLiteral("primary"));
    setup->setToolTip(tr("Download the program and a model that suit this computer, or use the ones you have, and set everything up"));
    connect(setup, &QPushButton::clicked, this, [this] { openSetupWizard(m_studio, this, SetupWizard::Page::Local); });
    setupRow->addWidget(setup);
    v->addLayout(setupRow);

    v->addWidget(sectionLabel(tr("Programs"), this));
    m_runtimes = makeTree({tr("Program"), tr("Version"), tr("Build"), tr("Folder")}, this);
    v->addWidget(m_runtimes, 1);
    auto *rbar = new QHBoxLayout;
    auto *installBtn = new QPushButton(Icons::get(QStringLiteral("download")), tr("Download…"), this);
    Theme::setRole(installBtn, QStringLiteral("primary"));
    installBtn->setToolTip(tr("Download llama.cpp or stable-diffusion.cpp from GitHub"));
    auto *existing = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Add existing…"), this);
    existing->setToolTip(tr("Use llama-server, sd-server, ComfyUI or Stable Diffusion WebUI you installed yourself"));
    auto *updates = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Check for updates"), this);
    m_removeRuntime = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Remove"), this);
    Theme::setRole(m_removeRuntime, QStringLiteral("danger"));
    rbar->addWidget(installBtn);
    rbar->addWidget(existing);
    rbar->addWidget(updates);
    rbar->addStretch();
    rbar->addWidget(m_removeRuntime);
    v->addLayout(rbar);
    m_updates = mutedLabel(QString(), this);
    m_updates->setVisible(false);
    v->addWidget(m_updates);

    v->addWidget(sectionLabel(tr("Servers"), this));
    m_servers = makeTree({tr("Server"), tr("Program"), tr("Port"), tr("Starts"), tr("State")}, this);
    v->addWidget(m_servers, 1);
    auto *sbar = new QHBoxLayout;
    auto *newBtn = new QPushButton(Icons::get(QStringLiteral("plus")), tr("New server…"), this);
    Theme::setRole(newBtn, QStringLiteral("primary"));
    m_editServer = new QPushButton(Icons::get(QStringLiteral("edit")), tr("Edit…"), this);
    m_startServer = new QPushButton(Icons::get(QStringLiteral("play")), tr("Start"), this);
    m_stopServer = new QPushButton(Icons::get(QStringLiteral("stop")), tr("Stop"), this);
    m_logServer = new QPushButton(Icons::get(QStringLiteral("text")), tr("Log…"), this);
    m_removeServer = new QPushButton(Icons::get(QStringLiteral("trash")), tr("Remove"), this);
    Theme::setRole(m_removeServer, QStringLiteral("danger"));
    for (QPushButton *b : {newBtn, m_editServer, m_startServer, m_stopServer, m_logServer})
        sbar->addWidget(b);
    sbar->addStretch();
    sbar->addWidget(m_removeServer);
    v->addLayout(sbar);

    connect(installBtn, &QPushButton::clicked, this, &LocalServersPage::install);
    connect(existing, &QPushButton::clicked, this, &LocalServersPage::addExisting);
    connect(updates, &QPushButton::clicked, this, &LocalServersPage::checkUpdates);
    connect(m_removeRuntime, &QPushButton::clicked, this, &LocalServersPage::removeRuntime);
    connect(newBtn, &QPushButton::clicked, this, &LocalServersPage::newServer);
    connect(m_editServer, &QPushButton::clicked, this, [this] { editServer(selectedServer()); });
    connect(m_servers, &QTreeWidget::itemDoubleClicked, this, [this] { editServer(selectedServer()); });
    connect(m_startServer, &QPushButton::clicked, this, [this] {
        const QString id = selectedServer();
        QPointer<LocalServersPage> self(this);
        m_studio->servers()->start(id).then(this, [self, id](const QString &error) {
            if (self && !error.isEmpty())
                self->m_studio->notify(error.section(QLatin1Char('\n'), 0, 0), QStringLiteral("error"));
        });
    });
    connect(m_stopServer, &QPushButton::clicked, this, [this] { m_studio->servers()->stop(selectedServer()); });
    connect(m_logServer, &QPushButton::clicked, this, [this] {
        auto *dlg = new ServerLogDialog(m_studio, selectedServer(), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });
    connect(m_removeServer, &QPushButton::clicked, this, &LocalServersPage::removeServer);
    connect(m_runtimes, &QTreeWidget::currentItemChanged, this, [this] { m_removeRuntime->setEnabled(!selectedRuntime().isEmpty()); });
    connect(m_servers, &QTreeWidget::currentItemChanged, this, &LocalServersPage::updateServerButtons);

    connect(m_studio->runtimes(), &runtime::RuntimeStore::changed, this, [this] {
        reloadRuntimes();
        reloadServers();
    });
    connect(m_studio->servers(), &runtime::ServerSupervisor::profilesChanged, this, &LocalServersPage::reloadServers);
    connect(m_studio->servers(), &runtime::ServerSupervisor::stateChanged, this, &LocalServersPage::reloadServers);
    reloadRuntimes();
    reloadServers();

    QPointer<LocalServersPage> self(this);
    runtime::probeHardware().then(this, [self](runtime::HardwareInfo hw) {
        if (!self)
            return;
        self->m_hardware = hw;
        self->m_hardwareLabel->setText(tr("This computer: %1").arg(hw.summary()));
    });
}

void LocalServersPage::reloadRuntimes()
{
    const QString current = selectedRuntime();
    m_runtimes->clear();
    for (const runtime::InstalledRuntime &rt : m_studio->runtimes()->runtimes()) {
        auto *item = new QTreeWidgetItem(m_runtimes, {runtimeName(rt.kind), rt.version, buildLabel(rt), QDir::toNativeSeparators(rt.directory)});
        item->setData(0, Qt::UserRole, rt.id);
        item->setIcon(0, Icons::get(rt.managed ? QStringLiteral("download") : QStringLiteral("folder")));
        item->setToolTip(0, rt.versionInfo.isEmpty() ? rt.executable : rt.versionInfo);
        item->setToolTip(3, rt.executable);
        if (rt.id == current)
            m_runtimes->setCurrentItem(item);
    }
    for (int c = 0; c < 3; ++c)
        m_runtimes->resizeColumnToContents(c);
    m_removeRuntime->setEnabled(!selectedRuntime().isEmpty());
}

void LocalServersPage::reloadServers()
{
    const QString current = selectedServer();
    QSignalBlocker block(m_servers);
    m_servers->clear();
    runtime::ServerSupervisor *servers = m_studio->servers();
    for (const runtime::ServerProfile &p : servers->profiles()) {
        const auto rt = m_studio->runtimes()->runtime(p.runtimeId);
        QString state = servers->statusText(p.id);
        if (servers->needsRestart(p.id))
            state = tr("%1 — restart to apply changes").arg(state);
        auto *item = new QTreeWidgetItem(m_servers, {p.name, rt ? rt->displayName() : tr("(missing)"), QString::number(p.port),
                                                     startLabel(p.startPolicy), state});
        item->setData(0, Qt::UserRole, p.id);
        item->setIcon(0, Icons::get(QStringLiteral("server")));
        item->setForeground(4, stateColor(servers->state(p.id)));
        const QString error = servers->lastError(p.id);
        item->setToolTip(4, error.isEmpty() ? state : error);
        if (p.id == current)
            m_servers->setCurrentItem(item);
    }
    for (int c = 0; c < 4; ++c)
        m_servers->resizeColumnToContents(c);
    block.unblock();
    updateServerButtons();
}

void LocalServersPage::updateServerButtons()
{
    const QString id = selectedServer();
    const ServerState s = m_studio->servers()->state(id);
    const bool running = s == ServerState::Ready || s == ServerState::Starting;
    m_editServer->setEnabled(!id.isEmpty());
    m_logServer->setEnabled(!id.isEmpty());
    m_removeServer->setEnabled(!id.isEmpty());
    m_startServer->setEnabled(!id.isEmpty() && !running && s != ServerState::Stopping);
    m_stopServer->setEnabled(!id.isEmpty() && running);
}

QString LocalServersPage::selectedRuntime() const
{
    QTreeWidgetItem *item = m_runtimes->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

QString LocalServersPage::selectedServer() const
{
    QTreeWidgetItem *item = m_servers->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void LocalServersPage::install()
{
    InstallRuntimeDialog dlg(m_studio, m_hardware, QString(), this);
    dlg.exec();
}

void LocalServersPage::addExisting()
{
    QStringList known;
    for (const runtime::InstalledRuntime &rt : m_studio->runtimes()->runtimes())
        known << QDir::cleanPath(rt.executable).toLower();
    QMenu menu(this);
    for (const QString &path : runtime::findExistingInstalls()) {
        if (known.contains(QDir::cleanPath(path).toLower()))
            continue;
        QAction *a = menu.addAction(Icons::get(QStringLiteral("server")), QDir::toNativeSeparators(path));
        a->setData(path);
    }
    if (!menu.isEmpty())
        menu.addSeparator();
    QAction *browse = menu.addAction(Icons::get(QStringLiteral("folder")), tr("Browse…"));
    QWidget *anchor = qobject_cast<QWidget *>(sender());
    QAction *chosen = menu.exec(anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos());
    if (!chosen)
        return;
    QString path = chosen->data().toString();
    if (chosen == browse) {
        path = QFileDialog::getOpenFileName(this, tr("Choose a server program"), QString(),
                                            tr("Servers and trainers (llama-server.exe sd-server.exe llama-server sd-server main.py webui.bat sdxl_train_network.py)"));
    }
    if (path.isEmpty())
        return;
    QPointer<LocalServersPage> self(this);
    m_studio->runtimes()
        ->adopt(path)
        .then(this, [self](runtime::InstalledRuntime rt) {
            if (self)
                self->m_studio->notify(tr("%1 added.").arg(rt.displayName()), QStringLiteral("success"));
        })
        .onFailed(this, [self](const std::exception &e) {
            if (self)
                QMessageBox::warning(self, tr("Add existing"), describeException(e));
        });
}

void LocalServersPage::checkUpdates()
{
    m_updates->setVisible(true);
    m_updates->setText(tr("Checking GitHub…"));
    Theme::setRole(m_updates, QStringLiteral("muted"));
    QStringList kinds;
    for (const runtime::InstalledRuntime &rt : m_studio->runtimes()->runtimes()) {
        if (rt.managed && !kinds.contains(rt.kind))
            kinds << rt.kind;
    }
    if (kinds.isEmpty()) {
        m_updates->setText(tr("Only programs downloaded by Lumen can be updated here."));
        return;
    }
    auto pending = std::make_shared<int>(int(kinds.size()));
    auto found = std::make_shared<QList<runtime::InstallPlan>>();
    auto errors = std::make_shared<QStringList>();
    QPointer<LocalServersPage> self(this);
    auto done = [self, pending, found, errors] {
        if (--*pending > 0 || !self)
            return;
        if (found->isEmpty()) {
            self->m_updates->setText(errors->isEmpty() ? tr("Everything is up to date.") : errors->join(QLatin1Char('\n')));
            Theme::setRole(self->m_updates, errors->isEmpty() ? QStringLiteral("success") : QStringLiteral("error"));
            return;
        }
        QStringList lines;
        for (const runtime::InstallPlan &p : std::as_const(*found)) {
            const auto current = self->m_studio->runtimes()->latestManaged(p.runtimeId);
            lines << tr("%1 %2 is available (you have %3).").arg(runtimeName(p.runtimeId), p.tag, current ? current->version : QString());
        }
        self->m_updates->setText(lines.join(QLatin1Char('\n')));
        Theme::setRole(self->m_updates, QStringLiteral("muted"));
        if (QMessageBox::question(self, tr("Updates"),
                                  lines.join(QLatin1Char('\n')) + QStringLiteral("\n\n")
                                      + tr("Download now? Servers switch to the new version the next time they start; the current "
                                           "version is kept so you can go back."))
            != QMessageBox::Yes)
            return;
        for (const runtime::InstallPlan &p : std::as_const(*found)) {
            Job *job = self->m_studio->runtimes()->install(p);
            const QString kind = p.runtimeId;
            const QString backend = p.backendId;
            QObject::connect(job, &Job::finished, self, [self, kind, backend](Job *j) {
                if (!self || j->state() != Job::State::Succeeded)
                    return;
                // Move servers on the same build to the new version (the old one stays for rollback).
                const QString newId = j->result().toString();
                for (runtime::ServerProfile prof : self->m_studio->servers()->profiles()) {
                    const auto rt = self->m_studio->runtimes()->runtime(prof.runtimeId);
                    if (rt && rt->managed && rt->kind == kind && rt->backend == backend && rt->id != newId) {
                        prof.runtimeId = newId;
                        self->m_studio->servers()->updateProfile(prof);
                    }
                }
                self->m_studio->notify(tr("%1 updated. Servers use it from their next start.").arg(runtimeName(kind)), QStringLiteral("success"));
            });
        }
    };
    for (const QString &kind : std::as_const(kinds)) {
        const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(kind);
        const auto current = m_studio->runtimes()->latestManaged(kind);
        if (!spec || !current) {
            done();
            continue;
        }
        m_studio->runtimes()
            ->feed()
            ->fetch(*spec)
            .then(this, [self, spec, current, found, done](QList<runtime::Release> releases) {
                if (self) {
                    const runtime::InstallPlan plan = runtime::planInstall(*spec, current->backend, releases);
                    if (plan.ok() && plan.tag != current->version
                        && !self->m_studio->runtimes()->runtime(spec->id + QLatin1Char('/') + plan.tag + QLatin1Char('-') + plan.backendId))
                        found->append(plan);
                }
                done();
            })
            .onFailed(this, [errors, done](const std::exception &e) {
                errors->append(describeException(e));
                done();
            });
    }
}

void LocalServersPage::removeRuntime()
{
    const QString id = selectedRuntime();
    const auto rt = m_studio->runtimes()->runtime(id);
    if (!rt)
        return;
    QStringList users;
    for (const runtime::ServerProfile &p : m_studio->servers()->profiles()) {
        if (p.runtimeId == id)
            users << p.name;
    }
    if (!users.isEmpty()) {
        QMessageBox::information(this, tr("Remove"), tr("%1 is used by: %2. Remove those servers or switch them to another version first.")
                                                         .arg(rt->displayName(), users.join(QStringLiteral(", "))));
        return;
    }
    const QString question = rt->managed ? tr("Delete %1 and its files (%2)?").arg(rt->displayName(), QDir::toNativeSeparators(rt->directory))
                                         : tr("Forget %1? The program itself stays where it is.").arg(rt->displayName());
    if (QMessageBox::question(this, tr("Remove"), question) != QMessageBox::Yes)
        return;
    QString error;
    if (!m_studio->runtimes()->remove(id, &error))
        QMessageBox::warning(this, tr("Remove"), error);
}

void LocalServersPage::newServer()
{
    QString runtimeId = selectedRuntime();
    QList<runtime::InstalledRuntime> all;
    for (const runtime::InstalledRuntime &rt : m_studio->runtimes()->runtimes()) {
        if (!runtime::isTrainerKind(rt.kind))
            all << rt; // trainers run training jobs, not servers
    }
    if (const auto selected = m_studio->runtimes()->runtime(runtimeId); selected && runtime::isTrainerKind(selected->kind))
        runtimeId.clear();
    if (all.isEmpty()) {
        if (QMessageBox::question(this, tr("New server"), tr("No server program is installed yet. Download one now?")) == QMessageBox::Yes)
            install();
        return;
    }
    if (runtimeId.isEmpty())
        runtimeId = all.last().id;
    ServerProfileDialog dlg(m_studio, m_studio->servers()->createProfile(runtimeId), true, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    addServer(m_studio, dlg.profile(), dlg.createProvider());
}

void addServer(Studio *studio, const runtime::ServerProfile &p, bool createProvider)
{
    studio->servers()->addProfile(p);
    if (createProvider) {
        const QString kind = studio->servers()->kindOf(p.id);
        QVariantMap settings{{QStringLiteral("url"), runtime::providerUrl(kind, p.port)}, {QStringLiteral("managed_server"), p.id}};
        if (ai::Provider *provider = studio->providers()->addProvider(runtime::providerTypeFor(kind), p.name, settings)) {
            // Roles nobody handles yet go to this server. A vision model takes every role: on one
            // GPU, switching between a vision and a text model costs a reload each time (measured:
            // twice the time of the reference scenario), and the vision model handled tool calls better.
            if (provider->kind() == ai::ProviderKind::Llm) {
                const QList<runtime::LlamaModel> models = runtime::llamaModels(p);
                QString vision;
                QString text;
                for (const runtime::LlamaModel &m : models) {
                    if (m.vision() && vision.isEmpty())
                        vision = m.name;
                    if (!m.vision() && text.isEmpty())
                        text = m.name;
                }
                if (text.isEmpty())
                    text = vision;
                for (ai::LlmRole role : ai::allLlmRoles()) {
                    if (!studio->providers()->role(role).providerId.isEmpty())
                        continue;
                    const QString model = vision.isEmpty() ? text : vision;
                    studio->providers()->setRole(role, {provider->instanceId(), model});
                }
            }
            studio->notify(QCoreApplication::translate("lumen::ui::LocalServersPage", "%1 added as a provider. It starts when Lumen first uses it.").arg(p.name),
                           QStringLiteral("success"));
        }
    }
}

void LocalServersPage::editServer(const QString &id)
{
    const auto p = m_studio->servers()->profile(id);
    if (!p)
        return;
    ServerProfileDialog dlg(m_studio, *p, false, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_studio->servers()->updateProfile(dlg.profile());
    if (m_studio->servers()->needsRestart(id)
        && QMessageBox::question(this, tr("Restart"), tr("Restart %1 now to apply the changes?").arg(p->name)) == QMessageBox::Yes)
        m_studio->servers()->restart(id);
}

void LocalServersPage::removeServer()
{
    const QString id = selectedServer();
    const auto p = m_studio->servers()->profile(id);
    if (!p || QMessageBox::question(this, tr("Remove"), tr("Stop and remove the server '%1'?").arg(p->name)) != QMessageBox::Yes)
        return;
    // Providers bound to it keep their settings but no longer start anything.
    for (ai::Provider *provider : m_studio->providers()->providers()) {
        if (provider->text(QStringLiteral("managed_server")) == id) {
            QVariantMap s = m_studio->providers()->settingsFor(provider->instanceId());
            s.insert(QStringLiteral("managed_server"), QString());
            m_studio->providers()->updateProvider(provider->instanceId(), provider->displayName(), s);
        }
    }
    m_studio->servers()->removeProfile(id);
}

// ---- InstallRuntimeDialog -------------------------------------------------------------------------

InstallRuntimeDialog::InstallRuntimeDialog(Studio *studio, const std::optional<runtime::HardwareInfo> &hw, const QString &runtimeId,
                                           QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_hw(hw)
{
    setWindowTitle(tr("Download a server program"));
    resize(560, 360);
    auto *v = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_runtime = new QComboBox(this);
    for (const runtime::RuntimeSpec &r : runtime::Catalog::instance().runtimes) {
        if (r.adoptOnly)
            continue;
        m_runtime->addItem(Icons::get(QStringLiteral("server")), r.name, r.id);
        m_runtime->setItemData(m_runtime->count() - 1, r.description, Qt::ToolTipRole);
    }
    // LoRA trainers install into a Python environment instead of from a release archive.
    for (const runtime::TrainerRecipe &t : runtime::TrainerCatalog::instance().trainers) {
        m_runtime->addItem(Icons::get(QStringLiteral("star")), tr("%1 (LoRA trainer)").arg(t.name), QStringLiteral("trainer:") + t.id);
        m_runtime->setItemData(m_runtime->count() - 1, t.description, Qt::ToolTipRole);
    }
    if (!runtimeId.isEmpty()) {
        int index = m_runtime->findData(runtimeId);
        if (index < 0)
            index = m_runtime->findData(QStringLiteral("trainer:") + runtimeId);
        m_runtime->setCurrentIndex(std::max(0, index));
    }
    form->addRow(tr("Program"), m_runtime);
    m_backend = new QComboBox(this);
    form->addRow(tr("Build"), m_backend);
    v->addLayout(form);
    m_about = mutedLabel(QString(), this);
    m_about->setOpenExternalLinks(true);
    m_about->setTextFormat(Qt::RichText);
    v->addWidget(m_about);
    m_release = new QLabel(this);
    m_release->setWordWrap(true);
    v->addWidget(m_release);
    v->addStretch();
    v->addWidget(mutedLabel(tr("Files are downloaded from the project's GitHub releases, checked against the published SHA-256 "
                               "checksums and unpacked into Lumen's data folder."),
                            this));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_install = buttons->addButton(tr("Download and install"), QDialogButtonBox::AcceptRole);
    Theme::setRole(m_install, QStringLiteral("primary"));
    m_install->setEnabled(false);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_install, &QPushButton::clicked, this, [this] {
        if (!trainerId().isEmpty()) {
            const runtime::TrainerRecipe *t = runtime::TrainerCatalog::instance().trainer(trainerId());
            m_studio->runtimes()->installTrainer(trainerId(), m_backend->currentData().toString());
            m_studio->notify(tr("Installing %1 — progress is shown in the Queue.").arg(t ? t->name : trainerId()));
            accept();
            return;
        }
        if (!m_plan.ok())
            return;
        m_studio->runtimes()->install(m_plan);
        m_studio->notify(tr("Installing %1 %2 — progress is shown in the Queue.").arg(runtimeName(m_plan.runtimeId), m_plan.tag));
        accept();
    });
    connect(m_runtime, &QComboBox::currentIndexChanged, this, &InstallRuntimeDialog::fetch);
    connect(m_backend, &QComboBox::currentIndexChanged, this, &InstallRuntimeDialog::updatePlan);
    if (!m_hw) {
        QPointer<InstallRuntimeDialog> self(this);
        runtime::probeHardware().then(this, [self](runtime::HardwareInfo info) {
            if (!self)
                return;
            self->m_hw = info;
            self->fetch();
        });
    }
    fetch();
}

QString InstallRuntimeDialog::trainerId() const
{
    const QString data = m_runtime->currentData().toString();
    return data.startsWith(QLatin1String("trainer:")) ? data.mid(8) : QString();
}

void InstallRuntimeDialog::fetch()
{
    if (const runtime::TrainerRecipe *t = runtime::TrainerCatalog::instance().trainer(trainerId())) {
        QString why;
        const QString recommended = m_hw ? runtime::recommendedTrainerVariant(*t, *m_hw, &why) : QString();
        {
            QSignalBlocker block(m_backend);
            m_backend->clear();
            for (const runtime::TrainerVariant &v : t->variants)
                m_backend->addItem(v.id == recommended ? v.label + tr(" (recommended)") : v.label, v.id);
            m_backend->setCurrentIndex(std::max(0, m_backend->findData(recommended)));
        }
        m_about->setText(QStringLiteral("%1<br>%2 · <a href='%3'>%3</a>").arg(t->description.toHtmlEscaped(), tr("Licence: %1").arg(t->license),
                                                                              t->homepage.toString()));
        updatePlan();
        if (m_hw && recommended.isEmpty()) {
            m_release->setText(m_release->text() + QStringLiteral("<br><span style='color:%1'>%2</span>").arg(Theme::palette().warning.name(), why.toHtmlEscaped()));
        }
        return;
    }
    const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(m_runtime->currentData().toString());
    if (!spec)
        return;
    {
        QSignalBlocker block(m_backend);
        m_backend->clear();
        const QString recommended = m_hw ? runtime::recommendedBackend(*spec, *m_hw) : QString();
        for (const runtime::BackendSpec &b : spec->backends) {
            QString label = b.label;
            QString why;
            if (b.id == recommended)
                label += tr(" (recommended)");
            else if (m_hw && !runtime::backendSupported(b, *m_hw, &why))
                label += QStringLiteral(" — ") + why;
            m_backend->addItem(label, b.id);
        }
        m_backend->setCurrentIndex(std::max(0, m_backend->findData(recommended)));
    }
    m_about->setText(QStringLiteral("%1<br>%2 · <a href='%3'>%3</a>").arg(spec->description.toHtmlEscaped(), tr("Licence: %1").arg(spec->license),
                                                                          spec->homepage.toString()));
    if (m_releases.contains(spec->id)) {
        updatePlan();
        return;
    }
    m_release->setText(tr("Looking up the newest release on GitHub…"));
    Theme::setRole(m_release, QStringLiteral("muted"));
    m_install->setEnabled(false);
    QPointer<InstallRuntimeDialog> self(this);
    const QString id = spec->id;
    m_studio->runtimes()
        ->feed()
        ->fetch(*spec)
        .then(this, [self, id](QList<runtime::Release> releases) {
            if (!self)
                return;
            self->m_releases.insert(id, releases);
            self->updatePlan();
        })
        .onFailed(this, [self](const std::exception &e) {
            if (!self)
                return;
            self->m_release->setText(describeException(e));
            Theme::setRole(self->m_release, QStringLiteral("error"));
        });
}

void InstallRuntimeDialog::updatePlan()
{
    if (const runtime::TrainerRecipe *t = runtime::TrainerCatalog::instance().trainer(trainerId())) {
        const runtime::TrainerVariant *v = t->variant(m_backend->currentData().toString());
        if (!v)
            return;
        QStringList parts;
        parts << tr("Version <b>%1</b>, Python %2, PyTorch %3.").arg(t->version, t->python, v->torch);
        parts << tr("Download: about %1 (PyTorch with CUDA is most of it); about %2 on disk.")
                     .arg(runtime::formatBytes(v->downloadSize), runtime::formatBytes(v->downloadSize * 2));
        parts << tr("Every file is checked: uv and the trainer against pinned SHA-256 checksums, the Python packages against a "
                    "hashed lock file.");
        const bool installed = m_studio->runtimes()->runtime(QStringLiteral("%1/%2-%3").arg(t->id, t->version, v->id)).has_value();
        if (installed)
            parts << tr("This version is already installed.");
        m_release->setText(parts.join(QStringLiteral("<br>")));
        Theme::setRole(m_release, QString());
        m_install->setText(installed ? tr("Reinstall") : tr("Download and install"));
        m_install->setEnabled(true);
        return;
    }
    const runtime::RuntimeSpec *spec = runtime::Catalog::instance().runtime(m_runtime->currentData().toString());
    if (!spec || !m_releases.contains(spec->id))
        return;
    const QString backendId = m_backend->currentData().toString();
    m_plan = runtime::planInstall(*spec, backendId, m_releases.value(spec->id));
    if (!m_plan.ok()) {
        m_release->setText(m_plan.error);
        Theme::setRole(m_release, QStringLiteral("error"));
        m_install->setEnabled(false);
        return;
    }
    QStringList parts;
    parts << tr("Release <b>%1</b> from %2.").arg(m_plan.tag, QLocale().toString(m_plan.published.date(), QLocale::ShortFormat));
    const qint64 pending = m_studio->runtimes()->pendingDownload(m_plan);
    QString size = tr("Download: %1").arg(runtime::formatBytes(pending));
    if (!m_plan.libraries.isEmpty()) {
        size += pending < m_plan.downloadSize() ? tr(" (the CUDA runtime is already installed)")
                                                : tr(" (including the CUDA runtime, %1, shared by later versions)")
                                                      .arg(runtime::formatBytes(m_plan.downloadSize() - m_plan.main.size));
    }
    parts << size + QLatin1Char('.');
    QString why;
    const runtime::BackendSpec *b = spec->backend(backendId);
    if (m_hw && b && !runtime::backendSupported(*b, *m_hw, &why))
        parts << QStringLiteral("<span style='color:%1'>%2</span>").arg(Theme::palette().warning.name(), tr("Warning: this build %1.").arg(why));
    const bool installed = m_studio->runtimes()->runtime(spec->id + QLatin1Char('/') + m_plan.tag + QLatin1Char('-') + backendId).has_value();
    if (installed)
        parts << tr("This version is already installed.");
    m_release->setText(parts.join(QStringLiteral("<br>")));
    Theme::setRole(m_release, QString());
    m_install->setText(installed ? tr("Reinstall") : tr("Download and install"));
    m_install->setEnabled(true);
}

// ---- ServerProfileDialog --------------------------------------------------------------------------

namespace {

// A path field that also takes a file dropped on it (a line edit would insert its URL as text).
class PathEdit : public QLineEdit
{
public:
    PathEdit(const QString &text, QWidget *parent)
        : QLineEdit(text, parent)
    {
    }

    std::function<void(const QString &)> dropped;

protected:
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (!localPath(e->mimeData()).isEmpty())
            e->acceptProposedAction();
        else
            QLineEdit::dragEnterEvent(e);
    }
    void dragMoveEvent(QDragMoveEvent *e) override
    {
        if (!localPath(e->mimeData()).isEmpty())
            e->acceptProposedAction();
        else
            QLineEdit::dragMoveEvent(e);
    }
    void dropEvent(QDropEvent *e) override
    {
        const QString path = localPath(e->mimeData());
        if (path.isEmpty()) {
            QLineEdit::dropEvent(e);
            return;
        }
        e->acceptProposedAction();
        setText(QDir::toNativeSeparators(path));
        if (dropped)
            dropped(path);
    }

private:
    static QString localPath(const QMimeData *mime)
    {
        if (!mime || !mime->hasUrls() || mime->urls().size() != 1)
            return {};
        return mime->urls().first().toLocalFile();
    }
};

// A field's status line under it: wrapped, and as tall as its text at the width it has (a form
// gives a label the height of its size hint, which QLabel works out for a narrower width, so a
// line of text stood in a box three lines high).
class StatusLabel : public QLabel
{
public:
    explicit StatusLabel(QWidget *parent)
        : QLabel(parent)
    {
        setProperty("role", QStringLiteral("muted"));
        setWordWrap(true);
        setTextFormat(Qt::RichText);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    QSize sizeHint() const override { return QSize(0, width() > 0 ? heightForWidth(width()) : QLabel::sizeHint().height()); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QLabel::resizeEvent(e);
        if (e->size().width() != e->oldSize().width())
            updateGeometry();
    }
};

// A choice in a scrolling form that the mouse wheel changes only once it has been clicked: turning
// the wheel over it to scroll the form must not change it (a model type changed in passing hides
// the files of the model).
class WheelSafeCombo : public QComboBox
{
public:
    explicit WheelSafeCombo(QWidget *parent)
        : QComboBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void wheelEvent(QWheelEvent *e) override
    {
        if (hasFocus())
            QComboBox::wheelEvent(e);
        else
            e->ignore(); // to the scroll area
    }
};

// A field's status: a mark in the colour of its state, then the words.
QString statusLine(const QString &mark, const QColor &color, const QString &text)
{
    return QStringLiteral("<span style='color:%1'>%2</span>&nbsp;%3").arg(color.name(), mark, text.toHtmlEscaped());
}

QString cleanPath(const QString &path)
{
    return path.trimmed().isEmpty() ? QString() : QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
}

QFormLayout *newForm()
{
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return form;
}

bool isFileField(const OptionField &f)
{
    return f.type == OptionField::Type::File || f.type == OptionField::Type::Directory;
}

// sd.cpp's two slots for the model file.
bool isModelSlot(const QString &key)
{
    return key == QLatin1String("model") || key == QLatin1String("diffusion_model");
}

// sd.cpp file options that are add-ons rather than the model's own files.
bool isAddon(const QString &key)
{
    static const QStringList addons{QStringLiteral("control_net"), QStringLiteral("ip_adapter"), QStringLiteral("clip_vision"),
                                    QStringLiteral("lora_dir"), QStringLiteral("upscalers_dir")};
    return addons.contains(key);
}

} // namespace

ServerProfileDialog::ServerProfileDialog(Studio *studio, const runtime::ServerProfile &profile, bool isNew, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_profile(profile)
    , m_isNew(isNew)
{
    setWindowTitle(isNew ? tr("New server") : tr("Server settings"));
    auto *outer = new QVBoxLayout(this);
    // Everything but the buttons scrolls, so they stay in sight on a small screen.
    m_content = new QWidget(this);
    m_scroll = verticalScroll(m_content, this);
    outer->addWidget(m_scroll, 1);
    auto *v = new QVBoxLayout(m_content);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(8);

    m_form = newForm();
    m_name = new QLineEdit(profile.name, m_content);
    m_form->addRow(tr("Name"), m_name);
    m_runtime = new QComboBox(m_content);
    for (const runtime::InstalledRuntime &rt : m_studio->runtimes()->runtimes())
        m_runtime->addItem(Icons::get(QStringLiteral("server")), rt.displayName(), rt.id);
    m_runtime->setCurrentIndex(std::max(0, m_runtime->findData(profile.runtimeId)));
    m_form->addRow(tr("Program"), m_runtime);
    v->addLayout(m_form);

    // The program's fields: for sd.cpp the model's type, the files it needs and the add-ons it can
    // use (buildOptions).
    m_optionsHost = new QWidget(m_content);
    auto *hostLayout = new QVBoxLayout(m_optionsHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_optionsHost);

    v->addWidget(sectionLabel(tr("Server"), m_content));
    m_memoryForm = newForm();
    m_preset = new QComboBox(m_content);
    m_preset->setObjectName(QStringLiteral("serverMemory"));
    m_preset->addItem(tr("Up to 8 GB of memory"), int(runtime::VramTier::Small));
    m_preset->addItem(tr("12 GB of memory"), int(runtime::VramTier::Medium));
    m_preset->addItem(tr("16 GB of memory or more"), int(runtime::VramTier::Large));
    m_preset->addItem(tr("Custom (set under Advanced)"), -1);
    m_preset->setToolTip(tr("Sets the memory options under Advanced for a graphics card with this much memory. Measured on an 8 GB "
                            "laptop GPU."));
    m_memoryForm->addRow(tr("Graphics card"), m_preset);
    m_port = new QSpinBox(m_content);
    m_port->setRange(1024, 65535);
    m_port->setValue(profile.port);
    m_port->setToolTip(tr("Lumen talks to the server at this port of this computer."));
    m_memoryForm->addRow(tr("Port"), m_port);
    v->addLayout(m_memoryForm);
    if (isNew) {
        m_createProvider = new QCheckBox(tr("Add it as a provider"), m_content);
        m_createProvider->setChecked(true);
        v->addWidget(m_createProvider);
    }
    // What keeps the server from working and has no field of its own to stand next to.
    m_problem = new QLabel(m_content);
    m_problem->setObjectName(QStringLiteral("problems"));
    m_problem->setWordWrap(true);
    m_problem->setTextFormat(Qt::RichText);
    Theme::setRole(m_problem, QStringLiteral("error"));
    v->addWidget(m_problem);

    // Folded: the launch settings, when the server starts and stops, and its command line.
    m_advancedToggle = new QToolButton(m_content);
    m_advancedToggle->setObjectName(QStringLiteral("serverAdvanced"));
    m_advancedToggle->setText(tr("Advanced"));
    m_advancedToggle->setToolTip(tr("Memory and speed settings, extra arguments, when the server starts and stops, and its command line"));
    m_advancedToggle->setCheckable(true);
    m_advancedToggle->setAutoRaise(true);
    m_advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_advancedToggle->setArrowType(Qt::RightArrow);
    v->addWidget(m_advancedToggle);
    m_advanced = new QWidget(m_content);
    m_advanced->setObjectName(QStringLiteral("serverAdvancedOptions"));
    auto *av = new QVBoxLayout(m_advanced);
    av->setContentsMargins(0, 0, 0, 0);
    av->setSpacing(8);
    m_advancedHost = new QWidget(m_advanced);
    auto *advancedHostLayout = new QVBoxLayout(m_advancedHost);
    advancedHostLayout->setContentsMargins(0, 0, 0, 0);
    av->addWidget(m_advancedHost);
    av->addWidget(sectionLabel(tr("Starting and stopping"), m_advanced));
    QFormLayout *life = newForm();
    m_start = new QComboBox(m_advanced);
    for (runtime::StartPolicy p : {runtime::StartPolicy::OnDemand, runtime::StartPolicy::AtStartup, runtime::StartPolicy::Manual})
        m_start->addItem(startLabel(p), runtime::startPolicyKey(p));
    m_start->setCurrentIndex(std::max(0, m_start->findData(runtime::startPolicyKey(profile.startPolicy))));
    m_start->setToolTip(tr("“When needed” starts the server on the first request that uses it."));
    life->addRow(tr("Starts"), m_start);
    m_idle = new QSpinBox(m_advanced);
    m_idle->setRange(0, 24 * 60);
    m_idle->setSuffix(tr(" min"));
    m_idle->setSpecialValueText(tr("Never"));
    m_idle->setValue(profile.idleStopMinutes);
    m_idle->setToolTip(tr("Stops the server after it was unused for this long, freeing memory."));
    life->addRow(tr("Stop when idle"), m_idle);
    av->addLayout(life);
    av->addWidget(sectionLabel(tr("Command line"), m_advanced));
    m_preview = new QPlainTextEdit(m_advanced);
    m_preview->setReadOnly(true);
    m_preview->setAccessibleName(tr("Command line"));
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setFixedHeight(84);
    av->addWidget(m_preview);
    m_advanced->setVisible(false);
    v->addWidget(m_advanced);
    v->addStretch();

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    outer->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_ok = m_buttons->button(QDialogButtonBox::Ok);
    m_ok->setEnabled(m_runtime->count() > 0);

    connect(m_advancedToggle, &QToolButton::toggled, this, &ServerProfileDialog::showAdvanced);
    // A finished download fills its field (the launch settings stay as they are).
    connect(m_studio->hub(), &runtime::ModelHub::downloaded, this, [this] {
        const QString path = modelPath();
        if (path.isEmpty())
            return;
        for (const QString &key : {QStringLiteral("diffusion_model"), QStringLiteral("model")}) {
            if (cleanPath(fieldText(key)) == path) {
                applyModel(key, path, false);
                return;
            }
        }
    });
    connect(m_runtime, &QComboBox::currentIndexChanged, this, [this] {
        m_profile = this->profile(); // keep typed values
        buildOptions();
        fitToContent();
    });
    connect(m_name, &QLineEdit::textChanged, this, &ServerProfileDialog::updatePreview);
    connect(m_preset, &QComboBox::activated, this, [this] {
        m_tierPicked = true;
        applyPreset();
    });
    if (isNew) {
        // New servers start tuned for this computer's GPU, unless a size was chosen meanwhile.
        QPointer<ServerProfileDialog> self(this);
        runtime::probeHardware().then(this, [self](runtime::HardwareInfo hw) {
            if (!self || self->m_tierPicked)
                return;
            self->m_preset->setCurrentIndex(self->m_preset->findData(int(runtime::vramTierFor(hw.maxVram()))));
            self->applyPreset();
        });
    }
    connect(m_port, &QSpinBox::valueChanged, this, &ServerProfileDialog::updatePreview);
    buildOptions();
    fitToContent();
}

QVariantMap ServerProfileDialog::tierOptions(int tier) const
{
    if (tier < 0)
        return {};
    QVariantMap out = runtime::presetOptions(kind(), runtime::VramTier(tier));
    // The model's own settings for the size (FLUX.1 on 8 GB: q4_K, text encoders on the CPU).
    const QString path = modelPath();
    if (kind() == QLatin1String("sd.cpp") && !path.isEmpty()) {
        if (const std::optional<runtime::ModelInfo> mi = runtime::modelInfoOf(m_studio->models(), path)) {
            if (const runtime::ModelPreset *preset = runtime::presetFor(*mi, path))
                out.insert(preset->vram.value(runtime::VramTier(tier)));
        }
    }
    return out;
}

void ServerProfileDialog::applyPreset()
{
    const int tier = m_preset->currentData().toInt();
    if (tier < 0)
        return; // Custom: the options under Advanced stay as they are
    const QVariantMap preset = tierOptions(tier);
    for (auto it = preset.cbegin(); it != preset.cend(); ++it)
        setField(it.key(), it.value());
    updatePreview();
}

void ServerProfileDialog::selectTier()
{
    const runtime::ServerProfile p = profile();
    const QString k = kind();
    int found = -1;
    for (runtime::VramTier tier : {runtime::VramTier::Small, runtime::VramTier::Medium, runtime::VramTier::Large}) {
        const QVariantMap expected = tierOptions(int(tier));
        if (expected.isEmpty())
            continue;
        bool same = true;
        for (auto it = expected.cbegin(); it != expected.cend() && same; ++it)
            same = p.option(it.key(), k).toString() == it.value().toString();
        if (same) {
            found = int(tier);
            break;
        }
    }
    const QSignalBlocker block(m_preset);
    m_preset->setCurrentIndex(std::max(0, m_preset->findData(found)));
}

void ServerProfileDialog::updatePresetRow()
{
    const bool hasPresets = !runtime::presetOptions(kind(), runtime::VramTier::Small).isEmpty();
    m_memoryForm->setRowVisible(m_preset, hasPresets);
}

QString ServerProfileDialog::kind() const
{
    const auto rt = m_studio->runtimes()->runtime(m_runtime->currentData().toString());
    return rt ? rt->kind : QString();
}

QString ServerProfileDialog::modelType() const
{
    const runtime::ModelType *type = currentType();
    return type ? type->id : QString();
}

void ServerProfileDialog::showAdvanced(bool on)
{
    m_advancedToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    m_advanced->setVisible(on);
    fitToContent();
    if (!on)
        return;
    // What unfolds comes into sight (once the content has its new height).
    QMetaObject::invokeMethod(
        this,
        [this] {
            QScrollBar *bar = m_scroll->verticalScrollBar();
            if (m_advanced->geometry().bottom() > bar->value() + m_scroll->viewport()->height())
                bar->setValue(std::min(bar->maximum(), m_advancedToggle->y() - 8));
        },
        Qt::QueuedConnection);
}

void ServerProfileDialog::alignForms()
{
    // The labels of every form (name and program, the files, the add-ons, the server, Advanced) in
    // one column, as wide as the widest shown.
    QList<QLabel *> labels;
    int width = 0;
    for (QFormLayout *form : m_content->findChildren<QFormLayout *>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
            auto *label = item ? qobject_cast<QLabel *>(item->widget()) : nullptr;
            if (!label)
                continue;
            label->setMinimumWidth(0);
            labels << label;
            if (!label->isHidden())
                width = std::max(width, label->sizeHint().width());
        }
    }
    for (QLabel *label : std::as_const(labels)) {
        label->setMinimumWidth(width);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
}

void ServerProfileDialog::fitToContent()
{
    const QMargins m = layout()->contentsMargins();
    const QSize content = m_content->sizeHint();
    // Room for the scroll bar, so the fields keep their width when the content scrolls.
    const int w = std::max({640, content.width() + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + m.left() + m.right(),
                            isVisible() ? width() : 0});
    // Wrapped text as tall as it is at that width.
    const int contentHeight = m_content->hasHeightForWidth() ? m_content->heightForWidth(w - m.left() - m.right()) : content.height();
    const int h = contentHeight + m_buttons->sizeHint().height() + layout()->spacing() + m.top() + m.bottom();
    resizeWithinScreen(this, QSize(w, h));
}

void ServerProfileDialog::buildOptions()
{
    const QString k = kind();
    if (k == m_builtFor && !m_fields.isEmpty()) {
        updatePreview();
        return;
    }
    m_builtFor = k;
    m_fields.clear();
    m_rows.clear();
    m_status.clear();
    m_type = nullptr;
    m_fileForm = nullptr;
    m_addonForm = nullptr;
    m_addonsLabel = nullptr;
    m_downloadMissing = nullptr;
    m_downloadRow = nullptr;
    for (QWidget *host : {m_optionsHost, m_advancedHost}) {
        clearLayout(host->layout());
    }
    updatePresetRow();
    auto *box = new QWidget(m_optionsHost);
    auto *bv = new QVBoxLayout(box);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->setSpacing(8);
    auto *advancedBox = new QWidget(m_advancedHost);
    QFormLayout *advanced = newForm();
    advancedBox->setLayout(advanced);
    advanced->setContentsMargins(0, 0, 0, 0);
    auto addOption = [this](QFormLayout *form, const OptionField &f, QWidget *parent) {
        // Check boxes carry their own label.
        const QString label = f.type == OptionField::Type::Boolean ? QString() : f.label + (f.required ? QStringLiteral(" *") : QString());
        form->addRow(label, isFileField(f) ? fileField(f, parent) : optionField(f, parent));
    };

    const QList<OptionField> fields = runtime::serverOptions(k);
    if (k == QLatin1String("sd.cpp")) {
        // The model's type decides which of the files show (plan §17.5): the model in its slot, the
        // components its preset needs, the add-ons it can use. Each file has a status line under it.
        bv->addWidget(sectionLabel(tr("Model"), box));
        m_fileForm = newForm();
        bv->addLayout(m_fileForm);
        m_type = new WheelSafeCombo(box);
        m_type->setObjectName(QStringLiteral("serverModelType"));
        m_type->setAccessibleName(tr("Model type"));
        m_type->setToolTip(tr("What kind of model the server runs: it decides which files are asked for. Choosing a model file sets it; "
                              "Other shows every field, for models Lumen does not know."));
        for (const runtime::ModelType &t : runtime::modelTypes())
            m_type->addItem(t.name, t.id);
        m_type->addItem(tr("Other (every field)"), QString());
        m_fileForm->addRow(tr("Model type"), m_type);
        m_addonsLabel = sectionLabel(tr("Add-ons"), box);
        m_addonForm = newForm();
        for (const OptionField &f : fields) {
            if (!isFileField(f)) {
                addOption(advanced, f, advancedBox);
                continue;
            }
            QFormLayout *form = isAddon(f.key) ? m_addonForm : m_fileForm;
            form->addRow(f.label, fileField(f, box));
            auto *status = new StatusLabel(box);
            status->setObjectName(QStringLiteral("status-%1").arg(f.key));
            m_status.insert(f.key, status);
            form->addRow(QString(), status);
            form->setRowVisible(status, false);
            if (f.key == QLatin1String("llm")) {
                // After the components: what is missing, in one download.
                m_downloadRow = new QWidget(box);
                auto *h = new QHBoxLayout(m_downloadRow);
                h->setContentsMargins(0, 0, 0, 0);
                m_downloadMissing = new QPushButton(Icons::get(QStringLiteral("download")), QString(), m_downloadRow);
                m_downloadMissing->setObjectName(QStringLiteral("downloadMissing"));
                h->addWidget(m_downloadMissing);
                h->addStretch();
                m_fileForm->addRow(QString(), m_downloadRow);
                m_fileForm->setRowVisible(m_downloadRow, false);
                connect(m_downloadMissing, &QPushButton::clicked, this, &ServerProfileDialog::downloadMissing);
            }
        }
        bv->addWidget(m_addonsLabel);
        bv->addLayout(m_addonForm);
        connect(m_type, &QComboBox::activated, this, &ServerProfileDialog::typeChosen);
        // An existing profile opens with its model's type; Other when Lumen does not know the model
        // or the profile has files its type has no place for. A server without any file yet (a new
        // one) starts on a type, so it asks only for that type's files rather than for every one.
        const runtime::ModelType *type = runtime::profileModelType(m_studio->models(), m_profile);
        const QStringList fileOptions = runtime::sdFileOptions();
        if (!type && std::none_of(fileOptions.cbegin(), fileOptions.cend(),
                                  [this](const QString &key) { return !m_profile.options.value(key).toString().trimmed().isEmpty(); }))
            type = runtime::defaultModelType(m_studio->models());
        const QSignalBlocker block(m_type);
        m_type->setCurrentIndex(std::max(0, m_type->findData(type ? type->id : QString())));
    } else {
        bv->addWidget(sectionLabel(tr("%1 options").arg(runtimeName(k)), box));
        QFormLayout *form = newForm();
        bv->addLayout(form);
        for (const OptionField &f : fields) {
            if (f.advanced)
                addOption(advanced, f, advancedBox);
            else
                addOption(form, f, box);
        }
    }
    m_optionsHost->layout()->addWidget(box);
    m_advancedHost->layout()->addWidget(advancedBox);
    // A llama.cpp model name defaults to the file name.
    auto *modelEdit = qobject_cast<QLineEdit *>(m_fields.value(QStringLiteral("model")));
    auto *nameEdit = qobject_cast<QLineEdit *>(m_fields.value(QStringLiteral("model_name")));
    if (modelEdit && nameEdit) {
        auto sync = [modelEdit, nameEdit] { nameEdit->setPlaceholderText(runtime::llamaModelName(modelEdit->text())); };
        connect(modelEdit, &QLineEdit::textChanged, nameEdit, sync);
        sync();
    }

    // Providers exist for llama.cpp; the sd.cpp provider comes with the next step of M12.
    if (m_createProvider) {
        const bool available = m_studio->providers()->type(runtime::providerTypeFor(k)).has_value();
        m_createProvider->setEnabled(available);
        m_createProvider->setChecked(available);
        m_createProvider->setToolTip(available ? QString() : tr("Lumen has no provider for %1 yet.").arg(runtimeName(k)));
    }
    showType();
    alignForms();
    updatePreview();
}

QWidget *ServerProfileDialog::fileField(const OptionField &f, QWidget *parent)
{
    const QString k = kind();
    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto *edit = new PathEdit(m_profile.option(f.key, k).toString(), row);
    edit->setProperty("optionKey", f.key);
    edit->setPlaceholderText(f.required ? tr("required") : tr("none"));
    edit->setAccessibleName(f.label);
    edit->setMinimumWidth(260);
    h->addWidget(edit, 1);
    if (!modelKindsForOption(k, f.key).isEmpty()) {
        // The library's files that fit this field first (fillLibraryMenu).
        auto *library = iconButton(QStringLiteral("layers"), tr("Choose from the model library"), row);
        library->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(library);
        menu->setObjectName(QStringLiteral("library-%1").arg(f.key));
        library->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu, key = f.key] { fillLibraryMenu(menu, key); });
        h->addWidget(library);
    }
    auto *browse = iconButton(QStringLiteral("folder"), tr("Browse…"), row);
    h->addWidget(browse);
    const bool dir = f.type == OptionField::Type::Directory;
    connect(browse, &QToolButton::clicked, this, [this, edit, dir, filter = f.filter, label = f.label, key = f.key] {
        const QString start = edit->text().isEmpty() ? Settings::instance().value(QStringLiteral("servers/lastModelDir")).toString()
                                                     : QFileInfo(QDir::fromNativeSeparators(edit->text())).absolutePath();
        const QString path = dir ? QFileDialog::getExistingDirectory(this, label, start)
                                 : QFileDialog::getOpenFileName(this, label, start, filter + QStringLiteral(";;") + tr("All files (*)"));
        if (path.isEmpty())
            return;
        Settings::instance().setValue(QStringLiteral("servers/lastModelDir"), dir ? path : QFileInfo(path).absolutePath());
        chooseFile(key, path);
    });
    edit->dropped = [this, key = f.key](const QString &path) { chooseFile(key, path); };
    connect(edit, &QLineEdit::textChanged, this, &ServerProfileDialog::updatePreview);
    // A path typed or pasted in is used like a chosen one, once: not again each time the field loses
    // the keyboard, which would set the model's launch options over the ones chosen since.
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, key = f.key] {
        if (!edit->isModified())
            return;
        edit->setModified(false);
        if (!edit->text().trimmed().isEmpty())
            chooseFile(key, edit->text());
    });
    if (!f.help.isEmpty()) {
        edit->setToolTip(f.help);
        row->setToolTip(f.help);
    }
    m_fields.insert(f.key, edit);
    m_rows.insert(f.key, row);
    return row;
}

QWidget *ServerProfileDialog::optionField(const OptionField &f, QWidget *parent)
{
    const QVariant value = m_profile.option(f.key, kind());
    QWidget *w = nullptr;
    switch (f.type) {
    case OptionField::Type::Integer: {
        auto *s = new QSpinBox(parent);
        s->setRange(int(f.maximum > f.minimum ? f.minimum : 0), int(f.maximum > f.minimum ? f.maximum : 1000000));
        s->setValue(value.toInt());
        if (f.key == QLatin1String("gpu_layers"))
            s->setSpecialValueText(tr("Automatic"));
        connect(s, &QSpinBox::valueChanged, this, &ServerProfileDialog::updatePreview);
        w = s;
        break;
    }
    case OptionField::Type::Number: {
        auto *s = new QDoubleSpinBox(parent);
        s->setRange(f.maximum > f.minimum ? f.minimum : 0, f.maximum > f.minimum ? f.maximum : 1e6);
        s->setDecimals(1);
        s->setValue(value.toDouble());
        connect(s, &QDoubleSpinBox::valueChanged, this, &ServerProfileDialog::updatePreview);
        w = s;
        break;
    }
    case OptionField::Type::Boolean: {
        auto *c = new QCheckBox(f.label, parent);
        c->setChecked(value.toBool());
        connect(c, &QCheckBox::toggled, this, &ServerProfileDialog::updatePreview);
        w = c;
        break;
    }
    case OptionField::Type::Choice: {
        auto *c = new QComboBox(parent);
        c->addItems(f.choices);
        c->setCurrentText(value.toString());
        connect(c, &QComboBox::currentTextChanged, this, &ServerProfileDialog::updatePreview);
        w = c;
        break;
    }
    case OptionField::Type::ModelList:
        w = new ModelListEditor(value.toList(), f.filter + QStringLiteral(";;") + tr("All files (*)"), [this] { updatePreview(); }, parent,
                                m_studio->models());
        break;
    case OptionField::Type::Text:
    case OptionField::Type::File:
    case OptionField::Type::Directory: {
        auto *e = new QLineEdit(value.toString(), parent);
        connect(e, &QLineEdit::textChanged, this, &ServerProfileDialog::updatePreview);
        w = e;
        break;
    }
    }
    if (!f.help.isEmpty())
        w->setToolTip(f.help);
    m_fields.insert(f.key, w);
    return w;
}

const runtime::ModelType *ServerProfileDialog::currentType() const
{
    return m_type ? runtime::modelTypeById(m_type->currentData().toString()) : nullptr;
}

QString ServerProfileDialog::typeModelOption() const
{
    const runtime::ModelType *type = currentType();
    if (!type)
        return {};
    // A checkpoint of a type that also takes them stays a checkpoint (FLUX.2 [klein] as one file).
    for (const QString &key : {QStringLiteral("diffusion_model"), QStringLiteral("model")}) {
        if (const QString file = fieldText(key); !file.isEmpty()) {
            const QString option = type->modelOption(runtime::modelInfoOf(m_studio->models(), file));
            // A model the type has no slot for (a FLUX.1 diffusion model with SDXL chosen by hand)
            // stays in sight in its own field, with what is wrong with it, rather than hidden and
            // left out of the saved server.
            return fieldText(option).isEmpty() ? key : option;
        }
    }
    return type->modelOption();
}

QStringList ServerProfileDialog::shownFileOptions() const
{
    if (!m_type)
        return {};
    const runtime::ModelType *type = currentType();
    return type ? type->fileOptions(typeModelOption()) : runtime::sdFileOptions();
}

void ServerProfileDialog::selectType(const runtime::ModelType *type)
{
    if (!m_type)
        return;
    const QSignalBlocker block(m_type);
    m_type->setCurrentIndex(std::max(0, m_type->findData(type ? type->id : QString())));
    showType();
}

void ServerProfileDialog::showType()
{
    if (!m_type)
        return;
    const runtime::ModelType *type = currentType();
    const QStringList shown = shownFileOptions();
    m_shownFiles = shown;
    const QString modelOption = typeModelOption();
    for (const QString &key : runtime::sdFileOptions()) {
        QWidget *row = m_rows.value(key);
        if (!row)
            continue;
        QFormLayout *form = isAddon(key) ? m_addonForm : m_fileForm;
        const bool on = shown.contains(key);
        form->setRowVisible(row, on);
        if (!on)
            form->setRowVisible(m_status.value(key), false);
        // Labelled with what the type expects ("Text encoder (Qwen3 4B)", "VAE (optional)").
        const QString label = runtime::fileFieldLabel(key, type);
        if (auto *l = qobject_cast<QLabel *>(form->labelForField(row)))
            l->setText(label);
        m_fields.value(key)->setAccessibleName(label);
        const runtime::PresetSlot *slot = type ? type->slot(key) : nullptr;
        QString placeholder = tr("none");
        if (key == modelOption || (slot && slot->required))
            placeholder = tr("required");
        else if (key == QLatin1String("vae") && type && !slot)
            placeholder = tr("the checkpoint's own");
        qobject_cast<QLineEdit *>(m_fields.value(key))->setPlaceholderText(placeholder);
    }
    m_addonsLabel->setVisible(std::any_of(shown.cbegin(), shown.cend(), [](const QString &key) { return isAddon(key); }));
    alignForms();
}

void ServerProfileDialog::typeChosen()
{
    const runtime::ModelType *type = currentType();
    // The model file moves to the new type's slot (unless something was typed there before), and a
    // model of this type gets the components it needs where their fields are empty or do not fit.
    QString from;
    for (const QString &key : {QStringLiteral("diffusion_model"), QStringLiteral("model")}) {
        if (!fieldText(key).isEmpty()) {
            from = key;
            break;
        }
    }
    if (type && !from.isEmpty()) {
        const QString path = cleanPath(fieldText(from));
        const std::optional<runtime::ModelInfo> info = runtime::modelInfoOf(m_studio->models(), path);
        const QString to = type->modelOption(info);
        // Only into a slot of its kind: a FLUX diffusion model does not become an SDXL checkpoint (it
        // stays in its field, hidden, until a type that has one is chosen again).
        const bool fits = !info || info->kind == runtime::ModelKind::Unknown || runtime::mainModelOption(*info) == to;
        if (to != from && fits && fieldText(to).isEmpty()) {
            setField(to, fieldText(from));
            setField(from, QString());
        }
        const runtime::ModelPreset *preset = info ? runtime::presetFor(*info, path) : nullptr;
        if (preset && type->contains(*preset) && cleanPath(fieldText(to)) == path)
            applyModel(to, path, false);
    }
    showType();
    updatePreview();
    fitToContent();
}

QString ServerProfileDialog::fieldText(const QString &key) const
{
    auto *e = qobject_cast<QLineEdit *>(m_fields.value(key));
    return e ? e->text().trimmed() : QString();
}

void ServerProfileDialog::setField(const QString &key, const QVariant &value)
{
    QWidget *w = m_fields.value(key);
    if (auto *e = qobject_cast<QLineEdit *>(w))
        e->setText(value.toString());
    else if (auto *c = qobject_cast<QCheckBox *>(w))
        c->setChecked(value.toBool());
    else if (auto *box = qobject_cast<QComboBox *>(w))
        box->setCurrentIndex(std::max(0, box->findText(value.toString())));
    else if (auto *spin = qobject_cast<QSpinBox *>(w))
        spin->setValue(value.toInt());
    else if (auto *dspin = qobject_cast<QDoubleSpinBox *>(w))
        dspin->setValue(value.toDouble());
}

QString ServerProfileDialog::modelPath() const
{
    if (const QString option = typeModelOption(); !option.isEmpty())
        return cleanPath(fieldText(option));
    for (const QString &key : {QStringLiteral("diffusion_model"), QStringLiteral("model")}) {
        if (const QString file = fieldText(key); !file.isEmpty())
            return cleanPath(file);
    }
    return {};
}

void ServerProfileDialog::chooseFile(const QString &key, const QString &path)
{
    const QString native = QDir::toNativeSeparators(cleanPath(path));
    if (kind() == QLatin1String("sd.cpp") && isModelSlot(key)) {
        // A model file Lumen does not know as an image model stays where it was chosen.
        if (!applyModel(key, path))
            setField(key, native);
    } else {
        // Any other file is kept as chosen (a text encoder chosen by hand stays).
        setField(key, native);
        // A vision model's projector usually sits next to it.
        if (kind() == QLatin1String("llama.cpp") && key == QLatin1String("model")) {
            if (auto *mmproj = qobject_cast<QLineEdit *>(m_fields.value(QStringLiteral("mmproj"))); mmproj && mmproj->text().isEmpty())
                mmproj->setText(QDir::toNativeSeparators(runtime::findProjector(path)));
        }
    }
    updatePreview();
}

bool ServerProfileDialog::applyModel(const QString &key, const QString &path, bool launchOptions)
{
    if (kind() != QLatin1String("sd.cpp") || !isModelSlot(key))
        return false;
    const QString clean = cleanPath(path);
    const int tier = m_preset->currentData().toInt();
    const QVariantMap options = runtime::modelServerOptions(m_studio->models(), clean, tier < 0 ? std::nullopt : std::optional(runtime::VramTier(tier)));
    if (options.isEmpty())
        return false;
    const std::optional<runtime::ModelInfo> info = runtime::modelInfoOf(m_studio->models(), clean);
    const runtime::ModelPreset *preset = info ? runtime::presetFor(*info, clean) : nullptr;
    // The type from its header. A model Lumen has no preset for keeps the type chosen by hand,
    // unless that type has no place for it (a checkpoint for a FLUX.2 type): then every field.
    const runtime::ModelType *type = preset ? runtime::modelTypeOf(*preset) : currentType();
    if (!preset && type && type->modelOption(info) != runtime::mainModelOption(*info))
        type = nullptr;
    // The model in its slot (the other slot cleared), then the type's fields.
    for (const QString &slot : {QStringLiteral("model"), QStringLiteral("diffusion_model")})
        setField(slot, options.value(slot));
    selectType(type);
    // A component replaces an empty field or a file that does not fit; a file chosen by hand that
    // fits stays. The preset's launch options are set.
    const QStringList problemOptions = [&] {
        QStringList out;
        for (const runtime::ProfileProblem &p : runtime::checkSdProfile(m_studio->models(), profile()))
            out << p.option;
        return out;
    }();
    const QStringList files = runtime::sdFileOptions();
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (isModelSlot(it.key()))
            continue;
        if (!files.contains(it.key())) {
            if (launchOptions)
                setField(it.key(), it.value());
        } else if (fieldText(it.key()).isEmpty() || problemOptions.contains(it.key())) {
            setField(it.key(), it.value());
        }
    }
    updatePreview();
    return true;
}

void ServerProfileDialog::fillLibraryMenu(QMenu *menu, const QString &key)
{
    const QString k = kind();
    std::function<bool(const runtime::ModelEntry &)> fits;
    if (k == QLatin1String("sd.cpp")) {
        const runtime::ModelType *type = currentType();
        const QString model = modelPath();
        const std::optional<runtime::ModelInfo> info = model.isEmpty() ? std::nullopt : runtime::modelInfoOf(m_studio->models(), model);
        const runtime::ModelPreset *preset = info ? runtime::presetFor(*info, model) : nullptr;
        if (preset && type && !type->contains(*preset))
            preset = nullptr; // a model of another type: what the chosen type needs
        if (isModelSlot(key)) {
            // The models of the type.
            if (type) {
                fits = [type](const runtime::ModelEntry &e) {
                    const runtime::ModelPreset *p = runtime::presetFor(e.info, e.path);
                    return p && type->contains(*p);
                };
            }
        } else if (const runtime::PresetSlot *slot = preset ? preset->slot(key) : type ? type->slot(key) : nullptr) {
            // A component: the slot's header family and variant.
            fits = [s = *slot](const runtime::ModelEntry &e) {
                return e.info.kind == s.kind && e.info.family == s.family && (s.variant.isEmpty() || e.info.variant == s.variant);
            };
        } else if (key == QLatin1String("vae") && type && !type->optionalVae.isEmpty()) {
            fits = [family = type->optionalVae](const runtime::ModelEntry &e) { return e.info.family == family; };
        } else if ((key == QLatin1String("control_net") || key == QLatin1String("ip_adapter")) && (preset || type)) {
            const bool cn = key == QLatin1String("control_net");
            const QStringList families = preset ? (cn ? preset->controlNetFamilies : preset->ipAdapterFamilies)
                                                : (cn ? type->controlNetFamilies : type->ipAdapterFamilies);
            fits = [families](const runtime::ModelEntry &e) { return families.contains(e.info.family); };
        }
    }
    fillModelMenu(menu, m_studio->models(), modelKindsForOption(k, key), [this, key](const QString &path) { chooseFile(key, path); }, fits);
}

void ServerProfileDialog::downloadMissing()
{
    const QString path = modelPath();
    const std::optional<runtime::ModelEntry> e = m_studio->models()->entry(path);
    if (!e)
        return;
    const QStringList shown = shownFileOptions();
    QStringList names;
    for (const runtime::ModelCompanion &c : runtime::companionsOf(m_studio->models(), path)) {
        // Only for the type's empty fields (a file chosen by hand stays), once.
        if (!c.missing() || (!c.option.isEmpty() && (!shown.contains(c.option) || !fieldText(c.option).isEmpty()))
            || (m_downloads.value(c.option) && !m_downloads.value(c.option)->isFinished()))
            continue;
        Job *job = m_studio->hub()->download(runtime::companionDownload(*e, c.file));
        if (!c.option.isEmpty() && job) {
            m_downloads.insert(c.option, job);
            // A failed or cancelled download is missing again.
            connect(job, &Job::finished, this, &ServerProfileDialog::updatePreview);
        }
        names << c.file.fileName();
    }
    if (names.isEmpty())
        return;
    updatePreview();
    m_studio->notify(tr("Downloading %1 — progress is shown in the Queue. The server form fills in when it is done.").arg(names.join(QStringLiteral(", "))));
}

runtime::ServerProfile ServerProfileDialog::profile() const
{
    runtime::ServerProfile p = m_profile;
    p.name = m_name->text().trimmed().isEmpty() ? runtimeName(kind()) : m_name->text().trimmed();
    p.runtimeId = m_runtime->currentData().toString();
    p.port = m_port->value();
    p.startPolicy = runtime::startPolicyFromKey(m_start->currentData().toString());
    p.idleStopMinutes = m_idle->value();
    if (m_builtFor != kind())
        return p;
    // Files of other model types are not saved: no T5 encoder left on a FLUX.2 server.
    const QStringList shown = shownFileOptions();
    QVariantMap options;
    for (const OptionField &f : runtime::serverOptions(m_builtFor)) {
        if (m_type && isFileField(f) && !shown.contains(f.key))
            continue;
        QWidget *w = m_fields.value(f.key);
        QVariant v;
        if (auto *e = qobject_cast<QLineEdit *>(w))
            v = e->text().trimmed();
        else if (auto *s = qobject_cast<QSpinBox *>(w))
            v = s->value();
        else if (auto *d = qobject_cast<QDoubleSpinBox *>(w))
            v = d->value();
        else if (auto *c = qobject_cast<QCheckBox *>(w))
            v = c->isChecked();
        else if (auto *cb = qobject_cast<QComboBox *>(w))
            v = cb->currentText();
        else if (auto *list = dynamic_cast<ModelListEditor *>(w))
            v = list->value().isEmpty() ? QVariant() : QVariant(list->value());
        // Only what differs from the defaults is stored, so better defaults reach existing servers.
        const bool isDefault = v == f.defaultValue || (v.toString().isEmpty() && !f.defaultValue.isValid());
        if (!isDefault)
            options.insert(f.key, v);
    }
    p.options = options;
    return p;
}

bool ServerProfileDialog::createProvider() const
{
    return m_createProvider && m_createProvider->isEnabled() && m_createProvider->isChecked();
}

void ServerProfileDialog::updateFileStatus(const runtime::ServerProfile &p, QStringList *problems, bool *blocking, qint64 *missingBytes,
                                           int *missingFiles)
{
    const Palette &pal = Theme::palette();
    const QString ok = QStringLiteral("✓");
    const QString bad = QStringLiteral("✗");
    runtime::ModelLibrary *library = m_studio->models();
    const runtime::ModelType *type = currentType();
    const QStringList shown = shownFileOptions();
    const QString model = modelPath();
    const std::optional<runtime::ModelInfo> info = model.isEmpty() ? std::nullopt : runtime::modelInfoOf(library, model);
    const runtime::ModelPreset *preset = info ? runtime::presetFor(*info, model) : nullptr;
    const bool otherType = preset && type && !type->contains(*preset);
    // A model file of a kind the type does not take at all (a diffusion model with SDXL chosen), as
    // the type was changed by hand.
    const bool wrongKind = type && info && (info->kind == runtime::ModelKind::Checkpoint || info->kind == runtime::ModelKind::DiffusionModel)
                        && !type->kinds.contains(info->kind);
    // What can be downloaded for the model (a file in the library).
    QHash<QString, runtime::ModelCompanion> downloadable;
    if (preset && !otherType && library->entry(model)) {
        for (const runtime::ModelCompanion &c : runtime::companionsOf(library, model)) {
            if (c.missing() && !c.option.isEmpty())
                downloadable.insert(c.option, c);
        }
    }
    // What is wrong stands next to its field; what has no field shown goes below the form.
    QHash<QString, runtime::ProfileProblem> byOption;
    QList<runtime::ProfileProblem> found;
    if (type && (!preset || otherType)) {
        // No model of the chosen type decides (none chosen yet, or one of another type): the fields
        // are the type's, so their files are checked against it. Of the model, only what is wrong
        // with it in its field (not the components a model of another type would need).
        runtime::ServerProfile models = p;
        for (const QString &key : runtime::sdFileOptions()) {
            if (!isModelSlot(key))
                models.options.remove(key);
        }
        for (const runtime::ProfileProblem &pp : runtime::checkSdProfile(library, models)) {
            if (isModelSlot(pp.option))
                found << pp;
        }
        found << runtime::checkTypeFiles(library, p, *type);
    } else {
        found = runtime::checkSdProfile(library, p);
    }
    for (const runtime::ProfileProblem &pp : std::as_const(found)) {
        *blocking = *blocking || !pp.missing;
        if (!shown.contains(pp.option))
            *problems << pp.message.toHtmlEscaped();
        else if (!byOption.contains(pp.option))
            byOption.insert(pp.option, pp);
    }
    for (const QString &key : runtime::sdFileOptions()) {
        QLabel *status = m_status.value(key);
        if (!status)
            continue;
        const QString file = p.options.value(key).toString().trimmed();
        const runtime::ProfileProblem problem = byOption.value(key);
        QString text;
        if (!shown.contains(key)) {
            // hidden with its field
        } else if (isModelSlot(key) && !file.isEmpty() && otherType) {
            // A model of another type than the one chosen: its components have no fields here.
            text = statusLine(bad, pal.danger, tr("This is a %1 model: choose its type, %2.").arg(preset->name, runtime::modelTypeOf(*preset)->name));
            *blocking = true;
        } else if (isModelSlot(key) && !file.isEmpty() && wrongKind) {
            text = statusLine(bad, pal.danger,
                              info->kind == runtime::ModelKind::Checkpoint
                                  ? tr("This is a complete checkpoint, and this model type takes a diffusion model: choose another type, or Other.")
                                  : tr("This is a diffusion model, and this model type takes a complete checkpoint: choose another type, or Other."));
            *blocking = true;
        } else if (!problem.option.isEmpty() && !problem.missing) {
            text = statusLine(bad, pal.danger, problem.brief.isEmpty() ? problem.message : problem.brief);
        } else if (isModelSlot(key)) {
            if (file.isEmpty()) {
                // What the server needs first, on the type's own field (not a line below the form
                // that names fields the type does not show).
                if (type)
                    text = statusLine(QStringLiteral("•"), pal.textMuted,
                                      key == QLatin1String("model") ? tr("Choose the checkpoint file: type its path, Browse…, or pick it from the model library.")
                                                                    : tr("Choose the diffusion model file: type its path, Browse…, or pick it from the model library."));
            } else if (preset) {
                text = statusLine(ok, pal.success, preset->note.isEmpty() ? preset->name : tr("%1: %2").arg(preset->name, preset->note));
            } else if (!info || info->kind == runtime::ModelKind::Unknown) {
                text = statusLine(QStringLiteral("?"), pal.textMuted, tr("Lumen cannot tell what this file is; it is used as chosen."));
            } else {
                text = statusLine(QStringLiteral("?"), pal.textMuted, tr("Lumen does not know this model: choose the files it needs by hand."));
            }
        } else if (!file.isEmpty()) {
            const bool folder = key == QLatin1String("lora_dir") || key == QLatin1String("upscalers_dir");
            if (folder && !QFileInfo(QDir::fromNativeSeparators(file)).isDir())
                text = statusLine(bad, pal.warning, tr("This folder does not exist."));
            else
                text = statusLine(ok, pal.success, tr("Found"));
        } else if (const runtime::PresetSlot *slot = model.isEmpty() ? nullptr : preset && !otherType ? preset->slot(key) : type ? type->slot(key) : nullptr;
                   slot && slot->required) {
            // A component the model needs: being downloaded, or missing (with the download below).
            const runtime::ModelCompanion c = downloadable.value(key);
            const QString name = c.file.path.isEmpty() ? slot->file.fileName() : c.file.fileName();
            const qint64 size = c.file.size > 0 ? c.file.size : slot->file.size;
            const QString what = size > 0 ? QStringLiteral("%1, %2").arg(name, runtime::formatBytes(size)) : name;
            const QPointer<Job> job = m_downloads.value(key);
            if (job && !job->isFinished()) {
                text = statusLine(QStringLiteral("↓"), pal.accent, tr("Will be downloaded (%1)").arg(what));
            } else {
                text = statusLine(bad, pal.warning, tr("Missing (%1)").arg(what));
                if (downloadable.contains(key)) {
                    *missingBytes += std::max<qint64>(0, c.file.size);
                    ++*missingFiles;
                }
            }
        }
        status->setText(text);
        (isAddon(key) ? m_addonForm : m_fileForm)->setRowVisible(status, !text.isEmpty());
    }
}

void ServerProfileDialog::updatePreview()
{
    // A model file cleared or changed by hand can change which fields its type shows (a model left
    // in a slot the type does not use makes way for the type's own).
    if (m_type && m_builtFor == kind() && shownFileOptions() != m_shownFiles) {
        showType();
        fitToContent();
    }
    const runtime::ServerProfile p = profile();
    const auto rt = m_studio->runtimes()->runtime(p.runtimeId);
    if (!rt) {
        m_preview->setPlainText(QString());
        m_problem->setText(tr("Choose a program."));
        m_problem->setVisible(true);
        return;
    }
    const runtime::LaunchCommand cmd = runtime::buildLaunch(p, *rt);
    m_preview->setPlainText(cmd.commandLine());
    // Without a model, a known type asks for it on its model field (updateFileStatus).
    const bool noModel = p.options.value(QStringLiteral("model")).toString().isEmpty()
                      && p.options.value(QStringLiteral("diffusion_model")).toString().isEmpty();
    QString problem = m_type && m_builtFor == rt->kind && currentType() && noModel ? QString() : cmd.error;
    for (const runtime::ServerProfile &other : m_studio->servers()->profiles()) {
        if (other.id != p.id && other.port == p.port && problem.isEmpty())
            problem = tr("Port %1 is already used by the server '%2'.").arg(p.port).arg(other.name);
    }
    QStringList problems;
    if (!problem.isEmpty())
        problems << problem.toHtmlEscaped();
    // An sd.cpp model: what each file field holds, and what keeps the server from working. Files in
    // the wrong slot or of the wrong family cannot be saved; missing components can (the start says
    // what to download).
    bool blocking = false;
    qint64 missingBytes = 0;
    int missingFiles = 0;
    if (m_type && m_builtFor == rt->kind)
        updateFileStatus(p, &problems, &blocking, &missingBytes, &missingFiles);
    if (m_downloadMissing) {
        m_downloadMissing->setText(tr("Download missing files (%1)").arg(runtime::formatBytes(missingBytes)));
        m_fileForm->setRowVisible(m_downloadRow, missingFiles > 0);
    }
    m_problem->setText(problems.join(QStringLiteral("<br>")));
    m_problem->setVisible(!problems.isEmpty());
    if (m_ok)
        m_ok->setEnabled(!blocking);
    selectTier();
}

// ---- ServerLogDialog ------------------------------------------------------------------------------

ServerLogDialog::ServerLogDialog(Studio *studio, const QString &profileId, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_id(profileId)
{
    const auto p = studio->servers()->profile(profileId);
    setWindowTitle(tr("Server log — %1").arg(p ? p->name : profileId));
    resize(900, 560);
    auto *v = new QVBoxLayout(this);
    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_state);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMaximumBlockCount(5000);
    m_log->setPlainText(studio->servers()->log(profileId).join(QLatin1Char('\n')));
    m_log->moveCursor(QTextCursor::End);
    v->addWidget(m_log, 1);
    auto *bar = new QHBoxLayout;
    m_start = new QPushButton(Icons::get(QStringLiteral("play")), tr("Start"), this);
    m_stop = new QPushButton(Icons::get(QStringLiteral("stop")), tr("Stop"), this);
    auto *restart = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Restart"), this);
    auto *copy = new QPushButton(Icons::get(QStringLiteral("copy")), tr("Copy command line"), this);
    auto *folder = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Open log folder"), this);
    auto *close = new QPushButton(tr("Close"), this);
    for (QPushButton *b : {m_start, m_stop, restart, copy, folder})
        bar->addWidget(b);
    bar->addStretch();
    bar->addWidget(close);
    v->addLayout(bar);
    connect(m_start, &QPushButton::clicked, this, [this] { m_studio->servers()->start(m_id); });
    connect(m_stop, &QPushButton::clicked, this, [this] { m_studio->servers()->stop(m_id); });
    connect(restart, &QPushButton::clicked, this, [this] { m_studio->servers()->restart(m_id); });
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_studio->servers()->launchFor(m_id).commandLine()); });
    connect(folder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_studio->servers()->logPath(m_id)).absolutePath()));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(studio->servers(), &runtime::ServerSupervisor::logAppended, this, [this](const QString &id, const QString &line) {
        if (id == m_id)
            m_log->appendPlainText(line);
    });
    connect(studio->servers(), &runtime::ServerSupervisor::stateChanged, this, [this](const QString &id) {
        if (id == m_id)
            updateState();
    });
    updateState();
}

void ServerLogDialog::updateState()
{
    runtime::ServerSupervisor *s = m_studio->servers();
    const ServerState state = s->state(m_id);
    const QString error = s->lastError(m_id);
    m_state->setText(state == ServerState::Failed && !error.isEmpty() ? error : s->statusText(m_id));
    QPalette pal = m_state->palette();
    pal.setColor(QPalette::WindowText, stateColor(state));
    m_state->setPalette(pal);
    const bool running = state == ServerState::Ready || state == ServerState::Starting;
    m_start->setEnabled(!running && state != ServerState::Stopping);
    m_stop->setEnabled(running);
}

} // namespace lumen::ui
