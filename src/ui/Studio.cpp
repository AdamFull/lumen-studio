#include "ui/Studio.h"

#include "ai/FilterNodes.h"
#include "ai/ProviderCatalog.h"
#include "core/Async.h"
#include "doc/ImageFile.h"
#include "doc/PsdIO.h"
#include "core/Error.h"
#include "core/ImageOps.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "ai/ModelDefaults.h"
#include "runtime/ModelPresets.h"
#include "ui/ext/Extension.h"

#include <QColorSpace>
#include <QDir>
#include <QJsonArray>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>

namespace lumen::ui {

namespace {
const QString kModelsFolderKey = QStringLiteral("models/folder");
// The question about an earlier version's models folder was asked (settleLegacyModels).
const QString kLegacyAskedKey = QStringLiteral("models/legacy_asked");
} // namespace

Studio::Studio(QObject *parent)
    : QObject(parent)
    , m_registry(&m_http)
    , m_runtimes(&m_http, &m_jobs)
    , m_servers(&m_runtimes, &m_http)
    , m_models(&m_http)
    , m_hub(&m_http, &m_jobs, &m_models)
    , m_generation(&m_registry, &m_jobs)
    , m_director(&m_registry)
    , m_parts(&m_registry, &m_jobs, &m_generation)
{
    ai::registerBuiltInProviders(m_registry);
    m_registry.load();
    // The AI steps of node filters run through this studio's engines; registered before any
    // filter is loaded (FilterCatalog), or filters with AI steps would not load.
    ai::registerFilterNodes(&m_generation);
    applyConcurrency();
    connect(&m_registry, &ai::ProviderRegistry::providersChanged, this, &Studio::applyConcurrency);
    m_runtimes.load();
    m_servers.load();
    // An sd.cpp profile that cannot work (a model in the wrong slot, a missing text encoder, a
    // ControlNet of another family) fails with what to change instead of a server exit code.
    m_servers.preflight = [this](const runtime::ServerProfile &profile, const QString &kind) {
        if (kind != QLatin1String("sd.cpp"))
            return QString();
        QStringList messages;
        for (const runtime::ProfileProblem &p : runtime::checkSdProfile(&m_models, profile))
            messages << p.message;
        return messages.join(QLatin1Char(' '));
    };
    applyServerBindings();
    connect(&m_registry, &ai::ProviderRegistry::providersChanged, this, &Studio::applyServerBindings);
    connect(&m_servers, &runtime::ServerSupervisor::profilesChanged, this, &Studio::applyServerBindings);
    QTimer::singleShot(0, &m_servers, &runtime::ServerSupervisor::startAutoStartServers);
    // Lumen's models folder: the one chosen in Settings → Models, else Documents\Lumen\Models.
    m_models.setFolder(Settings::instance().value(kModelsFolderKey).toString());
    // Models in an earlier version's folder stay in use (not saved as the choice) until MainWindow
    // has asked once whether to move them.
    if (!Settings::instance().value(kLegacyAskedKey).toBool())
        m_legacyModels = runtime::legacyModelsToAsk(&m_models);
    if (!m_legacyModels.isEmpty())
        m_models.setFolder(m_legacyModels);
    m_models.load();
    QTimer::singleShot(0, &m_models, &runtime::ModelLibrary::rescan); // new or changed files since the last run
    connect(&m_models, &runtime::ModelLibrary::folderChanged, this, [this] {
        if (m_models.chosenFolder().isEmpty())
            Settings::instance().remove(kModelsFolderKey);
        else
            Settings::instance().setValue(kModelsFolderKey, m_models.chosenFolder());
    });
    // Characters' LoRA files moved with the folder (those that stayed keep their paths).
    connect(&m_models, &runtime::ModelLibrary::relocated, this, [this](const QString &from, const QString &to) {
        for (const doc::CharacterEntry &c : m_characters.entries()) {
            QJsonObject lora = c.lora;
            const QString file = lora.value(QStringLiteral("file")).toString();
            const QString moved = runtime::movedPath(file, from, to);
            if (moved != file) {
                lora.insert(QStringLiteral("file"), moved);
                m_characters.setLora(c.id, lora);
            }
        }
    });

    m_autosave.setInterval(std::max(10, Settings::instance().autosaveIntervalSec()) * 1000);
    connect(&m_autosave, &QTimer::timeout, this, &Studio::autosave);
    m_autosave.start();
    m_idleSave.setSingleShot(true);
    m_idleSave.setInterval(kIdleSaveMs);
    connect(&m_idleSave, &QTimer::timeout, this, &Studio::autosave);
    m_sessionSave.setSingleShot(true);
    m_sessionSave.setInterval(500);
    connect(&m_sessionSave, &QTimer::timeout, this, &Studio::saveSession);
    connect(this, &Studio::documentsChanged, this, &Studio::scheduleSessionSave);
    connect(this, &Studio::activeDocumentChanged, this, &Studio::scheduleSessionSave);
    connect(this, &Studio::projectOpened, this, [this] {
        if (Settings::instance().value(QStringLiteral("doc/restoreSession"), true).toBool())
            restoreSession();
        restoreActiveCharacter();
    });
    connect(this, &Studio::projectClosed, this, [this] {
        if (!m_cast.isEmpty()) {
            m_cast.clear();
            emit activeCharacterChanged();
        }
    });
    // A character removed from the library leaves the cast.
    connect(&m_characters, &doc::CharacterLibrary::changed, this, [this] {
        QStringList kept = m_cast;
        kept.removeIf([this](const QString &id) { return !m_characters.entry(id); });
        if (kept != m_cast)
            setActiveCast(kept);
    });
    m_generation.setSafeContent(Settings::instance().value(QStringLiteral("generation/safeContent"), false).toBool());
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key == QLatin1String("doc/autosaveSec"))
            m_autosave.setInterval(std::max(10, Settings::instance().autosaveIntervalSec()) * 1000);
        if (key == QLatin1String("generation/safeContent"))
            m_generation.setSafeContent(Settings::instance().value(key, false).toBool());
    });
}

