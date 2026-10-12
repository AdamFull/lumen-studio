#include "runtime/ModelHub.h"

#include "core/Async.h"
#include "core/Error.h"
#include "core/Job.h"
#include "core/JobQueue.h"
#include "core/Log.h"
#include "core/SecretStore.h"
#include "net/HttpClient.h"
#include "runtime/Download.h"
#include "runtime/Hardware.h"
#include "runtime/ModelLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QPromise>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QUrlQuery>

namespace lumen::runtime {

namespace {

class Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::runtime::ModelHub)
};

const QRegularExpression &splitPattern()
{
    static const QRegularExpression re(QStringLiteral("^(.*)-(\\d{5})-of-(\\d{5})\\.gguf$"), QRegularExpression::CaseInsensitiveOption);
    return re;
}

QByteArray userAgent()
{
    return QStringLiteral("LumenStudio/%1").arg(QCoreApplication::applicationVersion()).toUtf8();
}

struct Fetched
{
    QJsonDocument json;
    QByteArray link; // Link header (Hugging Face pagination)
};

QFuture<Fetched> fetchJson(net::HttpClient *http, const QUrl &url, QObject *context)
{
    auto promise = std::make_shared<QPromise<Fetched>>();
    promise->start();
    QFuture<Fetched> future = promise->future();
    net::CallOptions o;
    o.maxRetries = 1;
    o.timeoutMs = 30000;
    o.quiet = true;
    net::HttpCall *call = http->get(net::HttpClient::makeRequest(url, {{"User-Agent", userAgent()}, {"Accept", "application/json"}}), o);
    QObject::connect(call, &net::HttpCall::finished, context, [call, promise, url] {
        if (!call->ok()) {
            QString message = call->errorString();
            if (call->status() == 404)
                message = Text::tr("not found");
            promise->setException(std::make_exception_ptr(Error(Text::tr("%1: %2").arg(url.host(), message))));
        } else {
            QJsonParseError pe;
            Fetched f{QJsonDocument::fromJson(call->body(), &pe), call->rawHeader("link")};
            if (pe.error != QJsonParseError::NoError)
                promise->setException(std::make_exception_ptr(Error(Text::tr("Unexpected answer from %1.").arg(url.host()))));
            else
                promise->addResult(f);
        }
        promise->finish();
    });
    return future;
}

QString formatOf(const QString &fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext == QLatin1String("gguf"))
        return QStringLiteral("gguf");
    if (ext == QLatin1String("safetensors") || ext == QLatin1String("sft"))
        return QStringLiteral("safetensors");
    if (ext == QLatin1String("ckpt") || ext == QLatin1String("pt") || ext == QLatin1String("pth") || ext == QLatin1String("bin"))
        return QStringLiteral("pickle");
    return QStringLiteral("other");
}

bool isModelFile(const QString &fileName)
{
    return formatOf(fileName) != QLatin1String("other");
}

// A first guess from names; the header decides once the file is here.
ModelKind guessKind(const QString &path, const QString &repo, const QString &pipeline)
{
    const QString p = path.toLower();
    const QString r = repo.toLower();
    const QString all = p + QLatin1Char(' ') + r;
    if (p.endsWith(QLatin1String(".gguf")))
        return QFileInfo(p).fileName().contains(QLatin1String("mmproj")) ? ModelKind::Projector : ModelKind::Llm; // "mmproj-…", "….mmproj-Q8_0"
    if (all.contains(QLatin1String("esrgan")) || all.contains(QLatin1String("upscal")) || all.contains(QLatin1String("swinir")))
        return ModelKind::Upscaler;
    if (p.contains(QLatin1String("image_encoder")) || all.contains(QLatin1String("clip_vision")) || all.contains(QLatin1String("clip-vit")))
        return ModelKind::ClipVision;
    if (all.contains(QLatin1String("ip-adapter")) || all.contains(QLatin1String("ip_adapter")))
        return ModelKind::IpAdapter;
    if (all.contains(QLatin1String("controlnet")) || all.contains(QLatin1String("control_")))
        return ModelKind::ControlNet;
    if (all.contains(QLatin1String("lora")))
        return ModelKind::Lora;
    if (QFileInfo(p).fileName().contains(QLatin1String("vae")))
        return ModelKind::Vae;
    if (pipeline == QLatin1String("text-to-image"))
        return ModelKind::Checkpoint;
    return ModelKind::Unknown;
}

ModelKind civitaiKind(const QString &modelType, const QString &fileType)
{
    if (fileType == QLatin1String("VAE"))
        return ModelKind::Vae;
    if (fileType == QLatin1String("Text Encoder"))
        return ModelKind::TextEncoder;
    const QString t = modelType.toLower();
    if (t == QLatin1String("checkpoint"))
        return ModelKind::Checkpoint;
    if (t == QLatin1String("lora") || t == QLatin1String("locon") || t == QLatin1String("dora") || t == QLatin1String("lycoris"))
        return ModelKind::Lora;
    if (t == QLatin1String("controlnet"))
        return ModelKind::ControlNet;
    if (t == QLatin1String("upscaler"))
        return ModelKind::Upscaler;
    if (t == QLatin1String("vae"))
        return ModelKind::Vae;
    if (t == QLatin1String("textualinversion"))
        return ModelKind::Embedding;
    return ModelKind::Unknown;
}

