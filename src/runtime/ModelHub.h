#pragma once

#include "runtime/ModelFormats.h"

#include <QDateTime>
#include <QFuture>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace lumen {
class Job;
class JobQueue;
}

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

class ModelLibrary;
struct ModelEntry;

enum class HubSource { HuggingFace, Civitai };
QString hubSourceKey(HubSource s);   // "huggingface", "civitai"
QString hubSourceName(HubSource s);  // "Hugging Face", "Civitai"

struct HubFile
{
    QString path;            // path in the repository (Hugging Face) or file name (Civitai)
    qint64 size = -1;
    QString sha256;          // lower-case hex; empty = unknown
    QUrl url;                // download address
    QString format;          // "gguf", "safetensors", "pickle", "other"
    QString precision;       // "Q4_K_M", "fp16", … (from the name or the hub's metadata)
    ModelKind kind = ModelKind::Unknown; // a guess; the header decides after the download
    bool primary = false;
    QString hubType;         // Civitai's file type: "Model", "Pruned Model", "VAE", "Text Encoder", …
    bool required = false;   // Civitai: the creator marks the file as needed to use the model

    QString fileName() const;
    bool pickle() const { return format == QLatin1String("pickle"); }
};

struct HubVersion
{
    QString id;              // Hugging Face: commit; Civitai: version id
    QString name;
    QString baseModel;
    QStringList trainedWords;
    QDateTime published;
    QList<HubFile> files;
};

struct HubModel
{
    HubSource source = HubSource::HuggingFace;
    QString id;              // "Qwen/Qwen3-VL-8B-Instruct-GGUF" or the Civitai model id
    QString name;
    QString author;
    QString type;            // Hugging Face pipeline tag, Civitai model type ("Checkpoint", "LORA")
    QString license;         // Hugging Face licence id
    QVariantMap permissions; // Civitai: allowCommercialUse, allowDerivatives, allowNoCredit, allowDifferentLicense
    QString gated;           // Hugging Face: "" (open), "auto", "manual"
    bool nsfw = false;
    qint64 downloads = 0;
    qint64 likes = 0;
    QStringList tags;
    QDateTime updated;
    QUrl page;
    QUrl preview;            // a safe-for-work preview image (Civitai)
    QString description;     // plain text, shortened
    QList<HubVersion> versions; // Hugging Face: filled by ModelHub::details()
    bool detailed = false;
};

struct HubQuery
{
    QString text;
    // "" (anything), "llm" (GGUF language and vision models), "checkpoint", "lora", "controlnet",
    // "upscaler".
    QString category;
    bool nsfw = false;       // Civitai: include adult content
    QString cursor;          // next page (from HubPage)
    int limit = 30;
};

struct HubPage
{
    QList<HubModel> models;
    QString nextCursor;      // empty on the last page
};

// One file to fetch, with the metadata that goes into the "<file>.lumen.json" sidecar.
struct HubDownload
{
    HubSource source = HubSource::HuggingFace;
    QString modelId;
    QString modelName;
    QString versionId;
    QString versionName;
    HubFile file;
    QString saveAs;          // file name; empty = the hub's file name
    QString folder;          // target folder; empty = Lumen's models folder, sub-folder by kind
    QString license;
    QVariantMap permissions;
    QString baseModel;
    QStringList trainedWords;
    bool nsfw = false;
    QUrl page;
    bool allowPickle = false;
    // Files the model needs (its VAE, text encoder), recorded in the sidecar (`companionFiles`).
    QList<HubFile> companions;

    QString title() const;
};

// ---- Companion files (VAE, text encoder) ----
// Some Civitai models are not complete checkpoints: their version carries the VAE and text
// encoder as separate files, sometimes marked as required. They are downloaded with the model and
// recorded with it ("companions" in its Civitai data), so the servers can load them together.

// The companion files of a model file from the same version (none for a companion itself, and
// none on Hugging Face, which has no file types).
QList<HubFile> companionFiles(const QList<HubFile> &files, const HubFile &model);
QJsonObject companionToJson(const HubFile &file);
HubFile companionFromJson(const QJsonObject &o);
// One file of a Civitai version ("files" of the REST API).
HubFile civitaiFile(const QJsonObject &file, const QString &modelType);

