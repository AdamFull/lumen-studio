#pragma once

#include <QToolButton>

#include <functional>

namespace lumen::ui {

class Studio;

// Status-bar button for the local servers Lumen runs: shows what is starting, running or failed
// and offers start/stop/restart/log per server. Hidden while no server is set up.
class ServerStatusButton : public QToolButton
{
    Q_OBJECT
public:
    ServerStatusButton(Studio *studio, std::function<void()> openSettings, QWidget *parent = nullptr);

private:
    void refresh();
    void buildMenu();

    Studio *m_studio;
    std::function<void()> m_openSettings;
};

} // namespace lumen::ui
