#pragma once

#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/ProviderRegistry.h"
#include "core/JobQueue.h"
#include "doc/Document.h"
#include "doc/Project.h"
#include "net/HttpClient.h"
#include "pose/Skeleton.h"

#include <QObject>
#include <QPointer>
#include <QTimer>

namespace lumen::ui {

enum class Page { Create, Generate, Editor, Poses, Library };

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
    doc::Project *project() { return m_project; }

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
    void openStoredDocument(const QString &documentId);
    void openAssetInEditor(const QString &assetId);
    void closeDocument(doc::Document *doc);

    // ---- cross-page actions -------------------------------------------------------------------
    void showPage(Page page) { emit pageRequested(page); }
    void useAsReference(const QString &assetId) { emit referenceRequested(assetId); }
    void usePoseForGeneration(const pose::Pose &pose) { emit poseForGenerationRequested(pose); }
    void editPose(const pose::Pose &pose, const QString &context = QString()) { emit poseEditRequested(pose, context); }
    void usePrompt(const QString &prompt, const QString &negative) { emit promptRequested(prompt, negative); }
    void rerunAsset(const QString &assetId) { emit rerunRequested(assetId); }

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
    void notification(const QString &message, const QString &level);

private:
    void applyConcurrency();
    void autosave();
    void adoptDocument(doc::Document *doc);

    net::HttpClient m_http;
    ai::ProviderRegistry m_registry;
    JobQueue m_jobs;
    ai::GenerationService m_generation;
    ai::Director m_director;
    QPointer<doc::Project> m_project;
    QList<doc::Document *> m_documents;
    QPointer<doc::Document> m_active;
    QTimer m_autosave;
};

} // namespace lumen::ui
