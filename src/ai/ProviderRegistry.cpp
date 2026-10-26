#include "ai/ProviderRegistry.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/SecretStore.h"
#include "net/HttpClient.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPromise>
#include <QSaveFile>
#include <QUuid>

namespace lumen::ai {

QString llmRoleKey(LlmRole role)
{
    switch (role) {
    case LlmRole::Assistant: return QStringLiteral("assistant");
    case LlmRole::Vision: return QStringLiteral("vision");
    case LlmRole::Director: return QStringLiteral("director");
    case LlmRole::PromptWriter: return QStringLiteral("prompt_writer");
    }
    return {};
}

QString llmRoleLabel(LlmRole role)
{
    switch (role) {
    case LlmRole::Assistant: return QObject::tr("Chat assistant");
    case LlmRole::Vision: return QObject::tr("Vision analysis (reference images)");
    case LlmRole::Director: return QObject::tr("Director (shot & pose planning)");
    case LlmRole::PromptWriter: return QObject::tr("Prompt writer");
    }
    return {};
}

QList<LlmRole> allLlmRoles()
{
    return {LlmRole::Assistant, LlmRole::Vision, LlmRole::Director, LlmRole::PromptWriter};
}

ProviderRegistry::ProviderRegistry(net::HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

ProviderRegistry::~ProviderRegistry() = default;

void ProviderRegistry::registerType(const ProviderType &type, Factory factory)
{
    m_types.append(type);
    m_factories.insert(type.id, std::move(factory));
}

QList<ProviderType> ProviderRegistry::types() const
{
    return m_types;
}

QList<ProviderType> ProviderRegistry::types(ProviderKind kind) const
{
    QList<ProviderType> out;
    for (const ProviderType &t : m_types) {
        if (t.kind == kind)
            out.append(t);
    }
    return out;
}

std::optional<ProviderType> ProviderRegistry::type(const QString &typeId) const
{
    for (const ProviderType &t : m_types) {
        if (t.id == typeId)
            return t;
    }
    return std::nullopt;
}

QList<Provider *> ProviderRegistry::providers(ProviderKind kind) const
{
    QList<Provider *> out;
    for (Provider *p : m_order) {
        if (p->kind() == kind)
            out.append(p);
    }
    return out;
}

QList<ImageProvider *> ProviderRegistry::imageProviders() const
{
    QList<ImageProvider *> out;
    for (Provider *p : m_order) {
        if (auto *ip = qobject_cast<ImageProvider *>(p); ip && isEnabled(p->instanceId()))
            out.append(ip);
    }
    return out;
}

QList<LlmProvider *> ProviderRegistry::llmProviders() const
{
    QList<LlmProvider *> out;
    for (Provider *p : m_order) {
        if (auto *lp = qobject_cast<LlmProvider *>(p); lp && isEnabled(p->instanceId()))
            out.append(lp);
    }
    return out;
}

ImageProvider *ProviderRegistry::imageProvider(const QString &id) const
{
    return qobject_cast<ImageProvider *>(m_instances.value(id));
}

LlmProvider *ProviderRegistry::llmProvider(const QString &id) const
{
    return qobject_cast<LlmProvider *>(m_instances.value(id));
}

Provider *ProviderRegistry::instantiate(const QString &id, const QString &typeId, const QString &name, const QVariantMap &settings)
{
    const auto t = type(typeId);
    if (!t || !m_factories.contains(typeId)) {
        qCWarning(lcAi) << "Unknown provider type" << typeId;
        return nullptr;
    }
    Provider *p = m_factories.value(typeId)(*t, id, m_http, this);
    if (!p)
        return nullptr;
    p->setDisplayName(name);
    p->configure(settings);
    m_instances.insert(id, p);
    m_order.append(p);
    return p;
}

void ProviderRegistry::storeSecrets(const QString &id, const ProviderType &type, QVariantMap &settings) const
{
    for (const SettingField &f : type.fields) {
        if (f.type != SettingField::Type::Secret || !settings.contains(f.key))
            continue;
        const QString value = settings.take(f.key).toString();
        const QString key = QStringLiteral("provider/%1/%2").arg(id, f.key);
        if (!SecretStore::write(key, value))
            qCWarning(lcAi) << "Could not store secret for" << id << SecretStore::lastError();
    }
}

QVariantMap ProviderRegistry::loadSecrets(const QString &id, const ProviderType &type) const
{
    QVariantMap out;
    for (const SettingField &f : type.fields) {
        if (f.type == SettingField::Type::Secret)
            out.insert(f.key, SecretStore::read(QStringLiteral("provider/%1/%2").arg(id, f.key)));
    }
    return out;
}

Provider *ProviderRegistry::addProvider(const QString &typeId, const QString &name, const QVariantMap &settings)
{
    const auto t = type(typeId);
    if (!t)
        return nullptr;
    const QString id = typeId + u'-' + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    QVariantMap plain = settings;
    storeSecrets(id, *t, plain);
    Provider *p = instantiate(id, typeId, name, settings);
    if (!p)
        return nullptr;
    if (t->kind == ProviderKind::Image && m_defaultImage.isEmpty())
        m_defaultImage = id;
    if (t->kind == ProviderKind::Llm && m_roles.value(llmRoleKey(LlmRole::Assistant)).providerId.isEmpty())
        m_roles.insert(llmRoleKey(LlmRole::Assistant), RoleBinding{id, {}});
    save();
    emit providersChanged();
    emit rolesChanged();
    return p;
}

void ProviderRegistry::updateProvider(const QString &id, const QString &name, const QVariantMap &settings)
{
    Provider *p = m_instances.value(id);
    if (!p)
        return;
    QVariantMap plain = settings;
    storeSecrets(id, p->type(), plain);
    p->setDisplayName(name);
    p->configure(settings);
    save();
    emit providersChanged();
}

void ProviderRegistry::removeProvider(const QString &id)
{
    Provider *p = m_instances.take(id);
    if (!p)
        return;
    for (const SettingField &f : p->type().fields) {
        if (f.type == SettingField::Type::Secret)
            SecretStore::remove(QStringLiteral("provider/%1/%2").arg(id, f.key));
    }
    m_order.removeAll(p);
    m_disabled.remove(id);
    if (m_defaultImage == id) {
        const auto images = imageProviders();
        m_defaultImage = images.isEmpty() ? QString() : images.first()->instanceId();
    }
    if (m_defaultEdit == id)
        m_defaultEdit.clear();
    for (auto it = m_roles.begin(); it != m_roles.end(); ++it) {
        if (it->providerId == id)
            it->providerId.clear();
    }
    p->deleteLater();
    save();
    emit providersChanged();
    emit rolesChanged();
}

void ProviderRegistry::setEnabled(const QString &id, bool enabled)
{
    if (enabled)
        m_disabled.remove(id);
    else
        m_disabled.insert(id);
    save();
    emit providersChanged();
}

QVariantMap ProviderRegistry::settingsFor(const QString &id) const
{
    Provider *p = m_instances.value(id);
    return p ? p->settings() : QVariantMap();
}

int ProviderRegistry::concurrency(const QString &id) const
{
    Provider *p = m_instances.value(id);
    if (!p)
        return 1;
    const int configured = p->setting(QStringLiteral("concurrency")).toInt();
    if (configured > 0)
        return configured;
    return p->type().local ? 1 : 4;
}

ImageProvider *ProviderRegistry::defaultImageProvider() const
{
    if (ImageProvider *p = imageProvider(m_defaultImage); p && isEnabled(m_defaultImage))
        return p;
    const auto list = imageProviders();
    return list.isEmpty() ? nullptr : list.first();
}

void ProviderRegistry::setDefaultImageProvider(const QString &id)
{
    m_defaultImage = id;
    save();
    emit rolesChanged();
}

ImageProvider *ProviderRegistry::editImageProvider() const
{
    if (ImageProvider *p = imageProvider(m_defaultEdit); p && isEnabled(m_defaultEdit))
        return p;
    return defaultImageProvider();
}

void ProviderRegistry::setEditImageProvider(const QString &id)
{
    m_defaultEdit = id;
    save();
    emit rolesChanged();
}

ImageProvider *ProviderRegistry::imageProviderFor(bool changesImage) const
{
    return changesImage ? editImageProvider() : defaultImageProvider();
}

RoleBinding ProviderRegistry::role(LlmRole role) const
{
    return m_roles.value(llmRoleKey(role));
}

void ProviderRegistry::setRole(LlmRole role, const RoleBinding &binding)
{
    m_roles.insert(llmRoleKey(role), binding);
    save();
    emit rolesChanged();
}

LlmProvider *ProviderRegistry::llmFor(LlmRole r, QString *model) const
{
    for (LlmRole candidate : {r, LlmRole::Assistant}) {
        const RoleBinding b = role(candidate);
        if (LlmProvider *p = llmProvider(b.providerId); p && isEnabled(b.providerId)) {
            if (model)
                *model = b.model;
            return p;
        }
    }
    const auto list = llmProviders();
    if (model)
        model->clear();
    return list.isEmpty() ? nullptr : list.first();
}

QString ProviderRegistry::configPath() const
{
    return paths::appData() + QStringLiteral("/providers.json");
}

void ProviderRegistry::load()
{
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("instances")).toArray()) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        const QString typeId = o.value(QStringLiteral("type")).toString();
        const auto t = type(typeId);
        if (id.isEmpty() || !t || m_instances.contains(id))
            continue;
        QVariantMap settings = o.value(QStringLiteral("settings")).toObject().toVariantMap();
        const QVariantMap secrets = loadSecrets(id, *t);
        for (auto it = secrets.begin(); it != secrets.end(); ++it)
            settings.insert(it.key(), it.value());
        instantiate(id, typeId, o.value(QStringLiteral("name")).toString(), settings);
        if (!o.value(QStringLiteral("enabled")).toBool(true))
            m_disabled.insert(id);
    }
    m_defaultImage = root.value(QStringLiteral("defaultImage")).toString();
    m_defaultEdit = root.value(QStringLiteral("defaultEdit")).toString();
    const QJsonObject roles = root.value(QStringLiteral("roles")).toObject();
    for (auto it = roles.begin(); it != roles.end(); ++it) {
        const QJsonObject b = it.value().toObject();
        m_roles.insert(it.key(), RoleBinding{b.value(QStringLiteral("provider")).toString(), b.value(QStringLiteral("model")).toString()});
    }
    emit providersChanged();
    emit rolesChanged();
}

