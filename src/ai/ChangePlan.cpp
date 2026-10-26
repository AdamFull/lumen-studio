#include "ai/ChangePlan.h"

#include "ai/Provider.h"
#include "ai/ProviderRegistry.h"
#include "core/ImageOps.h"

#include <QJsonArray>

namespace lumen::ai {

QJsonObject changePlanSchema()
{
    // Length limits keep small local models from looping inside a string (grammar-constrained servers enforce them).
    auto string = [](const char *desc, int maxLength) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), QLatin1String(desc)},
                           {QStringLiteral("maxLength"), maxLength}};
    };
    const QJsonObject properties{
        {QStringLiteral("select"), string("what to select before the change, in a few words, or empty", 120)},
        {QStringLiteral("prompt"), string("the request for the image model", 600)},
        {QStringLiteral("negative"), string("comma-separated things to leave out, or empty", 300)},
        {QStringLiteral("loras"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                                              {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                                                    {QStringLiteral("maxLength"), 200}}},
                                              {QStringLiteral("maxItems"), 3},
                                              {QStringLiteral("description"), QStringLiteral("names from the offered LoRAs, usually none")}}},
        {QStringLiteral("note"), string("the plan in one short sentence for the user", 240)},
    };
    QJsonArray required;
    for (auto it = properties.begin(); it != properties.end(); ++it)
        required.append(it.key());
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("required"), required},
                       {QStringLiteral("additionalProperties"), false}};
}

ChatRequest changePlanRequest(const QImage &picture, const PlanContext &context)
{
    ChatRequest req;
    req.temperature = 0.2;
    req.maxTokens = 1200;
    req.jsonSchema = changePlanSchema();
    req.schemaName = QStringLiteral("change_plan");
    QString model;
    if (context.instructions) {
        model = QStringLiteral("The image model edits a picture from an instruction (like FLUX.2 klein or Kontext): write the "
                               "request as one clear English instruction that says what to change and that everything else stays "
                               "as it is.");
    } else {
        model = QStringLiteral("The image model repaints the selected area from a description (inpainting): write the request as "
                               "a description of what should be there after the change, never as an instruction. For example, "
                               "\"remove the cup\" becomes \"empty wooden table\" and \"replace the cup with a candle\" becomes "
                               "\"a lit candle\".");
        model += context.style.dialect == PromptStyle::Dialect::Tags
                     ? QStringLiteral(" Write it as comma-separated tags in lower case (the model's family: %1). Do not add quality "
                                      "tags.")
                           .arg(context.style.family)
                     : QStringLiteral(" Write it as a short plain English phrase or sentence.");
    }
    QString select;
    if (context.hasSelection)
        select = QStringLiteral("The user has already selected the area to change: leave \"select\" empty unless the words ask "
                                "for something else.");
    else
        select = QStringLiteral("Nothing is selected. When the change is to a part of the picture, say in \"select\" what to "
                                "select, as the editor's finder understands it: thin lines or scratches (\"the black lines\"), "
                                "dust or specks, a colour (\"the red\"), or an object by name (\"the cup\", \"the man on the "
                                "left\"). Leave it empty when the change is to the whole picture.");
    const QString loras = context.loras.isEmpty()
                              ? QStringLiteral("No LoRAs are offered: \"loras\" stays empty.")
                              : QStringLiteral("LoRAs the image model can use: %1. Name one in \"loras\" only when it clearly fits "
                                               "the change (a style or character the user asks for); usually none.")
                                    .arg(context.loras.join(QStringLiteral(", ")));
    const QString negative = context.negative ? QStringLiteral("\"negative\": things to leave out of the result, a few terms.")
                                              : QStringLiteral("The image model takes no negative prompt: \"negative\" stays empty.");
    req.messages << ChatMessage::system(QStringLiteral("You plan a change to a picture for an image editor. Look at the picture and "
                                                       "the user's words, then answer with the plan.\n- %1\n- %2\n- %3\n- %4\n- \"note\": "
                                                       "the plan in one short sentence, in the user's language.")
                                            .arg(select, model, negative, loras));
    ChatMessage user = ChatMessage::user(QStringLiteral("The change: %1").arg(context.words));
    user.images.append(ChatImage{img::thumbnail(picture, 1024), QStringLiteral("The picture:")});
    req.messages << user;
    return req;
}

ChangePlan parseChangePlan(const QJsonObject &answer, const PlanContext &context)
{
    ChangePlan plan;
    plan.select = answer.value(QStringLiteral("select")).toString().simplified();
    plan.prompt = answer.value(QStringLiteral("prompt")).toString().trimmed();
    if (plan.prompt.isEmpty())
        plan.prompt = context.words;
    if (context.negative)
        plan.negative = answer.value(QStringLiteral("negative")).toString().trimmed();
    for (const QJsonValue &v : answer.value(QStringLiteral("loras")).toArray()) {
        const QString name = v.toString().trimmed();
        for (const QString &offered : context.loras) {
            if (offered.compare(name, Qt::CaseInsensitive) == 0 && !plan.loras.contains(offered))
                plan.loras << offered;
        }
    }
    plan.note = answer.value(QStringLiteral("note")).toString().simplified();
    return plan;
}

LlmProvider *changePlanner(const ProviderRegistry *registry, QString *model)
{
    if (!registry)
        return nullptr;
    LlmProvider *provider = registry->llmFor(LlmRole::Vision, model);
    return provider && provider->capabilities().vision ? provider : nullptr;
}

} // namespace lumen::ai