Studio::~Studio() = default;

namespace {

// Starts a managed server before a provider's requests and keeps it from idling out meanwhile.
class ServerGate : public ai::ProviderGate
{
public:
    ServerGate(runtime::ServerSupervisor *servers, QString profileId)
        : m_servers(servers)
        , m_profileId(std::move(profileId))
    {
    }
    QFuture<QString> acquire() override
    {
        if (!m_servers)
            return QtFuture::makeReadyValueFuture(QObject::tr("Lumen is shutting down."));
        const QString kind = m_servers->kindOf(m_profileId);
        if (kind == QLatin1String("llama.cpp"))
            return m_servers->acquire(m_profileId);
        // An image server: first let language servers that share the GPU unload their models.
        QList<QFuture<void>> unloads;
        for (const runtime::ServerProfile &p : m_servers->profiles()) {
            if (m_servers->kindOf(p.id) == QLatin1String("llama.cpp") && p.option(QStringLiteral("unload_for_images"), QStringLiteral("llama.cpp")).toBool()
                && m_servers->state(p.id) == runtime::ServerState::Ready)
                unloads << m_servers->unloadModels(p.id);
        }
        if (unloads.isEmpty())
            return m_servers->acquire(m_profileId);
        QPointer<runtime::ServerSupervisor> servers = m_servers;
        const QString id = m_profileId;
        return QtFuture::whenAll(unloads.begin(), unloads.end())
            .then(servers.data(), [servers, id](const QList<QFuture<void>> &) {
                return servers ? servers->acquire(id) : QtFuture::makeReadyValueFuture(QObject::tr("Lumen is shutting down."));
            })
            .unwrap();
    }
    void release() override
    {
        if (m_servers)
            m_servers->release(m_profileId);
    }

private:
    QPointer<runtime::ServerSupervisor> m_servers;
    QString m_profileId;
};

// Says how a move of the models folder ended; the Queue shows its progress.
Job *reportFolderMove(Studio *studio, Job *job)
{
    if (!job)
        return nullptr;
    QObject::connect(job, &Job::finished, studio, [studio](Job *j) {
        if (j->state() == Job::State::Succeeded)
            studio->notify(Studio::tr("The models are now in %1.").arg(QDir::toNativeSeparators(j->result().toString())), QStringLiteral("success"));
        else if (j->state() == Job::State::Failed)
            studio->notify(Studio::tr("The models folder was not moved: %1").arg(j->errorString()), QStringLiteral("error"));
    });
    return job;
}

} // namespace