void ProviderRegistry::save() const
{
    QJsonArray instances;
    for (Provider *p : m_order) {
        QVariantMap settings = p->settings();
        for (const SettingField &f : p->type().fields) {
            if (f.type == SettingField::Type::Secret)
                settings.remove(f.key);
        }
        instances.append(QJsonObject{{QStringLiteral("id"), p->instanceId()},
                                     {QStringLiteral("type"), p->type().id},
                                     {QStringLiteral("name"), p->displayName()},
                                     {QStringLiteral("enabled"), isEnabled(p->instanceId())},
                                     {QStringLiteral("settings"), QJsonObject::fromVariantMap(settings)}});
    }
    QJsonObject roles;
    for (auto it = m_roles.begin(); it != m_roles.end(); ++it)
        roles.insert(it.key(), QJsonObject{{QStringLiteral("provider"), it->providerId}, {QStringLiteral("model"), it->model}});
    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("instances"), instances},
                           {QStringLiteral("defaultImage"), m_defaultImage},
                           {QStringLiteral("defaultEdit"), m_defaultEdit},
                           {QStringLiteral("roles"), roles}};
    QSaveFile f(configPath());
    if (!f.open(QIODevice::WriteOnly)) {
        qCWarning(lcAi) << "Cannot save provider configuration:" << f.errorString();
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.commit();
}

