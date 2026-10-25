#include "runtime/ModelLibrary.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "net/HttpClient.h"
#include "runtime/Download.h"
#include "runtime/ModelHub.h"
#include "runtime/ModelsFolder.h"
#include "runtime/RuntimeStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QPromise>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ModelLibrary)
};

bool hasModelFiles(const QString &dir, int depth = 2)
{
    QDirIterator it(dir, modelFileFilters(), QDir::Files, QDirIterator::Subdirectories);
    const int baseDepth = QDir::cleanPath(dir).count(QLatin1Char('/'));
    while (it.hasNext()) {
        const QString path = it.next();
        if (QDir::cleanPath(path).count(QLatin1Char('/')) - baseDepth > depth + 1)
            continue;
        if (QFileInfo(path).size() > 10ll * 1024 * 1024)
            return true;
    }
    return false;
}

// Header info of a new or changed file, plus what its sidecar says about where it came from.
ModelEntry inspectEntry(const QString &path, const QFileInfo &fi)
{
    ModelEntry e;
    e.path = path;
    e.size = fi.size();
    e.modified = fi.lastModified();
    e.info = inspectModel(path);
    e.source = readSidecar(path);
    if (!e.source.isEmpty()) {
        // The checksum was verified when the file was downloaded.
        if (qint64(e.source.value(QStringLiteral("size")).toDouble(-1)) == e.size)
            e.sha256 = e.source.value(QStringLiteral("sha256")).toString().toLower();
        if (e.source.value(QStringLiteral("source")).toString() == QLatin1String("civitai"))
            e.civitai = e.source.value(QStringLiteral("civitai")).toObject();
        if (e.info.triggerWords.isEmpty()) {
            for (const QJsonValue &w : e.source.value(QStringLiteral("trainedWords")).toArray())
                e.info.triggerWords << w.toString();
        }
    }
    return e;
}

} // namespace

QString sidecarPath(const QString &modelPath)
{
    return modelPath + QStringLiteral(".lumen.json");
}

