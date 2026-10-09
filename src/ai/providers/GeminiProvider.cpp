#include "ai/providers/GeminiProvider.h"

#include "ai/ProviderUtil.h"
#include "core/Async.h"
#include "core/ImageOps.h"
#include "core/JsonUtil.h"
#include "net/SseParser.h"

#include <QJsonArray>
#include <cmath>
#include <QJsonDocument>
#include <QPointer>

namespace lumen::ai {

namespace {

const QStringList kGeminiAspects = {QStringLiteral("1:1"), QStringLiteral("2:3"), QStringLiteral("3:2"), QStringLiteral("3:4"),
                                    QStringLiteral("4:3"), QStringLiteral("4:5"), QStringLiteral("5:4"), QStringLiteral("9:16"),
                                    QStringLiteral("16:9"), QStringLiteral("21:9")};

QJsonObject inlineImage(const QImage &image, int maxSide = 1536)
{
    QImage img = img::thumbnail(image, maxSide);
    const bool alpha = img.hasAlphaChannel();
    const char *fmt = alpha ? "PNG" : "JPEG";
    return QJsonObject{{QStringLiteral("inline_data"), QJsonObject{{QStringLiteral("mime_type"), img::mimeForFormat(fmt)},
                                                                   {QStringLiteral("data"), QString::fromLatin1(img::toBase64(img, fmt, alpha ? -1 : 92))}}}};
}

QJsonObject textPart(const QString &t)
{
    return QJsonObject{{QStringLiteral("text"), t}};
}

QString modelPath(const QString &model)
{
    return model.startsWith(QLatin1String("models/")) ? model : QStringLiteral("models/") + model;
}

} // namespace

QNetworkRequest geminiRequest(const Provider *provider, const QString &path)
{
    QString base = provider->text(QStringLiteral("url"));
    while (base.endsWith(u'/'))
        base.chop(1);
    return net::HttpClient::makeRequest(QUrl(base + path), {{"x-goog-api-key", provider->text(QStringLiteral("api_key")).toUtf8()}});
}

QString referenceInstruction(const ImageRequest &r, int firstImageIndex, bool hasInit, bool hasPose)
{
    QStringList lines;
    int index = firstImageIndex;
    if (hasInit) {
        lines << QStringLiteral("Image %1 is the image to edit. Keep everything that the instruction does not ask to change "
                                "(composition, identity, lighting, style).").arg(index);
        ++index;
    }
    if (r.mode == GenMode::Inpaint && !r.mask.isNull()) {
        lines << QStringLiteral("Image %1 is a mask: modify only the white area of the image to edit and keep the black area "
                                "pixel-identical; blend the change seamlessly.").arg(index);
        ++index;
    }
    QList<int> character, style, other;
    for (const ReferenceImage &ref : r.references) {
        if (ref.role == ReferenceRole::Character)
            character << index;
        else if (ref.role == ReferenceRole::Style)
            style << index;
        else
            other << index;
        ++index;
    }
    auto list = [](const QList<int> &ids) {
        QStringList s;
        for (int i : ids)
            s << QString::number(i);
        return s.join(QStringLiteral(", "));
    };
    if (!character.isEmpty())
        lines << QStringLiteral("Image(s) %1 show the character. Reproduce exactly this character: same face, hair style and color, "
                                "eye color, body proportions, outfit, accessories and color palette.").arg(list(character));
    if (!style.isEmpty())
        lines << QStringLiteral("Image(s) %1 define the art style (rendering, line work, shading, palette) to follow.").arg(list(style));
    if (!other.isEmpty())
        lines << QStringLiteral("Image(s) %1 are additional visual references.").arg(list(other));
    if (hasPose)
        lines << QStringLiteral("Image %1 is an OpenPose skeleton (colored stick figure on black). Put the character in exactly this "
                                "body pose, framing and orientation. Do not draw the skeleton itself.").arg(index);
    return lines.join(u'\n');
}

// ---- Chat ---------------------------------------------------------------------------------------

ProviderType GeminiChatProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("gemini_chat");
    t.name = QStringLiteral("Google Gemini (chat)");
    t.description = tr("Gemini models for chat, vision analysis and planning through the Gemini API.");
    t.kind = ProviderKind::Llm;
    t.icon = QStringLiteral("chat");
    t.documentation = QUrl(QStringLiteral("https://ai.google.dev/gemini-api/docs"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Base URL"), F::Url, QStringLiteral("https://generativelanguage.googleapis.com/v1beta"), QString(), {}, 0, 0, true, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), tr("From Google AI Studio."), {}, 0, 0, true},
        {QStringLiteral("model"), tr("Model"), F::Model, QStringLiteral("gemini-2.5-flash"), QString(),
         {QStringLiteral("gemini-2.5-flash"), QStringLiteral("gemini-2.5-pro")}},
        {QStringLiteral("max_tokens"), tr("Max output tokens"), F::Integer, 16384, QString(), {}, 256, 65536, false, true},
        {QStringLiteral("timeout_s"), tr("Timeout (seconds)"), F::Integer, 300, QString(), {}, 10, 3600, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, 4, QString(), {}, 1, 16, false, true},
    };
    return t;
}

GeminiChatProvider::GeminiChatProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : LlmProvider(type, instanceId, http, parent)
{
}

QFuture<QStringList> GeminiChatProvider::listModels()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QStringList>(http()->get(geminiRequest(this, QStringLiteral("/models?pageSize=200")), opts), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : call->json().value(QStringLiteral("models")).toArray()) {
            const QJsonObject m = v.toObject();
            if (json::toStringList(m.value(QStringLiteral("supportedGenerationMethods")).toArray()).contains(QStringLiteral("generateContent")))
                out << m.value(QStringLiteral("name")).toString().mid(7);
        }
        return out;
    });
}