struct ModelCompanion
{
    HubFile file;
    QString path;            // where it is on this computer; empty = missing
    bool missing() const { return path.isEmpty(); }
};
// The companions recorded for a library model and whether each is on this computer.
QList<ModelCompanion> companionsOf(const ModelLibrary *library, const QString &modelPath);
// A download of a missing companion, with the model's Civitai data.
HubDownload companionDownload(const ModelEntry &model, const HubFile &companion);
// sd.cpp server options for the companions on this computer: "vae", and "llm" or "t5xxl" for a
// text encoder of that type.
QVariantMap companionServerOptions(const ModelLibrary *library, const QString &modelPath);
// Adds companions to a downloaded model's sidecar (when they came later than the model).
bool recordCompanions(const QString &modelPath, const QList<HubFile> &companions);

// Starter models for "Set up local AI" (resources/models/starters.json): pinned Hugging Face files.
struct StarterFile
{
    QString id;
    QString role;            // "language", "projector", "checkpoint", "control_net", "ip_adapter", "clip_vision", "upscaler"
    ModelKind kind = ModelKind::Unknown;
    QString name;
    QString repo;
    QString revision;
    QString path;
    QString saveAs;
    qint64 size = 0;
    QString sha256;
    QString license;

    QString fileName() const;
};

struct StarterSet
{
    QString id;              // "compact", "standard", "large"
    double minVramGb = 0;
    QList<StarterFile> files;

    qint64 totalSize() const;
    const StarterFile *file(const QString &role) const;
};

QList<StarterSet> starterSets(const QByteArray &json = QByteArray()); // default: the bundled list
// The largest set whose minimum fits the GPU memory (0 = no GPU).
const StarterSet *pickStarterSet(const QList<StarterSet> &sets, quint64 vramBytes);
HubDownload starterDownload(const StarterFile &file, const QString &huggingFaceBase = QStringLiteral("https://huggingface.co"));

// Split GGUF parts ("…-00001-of-00003.gguf"): every part of the file's set, in order.
QList<HubFile> splitParts(const QList<HubFile> &files, const HubFile &file);
// The vision projector (mmproj) to use with a GGUF language model of the same repository.
std::optional<HubFile> projectorFor(const QList<HubFile> &files, const HubFile &model);
// Quantisation / precision written in a file name ("Q4_K_M", "fp16"), upper-case for GGUF types.
QString precisionFromName(const QString &fileName);

// Searches Hugging Face and Civitai and downloads model files into Lumen's models folder as
// Queue jobs (resumable, checked against the hub's SHA-256, two at a time). Access tokens for
// gated repositories and Civitai downloads come from the system credential store.
class ModelHub : public QObject
{
    Q_OBJECT
public:
    ModelHub(net::HttpClient *http, JobQueue *jobs, ModelLibrary *library, QObject *parent = nullptr);

    void setBaseUrls(const QString &huggingFace, const QString &civitai); // tests
    QString baseUrl(HubSource s) const;

    QFuture<HubPage> search(HubSource source, const HubQuery &query);
    // Hugging Face: the current commit, licence, gating and files. Civitai results are complete.
    QFuture<HubModel> details(const HubModel &model);

    static QString tokenKey(HubSource s); // credential store key
    // Where tokens come from (tests replace it); default: the credential store.
    std::function<QString(HubSource)> tokenProvider;
    QString token(HubSource s) const;

    // Lumen's models folder, sub-folder for the file's kind.
    QString destinationFor(const HubDownload &d) const;
    // Queues the download. Fails at once (job failed) for pickle files unless allowed.
    Job *download(const HubDownload &d);

    net::HttpClient *http() const { return m_http; }
    ModelLibrary *library() const { return m_library; }

signals:
    // A download finished and the file is in the library.
    void downloaded(const QString &path);

private:
    net::HttpClient *m_http;
    JobQueue *m_jobs;
    ModelLibrary *m_library;
    QString m_hfBase{QStringLiteral("https://huggingface.co")};
    QString m_civitaiBase{QStringLiteral("https://civitai.com")};
};

} // namespace lumen::runtime
