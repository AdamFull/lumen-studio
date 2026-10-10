#include "ai/Types.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace lumen::ai {

QString genModeName(GenMode m)
{
    switch (m) {
    case GenMode::TextToImage: return QStringLiteral("txt2img");
    case GenMode::ImageToImage: return QStringLiteral("img2img");
    case GenMode::Inpaint: return QStringLiteral("inpaint");
    case GenMode::Edit: return QStringLiteral("edit");
    }
    return QStringLiteral("txt2img");
}

GenMode genModeFromName(const QString &name)
{
    if (name == QLatin1String("img2img"))
        return GenMode::ImageToImage;
    if (name == QLatin1String("inpaint"))
        return GenMode::Inpaint;
    if (name == QLatin1String("edit"))
        return GenMode::Edit;
    return GenMode::TextToImage;
}

QString referenceRoleName(ReferenceRole r)
{
    switch (r) {
    case ReferenceRole::Character: return QStringLiteral("character");
    case ReferenceRole::Style: return QStringLiteral("style");
    case ReferenceRole::Composition: return QStringLiteral("composition");
    case ReferenceRole::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

ReferenceRole referenceRoleFromName(const QString &name)
{
    if (name == QLatin1String("character"))
        return ReferenceRole::Character;
    if (name == QLatin1String("style"))
        return ReferenceRole::Style;
    if (name == QLatin1String("composition"))
        return ReferenceRole::Composition;
    return ReferenceRole::Other;
}

QString controlTypeName(ControlType t)
{
    switch (t) {
    case ControlType::OpenPose: return QStringLiteral("openpose");
    case ControlType::Depth: return QStringLiteral("depth");
    case ControlType::Canny: return QStringLiteral("canny");
    case ControlType::Lineart: return QStringLiteral("lineart");
    case ControlType::Scribble: return QStringLiteral("scribble");
    }
    return QStringLiteral("openpose");
}

QString utilityName(Utility u)
{
    switch (u) {
    case Utility::Upscale: return QStringLiteral("upscale");
    case Utility::RemoveBackground: return QStringLiteral("remove_background");
    case Utility::DetectPose: return QStringLiteral("detect_pose");
    case Utility::Caption: return QStringLiteral("caption");
    case Utility::SegmentParts: return QStringLiteral("segment_parts");
    }
    return {};
}

QJsonObject LoraRef::toJson() const
{
    QJsonObject o{{QStringLiteral("name"), name}, {QStringLiteral("weight"), weight}};
    if (!file.isEmpty())
        o.insert(QStringLiteral("file"), file);
    return o;
}

LoraRef LoraRef::fromJson(const QJsonObject &o)
{
    LoraRef l;
    l.name = o.value(QStringLiteral("name")).toString();
    l.file = o.value(QStringLiteral("file")).toString();
    l.weight = o.value(QStringLiteral("weight")).toDouble(0.8);
    if (l.name.isEmpty() && !l.file.isEmpty())
        l.name = QFileInfo(l.file).completeBaseName();
    return l;
}

bool resolveLoras(QList<LoraRef> &loras, const QList<ServerLora> &available, QString *error)
{
    for (LoraRef &l : loras) {
        const QString file = QDir::fromNativeSeparators(l.file);
        const QString stem = l.name.isEmpty() ? QFileInfo(file).completeBaseName() : l.name;
        const ServerLora *best = nullptr;
        for (const ServerLora &s : available) {
            const QString path = QDir::fromNativeSeparators(s.path);
            if (!file.isEmpty() && !path.isEmpty() && (file.compare(path, Qt::CaseInsensitive) == 0 || file.endsWith(QLatin1Char('/') + path, Qt::CaseInsensitive))
                && (!best || path.size() > best->path.size()))
                best = &s;
        }
        if (!best) {
            for (const ServerLora &s : available) {
                const QString name = s.name.isEmpty() ? QFileInfo(s.path).completeBaseName() : s.name;
                if (name.compare(stem, Qt::CaseInsensitive) == 0) {
                    best = &s;
                    break;
                }
            }
        }
        if (!best) {
            if (error)
                *error = QCoreApplication::translate("lumen::ai::Types", "The server has no LoRA \"%1\". Put the file into the server's LoRA folder.")
                             .arg(stem);
            return false;
        }
        l.resolved = best->path.isEmpty() ? best->name : QDir::fromNativeSeparators(best->path);
    }
    return true;
}

LoraRef CharacterLora::ref() const
{
    LoraRef l;
    l.file = file;
    l.name = QFileInfo(file).completeBaseName();
    l.weight = weight;
    return l;
}

QJsonObject CharacterLora::toJson() const
{
    return QJsonObject{{QStringLiteral("file"), file},
                       {QStringLiteral("trigger"), trigger},
                       {QStringLiteral("weight"), weight},
                       {QStringLiteral("family"), family}};
}

CharacterLora CharacterLora::fromJson(const QJsonObject &o)
{
    CharacterLora c;
    c.file = o.value(QStringLiteral("file")).toString();
    c.trigger = o.value(QStringLiteral("trigger")).toString();
    c.weight = o.value(QStringLiteral("weight")).toDouble(0.8);
    c.family = o.value(QStringLiteral("family")).toString();
    return c;
}

QJsonObject ImageRequest::describe() const
{
    QJsonObject o{{QStringLiteral("mode"), genModeName(mode)},
                  {QStringLiteral("prompt"), prompt},
                  {QStringLiteral("negative_prompt"), negativePrompt},
                  {QStringLiteral("width"), size.width()},
                  {QStringLiteral("height"), size.height()},
                  {QStringLiteral("seed"), double(seed)},
                  {QStringLiteral("steps"), steps},
                  {QStringLiteral("cfg"), cfg},
                  {QStringLiteral("batch"), batchCount}};
    if (!sampler.isEmpty())
        o.insert(QStringLiteral("sampler"), sampler);
    if (!scheduler.isEmpty())
        o.insert(QStringLiteral("scheduler"), scheduler);
    if (!model.isEmpty())
        o.insert(QStringLiteral("model"), model);
    if (mode != GenMode::TextToImage)
        o.insert(QStringLiteral("denoise"), denoise);
    if (!quality.isEmpty())
        o.insert(QStringLiteral("quality"), quality);
    if (!references.isEmpty()) {
        QJsonArray refs;
        for (const ReferenceImage &r : references)
            refs.append(QJsonObject{{QStringLiteral("asset"), r.assetId},
                                    {QStringLiteral("role"), referenceRoleName(r.role)},
                                    {QStringLiteral("weight"), r.weight}});
        o.insert(QStringLiteral("references"), refs);
    }
    if (!controls.isEmpty()) {
        QJsonArray ctl;
        for (const ControlInput &c : controls)
            ctl.append(QJsonObject{{QStringLiteral("type"), controlTypeName(c.type)}, {QStringLiteral("weight"), c.weight}});
        o.insert(QStringLiteral("controls"), ctl);
    }
    if (!loras.isEmpty()) {
        QJsonArray list;
        for (const LoraRef &l : loras)
            list.append(l.toJson());
        o.insert(QStringLiteral("loras"), list);
    }
    if (!extras.isEmpty())
        o.insert(QStringLiteral("extras"), extras);
    return o;
}

ChatMessage ChatMessage::system(const QString &text)
{
    ChatMessage m;
    m.role = ChatRole::System;
    m.text = text;
    return m;
}

ChatMessage ChatMessage::user(const QString &text, const QList<QImage> &images)
{
    ChatMessage m;
    m.role = ChatRole::User;
    m.text = text;
    for (const QImage &img : images)
        m.images.append(ChatImage{img, {}});
    return m;
}

ChatMessage ChatMessage::assistant(const QString &text)
{
    ChatMessage m;
    m.role = ChatRole::Assistant;
    m.text = text;
    return m;
}

ChatMessage ChatMessage::toolResult(const QString &callId, const QString &name, const QString &content)
{
    ChatMessage m;
    m.role = ChatRole::Tool;
    m.toolCallId = callId;
    m.toolName = name;
    m.text = content;
    return m;
}

ChatMessage ChatMessage::fromResult(const ChatResult &result)
{
    ChatMessage m;
    m.role = ChatRole::Assistant;
    m.text = result.text;
    m.toolCalls = result.toolCalls;
    m.providerContent = result.providerContent;
    m.providerType = result.providerType;
    return m;
}

} // namespace lumen::ai
