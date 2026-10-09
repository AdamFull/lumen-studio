#include "ai/providers/OpenAiChatProvider.h"

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

QString flavorId(OpenAiChatProvider::Flavor f)
{
    switch (f) {
    case OpenAiChatProvider::Flavor::OpenAI: return QStringLiteral("openai_chat");
    case OpenAiChatProvider::Flavor::LlamaCpp: return QStringLiteral("llamacpp");
    case OpenAiChatProvider::Flavor::Ollama: return QStringLiteral("ollama");
    case OpenAiChatProvider::Flavor::LmStudio: return QStringLiteral("lmstudio");
    case OpenAiChatProvider::Flavor::KoboldCpp: return QStringLiteral("koboldcpp");
    case OpenAiChatProvider::Flavor::OpenRouter: return QStringLiteral("openrouter");
    case OpenAiChatProvider::Flavor::Generic: return QStringLiteral("openai_compatible");
    }
    return {};
}

OpenAiChatProvider::Flavor flavorFromId(const QString &id)
{
    for (auto f : {OpenAiChatProvider::Flavor::OpenAI, OpenAiChatProvider::Flavor::LlamaCpp, OpenAiChatProvider::Flavor::Ollama,
                   OpenAiChatProvider::Flavor::LmStudio, OpenAiChatProvider::Flavor::KoboldCpp,
                   OpenAiChatProvider::Flavor::OpenRouter, OpenAiChatProvider::Flavor::Generic}) {
        if (flavorId(f) == id)
            return f;
    }
    return OpenAiChatProvider::Flavor::Generic;
}

// Vision models resize inputs anyway; sending at most ~1.5 MP keeps requests fast and cheap.
QString imageDataUrl(const QImage &image)
{
    QImage img = img::thumbnail(image, 1568);
    if (img.hasAlphaChannel())
        img = img::flatten(img, Qt::white);
    return img::toDataUrl(img, "JPEG", 90);
}

QString roleName(ChatRole r)
{
    switch (r) {
    case ChatRole::System: return QStringLiteral("system");
    case ChatRole::User: return QStringLiteral("user");
    case ChatRole::Assistant: return QStringLiteral("assistant");
    case ChatRole::Tool: return QStringLiteral("tool");
    }
    return QStringLiteral("user");
}

} // namespace

QList<ProviderType> OpenAiChatProvider::describeAll()
{
    return {describe(Flavor::LlamaCpp), describe(Flavor::Ollama), describe(Flavor::LmStudio), describe(Flavor::KoboldCpp),
            describe(Flavor::OpenAI), describe(Flavor::OpenRouter), describe(Flavor::Generic)};
}