Job *Studio::moveModelsFolder(const QString &to)
{
    return reportFolderMove(this, runtime::moveModelsFolder(&m_jobs, &m_models, &m_servers, to));
}

Job *Studio::settleLegacyModels(const std::function<bool(const runtime::FolderMovePlan &plan)> &moveThem)
{
    const QString legacy = std::exchange(m_legacyModels, QString());
    if (legacy.isEmpty())
        return nullptr;
    // Asked once: the old folder is the choice until its files are in the new one (a successful
    // move changes the folder, and the setting with it).
    Settings::instance().setValue(kLegacyAskedKey, true);
    Settings::instance().setValue(kModelsFolderKey, legacy);
    return reportFolderMove(this, runtime::settleLegacyModels(&m_jobs, &m_models, &m_servers, legacy, moveThem));
}

ai::Provider *Studio::providerForServer(const QString &profileId) const
{
    for (ai::Provider *p : m_registry.providers()) {
        if (!profileId.isEmpty() && p->text(QStringLiteral("managed_server")) == profileId)
            return p;
    }
    return nullptr;
}

ai::ModelDefaults modelDefaultsFor(const runtime::ModelLibrary *library, const QString &file)
{
    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(file));
    const std::optional<runtime::ModelInfo> info = path.isEmpty() ? std::nullopt : runtime::modelInfoOf(library, path);
    const runtime::ModelPreset *p = info ? runtime::presetFor(*info, path) : nullptr;
    ai::ModelDefaults d;
    if (!p)
        return d;
    d.id = p->id;
    d.name = p->name;
    d.cfg = p->cfg;
    d.guidance = p->guidance;
    d.imageCfg = p->imageCfg;
    d.flowShift = p->flowShift;
    d.steps = p->steps;
    d.sampler = p->sampler;
    d.scheduler = p->scheduler;
    d.dialect = p->dialect;
    d.nativeResolution = p->nativeResolution;
    d.generate = p->generate;
    d.edit = p->edit;
    d.references = p->references;
    d.negativePrompt = p->negativePrompt;
    return d;
}