QJsonObject readSidecar(const QString &modelPath)
{
    QFile f(sidecarPath(modelPath));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QString ModelEntry::name() const
{
    return QFileInfo(path).completeBaseName();
}

QJsonObject modelInfoToJson(const ModelInfo &i)
{
    return QJsonObject{{QStringLiteral("kind"), modelKindKey(i.kind)},
                       {QStringLiteral("format"), i.format},
                       {QStringLiteral("family"), i.family},
                       {QStringLiteral("variant"), i.variant},
                       {QStringLiteral("precision"), i.precision},
                       {QStringLiteral("title"), i.title},
                       {QStringLiteral("parameters"), double(i.parameters)},
                       {QStringLiteral("context"), i.contextLength},
                       {QStringLiteral("layers"), i.layers},
                       {QStringLiteral("kvHeads"), i.kvHeads},
                       {QStringLiteral("headDim"), i.headDim},
                       {QStringLiteral("triggers"), QJsonArray::fromStringList(i.triggerWords)},
                       {QStringLiteral("unsafe"), i.unsafeFormat},
                       {QStringLiteral("error"), i.error},
                       {QStringLiteral("inspector"), i.inspector}};
}

ModelInfo modelInfoFromJson(const QJsonObject &o)
{
    ModelInfo i;
    i.kind = modelKindFromKey(o.value(QStringLiteral("kind")).toString());
    i.format = o.value(QStringLiteral("format")).toString();
    i.inspector = o.value(QStringLiteral("inspector")).toInt();
    i.family = o.value(QStringLiteral("family")).toString();
    i.variant = o.value(QStringLiteral("variant")).toString();
    i.precision = o.value(QStringLiteral("precision")).toString();
    i.title = o.value(QStringLiteral("title")).toString();
    i.parameters = qint64(o.value(QStringLiteral("parameters")).toDouble());
    i.contextLength = o.value(QStringLiteral("context")).toInt();
    i.layers = o.value(QStringLiteral("layers")).toInt();
    i.kvHeads = o.value(QStringLiteral("kvHeads")).toInt();
    i.headDim = o.value(QStringLiteral("headDim")).toInt();
    for (const QJsonValue &v : o.value(QStringLiteral("triggers")).toArray())
        i.triggerWords << v.toString();
    i.unsafeFormat = o.value(QStringLiteral("unsafe")).toBool();
    i.error = o.value(QStringLiteral("error")).toString();
    return i;
}

ModelLibrary::ModelLibrary(net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_dataDir(paths::appData())
    , m_standardFolder(paths::defaultModels())
{
}

void ModelLibrary::setDataDir(const QString &dir)
{
    m_dataDir = dir;
    m_standardFolder = QDir(dir).filePath(QStringLiteral("models"));
}

QString ModelLibrary::indexPath() const
{
    return QDir(m_dataDir).filePath(QStringLiteral("models.json"));
}

QString ModelLibrary::defaultFolder() const
{
    return QDir::cleanPath(m_folder.isEmpty() ? m_standardFolder : m_folder);
}

QString ModelLibrary::standardFolder() const
{
    return QDir::cleanPath(m_standardFolder);
}

void ModelLibrary::setFolder(const QString &path)
{
    const QString typed = QDir::fromNativeSeparators(path.trimmed());
    QString clean = typed.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(typed).absoluteFilePath());
    // The standard folder is no choice: it stays the standard one when that changes (the portable
    // ZIP on another drive letter).
    if (samePath(clean, standardFolder()))
        clean.clear();
    if (clean == m_folder)
        return;
    const QString before = defaultFolder();
    m_folder = clean;
    if (m_loaded && !samePath(before, defaultFolder())) {
        useOwnFolder();
        save();
        emit changed();
        rescan();
    }
    emit folderChanged();
}

void ModelLibrary::useOwnFolder()
{
    const QString own = defaultFolder();
    for (const QString &sub : subfolders())
        QDir().mkpath(QDir(own).filePath(sub));
    // A folder that was added and is now Lumen's own is not listed twice.
    m_roots.removeIf([&own](const ModelRoot &r) { return r.builtin || samePath(r.path, own); });
    m_roots.prepend({own, Text::tr("Lumen models"), true});
}

void ModelLibrary::relocate(const QString &from, const QString &to)
{
    QList<ModelEntry> moved;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        // Files that stayed (in an added folder, or not moved) keep their entries where they are.
        const QString path = movedPath(it.key(), from, to);
        if (path == it.key()) {
            ++it;
            continue;
        }
        ModelEntry e = it.value();
        e.path = QDir::cleanPath(path);
        moved << e;
        it = m_entries.erase(it);
    }
    for (const ModelEntry &e : std::as_const(moved))
        m_entries.insert(e.path, e);
    // A scan that is still running saw the old paths; it is done again.
    ++m_generation;
    save();
    emit changed();
    emit relocated(QDir::cleanPath(from), QDir::cleanPath(to));
}

QString ModelLibrary::subfolderFor(ModelKind kind)
{
    switch (kind) {
    case ModelKind::Checkpoint: return QStringLiteral("checkpoints");
    case ModelKind::DiffusionModel: return QStringLiteral("diffusion_models");
    case ModelKind::Lora: return QStringLiteral("loras");
    case ModelKind::ControlNet: return QStringLiteral("controlnet");
    case ModelKind::IpAdapter: return QStringLiteral("ipadapter");
    case ModelKind::ClipVision: return QStringLiteral("clip_vision");
    case ModelKind::Vae: return QStringLiteral("vae");
    case ModelKind::TextEncoder: return QStringLiteral("text_encoders");
    case ModelKind::Upscaler: return QStringLiteral("upscale_models");
    case ModelKind::Embedding: return QStringLiteral("embeddings");
    case ModelKind::Llm:
    case ModelKind::Projector: return QStringLiteral("llm");
    case ModelKind::Unknown: break;
    }
    return QStringLiteral("other");
}

