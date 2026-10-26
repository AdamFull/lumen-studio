#pragma once

namespace lumen::ui {

class SmartSelect;

// The filter step "Find area" (plan M18 §18.1): the mask of the objects its words name ("the cup",
// "people"), found by Grounding DINO and cut out by SAM 2.1 on this computer, for "find area → fill
// area with AI" filters. It is a step that runs only on Run, like the AI steps, and says when its
// models are not downloaded. Registered once; it uses the newest studio's finder.
void registerFindAreaNode(SmartSelect *finder);

} // namespace lumen::ui