ProviderType OpenAiChatProvider::describe(Flavor flavor)
{
    ProviderType t;
    t.id = flavorId(flavor);
    t.kind = ProviderKind::Llm;
    t.icon = QStringLiteral("chat");
    QString url;
    bool needsKey = false;
    QString defaultModel;
    switch (flavor) {
    case Flavor::LlamaCpp:
        t.name = QStringLiteral("llama.cpp server");
        t.description = tr("Local llama-server (OpenAI-compatible). Start it with --jinja for tool calling and an mmproj file for vision.");
        url = QStringLiteral("http://127.0.0.1:8080/v1");
        t.local = true;
        t.documentation = QUrl(QStringLiteral("https://github.com/ggml-org/llama.cpp/tree/master/tools/server"));
        break;
    case Flavor::Ollama:
        t.name = QStringLiteral("Ollama");
        t.description = tr("Local Ollama server through its OpenAI-compatible endpoint. Use a vision model (e.g. qwen2.5vl, gemma3) for reference analysis.");
        url = QStringLiteral("http://127.0.0.1:11434/v1");
        t.local = true;
        t.documentation = QUrl(QStringLiteral("https://github.com/ollama/ollama/blob/main/docs/openai.md"));
        break;
    case Flavor::LmStudio:
        t.name = QStringLiteral("LM Studio");
        t.description = tr("Local LM Studio server (Developer tab → Start server).");
        url = QStringLiteral("http://127.0.0.1:1234/v1");
        t.local = true;
        break;
    case Flavor::KoboldCpp:
        t.name = QStringLiteral("KoboldCpp");
        t.description = tr("Local KoboldCpp through its OpenAI-compatible API.");
        url = QStringLiteral("http://127.0.0.1:5001/v1");
        t.local = true;
        break;
    case Flavor::OpenAI:
        t.name = QStringLiteral("OpenAI (chat)");
        t.description = tr("OpenAI GPT models for chat, vision analysis and planning.");
        url = QStringLiteral("https://api.openai.com/v1");
        needsKey = true;
        defaultModel = QStringLiteral("gpt-4.1");
        t.documentation = QUrl(QStringLiteral("https://platform.openai.com/docs/api-reference/chat"));
        break;
    case Flavor::OpenRouter:
        t.name = QStringLiteral("OpenRouter");
        t.description = tr("Hundreds of hosted models behind one OpenAI-compatible API.");
        url = QStringLiteral("https://openrouter.ai/api/v1");
        needsKey = true;
        t.documentation = QUrl(QStringLiteral("https://openrouter.ai/docs"));
        break;
    case Flavor::Generic:
        t.name = QStringLiteral("OpenAI-compatible server");
        t.description = tr("Any server implementing the OpenAI Chat Completions API (vLLM, TGI, LocalAI, Groq, DeepSeek...).");
        url = QStringLiteral("http://127.0.0.1:8000/v1");
        break;
    }
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Base URL"), F::Url, url, tr("Including the /v1 suffix."), {}, 0, 0, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), needsKey ? tr("Required.") : tr("Optional for local servers."), {}, 0, 0, needsKey},
        {QStringLiteral("model"), tr("Model"), F::Model, defaultModel, tr("Model identifier. Local single-model servers may ignore it.")},
        {QStringLiteral("vision"), tr("Supports images"), F::Choice, QStringLiteral("auto"),
         tr("Whether the model accepts image input (needed for reference analysis)."), {QStringLiteral("auto"), QStringLiteral("yes"), QStringLiteral("no")}},
        {QStringLiteral("structured"), tr("Structured output"), F::Choice, QStringLiteral("auto"),
         tr("How JSON answers are enforced: json_schema (grammar/strict), json_object, or prompt only."),
         {QStringLiteral("auto"), QStringLiteral("json_schema"), QStringLiteral("json_object"), QStringLiteral("prompt")}, 0, 0, false, true},
        {QStringLiteral("tools"), tr("Tool calling"), F::Boolean, true, tr("Disable for models/servers without function calling."), {}, 0, 0, false, true},
        {QStringLiteral("max_tokens"), tr("Max output tokens"), F::Integer, 4096, QString(), {}, 256, 200000, false, true},
        {QStringLiteral("timeout_s"), tr("Timeout (seconds)"), F::Integer, t.local ? 600 : 180, tr("Maximum silence before the request is aborted."), {}, 10, 3600, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, t.local ? 1 : 4, QString(), {}, 1, 16, false, true},
    };
    return t;
}

OpenAiChatProvider::OpenAiChatProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : LlmProvider(type, instanceId, http, parent)
    , m_flavor(flavorFromId(type.id))
{
}

QNetworkRequest OpenAiChatProvider::request(const QString &path) const
{
    QNetworkRequest req = net::HttpClient::makeRequest(endpoint(path));
    const QString key = text(QStringLiteral("api_key"));
    if (!key.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    if (m_flavor == Flavor::OpenRouter) {
        req.setRawHeader("HTTP-Referer", "https://lumen.studio");
        req.setRawHeader("X-Title", "Lumen Studio");
    }
    return req;
}

QFuture<QString> OpenAiChatProvider::testConnection()
{
    return listModels().then([](QStringList models) {
        if (models.isEmpty())
            return tr("Connected");
        return tr("Connected - %n model(s) available", nullptr, int(models.size()));
    });
}

QFuture<QStringList> OpenAiChatProvider::listModels()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    opts.maxRetries = 1;
    return futureFromCall<QStringList>(http()->get(request(QStringLiteral("/models")), opts), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : call->json().value(QStringLiteral("data")).toArray())
            out << v.toObject().value(QStringLiteral("id")).toString();
        out.sort(Qt::CaseInsensitive);
        return out;
    });
}