QFuture<QString> GeminiChatProvider::testConnection()
{
    return listModels().then([](QStringList models) { return tr("Connected - %n model(s) available", nullptr, int(models.size())); });
}

LlmCapabilities GeminiChatProvider::capabilities() const
{
    return LlmCapabilities{true, true, true, true};
}

QJsonObject GeminiChatProvider::buildBody(const ChatRequest &r) const
{
    QStringList system;
    QJsonArray contents;
    auto append = [&contents](const QString &role, const QJsonArray &parts) {
        if (!contents.isEmpty() && contents.last().toObject().value(QStringLiteral("role")).toString() == role) {
            QJsonObject last = contents.last().toObject();
            QJsonArray merged = last.value(QStringLiteral("parts")).toArray();
            for (const QJsonValue &p : parts)
                merged.append(p);
            last.insert(QStringLiteral("parts"), merged);
            contents[contents.size() - 1] = last;
        } else {
            contents.append(QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("parts"), parts}});
        }
    };
    for (const ChatMessage &m : r.messages) {
        switch (m.role) {
        case ChatRole::System:
            system << m.text;
            break;
        case ChatRole::User: {
            QJsonArray parts;
            for (const ChatImage &img : m.images) {
                if (!img.note.isEmpty())
                    parts.append(textPart(img.note));
                parts.append(inlineImage(img.image));
            }
            if (!m.text.isEmpty() || parts.isEmpty())
                parts.append(textPart(m.text));
            append(QStringLiteral("user"), parts);
            break;
        }
        case ChatRole::Assistant: {
            if (m.providerType == type().id && !m.providerContent.isEmpty()) {
                append(QStringLiteral("model"), m.providerContent); // keeps thought signatures
                break;
            }
            QJsonArray parts;
            if (!m.text.isEmpty())
                parts.append(textPart(m.text));
            for (const ToolCall &c : m.toolCalls)
                parts.append(QJsonObject{{QStringLiteral("functionCall"), QJsonObject{{QStringLiteral("name"), c.name}, {QStringLiteral("args"), c.arguments}}}});
            if (!parts.isEmpty())
                append(QStringLiteral("model"), parts);
            break;
        }
        case ChatRole::Tool:
            append(QStringLiteral("user"),
                   QJsonArray{QJsonObject{{QStringLiteral("functionResponse"),
                                           QJsonObject{{QStringLiteral("name"), m.toolName},
                                                       {QStringLiteral("response"), QJsonObject{{QStringLiteral("content"), m.text}}}}}}});
            break;
        }
    }
    QJsonObject gen{{QStringLiteral("temperature"), r.temperature},
                    {QStringLiteral("maxOutputTokens"), std::min(r.maxTokens > 0 ? std::max(r.maxTokens, 8192) : 16384,
                                                                 std::max(256, setting(QStringLiteral("max_tokens")).toInt()))}};
    if (!r.jsonSchema.isEmpty()) {
        gen.insert(QStringLiteral("responseMimeType"), QStringLiteral("application/json"));
        gen.insert(QStringLiteral("responseJsonSchema"), r.jsonSchema);
    }
    QJsonObject body{{QStringLiteral("contents"), contents}, {QStringLiteral("generationConfig"), gen}};
    if (!system.isEmpty())
        body.insert(QStringLiteral("systemInstruction"), QJsonObject{{QStringLiteral("parts"), QJsonArray{textPart(system.join(QStringLiteral("\n\n")))}}});
    if (!r.tools.isEmpty()) {
        QJsonArray decls;
        for (const ToolDefinition &t : r.tools)
            decls.append(QJsonObject{{QStringLiteral("name"), t.name},
                                     {QStringLiteral("description"), t.description},
                                     {QStringLiteral("parametersJsonSchema"), t.parameters}});
        body.insert(QStringLiteral("tools"), QJsonArray{QJsonObject{{QStringLiteral("functionDeclarations"), decls}}});
    }
    return body;
}

