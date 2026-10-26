#pragma once

#include "ai/CharacterContext.h"
#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/ModelDefaults.h"
#include "ai/PartService.h"
#include "ai/ProviderRegistry.h"
#include "core/JobQueue.h"
#include "doc/CharacterLibrary.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "net/HttpClient.h"
#include "pose/Skeleton.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelLibrary.h"
#include "runtime/ModelsFolder.h"
#include "runtime/RunPod.h"
#include "runtime/RuntimeStore.h"
#include "runtime/Supervisor.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

namespace lumen::ui {

// What Studio::showPage opens: the editor (the main window), New from prompt ("generate") and Browse
// project images ("library"), ids kept from the pages they replace. Plugins add their own pages
// (ext::PageSpec), e.g. "poses" or "parts", each in a window of its own; the core names those only
// to ask whether they exist (Studio::hasPage).
namespace pages {
inline constexpr QLatin1StringView Generate{"generate"};
inline constexpr QLatin1StringView Editor{"editor"};
inline constexpr QLatin1StringView Library{"library"};
} // namespace pages

// The model preset of a server's model file, as its provider sees it ("model_preset").
ai::ModelDefaults modelDefaultsFor(const runtime::ModelLibrary *library, const QString &file);

// Application context shared by all UI components: services, the open project, open documents
// and cross-page actions ("open this image in the editor", "use this pose for generation"...).
class Studio : public QObject
{
    Q_OBJECT
public:
    explicit Studio(QObject *parent = nullptr);
    ~Studio() override;

    net::HttpClient *http() { return &m_http; }
    ai::ProviderRegistry *providers() { return &m_registry; }
    JobQueue *jobs() { return &m_jobs; }
    ai::GenerationService *generation() { return &m_generation; }
    ai::Director *director() { return &m_director; }
    ai::PartService *parts() { return &m_parts; }
    // Local server programs (installed or adopted) and the servers Lumen runs with them.
    runtime::RuntimeStore *runtimes() { return &m_runtimes; }
    runtime::ServerSupervisor *servers() { return &m_servers; }
    // Model files on this computer (Lumen's models folder and folders the user added).
    runtime::ModelLibrary *models() { return &m_models; }
    // Hugging Face / Civitai search and downloads into the library.
    runtime::ModelHub *hub() { return &m_hub; }
    // Moves Lumen's models folder (Settings → Models → Move…), see runtime::moveModelsFolder. The
    // chosen folder is kept in the "models/folder" setting; characters' LoRA files follow it.
    Job *moveModelsFolder(const QString &to);
    // An earlier version's models folder (<app data>/models) with models in it while no folder is
    // chosen; "" when there is nothing to ask. The library uses it until MainWindow has asked, once,
    // at start-up.
    QString legacyModelsFolder() const { return m_legacyModels; }
    // Settles that question (runtime::settleLegacyModels): moveThem(plan) decides. Returns the
    // move job, if any.
    Job *settleLegacyModels(const std::function<bool(const runtime::FolderMovePlan &plan)> &moveThem);
    // RunPod API (cloud LoRA training, M15).
    runtime::RunPodClient *runPod() { return &m_runPod; }
    // Provider of a managed server (bound through its "managed_server" setting), if any.
    ai::Provider *providerForServer(const QString &profileId) const;
    doc::Project *project() { return m_project; }
    // Characters shared by all projects.
    doc::CharacterLibrary *characters() { return &m_characters; }
    // The project's cast (M16 §16.3): one or two character library ids in order (left, right),
    // or none. Stored in the project. Image tools draw it unless their page switches it off;
    // pages that draw one character use the first, the active character (M14 §14.2).
    static constexpr int kMaxCast = 2;
    QStringList activeCast() const { return m_cast; }
    // Empty ids, duplicates, ids not in the library and any beyond kMaxCast are dropped.
    void setActiveCast(const QStringList &ids);
    QString activeCharacter() const { return m_cast.value(0); }
    // A cast of one (or none for an empty id).
    void setActiveCharacter(const QString &id);
    // A library character as image requests use it (sheet, references, LoRA). Resolves with an
    // empty context when the character does not exist.
    QFuture<ai::CharacterContext> characterContext(const QString &id);

    // ---- project lifecycle --------------------------------------------------------------------
    void openProject(const QString &path);
    void openDefaultProject();
    QString defaultProjectPath() const;
    bool isProjectOpen() const { return m_project && m_project->isOpen(); }
    // Saves all modified documents. The returned future resolves when everything is written.
    QFuture<void> saveAll();