void Studio::applyServerBindings()
{
    QList<QPair<QString, QVariantMap>> urlUpdates;
    for (ai::Provider *p : m_registry.providers()) {
        const QString profileId = p->text(QStringLiteral("managed_server"));
        const auto profile = profileId.isEmpty() ? std::nullopt : m_servers.profile(profileId);
        if (!profile) {
            p->setGate(nullptr);
            continue;
        }
        p->setGate(std::make_shared<ServerGate>(&m_servers, profileId));
        const QString kind = m_servers.kindOf(profileId);
        if (kind.isEmpty())
            continue;
        // What the provider must know about the server: its URL and, for stable-diffusion.cpp,
        // which optional models it was started with.
        QVariantMap wanted{{QStringLiteral("url"), runtime::providerUrl(kind, profile->port)}};
        if (kind == QLatin1String("sd.cpp")) {
            auto has = [&](const QString &key) { return !profile->option(key, kind).toString().trimmed().isEmpty(); };
            wanted.insert(QStringLiteral("control_net"), has(QStringLiteral("control_net")));
            wanted.insert(QStringLiteral("ip_adapter"), has(QStringLiteral("ip_adapter")) && has(QStringLiteral("clip_vision")));
            wanted.insert(QStringLiteral("upscaler"), has(QStringLiteral("upscalers_dir")));
            // The checkpoint's file name, so the prompt style matches it (Illustrious → tags).
            QString model = profile->option(QStringLiteral("model"), kind).toString().trimmed();
            if (model.isEmpty())
                model = profile->option(QStringLiteral("diffusion_model"), kind).toString().trimmed();
            wanted.insert(QStringLiteral("model"), QFileInfo(QDir::fromNativeSeparators(model)).fileName());
            // Its family from the header, when the name does not tell (Anima models have any name).
            const auto entry = m_models.entry(QDir::cleanPath(QDir::fromNativeSeparators(model)));
            wanted.insert(QStringLiteral("model_family"), entry ? entry->info.family : QString());
            // Its preset: sampling defaults and what it can do (instruction edits, references).
            wanted.insert(QStringLiteral("model_preset"), modelDefaultsFor(&m_models, model).toVariant());
        }
        QVariantMap settings = m_registry.settingsFor(p->instanceId());
        if (kind == QLatin1String("llama.cpp")) {
            // Router mode needs a model name in every request: keep the provider's default valid.
            QStringList names;
            for (const runtime::LlamaModel &m : runtime::llamaModels(*profile))
                names << m.name;
            if (!names.isEmpty() && !names.contains(settings.value(QStringLiteral("model")).toString()))
                wanted.insert(QStringLiteral("model"), names.first());
        }
        bool changed = false;
        for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
            if (settings.value(it.key()) != it.value()) {
                settings.insert(it.key(), it.value());
                changed = true;
            }
        }
        if (changed)
            urlUpdates.append({p->instanceId(), settings});
    }
    // Updating re-creates provider instances (and re-enters here), so do it after the loop.
    for (const auto &[id, settings] : std::as_const(urlUpdates)) {
        if (ai::Provider *p = m_registry.provider(id))
            m_registry.updateProvider(id, p->displayName(), settings);
    }
}

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
        // Close documents of the previous project (its session keeps them for the next time).
        m_switching = true;
        m_sessionSave.stop();
        const auto docs = m_documents;
        for (doc::Document *d : docs)
            closeDocument(d);
        m_switching = false;
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
    bool newIds = false;
    for (doc::Document *d : std::as_const(m_documents)) {
        if (d->isModified() && !m_project->isSaving(d->id())) {
            newIds = newIds || d->id().isEmpty();
            m_project->saveDocument(d);
        }
    }
    if (newIds)
        scheduleSessionSave(); // documents saved for the first time join the session
}

void Studio::scheduleSessionSave()
{
    if (!m_restoring && !m_switching)
        m_sessionSave.start();
}

QFuture<void> Studio::saveSession()
{
    if (!m_project || !m_project->isOpen() || m_restoring || m_switching)
        return QtFuture::makeReadyVoidFuture();
    QJsonArray ids;
    for (doc::Document *d : std::as_const(m_documents)) {
        if (!d->id().isEmpty())
            ids.append(d->id());
    }
    doc::Record record;
    record.id = QStringLiteral("session");
    record.type = QStringLiteral("session");
    record.name = tr("Open documents");
    record.json = QJsonObject{{QStringLiteral("documents"), ids},
                              {QStringLiteral("active"), m_active ? m_active->id() : QString()}};
    return m_project->saveRecord(record).then([](doc::Record) {});
}

namespace {

QStringList cleanCast(const QStringList &ids, const doc::CharacterLibrary &library)
{
    QStringList out;
    for (const QString &id : ids) {
        if (!id.isEmpty() && !out.contains(id) && library.entry(id) && out.size() < Studio::kMaxCast)
            out << id;
    }
    return out;
}

} // namespace