ChatOperation *GeminiChatProvider::chat(const ChatRequest &r)
{
    auto *op = new ChatOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("Gemini API key is not set. Add it in Settings → Providers."));
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
    const QString model = r.model.isEmpty() ? text(QStringLiteral("model")) : r.model;
    const QString path = modelPath(model) + (r.stream ? QStringLiteral(":streamGenerateContent?alt=sse") : QStringLiteral(":generateContent"));
    QPointer<GeminiChatProvider> self(this);
    net::CallOptions opts;
    opts.timeoutMs = std::max(10, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.streaming = r.stream;
    const QString typeId = type().id;
    struct Acc
    {
        QString text, reasoning, finish;
        QJsonArray parts;
        QList<ToolCall> calls;
        QJsonObject usage;
    };
    auto acc = std::make_shared<Acc>();
    auto consume = [acc, op](const QJsonObject &chunk) {
        if (chunk.contains(QStringLiteral("error")))
            throw ProviderError(chunk.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString());
        if (chunk.contains(QStringLiteral("usageMetadata")))
            acc->usage = chunk.value(QStringLiteral("usageMetadata")).toObject();
        const QJsonObject cand = chunk.value(QStringLiteral("candidates")).toArray().at(0).toObject();
        if (cand.contains(QStringLiteral("finishReason")))
            acc->finish = cand.value(QStringLiteral("finishReason")).toString();
        const QJsonObject feedback = chunk.value(QStringLiteral("promptFeedback")).toObject();
        if (feedback.contains(QStringLiteral("blockReason")))
            throw ProviderError(tr("Gemini blocked the request (%1)").arg(feedback.value(QStringLiteral("blockReason")).toString()));
        for (const QJsonValue &pv : cand.value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray()) {
            const QJsonObject p = pv.toObject();
            acc->parts.append(p);
            if (p.contains(QStringLiteral("functionCall"))) {
                const QJsonObject fc = p.value(QStringLiteral("functionCall")).toObject();
                ToolCall c;
                c.id = fc.value(QStringLiteral("id")).toString(QStringLiteral("call_%1").arg(acc->calls.size()));
                c.name = fc.value(QStringLiteral("name")).toString();
                c.arguments = fc.value(QStringLiteral("args")).toObject();
                c.rawArguments = QString::fromUtf8(json::compact(c.arguments));
                acc->calls.append(c);
            } else if (p.value(QStringLiteral("thought")).toBool()) {
                acc->reasoning += p.value(QStringLiteral("text")).toString();
                op->appendReasoning(p.value(QStringLiteral("text")).toString());
            } else if (p.contains(QStringLiteral("text"))) {
                acc->text += p.value(QStringLiteral("text")).toString();
                op->appendText(p.value(QStringLiteral("text")).toString());
            }
        }
    };
    async::run([self, r] {
        if (!self)
            throw ProviderError(QStringLiteral("provider removed"));
        return json::compact(self->buildBody(r));
    })
        .then(op, [self, op, state, opts, path, consume, acc, typeId, stream = r.stream](QByteArray body) {
            if (!self)
                return;
            if (state->cancelled) {
                op->reportCancelled();
                return;
            }
            net::HttpCall *call = self->http()->postJsonBytes(geminiRequest(self, u'/' + path), body, opts);
            state->call = call;
            auto sse = std::make_shared<net::SseParser>();
            if (stream) {
                QObject::connect(call, &net::HttpCall::chunk, op, [op, sse, consume, state](const QByteArray &data) {
                    try {
                        for (const net::SseEvent &e : sse->feed(data))
                            consume(QJsonDocument::fromJson(e.data).object());
                    } catch (const std::exception &e) {
                        op->fail(describeException(e));
                        if (state->call)
                            state->call->abort();
                    }
                });
            }
            QObject::connect(call, &net::HttpCall::finished, op, [op, call, state, sse, consume, acc, typeId, stream] {
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
                        for (const net::SseEvent &e : sse->finish())
                            consume(QJsonDocument::fromJson(e.data).object());
                    } else {
                        consume(call->json());
                    }
                    ChatResult res;
                    res.text = acc->text;
                    res.reasoning = acc->reasoning;
                    res.toolCalls = acc->calls;
                    res.finishReason = acc->finish;
                    res.usage = acc->usage;
                    res.providerContent = acc->parts;
                    res.providerType = typeId;
                    op->succeed(res);
                } catch (const std::exception &e) {
                    op->fail(describeException(e));
                }
            });
        })
        .onFailed(op, [op](const std::exception &e) { op->fail(describeException(e)); });
    return op;
}