QList<ServerProbe> ProviderRegistry::defaultServerProbes()
{
    return {
        {QStringLiteral("sdwebui"), QStringLiteral("Stable Diffusion WebUI"), QStringLiteral("http://127.0.0.1:7860"), QStringLiteral("/sdapi/v1/sd-models")},
        {QStringLiteral("sdwebui"), QStringLiteral("Stable Diffusion WebUI (Forge)"), QStringLiteral("http://127.0.0.1:7861"), QStringLiteral("/sdapi/v1/sd-models")},
        {QStringLiteral("comfyui"), QStringLiteral("ComfyUI"), QStringLiteral("http://127.0.0.1:8188"), QStringLiteral("/system_stats")},
        {QStringLiteral("comfyui"), QStringLiteral("ComfyUI Desktop"), QStringLiteral("http://127.0.0.1:8000"), QStringLiteral("/system_stats")},
        // sd-server: its own default port, and the one Lumen's servers start from.
        {QStringLiteral("sdcpp"), QStringLiteral("stable-diffusion.cpp (sd-server)"), QStringLiteral("http://127.0.0.1:1234"), QStringLiteral("/sdcpp/v1/capabilities")},
        {QStringLiteral("sdcpp"), QStringLiteral("stable-diffusion.cpp (sd-server)"), QStringLiteral("http://127.0.0.1:7861"), QStringLiteral("/sdcpp/v1/capabilities")},
        {QStringLiteral("llamacpp"), QStringLiteral("llama.cpp server"), QStringLiteral("http://127.0.0.1:8080"), QStringLiteral("/v1/models")},
        {QStringLiteral("ollama"), QStringLiteral("Ollama"), QStringLiteral("http://127.0.0.1:11434"), QStringLiteral("/api/tags")},
        {QStringLiteral("lmstudio"), QStringLiteral("LM Studio"), QStringLiteral("http://127.0.0.1:1234"), QStringLiteral("/v1/models")},
        {QStringLiteral("koboldcpp"), QStringLiteral("KoboldCpp"), QStringLiteral("http://127.0.0.1:5001"), QStringLiteral("/v1/models")},
        {QStringLiteral("partserver"), QStringLiteral("Lumen Part Server"), QStringLiteral("http://127.0.0.1:7870"), QStringLiteral("/v1/info")},
    };
}