QString plainText(QString html, int maxLength = 700)
{
    static const QRegularExpression breaks(QStringLiteral("<\\s*(br|/p|/li|/h\\d)\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    static const QRegularExpression spaces(QStringLiteral("[ \\t]+"));
    static const QRegularExpression blank(QStringLiteral("\\n\\s*\\n+"));
    html.replace(breaks, QStringLiteral("\n"));
    html.remove(tags);
    html.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    html.replace(QLatin1String("&lt;"), QLatin1String("<"));
    html.replace(QLatin1String("&gt;"), QLatin1String(">"));
    html.replace(QLatin1String("&quot;"), QLatin1String("\""));
    html.replace(QLatin1String("&#39;"), QLatin1String("'"));
    html.replace(QLatin1String("&amp;"), QLatin1String("&"));
    html.replace(spaces, QStringLiteral(" "));
    html.replace(blank, QStringLiteral("\n\n"));
    html = html.trimmed();
    if (html.size() > maxLength)
        html = html.left(maxLength).trimmed() + QStringLiteral("…");
    return html;
}

QString uniquePath(const QString &path)
{
    if (!QFileInfo::exists(path) && !QFileInfo::exists(FileDownload::partPath(path)))
        return path;
    const QFileInfo fi(path);
    for (int i = 2;; ++i) {
        const QString candidate = fi.dir().filePath(QStringLiteral("%1 (%2).%3").arg(fi.completeBaseName()).arg(i).arg(fi.suffix()));
        if (!QFileInfo::exists(candidate) && !QFileInfo::exists(FileDownload::partPath(candidate)))
            return candidate;
    }
}

bool validRepoId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return re.match(id).hasMatch() && !id.contains(QLatin1String(".."));
}

QString encodePath(const QString &path)
{
    QStringList parts;
    for (const QString &p : path.split(QLatin1Char('/')))
        parts << QString::fromLatin1(QUrl::toPercentEncoding(p));
    return parts.join(QLatin1Char('/'));
}

// ---- Hugging Face ----------------------------------------------------------------------------------

HubModel hfModel(const QJsonObject &o)
{
    HubModel m;
    m.source = HubSource::HuggingFace;
    m.id = o.value(QStringLiteral("id")).toString();
    m.author = m.id.section(QLatin1Char('/'), 0, 0);
    m.name = m.id.section(QLatin1Char('/'), 1);
    m.downloads = qint64(o.value(QStringLiteral("downloads")).toDouble());
    m.likes = qint64(o.value(QStringLiteral("likes")).toDouble());
    m.type = o.value(QStringLiteral("pipeline_tag")).toString();
    const QJsonValue gated = o.value(QStringLiteral("gated"));
    m.gated = gated.isString() ? gated.toString() : (gated.toBool() ? QStringLiteral("auto") : QString());
    for (const QJsonValue &t : o.value(QStringLiteral("tags")).toArray()) {
        const QString tag = t.toString();
        if (tag.startsWith(QLatin1String("license:")))
            m.license = tag.mid(8);
        else if (!tag.contains(QLatin1Char(':')))
            m.tags << tag;
    }
    const QString cardLicense = o.value(QStringLiteral("cardData")).toObject().value(QStringLiteral("license")).toString();
    if (!cardLicense.isEmpty())
        m.license = cardLicense;
    m.updated = QDateTime::fromString(o.value(QStringLiteral("lastModified")).toString(), Qt::ISODateWithMs);
    m.page = QUrl(QStringLiteral("https://huggingface.co/") + m.id);
    return m;
}

QString nextCursor(const QByteArray &link)
{
    static const QRegularExpression re(QStringLiteral("<([^>]+)>\\s*;\\s*rel=\"?next\"?"));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(link));
    if (!m.hasMatch())
        return {};
    return QUrlQuery(QUrl(m.captured(1))).queryItemValue(QStringLiteral("cursor"), QUrl::FullyDecoded);
}

// ---- Civitai ------------------------------------------------------------------------------------------

QUrl civitaiPreview(const QJsonArray &images, bool nsfw)
{
    for (const QJsonValue &v : images) {
        const QJsonObject io = v.toObject();
        if (io.value(QStringLiteral("type")).toString(QStringLiteral("image")) != QLatin1String("image"))
            continue;
        if (!nsfw && io.value(QStringLiteral("nsfwLevel")).toInt(1) > 1)
            continue;
        QString url = io.value(QStringLiteral("url")).toString();
        url.replace(QRegularExpression(QStringLiteral("/original=true/|/width=\\d+/")), QStringLiteral("/width=320/"));
        return QUrl(url);
    }
    return {};
}

