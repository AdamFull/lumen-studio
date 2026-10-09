#pragma once

#include "ai/Provider.h"

#include <QFuture>
#include <QHash>
#include <QObject>

#include <functional>
#include <optional>

namespace lumen::ai {

// Tasks an LLM can be assigned to. Each role binds to a provider instance plus an optional model.
enum class LlmRole { Assistant, Vision, Director, PromptWriter };
QString llmRoleKey(LlmRole role);
QString llmRoleLabel(LlmRole role);
QList<LlmRole> allLlmRoles();

struct RoleBinding
{
    QString providerId;
    QString model; // empty = provider default
};

struct DetectedServer
{
    QString typeId;
    QString name;
    QUrl url;
    QString detail;
};

// Owns all configured provider instances. Non-secret settings are stored in
// <app data>/providers.json; secrets go to the OS credential store.
class ProviderRegistry : public QObject
{
    Q_OBJECT
public:
    using Factory = std::function<Provider *(const ProviderType &, const QString &instanceId, net::HttpClient *, QObject *parent)>;

    explicit ProviderRegistry(net::HttpClient *http, QObject *parent = nullptr);
    ~ProviderRegistry() override;

    void registerType(const ProviderType &type, Factory factory);
    QList<ProviderType> types() const;
    QList<ProviderType> types(ProviderKind kind) const;
    std::optional<ProviderType> type(const QString &typeId) const;

    // ---- instances ------------------------------------------------------------------------------
    QList<Provider *> providers() const { return m_order; }
    QList<Provider *> providers(ProviderKind kind) const;
    QList<ImageProvider *> imageProviders() const;
    QList<LlmProvider *> llmProviders() const;
    Provider *provider(const QString &id) const { return m_instances.value(id); }
    ImageProvider *imageProvider(const QString &id) const;
    LlmProvider *llmProvider(const QString &id) const;

    Provider *addProvider(const QString &typeId, const QString &name, const QVariantMap &settings);
    void updateProvider(const QString &id, const QString &name, const QVariantMap &settings);
    void removeProvider(const QString &id);
    bool isEnabled(const QString &id) const { return !m_disabled.contains(id); }
    void setEnabled(const QString &id, bool enabled);
    // Settings for the editor dialog (secrets included).
    QVariantMap settingsFor(const QString &id) const;
    int concurrency(const QString &id) const;

    // ---- defaults & roles -----------------------------------------------------------------------
    ImageProvider *defaultImageProvider() const;
    void setDefaultImageProvider(const QString &id);
    RoleBinding role(LlmRole role) const;
    void setRole(LlmRole role, const RoleBinding &binding);
    // Resolves a role with fallbacks (role → assistant → first enabled LLM). May return null.
    LlmProvider *llmFor(LlmRole role, QString *model = nullptr) const;

    // ---- persistence ----------------------------------------------------------------------------
    void load();
    void save() const;
    QString configPath() const;

    // Probes default local ports of known servers.
    QFuture<QList<DetectedServer>> detectLocalServers();

signals:
    void providersChanged();
    void rolesChanged();

private:
    Provider *instantiate(const QString &id, const QString &typeId, const QString &name, const QVariantMap &settings);
    void storeSecrets(const QString &id, const ProviderType &type, QVariantMap &settings) const;
    QVariantMap loadSecrets(const QString &id, const ProviderType &type) const;

    net::HttpClient *m_http;
    QList<ProviderType> m_types;
    QHash<QString, Factory> m_factories;
    QHash<QString, Provider *> m_instances;
    QList<Provider *> m_order;
    QSet<QString> m_disabled;
    QString m_defaultImage;
    QHash<QString, RoleBinding> m_roles;
};

} // namespace lumen::ai
