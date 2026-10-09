#include "ai/providers/AnthropicProvider.h"

#include "ai/ProviderUtil.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JsonSchema.h"
#include "core/JsonUtil.h"
#include "net/SseParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

namespace lumen::ai {

namespace {

const QString kDefaultModel = QStringLiteral("claude-opus-5-5");
constexpr auto kApiVersion = "2023-06-01";
constexpr auto kFallbackBeta = "server-side-fallback-2026-07-01";

QJsonObject imageBlock(const QImage &image)
{
    QImage img = img::thumbnail(image, 1568);
    if (img.hasAlphaChannel())
        img = img::flatten(img, Qt::white);
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                       {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                              {QStringLiteral("media_type"), QStringLiteral("image/jpeg")},
                                                              {QStringLiteral("data"), QString::fromLatin1(img::toBase64(img, "JPEG", 90))}}}};
}

QJsonObject textBlock(const QString &text)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}};
}

} // namespace

ProviderType AnthropicProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("anthropic");
    t.name = QStringLiteral("Anthropic Claude");
    t.description = tr("Claude models through the Anthropic API: excellent vision analysis, planning and tool use.");
    t.kind = ProviderKind::Llm;
    t.icon = QStringLiteral("chat");
    t.documentation = QUrl(QStringLiteral("https://docs.claude.com/en/api/messages"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Base URL"), F::Url, QStringLiteral("https://api.anthropic.com/v1"), QString(), {}, 0, 0, true, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), tr("From console.anthropic.com."), {}, 0, 0, true},
        {QStringLiteral("model"), tr("Model"), F::Model, kDefaultModel, QString(),
         {QStringLiteral("claude-opus-5-5"), QStringLiteral("claude-sonnet-5-5"), QStringLiteral("claude-haiku-5-5"), QStringLiteral("claude-fable-5-1")}},
        {QStringLiteral("effort"), tr("Effort"), F::Choice, QStringLiteral("medium"),
         tr("Reasoning depth vs. speed and cost."),
         {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("max")}},
        {QStringLiteral("fallback"), tr("Refusal fallback"), F::Boolean, true,
         tr("When a request is declined by a safety classifier, let the API retry it on Anthropic's recommended fallback model."),
         {}, 0, 0, false, true},
        {QStringLiteral("max_tokens"), tr("Max output tokens"), F::Integer, 32000, QString(), {}, 1024, 128000, false, true},
        {QStringLiteral("timeout_s"), tr("Timeout (seconds)"), F::Integer, 300, QString(), {}, 10, 3600, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, 4, QString(), {}, 1, 16, false, true},
    };
    return t;
}

AnthropicProvider::AnthropicProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : LlmProvider(type, instanceId, http, parent)
{
}

bool AnthropicProvider::fallbackSupported(const QString &model) const
{
    if (!setting(QStringLiteral("fallback")).toBool())
        return false;
    // Server-side "default" fallbacks exist for these model families on the Claude API.
    for (const char *prefix : {"claude-opus-5", "claude-fable-5-1", "claude-sonnet-5-5"}) {
        if (model.startsWith(QLatin1String(prefix)))
            return true;
    }
    return false;
}

QList<QPair<QByteArray, QByteArray>> AnthropicProvider::headers(const QString &model) const
{
    QList<QPair<QByteArray, QByteArray>> h{{"x-api-key", text(QStringLiteral("api_key")).toUtf8()},
                                           {"anthropic-version", kApiVersion}};
    if (!model.isEmpty() && fallbackSupported(model))
        h.append(qMakePair(QByteArray("anthropic-beta"), QByteArray(kFallbackBeta)));
    return h;
}

QNetworkRequest AnthropicProvider::request(const QString &path, const QString &model) const
{
    return net::HttpClient::makeRequest(endpoint(path), headers(model));
}

QFuture<QString> AnthropicProvider::testConnection()
{
    return listModels().then([](QStringList models) {
        return tr("Connected - %n model(s) available", nullptr, int(models.size()));
    });
}

