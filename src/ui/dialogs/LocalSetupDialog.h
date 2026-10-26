#pragma once

#include "runtime/AiSetup.h"
#include "runtime/Catalog.h"
#include "runtime/Hardware.h"
#include "runtime/ModelHub.h"

#include <QDialog>
#include <QFuture>
#include <QPointer>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

namespace lumen {
class Job;
}

namespace lumen::ui {

class Studio;

// "Set up local AI": installs llama.cpp and stable-diffusion.cpp when they are missing, downloads
// a starter set of models that suits the GPU (or reuses files already on this computer), then
// creates the servers and providers and makes them the defaults. Lives on after its dialog closes
// so that a running setup still finishes. The setup wizard (SetupWizard.h) uses it with a model
// preset's plan instead of the set's image files.
class LocalSetup : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString setId;
        bool language = true;   // llama.cpp with a vision language model
        bool images = true;     // stable-diffusion.cpp with ControlNet, IP-Adapter and an upscaler
        // The image model from a model preset instead of the set's image files: its files from the
        // library and found folders (which are added to the library) or downloaded, the server
        // options from the preset (runtime::planServerOptions).
        std::optional<runtime::LocalModelPlan> model;
    };
    struct RuntimeStep
    {
        QString kind;           // "llama.cpp", "sd.cpp"
        QString existingId;     // installed or adopted runtime that is reused
        runtime::InstallPlan plan;
        QString error;          // the release could not be looked up
    };
    struct FileStep
    {
        runtime::StarterFile file;
        QString localPath;      // already on this computer
    };

    explicit LocalSetup(Studio *studio, QObject *parent = nullptr);

    // Probes the hardware (unless it is given) and looks up releases of missing programs.
    QFuture<void> prepare(const std::optional<runtime::HardwareInfo> &hardware = std::nullopt);
    std::optional<runtime::HardwareInfo> hardware() const { return m_hw; }
    QList<runtime::StarterSet> sets() const { return m_sets; }
    void setSets(const QList<runtime::StarterSet> &sets) { m_sets = sets; } // tests
    QString recommendedSet() const;
    RuntimeStep runtimeStep(const QString &kind) const { return m_runtimeSteps.value(kind); }
    QList<FileStep> fileSteps(const Options &o) const;
    qint64 downloadSize(const Options &o) const;

    // Queues the installs and downloads, then configures servers and providers.
    void run(const Options &o);
    bool isRunning() const { return m_running; }
    QList<QPointer<Job>> jobs() const { return m_jobs; }
    // Results after finished(true).
    QString languageServer() const { return m_llamaProfile; }
    QString imageServer() const { return m_sdProfile; }

signals:
    void jobsChanged();
    void finished(bool ok, const QString &message);

private:
    static QStringList kindsFor(const Options &o);
    void checkDone();
    void configure();
    // The image server of a preset plan (Options::model); its name when it was created.
    QString configureModel(runtime::VramTier tier);

    Studio *m_studio;
    std::optional<runtime::HardwareInfo> m_hw;
    QList<runtime::StarterSet> m_sets;
    QHash<QString, RuntimeStep> m_runtimeSteps;
    Options m_options;
    bool m_running = false;
    QList<QPointer<Job>> m_jobs;
    QHash<QString, QPointer<Job>> m_runtimeJobs; // kind → install job
    QHash<QString, QPointer<Job>> m_fileJobs;    // role → download job
    QHash<QString, QString> m_paths;             // role → file
    QHash<QString, QPointer<Job>> m_modelJobs;   // preset plan: server option → download job
    QHash<QString, QString> m_modelPaths;        // preset plan: server option → file
    QString m_llamaProfile;
    QString m_sdProfile;
};

class LocalSetupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LocalSetupDialog(Studio *studio, QWidget *parent = nullptr);

    LocalSetup *setup() const { return m_setup; }
    // Fills the plan page (after prepare()); called again when the choices change.
    void refreshPlan();
    void start();

private:
    LocalSetup::Options options() const;
    void showProgress();
    void showResult(bool ok, const QString &message);
    void tryGeneration();

    Studio *m_studio;
    QPointer<LocalSetup> m_setup;
    QStackedWidget *m_pages;
    QLabel *m_hardware;
    QComboBox *m_set;
    QCheckBox *m_language;
    QCheckBox *m_images;
    QTreeWidget *m_plan;
    QLabel *m_summary;
    QPushButton *m_start;
    QTreeWidget *m_progress;
    QLabel *m_progressStatus;
    QLabel *m_result;
    QPushButton *m_try;
    QPushButton *m_again;
    QLabel *m_image;
};

} // namespace lumen::ui
