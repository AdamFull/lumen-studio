#pragma once

#include "ai/ProviderRegistry.h"
#include "runtime/AiSetup.h"

#include <QDialog>
#include <QHash>
#include <QPointer>

#include <optional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QVBoxLayout;

namespace lumen {
class Job;
}

namespace lumen::ui {

class LocalSetup;
class Studio;

// "Set up AI" (plan M17 §17.5): what opens on the first start, and from Help → Set up AI… and
// the Local servers page. Three ways to get an image engine, each page saying in plain words what
// will happen, how long it takes and how much it downloads:
//  1. Run on this computer: detects the graphics card, suggests one model preset (FLUX.2 [klein]
//     4B on 8 GB and more, an SDXL model below), takes its files from the library and the folders
//     it suggests (ComfyUI, WebUI, LM Studio, Downloads) and downloads only what is missing, installs
//     stable-diffusion.cpp, creates the server and the provider (LocalSetup with a preset plan) and
//     makes a test image. A language model only on request, and only while a plugin needs one.
//  2. Use an online service: fal.ai, OpenAI or Gemini with one key field, then a test image.
//  3. Connect a server that is already running: ComfyUI, Forge / WebUI or sd-server, found on their
//     default ports or by an address, then a test image.
class SetupWizard : public QDialog
{
    Q_OBJECT
public:
    enum class Page { Choose, Local, Online, Server, Result };

    explicit SetupWizard(Studio *studio, QWidget *parent = nullptr);
    ~SetupWizard() override;

    void showPage(Page page);
    Page currentPage() const;

    // Set before the wizard is first shown (tests, the layout audit): the hardware instead of
    // probing it, the folders to look for models in instead of ModelLibrary::suggestedRoots, the
    // presets to choose from instead of the bundled ones, and the base URLs of online services by
    // provider type ("openai_image" → a mock server).
    std::optional<runtime::HardwareInfo> hardwareOverride;
    std::optional<QStringList> searchFolders;
    QList<runtime::ModelPreset> presets;
    QHash<QString, QString> serviceUrls;
    // Tests: what every new wizard starts with, so one opened from a menu neither probes this
    // computer nor reads its folders.
    static inline std::optional<runtime::HardwareInfo> testHardware;
    static inline std::optional<QStringList> testSearchFolders;

    // Makes a test image on a provider and shows it on the result page, with its progress and Stop.
    void runTestImage(const QString &providerId, const QString &title, const QString &intro);
    // Reads a folder with model files the wizard did not look in (Use models from a folder…) and
    // plans again with its files; folderRead() when done.
    void useModelsFolder(const QString &folder);

    // What the local page found and plans (after localPlanReady).
    std::optional<runtime::LocalRecommendation> recommendation() const { return m_recommendation; }
    const runtime::LocalModelPlan &localPlan() const { return m_plan; }
    LocalSetup *localSetup() const;
    // The provider the last test image ran on, and whether it worked.
    QString testedProvider() const { return m_testedProvider; }

signals:
    void localPlanReady();
    void folderRead();
    void serversFound();
    void testImageFinished(bool ok);

protected:
    void showEvent(QShowEvent *e) override;

private:
    QWidget *buildChoosePage();
    QWidget *buildLocalPage();
    QWidget *buildOnlinePage();
    QWidget *buildServerPage();
    QWidget *buildResultPage();
    QPushButton *backButton(QWidget *parent);

    // Run on this computer.
    void startLooking();      // the hardware and the program releases, at the first show
    void lookForFiles();      // the folders, once the local page opens
    void showRecommendation();
    void maybePlan();
    const runtime::ModelPreset *preset(const QString &id) const;
    void refreshPlan();
    void startLocal();
    void showLocalProgress();
    void localFinished(bool ok, const QString &message);

    // Use an online service.
    void selectService(const QString &typeId);
    void connectService();

    // Connect a running server.
    void detectServers();
    void useServer(const ai::DetectedServer &server);
    void connectAddress();

    Studio *m_studio;
    QStackedWidget *m_pages;
    Page m_backTo = Page::Choose;
    bool m_started = false;

    // Choose.
    QLabel *m_localHint;
    QLabel *m_serverHint;

    // Local.
    QPointer<LocalSetup> m_setup;
    bool m_hardwareReady = false;
    bool m_lookingForFiles = false;
    bool m_foldersReady = false;
    QList<runtime::FoundFolder> m_found;
    QString m_folderNote; // what was found in a folder chosen with Use models from a folder…
    std::optional<runtime::LocalRecommendation> m_recommendation;
    runtime::LocalModelPlan m_plan;
    QStackedWidget *m_localStack;
    QLabel *m_hardware;
    QLabel *m_advice;
    QPushButton *m_adviceOnline;
    QComboBox *m_model;
    QLabel *m_modelInfo;
    QCheckBox *m_language;
    QLabel *m_languageInfo;
    QTreeWidget *m_planTree;
    QLabel *m_summary;
    QPushButton *m_setUp;
    QTreeWidget *m_progress;
    QLabel *m_progressStatus;

    // Online.
    QString m_service;
    QButtonGroup *m_services;
    QLineEdit *m_key;
    QLabel *m_keyLink;
    QLabel *m_onlineStatus;
    QPushButton *m_connectService;
    bool m_checkingKey = false;

    // Server.
    QVBoxLayout *m_foundList;
    QLabel *m_foundStatus;
    QLineEdit *m_address;
    QLabel *m_serverStatus;
    QList<ai::DetectedServer> m_servers;
    int m_serverSearch = 0; // the search whose answers fill the list

    // Result.
    QLabel *m_resultTitle;
    QLabel *m_resultText;
    QLabel *m_resultImage;
    QPushButton *m_retry;
    QProgressBar *m_testProgress;
    QPushButton *m_stopTest;
    QString m_testedProvider;
    QPointer<Job> m_testJob;
};

// Opens the setup wizard on a page: one at a time (an open one comes to the front), not modal,
// deleted when closed. The entry point for the first start, the Help menu, the Local servers page
// and the start screen. Over a modal dialog (Settings) the wizard is that dialog's child, also when
// it was open already, so the dialog does not block it; when the dialog closes, the wizard moves
// to the window under it.
SetupWizard *openSetupWizard(Studio *studio, QWidget *parent, SetupWizard::Page page = SetupWizard::Page::Choose);

} // namespace lumen::ui
