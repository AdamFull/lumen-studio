#pragma once

#include "runtime/Catalog.h"
#include "runtime/ServerProfile.h"

#include <QDialog>
#include <QHash>
#include <QPointer>
#include <QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace lumen::ui {

class Studio;

// Settings → Local servers: server programs (installed from GitHub or existing installs) and the
// servers Lumen runs with them.
class LocalServersPage : public QWidget
{
    Q_OBJECT
public:
    explicit LocalServersPage(Studio *studio, QWidget *parent = nullptr);

    // Hardware info shared with the dialogs (probed once per page).
    const std::optional<runtime::HardwareInfo> &hardware() const { return m_hardware; }

private:
    void reloadRuntimes();
    void reloadServers();
    void updateServerButtons();
    QString selectedRuntime() const;
    QString selectedServer() const;
    void install();
    void addExisting();
    void checkUpdates();
    void removeRuntime();
    void newServer();
    void editServer(const QString &id);
    void removeServer();

    Studio *m_studio;
    std::optional<runtime::HardwareInfo> m_hardware;
    QLabel *m_hardwareLabel;
    QTreeWidget *m_runtimes;
    QTreeWidget *m_servers;
    QPushButton *m_removeRuntime;
    QPushButton *m_editServer;
    QPushButton *m_startServer;
    QPushButton *m_stopServer;
    QPushButton *m_logServer;
    QPushButton *m_removeServer;
    QLabel *m_updates;
};

// Chooses a runtime and build, shows the newest release and its download size, and starts the
// install job.
class InstallRuntimeDialog : public QDialog
{
    Q_OBJECT
public:
    InstallRuntimeDialog(Studio *studio, const std::optional<runtime::HardwareInfo> &hw, const QString &runtimeId = QString(),
                         QWidget *parent = nullptr);

private:
    void fetch();
    void updatePlan();

    Studio *m_studio;
    std::optional<runtime::HardwareInfo> m_hw;
    QComboBox *m_runtime;
    QComboBox *m_backend;
    QLabel *m_about;
    QLabel *m_release;
    QPushButton *m_install;
    QHash<QString, QList<runtime::Release>> m_releases;
    runtime::InstallPlan m_plan;
};

// Creates or edits a server profile: program, port, lifecycle and the kind's launch options.
class ServerProfileDialog : public QDialog
{
    Q_OBJECT
public:
    ServerProfileDialog(Studio *studio, const runtime::ServerProfile &profile, bool isNew, QWidget *parent = nullptr);

    runtime::ServerProfile profile() const;
    bool createProvider() const;

private:
    void buildOptions();
    void updatePreview();
    QString kind() const;

    Studio *m_studio;
    runtime::ServerProfile m_profile;
    bool m_isNew;
    QLineEdit *m_name;
    QComboBox *m_runtime;
    QSpinBox *m_port;
    QComboBox *m_start;
    QSpinBox *m_idle;
    QWidget *m_optionsHost;
    QHash<QString, QWidget *> m_fields;
    QPlainTextEdit *m_preview;
    QLabel *m_problem;
    QCheckBox *m_createProvider = nullptr;
    QString m_builtFor;
};

// Live log of a server with start/stop controls.
class ServerLogDialog : public QDialog
{
    Q_OBJECT
public:
    ServerLogDialog(Studio *studio, const QString &profileId, QWidget *parent = nullptr);

private:
    void updateState();

    Studio *m_studio;
    QString m_id;
    QPlainTextEdit *m_log;
    QLabel *m_state;
    QPushButton *m_start;
    QPushButton *m_stop;
};

} // namespace lumen::ui