// ---- Image --------------------------------------------------------------------------------------

ProviderType GeminiImageProvider::describe()
{
    ProviderType t;
    t.id = QStringLiteral("gemini_image");
    t.name = QStringLiteral("Google Gemini (image)");
    t.description = tr("Gemini native image models: text-to-image, multi-image character references, pose-guided and instruction edits.");
    t.kind = ProviderKind::Image;
    t.icon = QStringLiteral("image");
    t.documentation = QUrl(QStringLiteral("https://ai.google.dev/gemini-api/docs/image-generation"));
    using F = SettingField::Type;
    t.fields = {
        {QStringLiteral("url"), tr("Base URL"), F::Url, QStringLiteral("https://generativelanguage.googleapis.com/v1beta"), QString(), {}, 0, 0, true, true},
        {QStringLiteral("api_key"), tr("API key"), F::Secret, QString(), tr("From Google AI Studio."), {}, 0, 0, true},
        {QStringLiteral("model"), tr("Model"), F::Model, QStringLiteral("gemini-2.5-flash-image"), QString(),
         {QStringLiteral("gemini-2.5-flash-image")}},
        {QStringLiteral("segment_model"), tr("Segmentation model"), F::Model, QStringLiteral("gemini-2.5-flash"),
         tr("Text model used to segment character parts (Live2D slicing)."),
         {QStringLiteral("gemini-2.5-flash"), QStringLiteral("gemini-2.5-pro")}, 0, 0, false, true},
        {QStringLiteral("timeout_s"), tr("Timeout (seconds)"), F::Integer, 300, QString(), {}, 10, 3600, false, true},
        {QStringLiteral("concurrency"), tr("Parallel requests"), F::Integer, 3, QString(), {}, 1, 16, false, true},
    };
    return t;
}

GeminiImageProvider::GeminiImageProvider(const ProviderType &type, const QString &instanceId, net::HttpClient *http, QObject *parent)
    : ImageProvider(type, instanceId, http, parent)
{
}

QFuture<QString> GeminiImageProvider::testConnection()
{
    return listModels().then([](QStringList models) { return tr("Connected - %n image model(s) available", nullptr, int(models.size())); });
}

QFuture<QStringList> GeminiImageProvider::listModels()
{
    net::CallOptions opts;
    opts.timeoutMs = 15000;
    return futureFromCall<QStringList>(http()->get(geminiRequest(this, QStringLiteral("/models?pageSize=200")), opts), [](net::HttpCall *call) {
        QStringList out;
        for (const QJsonValue &v : call->json().value(QStringLiteral("models")).toArray()) {
            const QString name = v.toObject().value(QStringLiteral("name")).toString().mid(7);
            if (name.contains(QLatin1String("image")))
                out << name;
        }
        return out;
    });
}

