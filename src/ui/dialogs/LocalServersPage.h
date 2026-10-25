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
    QString trainerId() const; // selected trainer ("kohya"), or empty for a server program

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
    // A model chosen for `key` ("model", "diffusion_model") brings its VAE / text encoder into
    // the empty fields (Civitai companions in the library).
    // A model chosen for "model" or "diffusion_model": its preset puts it in its slot and fills the
    // components it needs from the library (ModelPresets.h).
    void applyModel(const QString &key, const QString &path);
    void setField(const QString &key, const QVariant &value);
    QString modelPath() const;
    void downloadMissing();
    void updatePreview();
    void applyPreset();
    void updatePresetRow();
    QString kind() const;

    Studio *m_studio;
    runtime::ServerProfile m_profile;
    bool m_isNew;
    QLineEdit *m_name;
    QComboBox *m_runtime;
    QSpinBox *m_port;
    QComboBox *m_start;
    QSpinBox *m_idle;
    QComboBox *m_preset;
    QFormLayout *m_form;
    QWidget *m_optionsHost;
    QHash<QString, QWidget *> m_fields;
    QPlainTextEdit *m_preview;
    QLabel *m_problem;
    QLabel *m_modelInfo;
    QPushButton *m_downloadMissing;
    QPushButton *m_ok = nullptr;
    QCheckBox *m_createProvider = nullptr;
    QString m_builtFor;
};

// Adds a server profile and, optionally, a provider bound to it; a language server takes the roles
// nobody handles yet.
void addServer(Studio *studio, const runtime::ServerProfile &profile, bool createProvider);

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