QStringList ModelLibrary::subfolders()
{
    QStringList out;
    for (ModelKind kind : {ModelKind::Checkpoint, ModelKind::DiffusionModel, ModelKind::Lora, ModelKind::ControlNet, ModelKind::IpAdapter,
                           ModelKind::ClipVision, ModelKind::Vae, ModelKind::TextEncoder, ModelKind::Upscaler, ModelKind::Embedding, ModelKind::Llm})
        out << subfolderFor(kind);
    return out;
}

void ModelLibrary::load()
{
    m_roots.clear();
    m_entries.clear();
    QFile f(indexPath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        for (const QJsonValue &v : root.value(QStringLiteral("roots")).toArray()) {
            const QJsonObject o = v.toObject();
            m_roots.append({o.value(QStringLiteral("path")).toString(), o.value(QStringLiteral("label")).toString(), false});
        }
        for (const QJsonValue &v : root.value(QStringLiteral("models")).toArray()) {
            const QJsonObject o = v.toObject();
            ModelEntry e;
            e.path = o.value(QStringLiteral("path")).toString();
            e.size = qint64(o.value(QStringLiteral("size")).toDouble());
            e.modified = QDateTime::fromMSecsSinceEpoch(qint64(o.value(QStringLiteral("modified")).toDouble()));
            e.info = modelInfoFromJson(o.value(QStringLiteral("info")).toObject());
            e.sha256 = o.value(QStringLiteral("sha256")).toString();
            e.civitai = o.value(QStringLiteral("civitai")).toObject();
            e.source = o.value(QStringLiteral("source")).toObject();
            if (!e.path.isEmpty())
                m_entries.insert(e.path, e);
        }
    }
    // Lumen's own folder, with the usual sub-folders, is always scanned first.
    useOwnFolder();
    m_loaded = true;
    emit changed();
}

void ModelLibrary::save() const
{
    QJsonArray roots;
    for (const ModelRoot &r : m_roots) {
        if (!r.builtin)
            roots.append(QJsonObject{{QStringLiteral("path"), r.path}, {QStringLiteral("label"), r.label}});
    }
    QJsonArray models;
    for (const ModelEntry &e : m_entries) {
        models.append(QJsonObject{{QStringLiteral("path"), e.path},
                                  {QStringLiteral("size"), double(e.size)},
                                  {QStringLiteral("modified"), double(e.modified.toMSecsSinceEpoch())},
                                  {QStringLiteral("info"), modelInfoToJson(e.info)},
                                  {QStringLiteral("sha256"), e.sha256},
                                  {QStringLiteral("civitai"), e.civitai},
                                  {QStringLiteral("source"), e.source}});
    }
    QDir().mkpath(m_dataDir);
    QSaveFile f(indexPath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("roots"), roots}, {QStringLiteral("models"), models}})
                    .toJson(QJsonDocument::Compact));
        f.commit();
    }
}

bool ModelLibrary::addRoot(const QString &path, const QString &label)
{
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!QFileInfo(clean).isDir())
        return false;
    for (const ModelRoot &r : std::as_const(m_roots)) {
        if (samePath(r.path, clean))
            return false;
    }
    m_roots.append({clean, label.isEmpty() ? QFileInfo(clean).fileName() : label, false});
    save();
    emit changed();
    rescan();
    return true;
}

void ModelLibrary::removeRoot(const QString &path)
{
    for (int i = 0; i < m_roots.size(); ++i) {
        if (!m_roots[i].builtin && samePath(m_roots[i].path, path)) {
            const QString root = m_roots[i].path + QLatin1Char('/');
            m_roots.removeAt(i);
            // Forget its files unless another root still contains them.
            for (auto it = m_entries.begin(); it != m_entries.end();) {
                bool covered = false;
                for (const ModelRoot &r : std::as_const(m_roots))
                    covered |= it.key().startsWith(r.path + QLatin1Char('/'), Qt::CaseInsensitive);
                if (it.key().startsWith(root, Qt::CaseInsensitive) && !covered)
                    it = m_entries.erase(it);
                else
                    ++it;
            }
            save();
            emit changed();
            return;
        }
    }
}

