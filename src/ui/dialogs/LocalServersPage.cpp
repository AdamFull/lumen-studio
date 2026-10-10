#include "ui/dialogs/LocalServersPage.h"

#include "ai/ProviderRegistry.h"
#include "core/Error.h"
#include "core/Job.h"
#include "core/Settings.h"
#include "runtime/Hardware.h"
#include "runtime/RuntimeStore.h"
#include "runtime/Supervisor.h"
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
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

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
    v->addWidget(m_hardwareLabel);

    v->addWidget(sectionLabel(tr("Programs"), this));
    m_runtimes = makeTree({tr("Program"), tr("Version"), tr("Build"), tr("Folder")}, this);
    v->addWidget(m_runtimes, 1);
    auto *rbar = new QHBoxLayout;
    auto *installBtn = new QPushButton(Icons::get(QStringLiteral("download")), tr("Download…"), this);
    Theme::setRole(installBtn, QStringLiteral("primary"));
    installBtn->setToolTip(tr("Download llama.cpp or stable-diffusion.cpp from GitHub"));
    auto *existing = new QPushButton(Icons::get(QStringLiteral("folder")), tr("Add existing…"), this);
    existing->setToolTip(tr("Use llama-server or sd-server you installed yourself"));
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
                                            tr("Servers (llama-server.exe sd-server.exe llama-server sd-server)"));
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
    const QList<runtime::InstalledRuntime> all = m_studio->runtimes()->runtimes();
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
    const runtime::ServerProfile p = dlg.profile();
    m_studio->servers()->addProfile(p);
    if (dlg.createProvider()) {
        const QString kind = m_studio->servers()->kindOf(p.id);
        QVariantMap settings{{QStringLiteral("url"), runtime::providerUrl(kind, p.port)}, {QStringLiteral("managed_server"), p.id}};
        if (ai::Provider *provider = m_studio->providers()->addProvider(runtime::providerTypeFor(kind), p.name, settings)) {
            // A fresh setup has no roles yet: let the first local language model handle them.
            if (provider->kind() == ai::ProviderKind::Llm && m_studio->providers()->role(ai::LlmRole::Assistant).providerId.isEmpty())
                m_studio->providers()->setRole(ai::LlmRole::Assistant, {provider->instanceId(), QString()});
            m_studio->notify(tr("%1 added as a provider. It starts when Lumen first uses it.").arg(p.name), QStringLiteral("success"));
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
        m_runtime->addItem(Icons::get(QStringLiteral("server")), r.name, r.id);
        m_runtime->setItemData(m_runtime->count() - 1, r.description, Qt::ToolTipRole);
    }
    if (!runtimeId.isEmpty())
        m_runtime->setCurrentIndex(std::max(0, m_runtime->findData(runtimeId)));
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

void InstallRuntimeDialog::fetch()
{
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

ServerProfileDialog::ServerProfileDialog(Studio *studio, const runtime::ServerProfile &profile, bool isNew, QWidget *parent)
    : QDialog(parent)
    , m_studio(studio)
    , m_profile(profile)
    , m_isNew(isNew)
{
    setWindowTitle(isNew ? tr("New server") : tr("Server settings"));
    resize(640, 720);
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(4, 4, 4, 4);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_name = new QLineEdit(profile.name, content);
    form->addRow(tr("Name"), m_name);
    m_runtime = new QComboBox(content);
    for (const runtime::InstalledRuntime &rt : m_studio->runtimes()->runtimes())
        m_runtime->addItem(Icons::get(QStringLiteral("server")), rt.displayName(), rt.id);
    m_runtime->setCurrentIndex(std::max(0, m_runtime->findData(profile.runtimeId)));
    form->addRow(tr("Program"), m_runtime);
    m_port = new QSpinBox(content);
    m_port->setRange(1024, 65535);
    m_port->setValue(profile.port);
    form->addRow(tr("Port"), m_port);
    m_start = new QComboBox(content);
    for (runtime::StartPolicy p : {runtime::StartPolicy::OnDemand, runtime::StartPolicy::AtStartup, runtime::StartPolicy::Manual})
        m_start->addItem(startLabel(p), runtime::startPolicyKey(p));
    m_start->setCurrentIndex(std::max(0, m_start->findData(runtime::startPolicyKey(profile.startPolicy))));
    m_start->setToolTip(tr("“When needed” starts the server on the first request that uses it."));
    form->addRow(tr("Starts"), m_start);
    m_idle = new QSpinBox(content);
    m_idle->setRange(0, 24 * 60);
    m_idle->setSuffix(tr(" min"));
    m_idle->setSpecialValueText(tr("Never"));
    m_idle->setValue(profile.idleStopMinutes);
    m_idle->setToolTip(tr("Stops the server after it was unused for this long, freeing memory."));
    form->addRow(tr("Stop when idle"), m_idle);
    v->addLayout(form);

    m_optionsHost = new QWidget(content);
    auto *hostLayout = new QVBoxLayout(m_optionsHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_optionsHost);

    v->addWidget(sectionLabel(tr("Command line"), content));
    m_preview = new QPlainTextEdit(content);
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setFixedHeight(84);
    v->addWidget(m_preview);
    m_problem = new QLabel(content);
    m_problem->setWordWrap(true);
    Theme::setRole(m_problem, QStringLiteral("error"));
    v->addWidget(m_problem);
    if (isNew) {
        m_createProvider = new QCheckBox(tr("Add it as a provider"), content);
        m_createProvider->setChecked(true);
        v->addWidget(m_createProvider);
    }
    v->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(m_runtime->count() > 0);

    connect(m_runtime, &QComboBox::currentIndexChanged, this, [this] {
        m_profile = this->profile(); // keep typed values
        buildOptions();
    });
    connect(m_name, &QLineEdit::textChanged, this, &ServerProfileDialog::updatePreview);
    connect(m_port, &QSpinBox::valueChanged, this, &ServerProfileDialog::updatePreview);
    buildOptions();
}

QString ServerProfileDialog::kind() const
{
    const auto rt = m_studio->runtimes()->runtime(m_runtime->currentData().toString());
    return rt ? rt->kind : QString();
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
    auto *host = m_optionsHost->layout();
    while (QLayoutItem *item = host->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto *box = new QWidget(m_optionsHost);
    auto *bv = new QVBoxLayout(box);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->addWidget(sectionLabel(tr("%1 options").arg(runtimeName(k)), box));
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bv->addLayout(form);
    auto *advancedBox = new QGroupBox(tr("Advanced"), box);
    advancedBox->setCheckable(true);
    advancedBox->setChecked(false);
    auto *advancedInner = new QWidget(advancedBox);
    auto *advancedForm = new QFormLayout(advancedInner);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *abl = new QVBoxLayout(advancedBox);
    abl->addWidget(advancedInner);
    advancedInner->setVisible(false);
    connect(advancedBox, &QGroupBox::toggled, advancedInner, &QWidget::setVisible);
    bool anyAdvanced = false;

    for (const OptionField &f : runtime::serverOptions(k)) {
        const QVariant value = m_profile.option(f.key, k);
        QWidget *w = nullptr;
        QWidget *row = nullptr;
        switch (f.type) {
        case OptionField::Type::File:
        case OptionField::Type::Directory: {
            row = new QWidget(box);
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto *edit = new QLineEdit(value.toString(), row);
            edit->setPlaceholderText(f.required ? tr("required") : tr("none"));
            edit->setMinimumWidth(320);
            auto *browse = iconButton(QStringLiteral("folder"), tr("Browse…"), row);
            h->addWidget(edit, 1);
            h->addWidget(browse);
            const bool dir = f.type == OptionField::Type::Directory;
            const QString filter = f.filter;
            connect(browse, &QToolButton::clicked, this, [this, edit, dir, filter, label = f.label] {
                const QString start = edit->text().isEmpty()
                                          ? Settings::instance().value(QStringLiteral("servers/lastModelDir")).toString()
                                          : QFileInfo(edit->text()).absolutePath();
                const QString path = dir ? QFileDialog::getExistingDirectory(this, label, start)
                                         : QFileDialog::getOpenFileName(this, label, start, filter + QStringLiteral(";;") + tr("All files (*)"));
                if (path.isEmpty())
                    return;
                edit->setText(QDir::toNativeSeparators(path));
                Settings::instance().setValue(QStringLiteral("servers/lastModelDir"), dir ? path : QFileInfo(path).absolutePath());
            });
            connect(edit, &QLineEdit::textChanged, this, &ServerProfileDialog::updatePreview);
            w = edit;
            break;
        }
        case OptionField::Type::Integer: {
            auto *s = new QSpinBox(box);
            s->setRange(int(f.maximum > f.minimum ? f.minimum : 0), int(f.maximum > f.minimum ? f.maximum : 1000000));
            s->setValue(value.toInt());
            connect(s, &QSpinBox::valueChanged, this, &ServerProfileDialog::updatePreview);
            w = s;
            break;
        }
        case OptionField::Type::Number: {
            auto *s = new QDoubleSpinBox(box);
            s->setRange(f.maximum > f.minimum ? f.minimum : 0, f.maximum > f.minimum ? f.maximum : 1e6);
            s->setDecimals(1);
            s->setValue(value.toDouble());
            connect(s, &QDoubleSpinBox::valueChanged, this, &ServerProfileDialog::updatePreview);
            w = s;
            break;
        }
        case OptionField::Type::Boolean: {
            auto *c = new QCheckBox(box);
            c->setChecked(value.toBool());
            connect(c, &QCheckBox::toggled, this, &ServerProfileDialog::updatePreview);
            w = c;
            break;
        }
        case OptionField::Type::Choice: {
            auto *c = new QComboBox(box);
            c->addItems(f.choices);
            c->setCurrentText(value.toString());
            connect(c, &QComboBox::currentTextChanged, this, &ServerProfileDialog::updatePreview);
            w = c;
            break;
        }
        case OptionField::Type::Text: {
            auto *e = new QLineEdit(value.toString(), box);
            connect(e, &QLineEdit::textChanged, this, &ServerProfileDialog::updatePreview);
            w = e;
            break;
        }
        }
        if (!f.help.isEmpty()) {
            w->setToolTip(f.help);
            if (row)
                row->setToolTip(f.help);
        }
        m_fields.insert(f.key, w);
        const QString label = f.label + (f.required ? QStringLiteral(" *") : QString());
        if (f.advanced) {
            anyAdvanced = true;
            advancedForm->addRow(label, row ? row : w);
        } else {
            form->addRow(label, row ? row : w);
        }
    }
    if (anyAdvanced)
        bv->addWidget(advancedBox);
    else
        delete advancedBox;
    host->addWidget(box);

    // Providers exist for llama.cpp; the sd.cpp provider comes with the next step of M12.
    if (m_createProvider) {
        const bool available = m_studio->providers()->type(runtime::providerTypeFor(k)).has_value();
        m_createProvider->setEnabled(available);
        m_createProvider->setChecked(available);
        m_createProvider->setToolTip(available ? QString() : tr("Lumen has no provider for %1 yet.").arg(runtimeName(k)));
    }
    updatePreview();
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
    QVariantMap options;
    for (const OptionField &f : runtime::serverOptions(m_builtFor)) {
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

void ServerProfileDialog::updatePreview()
{
    const runtime::ServerProfile p = profile();
    const auto rt = m_studio->runtimes()->runtime(p.runtimeId);
    if (!rt) {
        m_preview->setPlainText(QString());
        m_problem->setText(tr("Choose a program."));
        return;
    }
    const runtime::LaunchCommand cmd = runtime::buildLaunch(p, *rt);
    m_preview->setPlainText(cmd.commandLine());
    QString problem = cmd.error;
    for (const runtime::ServerProfile &other : m_studio->servers()->profiles()) {
        if (other.id != p.id && other.port == p.port && problem.isEmpty())
            problem = tr("Port %1 is already used by the server '%2'.").arg(p.port).arg(other.name);
    }
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
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