QFuture<QStringList> AnthropicProvider::listModels()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/models?limit=100")), opts), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : call->json().value(QStringLiteral("data")).toArray())
            out << v.toObject().value(QStringLiteral("id")).toString();
        return out;
    });
}

LlmCapabilities AnthropicProvider::capabilities() const
{
    return LlmCapabilities{true, true, true, true};
}

QJsonObject AnthropicProvider::buildBody(const ChatRequest &r) const
{
    QStringList system;
    QJsonArray messages;
    auto appendMessage = [&messages](const QString &role, const QJsonArray &content) {
        // Consecutive same-role messages are merged (e.g. several tool results in one user turn).
        if (!messages.isEmpty() && messages.last().toObject().value(QStringLiteral("role")).toString() == role) {
            QJsonObject last = messages.last().toObject();
            QJsonArray merged = last.value(QStringLiteral("content")).toArray();
            for (const QJsonValue &v : content)
                merged.append(v);
            last.insert(QStringLiteral("content"), merged);
            messages[messages.size() - 1] = last;
        } else {
            messages.append(QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), content}});
        }
    };
    for (const ChatMessage &m : r.messages) {
        switch (m.role) {
        case ChatRole::System:
            system << m.text;
            break;
        case ChatRole::User: {
            QJsonArray content;
            for (const ChatImage &img : m.images) {
                if (!img.note.isEmpty())
                    content.append(textBlock(img.note));
                content.append(imageBlock(img.image));
            }
            if (!m.text.isEmpty() || content.isEmpty())
                content.append(textBlock(m.text.isEmpty() ? QStringLiteral("(no text)") : m.text));
            appendMessage(QStringLiteral("user"), content);
            break;
        }
        case ChatRole::Assistant: {
            // Echo native content (thinking + tool_use blocks) unchanged when available.
            if (m.providerType == type().id && !m.providerContent.isEmpty()) {
                appendMessage(QStringLiteral("assistant"), m.providerContent);
                break;
            }
            QJsonArray content;
            if (!m.text.isEmpty())
                content.append(textBlock(m.text));
            for (const ToolCall &c : m.toolCalls) {
                content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                           {QStringLiteral("id"), c.id},
                                           {QStringLiteral("name"), c.name},
                                           {QStringLiteral("input"), c.arguments}});
            }
            if (!content.isEmpty())
                appendMessage(QStringLiteral("assistant"), content);
            break;
        }
        case ChatRole::Tool:
            appendMessage(QStringLiteral("user"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                                                        {QStringLiteral("tool_use_id"), m.toolCallId},
                                                                        {QStringLiteral("content"), m.text}}});
            break;
        }
    }

    const QString model = r.model.isEmpty() ? (text(QStringLiteral("model")).isEmpty() ? kDefaultModel : text(QStringLiteral("model"))) : r.model;
    const int maxTokens = std::max(1024, std::min(r.maxTokens > 0 ? std::max(r.maxTokens, 16000) : 32000,
                                                  setting(QStringLiteral("max_tokens")).toInt() > 0 ? setting(QStringLiteral("max_tokens")).toInt() : 32000));
    QJsonObject body{{QStringLiteral("model"), model},
                     {QStringLiteral("max_tokens"), maxTokens},
                     {QStringLiteral("messages"), messages},
                     {QStringLiteral("stream"), r.stream}};
    if (!system.isEmpty())
        body.insert(QStringLiteral("system"), system.join(QStringLiteral("\n\n")));
    // Current Claude models always think adaptively and reject sampling parameters; depth is set by effort.
    QJsonObject outputConfig;
    const QString effort = text(QStringLiteral("effort"));
    if (!effort.isEmpty())
        outputConfig.insert(QStringLiteral("effort"), effort);
    if (!r.jsonSchema.isEmpty()) {
        outputConfig.insert(QStringLiteral("format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                                                  {QStringLiteral("schema"), json::toStrictSchema(r.jsonSchema)}});
    }
    if (!outputConfig.isEmpty())
        body.insert(QStringLiteral("output_config"), outputConfig);
    if (!r.tools.isEmpty()) {
        QJsonArray tools;
        for (const ToolDefinition &t : r.tools) {
            tools.append(QJsonObject{{QStringLiteral("name"), t.name},
                                     {QStringLiteral("description"), t.description},
                                     {QStringLiteral("input_schema"), t.parameters}});
        }
        body.insert(QStringLiteral("tools"), tools);
    }
    if (fallbackSupported(model))
        body.insert(QStringLiteral("fallbacks"), QStringLiteral("default"));
    return body;
}

ChatOperation *AnthropicProvider::chat(const ChatRequest &r)
{
    auto *op = new ChatOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("Anthropic API key is not set. Add it in Settings → Providers."));
        return op;
    }
    auto state = std::make_shared<RequestState>();
    op->onCancel([state, op] {
        state->cancelled = true;
        if (state->call)
            state->call->abort();
        else
            op->reportCancelled();
    });
    QPointer<AnthropicProvider> self(this);
    net::CallOptions opts;
    opts.timeoutMs = std::max(10, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.streaming = r.stream;
    opts.maxRetries = 2;
    const QString typeId = type().id;
    async::run([self, r] {
        if (!self)
            throw ProviderError(QStringLiteral("provider removed"));
        return self->buildBody(r);
    })
        .then(op, [self, op, state, opts, stream = r.stream, typeId](QJsonObject body) {
            if (!self)
                return;
            if (state->cancelled) {
                op->reportCancelled();
                return;
            }
            const QString model = body.value(QStringLiteral("model")).toString();
            net::HttpCall *call = self->http()->postJson(self->request(QStringLiteral("/messages"), model), body, opts);
            state->call = call;
            auto sse = std::make_shared<net::SseParser>();
            auto acc = std::make_shared<AnthropicStreamAccumulator>();
            auto handleEvents = [op, acc](const QList<net::SseEvent> &events) {
                for (const net::SseEvent &e : events) {
                    QString text, thinking;
                    acc->feed(e.event, e.data, &text, &thinking);
                    op->appendText(text);
                    op->appendReasoning(thinking);
                }
            };
            if (stream) {
                QObject::connect(call, &net::HttpCall::chunk, op, [op, sse, state, handleEvents](const QByteArray &data) {
                    try {
                        handleEvents(sse->feed(data));
                    } catch (const std::exception &e) {
                        op->fail(describeException(e));
                        if (state->call)
                            state->call->abort();
                    }
                });
            }
            QObject::connect(call, &net::HttpCall::finished, op, [op, call, state, sse, acc, stream, handleEvents, typeId] {
                state->call = nullptr;
                if (state->cancelled || call->isAborted()) {
                    op->reportCancelled();
                    return;
                }
                if (!call->ok()) {
                    op->fail(call->errorString());
                    return;
                }
                try {
                    if (stream)
                        handleEvents(sse->finish());
                    else
                        acc->feedMessage(call->json());
                    ChatResult result = acc->finish();
                    result.providerType = typeId;
                    if (result.finishReason == QLatin1String("refusal"))
                        throw ProviderError(QObject::tr("Claude declined this request%1.")
                                                .arg(result.usage.value(QStringLiteral("refusal_explanation")).toString().isEmpty()
                                                         ? QString()
                                                         : QStringLiteral(": ") + result.usage.value(QStringLiteral("refusal_explanation")).toString()));
                    op->succeed(result);
                } catch (const std::exception &e) {
                    op->fail(describeException(e));
                }
            });
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    return op;
}

// ---- stream accumulator -------------------------------------------------------------------------

void AnthropicStreamAccumulator::feed(const QString &event, const QByteArray &data, QString *textDelta, QString *thinkingDelta)
{
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    const QString type = o.value(QStringLiteral("type")).toString(event);
    if (type == QLatin1String("error")) {
        throw ProviderError(o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(QStringLiteral("stream error")));
    }
    if (type == QLatin1String("message_start")) {
        m_usage = o.value(QStringLiteral("message")).toObject().value(QStringLiteral("usage")).toObject();
    } else if (type == QLatin1String("content_block_start")) {
        const int index = o.value(QStringLiteral("index")).toInt();
        m_blocks[index].block = o.value(QStringLiteral("content_block")).toObject();
    } else if (type == QLatin1String("content_block_delta")) {
        const int index = o.value(QStringLiteral("index")).toInt();
        Block &b = m_blocks[index];
        const QJsonObject delta = o.value(QStringLiteral("delta")).toObject();
        const QString dt = delta.value(QStringLiteral("type")).toString();
        if (dt == QLatin1String("text_delta")) {
            const QString t = delta.value(QStringLiteral("text")).toString();
            b.block.insert(QStringLiteral("text"), b.block.value(QStringLiteral("text")).toString() + t);
            if (textDelta)
                *textDelta += t;
        } else if (dt == QLatin1String("thinking_delta")) {
            const QString t = delta.value(QStringLiteral("thinking")).toString();
            b.block.insert(QStringLiteral("thinking"), b.block.value(QStringLiteral("thinking")).toString() + t);
            if (thinkingDelta)
                *thinkingDelta += t;
        } else if (dt == QLatin1String("signature_delta")) {
            b.block.insert(QStringLiteral("signature"), b.block.value(QStringLiteral("signature")).toString()
                                                            + delta.value(QStringLiteral("signature")).toString());
        } else if (dt == QLatin1String("input_json_delta")) {
            b.partialJson += delta.value(QStringLiteral("partial_json")).toString();
        }
    } else if (type == QLatin1String("content_block_stop")) {
        const int index = o.value(QStringLiteral("index")).toInt();
        Block &b = m_blocks[index];
        if (b.block.value(QStringLiteral("type")).toString() == QLatin1String("tool_use") && !b.partialJson.isEmpty())
            b.block.insert(QStringLiteral("input"), QJsonDocument::fromJson(b.partialJson.toUtf8()).object());
    } else if (type == QLatin1String("message_delta")) {
        const QJsonObject delta = o.value(QStringLiteral("delta")).toObject();
        if (delta.contains(QStringLiteral("stop_reason")))
            m_stopReason = delta.value(QStringLiteral("stop_reason")).toString();
        if (delta.contains(QStringLiteral("stop_details")))
            m_stopDetails = delta.value(QStringLiteral("stop_details")).toObject();
        const QJsonObject usage = o.value(QStringLiteral("usage")).toObject();
        for (auto it = usage.begin(); it != usage.end(); ++it)
            m_usage.insert(it.key(), it.value());
    }
}

void AnthropicStreamAccumulator::feedMessage(const QJsonObject &message)
{
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    for (qsizetype i = 0; i < content.size(); ++i)
        m_blocks[int(i)].block = content.at(i).toObject();
    m_stopReason = message.value(QStringLiteral("stop_reason")).toString();
    m_stopDetails = message.value(QStringLiteral("stop_details")).toObject();
    m_usage = message.value(QStringLiteral("usage")).toObject();
}

ChatResult AnthropicStreamAccumulator::finish()
{
    ChatResult r;
    for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
        const QJsonObject &b = it->block;
        const QString type = b.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text")) {
            r.text += b.value(QStringLiteral("text")).toString();
        } else if (type == QLatin1String("thinking")) {
            const QString t = b.value(QStringLiteral("thinking")).toString();
            if (!t.isEmpty())
                r.reasoning += (r.reasoning.isEmpty() ? QString() : QStringLiteral("\n")) + t;
        } else if (type == QLatin1String("tool_use")) {
            ToolCall c;
            c.id = b.value(QStringLiteral("id")).toString();
            c.name = b.value(QStringLiteral("name")).toString();
            c.arguments = b.value(QStringLiteral("input")).toObject();
            c.rawArguments = QString::fromUtf8(json::compact(c.arguments));
            r.toolCalls.append(c);
        }
        r.providerContent.append(b);
    }
    r.finishReason = m_stopReason;
    r.usage = m_usage;
    if (!m_stopDetails.isEmpty())
        r.usage.insert(QStringLiteral("refusal_explanation"), m_stopDetails.value(QStringLiteral("explanation")).toString());
    return r;
}

} // namespace lumen::ai