QStringList ModelLibrary::suggestedRoots(const RuntimeStore *runtimes, const QStringList &usedFiles) const
{
    QStringList candidates;
    auto add = [&candidates](const QString &dir) {
        if (!dir.isEmpty() && QFileInfo(dir).isDir())
            candidates << QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    };
    if (runtimes) {
        for (const InstalledRuntime &rt : runtimes->runtimes()) {
            if (rt.kind == QLatin1String("comfyui")) {
                const QString base = rt.settings.value(QStringLiteral("baseDirectory")).toString();
                add(QDir(base.isEmpty() ? rt.directory : base).filePath(QStringLiteral("models")));
            } else if (rt.kind == QLatin1String("sdwebui")) {
                add(QDir(rt.directory).filePath(QStringLiteral("models")));
            }
        }
    }
    // ComfyUI Desktop, even when it was not added as a server.
    QFile desktop(QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("ComfyUI/config.json")));
    if (!qEnvironmentVariable("APPDATA").isEmpty() && desktop.open(QIODevice::ReadOnly)) {
        const QString base = QJsonDocument::fromJson(desktop.readAll()).object().value(QStringLiteral("basePath")).toString();
        if (!base.isEmpty())
            add(QDir(base).filePath(QStringLiteral("models")));
    }
    const QString home = QDir::homePath();
    add(QDir(home).filePath(QStringLiteral(".lmstudio/models")));
    add(QDir(home).filePath(QStringLiteral(".cache/lm-studio/models")));
    add(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    for (const QString &file : usedFiles) {
        if (!file.isEmpty())
            add(QFileInfo(file).absolutePath());
    }
    QStringList out;
    for (const QString &c : std::as_const(candidates)) {
        bool covered = false;
        for (const ModelRoot &r : m_roots)
            covered |= samePath(r.path, c) || c.startsWith(r.path + QLatin1Char('/'), Qt::CaseInsensitive);
        bool dup = false;
        for (const QString &o : std::as_const(out))
            dup |= samePath(o, c);
        if (!covered && !dup && hasModelFiles(c))
            out << c;
    }
    return out;
}

void ModelLibrary::beginMove()
{
    m_moving = true;
    ++m_generation; // a scan that runs now would see half of the files
}

void ModelLibrary::endMove()
{
    m_moving = false;
    if (std::exchange(m_rescanPending, false))
        rescan();
}

