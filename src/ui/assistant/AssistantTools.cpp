#include "ui/assistant/AssistantTools.h"

#include "ai/CharacterContext.h"
#include "ai/Director.h"
#include "ai/GenerationService.h"
#include "ai/PromptStyle.h"
#include "core/ImageOps.h"
#include "core/JobQueue.h"
#include "core/JsonUtil.h"
#include "doc/CharacterLibrary.h"
#include "ui/Studio.h"
#include "ui/dialogs/CharacterLoraDialog.h"
#include "ui/assistant/DirectorActions.h"

#include <QJsonArray>
#include <QPointer>
#include <QPromise>

#include <cstring>

namespace lumen::ui {

namespace {

using ai::ToolResult;

// ---- schema helpers ------------------------------------------------------------------------------

QJsonObject prop(const char *type, const char *description)
{
    return QJsonObject{{QStringLiteral("type"), QLatin1String(type)}, {QStringLiteral("description"), QLatin1String(description)}};
}

QJsonObject stringArray(const char *description)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                       {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                       {QStringLiteral("description"), QLatin1String(description)}};
}

QJsonObject schema(const QJsonObject &properties, const QStringList &required = {})
{
    QJsonArray req;
    for (const QString &r : required)
        req.append(r);
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), properties}, {QStringLiteral("required"), req}};
}

ai::ToolDefinition tool(const char *name, const char *description, const QJsonObject &parameters)
{
    return ai::ToolDefinition{QLatin1String(name), QLatin1String(description), parameters};
}

// ---- async helpers ---------------------------------------------------------------------------------

// A library character for the model: id, name, a short description and whether it has a LoRA.
QJsonObject characterJson(const doc::CharacterEntry &e)
{
    return QJsonObject{{QStringLiteral("id"), e.id},
                       {QStringLiteral("name"), e.name},
                       {QStringLiteral("summary"), ai::Director::appearanceTags(e.sheet).left(300)},
                       {QStringLiteral("has_lora"), ai::CharacterLora::fromJson(e.lora).isValid()}};
}

QFuture<ToolResult> ready(ToolResult r)
{
    return QtFuture::makeReadyValueFuture(r);
}

QFuture<ToolResult> fail(const QString &message)
{
    return ready(ToolResult::failure(message));
}

// Resolves when the job finishes, with the assets it stored. The image providers release the GPU
// first, so the language model that reads the result runs at full speed.
QFuture<ToolResult> waitForJob(Studio *studio, Job *job, QJsonObject extra = {})
{
    auto promise = std::make_shared<QPromise<ToolResult>>();
    promise->start();
    QFuture<ToolResult> future = promise->future();
    QPointer<Studio> s(studio);
    QObject::connect(job, &Job::finished, job, [promise, extra, s](Job *j) {
        ToolResult r;
        r.data = extra;
        if (j->state() == Job::State::Succeeded) {
            for (const doc::AssetInfo &a : ai::GenerationService::outputOf(j).assets) {
                if (!a.id.isEmpty())
                    r.assetIds << a.id;
            }
        } else {
            r.error = j->state() == Job::State::Cancelled ? QObject::tr("cancelled") : j->errorString();
        }
        auto resolve = [promise, r] {
            promise->addResult(r);
            promise->finish();
        };
        if (s)
            s->generation()->releaseGpu().then(s.data(), resolve);
        else
            resolve();
    });
    return future;
}

QJsonObject assetJson(const doc::AssetInfo &a)
{
    QJsonObject o{{QStringLiteral("id"), a.id},
                  {QStringLiteral("kind"), doc::assetKindName(a.kind)},
                  {QStringLiteral("name"), a.name},
                  {QStringLiteral("width"), a.size.width()},
                  {QStringLiteral("height"), a.size.height()},
                  {QStringLiteral("created"), a.created.toString(Qt::ISODate)}};
    const QString prompt = a.prompt();
    if (!prompt.isEmpty())
        o.insert(QStringLiteral("prompt"), prompt.left(160));
    if (a.metadata.contains(QStringLiteral("critic")))
        o.insert(QStringLiteral("critic_score"), a.metadata.value(QStringLiteral("critic")).toObject().value(QStringLiteral("score")));
    return o;
}

