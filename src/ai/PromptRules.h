#pragma once

#include "ai/PromptStyle.h"
#include "ai/Types.h"

#include <QString>

namespace lumen::ai {

// What a change asks for (plan M18 §18.2), read from the user's words by a few patterns, in English
// and German: "remove the cup", "replace the cup with a candle", "make the car red", "add a hat".
struct Instruction
{
    enum class Kind { None, Remove, Replace, Recolor, Add };
    Kind kind = Kind::None;
    QString target; // what is there now ("the cup"); empty for "it" or when not said
    QString result; // what should be there ("a candle", "red")
};
Instruction parseInstruction(const QString &words);

// The request a model gets for the user's words (plan M18 §18.2):
// * a new picture: the words with the family's quality words;
// * an instruction edit (FLUX.2 klein, Kontext, Qwen-Image-Edit): the words as they are;
// * an area changed by inpainting or image-to-image: what should be there instead of the
//   instruction ("replace the cup with a candle" → "a candle"; "remove the cup" → the words for an
//   empty place, with the cup among the things to leave out), in the family's dialect (tags without
//   articles for SDXL anime models, Pony, Anima) and with its quality words.
// `negative` is the user's (or, without one, the family's default); `literal` (Use my words as they
// are) sends both as they are.
struct FittedPrompt
{
    QString prompt;
    QString negative;
    bool rewritten = false; // the words were turned into a description of the result
};
FittedPrompt fitPrompt(const PromptStyle &style, GenMode mode, bool area, const QString &words, const QString &negative,
                       bool literal = false);

// What an inpainting model gets for "remove": a place with nothing in it.
QString emptyPlaceWords();

} // namespace lumen::ai