HubModel civitaiModel(const QJsonObject &o, bool nsfw)
{
    HubModel m;
    m.source = HubSource::Civitai;
    const qint64 id = qint64(o.value(QStringLiteral("id")).toDouble());
    m.id = QString::number(id);
    m.name = o.value(QStringLiteral("name")).toString();
    m.author = o.value(QStringLiteral("creator")).toObject().value(QStringLiteral("username")).toString();
    m.type = o.value(QStringLiteral("type")).toString();
    m.nsfw = o.value(QStringLiteral("nsfw")).toBool();
    const QJsonObject stats = o.value(QStringLiteral("stats")).toObject();
    m.downloads = qint64(stats.value(QStringLiteral("downloadCount")).toDouble());
    m.likes = qint64(stats.value(QStringLiteral("thumbsUpCount")).toDouble());
    for (const QJsonValue &t : o.value(QStringLiteral("tags")).toArray())
        m.tags << t.toString();
    for (const char *key : {"allowCommercialUse", "allowDerivatives", "allowNoCredit", "allowDifferentLicense"}) {
        const QJsonValue v = o.value(QLatin1String(key));
        if (!v.isUndefined())
            m.permissions.insert(QLatin1String(key), v.toVariant());
    }
    m.page = QUrl(QStringLiteral("https://civitai.com/models/%1").arg(id));
    m.description = plainText(o.value(QStringLiteral("description")).toString());
    for (const QJsonValue &vv : o.value(QStringLiteral("modelVersions")).toArray()) {
        const QJsonObject vo = vv.toObject();
        HubVersion v;
        v.id = QString::number(qint64(vo.value(QStringLiteral("id")).toDouble()));
        v.name = vo.value(QStringLiteral("name")).toString();
        v.baseModel = vo.value(QStringLiteral("baseModel")).toString();
        for (const QJsonValue &w : vo.value(QStringLiteral("trainedWords")).toArray())
            v.trainedWords << w.toString().trimmed();
        v.published = QDateTime::fromString(vo.value(QStringLiteral("publishedAt")).toString(), Qt::ISODateWithMs);
        for (const QJsonValue &fv : vo.value(QStringLiteral("files")).toArray()) {
            const HubFile f = civitaiFile(fv.toObject(), m.type);
            if (!f.path.isEmpty())
                v.files << f;
        }
        if (m.preview.isEmpty())
            m.preview = civitaiPreview(vo.value(QStringLiteral("images")).toArray(), nsfw);
        m.versions << v;
    }
    if (!m.versions.isEmpty())
        m.updated = m.versions.first().published;
    m.detailed = true;
    return m;
}

// ---- download job --------------------------------------------------------------------------------------