// Loads an asset's image, then runs `fn` with it (on the GUI thread).
QFuture<ToolResult> withAssetImage(Studio *studio, const QString &assetId, std::function<QFuture<ToolResult>(QImage)> fn)
{
    doc::Project *project = studio->project();
    if (!project || !project->asset(assetId))
        return fail(QObject::tr("There is no asset \"%1\". Use list_assets to find ids.").arg(assetId));
    return project->loadImage(assetId)
        .then(studio, [fn](QImage image) {
            if (image.isNull())
                return fail(QObject::tr("The image could not be loaded."));
            return fn(image);
        })
        .unwrap()
        .onFailed(studio, [](const std::exception &e) { return ToolResult::failure(QString::fromUtf8(e.what())); });
}

// Adapts a DirectorActions callback to a future.
template<typename T>
struct Bridge
{
    std::shared_ptr<QPromise<ToolResult>> promise = std::make_shared<QPromise<ToolResult>>();
    Bridge() { promise->start(); }
    QFuture<ToolResult> future() const { return promise->future(); }
    void resolve(const ToolResult &r) const
    {
        promise->addResult(r);
        promise->finish();
    }
};

} // namespace

QString assistantSystemPrompt()
{
    return QStringLiteral(
        "You are the assistant inside Lumen Studio, a desktop studio for AI image generation and editing. You act through tools; "
        "never claim you did something without calling the tool. All generated and edited images are stored in the project "
        "library and identified by asset ids.\n\n"
        "Guidelines:\n"
        "- Start with get_state when you need to know what exists (references, plan, recent images).\n"
        "- For several images of the same character (sets, Live2D/VTuber sources, turnarounds, stickers) use the director tools in "
        "order: analyze_references (with the user's reference asset ids) → plan_shots (pick the matching purpose) → optionally "
        "revise_pose → generate_shots. The user can review and edit the plan on the Create page.\n"
        "- The project's active character (get_state) is the one the user works on; list_characters and set_active_character "
        "switch it. Name the character in prompts only when no character is active.\n"
        "- For a single image use generate_image; pass reference_asset_ids to keep a character consistent.\n"
        "- To change an existing image use edit_image (whole image, by instruction) or inpaint_region (a rectangle given in "
        "fractions of the image size, 0-1). Results are new images; the original is kept.\n"
        "- Write prompts in English. Keep answers short: say what you did and list the new asset ids; offer the next step.\n"
        "- Ask a short question when the request is ambiguous instead of guessing expensive generations.");
}

namespace {

const char *kCharacterParam = "true (default) draws the project's active character (see get_state); false leaves it out";

// Adds the project's active character to an image request (M14), unless the call passes
// "character": false, then submits it. The result tells the model what went in.
QFuture<ToolResult> submitWithCharacter(Studio *studio, ai::ImageProvider *provider, ai::GenerationSpec spec, ai::CharacterUse use,
                                        const QJsonObject &args)
{
    QPointer<Studio> s(studio);
    const QString id = args.value(QStringLiteral("character")).toBool(true) ? studio->activeCharacter() : QString();
    if (id.isEmpty() || !provider)
        return waitForJob(studio, studio->generation()->submit(spec));
    const ai::ImageCapabilities caps = provider->capabilities();
    const ai::PromptStyle style = ai::promptStyleFor(provider);
    const QString family = checkpointFamily(studio, provider);
    const QString providerName = provider->displayName();
    return studio->characterContext(id)
        .then(studio, [s, spec, use, caps, style, family, providerName](const ai::CharacterContext &c) mutable -> QFuture<ToolResult> {
            if (!s)
                return fail(QStringLiteral("closed"));
            QJsonObject extra;
            if (!c.isEmpty()) {
                const ai::CharacterApplied applied = ai::applyCharacter(spec.request, c, caps, use, style, family);
                if (applied.any())
                    spec.meta.insert(QStringLiteral("character"), applied.toJson(c));
                QStringList used;
                if (applied.lora)
                    used << QStringLiteral("LoRA");
                if (applied.reference)
                    used << QStringLiteral("reference image");
                if (applied.description)
                    used << QStringLiteral("description");
                QJsonObject info{{QStringLiteral("name"), c.name}, {QStringLiteral("used"), used.join(QStringLiteral(", "))}};
                const QString note = applied.fallbackNote(c, providerName);
                if (!note.isEmpty())
                    info.insert(QStringLiteral("note"), note);
                extra.insert(QStringLiteral("character"), info);
            }
            return waitForJob(s, s->generation()->submit(spec), extra);
        })
        .unwrap();
}

} // namespace