void Studio::setActiveCast(const QStringList &ids)
{
    const QStringList cast = cleanCast(ids, m_characters);
    if (cast == m_cast)
        return;
    m_cast = cast;
    if (m_project && m_project->isOpen()) {
        doc::Record record;
        record.id = QStringLiteral("active_cast");
        record.type = QStringLiteral("active_cast");
        record.name = tr("Cast");
        record.json = QJsonObject{{QStringLiteral("characters"), QJsonArray::fromStringList(m_cast)}};
        m_project->saveRecord(record);
        m_project->removeRecord(QStringLiteral("active_character"), QStringLiteral("active_character")); // before M16
    }
    emit activeCharacterChanged();
}

void Studio::setActiveCharacter(const QString &id)
{
    setActiveCast(id.isEmpty() ? QStringList() : QStringList{id});
}

void Studio::restoreActiveCharacter()
{
    QPointer<doc::Project> project = m_project;
    if (!project)
        return;
    auto adopt = [this, project](const QStringList &ids) {
        const QStringList cast = cleanCast(ids, m_characters);
        if (project == m_project && cast != m_cast) {
            m_cast = cast;
            emit activeCharacterChanged();
        }
    };
    project->records(QStringLiteral("active_cast")).then(this, [this, project, adopt](QList<doc::Record> records) {
        if (project != m_project)
            return;
        if (!records.isEmpty()) {
            QStringList ids;
            for (const QJsonValue &v : records.first().json.value(QStringLiteral("characters")).toArray())
                ids << v.toString();
            adopt(ids);
            return;
        }
        // A project from before M16: its one active character.
        project->records(QStringLiteral("active_character")).then(this, [project, adopt](QList<doc::Record> old) {
            if (!old.isEmpty())
                adopt({old.first().json.value(QStringLiteral("character")).toString()});
        });
    });
}

QFuture<ai::CharacterContext> Studio::characterContext(const QString &id)
{
    const auto entry = id.isEmpty() ? std::nullopt : m_characters.entry(id);
    if (!entry)
        return QtFuture::makeReadyValueFuture(ai::CharacterContext{});
    ai::CharacterContext c;
    c.id = entry->id;
    c.name = entry->name;
    c.sheet = entry->sheet;
    c.lora = ai::CharacterLora::fromJson(entry->lora);
    return m_characters.loadReferences(id).then([c](const QList<QImage> &refs) mutable {
        c.references = refs;
        return c;
    });
}

void Studio::restoreSession()
{
    if (!m_project || !m_project->isOpen())
        return;
    QPointer<doc::Project> project = m_project;
    m_project->records(QStringLiteral("session")).then(this, [this, project](QList<doc::Record> records) {
        if (project != m_project || records.isEmpty())
            return;
        const QJsonObject json = records.first().json;
        QStringList ids;
        for (const QJsonValue &v : json.value(QStringLiteral("documents")).toArray()) {
            const QString id = v.toString();
            bool known = false;
            for (const doc::DocumentInfo &info : project->documents())
                known = known || info.id == id;
            bool open = false;
            for (doc::Document *d : std::as_const(m_documents))
                open = open || d->id() == id;
            if (known && !open)
                ids << id;
        }
        if (ids.isEmpty())
            return;
        const QString active = json.value(QStringLiteral("active")).toString();
        m_restoring = true;
        auto remaining = std::make_shared<int>(int(ids.size()));
        auto done = [this, remaining, active] {
            if (--*remaining > 0)
                return;
            m_restoring = false;
            for (doc::Document *d : std::as_const(m_documents)) {
                if (d->id() == active)
                    setActiveDocument(d);
            }
            emit sessionRestored();
        };
        for (const QString &id : ids) {
            project->loadDocument(id)
                .then(this, [this, project, done](std::shared_ptr<doc::LoadedDocument> loaded) {
                    if (project == m_project)
                        adoptLoaded(loaded);
                    done();
                })
                .onFailed(this, [this, done](const std::exception &e) {
                    qCWarning(lcApp) << "Could not restore a document:" << describeException(e);
                    done();
                });
        }
    });
}