QJsonObject sidecarFor(const HubDownload &d, const QString &digest, qint64 size)
{
    QJsonObject o{{QStringLiteral("source"), hubSourceKey(d.source)},
                  {QStringLiteral("model"), d.modelId},
                  {QStringLiteral("name"), d.modelName},
                  {QStringLiteral("version"), d.versionId},
                  {QStringLiteral("versionName"), d.versionName},
                  {QStringLiteral("file"), d.file.path},
                  {QStringLiteral("url"), d.file.url.toString()},
                  {QStringLiteral("page"), d.page.toString()},
                  {QStringLiteral("license"), d.license},
                  {QStringLiteral("baseModel"), d.baseModel},
                  {QStringLiteral("trainedWords"), QJsonArray::fromStringList(d.trainedWords)},
                  {QStringLiteral("nsfw"), d.nsfw},
                  {QStringLiteral("sha256"), digest},
                  {QStringLiteral("size"), double(size)},
                  {QStringLiteral("downloaded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!d.permissions.isEmpty())
        o.insert(QStringLiteral("permissions"), QJsonObject::fromVariantMap(d.permissions));
    if (d.source == HubSource::Civitai) {
        // Same shape as ModelLibrary::lookupCivitai, so the library shows it without a lookup.
        QJsonObject civitai{{QStringLiteral("modelId"), d.modelId.toDouble()},
                            {QStringLiteral("versionId"), d.versionId.toDouble()},
                            {QStringLiteral("name"), d.modelName},
                            {QStringLiteral("version"), d.versionName},
                            {QStringLiteral("nsfw"), d.nsfw},
                            {QStringLiteral("baseModel"), d.baseModel},
                            {QStringLiteral("trainedWords"), QJsonArray::fromStringList(d.trainedWords)},
                            {QStringLiteral("page"), d.page.toString()}};
        if (!d.companions.isEmpty()) {
            QJsonArray companions;
            for (const HubFile &c : d.companions)
                companions.append(companionToJson(c));
            civitai.insert(QStringLiteral("companions"), companions);
        }
        o.insert(QStringLiteral("civitai"), civitai);
    }
    return o;
}

// Drives one download job: existing file check → download (resumable, verified) → header check
// (moves the file to the right sub-folder when the guess was wrong) → sidecar → library.
class Fetcher : public QObject
{
public:
    Fetcher(ModelHub *hub, HubDownload d, Job *job)
        : QObject(job)
        , m_hub(hub)
        , m_d(std::move(d))
        , m_job(job)
    {
    }

    void run()
    {
        if (m_d.file.pickle() && !m_d.allowPickle)
            return m_job->fail(Text::tr("%1 is a pickle file, which can run code when loaded. Lumen only downloads it when you allow it.")
                                   .arg(m_d.file.fileName()));
        const bool local = m_d.file.url.host() == QLatin1String("127.0.0.1") || m_d.file.url.host() == QLatin1String("localhost");
        if (!m_d.file.url.isValid() || (m_d.file.url.scheme() != QLatin1String("https") && !local))
            return m_job->fail(Text::tr("Invalid download address."));
        m_dest = m_hub->destinationFor(m_d);
        QDir().mkpath(QFileInfo(m_dest).absolutePath());
        const QStorageInfo storage(QFileInfo(m_dest).absolutePath());
        const qint64 partial = QFileInfo(FileDownload::partPath(m_dest)).size();
        const qint64 needed = std::max<qint64>(0, m_d.file.size - partial) + 256LL * 1024 * 1024;
        if (m_d.file.size > 0 && storage.isValid() && storage.bytesAvailable() < needed)
            return m_job->fail(Text::tr("Not enough disk space: %1 needed on %2, %3 free.")
                                   .arg(formatBytes(needed), storage.rootPath(), formatBytes(storage.bytesAvailable())));
        if (!QFileInfo::exists(m_dest))
            return startDownload();
        // A file of that name is already there: keep it when it is this file, otherwise pick another name.
        m_job->setProgress(0, Text::tr("Checking the existing %1…").arg(QFileInfo(m_dest).fileName()));
        QPointer<Fetcher> self(this);
        async::run([path = m_dest] { return sha256OfFile(path); }).then(this, [self](const QString &digest) {
            if (!self || self->m_cancelled)
                return;
            if (!digest.isEmpty() && digest == self->m_d.file.sha256)
                return self->place(self->m_dest, digest);
            self->m_dest = uniquePath(self->m_dest);
            self->startDownload();
        });
    }

    void cancel()
    {
        m_cancelled = true;
        if (m_download)
            m_download->cancel();
    }

private:
    void startDownload()
    {
        DownloadSpec spec;
        spec.url = m_d.file.url;
        spec.destination = m_dest;
        spec.sha256 = m_d.file.sha256;
        // Civitai lists sizes in rounded kilobytes; the checksum is what counts there.
        spec.size = m_d.source == HubSource::HuggingFace ? m_d.file.size : -1;
        spec.headers << qMakePair(QByteArrayLiteral("User-Agent"), userAgent());
        const QString token = m_hub->token(m_d.source);
        if (!token.isEmpty())
            spec.headers << qMakePair(QByteArrayLiteral("Authorization"), QByteArray("Bearer ") + token.toUtf8());
        m_download = new FileDownload(m_hub->http()->nam(), spec, this);
        const QString name = QFileInfo(m_dest).fileName();
        m_job->setProgress(0, Text::tr("Downloading %1…").arg(name));
        connect(m_download, &FileDownload::progress, this, [this, name](qint64 received, qint64 total) {
            if (total <= 0)
                total = m_d.file.size;
            const double fraction = total > 0 ? 0.97 * double(received) / double(total) : 0.0;
            m_job->setProgress(fraction, total > 0 ? Text::tr("Downloading %1 — %2 of %3").arg(name, formatBytes(received), formatBytes(total))
                                                   : Text::tr("Downloading %1 — %2").arg(name, formatBytes(received)));
        });
        connect(m_download, &FileDownload::finished, this, [this] {
            FileDownload *d = m_download;
            m_download = nullptr;
            d->deleteLater();
            if (m_cancelled)
                return m_job->reportCancelled();
            if (!m_hub)
                return m_job->fail(Text::tr("Shutting down."));
            if (!d->ok())
                return m_job->fail(describe(d));
            place(d->spec().destination, d->sha256());
        });
        m_download->start();
    }

    QString describe(const FileDownload *d) const
    {
        const int status = d->httpStatus();
        if (status == 401 || status == 403) {
            if (m_d.source == HubSource::Civitai)
                return m_hub->token(HubSource::Civitai).isEmpty()
                           ? Text::tr("Civitai requires an API key for this download. Add one under Settings → Models → Download.")
                           : Text::tr("Civitai refused the download (HTTP %1). Check your API key, or whether the model needs early access.").arg(status);
            return m_hub->token(HubSource::HuggingFace).isEmpty()
                       ? Text::tr("This repository is gated: accept its terms on huggingface.co and add an access token under Settings → Models → Download.")
                       : Text::tr("Hugging Face refused the download (HTTP %1). Accept the repository's terms on huggingface.co with the account of your token.").arg(status);
        }
        return d->errorString();
    }

    // The header decides the sub-folder; then the sidecar and the library entry.
    void place(const QString &path, const QString &digest)
    {
        if (!m_hub)
            return m_job->fail(Text::tr("Shutting down."));
        m_job->setProgress(0.98, Text::tr("Checking %1…").arg(QFileInfo(path).fileName()));
        const QString own = QDir::cleanPath(m_hub->library()->defaultFolder());
        const bool managedFolder = m_d.folder.isEmpty();
        const HubDownload d = m_d;
        QPointer<Fetcher> self(this);
        async::run([path, digest, own, managedFolder, d]() -> QString {
            QString final = path;
            const ModelInfo info = inspectModel(path);
            if (managedFolder && info.kind != ModelKind::Unknown && info.kind != d.file.kind) {
                const QString target = QDir(QDir(own).filePath(ModelLibrary::subfolderFor(info.kind))).filePath(QFileInfo(path).fileName());
                const QString unique = uniquePath(target);
                QDir().mkpath(QFileInfo(unique).absolutePath());
                if (QFile::rename(path, unique))
                    final = unique;
            }
            QSaveFile f(sidecarPath(final));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(sidecarFor(d, digest, QFileInfo(final).size())).toJson());
                f.commit();
            }
            return final;
        }).then(this, [self](const QString &final) {
            if (!self || !self->m_hub)
                return;
            ModelHub *hub = self->m_hub;
            Job *job = self->m_job;
            hub->library()->addFile(final).then(job, [hub, job, final](const ModelEntry &) {
                job->succeed(final);
                emit hub->downloaded(final);
            });
        });
    }

    QPointer<ModelHub> m_hub;
    HubDownload m_d;
    Job *m_job;
    QString m_dest;
    QPointer<FileDownload> m_download;
    bool m_cancelled = false;
};

} // namespace

// ---- free functions ----------------------------------------------------------------------------------