ImageCapabilities GeminiImageProvider::capabilities() const
{
    ImageCapabilities c;
    c.textToImage = true;
    c.imageToImage = true;
    c.inpaint = false; // no native mask: Lumen crops + composites, the mask is sent as a hint image
    c.edit = true;
    c.maxReferences = 10;
    c.poseControl = true; // pose map passed as a guidance image
    c.negativePrompt = false;
    c.seed = false;
    c.batch = true;
    c.aspectRatios = kGeminiAspects;
    c.nativePixels = 1024 * 1024;
    return c;
}

QJsonObject GeminiImageProvider::buildBody(const ImageRequest &r, const QStringList &aspectRatios)
{
    QJsonArray parts;
    const bool hasInit = r.mode != GenMode::TextToImage && !r.initImage.isNull();
    const ControlInput *pose = nullptr;
    for (const ControlInput &c : r.controls) {
        if (c.type == ControlType::OpenPose)
            pose = &c;
    }
    const QString instruction = referenceInstruction(r, 1, hasInit, pose != nullptr);
    QString prompt = r.prompt;
    if (!r.negativePrompt.isEmpty())
        prompt += QStringLiteral("\nAvoid: ") + r.negativePrompt;
    if (r.transparentBackground)
        prompt += QStringLiteral("\nUse a plain, uniform background that is easy to remove.");
    parts.append(textPart(instruction.isEmpty() ? prompt : instruction + QStringLiteral("\n\n") + prompt));
    if (hasInit)
        parts.append(inlineImage(r.initImage, 2048));
    if (r.mode == GenMode::Inpaint && !r.mask.isNull())
        parts.append(inlineImage(img::thresholdMask(r.mask, 128).convertToFormat(QImage::Format_RGB32), 2048));
    for (const ReferenceImage &ref : r.references)
        parts.append(inlineImage(ref.image));
    if (pose)
        parts.append(inlineImage(pose->image));
    QJsonObject gen{{QStringLiteral("responseModalities"), QJsonArray{QStringLiteral("TEXT"), QStringLiteral("IMAGE")}},
                    {QStringLiteral("imageConfig"), QJsonObject{{QStringLiteral("aspectRatio"), closestAspectRatio(r.size, aspectRatios)}}}};
    QJsonObject body{{QStringLiteral("contents"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("parts"), parts}}}},
                     {QStringLiteral("generationConfig"), gen}};
    return json::merge(body, r.extras);
}

ImageResult GeminiImageProvider::parseResponse(const QByteArray &body)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    ImageResult result;
    const QJsonObject feedback = o.value(QStringLiteral("promptFeedback")).toObject();
    if (feedback.contains(QStringLiteral("blockReason")))
        throw ProviderError(QObject::tr("Gemini blocked the request (%1)").arg(feedback.value(QStringLiteral("blockReason")).toString()));
    for (const QJsonValue &cv : o.value(QStringLiteral("candidates")).toArray()) {
        const QJsonObject cand = cv.toObject();
        for (const QJsonValue &pv : cand.value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray()) {
            const QJsonObject p = pv.toObject();
            QJsonObject inl = p.value(QStringLiteral("inlineData")).toObject();
            if (inl.isEmpty())
                inl = p.value(QStringLiteral("inline_data")).toObject();
            if (!inl.isEmpty()) {
                GeneratedImage g;
                g.image = decodeBase64Image(inl.value(QStringLiteral("data")).toString());
                if (!g.image.isNull())
                    result.images.append(g);
            } else if (p.contains(QStringLiteral("text"))) {
                result.text += p.value(QStringLiteral("text")).toString();
            }
        }
        if (result.images.isEmpty() && cand.value(QStringLiteral("finishReason")).toString() != QLatin1String("STOP")
            && !cand.value(QStringLiteral("finishReason")).toString().isEmpty())
            throw ProviderError(QObject::tr("Gemini returned no image (%1). %2")
                                    .arg(cand.value(QStringLiteral("finishReason")).toString(), result.text));
    }
    if (result.images.isEmpty())
        throw ProviderError(result.text.isEmpty() ? QObject::tr("Gemini returned no image") : QObject::tr("Gemini returned no image: %1").arg(result.text));
    return result;
}