doc::Document *Studio::adoptLoaded(const std::shared_ptr<doc::LoadedDocument> &loaded)
{
    auto *doc = new doc::Document(loaded->size);
    doc->setId(loaded->id);
    doc->setName(loaded->name);
    doc->replaceContentRaw(loaded->size, loaded->layers, loaded->selection);
    doc->setActiveLayer(loaded->active);
    doc->setSettings(loaded->settings);
    if (loaded->pose)
        doc->setControlPose(loaded->pose);
    doc->undoStack()->clear();
    // The file it saves to, and whether it had changes that are not in the file.
    if (!loaded->file.isEmpty()) {
        doc->setFile(loaded->file);
        doc->setFileModified(loaded->fileModified);
    }
    doc->setModified(false);
    adoptDocument(doc);
    return doc;
}

void Studio::adoptDocument(doc::Document *doc)
{
    // Save shortly after the user stops changing the document.
    connect(doc->undoStack(), &QUndoStack::indexChanged, &m_idleSave, qOverload<>(&QTimer::start));
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
    showPage(pages::Editor);
    return doc;
}

QString Studio::openFileFilter()
{
    // GIF opens (its first frame); saving one goes to Save as…, since Lumen does not write GIF.
    return tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff *.psd);;Layered Photoshop (*.psd);;All files (*)");
}

doc::Document *Studio::documentForFile(const QString &path) const
{
    for (doc::Document *d : m_documents) {
        if (paths::sameFile(d->file().path, path))
            return d;
    }
    return nullptr;
}

void Studio::openFile(const QString &file)
{
    const QString path = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
    // Open already: shown, not opened twice.
    if (doc::Document *open = documentForFile(path)) {
        Settings::instance().addRecentFile(path);
        setActiveDocument(open);
        showPage(pages::Editor);
        return;
    }
    // The project keeps this picture as a document with its layers, as it was saved to the file or
    // read from it, and no other program has changed the file since: that document, rather than a
    // second one with the layers merged.
    if (m_project && m_project->isOpen()) {
        for (const doc::DocumentInfo &info : m_project->documents()) {
            if (paths::sameFile(info.file.path, path) && !info.fileModified && info.file.isUnchangedOnDisk()) {
                Settings::instance().addRecentFile(path);
                openStoredDocument(info.id);
                return;
            }
        }
    }
    // Gone since it was listed (File → Open recent, the start screen): said, and off the list.
    if (!QFileInfo::exists(path)) {
        notify(tr("%1 is not there any more.").arg(QDir::toNativeSeparators(path)), QStringLiteral("warning"));
        Settings::instance().removeRecentFile(path);
        return;
    }
    if (QFileInfo(path).suffix().compare(QLatin1String("psd"), Qt::CaseInsensitive) == 0) {
        openPsdAsDocument(path);
        return;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QByteArray format = reader.format().toLower();
    QImage image = reader.read();
    if (image.isNull()) {
        notify(tr("Cannot open %1: %2").arg(QFileInfo(path).fileName(), reader.errorString()), QStringLiteral("error"));
        return;
    }
    if (format == "jpg")
        format = "jpeg";
    else if (format == "tif")
        format = "tiff";
    doc::FileLink link;
    link.path = path;
    link.format = format;
    if (format == "jpeg")
        link.quality = doc::estimateJpegQuality(path);
    // The document keeps 8 bits per colour.
    if (image.depth() > 32 || image.format() == QImage::Format_Grayscale16)
        link.losses << tr("The file has 16 bits per colour; Lumen saves 8, so the finest shades are lost.");
    // A picture in colours of its own (Display P3 from a phone, Adobe RGB) is turned into standard
    // colours (sRGB), which Lumen shows and saves: it looks as it did, only colours more vivid than
    // sRGB can show are lost, and the file is saved without its profile.
    const QColorSpace space = image.colorSpace();
    if (space.isValid() && space.colorModel() == QColorSpace::ColorModel::Rgb && space.primaries() != QColorSpace::Primaries::SRgb) {
        const QImage standard = image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), QImage::Format_ARGB32);
        if (!standard.isNull())
            image = standard;
        const QString profile = space.description().isEmpty() ? tr("a colour profile of its own") : space.description();
        link.losses << tr("The file has %1; Lumen saves it in standard colours (sRGB), so the most vivid colours may be a little duller.")
                           .arg(profile);
    }
    if (doc::hasCameraDetails(path))
        link.losses << tr("The photo's details from the camera (date, place, camera) are not kept.");
    link.stamp();
    doc::Document *doc = openImageAsDocument(image, QFileInfo(path).completeBaseName());
    doc->setFile(link);
    Settings::instance().addRecentFile(path);
}