QString hubSourceKey(HubSource s)
{
    return s == HubSource::Civitai ? QStringLiteral("civitai") : QStringLiteral("huggingface");
}

QString hubSourceName(HubSource s)
{
    return s == HubSource::Civitai ? QStringLiteral("Civitai") : QStringLiteral("Hugging Face");
}

QString HubFile::fileName() const
{
    return path.section(QLatin1Char('/'), -1);
}

QString HubDownload::title() const
{
    return saveAs.isEmpty() ? file.fileName() : saveAs;
}

QString StarterFile::fileName() const
{
    return saveAs.isEmpty() ? path.section(QLatin1Char('/'), -1) : saveAs;
}

qint64 StarterSet::totalSize() const
{
    qint64 total = 0;
    for (const StarterFile &f : files)
        total += f.size;
    return total;
}

const StarterFile *StarterSet::file(const QString &role) const
{
    for (const StarterFile &f : files) {
        if (f.role == role)
            return &f;
    }
    return nullptr;
}

QList<StarterSet> starterSets(const QByteArray &json)
{
    QByteArray data = json;
    if (data.isEmpty()) {
        QFile f(QStringLiteral(":/models/starters.json"));
        if (f.open(QIODevice::ReadOnly))
            data = f.readAll();
    }
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    QHash<QString, StarterFile> files;
    for (const QJsonValue &v : root.value(QStringLiteral("files")).toArray()) {
        const QJsonObject o = v.toObject();
        StarterFile f;
        f.id = o.value(QStringLiteral("id")).toString();
        f.role = o.value(QStringLiteral("role")).toString();
        f.kind = modelKindFromKey(o.value(QStringLiteral("kind")).toString());
        f.name = o.value(QStringLiteral("name")).toString();
        f.repo = o.value(QStringLiteral("repo")).toString();
        f.revision = o.value(QStringLiteral("revision")).toString();
        f.path = o.value(QStringLiteral("path")).toString();
        f.saveAs = o.value(QStringLiteral("saveAs")).toString();
        f.size = qint64(o.value(QStringLiteral("size")).toDouble());
        f.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
        f.license = o.value(QStringLiteral("license")).toString();
        if (!f.id.isEmpty())
            files.insert(f.id, f);
    }
    QList<StarterSet> sets;
    for (const QJsonValue &v : root.value(QStringLiteral("sets")).toArray()) {
        const QJsonObject o = v.toObject();
        StarterSet s;
        s.id = o.value(QStringLiteral("id")).toString();
        s.minVramGb = o.value(QStringLiteral("minVramGb")).toDouble();
        for (const QJsonValue &id : o.value(QStringLiteral("files")).toArray()) {
            if (files.contains(id.toString()))
                s.files << files.value(id.toString());
        }
        sets << s;
    }
    std::sort(sets.begin(), sets.end(), [](const StarterSet &a, const StarterSet &b) { return a.minVramGb < b.minVramGb; });
    return sets;
}

const StarterSet *pickStarterSet(const QList<StarterSet> &sets, quint64 vramBytes)
{
    const double gb = double(vramBytes) / (1024.0 * 1024 * 1024);
    const StarterSet *best = sets.isEmpty() ? nullptr : &sets.first();
    for (const StarterSet &s : sets) {
        // Drivers report a little less than the nominal size (8 GB cards: ~7.6–8.0 GiB).
        if (s.minVramGb <= gb + 0.25)
            best = &s;
    }
    return best;
}

HubDownload starterDownload(const StarterFile &file, const QString &huggingFaceBase)
{
    HubDownload d;
    d.source = HubSource::HuggingFace;
    d.modelId = file.repo;
    d.modelName = file.name;
    d.versionId = file.revision;
    d.versionName = file.revision.left(7);
    d.file.path = file.path;
    d.file.size = file.size;
    d.file.sha256 = file.sha256;
    d.file.url = QUrl(huggingFaceBase + QLatin1Char('/') + file.repo + QStringLiteral("/resolve/") + file.revision + QLatin1Char('/') + encodePath(file.path));
    d.file.format = formatOf(file.path);
    d.file.kind = file.kind;
    d.file.precision = precisionFromName(file.path);
    d.saveAs = file.saveAs;
    d.license = file.license;
    d.page = QUrl(QStringLiteral("https://huggingface.co/") + file.repo);
    return d;
}