QFuture<QList<DetectedServer>> ProviderRegistry::detectLocalServers()
{
    return probe(serverProbes);
}

QFuture<QList<DetectedServer>> ProviderRegistry::probeAddress(const QUrl &address)
{
    const QString base = address.adjusted(QUrl::StripTrailingSlash | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
    return probe({{QStringLiteral("sdcpp"), QStringLiteral("stable-diffusion.cpp (sd-server)"), base, QStringLiteral("/sdcpp/v1/capabilities")},
                  {QStringLiteral("comfyui"), QStringLiteral("ComfyUI"), base, QStringLiteral("/system_stats")},
                  {QStringLiteral("sdwebui"), QStringLiteral("Stable Diffusion WebUI / Forge"), base, QStringLiteral("/sdapi/v1/sd-models")}});
}

QFuture<QList<DetectedServer>> ProviderRegistry::probe(const QList<ServerProbe> &probes)
{
    struct State
    {
        QPromise<QList<DetectedServer>> promise;
        QList<std::optional<DetectedServer>> found; // by probe, so the result keeps the probes' order
        int remaining = 0;
    };
    auto state = std::make_shared<State>();
    state->remaining = int(probes.size()) + 1; // held until every call is made
    state->found.resize(probes.size());
    state->promise.start();
    QFuture<QList<DetectedServer>> future = state->promise.future();
    auto done = [state] {
        if (--state->remaining > 0)
            return;
        QList<DetectedServer> out;
        for (const std::optional<DetectedServer> &s : std::as_const(state->found)) {
            if (s)
                out << *s;
        }
        state->promise.addResult(out);
        state->promise.finish();
    };
    net::CallOptions opts;
    opts.timeoutMs = 2500;
    opts.maxRetries = 0;
    opts.quiet = true;
    for (qsizetype i = 0; i < probes.size(); ++i) {
        const ServerProbe probe = probes[i];
        if (!type(probe.typeId)) {
            done();
            continue;
        }
        net::HttpCall *call = m_http->get(net::HttpClient::makeRequest(QUrl(probe.base + probe.path)), opts);
        connect(call, &net::HttpCall::finished, this, [state, call, probe, i, done] {
            if (call->ok()) {
                QString detail;
                const QJsonDocument doc = QJsonDocument::fromJson(call->body());
                if (doc.isArray())
                    detail = QObject::tr("%n model(s)", nullptr, int(doc.array().size()));
                else if (doc.object().contains(QStringLiteral("data")))
                    detail = QObject::tr("%n model(s)", nullptr, int(doc.object().value(QStringLiteral("data")).toArray().size()));
                else if (doc.object().value(QStringLiteral("models")).isArray())
                    detail = QObject::tr("%n model(s)", nullptr, int(doc.object().value(QStringLiteral("models")).toArray().size()));
                else if (doc.object().value(QStringLiteral("model")).isObject()) // sd-server: the one model it runs
                    detail = doc.object().value(QStringLiteral("model")).toObject().value(QStringLiteral("name")).toString();
                state->found[i] = DetectedServer{probe.typeId, probe.name, QUrl(probe.base), detail};
            }
            done();
        });
    }
    done();
    return future;
}

} // namespace lumen::ai