void ModelLibrary::rescan()
{
    if (m_scanning || m_moving) {
        m_rescanPending = true;
        return;
    }
    m_scanning = true;
    QStringList roots;
    for (const ModelRoot &r : std::as_const(m_roots))
        roots << r.path;
    const QHash<QString, ModelEntry> known = m_entries;
    const int generation = m_generation;
    QPointer<ModelLibrary> self(this);
    async::run([roots, known]() {
        QHash<QString, ModelEntry> found;
        for (const QString &root : roots) {
            QDirIterator it(root, modelFileFilters(), QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
            while (it.hasNext()) {
                const QString path = QDir::cleanPath(it.next());
                if (found.contains(path))
                    continue;
                const QFileInfo fi(path);
                if (fi.size() < 1024 * 1024)
                    continue; // tokenizer files, configs, …
                ModelEntry e;
                e.path = path;
                e.size = fi.size();
                e.modified = fi.lastModified();
                const auto old = known.constFind(path);
                if (old != known.cend() && old->size == e.size && old->modified == e.modified
                    && old->info.inspector >= kModelInspectorVersion) {
                    e = *old; // unchanged: keep header info, hash and lookups
                } else if (old != known.cend() && old->size == e.size && old->modified == e.modified) {
                    // Read by an older inspector: the header again, the hash and lookups stay.
                    e = inspectEntry(path, fi);
                    if (e.sha256.isEmpty())
                        e.sha256 = old->sha256;
                    if (e.civitai.isEmpty())
                        e.civitai = old->civitai;
                } else {
                    e = inspectEntry(path, fi);
                }
                found.insert(path, e);
            }
        }
        return found;
    }).then(this, [self, generation](QHash<QString, ModelEntry> found) {
        if (!self)
            return;
        self->m_scanning = false;
        // The folder moved while this scan ran: its result would drop the entries (checksums,
        // lookups) of files it did not reach. Scanned again, from the moved entries.
        if (generation != self->m_generation) {
            self->m_rescanPending = false;
            self->rescan();
            return;
        }
        self->m_entries = found;
        self->save();
        emit self->changed();
        emit self->scanFinished();
        if (std::exchange(self->m_rescanPending, false))
            self->rescan();
    });
}

QList<ModelEntry> ModelLibrary::entries() const
{
    QList<ModelEntry> out = m_entries.values();
    std::sort(out.begin(), out.end(), [](const ModelEntry &a, const ModelEntry &b) { return a.name().compare(b.name(), Qt::CaseInsensitive) < 0; });
    return out;
}

QList<ModelEntry> ModelLibrary::entries(ModelKind kind) const
{
    QList<ModelEntry> out;
    for (const ModelEntry &e : entries()) {
        if (e.info.kind == kind)
            out << e;
    }
    return out;
}

std::optional<ModelEntry> ModelLibrary::find(const QString &sha256, const QString &fileName, qint64 size) const
{
    const QString digest = sha256.toLower();
    std::optional<ModelEntry> byName;
    for (const ModelEntry &e : m_entries) {
        if (!digest.isEmpty() && e.sha256.compare(digest, Qt::CaseInsensitive) == 0)
            return e;
        if (!byName && !fileName.isEmpty() && size >= 0 && e.size == size
            && QFileInfo(e.path).fileName().compare(fileName, Qt::CaseInsensitive) == 0)
            byName = e;
    }
    return byName;
}

QFuture<ModelEntry> ModelLibrary::addFile(const QString &path)
{
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QPointer<ModelLibrary> self(this);
    return async::run([clean] { return inspectEntry(clean, QFileInfo(clean)); }).then(this, [self](ModelEntry e) {
        if (self && QFileInfo::exists(e.path)) {
            self->m_entries.insert(e.path, e);
            self->save();
            emit self->changed();
        }
        return e;
    });
}

std::optional<ModelEntry> ModelLibrary::entry(const QString &path) const
{
    const auto it = m_entries.constFind(QDir::cleanPath(path));
    if (it != m_entries.cend())
        return *it;
    return std::nullopt;
}

QFuture<QString> ModelLibrary::computeHash(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    const auto e = entry(clean);
    if (e && !e->sha256.isEmpty())
        return QtFuture::makeReadyValueFuture(e->sha256);
    QPointer<ModelLibrary> self(this);
    return async::run([clean] { return sha256OfFile(clean); }).then(this, [self, clean](QString digest) {
        if (digest.isEmpty())
            throw Error(Text::tr("Cannot read %1.").arg(QDir::toNativeSeparators(clean)));
        if (self && self->m_entries.contains(clean)) {
            self->m_entries[clean].sha256 = digest;
            self->save();
            emit self->changed();
        }
        return digest;
    });
}

QFuture<QJsonObject> ModelLibrary::lookupCivitai(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    QPointer<ModelLibrary> self(this);
    return computeHash(clean)
        .then(this, [self, clean](const QString &digest) {
            auto promise = std::make_shared<QPromise<QJsonObject>>();
            promise->start();
            QFuture<QJsonObject> future = promise->future();
            if (!self) {
                promise->addResult({});
                promise->finish();
                return future;
            }
            net::CallOptions o;
            o.maxRetries = 1;
            o.timeoutMs = 20000;
            o.quiet = true;
            const QUrl url(self->m_civitaiBase + QStringLiteral("/api/v1/model-versions/by-hash/") + digest.toUpper());
            net::HttpCall *call = self->m_http->get(net::HttpClient::makeRequest(url, {{"User-Agent", "LumenStudio"}}), o);
            QObject::connect(call, &net::HttpCall::finished, self, [self, call, promise, clean, digest] {
                QJsonObject result;
                if (call->status() == 404) {
                    result.insert(QStringLiteral("notFound"), true);
                } else if (call->ok()) {
                    const QJsonObject v = call->json();
                    const QJsonObject model = v.value(QStringLiteral("model")).toObject();
                    const qint64 modelId = qint64(v.value(QStringLiteral("modelId")).toDouble());
                    const qint64 versionId = qint64(v.value(QStringLiteral("id")).toDouble());
                    QString preview;
                    for (const QJsonValue &img : v.value(QStringLiteral("images")).toArray()) {
                        const QJsonObject io = img.toObject();
                        // The first image that is safe for work, if any.
                        if (io.value(QStringLiteral("nsfwLevel")).toInt(1) <= 1 && io.value(QStringLiteral("type")).toString(QStringLiteral("image")) == QLatin1String("image")) {
                            preview = io.value(QStringLiteral("url")).toString();
                            break;
                        }
                    }
                    result = QJsonObject{{QStringLiteral("modelId"), double(modelId)},
                                         {QStringLiteral("versionId"), double(versionId)},
                                         {QStringLiteral("name"), model.value(QStringLiteral("name")).toString()},
                                         {QStringLiteral("version"), v.value(QStringLiteral("name")).toString()},
                                         {QStringLiteral("type"), model.value(QStringLiteral("type")).toString()},
                                         {QStringLiteral("nsfw"), model.value(QStringLiteral("nsfw")).toBool()},
                                         {QStringLiteral("baseModel"), v.value(QStringLiteral("baseModel")).toString()},
                                         {QStringLiteral("trainedWords"), v.value(QStringLiteral("trainedWords")).toArray()},
                                         {QStringLiteral("preview"), preview},
                                         {QStringLiteral("page"), QStringLiteral("https://civitai.com/models/%1?modelVersionId=%2").arg(modelId).arg(versionId)}};
                    // Its version's VAE and text encoder. This endpoint does not say which are
                    // required; a mark from an earlier download stays.
                    QList<HubFile> files;
                    HubFile self_;
                    for (const QJsonValue &fv : v.value(QStringLiteral("files")).toArray()) {
                        const HubFile f = civitaiFile(fv.toObject(), model.value(QStringLiteral("type")).toString());
                        if (f.path.isEmpty())
                            continue;
                        files << f;
                        if (f.sha256 == digest.toLower())
                            self_ = f;
                    }
                    if (self_.path.isEmpty()) {
                        self_.path = QFileInfo(clean).fileName();
                        self_.hubType = QStringLiteral("Model");
                    }
                    QSet<QString> required;
                    if (self && self->m_entries.contains(clean)) {
                        for (const QJsonValue &c : self->m_entries[clean].civitai.value(QStringLiteral("companions")).toArray()) {
                            if (c.toObject().value(QStringLiteral("required")).toBool())
                                required.insert(c.toObject().value(QStringLiteral("sha256")).toString());
                        }
                    }
                    QJsonArray companions;
                    for (HubFile c : companionFiles(files, self_)) {
                        c.required = c.required || required.contains(c.sha256);
                        companions.append(companionToJson(c));
                    }
                    if (!companions.isEmpty())
                        result.insert(QStringLiteral("companions"), companions);
                } else {
                    promise->setException(std::make_exception_ptr(Error(Text::tr("Civitai lookup failed: %1").arg(call->errorString()))));
                    promise->finish();
                    return;
                }
                if (self && self->m_entries.contains(clean)) {
                    self->m_entries[clean].civitai = result;
                    self->save();
                    emit self->changed();
                }
                promise->addResult(result);
                promise->finish();
            });
            return future;
        })
        .unwrap();
}

bool ModelLibrary::trash(const QString &path, QString *error)
{
    const QString clean = QDir::cleanPath(path);
    QString why;
    const bool moved = moveToTrash ? moveToTrash(clean, &why) : [&] {
        QFile f(clean);
        const bool ok = f.moveToTrash();
        why = f.errorString();
        return ok;
    }();
    if (!moved) {
        if (error)
            *error = Text::tr("Cannot move %1 to the recycle bin: %2").arg(QDir::toNativeSeparators(clean), why);
        return false;
    }
    QFile::remove(sidecarPath(clean)); // a few bytes of metadata; not worth the recycle bin
    m_entries.remove(clean);
    save();
    emit changed();
    return true;
}

} // namespace lumen::runtime