LlmCapabilities OpenAiChatProvider::capabilities() const
{
    LlmCapabilities c;
    const QString vision = text(QStringLiteral("vision"));
    c.vision = vision != QLatin1String("no");
    c.tools = setting(QStringLiteral("tools")).toBool();
    c.jsonSchema = structuredMode() != QLatin1String("prompt");
    c.streaming = true;
    return c;
}

QString OpenAiChatProvider::structuredMode() const
{
    const QString mode = text(QStringLiteral("structured"));
    if (mode.isEmpty() || mode == QLatin1String("auto")) {
        // KoboldCpp only understands json_object reliably; everything else accepts json_schema.
        return m_flavor == Flavor::KoboldCpp ? QStringLiteral("json_object") : QStringLiteral("json_schema");
    }
    return mode;
}

QJsonObject OpenAiChatProvider::buildBody(const ChatRequest &r) const
{
    QJsonArray messages;
    const bool vision = capabilities().vision;
    const QString mode = structuredMode();
    for (const ChatMessage &m : r.messages) {
        QJsonObject msg{{QStringLiteral("role"), roleName(m.role)}};
        if (m.role == ChatRole::Tool) {
            msg.insert(QStringLiteral("tool_call_id"), m.toolCallId);
            msg.insert(QStringLiteral("content"), m.text);
        } else if (!m.images.isEmpty() && vision) {
            QJsonArray parts;
            if (!m.text.isEmpty())
                parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), m.text}});
            for (const ChatImage &img : m.images) {
                if (!img.note.isEmpty())
                    parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), img.note}});
                parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                                         {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"), imageDataUrl(img.image)}}}});
            }
            msg.insert(QStringLiteral("content"), parts);
        } else {
            QString text = m.text;
            if (!m.images.isEmpty())
                text += tr("\n[%n image(s) attached but this model cannot see images]", nullptr, int(m.images.size()));
            msg.insert(QStringLiteral("content"), text);
        }
        if (!m.toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const ToolCall &c : m.toolCalls) {
                calls.append(QJsonObject{{QStringLiteral("id"), c.id},
                                         {QStringLiteral("type"), QStringLiteral("function")},
                                         {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), c.name},
                                                                                  {QStringLiteral("arguments"), QString::fromUtf8(json::compact(c.arguments))}}}});
            }
            msg.insert(QStringLiteral("tool_calls"), calls);
            if (m.text.isEmpty())
                msg.insert(QStringLiteral("content"), QJsonValue::Null);
        }
        messages.append(msg);
    }

    // Prompt-level schema guidance for modes without hard enforcement.
    if (!r.jsonSchema.isEmpty() && mode != QLatin1String("json_schema")) {
        const QString guide = QStringLiteral("Respond with a single JSON object (no prose, no code fences) that validates against this JSON schema:\n%1")
                                  .arg(QString::fromUtf8(json::compact(r.jsonSchema)));
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), guide}});
    }

    const QString model = r.model.isEmpty() ? text(QStringLiteral("model")) : r.model;
    QJsonObject body{{QStringLiteral("messages"), messages}, {QStringLiteral("stream"), r.stream}};
    if (!model.isEmpty())
        body.insert(QStringLiteral("model"), model);
    body.insert(QStringLiteral("temperature"), r.temperature);
    const int maxTokens = std::min(r.maxTokens > 0 ? r.maxTokens : 4096, std::max(256, setting(QStringLiteral("max_tokens")).toInt()));
    body.insert(m_flavor == Flavor::OpenAI ? QStringLiteral("max_completion_tokens") : QStringLiteral("max_tokens"), maxTokens);
    if (r.stream && (m_flavor == Flavor::OpenAI || m_flavor == Flavor::OpenRouter))
        body.insert(QStringLiteral("stream_options"), QJsonObject{{QStringLiteral("include_usage"), true}});

    if (!r.jsonSchema.isEmpty()) {
        if (mode == QLatin1String("json_schema")) {
            const bool strict = m_flavor == Flavor::OpenAI;
            body.insert(QStringLiteral("response_format"),
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                    {QStringLiteral("json_schema"),
                                     QJsonObject{{QStringLiteral("name"), r.schemaName},
                                                 {QStringLiteral("strict"), strict},
                                                 {QStringLiteral("schema"), strict ? json::toStrictSchema(r.jsonSchema) : r.jsonSchema}}}});
        } else if (mode == QLatin1String("json_object")) {
            body.insert(QStringLiteral("response_format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}});
        }
    }
    if (!r.tools.isEmpty() && capabilities().tools) {
        QJsonArray tools;
        for (const ToolDefinition &t : r.tools) {
            tools.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                                     {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), t.name},
                                                                              {QStringLiteral("description"), t.description},
                                                                              {QStringLiteral("parameters"), t.parameters}}}});
        }
        body.insert(QStringLiteral("tools"), tools);
    }
    return body;
}

