#include "ai/CharacterContext.h"

#include "ai/Director.h"
#include "ai/PromptStyle.h"

#include <QCoreApplication>

namespace lumen::ai {

namespace {

struct Text
{
    Q_DECLARE_TR_FUNCTIONS(lumen::ai::CharacterContext)
};

bool sameFamily(const QString &a, const QString &b)
{
    return a.isEmpty() || b.isEmpty() || a.compare(b, Qt::CaseInsensitive) == 0;
}

// The appearance in the prompt's dialect: booru tags, or a sentence for natural-language models.
QString description(const QJsonObject &sheet, const PromptStyle &style)
{
    const QString tags = Director::appearanceTags(sheet);
    if (style.dialect == PromptStyle::Dialect::Tags)
        return tags;
    const QString prose = sheet.value(QStringLiteral("description")).toString().trimmed();
    return prose.isEmpty() ? (tags.isEmpty() ? QString() : QStringLiteral("The character has %1.").arg(tags)) : prose;
}

} // namespace

CharacterApplied applyCharacter(ImageRequest &request, const CharacterContext &character, const ImageCapabilities &caps, CharacterUse use,
                                const PromptStyle &style, const QString &modelFamily)
{
    CharacterApplied applied;
    if (character.isEmpty())
        return applied;

    // 1. The LoRA.
    if (character.lora.isValid()) {
        if (use == CharacterUse::Edit)
            applied.loraSkipped = QStringLiteral("use");
        else if (!caps.loras)
            applied.loraSkipped = QStringLiteral("no_lora_support");
        else if (!sameFamily(character.lora.family, modelFamily))
            applied.loraSkipped = QStringLiteral("family");
        else
            applied.lora = Director::applyCharacterLora(request, character.lora, caps);
    }

    // 2. References: Create shots always send them (with the LoRA, as in M13); otherwise only in
    // place of a LoRA, since a reference through IP-Adapter pulls its pose into the image.
    const bool wantReference = use == CharacterUse::Shot || !applied.lora;
    if (wantReference && caps.maxReferences > 0 && !character.references.isEmpty()) {
        const int room = caps.maxReferences - int(request.references.size());
        const int count = use == CharacterUse::Shot ? room : std::min(1, room);
        for (int i = 0; i < std::min(count, int(character.references.size())); ++i) {
            ReferenceImage r;
            r.image = character.references[i];
            r.role = ReferenceRole::Character;
            r.weight = 0.8;
            request.references << r;
            applied.reference = true;
        }
    }

    // 3. The description: whole images only; Create's planned prompts already carry it.
    const bool wantDescription = use == CharacterUse::Full || (use == CharacterUse::Edit && !applied.reference);
    if (wantDescription) {
        const QString text = description(character.sheet, style);
        if (!text.isEmpty() && !request.prompt.contains(text, Qt::CaseInsensitive)) {
            const QString prompt = request.prompt.trimmed();
            if (prompt.isEmpty())
                request.prompt = text;
            else
                request.prompt = style.dialect == PromptStyle::Dialect::Tags ? prompt + QStringLiteral(", ") + text : prompt + QStringLiteral("\n\n") + text;
            applied.description = true;
        }
    }
    return applied;
}

QJsonObject CharacterApplied::toJson(const CharacterContext &character) const
{
    QJsonObject o{{QStringLiteral("character"), character.id},
                  {QStringLiteral("name"), character.name},
                  {QStringLiteral("reference"), reference},
                  {QStringLiteral("description"), description}};
    if (lora) {
        o.insert(QStringLiteral("lora"), character.lora.file);
        o.insert(QStringLiteral("trigger"), character.lora.trigger);
        o.insert(QStringLiteral("lora_weight"), character.lora.weight);
    }
    if (!loraSkipped.isEmpty())
        o.insert(QStringLiteral("lora_skipped"), loraSkipped);
    return o;
}

QString CharacterApplied::fallbackNote(const CharacterContext &character, const QString &providerName) const
{
    if (loraSkipped.isEmpty() || loraSkipped == QLatin1String("use"))
        return {};
    const QString instead = reference     ? Text::tr("using the reference image of %1").arg(character.name)
                            : description ? Text::tr("describing %1 in the prompt").arg(character.name)
                                          : Text::tr("without %1").arg(character.name);
    if (loraSkipped == QLatin1String("family"))
        return Text::tr("The LoRA of %1 is for %2 models, not the loaded one: %3.").arg(character.name, character.lora.family, instead);
    return Text::tr("%1 cannot load LoRAs: %2.").arg(providerName, instead);
}

} // namespace lumen::ai
