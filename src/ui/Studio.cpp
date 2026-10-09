#include "ui/Studio.h"

#include "ai/ProviderCatalog.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"

#include <QDir>
#include <QFileInfo>
#include <QPainter>

namespace lumen::ui {

Studio::Studio(QObject *parent)
    : QObject(parent)
    , m_registry(&m_http)
    , m_generation(&m_registry, &m_jobs)
    , m_director(&m_registry)
{
    ai::registerBuiltInProviders(m_registry);
    m_registry.load();
    applyConcurrency();
    connect(&m_registry, &ai::ProviderRegistry::providersChanged, this, &Studio::applyConcurrency);

    m_autosave.setInterval(std::max(10, Settings::instance().autosaveIntervalSec()) * 1000);
    connect(&m_autosave, &QTimer::timeout, this, &Studio::autosave);
    m_autosave.start();
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String("doc/autosaveSec"))
            m_autosave.setInterval(std::max(10, Settings::instance().autosaveIntervalSec()) * 1000);
    });
}

Studio::~Studio() = default;

void Studio::applyConcurrency()
{
    for (ai::Provider *p : m_registry.providers())
        m_jobs.setConcurrency(p->instanceId(), m_registry.concurrency(p->instanceId()));
}

QString Studio::defaultProjectPath() const
{
    return paths::defaultProjects() + QStringLiteral("/My Studio.") + doc::Project::fileExtension();
}

void Studio::openDefaultProject()
{
    const QStringList recent = Settings::instance().recentProjects();
    for (const QString &path : recent) {
        if (QFileInfo::exists(path)) {
            openProject(path);
            return;
        }
    }
    QDir().mkpath(paths::defaultProjects());
    openProject(defaultProjectPath());
}

void Studio::openProject(const QString &path)
{
    auto open = [this, path] {
        // Close documents of the previous project.
        const auto docs = m_documents;
        for (doc::Document *d : docs)
            closeDocument(d);
        if (m_project) {
            m_project->close();
            m_project->deleteLater();
            emit projectClosed();
        }
        auto *project = new doc::Project(this);
        m_project = project;
        m_generation.setProject(project);
        connect(project, &doc::Project::errorOccurred, this, [this](const QString &msg) { notify(msg, QStringLiteral("error")); });
        project->open(path)
            .then(this, [this, project, path] {
                if (project != m_project)
                    return;
                Settings::instance().addRecentProject(path);
                emit projectOpened();
            })
            .onFailed(this, [this, path](const std::exception &e) {
                notify(tr("Could not open project %1: %2").arg(QFileInfo(path).fileName(), describeException(e)), QStringLiteral("error"));
            });
    };
    if (m_project && m_project->isOpen())
        saveAll().then(this, open);
    else
        open();
}

QFuture<void> Studio::saveAll()
{
    if (!m_project || !m_project->isOpen())
        return QtFuture::makeReadyVoidFuture();
    QList<QFuture<void>> futures;
    for (doc::Document *d : std::as_const(m_documents)) {
        if (d->isModified())
            futures << m_project->saveDocument(d);
    }
    if (futures.isEmpty())
        return QtFuture::makeReadyVoidFuture();
    return QtFuture::whenAll(futures.begin(), futures.end()).then([](const QList<QFuture<void>> &) {});
}

void Studio::autosave()
{
    if (!m_project || !m_project->isOpen())
        return;
    for (doc::Document *d : std::as_const(m_documents)) {
        if (d->isModified() && !m_project->isSaving(d->id()))
            m_project->saveDocument(d);
    }
}

void Studio::adoptDocument(doc::Document *doc)
{
    doc->setParent(this);
    m_documents.append(doc);
    emit documentsChanged();
    setActiveDocument(doc);
}

void Studio::setActiveDocument(doc::Document *doc)
{
    if (m_active == doc)
        return;
    m_active = doc;
    emit activeDocumentChanged(doc);
}

doc::Document *Studio::newDocument(const QSize &size, const QString &name, const QColor &background)
{
    auto *doc = new doc::Document(size);
    doc->setName(name);
    QImage bg(size, QImage::Format_ARGB32_Premultiplied);
    bg.fill(background);
    doc->addLayer(tr("Background"), bg);
    doc->undoStack()->clear();
    adoptDocument(doc);
    return doc;
}

doc::Document *Studio::openImageAsDocument(const QImage &image, const QString &name, const QJsonObject &meta)
{
    auto *doc = new doc::Document(image.size());
    doc->setName(name);
    doc->addLayer(name.isEmpty() ? tr("Image") : name, img::normalized(image), meta);
    doc->undoStack()->clear();
    adoptDocument(doc);
    showPage(Page::Editor);
    return doc;
}

void Studio::openAssetInEditor(const QString &assetId)
{
    if (!m_project)
        return;
    const auto info = m_project->asset(assetId);
    m_project->loadImage(assetId)
        .then(this, [this, info, assetId](QImage image) {
            QJsonObject meta = info ? info->metadata : QJsonObject();
            meta.insert(QStringLiteral("source_asset"), assetId);
            openImageAsDocument(image, info ? info->name : tr("Image"), meta);
        })
        .onFailed(this, [this](const std::exception &e) { notify(describeException(e), QStringLiteral("error")); });
}

void Studio::openStoredDocument(const QString &documentId)
{
    for (doc::Document *d : std::as_const(m_documents)) {
        if (d->id() == documentId) {
            setActiveDocument(d);
            showPage(Page::Editor);
            return;
        }
    }
    if (!m_project)
        return;
    m_project->loadDocument(documentId)
        .then(this, [this](std::shared_ptr<doc::LoadedDocument> loaded) {
            auto *doc = new doc::Document(loaded->size);
            doc->setId(loaded->id);
            doc->setName(loaded->name);
            doc->replaceContentRaw(loaded->size, loaded->layers, loaded->selection);
            doc->setActiveLayer(loaded->active);
            doc->setSettings(loaded->settings);
            if (loaded->pose)
                doc->setControlPose(loaded->pose);
            doc->undoStack()->clear();
            doc->setModified(false);
            adoptDocument(doc);
            showPage(Page::Editor);
        })
        .onFailed(this, [this](const std::exception &e) { notify(describeException(e), QStringLiteral("error")); });
}

void Studio::closeDocument(doc::Document *doc)
{
    if (!doc || !m_documents.contains(doc))
        return;
    if (doc->isModified() && m_project && m_project->isOpen()) {
        // Persist before closing; deletion waits for the save to finish.
        QPointer<doc::Document> guard(doc);
        m_project->saveDocument(doc).then(this, [guard] {
            if (guard)
                guard->deleteLater();
        });
    } else {
        doc->deleteLater();
    }
    m_documents.removeAll(doc);
    if (m_active == doc)
        setActiveDocument(m_documents.isEmpty() ? nullptr : m_documents.last());
    emit documentsChanged();
}

} // namespace lumen::ui