ai::ToolSet assistantTools(Studio *studio)
{
    ai::ToolSet tools;
    QPointer<Studio> s(studio);

    tools.add(tool("get_state", "Overview of the open project: recent library images, the active editor document, and the "
                                "director session (references, character, shot plan with results and critic scores).",
                   schema({})),
              [s](const QJsonObject &) {
                  if (!s || !s->project())
                      return fail(QObject::tr("No project is open."));
                  ToolResult r;
                  r.data.insert(QStringLiteral("project"), s->project()->displayName());
                  QJsonArray recent;
                  const auto &assets = s->project()->assets();
                  for (int i = 0; i < std::min<int>(12, int(assets.size())); ++i)
                      recent.append(assetJson(assets[i]));
                  r.data.insert(QStringLiteral("recent_assets"), recent);
                  if (doc::Document *d = s->activeDocument())
                      r.data.insert(QStringLiteral("editor_document"), QJsonObject{{QStringLiteral("name"), d->name()},
                                                                                     {QStringLiteral("width"), d->size().width()},
                                                                                     {QStringLiteral("height"), d->size().height()},
                                                                                     {QStringLiteral("layers"), d->layerCount()}});
                  if (DirectorActions *director = s->directorActions())
                      r.data.insert(QStringLiteral("director"), director->directorState());
                  if (ai::ImageProvider *p = s->providers()->defaultImageProvider())
                      r.data.insert(QStringLiteral("image_provider"), p->displayName());
                  if (const auto c = s->characters()->entry(s->activeCharacter()))
                      r.data.insert(QStringLiteral("active_character"), characterJson(*c));
                  return ready(r);
              });

    tools.add(tool("list_characters", "Lists the characters in the character library (shared by all projects).", schema({})),
              [s](const QJsonObject &) {
                  if (!s)
                      return fail(QObject::tr("Lumen is shutting down."));
                  QJsonArray list;
                  for (const doc::CharacterEntry &e : s->characters()->entries())
                      list.append(characterJson(e));
                  ToolResult r;
                  r.data.insert(QStringLiteral("characters"), list);
                  r.data.insert(QStringLiteral("active"), s->activeCharacter());
                  return ready(r);
              });

    tools.add(tool("set_active_character", "Makes a library character the project's active character, which image tools then draw. "
                                           "An empty id clears it.",
                   schema({{QStringLiteral("character_id"), prop("string", "id from list_characters, or empty for none")}}, {QStringLiteral("character_id")})),
              [s](const QJsonObject &args) {
                  if (!s || !s->project())
                      return fail(QObject::tr("No project is open."));
                  const QString id = args.value(QStringLiteral("character_id")).toString().trimmed();
                  if (!id.isEmpty() && !s->characters()->entry(id))
                      return fail(QObject::tr("There is no character %1 in the library; use list_characters.").arg(id));
                  s->setActiveCharacter(id);
                  ToolResult r;
                  r.data.insert(QStringLiteral("active"), id);
                  return ready(r);
              });

    tools.add(tool("list_assets", "Lists library images, newest first.",
                   schema({{QStringLiteral("kind"), prop("string", "optional filter: reference, generation, edit, import, pose, part")},
                           {QStringLiteral("query"), prop("string", "optional text to search in names and prompts")},
                           {QStringLiteral("limit"), prop("integer", "maximum number of results (default 20)")}})),
              [s](const QJsonObject &args) {
                  if (!s || !s->project())
                      return fail(QObject::tr("No project is open."));
                  const QString kind = args.value(QStringLiteral("kind")).toString();
                  const QString query = args.value(QStringLiteral("query")).toString();
                  const int limit = std::clamp(args.value(QStringLiteral("limit")).toInt(20), 1, 100);
                  QJsonArray list;
                  for (const doc::AssetInfo &a : s->project()->assets()) {
                      if (!kind.isEmpty() && doc::assetKindName(a.kind) != kind)
                          continue;
                      if (!query.isEmpty() && !a.name.contains(query, Qt::CaseInsensitive) && !a.prompt().contains(query, Qt::CaseInsensitive))
                          continue;
                      list.append(assetJson(a));
                      if (list.size() >= limit)
                          break;
                  }
                  ToolResult r;
                  r.data.insert(QStringLiteral("assets"), list);
                  return ready(r);
              });

    tools.add(tool("generate_image", "Generates new images from a prompt with the default image provider. Waits until they are done. "
                                     "The project's active character is drawn automatically (its LoRA, reference or description): "
                                     "describe only the scene, pose and mood.",
                   schema({{QStringLiteral("prompt"), prop("string", "the image prompt, in English")},
                           {QStringLiteral("character"), prop("boolean", kCharacterParam)},
                           {QStringLiteral("negative_prompt"), prop("string", "optional things to avoid")},
                           {QStringLiteral("aspect_ratio"), prop("string", "e.g. 1:1, 2:3, 3:2, 16:9 (default 1:1)")},
                           {QStringLiteral("count"), prop("integer", "number of variants, 1-4 (default 1)")},
                           {QStringLiteral("reference_asset_ids"), stringArray("optional library images of the character to stay consistent with")}},
                          {QStringLiteral("prompt")})),
              [s](const QJsonObject &args) {
                  if (!s)
                      return fail(QStringLiteral("closed"));
                  ai::ImageProvider *provider = s->providers()->defaultImageProvider();
                  if (!provider)
                      return fail(QObject::tr("No image provider is configured."));
                  const ai::ImageCapabilities caps = provider->capabilities();
                  const QStringList refIds = json::toStringList(args.value(QStringLiteral("reference_asset_ids")).toArray());
                  auto submit = [s, provider, caps, args](QList<QImage> refs) -> QFuture<ToolResult> {
                      if (!s)
                          return fail(QStringLiteral("closed"));
                      const ai::PromptStyle style = ai::promptStyleFor(provider);
                      ai::GenerationSpec spec;
                      spec.providerId = provider->instanceId();
                      spec.request.prompt = args.value(QStringLiteral("prompt")).toString();
                      spec.request.negativePrompt = args.value(QStringLiteral("negative_prompt")).toString();
                      if (spec.request.negativePrompt.isEmpty())
                          spec.request.negativePrompt = style.defaultNegative;
                      spec.request.size = ai::Director::sizeForAspect(args.value(QStringLiteral("aspect_ratio")).toString(QStringLiteral("1:1")),
                                                                      caps.nativePixels, std::max(8, caps.sizeMultiple));
                      spec.request.batchCount = std::clamp(args.value(QStringLiteral("count")).toInt(1), 1, 4);
                      for (const QImage &img : std::as_const(refs).mid(0, std::max(0, caps.maxReferences))) {
                          ai::ReferenceImage r;
                          r.image = img;
                          r.role = ai::ReferenceRole::Character;
                          r.weight = 0.8;
                          spec.request.references << r;
                      }
                      spec.title = spec.request.prompt.left(60);
                      spec.meta.insert(QStringLiteral("assistant"), true);
                      return submitWithCharacter(s, provider, spec, ai::CharacterUse::Full, args);
                  };
                  if (refIds.isEmpty() || !s->project())
                      return submit({});
                  // Load the references first.
                  auto images = std::make_shared<QList<QImage>>();
                  QList<QFuture<QImage>> loads;
                  for (const QString &id : refIds)
                      loads << s->project()->loadImage(id);
                  return QtFuture::whenAll(loads.begin(), loads.end())
                      .then(s.data(), [submit](const QList<QFuture<QImage>> &done) {
                          QList<QImage> refs;
                          for (const QFuture<QImage> &f : done) {
                              if (!f.isCanceled() && f.isValid() && f.resultCount() > 0)
                                  refs << f.result();
                          }
                          return submit(refs);
                      })
                      .unwrap();
              });

    tools.add(tool("edit_image", "Changes a whole library image by instruction (e.g. 'make it night', 'add a red scarf'). The result is a new image. "
                                 "The active character's reference or description helps keep the character.",
                   schema({{QStringLiteral("asset_id"), prop("string", "the image to edit")},
                           {QStringLiteral("character"), prop("boolean", kCharacterParam)},
                           {QStringLiteral("instruction"), prop("string", "what to change, in English")},
                           {QStringLiteral("strength"), prop("number", "0.2 (subtle) to 0.9 (strong), default 0.6; ignored by instruction-edit models")}},
                          {QStringLiteral("asset_id"), QStringLiteral("instruction")})),
              [s](const QJsonObject &args) {
                  if (!s)
                      return fail(QStringLiteral("closed"));
                  ai::ImageProvider *provider = s->providers()->defaultImageProvider();
                  if (!provider)
                      return fail(QObject::tr("No image provider is configured."));
                  const QString assetId = args.value(QStringLiteral("asset_id")).toString();
                  return withAssetImage(s, assetId, [s, provider, args, assetId](QImage image) {
                      const ai::ImageCapabilities caps = provider->capabilities();
                      ai::GenerationSpec spec;
                      spec.providerId = provider->instanceId();
                      spec.request.mode = caps.edit ? ai::GenMode::Edit : ai::GenMode::ImageToImage;
                      spec.request.prompt = args.value(QStringLiteral("instruction")).toString();
                      spec.request.denoise = std::clamp(args.value(QStringLiteral("strength")).toDouble(0.6), 0.05, 1.0);
                      const QSize work = caps.fixedSizes.isEmpty()
                                             ? img::fitToPixelBudget(image.size(), std::min(caps.nativePixels, caps.maxPixels), caps.sizeMultiple)
                                             : image.size();
                      spec.request.initImage = img::normalized(img::scaled(img::flatten(image, Qt::white), work, true));
                      spec.request.size = work;
                      spec.assetKind = doc::AssetKind::Edit;
                      spec.parentAssetId = assetId;
                      spec.title = QObject::tr("Edit: %1").arg(spec.request.prompt.left(40));
                      spec.meta.insert(QStringLiteral("assistant"), true);
                      return submitWithCharacter(s, provider, spec, ai::CharacterUse::Edit, args);
                  });
              });

    tools.add(tool("inpaint_region", "Regenerates a rectangular region of a library image from a prompt (local fix or replacement); "
                                     "the rest stays untouched. Coordinates are fractions (0-1) of the image width and height. The "
                                     "active character's LoRA keeps a fixed face or hand in its style; the prompt describes only the region.",
                   schema({{QStringLiteral("asset_id"), prop("string", "the image")},
                           {QStringLiteral("character"), prop("boolean", kCharacterParam)},
                           {QStringLiteral("x"), prop("number", "left edge, 0-1")},
                           {QStringLiteral("y"), prop("number", "top edge, 0-1")},
                           {QStringLiteral("width"), prop("number", "0-1")},
                           {QStringLiteral("height"), prop("number", "0-1")},
                           {QStringLiteral("prompt"), prop("string", "what the region should show, in English")}},
                          {QStringLiteral("asset_id"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height"),
                           QStringLiteral("prompt")})),
              [s](const QJsonObject &args) {
                  if (!s)
                      return fail(QStringLiteral("closed"));
                  const QString assetId = args.value(QStringLiteral("asset_id")).toString();
                  return withAssetImage(s, assetId, [s, args, assetId](QImage image) {
                      const QRectF frac(args.value(QStringLiteral("x")).toDouble(), args.value(QStringLiteral("y")).toDouble(),
                                        args.value(QStringLiteral("width")).toDouble(), args.value(QStringLiteral("height")).toDouble());
                      const QRect rect = QRectF(frac.x() * image.width(), frac.y() * image.height(), frac.width() * image.width(),
                                                frac.height() * image.height())
                                             .toAlignedRect()
                                             .intersected(image.rect());
                      if (rect.width() < 4 || rect.height() < 4)
                          return fail(QObject::tr("The region is empty; give x, y, width and height as fractions between 0 and 1."));
                      QImage mask = img::createMask(image.size());
                      for (int y = rect.top(); y <= rect.bottom(); ++y)
                          std::memset(mask.scanLine(y) + rect.left(), 255, size_t(rect.width()));
                      ai::GenerationSpec spec;
                      spec.request.prompt = args.value(QStringLiteral("prompt")).toString();
                      spec.request.denoise = 0.85;
                      spec.region = ai::RegionInput{img::normalized(image), mask, 48, 6, 12, true};
                      spec.assetKind = doc::AssetKind::Edit;
                      spec.parentAssetId = assetId;
                      spec.title = QObject::tr("Inpaint: %1").arg(spec.request.prompt.left(40));
                      spec.meta.insert(QStringLiteral("assistant"), true);
                      ai::ImageProvider *provider = s->providers()->defaultImageProvider();
                      if (provider)
                          spec.providerId = provider->instanceId();
                      return submitWithCharacter(s, provider, spec, ai::CharacterUse::Region, args);
                  });
              });

    tools.add(tool("remove_background", "Cuts the subject of a library image out onto transparency (new image).",
                   schema({{QStringLiteral("asset_id"), prop("string", "the image")}}, {QStringLiteral("asset_id")})),
              [s](const QJsonObject &args) {
                  if (!s)
                      return fail(QStringLiteral("closed"));
                  const QString id = args.value(QStringLiteral("asset_id")).toString();
                  if (!s->project() || !s->project()->asset(id))
                      return fail(QObject::tr("There is no asset \"%1\".").arg(id));
                  return waitForJob(s, s->generation()->submitUtilityForAsset(QString(), ai::Utility::RemoveBackground, id, {},
                                                                           QObject::tr("Remove background")));
              });

    tools.add(tool("upscale", "Upscales a library image (new image).",
                   schema({{QStringLiteral("asset_id"), prop("string", "the image")},
                           {QStringLiteral("factor"), prop("integer", "2 or 4 (default 2)")}},
                          {QStringLiteral("asset_id")})),
              [s](const QJsonObject &args) {
                  if (!s)
                      return fail(QStringLiteral("closed"));
                  const QString id = args.value(QStringLiteral("asset_id")).toString();
                  if (!s->project() || !s->project()->asset(id))
                      return fail(QObject::tr("There is no asset \"%1\".").arg(id));
                  const int factor = args.value(QStringLiteral("factor")).toInt(2) >= 4 ? 4 : 2;
                  return waitForJob(s, s->generation()->submitUtilityForAsset(QString(), ai::Utility::Upscale, id,
                                                                           QJsonObject{{QStringLiteral("factor"), factor}},
                                                                           QObject::tr("Upscale %1×").arg(factor)));
              });

    tools.add(tool("open_in_editor", "Opens a library image in the layered editor for the user.",
                   schema({{QStringLiteral("asset_id"), prop("string", "the image")}}, {QStringLiteral("asset_id")})),
              [s](const QJsonObject &args) {
                  const QString id = args.value(QStringLiteral("asset_id")).toString();
                  if (!s || !s->project() || !s->project()->asset(id))
                      return fail(QObject::tr("There is no asset \"%1\".").arg(id));
                  s->openAssetInEditor(id);
                  return ready(ToolResult{});
              });

    // ---- director workflow -----------------------------------------------------------------------
    tools.add(tool("analyze_references", "Adds reference images of the character to the director session and creates the character "
                                         "sheet (appearance, outfit, palette) from them with the vision model.",
                   schema({{QStringLiteral("asset_ids"), stringArray("library images showing the character")},
                           {QStringLiteral("notes"), prop("string", "optional notes from the user about the character")}},
                          {QStringLiteral("asset_ids")})),
              [s](const QJsonObject &args) {
                  DirectorActions *director = s ? s->directorActions() : nullptr;
                  if (!director)
                      return fail(QObject::tr("The director is not available."));
                  Bridge<QJsonObject> bridge;
                  director->analyzeReferences(json::toStringList(args.value(QStringLiteral("asset_ids")).toArray()),
                                              args.value(QStringLiteral("notes")).toString(), [bridge](QJsonObject sheet, QString error) {
                                                  ToolResult r;
                                                  r.error = error;
                                                  if (error.isEmpty())
                                                      r.data.insert(QStringLiteral("character_sheet"), sheet);
                                                  bridge.resolve(r);
                                              });
                  return bridge.future();
              });

    tools.add(tool("plan_shots", "Plans (or revises) the shot plan of the director session: every shot gets a pose, framing, "
                                 "background and prompt. The user sees it on the Create page.",
                   schema({{QStringLiteral("request"), prop("string", "what the images are for and what they should show")},
                           {QStringLiteral("purpose"), prop("string", "purpose profile id: general, live2d, turnaround, expressions, stickers, game, portraits")},
                           {QStringLiteral("count"), prop("integer", "number of shots, 0 = as described")}},
                          {QStringLiteral("request")})),
              [s](const QJsonObject &args) {
                  DirectorActions *director = s ? s->directorActions() : nullptr;
                  if (!director)
                      return fail(QObject::tr("The director is not available."));
                  Bridge<QJsonObject> bridge;
                  director->planShots(args.value(QStringLiteral("request")).toString(), args.value(QStringLiteral("purpose")).toString(),
                                      args.value(QStringLiteral("count")).toInt(0), [bridge](QJsonObject plan, QString error) {
                                          ToolResult r;
                                          r.error = error;
                                          if (error.isEmpty()) {
                                              QJsonArray shots;
                                              for (const QJsonValue &v : plan.value(QStringLiteral("shots")).toArray()) {
                                                  const QJsonObject shot = v.toObject();
                                                  shots.append(QJsonObject{{QStringLiteral("id"), shot.value(QStringLiteral("id"))},
                                                                           {QStringLiteral("title"), shot.value(QStringLiteral("title"))},
                                                                           {QStringLiteral("description"), shot.value(QStringLiteral("description"))}});
                                              }
                                              r.data.insert(QStringLiteral("title"), plan.value(QStringLiteral("title")));
                                              r.data.insert(QStringLiteral("shots"), shots);
                                          }
                                          bridge.resolve(r);
                                      });
                  return bridge.future();
              });

    tools.add(tool("revise_pose", "Changes the pose of a planned shot from an instruction (e.g. 'raise the left arm', 'turn to three-quarter view').",
                   schema({{QStringLiteral("shot_id"), prop("string", "id from the shot plan")},
                           {QStringLiteral("instruction"), prop("string", "the change, in English")}},
                          {QStringLiteral("shot_id"), QStringLiteral("instruction")})),
              [s](const QJsonObject &args) {
                  DirectorActions *director = s ? s->directorActions() : nullptr;
                  if (!director)
                      return fail(QObject::tr("The director is not available."));
                  Bridge<QJsonObject> bridge;
                  director->revisePose(args.value(QStringLiteral("shot_id")).toString(), args.value(QStringLiteral("instruction")).toString(),
                                       [bridge](QJsonObject shot, QString error) {
                                           ToolResult r;
                                           r.error = error;
                                           if (error.isEmpty())
                                               r.data.insert(QStringLiteral("pose"), shot.value(QStringLiteral("pose")));
                                           bridge.resolve(r);
                                       });
                  return bridge.future();
              });

    tools.add(tool("generate_shots", "Generates planned shots with the character references and their poses (all approved shots when "
                                     "shot_ids is empty). Waits until they are done, including Critic retries when the Critic is on.",
                   schema({{QStringLiteral("shot_ids"), stringArray("optional shot ids; empty = every approved shot")}})),
              [s](const QJsonObject &args) {
                  DirectorActions *director = s ? s->directorActions() : nullptr;
                  if (!director)
                      return fail(QObject::tr("The director is not available."));
                  Bridge<QStringList> bridge;
                  director->generateShots(json::toStringList(args.value(QStringLiteral("shot_ids")).toArray()),
                                          [bridge, s](QStringList assets, QString error) {
                                              ToolResult r;
                                              r.error = error;
                                              r.assetIds = assets;
                                              if (s)
                                                  s->generation()->releaseGpu().then(s.data(), [bridge, r] { bridge.resolve(r); });
                                              else
                                                  bridge.resolve(r);
                                          });
                  return bridge.future();
              });

    return tools;
}

} // namespace lumen::ui
