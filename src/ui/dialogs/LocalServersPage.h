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
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QToolButton;
class QTreeWidget;

namespace lumen {
class Job;
}

namespace lumen::runtime {
struct ModelType;
}

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

// Creates or edits a server profile. What a person sees first is plain (plan M17 §17.5). For
// stable-diffusion.cpp: the model's type (FLUX.1, FLUX.2 [klein] 4B, SDXL… from the model presets,
// or Other with every field), then exactly the files that type needs, each with a line saying
// whether it is found, will be downloaded, is missing or does not fit, the add-ons the type can
// use, the graphics card's memory and the port. Fields of other types are hidden and not saved.
// Other programs show their options. The launch settings (precision, offloading, threads, extra
// arguments…), when the server starts and stops, and the command line are under a folded Advanced.
class ServerProfileDialog : public QDialog
{
    Q_OBJECT
public:
    ServerProfileDialog(Studio *studio, const runtime::ServerProfile &profile, bool isNew, QWidget *parent = nullptr);

    runtime::ServerProfile profile() const;
    bool createProvider() const;
    // The sd.cpp model type shown (runtime::ModelType::id; empty = Other, every field).
    QString modelType() const;

private:
    void buildOptions();
    // One file or folder field: the path (typed, pasted or dropped), the model library's files that
    // fit and Browse…; for sd.cpp a status line in the form's next row.
    QWidget *fileField(const runtime::OptionField &f, QWidget *parent);
    QWidget *optionField(const runtime::OptionField &f, QWidget *parent);
    // A file chosen for a field (from the library, Browse…, typed or dropped). A model file sets
    // the type from its header (applyModel); any other file is kept as chosen.
    void chooseFile(const QString &key, const QString &path);
    // A model file for "model" or "diffusion_model": its type is chosen, it goes in the type's slot,
    // and its preset fills the components that are empty or do not fit (ModelPresets.h), with the
    // preset's launch options when `launchOptions`. False when Lumen does not know it as an image
    // model.
    bool applyModel(const QString &key, const QString &path, bool launchOptions = true);
    // The type chosen by hand: its fields show, and the model moves to the type's slot.
    void typeChosen();
    void selectType(const runtime::ModelType *type);
    void showType();
    const runtime::ModelType *currentType() const;
    // Where the current type's model file goes ("model" or "diffusion_model"; empty for Other).
    QString typeModelOption() const;
    // The sd.cpp file options shown (and saved) for the current type.
    QStringList shownFileOptions() const;
    void fillLibraryMenu(QMenu *menu, const QString &key);
    void setField(const QString &key, const QVariant &value);
    QString fieldText(const QString &key) const;
    QString modelPath() const;
    void downloadMissing();
    void updatePreview();
    // The status line of each shown sd.cpp file field; the problems that have no field shown.
    void updateFileStatus(const runtime::ServerProfile &p, QStringList *problems, bool *blocking, qint64 *missingBytes, int *missingFiles);
    void applyPreset();
    void updatePresetRow();
    // The memory choice whose options the profile has, else Custom.
    void selectTier();
    QVariantMap tierOptions(int tier) const;
    void showAdvanced(bool on);
    // The forms' labels in one column (they change with the type).
    void alignForms();
    // As tall as the content (folded or not), never beyond the screen.
    void fitToContent();
    QString kind() const;

    Studio *m_studio;
    runtime::ServerProfile m_profile;
    bool m_isNew;
    QWidget *m_content;
    QScrollArea *m_scroll;
    QLineEdit *m_name;
    QComboBox *m_runtime;
    QSpinBox *m_port;
    QComboBox *m_start;
    QSpinBox *m_idle;
    QComboBox *m_preset;
    QFormLayout *m_form;       // name, program
    QFormLayout *m_memoryForm; // the graphics card, the port
    QToolButton *m_advancedToggle;
    QWidget *m_advanced;
    QWidget *m_optionsHost;    // the program's fields (rebuilt for another program)
    QWidget *m_advancedHost;   // its launch settings, under Advanced
    // sd.cpp: the model's type, its files, the add-ons (null for other programs).
    QComboBox *m_type = nullptr;
    QFormLayout *m_fileForm = nullptr;
    QFormLayout *m_addonForm = nullptr;
    QLabel *m_addonsLabel = nullptr;
    QPushButton *m_downloadMissing = nullptr;
    QWidget *m_downloadRow = nullptr;
    QHash<QString, QWidget *> m_fields; // the editor of each option
    QHash<QString, QWidget *> m_rows;   // a file field's row (path and buttons)
    QHash<QString, QLabel *> m_status;  // a file field's status line
    QStringList m_shownFiles;           // the file fields showType() shows
    QHash<QString, QPointer<Job>> m_downloads; // components being downloaded, by option
    QPlainTextEdit *m_preview;
    QLabel *m_problem;
    QDialogButtonBox *m_buttons = nullptr;
    QPushButton *m_ok = nullptr;
    QCheckBox *m_createProvider = nullptr;
    QString m_builtFor;
    bool m_tierPicked = false; // the memory size was chosen by hand (not the one detected)
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
