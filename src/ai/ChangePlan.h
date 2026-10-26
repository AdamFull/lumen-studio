#pragma once

#include "ai/PromptStyle.h"
#include "ai/Types.h"

#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace lumen::ai {

class LlmProvider;
class ProviderRegistry;

// A plan for a change (plan M18 §18.5), proposed by a language model that sees the picture and
// the user's words, and shown to the user, who can change every part of it before it runs.
struct ChangePlan
{
    QString select;    // what to select first, in Select by description's words ("the black lines",
                       // "the cup"); empty: the selection as it is, or the whole picture
    QString prompt;    // the request for the image model, written for its family
    QString negative;  // what to leave out, for models that take it
    QStringList loras; // names from those offered
    QString note;      // the plan in a sentence, for the user
};

// What the planner is told about the change and the image model.
struct PlanContext
{
    QString words;         // the user's
    PromptStyle style;     // the image model's family
    bool instructions = false; // the model edits from instructions (FLUX.2 klein, Kontext, Qwen-Image-Edit)
    bool negative = false;     // the model takes a negative prompt
    bool hasSelection = false; // the user has selected the area already
    QStringList loras;     // the engine's LoRAs that fit its model
};

QJsonObject changePlanSchema();
// The request for the planner: the picture (at most 1024 pixels across), the words and what the
// image model needs.
ChatRequest changePlanRequest(const QImage &picture, const PlanContext &context);
// The plan in the answer: LoRAs that were not offered and a negative prompt the model does not
// take are dropped; an empty request falls back to the user's words.
ChangePlan parseChangePlan(const QJsonObject &answer, const PlanContext &context);

// The language model for planning: the one for "Vision analysis" (else the assistant's, else any),
// when it sees images; null when there is none that does.
LlmProvider *changePlanner(const ProviderRegistry *registry, QString *model = nullptr);

} // namespace lumen::ai
