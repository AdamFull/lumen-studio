#pragma once

#include "ai/Operation.h"
#include "ai/Types.h"

#include <QFuture>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

namespace lumen::net {
class HttpClient;
class HttpCall;
}

namespace lumen::ai {

enum class ProviderKind { Image, Llm };

// Describes one configurable setting of a provider; the settings UI is generated from these.
struct SettingField
{
    enum class Type { Text, Secret, Url, Integer, Number, Boolean, Choice, Model, MultilineText };
    QString key;
    QString label;
    Type type = Type::Text;
    QVariant defaultValue;
    QString help;
    QStringList choices;    // Choice: allowed values; Model: suggestions
    double minimum = 0;
    double maximum = 0;
    bool required = false;
    bool advanced = false;
};

struct ProviderType
{
    QString id;             // e.g. "sdwebui", "comfyui", "openai_chat"
    QString name;
    QString description;
    ProviderKind kind = ProviderKind::Image;
    bool local = false;     // runs on the user's machine (default concurrency 1)
    QList<SettingField> fields;
    QUrl documentation;
    QString icon;           // resource icon name
};

class Provider : public QObject
{
    Q_OBJECT
public:
    Provider(ProviderType type, QString instanceId, net::HttpClient *http, QObject *parent = nullptr);

    const ProviderType &type() const { return m_type; }
    ProviderKind kind() const { return m_type.kind; }
    QString instanceId() const { return m_instanceId; }
    QString displayName() const { return m_name.isEmpty() ? m_type.name : m_name; }
    void setDisplayName(const QString &name) { m_name = name; }

    // Settings include secrets (resolved by the registry from the credential store).
    void configure(const QVariantMap &settings);
    const QVariantMap &settings() const { return m_settings; }
    QVariant setting(const QString &key) const;
    QString text(const QString &key) const { return setting(key).toString().trimmed(); }
    QUrl baseUrl(const QString &key = QStringLiteral("url")) const;

    // Lightweight connectivity check. Resolves to a human readable summary or throws ProviderError.
    virtual QFuture<QString> testConnection() = 0;
    // Model names available for the "model" setting.
    virtual QFuture<QStringList> listModels() = 0;
    // Options for a Model-type setting other than "model" (e.g. ControlNet models). Default: none.
    virtual QFuture<QStringList> listOptions(const QString &settingKey);

signals:
    void configurationChanged();

protected:
    virtual void onConfigured() {}
    net::HttpClient *http() const { return m_http; }
    // Builds a request relative to the base URL.
    QUrl endpoint(const QString &path, const QString &baseKey = QStringLiteral("url")) const;

private:
    ProviderType m_type;
    QString m_instanceId;
    QString m_name;
    QVariantMap m_settings;
    net::HttpClient *m_http;
};

class ImageProvider : public Provider
{
    Q_OBJECT
public:
    using Provider::Provider;

    virtual ImageCapabilities capabilities() const = 0;
    // Capabilities for a specific request (e.g. a workflow selected through request extras).
    virtual ImageCapabilities capabilitiesFor(const ImageRequest &request) const
    {
        Q_UNUSED(request);
        return capabilities();
    }
    virtual ImageOperation *generate(const ImageRequest &request) = 0;

    virtual bool supportsUtility(Utility) const { return false; }
    // Runs an auxiliary model (upscale, background removal, pose detection, captioning).
    // Image utilities return images; Caption returns ImageResult::text.
    virtual ImageOperation *runUtility(Utility utility, const QImage &image, const QJsonObject &params);
    virtual QFuture<QStringList> listSamplers();
    virtual QFuture<QStringList> listSchedulers();
    // Frees GPU memory held between jobs (e.g. unloads models) so another local model, such as a
    // language model on the same GPU, can run at full speed. Resolves when done; no-op by default.
    virtual QFuture<void> releaseGpu() { return QtFuture::makeReadyVoidFuture(); }
};

class LlmProvider : public Provider
{
    Q_OBJECT
public:
    using Provider::Provider;

    virtual LlmCapabilities capabilities() const = 0;
    virtual ChatOperation *chat(const ChatRequest &request) = 0;
};

// Helpers shared by provider implementations ------------------------------------------------------

// Resolves a future with the result of an HTTP call processed by `fn` (runs on the GUI thread).
template<typename T>
QFuture<T> futureFromCall(net::HttpCall *call, std::function<T(net::HttpCall *)> fn);

// A ready future holding a ProviderError.
template<typename T>
QFuture<T> failedFuture(const QString &message)
{
    return QtFuture::makeExceptionalFuture<T>(std::make_exception_ptr(ProviderError(message)));
}

QString errorMessage(const std::exception_ptr &e);

} // namespace lumen::ai

#include "ai/ProviderImpl.h"