ImageOperation *GeminiImageProvider::generate(const ImageRequest &r)
{
    auto *op = new ImageOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("Gemini API key is not set. Add it in Settings → Providers."));
        return op;
    }
    const QString path = u'/' + modelPath(r.model.isEmpty() ? text(QStringLiteral("model")) : r.model) + QStringLiteral(":generateContent");
    net::CallOptions opts;
    opts.timeoutMs = std::max(10, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.maxRetries = 1;
    // Gemini returns one image per call; batches are issued as parallel calls and merged.
    const int count = std::max(1, r.batchCount);
    if (count == 1) {
        runImageRequest(op, http(), geminiRequest(this, path), [r] { return Payload{json::compact(buildBody(r, kGeminiAspects))}; },
                        [](const HttpResponse &resp) { return parseResponse(resp.body); }, opts);
        return op;
    }
    struct Batch
    {
        int remaining;
        ImageResult merged;
        QStringList errors;
        QList<QPointer<ImageOperation>> children;
    };
    auto batch = std::make_shared<Batch>();
    batch->remaining = count;
    for (int i = 0; i < count; ++i) {
        auto *child = new ImageOperation(op);
        child->setAutoDelete(false);
        batch->children << child;
        runImageRequest(child, http(), geminiRequest(this, path), [r] { return Payload{json::compact(buildBody(r, kGeminiAspects))}; },
                        [](const HttpResponse &resp) { return parseResponse(resp.body); }, opts);
        connect(child, &Operation::finished, op, [op, child, batch, count] {
            if (child->state() == Operation::State::Succeeded)
                batch->merged.images += child->result().images;
            else if (child->state() == Operation::State::Failed)
                batch->errors << child->errorString();
            op->reportProgress(double(count - batch->remaining + 1) / count, tr("%1 of %2 images").arg(count - batch->remaining + 1).arg(count));
            if (--batch->remaining > 0)
                return;
            if (!batch->merged.images.isEmpty())
                op->succeed(batch->merged);
            else if (!batch->errors.isEmpty())
                op->fail(batch->errors.first());
            else
                op->reportCancelled();
        });
    }
    op->onCancel([batch] {
        for (const auto &c : batch->children) {
            if (c)
                c->cancel();
        }
    });
    return op;
}

// ---- Segmentation -----------------------------------------------------------------------------------

bool GeminiImageProvider::supportsUtility(Utility u) const
{
    return u == Utility::SegmentParts;
}

QJsonObject GeminiImageProvider::buildSegmentationBody(const QImage &image, const QJsonArray &parts)
{
    QStringList names;
    for (const QJsonValue &v : parts) {
        const QJsonObject p = v.toObject();
        const QString tag = p.value(QStringLiteral("tag")).toString();
        const QString prompt = p.value(QStringLiteral("prompt")).toString();
        names << (prompt.isEmpty() || prompt == tag ? tag : QStringLiteral("%1 (%2)").arg(tag, prompt));
    }
    const QString instruction =
        QStringLiteral("This is a character illustration. Give the segmentation masks for these parts of the character: %1. "
                       "Output a JSON list of segmentation masks where each entry contains the 2D bounding box in the key "
                       "\"box_2d\", the segmentation mask in key \"mask\", and the text label in the key \"label\". The label "
                       "must be exactly one of the part names before the parentheses. Return one entry per visible part "
                       "(combine left and right sides into one mask) and skip parts that are not visible.")
            .arg(names.isEmpty() ? QStringLiteral("hair, face, eyes, mouth, body, arms, legs, clothing, accessories")
                                 : names.join(QStringLiteral(", ")));
    return QJsonObject{
        {QStringLiteral("contents"),
         QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("parts"), QJsonArray{inlineImage(image, 1024), textPart(instruction)}}}}},
        {QStringLiteral("generationConfig"),
         QJsonObject{{QStringLiteral("responseMimeType"), QStringLiteral("application/json")},
                     {QStringLiteral("temperature"), 0.2},
                     {QStringLiteral("thinkingConfig"), QJsonObject{{QStringLiteral("thinkingBudget"), 0}}}}}};
}

