#pragma once

#include "filters/NodeType.h"

#include <QList>
#include <QString>

namespace lumen::ai {

class GenerationService;

// The AI steps of node filters (M17 §17.6), in the node editor's "AI" category:
// * "Edit with AI" (ai-edit): an instruction edit of the picture ("colorize it"), or, with its
//   "Area" input connected, of that area only, through the region pipeline as the editor's prompt
//   does it (a crop around the area at the model's working size, blended back through the
//   feathered mask, so the rest stays as it was);
// * "Fill area with AI" (ai-inpaint): the area painted anew from a description (inpainting);
// * "Upscale with AI" (ai-upscale) and "Remove background" (ai-remove-background): the image
//   engine's utilities.
// Each runs as a Queue job of GenerationService on the image engine in use, or on the one chosen in
// the node (its "Image engine" setting), so it shows in the Queue with its progress and can be
// cancelled there or from the filter; nothing is saved as a project asset. The nodes submit their
// job from the GUI thread and hand its result to the evaluation as a future (filters/NodeType.h),
// report the job's progress (EvalContext::progressReporter) and cancel it when the evaluation is
// cancelled. They are expensive: a filter with one previews only after an explicit Run. A node whose
// engine cannot do its step says so in plain words (NodeType::unavailableReason) and fails with that
// reason when it runs. Their results arrive through the GUI thread's event loop, so the GUI thread
// must never block on an evaluation with an AI step (filters::evaluateNode, waitForFinished).
namespace filter_nodes {
inline const QString Edit = QStringLiteral("ai-edit");
inline const QString Inpaint = QStringLiteral("ai-inpaint");
inline const QString Upscale = QStringLiteral("ai-upscale");
inline const QString RemoveBackground = QStringLiteral("ai-remove-background");
// The setting that picks the image engine: a provider instance id; empty for the one in use.
inline const QString EngineParam = QStringLiteral("engine");
// Editor hints (filters::ParamSpec::editor) of text settings: the list of image engines, and a
// field of several lines for a prompt.
inline const QString EngineEditor = QStringLiteral("image-engine");
inline const QString PromptEditor = QStringLiteral("prompt");
} // namespace filter_nodes

// Adds the AI nodes to filters::NodeRegistry::global() (once) and, with a service, makes it the one
// they run through: the newest registered service that still exists (there is one per Studio).
// Call on the GUI thread at start-up, before filters are loaded: a filter with an AI step does not
// load while its node types are missing.
void registerFilterNodes(GenerationService *service = nullptr);
// The service the AI nodes run through now; null when there is none. GUI thread.
GenerationService *filterNodeService();
// The engines an AI node can choose, for its "Image engine" list: first "The one in use" (an empty
// id), then every image provider of the service. GUI thread.
QList<filters::ChoiceOption> filterNodeEngines();
// The name of an engine id as that list shows it ("The one in use" for an empty id).
QString filterNodeEngineName(const QString &id);

} // namespace lumen::ai
