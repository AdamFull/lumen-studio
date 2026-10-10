#pragma once

#include <QFuture>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

#include <functional>

namespace lumen::net {
class HttpClient;
}

namespace lumen::runtime {

// A GPU type of RunPod's catalogue: VRAM and price per hour of one GPU.
struct RunPodGpu
{
    QString id;   // used to create pods
    QString name;
    int memoryGb = 0;
    double securePrice = 0;    // USD per hour; 0 = not offered
    double communityPrice = 0;
    int maxSecure = 0;
    int maxCommunity = 0;

    double price(bool community) const { return community ? communityPrice : securePrice; }
    bool offered(bool community) const { return price(community) > 0 && (community ? maxCommunity : maxSecure) > 0; }
};

// What a new pod runs.
struct RunPodPodSpec
{
    QString name;              // Lumen's pods start with RunPodClient::kTrainingPrefix
    QString image;
    QString gpuId;
    int gpuCount = 1;
    QString minCudaVersion;    // "12.8"
    bool community = false;    // Community cloud (cheaper) instead of Secure
    int diskGb = 80;           // container disk (ephemeral)
    QMap<QString, QString> env;
    QStringList ports;         // "8000/http"
    QStringList cmd;           // replaces the image's command
};

struct RunPodPod
{
    QString id;
    QString name;
    QString status;            // PROVISIONING, STARTING, RUNNING, EXITED, ERROR, TERMINATED
    double costPerHour = 0;
    QJsonObject raw;

    bool isRunning() const { return status == QLatin1String("RUNNING"); }
    bool isGone() const { return status == QLatin1String("TERMINATED") || status == QLatin1String("EXITED") || status == QLatin1String("ERROR"); }
    // HTTPS address of a port served through RunPod's proxy.
    QString proxyUrl(int port) const;
    static RunPodPod fromJson(const QJsonObject &o);
};

// RunPod REST API v2 (M15): pods and the GPU catalogue. The API key comes from the credential
// store. Failures are lumen::Error with RunPod's own message (RFC 9457 "detail"). GUI-thread object.
class RunPodClient : public QObject
{
    Q_OBJECT
public:
    static constexpr const char *kTrainingPrefix = "lumen-train-";

    explicit RunPodClient(net::HttpClient *http, QObject *parent = nullptr);

    // Credential store key of the API key.
    static QString apiKeyName();
    // The API key (tests replace it); default: the credential store.
    std::function<QString()> keyProvider;
    bool hasKey() const { return !key().isEmpty(); }
    void setBaseUrl(const QString &url) { m_base = url; }

    QFuture<QList<RunPodGpu>> gpus();
    QFuture<RunPodPod> createPod(const RunPodPodSpec &spec);
    QFuture<RunPodPod> pod(const QString &id);
    // Pods whose name starts with `namePrefix` (all when empty).
    QFuture<QList<RunPodPod>> pods(const QString &namePrefix = QString());
    QFuture<void> terminate(const QString &id);

    static QJsonObject podRequest(const RunPodPodSpec &spec);

private:
    QString key() const;
    // `notFoundOk`: a 404 resolves with an empty document (the pod is already gone).
    QFuture<QJsonDocument> call(const QByteArray &method, const QString &path, const QJsonObject &body = {}, bool notFoundOk = false);

    net::HttpClient *m_http;
    QString m_base = QStringLiteral("https://api.runpod.io/v2");
};

} // namespace lumen::runtime
