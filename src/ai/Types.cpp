#include "ai/Types.h"

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
    }
    return {};
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