ImageResult GeminiImageProvider::parseSegmentation(const QByteArray &body, const QSize &canvas)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    QString text;
    for (const QJsonValue &cv : o.value(QStringLiteral("candidates")).toArray()) {
        for (const QJsonValue &pv : cv.toObject().value(QStringLiteral("content")).toObject().value(QStringLiteral("parts")).toArray())
            text += pv.toObject().value(QStringLiteral("text")).toString();
    }
    const QJsonValue parsed = json::extractJson(text);
    const QJsonArray entries = parsed.isArray() ? parsed.toArray() : parsed.toObject().value(QStringLiteral("masks")).toArray();
    ImageResult result;
    int order = 0;
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        const QJsonArray box = e.value(QStringLiteral("box_2d")).toArray();
        const QString label = e.value(QStringLiteral("label")).toString().section(QLatin1Char('('), 0, 0).trimmed();
        if (box.size() != 4 || label.isEmpty())
            continue;
        const int y0 = int(std::floor(box[0].toDouble() / 1000.0 * canvas.height()));
        const int x0 = int(std::floor(box[1].toDouble() / 1000.0 * canvas.width()));
        const int y1 = int(std::ceil(box[2].toDouble() / 1000.0 * canvas.height()));
        const int x1 = int(std::ceil(box[3].toDouble() / 1000.0 * canvas.width()));
        const QRect rect = QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1)).intersected(QRect(QPoint(0, 0), canvas));
        if (rect.isEmpty())
            continue;
        QImage mask = decodeBase64Image(e.value(QStringLiteral("mask")).toString());
        QImage full = img::createMask(canvas, 0);
        if (mask.isNull()) {
            // Box only: still useful as a rough starting point.
            for (int y = rect.top(); y <= rect.bottom(); ++y)
                std::fill(full.scanLine(y) + rect.left(), full.scanLine(y) + rect.right() + 1, uchar(255));
        } else {
            mask = mask.convertToFormat(QImage::Format_Grayscale8).scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            for (int y = 0; y < rect.height(); ++y) {
                const uchar *src = mask.constScanLine(y);
                uchar *dst = full.scanLine(rect.y() + y) + rect.x();
                for (int x = 0; x < rect.width(); ++x)
                    dst[x] = src[x] > 127 ? 255 : 0;
            }
        }
        GeneratedImage g;
        g.image = full;
        g.info = QJsonObject{{QStringLiteral("label"), label}, {QStringLiteral("kind"), QStringLiteral("mask")},
                             {QStringLiteral("order"), order++}};
        result.images.append(g);
    }
    if (result.images.isEmpty())
        throw ProviderError(QObject::tr("Gemini returned no part masks. %1").arg(text.left(200)));
    return result;
}

ImageOperation *GeminiImageProvider::runUtility(Utility utility, const QImage &image, const QJsonObject &params)
{
    if (utility != Utility::SegmentParts)
        return ImageProvider::runUtility(utility, image, params);
    auto *op = new ImageOperation(this);
    if (text(QStringLiteral("api_key")).isEmpty()) {
        failLater(op, tr("Gemini API key is not set. Add it in Settings → Providers."));
        return op;
    }
    QString model = text(QStringLiteral("segment_model"));
    if (model.isEmpty())
        model = QStringLiteral("gemini-2.5-flash");
    const QString path = u'/' + modelPath(model) + QStringLiteral(":generateContent");
    net::CallOptions opts;
    opts.timeoutMs = std::max(10, setting(QStringLiteral("timeout_s")).toInt()) * 1000;
    opts.maxRetries = 1;
    const QJsonArray parts = params.value(QStringLiteral("parts")).toArray();
    const QSize canvas = image.size();
    op->reportProgress(-1.0, tr("Segmenting parts with Gemini..."));
    runImageRequest(op, http(), geminiRequest(this, path),
                    [image, parts] { return Payload{json::compact(buildSegmentationBody(image, parts))}; },
                    [canvas](const HttpResponse &resp) { return parseSegmentation(resp.body, canvas); }, opts);
    return op;
}

} // namespace lumen::ai