HubFile civitaiFile(const QJsonObject &fo, const QString &modelType)
{
    HubFile f;
    const QString name = fo.value(QStringLiteral("name")).toString();
    if (!isModelFile(name) || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return f;
    f.path = name;
    f.size = qRound64(fo.value(QStringLiteral("sizeKB")).toDouble() * 1024.0);
    f.sha256 = fo.value(QStringLiteral("hashes")).toObject().value(QStringLiteral("SHA256")).toString().toLower();
    f.url = QUrl(fo.value(QStringLiteral("downloadUrl")).toString());
    const QJsonObject meta = fo.value(QStringLiteral("metadata")).toObject();
    const QString format = meta.value(QStringLiteral("format")).toString();
    f.format = format == QLatin1String("PickleTensor") ? QStringLiteral("pickle") : formatOf(f.path);
    f.precision = meta.value(QStringLiteral("fp")).toString().toUpper();
    if (f.precision.isEmpty())
        f.precision = precisionFromName(f.path);
    f.hubType = fo.value(QStringLiteral("type")).toString();
    f.kind = civitaiKind(modelType, f.hubType);
    f.primary = fo.value(QStringLiteral("primary")).toBool();
    // Only the search results carry it (not /model-versions); absent = not marked.
    f.required = meta.value(QStringLiteral("isRequired")).toBool();
    return f;
}

namespace {

bool isCompanionType(const QString &type)
{
    return type == QLatin1String("VAE") || type == QLatin1String("Text Encoder");
}

bool isModelType(const QString &type)
{
    return type == QLatin1String("Model") || type == QLatin1String("Pruned Model");
}

} // namespace

QList<HubFile> companionFiles(const QList<HubFile> &files, const HubFile &model)
{
    if (model.hubType.isEmpty() || !isModelType(model.hubType))
        return {};
    QList<HubFile> out;
    QSet<QString> seen;
    for (const HubFile &f : files) {
        if (f.path == model.path || !(isCompanionType(f.hubType) || (f.required && !isModelType(f.hubType))))
            continue;
        const QString key = f.sha256.isEmpty() ? f.path : f.sha256;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        out << f;
    }
    return out;
}

QJsonObject companionToJson(const HubFile &f)
{
    return QJsonObject{{QStringLiteral("type"), f.hubType},
                       {QStringLiteral("kind"), modelKindKey(f.kind)},
                       {QStringLiteral("file"), f.path},
                       {QStringLiteral("size"), double(f.size)},
                       {QStringLiteral("sha256"), f.sha256},
                       {QStringLiteral("url"), f.url.toString()},
                       {QStringLiteral("format"), f.format},
                       {QStringLiteral("precision"), f.precision},
                       {QStringLiteral("required"), f.required}};
}

HubFile companionFromJson(const QJsonObject &o)
{
    HubFile f;
    f.hubType = o.value(QStringLiteral("type")).toString();
    f.kind = modelKindFromKey(o.value(QStringLiteral("kind")).toString());
    f.path = o.value(QStringLiteral("file")).toString();
    f.size = qint64(o.value(QStringLiteral("size")).toDouble(-1));
    f.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
    f.url = QUrl(o.value(QStringLiteral("url")).toString());
    f.format = o.value(QStringLiteral("format")).toString(formatOf(f.path));
    f.precision = o.value(QStringLiteral("precision")).toString();
    f.required = o.value(QStringLiteral("required")).toBool();
    return f;
}

QList<ModelCompanion> companionsOf(const ModelLibrary *library, const QString &modelPath)
{
    const std::optional<ModelEntry> e = library->entry(modelPath);
    if (!e)
        return {};
    QList<ModelCompanion> out;
    for (const QJsonValue &v : e->civitai.value(QStringLiteral("companions")).toArray()) {
        ModelCompanion c;
        c.file = companionFromJson(v.toObject());
        if (c.file.path.isEmpty())
            continue;
        if (const auto local = library->find(c.file.sha256, c.file.fileName(), c.file.size))
            c.path = local->path;
        out << c;
    }
    return out;
}

HubDownload companionDownload(const ModelEntry &model, const HubFile &companion)
{
    const QJsonObject c = model.civitai;
    HubDownload d;
    d.source = HubSource::Civitai;
    d.modelId = QString::number(qint64(c.value(QStringLiteral("modelId")).toDouble()));
    d.modelName = c.value(QStringLiteral("name")).toString();
    d.versionId = QString::number(qint64(c.value(QStringLiteral("versionId")).toDouble()));
    d.versionName = c.value(QStringLiteral("version")).toString();
    d.baseModel = c.value(QStringLiteral("baseModel")).toString();
    d.nsfw = c.value(QStringLiteral("nsfw")).toBool();
    d.page = QUrl(c.value(QStringLiteral("page")).toString());
    d.file = companion;
    if (!model.source.isEmpty()) {
        d.license = model.source.value(QStringLiteral("license")).toString();
        d.permissions = model.source.value(QStringLiteral("permissions")).toObject().toVariantMap();
    }
    return d;
}

QVariantMap companionServerOptions(const ModelLibrary *library, const QString &modelPath)
{
    QVariantMap out;
    for (const ModelCompanion &c : companionsOf(library, modelPath)) {
        if (c.missing())
            continue;
        const std::optional<ModelEntry> e = library->entry(c.path);
        const ModelKind kind = e && e->info.kind != ModelKind::Unknown ? e->info.kind : c.file.kind;
        const QString native = QDir::toNativeSeparators(c.path);
        if (kind == ModelKind::Vae)
            out.insert(QStringLiteral("vae"), native);
        else if (kind == ModelKind::TextEncoder && e && e->info.family == QLatin1String("LLM"))
            out.insert(QStringLiteral("llm"), native);
        else if (kind == ModelKind::TextEncoder && e && e->info.family == QLatin1String("T5"))
            out.insert(QStringLiteral("t5xxl"), native);
    }
    return out;
}

bool recordCompanions(const QString &modelPath, const QList<HubFile> &companions)
{
    QJsonObject sidecar = readSidecar(modelPath);
    if (sidecar.isEmpty() || companions.isEmpty())
        return false;
    QJsonObject civitai = sidecar.value(QStringLiteral("civitai")).toObject();
    QJsonArray list;
    for (const HubFile &c : companions)
        list.append(companionToJson(c));
    civitai.insert(QStringLiteral("companions"), list);
    sidecar.insert(QStringLiteral("civitai"), civitai);
    QSaveFile f(sidecarPath(modelPath));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(sidecar).toJson());
    return f.commit();
}