void Studio::openPsdAsDocument(const QString &file)
{
    const QString path = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
    const QString name = QFileInfo(path).completeBaseName();
    notify(tr("Opening %1...").arg(QFileInfo(path).fileName()));
    async::run([path] { return doc::importPsd(path); })
        .then(this, [this, name, path](doc::PsdImport imported) {
            doc::FileLink link;
            link.path = path;
            link.format = "psd";
            // What Lumen made do with (clipping, masks, blend modes) is lost when it writes the file.
            if (!imported.warnings.isEmpty())
                link.losses << tr("Lumen could not keep everything of this Photoshop file (%1).")
                                   .arg(imported.warnings.join(QStringLiteral("; ")));
            link.stamp();
            Settings::instance().addRecentFile(path);
            if (imported.layers.isEmpty()) {
                openImageAsDocument(imported.composite, name)->setFile(link);
                return;
            }
            auto *d = new doc::Document(imported.size);
            d->setName(name);
            for (int i = 0; i < imported.layers.size(); ++i)
                d->insertLayerRaw(i, imported.layers[i]);
            d->setActiveLayer(imported.layers.last()->id);
            d->undoStack()->clear();
            d->setFile(link);
            d->setModified(false);
            adoptDocument(d);
            showPage(pages::Editor);
            if (!imported.warnings.isEmpty())
                notify(tr("Opened %1 (%2).").arg(name, imported.warnings.join(QStringLiteral("; "))), QStringLiteral("warning"));
        })
        .onFailed(this, [this](const std::exception &e) { notify(describeException(e), QStringLiteral("error")); });
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

void Studio::openStoredDocument(const QString &documentId, bool show)
{
    for (doc::Document *d : std::as_const(m_documents)) {
        if (d->id() == documentId) {
            setActiveDocument(d);
            if (show)
                showPage(pages::Editor);
            return;
        }
    }
    if (!m_project)
        return;
    m_project->loadDocument(documentId)
        .then(this, [this, show](std::shared_ptr<doc::LoadedDocument> loaded) {
            adoptLoaded(loaded);
            if (show)
                showPage(pages::Editor);
        })
        .onFailed(this, [this](const std::exception &e) { notify(describeException(e), QStringLiteral("error")); });
}

QFuture<void> Studio::closeDocument(doc::Document *doc)
{
    if (!doc || !m_documents.contains(doc))
        return QtFuture::makeReadyVoidFuture();
    emit documentClosing(doc);
    QFuture<void> saved = QtFuture::makeReadyVoidFuture();
    if (doc->isModified() && m_project && m_project->isOpen()) {
        // Persist before closing; deletion waits for the save to finish.
        QPointer<doc::Document> guard(doc);
        saved = m_project->saveDocument(doc).then(this, [guard] {
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
    return saved;
}

bool Studio::hasPage(const QString &id) const
{
    return id == pages::Generate || id == pages::Editor || id == pages::Library || ext::Extensions::instance().hasPage(id);
}

} // namespace lumen::ui
