#pragma once

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/PartService.h"
#include "ai/ProviderRegistry.h"
#include "core/JobQueue.h"
#include "doc/CharacterLibrary.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "net/HttpClient.h"
#include "pose/Skeleton.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

namespace lumen::ui {

// Values are persisted (last page); new pages are appended.
enum class Page { Create, Generate, Editor, Poses, Library, Parts };

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
    doc::Project *project() { return m_project; }
    // Characters shared by all projects.
    doc::CharacterLibrary *characters() { return &m_characters; }

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
    // Opens an image file as a document; layered PSD files keep their layers.
    void openFile(const QString &path);
    void openPsdAsDocument(const QString &path);
    static QString openFileFilter();
    // Opens a document stored in the project; `show` switches to the editor.
    void openStoredDocument(const QString &documentId, bool show = true);
    void openAssetInEditor(const QString &assetId);
    void closeDocument(doc::Document *doc);

    // ---- session ------------------------------------------------------------------------------
    // The open documents are remembered in the project and reopened when it opens again (also after
    // a crash). Modified documents are saved a few seconds after the last change and periodically.
    void restoreSession();
    QFuture<void> saveSession();
    static constexpr int kIdleSaveMs = 4000;

    // ---- cross-page actions -------------------------------------------------------------------
    void showPage(Page page) { emit pageRequested(page); }
    void useAsReference(const QString &assetId) { emit referenceRequested(assetId); }
    void usePoseForGeneration(const pose::Pose &pose) { emit poseForGenerationRequested(pose); }
    void editPose(const pose::Pose &pose, const QString &context = QString()) { emit poseEditRequested(pose, context); }
    void usePrompt(const QString &prompt, const QString &negative) { emit promptRequested(prompt, negative); }
    void rerunAsset(const QString &assetId) { emit rerunRequested(assetId); }
    // Starts a part slicing session (Live2D) from an image asset.
    void sliceAsset(const QString &assetId)
    {
        showPage(Page::Parts);
        emit sliceRequested(assetId);
    }

    void notify(const QString &message, const QString &level = QStringLiteral("info")) { emit notification(message, level); }

signals:
    void projectOpened();
    void projectClosed();
    void documentsChanged();
    void activeDocumentChanged(lumen::doc::Document *doc);
    void pageRequested(lumen::ui::Page page);
    void referenceRequested(const QString &assetId);
    void poseForGenerationRequested(const lumen::pose::Pose &pose);
    void poseEditRequested(const lumen::pose::Pose &pose, const QString &context);
    void promptRequested(const QString &prompt, const QString &negative);
    void rerunRequested(const QString &assetId);
    void sliceRequested(const QString &assetId);
    void notification(const QString &message, const QString &level);
    void sessionRestored();

private:
    void applyConcurrency();
    void autosave();
    void adoptDocument(doc::Document *doc);
    void scheduleSessionSave();
    doc::Document *adoptLoaded(const std::shared_ptr<doc::LoadedDocument> &loaded);

    net::HttpClient m_http;
    ai::ProviderRegistry m_registry;
    JobQueue m_jobs;
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
};

} // namespace lumen::ui