QList<HubFile> splitParts(const QList<HubFile> &files, const HubFile &file)
{
    const QRegularExpressionMatch m = splitPattern().match(file.path);
    if (!m.hasMatch())
        return {file};
    QMap<int, HubFile> parts;
    for (const HubFile &f : files) {
        const QRegularExpressionMatch fm = splitPattern().match(f.path);
        if (fm.hasMatch() && fm.captured(1) == m.captured(1) && fm.captured(3) == m.captured(3))
            parts.insert(fm.captured(2).toInt(), f);
    }
    return parts.values();
}

std::optional<HubFile> projectorFor(const QList<HubFile> &files, const HubFile &model)
{
    if (model.format != QLatin1String("gguf") || model.kind == ModelKind::Projector)
        return std::nullopt;
    QList<HubFile> candidates;
    for (const HubFile &f : files) {
        if (f.format == QLatin1String("gguf") && f.fileName().contains(QLatin1String("mmproj"), Qt::CaseInsensitive)
            && f.path.section(QLatin1Char('/'), 0, -2) == model.path.section(QLatin1Char('/'), 0, -2))
            candidates << f;
    }
    if (candidates.isEmpty()) {
        for (const HubFile &f : files) {
            if (f.format == QLatin1String("gguf") && f.fileName().contains(QLatin1String("mmproj"), Qt::CaseInsensitive))
                candidates << f;
        }
    }
    for (const QString &pref : {QStringLiteral("Q8_0"), QStringLiteral("F16"), QStringLiteral("BF16"), QStringLiteral("F32")}) {
        for (const HubFile &f : std::as_const(candidates)) {
            if (precisionFromName(f.path) == pref)
                return f;
        }
    }
    if (!candidates.isEmpty())
        return candidates.first();
    return std::nullopt;
}

