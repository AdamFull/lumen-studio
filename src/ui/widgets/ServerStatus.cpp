#include "ui/widgets/ServerStatus.h"

#include "runtime/Supervisor.h"
#include "ui/Icons.h"
#include "ui/Studio.h"
#include "ui/Theme.h"
#include "ui/dialogs/LocalServersPage.h"

#include <QMenu>

namespace lumen::ui {

using runtime::ServerState;

ServerStatusButton::ServerStatusButton(Studio *studio, std::function<void()> openSettings, QWidget *parent)
    : QToolButton(parent)
    , m_studio(studio)
    , m_openSettings(std::move(openSettings))
{
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setPopupMode(QToolButton::InstantPopup);
    setAccessibleName(tr("Local servers"));
    auto *menu = new QMenu(this);
    setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, &ServerStatusButton::buildMenu);
    connect(studio->servers(), &runtime::ServerSupervisor::stateChanged, this, &ServerStatusButton::refresh);
    connect(studio->servers(), &runtime::ServerSupervisor::profilesChanged, this, &ServerStatusButton::refresh);
    refresh();
}

void ServerStatusButton::refresh()
{
    runtime::ServerSupervisor *servers = m_studio->servers();
    const QList<runtime::ServerProfile> profiles = servers->profiles();
    setVisible(!profiles.isEmpty());
    int running = 0;
    QString starting;
    QString failed;
    QStringList tips;
    for (const runtime::ServerProfile &p : profiles) {
        const ServerState s = servers->state(p.id);
        running += s == ServerState::Ready;
        if (s == ServerState::Starting && starting.isEmpty())
            starting = p.name;
        if (s == ServerState::Failed && failed.isEmpty())
            failed = p.name;
        tips << QStringLiteral("%1: %2").arg(p.name, servers->statusText(p.id));
    }
    QColor color = Theme::palette().textMuted;
    QString text;
    if (!failed.isEmpty()) {
        text = tr("%1 failed").arg(failed);
        color = Theme::palette().danger;
    } else if (!starting.isEmpty()) {
        text = tr("Starting %1…").arg(starting);
        color = Theme::palette().warning;
    } else if (running > 0) {
        text = tr("%n server(s) running", nullptr, running);
        color = Theme::palette().success;
    } else {
        text = tr("Servers stopped");
    }
    setText(text);
    setIcon(QIcon(Icons::pixmap(QStringLiteral("server"), 14, color)));
    setToolTip(tips.join(QLatin1Char('\n')));
}

void ServerStatusButton::buildMenu()
{
    QMenu *m = menu();
    m->clear();
    runtime::ServerSupervisor *servers = m_studio->servers();
    for (const runtime::ServerProfile &p : servers->profiles()) {
        const ServerState s = servers->state(p.id);
        QMenu *sub = m->addMenu(Icons::get(QStringLiteral("server")), QStringLiteral("%1 — %2").arg(p.name, runtime::serverStateLabel(s)));
        const bool running = s == ServerState::Ready || s == ServerState::Starting;
        const QString id = p.id;
        sub->addAction(Icons::get(QStringLiteral("play")), tr("Start"), this, [servers, id] { servers->start(id); })->setEnabled(!running);
        sub->addAction(Icons::get(QStringLiteral("stop")), tr("Stop"), this, [servers, id] { servers->stop(id); })->setEnabled(running);
        sub->addAction(Icons::get(QStringLiteral("refresh")), tr("Restart"), this, [servers, id] { servers->restart(id); });
        sub->addAction(Icons::get(QStringLiteral("text")), tr("Log…"), this, [this, id] {
            auto *dlg = new ServerLogDialog(m_studio, id, window());
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
        });
    }
    m->addSeparator();
    m->addAction(Icons::get(QStringLiteral("settings")), tr("Local servers…"), this, [this] {
        if (m_openSettings)
            m_openSettings();
    });
}

} // namespace lumen::ui
