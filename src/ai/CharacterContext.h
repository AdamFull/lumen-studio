#pragma once

#include "ai/Types.h"

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace lumen::ai {

struct PromptStyle;

// A character as image requests use it (M14 §14.3): its sheet, reference images and LoRA.
struct CharacterContext
{
    QString id;                 // character library id; empty for a character not in the library
    QString name;
    QJsonObject sheet;
    QList<QImage> references;
    CharacterLora lora;

    bool isEmpty() const { return sheet.isEmpty() && references.isEmpty() && !lora.isValid(); }
};

// What a request is for, which decides how much of the character goes in.
enum class CharacterUse {
    Full,   // txt2img, img2img, variations: the LoRA (else the reference), and the description
    Region, // inpaint, expand: the LoRA (else the reference); the prompt describes the region only
    Edit,   // instruction edit: the reference (else the description); edit models take no LoRA
    Shot,   // Create page shots: the planned prompt already describes the character; LoRA and reference
};

// How a character went into a request.
struct CharacterApplied
{
    bool lora = false;
    bool reference = false;
    bool description = false;
    // Why the LoRA was not used although the character has one: "no_lora_support", "family", or
    // "use" (the request kind takes no LoRA). Empty when it was used or there is none.
    QString loraSkipped;

    bool any() const { return lora || reference || description; }
    // For the generation's metadata: {character, name, lora, trigger, lora_weight, reference, description}.
    QJsonObject toJson(const CharacterContext &character) const;
    // "Gemini cannot load LoRAs: using Akane's reference image." Empty when the LoRA was
    // used, the character has none, or the request kind takes none.
    QString fallbackNote(const CharacterContext &character, const QString &providerName) const;
};

// Puts the character into the request as far as the back-end allows:
// 1. the LoRA (trigger word first) when the provider loads LoRAs and `modelFamily` (the loaded
//    checkpoint's family, "SDXL" / "SD 1.5" / …; empty = unknown) matches the LoRA's;
// 2. the first reference image (Character role) when the provider takes references;
// 3. the appearance description: tags for tag-style models, a sentence otherwise.
// Which of these apply depends on `use` (see CharacterUse). Existing references and LoRAs of the
// request stay; nothing is added twice.
CharacterApplied applyCharacter(ImageRequest &request, const CharacterContext &character, const ImageCapabilities &caps, CharacterUse use,
                                const PromptStyle &style, const QString &modelFamily = QString());

} // namespace lumen::ai