QString precisionFromName(const QString &fileName)
{
    static const QRegularExpression re(QStringLiteral("(?:^|[-_.])((?:I?Q\\d(?:_[A-Z0-9]{1,2}){0,3})|BF16|FP16|FP32|FP8|F16|F32|MXFP4)(?=[-_.]|$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = re.globalMatch(QFileInfo(fileName).completeBaseName());
    QString last;
    while (it.hasNext())
        last = it.next().captured(1).toUpper();
    return last;
}

// ---- ModelHub ------------------------------------------------------------------------------------------

ModelHub::ModelHub(net::HttpClient *http, JobQueue *jobs, ModelLibrary *library, QObject *parent)
    : QObject(parent)
    , m_http(http)
    , m_jobs(jobs)
    , m_library(library)
{
    tokenProvider = [](HubSource s) { return SecretStore::read(tokenKey(s)); };
    m_jobs->setConcurrency(QStringLiteral("model-download"), 2);
}

void ModelHub::setBaseUrls(const QString &huggingFace, const QString &civitai)
{
    m_hfBase = huggingFace;
    m_civitaiBase = civitai;
}

QString ModelHub::baseUrl(HubSource s) const
{
    return s == HubSource::Civitai ? m_civitaiBase : m_hfBase;
}

QString ModelHub::tokenKey(HubSource s)
{
    return QStringLiteral("hub/%1/token").arg(hubSourceKey(s));
}

QString ModelHub::token(HubSource s) const
{
    return tokenProvider ? tokenProvider(s).trimmed() : QString();
}

QFuture<HubPage> ModelHub::search(HubSource source, const HubQuery &query)
{
    QUrlQuery q;
    QUrl url;
    if (source == HubSource::HuggingFace) {
        url = QUrl(m_hfBase + QStringLiteral("/api/models"));
        if (!query.text.trimmed().isEmpty())
            q.addQueryItem(QStringLiteral("search"), query.text.trimmed());
        if (query.category == QLatin1String("llm"))
            q.addQueryItem(QStringLiteral("filter"), QStringLiteral("gguf"));
        else if (query.category == QLatin1String("checkpoint"))
            q.addQueryItem(QStringLiteral("pipeline_tag"), QStringLiteral("text-to-image"));
        else if (query.category == QLatin1String("lora"))
            q.addQueryItem(QStringLiteral("filter"), QStringLiteral("lora"));
        else if (query.category == QLatin1String("controlnet"))
            q.addQueryItem(QStringLiteral("filter"), QStringLiteral("controlnet"));
        q.addQueryItem(QStringLiteral("sort"), QStringLiteral("downloads"));
        q.addQueryItem(QStringLiteral("direction"), QStringLiteral("-1"));
        q.addQueryItem(QStringLiteral("limit"), QString::number(query.limit));
        for (const char *field : {"downloads", "likes", "tags", "pipeline_tag", "gated", "lastModified"})
            q.addQueryItem(QStringLiteral("expand[]"), QLatin1String(field));
    } else {
        if (query.category == QLatin1String("llm"))
            return QtFuture::makeReadyValueFuture(HubPage{}); // Civitai has no language models
        url = QUrl(m_civitaiBase + QStringLiteral("/api/v1/models"));
        q.addQueryItem(QStringLiteral("limit"), QString::number(query.limit));
        q.addQueryItem(QStringLiteral("nsfw"), query.nsfw ? QStringLiteral("true") : QStringLiteral("false"));
        if (!query.text.trimmed().isEmpty())
            q.addQueryItem(QStringLiteral("query"), query.text.trimmed());
        else
            q.addQueryItem(QStringLiteral("sort"), QStringLiteral("Most Downloaded"));
        QStringList types;
        if (query.category == QLatin1String("checkpoint"))
            types << QStringLiteral("Checkpoint");
        else if (query.category == QLatin1String("lora"))
            types << QStringLiteral("LORA") << QStringLiteral("LoCon") << QStringLiteral("DoRA");
        else if (query.category == QLatin1String("controlnet"))
            types << QStringLiteral("Controlnet");
        else if (query.category == QLatin1String("upscaler"))
            types << QStringLiteral("Upscaler");
        for (const QString &t : std::as_const(types))
            q.addQueryItem(QStringLiteral("types"), t);
    }
    if (!query.cursor.isEmpty())
        q.addQueryItem(QStringLiteral("cursor"), query.cursor);
    url.setQuery(q);
    const bool nsfw = query.nsfw;
    return fetchJson(m_http, url, this).then(this, [source, nsfw](const Fetched &f) {
        HubPage page;
        if (source == HubSource::HuggingFace) {
            for (const QJsonValue &v : f.json.array())
                page.models << hfModel(v.toObject());
            page.nextCursor = nextCursor(f.link);
        } else {
            const QJsonObject root = f.json.object();
            for (const QJsonValue &v : root.value(QStringLiteral("items")).toArray()) {
                HubModel m = civitaiModel(v.toObject(), nsfw);
                if (m.nsfw && !nsfw)
                    continue;
                page.models << m;
            }
            const QJsonValue cursor = root.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("nextCursor"));
            page.nextCursor = cursor.isDouble() ? QString::number(qint64(cursor.toDouble())) : cursor.toString();
        }
        return page;
    });
}

QFuture<HubModel> ModelHub::details(const HubModel &model)
{
    if (model.source == HubSource::Civitai || model.detailed)
        return QtFuture::makeReadyValueFuture(model);
    if (!validRepoId(model.id))
        return QtFuture::makeExceptionalFuture<HubModel>(std::make_exception_ptr(Error(Text::tr("Invalid repository name: %1").arg(model.id))));
    const QString base = m_hfBase;
    net::HttpClient *http = m_http;
    QPointer<ModelHub> self(this);
    return fetchJson(m_http, QUrl(base + QStringLiteral("/api/models/") + model.id), this)
        .then(this, [self, base, http, model](const Fetched &info) {
            const QJsonObject o = info.json.object();
            HubModel m = hfModel(o);
            if (m.downloads == 0)
                m.downloads = model.downloads;
            if (m.likes == 0)
                m.likes = model.likes;
            const QString sha = o.value(QStringLiteral("sha")).toString();
            if (!QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(sha).hasMatch())
                throw Error(Text::tr("Hugging Face did not report the current version of %1.").arg(model.id));
            if (!self)
                throw Error(Text::tr("Shutting down."));
            const QUrl tree(base + QStringLiteral("/api/models/") + m.id + QStringLiteral("/tree/") + sha + QStringLiteral("?recursive=true"));
            return fetchJson(http, tree, self).then([m, sha, base](const Fetched &t) mutable {
                HubVersion v;
                v.id = sha;
                v.name = sha.left(7);
                v.published = m.updated;
                for (const QJsonValue &ev : t.json.array()) {
                    const QJsonObject e = ev.toObject();
                    const QString path = e.value(QStringLiteral("path")).toString();
                    if (e.value(QStringLiteral("type")).toString() != QLatin1String("file") || !isModelFile(path) || path.contains(QLatin1String("..")))
                        continue;
                    HubFile f;
                    f.path = path;
                    f.size = qint64(e.value(QStringLiteral("size")).toDouble(-1));
                    if (f.size >= 0 && f.size < 1024 * 1024)
                        continue; // configs, tokenizers
                    f.sha256 = e.value(QStringLiteral("lfs")).toObject().value(QStringLiteral("oid")).toString().toLower();
                    f.url = QUrl(base + QLatin1Char('/') + m.id + QStringLiteral("/resolve/") + sha + QLatin1Char('/') + encodePath(path));
                    f.format = formatOf(path);
                    f.precision = precisionFromName(path);
                    f.kind = guessKind(path, m.id, m.type);
                    v.files << f;
                }
                std::sort(v.files.begin(), v.files.end(), [](const HubFile &a, const HubFile &b) { return a.path.compare(b.path, Qt::CaseInsensitive) < 0; });
                m.versions = {v};
                m.detailed = true;
                return m;
            });
        })
        .unwrap();
}

QString ModelHub::destinationFor(const HubDownload &d) const
{
    const QString name = QFileInfo(d.saveAs.isEmpty() ? d.file.fileName() : d.saveAs).fileName(); // no paths from the hub
    const QString folder = d.folder.isEmpty() ? QDir(m_library->defaultFolder()).filePath(ModelLibrary::subfolderFor(d.file.kind)) : d.folder;
    return QDir::cleanPath(QDir(folder).filePath(name));
}

Job *ModelHub::download(const HubDownload &d)
{
    QPointer<ModelHub> self(this);
    return m_jobs->submit(Text::tr("Download %1").arg(d.title()), QStringLiteral("model-download"), [self, d](Job *job) {
        if (!self)
            return job->fail(Text::tr("Shutting down."));
        auto *fetcher = new Fetcher(self, d, job);
        job->setCancelHandler([fetcher = QPointer<Fetcher>(fetcher)] {
            if (fetcher)
                fetcher->cancel();
        });
        fetcher->run();
    }, QStringLiteral("download"));
}

} // namespace lumen::runtime