    // ---- documents ----------------------------------------------------------------------------
    const QList<doc::Document *> &documents() const { return m_documents; }
    doc::Document *activeDocument() const { return m_active; }
    void setActiveDocument(doc::Document *doc);
    doc::Document *newDocument(const QSize &size, const QString &name, const QColor &background = Qt::white);
    doc::Document *openImageAsDocument(const QImage &image, const QString &name, const QJsonObject &meta = {});
    // Opens an image file as a document that remembers the file (File → Save writes it back);
    // layered PSD files keep their layers. A file that is open already is shown; one the project
    // keeps as a document, unchanged since (in the document and on disk), opens that document with
    // its layers. Either way the file goes to the top of File → Open recent.
    void openFile(const QString &path);
    void openPsdAsDocument(const QString &path);
    static QString openFileFilter();
    // The open document that saves to `path`, if any.
    doc::Document *documentForFile(const QString &path) const;
    // Opens a document stored in the project; `show` switches to the editor.
    void openStoredDocument(const QString &documentId, bool show = true);
    void openAssetInEditor(const QString &assetId);
    // Closes the document without asking (the editor asks first about changes not saved to its
    // file); the project keeps it. The future finishes once the project has saved it.
    QFuture<void> closeDocument(doc::Document *doc);

    // ---- session ------------------------------------------------------------------------------
    // The open documents are remembered in the project and reopened when it opens again (also after
    // a crash). Modified documents are saved a few seconds after the last change and periodically.
    void restoreSession();
    QFuture<void> saveSession();
    static constexpr int kIdleSaveMs = 4000;

    // ---- cross-page actions -------------------------------------------------------------------
    // Shows the editor, opens New from prompt or Browse project images, or opens (and raises) a
    // plugin page's window (see pages).
    void showPage(const QString &id) { emit pageRequested(id); }
    // Whether a page exists in this session: a core page, or one of a plugin that is on.
    bool hasPage(const QString &id) const;
    // Adds an image to the references of the editor's generation (the AI panel).
    void useAsReference(const QString &assetId) { emit referenceRequested(assetId); }
    // A new image with this pose: New from prompt, with the pose as the new document's control pose.
    void usePoseForGeneration(const pose::Pose &pose) { emit poseForGenerationRequested(pose); }
    // Opens the pose in the Poses plugin's editor. Its window is built on first use, so it is opened
    // before the request goes out.
    void editPose(const pose::Pose &pose, const QString &context = QString())
    {
        showPage(QStringLiteral("poses"));
        emit poseEditRequested(pose, context);
    }
    // Puts a prompt into the editor's prompt field.
    void usePrompt(const QString &prompt, const QString &negative) { emit promptRequested(prompt, negative); }
    // New from prompt with the prompt, negative prompt, size and seed of an image.
    void rerunAsset(const QString &assetId) { emit rerunRequested(assetId); }
    // Starts a part slicing session (Live2D) from an image asset.
    void sliceAsset(const QString &assetId)
    {
        showPage(QStringLiteral("parts"));
        emit sliceRequested(assetId);
    }

    void notify(const QString &message, const QString &level = QStringLiteral("info")) { emit notification(message, level); }

signals:
    void projectOpened();
    void projectClosed();
    void documentsChanged();
    void activeDocumentChanged(lumen::doc::Document *doc);
    // The document is closing (it is still in documents()): what works on it for the user stops.
    void documentClosing(lumen::doc::Document *doc);
    void pageRequested(const QString &id);
    void referenceRequested(const QString &assetId);
    void poseForGenerationRequested(const lumen::pose::Pose &pose);
    void poseEditRequested(const lumen::pose::Pose &pose, const QString &context);
    void promptRequested(const QString &prompt, const QString &negative);
    void rerunRequested(const QString &assetId);
    void sliceRequested(const QString &assetId);
    void notification(const QString &message, const QString &level);
    // A Settings page should open (its id, as SettingsDialog::showTab takes it): asked for by a widget
    // that cannot reach the main window, such as the LoRA picker in a popup.
    void settingsRequested(const QString &tab);
    void sessionRestored();
    // The cast changed: a member, the order, or none.
    void activeCharacterChanged();

private:
    void applyConcurrency();
    // Gates providers bound to managed servers and keeps their URLs in sync with the server ports.
    void applyServerBindings();
    void autosave();
    void adoptDocument(doc::Document *doc);
    void scheduleSessionSave();
    void restoreActiveCharacter();
    doc::Document *adoptLoaded(const std::shared_ptr<doc::LoadedDocument> &loaded);

    net::HttpClient m_http;
    ai::ProviderRegistry m_registry;
    JobQueue m_jobs;
    runtime::RuntimeStore m_runtimes;
    runtime::ServerSupervisor m_servers; // destroyed before the providers: stops the servers
    runtime::ModelLibrary m_models;
    runtime::ModelHub m_hub;
    runtime::RunPodClient m_runPod{&m_http};
    ai::GenerationService m_generation;
    ai::Director m_director;
    ai::PartService m_parts;
    QPointer<doc::Project> m_project;
    doc::CharacterLibrary m_characters{doc::CharacterLibrary::defaultDirectory()};
    QList<doc::Document *> m_documents;
    QPointer<doc::Document> m_active;
    QTimer m_autosave;
    QTimer m_idleSave;
    QTimer m_sessionSave;
    bool m_restoring = false;
    bool m_switching = false;
    QStringList m_cast;
    QString m_legacyModels;
};

} // namespace lumen::ui
