#pragma once

#include "ai/Agent.h"

namespace lumen::ui {

class Studio;

// The tools the assistant can call: project state and library, image generation, instruction edits,
// region inpainting, background removal, upscaling, opening results, and the director workflow
// (references → character sheet → shot plan → poses → generation) when the Create page is available.
// Every image result is stored in the project library; tools return asset ids.
ai::ToolSet assistantTools(Studio *studio);

// System prompt of the assistant.
QString assistantSystemPrompt();

} // namespace lumen::ui