ChatOperation *OpenAiChatProvider::chat(const ChatRequest &r)
{
    auto *op = new ChatOperation(this);
    auto state = std::make_shared<RequestState>();
    op->onCancel([state, op] {
        state->cancelled = true;
        if (state->call)
            state->call->abort();
        else
            op->reportCancelled();
    });
    QPointer<OpenAiChatProvider> self(this);
    net::CallOptions opts;
    opts.timeoutMs = std::max(10, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.streaming = r.stream;
    opts.maxRetries = 2;
    // Body building encodes images: do it off the GUI thread.
    async::run([self, r] {
        if (!self)
            throw ProviderError(QStringLiteral("provider removed"));
        return json::compact(self->buildBody(r));
    })
        .then(op, [self, op, state, opts, stream = r.stream](QByteArray body) {
            if (!self)
                return;
            if (state->cancelled) {
                op->reportCancelled();
                return;
            }
            net::HttpCall *call = self->http()->postJsonBytes(self->request(QStringLiteral("/chat/completions")), body, opts);
            state->call = call;
            auto sse = std::make_shared<net::SseParser>();
            auto acc = std::make_shared<OpenAiStreamAccumulator>();
            if (stream) {
                QObject::connect(call, &net::HttpCall::chunk, op, [op, sse, acc, state](const QByteArray &data) {
                    try {
                        for (const net::SseEvent &e : sse->feed(data)) {
                            QString text, reasoning;
                            acc->feed(e.data, &text, &reasoning);
                            op->appendText(text);
                            op->appendReasoning(reasoning);
                        }
                    } catch (const std::exception &e) {
                        op->fail(describeException(e));
                        if (state->call)
                            state->call->abort();
                    }
                });
            }
            QObject::connect(call, &net::HttpCall::finished, op, [op, call, state, sse, acc, stream] {
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
                if (stream) {
                    for (const net::SseEvent &e : sse->finish()) {
                        QString text, reasoning;
                        acc->feed(e.data, &text, &reasoning);
                        op->appendText(text);
                    }
                    // Servers that ignore stream=true reply with a single JSON body.
                    if (!acc->isDone() && op->partialText().isEmpty() && call->body().trimmed().startsWith('{')) {
                        QString t, rs;
                        acc->feed(call->body(), &t, &rs);
                    }
                    op->succeed(acc->finish());
                    return;
                }
                QString t, rs;
                acc->feed(call->body(), &t, &rs);
                op->succeed(acc->finish());
                } catch (const std::exception &e) {
                    op->fail(describeException(e));
                }
            });
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    return op;
}

// ---- stream accumulator -------------------------------------------------------------------------

void OpenAiStreamAccumulator::feed(const QByteArray &data, QString *textDelta, QString *reasoningDelta)
{
    const QByteArray trimmed = data.trimmed();
    if (trimmed == "[DONE]") {
        m_done = true;
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(trimmed).object();
    if (o.isEmpty())
        return;
    if (o.contains(QStringLiteral("error"))) {
        const QJsonValue err = o.value(QStringLiteral("error"));
        throw ProviderError(err.isObject() ? err.toObject().value(QStringLiteral("message")).toString() : err.toString());
    }
    if (o.contains(QStringLiteral("usage")) && o.value(QStringLiteral("usage")).isObject())
        m_usage = o.value(QStringLiteral("usage")).toObject();
    const QJsonArray choices = o.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject choice = choices.at(0).toObject();
    // Streaming chunks carry "delta"; complete responses carry "message".
    const QJsonObject delta = choice.contains(QStringLiteral("delta")) ? choice.value(QStringLiteral("delta")).toObject()
                                                                       : choice.value(QStringLiteral("message")).toObject();
    const QString text = delta.value(QStringLiteral("content")).toString();
    if (!text.isEmpty()) {
        m_text += text;
        if (textDelta)
            *textDelta += text;
    }
    QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString();
    if (reasoning.isEmpty())
        reasoning = delta.value(QStringLiteral("reasoning")).toString();
    if (!reasoning.isEmpty()) {
        m_reasoning += reasoning;
        if (reasoningDelta)
            *reasoningDelta += reasoning;
    }
    const QJsonArray calls = delta.value(QStringLiteral("tool_calls")).toArray();
    for (qsizetype i = 0; i < calls.size(); ++i) {
        const QJsonObject c = calls.at(i).toObject();
        const int index = c.contains(QStringLiteral("index")) ? c.value(QStringLiteral("index")).toInt() : int(i);
        PendingCall &pc = m_calls[index];
        if (c.contains(QStringLiteral("id")) && !c.value(QStringLiteral("id")).toString().isEmpty())
            pc.id = c.value(QStringLiteral("id")).toString();
        const QJsonObject fn = c.value(QStringLiteral("function")).toObject();
        if (fn.contains(QStringLiteral("name")) && !fn.value(QStringLiteral("name")).toString().isEmpty())
            pc.name = fn.value(QStringLiteral("name")).toString();
        const QJsonValue args = fn.value(QStringLiteral("arguments"));
        if (args.isString())
            pc.args += args.toString();
        else if (args.isObject())
            pc.args = QString::fromUtf8(json::compact(args.toObject()));
    }
    const QString finish = choice.value(QStringLiteral("finish_reason")).toString();
    if (!finish.isEmpty())
        m_finish = finish;
}

ChatResult OpenAiStreamAccumulator::finish()
{
    ChatResult r;
    r.text = m_text;
    r.reasoning = m_reasoning;
    splitThinking(r.text, r.reasoning);
    r.finishReason = m_finish;
    r.usage = m_usage;
    int n = 0;
    for (auto it = m_calls.begin(); it != m_calls.end(); ++it) {
        ToolCall c;
        c.id = it->id.isEmpty() ? QStringLiteral("call_%1").arg(n) : it->id;
        c.name = it->name;
        c.rawArguments = it->args;
        const QJsonValue parsed = json::extractJson(it->args);
        c.arguments = parsed.toObject();
        r.toolCalls.append(c);
        ++n;
    }
    return r;
}

void splitThinking(QString &text, QString &reasoning)
{
    const qsizetype open = text.indexOf(QStringLiteral("<think>"));
    const qsizetype close = text.indexOf(QStringLiteral("</think>"));
    if (close < 0)
        return;
    const qsizetype start = open >= 0 && open < close ? open + 7 : 0;
    const QString thought = text.mid(start, close - start).trimmed();
    if (!thought.isEmpty())
        reasoning += (reasoning.isEmpty() ? QString() : QStringLiteral("\n")) + thought;
    text = (open >= 0 && open < close ? text.left(open) : QString()) + text.mid(close + 8);
    text = text.trimmed();
}

} // namespace lumen::ai
