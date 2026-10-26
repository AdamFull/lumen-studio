# Lumen Studio — Development Plan

AI-assisted image generation and editing desktop application built with C++20 and Qt 6.

> Status: living document. Each milestone has a checklist; tick items as they land.
>
> Direction change (October 2026): Lumen becomes an editor-first AI image editor, with the
> character, pose, Live2D and LoRA features as optional plugins. See **M17**; where it conflicts
> with sections 1–3, M17 wins.

---

## 1. Product vision

Lumen Studio is a production-grade desktop studio that unifies **AI generation, AI-assisted editing,
pose authoring and LLM-driven planning** in one responsive application, working equally well with
**local** back-ends (Stable Diffusion WebUI / Forge, ComfyUI, llama.cpp, Ollama, LM Studio, …) and
**cloud** APIs (OpenAI, Google Gemini, Anthropic, Stability AI, fal.ai, OpenRouter, …).

### 1.1 Reference scenario (must work end-to-end)

> "I need three images of my character for a Live2D avatar." User attaches five reference images,
> describes the poses / purpose in the chat.

1. **Intake** – chat message + attachments are stored in the project as reference assets.
2. **Analysis** – a vision-capable LLM extracts a *Character Sheet* (appearance, outfit, palette,
   distinctive features, art style) from the references; the user can edit it.
3. **Planning** – a *Director* LLM produces a structured *Shot Plan* (N shots: purpose, pose,
   framing, expression, background, aspect ratio, prompt). For "Live2D" it applies the purpose
   profile (front view, neutral A-pose, arms clear of the torso, closed mouth, plain background,
   high resolution).
4. **Pose drafts** – each shot gets an editable skeleton (OpenPose-compatible) generated from the
   plan; the user adjusts joints, mirrors, swaps presets, or asks the chat to change it.
5. **Generation** – for every approved shot the app builds a request for the selected provider
   using the best available strategy (ControlNet OpenPose + IP-Adapter, FLUX Kontext /
   Qwen-Image-Edit multi-reference workflows, multi-image edit APIs with the pose map as an extra
   reference) and generates N variants without blocking the UI.
6. **Review & edit** – every result opens in the editor: layers, masks, local inpainting,
   outpainting, variations, upscale, background removal, adjustments, layered PSD export.

### 1.2 Functional scope

| Area | Capabilities |
|---|---|
| Generation | txt2img, img2img, inpaint, outpaint, instruction edit, variations, multi-reference, pose/structure control, batch, seeds, presets |
| Editor | GPU canvas, layers + blend modes, brush/eraser, selections, mask painting, transform, crop, fill, eyedropper, adjustments, unlimited undo |
| Inpainting | "only masked" region pipeline with padding, model-resolution scaling, feathered compositing into a **new layer** (non-destructive), any provider |
| Poses | OpenPose COCO-18 body + 21-pt hands + 70-pt face, FK/IK editing, presets, mirror, multi-person, pose from image, LLM-generated poses |
| Chat / Director | multi-turn chat with attachments, streaming, vision, structured outputs, tool-calling agent, shot planning, critique |
| Providers | pluggable image / LLM / utility providers, schema-driven settings UI, health check, model discovery, secrets in OS credential store |
| Library | project assets, characters, poses, prompts, generation metadata, favourites, search, reproducibility |
| Export | PNG (A1111-compatible metadata), JPEG, WebP, layered PSD (Live2D Cubism), OpenPose JSON |

### 1.3 Non-functional requirements

* **Responsiveness** – no GUI-thread task may exceed ~8 ms; all I/O is asynchronous, all heavy CPU
  work (decode/encode, base64, compositing, thumbnails, feathering, DB writes) runs on worker threads.
  A debug-build event-loop latency watchdog logs stalls > 50 ms.
* **Performance** – smooth 60 fps brushing and panning on 4096×4096 documents; tile-based composite
  cache (256 px tiles) rendered through an OpenGL-backed canvas; startup < 1.5 s.
* **Reliability** – autosave + crash recovery, SQLite WAL project files, cancellable jobs,
  retries with exponential back-off for 429/5xx, clear actionable error messages.
* **Security** – API keys only in the OS credential store (Windows Credential Manager), redacted in logs,
  never written to project files.
* **Portability** – Windows first (MinGW-w64 / Qt 6.9.2); code and CMake stay cross-platform, OS
  specifics behind interfaces.
* **Maintainability** – modular static libraries, unit + contract tests, provider adapters isolated.

---

## 2. Technology stack

### 2.1 Application stack

| Concern | Choice | Rationale |
|---|---|---|
| Language | **C++20** | concepts, ranges, `std::span`, designated initialisers; supported by GCC 13 / MSVC 19.3x |
| Framework | **Qt 6.9** | Core, Gui, Widgets, Network, Concurrent, Sql, Svg, OpenGL/OpenGLWidgets, LinguistTools, Test |
| Optional Qt | **Qt WebSockets** (ComfyUI live progress), **Qt Image Formats** plugins (WebP/TIFF) | detected by CMake; features degrade gracefully (polling progress) when absent |
| UI | **Qt Widgets** + custom QSS theme + custom-painted widgets | mature docking, compile-time safety, best for pro editors |
| Canvas | `QOpenGLWidget` + `QPainter` (GL paint engine) over a tile cache | GPU-composited viewport, texture re-upload only for dirty tiles |
| Build | **CMake ≥ 3.25 + Ninja**, `CMakePresets.json` | reproducible configure/build/test presets |
| Toolchain | MinGW-w64 GCC 13.1 (Qt kit); MSVC/clang-cl supported | matches the installed Qt kit |
| Persistence | **SQLite** via QtSql (one `.lumen` file per project, WAL) | atomic, crash-safe, single file, blobs for images |
| Settings | `QSettings` (INI in AppData) + JSON provider configs | human-readable, versioned |
| Secrets | Windows Credential Manager (`wincred`) behind `SecretStore` | OS-grade secret storage |
| JSON | `QJsonDocument` + in-house JSON-Schema-subset validator | no external deps, validates LLM output |
| Networking | `QNetworkAccessManager` (HTTP/2, TLS via Schannel), SSE parser, multipart builder | fully async on GUI thread event loop |
| Concurrency | `QThreadPool` / `QtConcurrent` / `QPromise` / `QFutureWatcher`, dedicated DB thread | never block the GUI |
| Tests | **Qt Test** + in-process mock HTTP server (`QTcpServer`) | contract tests for every provider |
| Packaging | `windeployqt` + CPack (NSIS / ZIP) | installer + portable build |
| Optional ML runtime | **ONNX Runtime** (C API, CMake option `LUMEN_WITH_ONNX`) | local DWPose/RTMPose, RMBG/BiRefNet, SAM — no Python needed |

### 2.2 AI technology stack

#### Image generation / editing back-ends

| Provider | Transport | Features used |
|---|---|---|
| **SD WebUI (A1111 / Forge / reForge)** | REST `/sdapi/v1/*` | txt2img, img2img, inpaint (mask, `inpaint_full_res`, padding), `/progress` live preview, `/interrupt`, models / samplers / schedulers / upscalers discovery, `extra-single-image` upscaling, ControlNet extension (`alwayson_scripts.controlnet`: OpenPose, IP-Adapter, reference-only; `/controlnet/detect` for DWPose), rembg extension |
| **stable-diffusion.cpp `sd-server`** (planned, M12) | native async REST `/sdcpp/v1` (`capabilities`, `img_gen` → job, `jobs/{id}`, cancel, `upscale`) | txt2img, img2img, inpaint, ControlNet, IP-Adapter (SD 1.5 / SDXL), LoRA, hires fix, ESRGAN; started and supervised by Lumen, model set fixed per process |
| **ComfyUI** | REST `/prompt`, `/upload/image`, `/history`, `/view`, `/object_info`, `/interrupt`, **WebSocket `/ws`** | workflow templates (API format) with declarative bindings: SDXL/Illustrious + ControlNet Union + IP-Adapter, FLUX.1 Kontext (multi-reference), Qwen-Image-Edit (multi-image + pose), inpaint (InpaintModelConditioning), upscale, DWPose preprocessor, background removal |
| **OpenAI Images** | `/v1/images/generations`, `/v1/images/edits` (multipart, multiple `image[]`, mask) | GPT-Image models: text-to-image, multi-reference edit, masked edit, transparent background |
| **Google Gemini / Imagen** | `generateContent` (inline image parts), Imagen `predict` | multi-image reference editing, instruction edits |
| **Stability AI** | REST v2beta `stable-image/*` (multipart) | generate (core/ultra/SD3.5), inpaint, outpaint, search-and-replace, control/structure, sketch, remove-background, upscale |
| **fal.ai** | queue REST API (submit → poll `status_url` → `response_url`, `cancel_url`) | FLUX dev/schnell/pro/ultra text-to-image, FLUX image-to-image, FLUX Fill inpainting, FLUX Kontext edits, BiRefNet background removal, ESRGAN upscale; one configurable endpoint id per mode, `extras` merged into the input |

#### LLM back-ends

| Provider | API | Notes |
|---|---|---|
| **OpenAI-compatible** | `/v1/chat/completions` (SSE), `/v1/models` | covers **llama.cpp server**, **Ollama**, **LM Studio**, vLLM, KoboldCpp, OpenRouter, OpenAI, Groq, DeepSeek… Vision via `image_url` data URIs, structured output via `response_format: json_schema` (llama.cpp turns it into a grammar), tool calling |
| **Anthropic** | Messages API (SSE) | vision, tool use for structured output |
| **Google Gemini** | `generateContent` / `streamGenerateContent` | vision, `responseSchema` |

#### LLM roles (each role assignable to any configured LLM)

* **Vision Analyst** – references → Character Sheet JSON.
* **Director** – request + character sheet + purpose profile → Shot Plan JSON.
* **Pose Planner** – shot description → rig parameters (preset + joint rotations, body yaw/pitch).
* **Prompt Engineer** – natural language → model-dialect prompt (booru tags for SDXL anime models,
  natural language for FLUX / GPT-Image / Gemini), negative prompt.
* **Critic** (optional) – compares output with references, proposes fixes / inpaint regions.
* **Assistant** – free chat with tool calling (`plan_shots`, `generate`, `edit_image`, `set_pose`, …).

Structured outputs: JSON Schema in the request when supported → local validation → automatic
repair loop (re-prompt with validation errors, max 2 attempts) → user-visible error if it still fails.

#### Computer-vision utilities

| Task | Primary | Fallbacks |
|---|---|---|
| Pose detection | A1111 `/controlnet/detect` (dw_openpose_full), ComfyUI DWPreprocessor | ONNX Runtime DWPose/RTMPose (optional) |
| Background removal | rembg extension / ComfyUI / Stability remove-background | ONNX RMBG / BiRefNet (optional) |
| Upscaling | A1111 extras, ComfyUI upscale models, Stability upscale | Lanczos (local) |
| Smart selection | — | ONNX SAM / MobileSAM (optional, late milestone) |
| Character part segmentation (M11) | see-through (LayerDiff 3D + Marigold + body-parsing SAM) via ComfyUI-See-through or the Lumen Part Server sidecar | Grounding DINO + SAM/SAM2 ComfyUI template (text-prompted per part), Gemini segmentation, local heuristics (skeleton/side/component splits) |
| Occluded part completion (M11) | see-through layers, inpainting on any image provider (§3.6) | LayerDiffuse ComfyUI template, cut-only (no generation) |

#### Character-consistency strategies (chosen automatically from provider capabilities)

1. **SD WebUI / ComfyUI SDXL**: ControlNet OpenPose (pose map) + IP-Adapter Plus / FaceID
   (identity from references) + Character Sheet prompt.
2. **ComfyUI FLUX Kontext / Qwen-Image-Edit**: multi-reference conditioning + pose map image.
3. **GPT-Image / Gemini**: multi-image edit request: references + rendered pose skeleton + explicit
   instruction ("reproduce the character from images 1–5 in the pose shown in image 6").
4. **Fallback**: img2img from the best reference with pose control, or txt2img with the sheet prompt.

### 2.3 Pose technology

* **Canonical representation**: 2D OpenPose keypoints (COCO-18 body, 2×21 hand, 70 face) with
  confidence/visibility per point, normalised to the canvas; multiple people per pose document.
* **Rendering**: byte-exact OpenPose colour conventions (as in `controlnet_aux`) so the map is
  consumable by every OpenPose ControlNet; also OpenPose JSON import/export.
* **Editing**: direct joint drag, FK (rotate sub-tree around a joint), 2-bone IK for limbs,
  bone-length lock, mirror L/R, flip, per-joint hide, onion-skin reference image underlay.
* **Generation rig**: a lightweight 3D forward-kinematics rig (skeleton with anatomical proportions,
  per-joint Euler rotations, body yaw/pitch, perspective/orthographic projection). LLMs output rig
  parameters (more reliable than raw coordinates); the rig is projected into editable 2D keypoints.
* **Preset library**: T-pose, A-pose (Live2D), standing variants, contrapposto, walking, running,
  sitting, kneeling, waving, arms crossed, hands on hips, pointing, jumping, lying, 3/4 and profile
  views, etc.; user presets saved to the library.

---

## 3. Architecture

### 3.1 Module layout

```
lumen/
  CMakeLists.txt, CMakePresets.json
  cmake/                      helper modules (warnings, deploy, version)
  src/
    core/        lumen_core      logging, settings, secrets, jobs, async utils, image ops, json schema
    net/         lumen_net       http client, retry/back-off, SSE, multipart, websocket wrapper
    doc/         lumen_doc       project store (SQLite), assets, document, layers, tiles, undo commands
    pose/        lumen_pose      skeleton, presets, FK/IK, 3D rig, renderer, OpenPose JSON
    runtime/     lumen_runtime   (M12) runtime installer, server supervisor, model library, downloads
    ai/          lumen_ai        provider interfaces + registry, request model, providers/*,
                                 comfy workflow templates, director pipeline, prompt templates
    filters/     lumen_filters   (M17) node filter graphs: model, .lumenfilter files, evaluation,
                                 classic image nodes
    ui/          lumen_ui        main window, theme, canvas, tools, panels, dialogs, chat, pose editor
    app/         lumen (exe)     main.cpp, resources, icons, translations
  resources/     icons (SVG), QSS theme, presets, comfy workflows, prompt templates
  tests/         unit + contract + ui smoke tests, mock servers
  docs/          plan, architecture notes, provider guides, user guide
```

Dependency direction: `app → ui → ai, doc, pose, filters → net, core` (`filters` needs only `core`).
No module depends on `ui` except `app`.

### 3.2 Threading model

| Thread | Responsibilities |
|---|---|
| GUI | widgets, document mutations (single writer), `QNetworkAccessManager`, job orchestration |
| Worker pool (`QThreadPool`, N = cores − 1) | decode/encode, base64, scaling, compositing, mask ops, thumbnails, pose rendering, JSON building for large payloads |
| DB thread | single SQLite connection, serialised async writes/reads returning `QFuture` |
| Optional ML thread | ONNX Runtime sessions |

Rules: GUI thread never decodes/encodes images, never touches SQLite, never sleeps/waits on futures.
Results return through `QFutureWatcher` / queued signals. Shared images are implicitly-shared `QImage`
copies (copy-on-write) – no locks on document data.

### 3.3 Job system

`Job` (QObject): id, title, kind, provider, state (Queued → Running → Succeeded/Failed/Cancelled),
progress (0–1, indeterminate), ETA, live preview image, log, result payload.
`JobQueue`: per-provider concurrency limits (local GPU back-ends default 1), priorities, cancel
(aborts replies + server-side interrupt), retry, persistence of finished results to the project.
UI: queue panel, status bar summary, taskbar progress.

### 3.4 Provider framework

```
ProviderDescriptor { id, kind(Image|Llm|Utility), name, description, settingsSchema, defaults }
Provider (QObject)    configure(settings, secrets) · checkHealth() · capabilities()
ImageProvider         listModels() · generate(ImageRequest) → ImageOperation (signals: progress, preview, finished, failed; cancel())
LlmProvider           listModels() · chat(ChatRequest) → ChatOperation (signals: delta, toolCall, finished, failed; cancel())
UtilityProvider       detectPose · removeBackground · upscale · interrogate
ProviderRegistry      factories, instances from config, role assignment, capability queries
```

`ImageRequest` is provider-neutral: mode (txt2img/img2img/inpaint/outpaint/edit), prompt, negative,
size, seed, steps, cfg, sampler, scheduler, model, batch, init image, mask, denoise, mask blur,
references (image + role: identity/style/composition + weight), controls (type: openpose/depth/canny
+ image + weight + range), provider-specific extras. Each adapter maps what it supports;
`capabilities()` drives the UI so unsupported controls are hidden/disabled with an explanation.

Settings forms are generated from `settingsSchema` (url, api key [secret], model [dynamic list],
timeouts, concurrency…), each provider has *Test connection* and *Refresh models*.

### 3.5 Document model

* `Project` – SQLite file: `meta`, `assets` (blobs + thumbnails + metadata JSON), `documents`,
  `layers` (tile blobs), `poses`, `characters`, `chats`, `messages`, `generations`, `jobs`.
* `Document` – size, layers (raster, with opacity/blend/visibility/lock), selection, inpaint mask,
  attached generation settings. Layers store pixels in 256×256 tiles (`ARGB32_Premultiplied`).
* Undo – `QUndoStack`; stroke commands store only touched tiles (memory-bounded history).
* Autosave – debounced background serialisation of dirty tiles; recovery on next launch.

### 3.6 Inpainting pipeline (provider-agnostic, "only masked")

1. Mask (8-bit) → bounding box → expand by padding (default 32 px) → aspect-correct region.
2. Crop composite image; scale region to the model's native resolution (multiples of 8/64, target
   megapixels per model family).
3. Dilate + blur mask for the request; send image + mask (or for mask-less APIs: image region +
   instruction, then rely on step 5).
4. Receive result, scale back to region size.
5. Composite onto a **new layer** with a feathered mask (+ optional colour match), so the user can
   compare, mask further, or discard. Outpainting extends the canvas and fills the new area with
   a mask automatically.

---

## 4. Step-by-step development plan

Each milestone ends with a buildable, tested, committed state. Acceptance criteria are verified
before ticking a milestone.

### M0 — Foundations
- [x] Git repo, `.gitignore`, `.gitattributes`, README, this plan
- [x] CMake project, presets (`mingw-debug`, `mingw-release`), warnings, module libraries
- [x] Application skeleton: `QApplication`, organisation/app names, high-DPI, single main window
- [x] Theme engine: dark/light palettes, QSS, SVG icon set tinted to theme
- [x] Logging (categories, rotating file log, redaction), crash-safe log flush
- [x] Settings service, paths (AppData, cache, logs)
- [x] Test harness (Qt Test, CTest) — **AC**: `cmake --preset mingw-debug && ctest` green, app launches with themed empty window.

### M1 — Core infrastructure
- [x] Async utilities (`runAsync`, `then` on GUI thread, cancellation token)
- [x] Job & JobQueue with per-provider concurrency, cancel, retry
- [x] HTTP client: JSON helpers, timeouts, retry/back-off, multipart, SSE parser, error mapping
- [x] SecretStore (wincred) + fallback interface
- [x] Image ops: async decode/encode, base64, scaling, mask dilate/erode/feather, compositing
- [x] JSON-Schema-subset validator
- [x] Mock HTTP server for tests — **AC**: unit tests for SSE, multipart, retry, mask ops, schema validator.

### M2 — Provider framework + first adapters
- [x] Interfaces, request/response model, capabilities, registry, settings schema
- [x] SD WebUI adapter (txt2img/img2img/inpaint/ControlNet/progress/interrupt/discovery/upscale)
- [x] OpenAI-compatible LLM adapter (streaming, vision, json_schema, tools, model list)
- [x] Providers settings dialog (auto-generated forms, test connection, refresh models)
- [x] Auto-detection of local servers on default ports — **AC**: contract tests against mock servers; manual test with real A1111 and llama.cpp.

### M3 — Canvas & editor core
- [x] Tiled raster layer, document, composite cache
- [x] GPU canvas view: zoom/pan/rotate, fit, checkerboard, pixel grid, overlays
- [x] Tools: move, free transform (move/scale/rotate/flip, selection-aware, lossless within a session), brush, eraser, rect/ellipse/lasso selection, mask brush/eraser, eyedropper, fill, crop
- [x] Layers panel (opacity, blend, visibility, lock, reorder, duplicate, merge)
- [x] Undo/redo with tile-delta commands, history panel
- [x] Open/import/export (PNG/JPEG/WebP), clipboard, drag & drop — **AC**: 60 fps brush on 4096² doc, undo memory bounded.

### M4 — Generation workspace
- [x] Project store (SQLite) + asset library + thumbnails (async)
- [x] Generation panel: provider/model picker, prompt/negative, size presets, seed, steps, cfg, sampler, batch, references, controls — capability-driven
- [x] Queue panel with live previews, cancel/retry
- [x] Gallery: virtualised grid, filters, favourites, metadata inspector, "send to editor/use as reference/re-run"
- [x] PNG metadata (A1111 `parameters` chunk) read/write — **AC**: end-to-end txt2img with SD WebUI, UI never stalls.

### M5 — AI editing
- [x] Inpaint pipeline (§3.6) for all image providers; outpaint; img2img variations
- [x] Instruction edit (edit-capable providers), upscale, background removal
- [x] Contextual generative toolbar on the canvas (prompt field near selection: Generate, Remove, more options; hidden while drawing; View menu toggle) — **AC**: inpainting a 4096² image modifies only the masked area; result on new layer.

### M6 — Pose system
- [x] Skeleton model, OpenPose renderer, JSON import/export
- [x] Pose editor: drag/FK/IK, mirror, presets, multi-person, hands/face toggles, reference underlay
- [x] 3D FK generation rig + projection; preset library
- [x] Pose detection from image (provider-based; ONNX optional) — **AC**: pose map works as ControlNet OpenPose input in A1111 and ComfyUI.

### M7 — Chat & Director
- [x] Chat panel: threads, streaming markdown, attachments (drag & drop, paste), image previews
- [x] Vision analyst → Character Sheet (editable, stored in library)
- [x] Director → Shot Plan (schema-validated, repair loop), purpose profiles (Live2D, sticker set, turnaround sheet, portrait set…)
- [x] Pose drafts board; per-shot approve/edit/regenerate pose
- [x] Batch generation with reference strategies; review board
- [x] Tool-calling assistant (plan, generate, edit, pose): `ai::AgentRun` loop (native tool calls, JSON-protocol fallback, step limit, cancel); studio tools (state, library, generate, edit, inpaint region, background removal, upscale, open, director: analyse references → plan shots → revise pose → generate shots); Assistant drawer on every page; the reference scenario runs end-to-end in `tst_canvas` against mock llama.cpp + SD WebUI — **AC**: reference scenario (§1.1) completes with SD WebUI + llama.cpp and with a cloud provider.

### M8 — Additional providers
- [x] ComfyUI adapter (workflow templates + bindings, uploads, WebSocket progress/previews, interrupt, object_info discovery) + bundled workflows
- [x] OpenAI Images, Google Gemini (LLM + image), Anthropic LLM, Stability AI, fal.ai — **AC**: contract tests for every adapter; manual smoke test per provider.

### M9 — Live2D & advanced
- [x] Layered PSD export (RLE) and import: editor documents (groups resolved, blend modes, opacity) and slicing sessions from layered PSDs
- [x] Expression/mouth/eye variant generation via masked inpainting (M11)
- [x] Background removal batch (part separation moved to M11): Library context menu runs background removal or 2× upscale on every selected asset; each result is a new asset linked to its source
- [x] Character library reuse across projects (`CharacterLibrary` in app data: sheet + references; save/load from the Create page, references imported as project assets); Critic loop (vision-model verdict per result with overall/identity/pose scores, issues and a revised prompt; shots below the minimum score are retried up to N times; verdicts on the shot card and in asset metadata)

### M10 — Polish & release
- [x] Onboarding wizard (detect local servers, add cloud keys), keyboard shortcuts editor (central `Shortcuts` registry with ids, scopes and persisted overrides; Settings → Shortcuts with search, primary/alternative keys, conflict moves, reset; a test asserts no conflicts across the whole window)
- [x] Autosave/crash recovery hardening (idle autosave 4 s after the last change plus the periodic one; open documents remembered per project and reopened; per-process session locks detect an unclean exit; crash reports + minidumps for SEH exceptions, `std::terminate` and `abort`), performance profiling pass (`tst_perf` budgets on a 4096² × 8-layer document; 64 KiB SQLite pages −40 % save time; word-wise mask bounds 26 → 2 ms; save phase timings and startup time in `lumen.perf`), latency watchdog (also in release, 250 ms)
- [x] i18n (Qt Linguist: `qt_add_translations`, embedded .qm under `:/i18n`, Qt's own translations deployed, language setting, German translation with `scripts/check-translations.py` validation; prompts sent to models are deliberately not translatable), accessibility names (application-wide naming of interactive widgets on polish; a test walks every page), high-contrast check (high-contrast theme following the Windows colours, "auto" theme, WCAG contrast tests for every theme; dark accent and light status colours adjusted)
- [x] Packaging (windeployqt via the CMake deploy API, NSIS installer through CPack, portable ZIP with a `portable.txt` marker → data in `userdata/`, exe icon and version info), user guide (`docs/USER_GUIDE.md`)

### M11 — Character part slicing for Live2D

Goal: turn one finished character illustration into a clean, layered set of body parts that a
rigger can import into Live2D Cubism (or StretchyStudio / Anime2.5DRig), with occluded areas
painted in. The approach follows [see-through](https://github.com/shitagaki-lab/see-through)
(SIGGRAPH 2026: LayerDiff 3D for transparent layer completion, Marigold for per-layer depth and
draw order, a body-parsing SAM for part masks; up to 23 semantic layers). see-through runs as one
monolithic pass. Lumen splits the job into **reviewable steps**: the user fixes the masks before
any expensive generation and can regenerate one part without touching the others.

Other references:
* ComfyUI-See-through (node-based decomposition plus PSD export).
* Grounding DINO + SAM/SAM2 (text-prompted masks).
* LayerDiffuse (native transparent generation).
* CubismPartExtr and the see-through `heuristic_partseg` (depth and left/right splits).
* PachiPakuGen (blink/lip-sync variants).
* StretchyStudio and Anime2.5DRig (in-browser 2.5D puppets from layered PSDs; model for our preview).

#### 11.1 Concepts

| Concept | Description |
|---|---|
| **Part set** | A slicing session bound to one source image and stored as a project record (`parts`). Holds the taxonomy, parts, settings and the cached decomposition. |
| **Taxonomy** | Ordered list of part kinds, with name, group (head/body/extra), default draw order, default colour, whether it is usually occluded, an LLM-ready description, and a parallax depth for the preview. Presets: **Live2D standard** (see-through V3 tags: back hair, front hair, headwear, face, irides, eyebrow, eyewhite, eyelash, eyewear, ears, earwear, nose, mouth, neck, neckwear, topwear, handwear, bottomwear, legwear, footwear, tail, wings, objects), **Live2D detailed** (adds L/R splits, upper/lower arm, side hair), **Simple** (hair, head, body, arms, legs, accessories), and **Custom**. |
| **Part** | id, name, kind (taxonomy tag), side (L/R/centre), colour, draw order, visibility/lock, **visible mask** (pixels of the part that are visible in the source; masks are mutually exclusive), **extension mask** (area where the part may continue behind other parts; generated there), state (*Empty → Masked → Queued → Generating → Ready → Approved*, or *Failed*), result layer (RGBA, full canvas), result variants, prompt override, provenance (auto/manual/split/merge). |
| **Decomposition cache** | Raw RGBA layers and depth returned by a see-through run, keyed by source image hash. Reused when the user re-applies masks or regenerates "from decomposition" without re-running the model. |

#### 11.2 Workflow (four steps, shown as a stepper on the **Parts** page)

1. **Source.**
   * Pick an image from the library, the editor or a file.
   * Optional one-click prep:
     * Background removal (alpha drives everything later).
     * Upscale to ≥ 1536 px on the long side.
     * Trim margins.
   * Choose a taxonomy preset.
   * The page explains the Live2D-friendly input requirements and links to the **Live2D pose presets** (11.5) so the user can generate a better source first.
2. **Masks.**
   * **Auto-segment** fills masks for every taxonomy entry, or for the selected ones, using the chosen segmenter (11.3). The result shows instantly as a coloured overlay; parts that came back empty are flagged.
   * Mask editing tools, all undoable:
     * **Paint / erase** on the selected part. Masks are stored as one label map, so they never overlap: painting a part takes the pixels from other parts (locked parts are protected, and *Only inside the character* keeps strokes within the silhouette). Overlap between parts is expressed by the hidden-area (extension) masks.
     * **Lasso / polygon** assign; **magic wand** (colour similarity within the character alpha); **grow / shrink / smooth / fill holes**.
     * **Add part**: pick a taxonomy kind or name a custom one, then paint or lasso.
     * **Split part**:
       * *by left/right* (centre line from the detected skeleton, or the mask's centre);
       * *by connected components* (each island becomes a part);
       * *by skeleton* (pixels assigned to the nearest bone: upper arm / forearm / hand / thigh / shin / foot, using the pose detector or the editable skeleton);
       * *by cut line* (user draws a polyline that cuts the mask; each side becomes a part);
       * *by brush* (paint the subset that should become a new part).
     * **Merge parts**, **reassign kind**, **rename**, **duplicate**, **delete** (pixels return to "unassigned").
     * **Unassigned** pseudo-part: shows character pixels not covered by any mask, so the user can see at a glance what is missing.
     * **Extension mask** brush (hatched overlay): marks where a part continues behind others. Defaulted automatically: the part's own mask grown by an adaptive radius, intersected with the parts drawn in front of it (hair behind the face, neck under the chin, torso under the arms, irides under the eyelids). Editable per part.
   * Part list (left panel):
     * Tree grouped Head / Body / Extras, with colour chip, eye (solo/hide), lock, draw-order drag handle, state badge and pixel-coverage indicator.
     * Multi-select supports bulk actions.
     * Filter "needs attention" (empty, tiny, overlapping, not generated).
3. **Generate.**
   * Scope buttons: **Generate all**, **Generate selected**, **Generate this part** (in the part's context menu and on its card).
   * Parts are queued as normal jobs (cancellable, retryable, one per part, honouring provider concurrency). Each job:
     1. Isolates the part: visible pixels kept; the extension area is the inpaint mask; everything else becomes neutral or transparent.
     2. Completes the hidden area with the chosen strategy (11.4).
     3. Cuts out the result: alpha = visible ∪ (extension ∩ generated alpha), feathered edges, colour-matched to the source.
     4. Stores the result layer and variants.
   * Prompts per part are composed from the taxonomy description, the character sheet (Director) and an optional per-part override. An LLM can draft them all at once ("Describe parts").
   * Review grid: one card per part with the result on a checkerboard, a before/after toggle, variant picker, *Approve*, *Regenerate*, *Edit mask* (jumps back to step 2 with the part selected) and *Open in editor*. Approval is optional; the stack preview updates live.
4. **Preview & export.**
   * **Live2D-style preview** (11.6) of the assembled stack.
   * Export:
     * **Layered PSD**: one layer per part in draw order, grouped Head/Body, named with the taxonomy/Cubism-friendly names, original canvas size.
     * **PNG folder + `parts.json` manifest**: name, kind, draw order, bounds, depth.
     * **Send to editor** as a layered document.
     * **Generate expression variants** (eyes closed, mouth open A/I/U/E/O, brows): masked inpaint on the eye/mouth parts, exported as extra layers. This is the PachiPakuGen-style material Live2D needs for blink and lip-sync.

The session saves automatically, so closing the page and coming back restores masks, results and
the current step. Every step is skippable: an advanced user can paint masks manually and go
straight to export ("cut only").

#### 11.3 Segmenters (pluggable, chosen in the step-2 toolbar)

| Segmenter | How | Notes |
|---|---|---|
| see-through via ComfyUI | Bundled workflow template (`see_through_decompose.json`) with `SeeThrough Load LayerDiff Model` → `SeeThrough Decompose` → per-layer outputs. Lumen downloads each RGBA layer; alpha → visible mask (after exclusivity resolution by depth); RGB → decomposition cache. | Best quality; needs the ComfyUI-See-through custom node and 8–16 GB VRAM. |
| Lumen Part Server (sidecar) | Small documented HTTP protocol (`GET /v1/info`, `POST /v1/segment`, `POST /v1/complete`; base64 PNG in/out). A reference Python server wrapping see-through ships in `tools/partserver/`. | For users without ComfyUI; also lets other segmenters be plugged in. |
| Grounding DINO + SAM via ComfyUI | Template with a text prompt binding; one run per selected part kind using the taxonomy's segmentation prompt. | Generic; good for accessories, weaker on fine facial parts. |
| Gemini segmentation | Vision LLM returns labelled boxes plus mask PNGs per requested label. | Cloud fallback without a local GPU. |
| Skeleton (local) | Character silhouette + body skeleton (fitted A-pose, detected by a pose provider, or placed by hand) → head/torso/limb masks by nearest bone (sided). | Always available, instant; a starting point for manual editing. |

Post-processing for all segmenters:
* Resolve overlaps by draw order/depth.
* Drop islands smaller than a threshold.
* Fill pinholes.
* Clip to the character alpha.
* Map returned labels to the taxonomy.
* Report unmatched labels as extra parts.

#### 11.4 Completion strategies (chosen in step 3, per run)

1. **Inpaint** (default; works with every inpaint-capable provider via the §3.6 region pipeline):
   * Mask = extension minus visible.
   * Context image = the isolated part over a neutral background.
   * Prompt = part description.
   * ControlNet/IP-Adapter references from the source where the provider supports them.
2. **see-through layers**: use the cached decomposition layer(s) of the matching kind, clipped to the part's visible ∪ extension masks. This is how user-split masks (e.g. left/right arm) still benefit from the model.
3. **LayerDiffuse via ComfyUI**: the bundled `complete_part_layerdiffuse.json` template (SDXL + ComfyUI-layerdiffuse) inpaints the hidden area and decodes RGBA, so the part's alpha comes from the model instead of colour keying. Selected per request (`extras.comfy_template`), never as the provider's main workflow.
4. **Cut only**: no generation, visible pixels only; instant and local.

#### 11.5 Live2D pose presets

Category **Live2D** in the pose library, used by the Director's `live2d` purpose profile.
Requirements built into every preset:
* Front-facing (or a slight 3/4 view).
* Arms held away from the torso, hands open with fingers visible.
* Legs slightly apart.
* Neutral face, eyes open, mouth closed, hair kept off the face.

Presets:
* A-pose full body.
* A-pose knees (for half-body models).
* Bust-up VTuber (arms relaxed, slightly out).
* Waist-up with hands visible.
* Slight 3/4 left / right (yaw ±15°).
* Arms raised for sleeve separation.
* Seated front.
* Turnaround (front/side/back).

Matching prompt hints ("flat lighting, no motion blur, simple background") are attached to the
profile.

#### 11.6 Live2D-style preview

* **Built-in puppet preview** (no external SDK). A GPU-free, QPainter-based 2.5D renderer over the part stack:
  * Per-part depth (taxonomy defaults, overridden by see-through depth) drives **parallax** for head angle X/Y and body angle.
  * **Mesh warp** (grid deformation) for hair sway and breathing.
  * **Blink** and **mouth open** by swapping in or interpolating the expression-variant layers (or scaling the eyelid parts when no variants exist).
  * **Auto idle** animation.
  * **Mouse-follow** (look at cursor).
  * Parameter sliders named like Cubism standard parameters (`ParamAngleX/Y/Z`, `ParamEyeLOpen`, `ParamMouthOpenY`, `ParamBreath`, `ParamHairFront`).
  * Renders at display refresh on a timer only while visible.
* **Cubism model preview**: the Cubism Core (proprietary; cannot be bundled) is loaded at runtime from the user's Cubism SDK for Native, so no build flag or SDK is needed to build Lumen. Lumen parses `.model3.json` / `.motion3.json`, drives the model through the Core C API and renders the drawables with its own band-parallel software renderer (textured meshes, normal/additive/multiplicative blending, clipping and inverted masks, multiply/screen colours). Plays the Idle motion, animates the standard parameters (angles, breath, blink, lip-sync), follows the mouse, and exposes every parameter as a slider. Physics (`physics3.json`) are simulated with the Cubism algorithm: particle strands driven by normalised inputs, fixed-step integration at the file's FPS with interpolated outputs, stabilised at load, optional wind. Motions from every group play with Cubism-style priorities and eased crossfades (per-motion and per-file fade times; the Idle group picks a random motion whenever nothing else plays); `EyeBlink`/`LipSync` model curves drive the model's groups; expressions blend Add/Multiply/Overwrite with fades; `pose3.json` groups switch parts with linked parts following.

#### 11.7 Implementation steps
- [x] Taxonomy + part-set model (`lumen_doc`): parts, masks (compressed), extension masks, results, record persistence, undo commands
- [x] Mask algorithms (`lumen_core`): connected components, left/right split, skeleton split, cut-line split, exclusivity resolution, island cleanup, adaptive extension masks, magic wand
- [x] Layered PSD writer (RLE, groups, names) and a minimal reader for round-trip tests
- [x] Segmenter framework (`lumen_ai`): `Utility::SegmentParts`, ComfyUI segmentation templates (bundled Grounding DINO + SAM; see-through graphs via the generic template mechanism), Lumen Part Server protocol + reference Python server, Gemini segmentation, local skeleton segmenter
- [x] Part completion service: inpaint / see-through layers / LayerDiffuse (ComfyUI) / cut-only, per-part jobs, variants
- [x] Parts page: stepper, part list, mask overlay canvas tools, split/merge/add, generation review grid, export
- [x] Live2D pose presets + Director `live2d` profile wiring
- [x] Puppet preview widget (parallax, mesh warp, blink/mouth, idle, mouse-follow)
- [x] Expression variant generation (eyes closed, mouth shapes)
- [x] Cubism model preview: runtime Core binding (`lumen_live2d`), model3/motion3 loading, software renderer, Parts page tab, physics, all motion groups, expressions and pose — **AC**: a generated A-pose character is segmented, its arm mask split left/right and re-generated for one side only, exported as a layered PSD that opens in Photoshop/Krita/Cubism with correctly named, ordered parts; the preview animates it; the UI never stalls during segmentation or generation.

### M12 — Managed local runtimes & model hub

Goal: a user with a GPU and no AI tooling gets a working local studio from inside Lumen. Today
every local back-end is a server the user installs, starts and configures elsewhere; Lumen only
connects to its URL. After M12 Lumen can **install** a local runtime (download from the
project's GitHub releases) or **adopt** one that is already installed. It **starts and stops**
the server itself, exposes its launch options as provider settings, and manages the **model files**
the servers load, including downloads from Hugging Face and Civitai (phase E). Remote
or externally started servers keep working as they do now (URL only).

#### 12.1 Why stable-diffusion.cpp becomes the default local image back-end

[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) (MIT) ships `sd-server`
(`examples/server`). The server has three APIs: a native async API under `/sdcpp/v1`, an
OpenAI-compatible API under `/v1/images/*`, and an A1111-compatible API under `/sdapi/v1`.

| | sd.cpp `sd-server` | ComfyUI | SD WebUI / Forge |
|---|---|---|---|
| Install | one zip, no Python (CPU 17.5 MB, Vulkan 30 MB, CUDA 12 338 MB + cudart 564 MB) | Python env, GBs, custom nodes for IP-Adapter / DWPose | Python env, GBs |
| Start-up / supervision | single native process, CLI flags | Python process tree | Python process tree, `webui-user.bat` |
| Progress / cancel | async jobs: `POST /img_gen` → 202 + job id, `GET /jobs/{id}` (state, preview, `b64_json` results), `POST /jobs/{id}/cancel` | WebSocket | `/progress`, `/interrupt` |
| Features Lumen uses | txt2img, img2img (`init_image`, `strength`), inpaint (`mask_image`), ControlNet (`control_image`, `control_strength`), **IP-Adapter for SD 1.5 / SDXL** (`ip_adapter_image`, ViT-H CLIP vision), LoRA (`lora[]`), hires fix, ESRGAN `/upscale`, VAE tiling, `capabilities` discovery (samplers, schedulers, LoRAs, upscalers, limits) | all, through workflow templates | all, through extensions |
| Low VRAM | GGUF quantisation of diffusion models, `--offload-to-cpu`, `--max-vram <GiB>`, `--clip-on-cpu`, `--vae-on-cpu`, `--diffusion-fa` | `--lowvram` | `--medvram` |
| Model switching | **none**: one model set per process, chosen on the command line, so a switch means a restart | per workflow | `/options` |

The table is from the server [README](https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/server/README.md)
and [api.md](https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/server/api.md),
the CLI option table in `examples/common/common.cpp`, and
[ip_adapter.md](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/ip_adapter.md).
The "sdcpp REST API" is the native `/sdcpp/v1` API of this same server, not a separate project.

The biggest gain for Lumen: identity references through IP-Adapter with no custom nodes. The live
tests showed that our plain ComfyUI setup cannot do this. The cost is the restart on every model
change. ControlNet, IP-Adapter, LoRA directory and upscaler are also start-up flags, so the set of
features a server offers depends on how it was started (§12.4).

Verified in the phase B spike (RTX 4060 Laptop, 8 GB; release `master-951-f89d9b1`, CUDA 12):
the xinsir SDXL OpenPose ControlNet steers the pose, and the SDXL ViT-H IP-Adapter carries a
character's look over from a reference.

| 832×1216, 28 steps, euler_a | f16 (weights streamed) | `--type q8_0` | q8_0 + `--offload-to-cpu` |
|---|---|---|---|
| text to image | 42 s | 32 s | 42 s |
| + OpenPose ControlNet | 90 s | 65 s | — |
| + ControlNet + IP-Adapter | 132 s | 76–81 s | 76 s |
| VRAM between images | 5.5 GB | 5.5 GB | 0.24 GB |

Findings:
* q8_0 images are practically identical to f16.
* Without VAE tiling, encoding the control image at full size runs out of memory on 8 GB.
* The plain IP-Adapter also copies the reference's pose: at weight 0.45 or more it overrides the
  pose map. Around 0.25–0.3 both work together.
* A job that is already generating cannot be cancelled (`cancel_generating: false`); queued jobs
  can.

#### 12.2 Concepts

* **Runtime**: an installed server program of a known *kind*: `llama.cpp`, `sd.cpp`, `comfyui`,
  `sdwebui`, or later `koboldcpp`. It is either:
  * *managed*: downloaded by Lumen into `appData/runtimes/<kind>/<tag>-<backend>/`. Lumen records
    repo, tag, asset name, sha256, backend and install date.
  * *adopted*: an existing install. Lumen records the executable or launcher path plus the version
    it probed. Lumen never modifies or updates adopted runtimes.
* **Server profile**: a runtime plus a launch configuration: models, port, GPU options, extra args
  and lifecycle policy. One runtime can have several profiles, for example "Qwen3-VL 8B vision" and
  "Qwen3 8B fast".
* **Binding**: a provider instance may name a server profile (`managedBy`). The provider stays a
  pure API client. The supervisor makes sure the server is up before the provider's first request.
  Providers without a binding behave exactly as today.
* **Model library**: the model files known to Lumen, typed and indexed. Server profiles reference
  library entries rather than raw paths, so models can be moved or rescanned.

#### 12.3 Runtime installer (GitHub releases)

* **Catalogue** (`resources/runtimes/catalog.json`, data, not code). It lists only pinned
  repositories, each with tag and asset patterns per platform and backend:
  * `ggml-org/llama.cpp`:
    * tags `b<N>` (daily builds);
    * assets `llama-b<N>-bin-win-{cpu,cuda-12.4,cuda-13.x,vulkan,…}-x64.zip`;
    * companion runtime `cudart-llama-bin-win-cuda-<ver>-x64.zip` for CUDA builds.
  * `leejet/stable-diffusion.cpp`:
    * tags `master-<N>-<sha>`;
    * assets `sd-master-<sha>-bin-win-{cpu,cuda12,vulkan,rocm-…}-x64.zip`;
    * companion `cudart-sd-bin-win-cu12-x64.zip`.
* **Release resolution**: `GET api.github.com/repos/{repo}/releases?per_page=N`, newest release
  whose tag matches the pattern. Not `releases/latest`: for llama.cpp it points to an unrelated
  `v0.6.0`, and the daily builds are flagged *prerelease*. Responses are cached with `ETag` /
  `If-None-Match` because of the 60 requests/hour unauthenticated limit. An optional GitHub
  token can go in the credential store.
* **Backend choice**:
  * Hardware probe on a worker thread: DXGI adapter list with dedicated VRAM, vendor id and driver
    version; `nvidia-smi --query-gpu` when present.
  * Recommendation: NVIDIA → CUDA (the build matching the driver's maximum CUDA version);
    AMD / Intel → Vulkan; no GPU → CPU.
  * The user sees each option's download size and can override, for example to pick Vulkan
    (33 MB) over CUDA 12.4 (265 MB + 391 MB cudart).
* **Download and install**, through a download manager shared with §12.7:
  * The download is a `Job` in the Queue, resumable with `Range` into a `.part` file, with a
    disk-space check up front.
  * **sha256 is verified against the release asset's `digest`** from the GitHub API. Assets
    without a digest are refused.
  * Extraction streams entries through zlib (the MinGW toolchain's, or the system's) and checks
    CRC-32. Before anything is written it rejects absolute paths, drive letters, `..`, symbolic
    links and encrypted entries, and every parent folder must resolve inside the target.
  * Files go into a staging folder that is renamed into place atomically. Lumen then runs
    `<exe> --version` and records the version.
* **Updates**: "check for updates" on demand, plus an optional weekly check. A running server is
  never updated: the update applies at the next start. The previous version is kept for
  **rollback**, two per kind by default.
* **Adopt existing**:
  * The user picks an executable or folder; Lumen detects the kind from the exe name
    (`llama-server.exe`, `sd-server.exe`), and records the version from `--version`.
  * ComfyUI Desktop: detected from `AppData/Local/Programs/ComfyUI` plus the base directory and
    `extra_models_config.yaml` in `AppData/Roaming/ComfyUI`, using the same launch line the live
    tests use (`--front-end-root …`).
  * ComfyUI portable: `python_embeded` + `main.py`. SD WebUI / Forge: `webui-user.bat` / venv.
  * Ollama and LM Studio stay external services. Lumen connects to them and may run
    `ollama serve`, but does not supervise their models.
* **Security**:
  * Only catalogue repositories, downloaded over HTTPS from GitHub hosts.
  * Digest verification is mandatory, and the only files ever executed are the catalogue's known
    server executables inside a verified install.
  * Nothing is elevated, and nothing is installed outside `appData/runtimes`.
  * llama.cpp and sd.cpp are MIT and downloaded by the user at run time; Lumen redistributes
    nothing.

#### 12.4 Process supervisor

* `ServerSupervisor` (new module `lumen_runtime`; depends on `core`, `net`; used by `ai` and `ui`)
  manages one `QProcess` per running server profile.
* **Process control**:
  * Each child is placed in a **Windows job object** with `KILL_ON_JOB_CLOSE`, so servers and
    their child processes (ComfyUI's Python tree) die with Lumen.
  * Servers bind to `127.0.0.1` only. The port is the kind's default, or the first free one.
* **Readiness**:
  * Probes: llama.cpp `GET /health` (503 while loading, 200 when ready); sd-server
    `GET /sdcpp/v1/capabilities`; ComfyUI `GET /system_stats`.
  * Model-loading progress is parsed from stdout where the server prints it, with a start-up
    timeout per profile.
* **Logs**: a ring buffer plus `logs/runtimes/<profile>.log`, viewable from the server's status
  chip and from Settings. Failure messages quote the last log lines, for example CUDA out of
  memory or a missing DLL.
* **Lifecycle policy per profile**:
  * Start on demand (default), at app start, or manually.
  * Stop after N idle minutes, and always on app exit (a "keep running" option may come later).
  * After a crash, restart with back-off, at most three times, then surface the error.
* **Status**: status-bar chips per server (stopped / starting / loading x % / ready / sleeping /
  error) with start, stop, restart, open log and copy command line.
* **Model switches**:
  * sd-server is restarted with the new model set. The job queue groups pending jobs by model set
    to avoid ping-pong.
  * A request needing a component that is not loaded (ControlNet for pose shots, IP-Adapter for
    identity) offers "restart with …", or the profile's auto-components handle it. The Director
    knows which components the planned shots need before generation starts.
  * llama.cpp: restart, or router mode (`--models-dir`) where the build supports it.
* **GPU arbitration** builds on `ImageProvider::releaseGpu()` and the deferred Critic:
  * The supervisor knows which servers share a GPU and how much VRAM is free (DXGI
    `QueryVideoMemoryInfo`).
  * llama.cpp profiles default to `--sleep-idle-seconds`.
  * sd-server cannot unload without exiting. Its `releaseGpu()` either stops the process (cost:
    a model reload) or is a no-op when the profile runs with `--offload-to-cpu` / `--max-vram`.
    The spike decides the default for 8 GB cards.

#### 12.5 Local provider settings

* Settings → **Local runtimes**:
  * the runtimes list: kind, version, backend, managed/adopted, disk size, update available;
    install / adopt / update / roll back / remove;
  * the server profiles list.
* Profile forms use the existing `settingsSchema` mechanism, plus an **Extra arguments** field and
  a read-only preview of the full command line.
* **llama.cpp profile**:
  * model and `--mmproj` (paired automatically when the library knows the projector);
  * context, `-ngl` or `--fit`, KV cache type, flash attention, threads, parallel slots;
  * `--jinja`, reasoning budget, `--sleep-idle-seconds`.
* **sd.cpp profile**:
  * checkpoint, or diffusion model + VAE + text encoders (FLUX / Qwen-Image style models);
  * ControlNet; IP-Adapter + CLIP vision; LoRA and embedding directories; upscaler;
  * `--offload-to-cpu`, `--max-vram`, `--clip-on-cpu`, `--vae-on-cpu`, `--diffusion-fa`,
    VAE tiling, threads.
* **ComfyUI profile**: launcher / Python, base directory, extra model paths,
  `--lowvram`/`--normalvram`, port, "Free VRAM when idle".
* **VRAM presets** from the hardware probe (8 / 12 / 16+ GB) fill sensible defaults. The tested
  8 GB setup from the user guide becomes the 8 GB preset.

#### 12.6 Model library & browser

* **Library roots**: a Lumen models folder (default `appData/models`, user-changeable) with typed
  sub-folders: `checkpoints`, `diffusion_models`, `vae`, `text_encoders`, `loras`, `controlnet`,
  `ipadapter`, `clip_vision`, `upscale_models`, `llm`, `mmproj`. Lumen can also scan existing
  folders in place, without copying:
  * ComfyUI `extra_model_paths.yaml` / `extra_models_config.yaml`;
  * the A1111 `models/` folder;
  * the LM Studio models folder;
  * any folder the user adds.
* **Index**: SQLite in app data, on a worker thread. Each entry holds path, size, mtime and type,
  plus:
  * **GGUF metadata**: `general.architecture`, quantisation, context length; mmproj (`clip`
    architecture) detection;
  * **safetensors header**: tensor-name heuristics for SD 1.5 / SDXL / FLUX / LoRA / ControlNet /
    IP-Adapter, and `__metadata__` such as `ss_base_model_version`;
  * **sha256**, computed lazily, cached and invalidated by mtime/size.
* **Enrichment**: `GET civitai.com/api/v1/model-versions/by-hash/{sha256}` returns name, base
  model, trigger words, preview images and the NSFW flag. Opt-in, because it sends file hashes.
* **Browser UI** (new page or Library tab):
  * grid/list with filters (type, base model, quantisation, size, source);
  * details pane: preview, trigger words, licence, source link, used by which profiles;
  * actions: use in profile, reveal in Explorer, rename, move, delete (to the Recycle Bin only).
  * **Fit hints**: estimated VRAM (weights + KV cache at the profile's context) against the
    detected VRAM gives "fits / partial offload / too large".
* **Starter sets** (curated `resources/models/starters.json`, per VRAM tier):
  * a vision LLM + mmproj (Qwen3-VL-8B Q4_K_M for 8 GB);
  * an SDXL anime checkpoint;
  * OpenPose ControlNet;
  * IP-Adapter SDXL + CLIP ViT-H;
  * an ESRGAN upscaler.

  "Set up local studio" downloads the set (total size shown first), creates the profiles and runs
  a test generation.

#### 12.7 Hugging Face & Civitai downloads (later phase)

* **Shared download manager**:
  * Queue jobs, one or two parallel downloads, resumable `.part` files, disk-space check.
  * Mandatory hash verification when the source publishes one.
  * A sidecar `<file>.lumen.json` records source, URL, revision, licence, trigger words and the
    NSFW flag.
  * Only `.gguf` and `.safetensors` by default. Pickle formats (`.ckpt`, `.pt`, `.bin`) are refused
    unless the user opts in per file with a warning. Downloaded files are never executed.
* **Hugging Face**:
  * Search: `GET huggingface.co/api/models?search=…&filter=gguf&sort=downloads&limit=…`.
  * Files: `GET /api/models/{repo}/tree/{revision}` gives sizes, and `lfs.oid` = sha256.
  * Download: `GET /{repo}/resolve/{revision}/{path}` (a 307 redirect to the CDN), with the
    revision pinned to the commit from `X-Repo-Commit` and `Range` resume.
  * Gated repositories need an HF token (Bearer, credential store). The licence comes from
    `cardData.license`.
  * For GGUF repos: a quantisation picker with sizes and fit hints, and the matching `mmproj-*`
    offered automatically.
* **Civitai**:
  * Search: `GET civitai.com/api/v1/models?query=…&types=Checkpoint|LORA|Controlnet&baseModels=…&nsfw=…`,
    with cursor paging.
  * Version details: `GET /api/v1/model-versions/{id}` returns files with `sizeKB`, `hashes.SHA256`,
    `downloadUrl`, `baseModel` and `trainedWords`.
  * Download: `GET /api/download/models/{versionId}` with `Authorization: Bearer <token>`. The
    token is in the credential store, never in the URL; many downloads require it.
  * NSFW results are hidden by default and previews blurred, per setting. The licence and
    commercial-use fields are shown before download.
* API keys and tokens live only in the OS credential store, as for cloud providers.

#### 12.8 Implementation steps

Phase A — foundations
- [x] `lumen_runtime` module:
  - hardware probe (DXGI + `nvidia-smi`, including the newer "KMD / CUDA UMD" banner);
  - download manager (resume, sha256, disk check, Queue jobs);
  - streaming zip extraction on zlib with zip-slip checks;
  - runtime catalogue + GitHub release resolver (tag patterns, digests, ETag cache).
- [x] Runtime store: install / adopt / update / roll back / remove; Settings → Local servers.
  Companion CUDA runtimes are unpacked once per digest and shared by later versions. An update
  moves the servers on the same build to the new version and keeps the old one.
- [x] `ServerSupervisor`:
  - job objects, ports, readiness probes, logs, restart policy, idle stop;
  - status-bar button;
  - provider ↔ profile binding with start-on-demand (`ai::ProviderGate`: generation, utilities,
    segmentation, the agent and structured chat wait for the server and release it afterwards).
  - Verified live: an adopted llama.cpp b10964 (CUDA 12.4) with Qwen3-VL-8B starts on the first
    request and answers in 8.3 s including the start; it stops in 0.8 s and frees the VRAM.

Phase B — stable-diffusion.cpp
- [x] Spike on the 8 GB test laptop (results in §12.1): WAI-Illustrious + xinsir OpenPose
  ControlNet + IP-Adapter SDXL ViT-H.
  - Measured: q8_0 against f16 and offload, VRAM, VAE tiling, and the IP-Adapter weight against
    the pose.
  - The server answers its health check about 3 s after start; models load lazily on the first
    request, so restarting for a model switch costs little more than that first load.
  - Still open: inpaint quality, and a side-by-side timing with ComfyUI.
- [x] `SdCppProvider` on `/sdcpp/v1`:
  - capabilities discovery, `img_gen` + job polling with queue position, per-pass progress and
    previews, cancel, `/upscale`;
  - ControlNet / IP-Adapter / upscaler flags follow the server's launch options;
  - the IP-Adapter weight is capped next to a pose map; works unmanaged too (URL only);
  - mock-server contract tests; `tst_live` `managedSdServer` (Lumen installs, starts and uses
    sd-server: 92 s for the first pose + reference shot including the start, 82 s for the next).
  - Model switching stays manual (edit the server's model; Lumen restarts it). Grouping queued jobs
    by model set waits until a model browser offers several models per server.
- [x] GPU sharing: sd.cpp profiles default to q8_0 + `--offload-to-cpu` (0.24 GB VRAM between
  images), and llama.cpp sleeps after 30 s idle, so `releaseGpu()` needs no stop/start.
  - The reference scenario with both servers run by Lumen (Qwen3-VL-8B + WAI-Illustrious with
    ControlNet and IP-Adapter, Critic on) passed: 5.0 min, Critic scores 8 and 6.
  - Peak VRAM 7.9 GB: the first image starts while llama.cpp has not yet unloaded, and sd.cpp
    streams weights until it does. An explicit "unload now" needs llama.cpp's router mode
    (phase C).

Phase C — llama.cpp management
- [x] llama-server profiles in **router mode** (`--models-preset`, `--models-max`):
  - Lumen writes the presets file: the main model plus "More models", each with an optional
    vision projector (found automatically next to the model). Shared settings go to `[*]`.
  - A build's options are read from `--help` at install/adopt; older builds fall back to
    single-model command lines.
  - The provider's default model stays a valid preset name.
  - GPU layers default to automatic (`--fit`).
  - Verified on b10964: presets load, `--models-max 1` swaps models, and `/models/unload` frees
    the VRAM in 2.5 s.
- [x] GPU handoff: before an image server's request, Lumen unloads the models of llama.cpp servers
  that have "Free the GPU for image generation" on (default).
  - The managed reference scenario now peaks at 6.8 GB instead of 7.9 GB, and the two servers never
    hold the GPU together.
  - Splitting roles across a vision model and a text model doubled the time (10.2 min instead of
    5.0) because every switch reloads a model, and Qwen3-8B mangled a tool call. So a vision model
    takes all roles by default.
- [x] VRAM presets ("Tuned for": up to 8 GB / 12 GB / 16 GB+) for llama.cpp, sd.cpp and ComfyUI;
  new servers start with the detected GPU's preset.
- [x] Role defaults for new llama.cpp providers: unassigned roles go to the server's vision model
  (or its first model).
- [x] Adopted ComfyUI and SD WebUI / Forge start and stop through the supervisor.
  - ComfyUI Desktop (base folder, venv, extra model paths and front-end from its config), the
    portable build and git checkouts with a venv are recognised from the files; nothing is run to
    detect them.
  - WebUI / Forge starts through `webui.bat` with `COMMANDLINE_ARGS` (`--api --port …`).
  - Verified live: Lumen adopted ComfyUI Desktop 0.18.5, started it (ready after 14.7 s), listed
    its checkpoints and stopped it, leaving no Python process behind.
  - SD WebUI / Forge is covered by unit tests only (no install on the test machine).

Phase D — model library
- [x] Header inspection (`ModelFormats`):
  - GGUF and safetensors headers are read without loading weights.
  - Classification comes from tensor names and metadata: checkpoints (SD 1.5 / 2 / SDXL / SD 3 /
    FLUX), diffusion models (FLUX, SD 3, Qwen-Image, Anima / Cosmos), LoRAs (base family, trigger
    phrase), ControlNets, IP-Adapters (SD 1.5 vs SDXL by width), CLIP vision, VAEs (latent
    channels), text encoders, upscalers, embeddings, LLMs (architecture, quantisation, context,
    layers / KV heads) and vision projectors.
  - Pickle files are never unpickled; folder names classify them.
  - Checked against the 22 model files on the test machine.
- [x] `ModelLibrary`:
  - Lumen's models folder with typed sub-folders, plus folders scanned in place.
  - A JSON index next to the settings (instead of SQLite): unchanged files are not read again.
  - Lazy SHA-256; opt-in Civitai lookup by hash per file (page, base model, trigger words, a safe
    preview).
  - Folder suggestions: ComfyUI / WebUI installs, ComfyUI Desktop, LM Studio, Downloads, and
    folders of models the servers use.
  - Removal to the recycle bin.
- [x] Settings → Models:
  - search and type filter, family / precision / size / folder, and GPU fit ("Fits / Partly /
    Too large" from the weights plus a 16k KV cache, or SDXL at q8_0 with activations);
  - details with Civitai info and preview, "used by" servers, "use with a server" (new or
    existing), show in folder, recycle bin;
  - model file fields of the server dialog get a "choose from the library" menu.
- Moved to phase E: starter sets and "Set up local studio" (they need the Hugging Face
  downloads).

Phase E — hub downloads
- [x] `ModelHub` (runtime):
  - Hugging Face search (`/api/models`, GGUF / text-to-image / LoRA / ControlNet filters, cursor
    paging from the `Link` header); details pin the current commit and list the files from the
    tree API with their LFS SHA-256.
  - Civitai search (`/api/v1/models`, types, `nsfw=false` by default, adult models also dropped
    client-side, safe preview images); versions with files, SHA-256, format, precision, trigger
    words and the licence permissions.
  - Downloads are Queue jobs (group `model-download`, two at a time). They resume, check the
    hub's SHA-256 and refuse pickle files unless allowed.
  - The file goes into Lumen's models folder, in the sub-folder for its kind. After the
    download the header decides the kind and the file moves if the name-based guess was wrong.
  - A `<file>.lumen.json` sidecar records source, revision, licence, trigger words and checksum;
    the library reads it, so downloaded files need no hashing or Civitai lookup.
  - A same-named different file is never overwritten.
- [x] `FileDownload` follows redirects itself: `Authorization`/`Cookie` only go to the original
  origin (Hugging Face and Civitai redirect to CDNs with signed URLs); https → http is refused;
  retries start again from the original URL because signed links expire.
- [x] Tokens: Hugging Face (gated repositories) and Civitai (most downloads need an API key) in
  the credential store (`hub/<source>/token`), edited under *Accounts…*; 401/403 explain which
  token is missing.
- [x] Settings → Models → **Download…**:
  - search both hubs; a model's files with precision, size, GPU fit and "in the library";
  - the suggested file is the best common quantisation that fits the GPU (Q8_0 → Q6_K → Q5_K_M →
    Q4_K_M …);
  - a GGUF model offers its vision projector (`mmproj-*` and `*.mmproj-*` names, Q8_0 preferred);
  - split GGUF files are downloaded with all their parts.
- [x] Companion files (added October 2026): Civitai versions of models that are not complete
  checkpoints (Anima, Qwen-Image-based models…) carry their VAE and text encoder as separate
  files ("VAE", "Text Encoder"), some with `metadata.isRequired` (only in the search results,
  not in `/model-versions`; the site's own API is closed to clients).
  - `HubFile` keeps Civitai's file type and the required mark; a "Text Encoder" file is a text
    encoder, not a checkpoint. `companionFiles` gives a model file the VAE / text encoder files
    of its version (deduplicated by checksum).
  - The download dialog downloads them with the model unless already in the library (required
    ones always; the others behind a ticked box). They are recorded as `companions` in the
    model's Civitai data: in the sidecar of a download (added later with `recordCompanions`
    when the model came first), and by *Look up on Civitai* for files from elsewhere, which keeps
    an earlier required mark.
  - Models page: the model's details list the companions with "missing" / "in the library";
    **Download missing files** fetches them with the model's Civitai data (`companionDownload`).
  - Servers: *Use with a server* and the sd.cpp profile's model fields fill `vae`, and `llm` or
    `t5xxl` for a text encoder of that type (the header now tells LLM / T5 / CLIP text encoders),
    when empty; switching models removes the previous model's companions. Missing ones are
    reported.
  - Tests: parsing, selection and JSON round trip; download with recorded companions, missing ones
    fetched, server options, a later record, the lookup of a file from elsewhere; the dialog
    (required box, a companion's own row, download of all three) and the Models page (a missing
    VAE fetched again).
- [x] Anima (October 2026). sd.cpp runs Anima since March 2026 (`--diffusion-model`, `--vae`
  Qwen-Image VAE, `--llm` Qwen3-0.6B-Base; docs/anima.md); the installed master-951 does.
  Measured with MiaoMiao Harem 2.9B (a bare DiT + LLM adapter, 5.6 GB bf16) on the RTX 4060
  laptop: 1024², 30 steps, weights in RAM: 157 s sampling, 188 s in all; the full-size VAE decode
  ran out of VRAM and sd.cpp tiled it by itself. Through Lumen with its defaults (q8_0: 3.8 GB of
  weights instead of 7.0; VAE tiling): 832×1216, 28 steps, 87 s sampling, 99 s with the server
  start; with "safe" the outfit stayed covered.
  - Headers: Wan-layout VAEs (`decoder.conv1` 5D) get the family "Qwen-Image / Wan" (16
    channels) or "Wan 2.2" (48); language-model text encoders get a variant ("Qwen3 0.6B",
    "Qwen2.5 7B") from the q/k norms and the width. `kModelInspectorVersion` (2) makes the
    library read files indexed by an older inspector again, keeping hashes and lookups.
  - `familyCompanions("Anima")`: the VAE and text encoder pinned to circlestone-labs/Anima
    (commit, SHA-256, size, non-commercial licence). A model without recorded companions gets its
    family's; a library file with the same header family / variant stands in (a Qwen-named one
    first), else it is offered for download from that repository.
  - Prompt style "anima" (by the family Studio passes as `model_family`, or "anima" in the name,
    not Animagine): tags, "masterpiece, best quality, score_7, " in front, the model card's
    negative prompt; with *Safe for work* the "safe" rating tag goes after the quality tags.
    CFG 4.5 (the model card suggests 4–5): `PromptStyle::cfg` is the recommended guidance (6 for
    other models, as before). `GenerationService` applies it unless the request says
    `cfgChosen`, which only the Generate page sets; its CFG slider moves to the recommendation
    when the model family changes and keeps the user's value otherwise (a re-run sets the model
    before the image's CFG). Steps stay 28.
  - Tests: header families and variants, re-inspection; Anima companions with stand-ins under
    other names, a missing one from Hugging Face, server options; the prompt style and the safe
    tag; the safe tag reaching the server. Live (`animaServer`): the user's model through Lumen's
    library, server and defaults.
- [x] Starter sets (`resources/models/starters.json`): compact (Qwen3-VL 4B Q4_K_M), standard
  (8B Q4_K_M, 8 GB GPUs) and large (8B Q8_0, 12 GB+). Each set adds Animagine XL 4.0 Opt, xinsir
  OpenPose ControlNet, IP-Adapter SDXL ViT-H, its CLIP ViT-H encoder and Real-ESRGAN x4plus.
  All files are pinned to a commit with their SHA-256 and licence.
- [x] **Set up local AI** (Local servers page and the welcome dialog):
  - hardware → recommended set → plan with sizes, licences and free disk space;
  - files already on the computer (same checksum, or same name and size) and installed programs
    are reused;
  - missing programs install from GitHub and missing files download from Hugging Face (Queue);
  - then the llama.cpp and sd.cpp servers are created with the GPU preset, as providers. The
    language model takes the free roles and the image server becomes the default image provider.
  - The setup finishes when the window is closed meanwhile. Running it again changes nothing.
  - "Generate a test image" at the end.
- Tests: redirects keep the token at home, hub search parsing (both hubs), details + downloads
  (kind correction, sidecar, reuse, name clash, pickle refusal, Civitai 401), starter sets
  (pinned and complete, tier choice), the wizard end to end with fake servers and a mock hub.
  Live (`LUMEN_LIVE_HUB=1`): both hubs searched, Qwen3-VL files listed with the pinned checksum,
  Real-ESRGAN (64 MB) downloaded through the CDN redirect in 5 s and recognised as an upscaler.
- Not verified yet: AC 1 on a clean machine (this machine already has the models; the full
  17 GB standard set was not downloaded again).

**AC**:
1. On a clean Windows machine with an NVIDIA GPU, a user installs llama.cpp (CUDA) and sd.cpp
   from Settings, downloads the 8 GB starter set, and runs the reference scenario (§1.1) with the
   Critic. They never open a terminal or another app.
2. Lumen starts both servers on demand, alternates them on one GPU without CPU fallback, and
   stops them on exit. No orphan process remains after a crash of Lumen.
3. A tampered download (digest mismatch) is rejected.
4. An adopted ComfyUI Desktop install starts and stops from Lumen.

Open questions:
* Can CUDA builds of llama.cpp and sd.cpp share one cudart download? The DLL sets differ; check.
* Run several sd-server instances (one per model set) when VRAM allows, or always one?
* Should ComfyUI become installable? Its portable build is a 7z with a Python environment, so the
  plan only adopts it for now.
* Linux and macOS asset patterns come later; the catalogue format already keys them by platform.

### M13 — Stable characters (character LoRA)

Goal: a character keeps exactly the same look in every image (face, hair, eyes, outfit, colours,
proportions) while the pose, framing, expression and background change. The user designs the
character once, Lumen trains a **character LoRA** from it, and from then on the character is part
of every generation that uses it, from any page, through the Director or the assistant.

#### 13.1 Why a LoRA

A LoRA (low-rank adaptation) is a small add-on to a base model, typically 20–200 MB for SDXL. It
changes the model's weights so that a **trigger word** (e.g. `lmn_akane`) produces this one
character. Because the identity lives in the weights rather than in the prompt or a reference
image, it holds across poses, and it combines with pose control (ControlNet), inpainting and
img2img.

| Method | Identity | Pose freedom | Cost | In Lumen |
|---|---|---|---|---|
| Prompt + character sheet | weak: details drift | full | none | M7 |
| IP-Adapter (reference image) | partial: colours and style, details drift | the reference's pose leaks in at weight ≥ 0.45 (measured, §12.1) | none | M12 phase B |
| Face adapters (InstantID, PuLID, FaceID) | strong for faces, made for photos | full | none; extra models | — |
| Textual inversion (an embedding) | weak for complex outfits | full | ~30 min training | — |
| **Character LoRA** | **strong: face, hair, outfit, palette** | **full** | **1–2.5 h training on 8 GB (estimate)** | **M13** |
| Full fine-tune / DreamBooth checkpoint | strongest | full | GBs per character; beyond 8 GB | — |

LoRA and IP-Adapter combine well. The LoRA carries the identity; a low-weight IP-Adapter can add the
reference's exact colours for one image. A LoRA belongs to a model family: an SDXL /
Illustrious / Pony LoRA works on checkpoints of that family, not on SD 1.5 or FLUX.

#### 13.2 Using LoRAs (all image back-ends)

* Providers:
  * sd.cpp: the `lora[]` field of `img_gen`, with files from the server's `--lora-model-dir`.
  * ComfyUI: a `LoraLoader` binding in the workflow templates.
  * SD WebUI / Forge: `<lora:name:weight>` in the prompt.
  * `ImageRequest` gains `loras: [{file, weight}]`; `capabilities()` reports LoRA support and the
    loaded model family.
* Character library (`CharacterLibrary`, M9): a character gains a LoRA entry: file, trigger word,
  recommended weight, base model family, training data and its report (§13.6).
* Generation, Director and assistant: choosing a character with a LoRA adds the trigger word and
  the LoRA to every request. Lumen warns when the selected checkpoint is from another family (read
  from the safetensors metadata, e.g. `ss_base_model_version`).
* LoRA files from elsewhere (the model library, M12 phase D; Civitai, phase E) can be attached to a
  character too.

#### 13.3 Dataset builder

A LoRA learns from 15–40 varied images of the character. They come from what Lumen already does:
* **Source:** the character sheet and reference images (M7), or a character designed in Lumen
  first (Create page).
* **Shot template "LoRA dataset":** a Director purpose profile.
  * Full body front / three-quarter / side / back.
  * Upper body and portrait close-ups.
  * Three or four expressions, a few hand and arm poses (rig presets, M6).
  * Plain and simple backgrounds, so the background does not become part of the character.
  * Generated with pose control + IP-Adapter (sd.cpp), several variants per shot.
* **Curation grid:**
  * The Critic scores identity against the references; the user keeps or rejects each image and
    can regenerate or inpaint fixes.
  * Near-duplicates are flagged.
  * Images are cropped and bucketed for training (1024-class sizes for SDXL).
* **Bootstrapping:** with only one reference, a first LoRA trained on the best images generates a
  more consistent second set, and v2 is trained on that.

#### 13.4 Captions

* The vision model (Qwen3-VL) writes booru-style tags per image (the dialect of anime SDXL models)
  and the trigger word is put first.
* Tags that describe the fixed identity (hair colour, eye colour, the signature outfit) are removed,
  so the LoRA ties them to the trigger word. Pose, view, expression, framing and background tags
  stay, so they remain controllable.
  * "Outfit is part of the character" can be switched off, keeping outfit tags so outfits can be
    changed by prompt.
* Captions are editable in a table: search and replace, add or remove a tag on all images.

#### 13.5 Trainer runtime

* Training needs Python and PyTorch with CUDA; sd.cpp only loads LoRAs. The reference trainer is
  **kohya-ss/sd-scripts** (Apache-2.0; `sdxl_train_network.py`), the most common SDXL LoRA trainer.
* **Managed install**, extending the M12 runtime store:
  * `astral-sh/uv` from its GitHub releases (one executable, digest-verified) creates a private
    venv under `runtimes/trainers/`.
  * It installs a pinned sd-scripts release and its lock-filed requirements (PyTorch CUDA wheels
    from the PyTorch index, about 4–5 GB) with hash checking.
  * The catalogue gains a "python environment" kind with these sources and their pins.
* **Adopt existing:** kohya_ss (GUI bundle), plain sd-scripts, or later OneTrainer / ai-toolkit
  through their CLIs.

#### 13.6 Training job

* A Queue job driving a supervised process:
  * dataset folder (`<repeats>_<trigger>`), captions and a generated TOML config;
  * progress parsed from the trainer's output: step, epoch, loss, ETA;
  * a sample image per epoch (fixed seeds and poses);
  * cancel, and resume from the saved state;
  * a log viewer like the servers' (M12).
* GPU: training needs the whole card, so the supervisor stops or unloads every server first and
  restarts them afterwards. The status bar shows the training.
* Presets, starting points to be measured on the 8 GB test laptop (sd-scripts documents 8 GB as
  possible, 10 GB recommended):
  * **8 GB:**
    * U-Net only, network dim 8–16, alpha dim/2;
    * Adafactor, gradient checkpointing, cached latents and text-encoder outputs on disk;
    * bf16 or fp16 + `--no_half_vae`, batch 1, 1024 buckets;
    * about 1,500–2,500 steps (≈ 25 images × 8 repeats × 10 epochs).
  * **12 / 16 GB+:** larger dim, the text encoders trained too, batch 2–4.
* **Evaluation:**
  * Every saved epoch renders a test grid: four poses × weights 0.6 / 0.8 / 1.0, plus a "no LoRA"
    column.
  * The Critic scores identity against the references and checks for overfitting (copied
    backgrounds, pose stiffness).
  * Lumen recommends an epoch and a weight; the user picks.
* **Result:** the LoRA file goes to the library's LoRA folder (which the sd.cpp server's
  `--lora-model-dir` points to) and is attached to the character with its trigger word, weight,
  base model, dataset and report.

#### 13.7 Implementation steps

- [x] LoRA use:
  - `ImageRequest.loras` (`LoraRef`: name, file, weight) and `ImageCapabilities.loras`.
  - Each provider asks its server for its LoRA list first and matches by path (the server's
    relative path is a suffix of the file) or by name. A missing LoRA fails at once with a clear
    message, before anything is generated.
    - sd.cpp: `/sdcpp/v1/capabilities` → `lora: [{path, multiplier}]`.
    - ComfyUI: `/object_info/LoraLoader` → a LoraLoader chain behind every checkpoint loader. The
      loader's model and CLIP users are rewired, so user templates get LoRAs too.
    - WebUI / Forge: `/sdapi/v1/loras` → `<lora:name:weight>` in the prompt.
  - Character ↔ LoRA (`CharacterLora`: file, trigger word, weight, family):
    - kept in the character library (`character.json`) and the Director session; a re-save keeps
      the LoRA;
    - Create page → Character → **LoRA…**: the library's LoRAs or any file, the trigger word
      (from the file's metadata, Civitai or the download notes), weight;
    - checks: the provider cannot use LoRAs, the file is missing, another base family than the
      provider's checkpoint (from the headers), or the Lumen-run sd.cpp server's LoRA folder does
      not hold the file (one click points it there and restarts the server);
    - every shot gets the LoRA and the trigger word first.
  - The setup wizard points the image server at the library's `loras` folder.
  - Measured (RTX 4060 8 GB, WAI-Illustrious, q8_0 + offload, 1024², 20 steps, the same seed):
    33.4 s without and 35.8 s with `pixel-art-xl`. All 2,166 LoRA tensors were applied at
    runtime in 0.5 s, and the style change is obvious.
  - ComfyUI and WebUI are covered by unit tests, not live yet.
- [x] Dataset builder:
  - "LoRA training set" purpose. With a character sheet, `Director::datasetPlan` builds the plan
    without the language model (24 shots by default, up to 32):
    - views: front, three-quarter both ways, side, back, looking back;
    - framings: full body, cowboy shot, upper body, portrait;
    - six expressions, eight arm and leg poses;
    - five plain backgrounds.
    Any prefix of the list covers views and framings evenly. Prompts are the sheet's appearance
    tags plus view, framing, expression, pose and background tags (no expression on back views).
    The rig poses drive the OpenPose ControlNet.
  - `doc::LoraDataset` lives in `characters/<id>/dataset`: images plus `dataset.json` with source,
    shot, kept, Critic score and critique, difference hash, near-duplicate link and caption.
  - Create page → Character → LoRA → **Training set…**:
    - plan the training shots; add the session's results (Critic verdicts from the Create page
      carry over); add files;
    - score with the Critic (the lower of identity and overall; images below the minimum are
      rejected);
    - find near-duplicates (64-bit difference hash, ≤ 6 bits; the lower-scored copy is
      rejected);
    - keep or reject by hand, with a count against the 15 / 20 image goals.
  - The session remembers its library character; the set needs one, so Lumen offers to save it
    first.
  - Export: kept images cropped to the standard SDXL buckets (1024², 896×1152, 832×1216,
    768×1344, 640×1536 and their landscape twins; SD 1.5: 512-class), keeping more of the top,
    with a `.txt` caption per image.
  - Tests: plan coverage, appearance fallback, hash, buckets, crop, dataset bookkeeping and export,
    and the window end to end (plan → generate → import → Critic → duplicates).
  - Measured (RTX 4060 8 GB, WAI-Illustrious q8_0 + offload, pose ControlNet + IP-Adapter at 0.3,
    24 steps): the first 8 plan shots took 585 s (about 73 s each). 24 shots take about 30 min, or
    about 1 h with two variants.
    - The character stayed recognisable in every view.
    - The back view lost the hood: the kind of image the curation step rejects.
    - The reference's grey background sometimes beats the requested one (IP-Adapter).
    - Three-quarter views use 45°; 35° read as almost frontal.
- [x] Captioning:
  - `Director::tagImage`: the vision role answers with Danbooru tags (schema `{tags}`, 15–40 tags,
    the model's `long_red_hair` spelling turned into `long red hair`).
  - Identity pruning (`ai/LoraCaptions`) removes these tags:
    - hair colour, length and style; eye colours (not eye states such as closed eyes); body features
      (ears, tails, horns, skin, moles…);
    - the sheet's own tags;
    - with *Outfit is part of the character* (default), the outfit and accessories, including the
      model's own words for them: a shared stem of 4+ letters matches hood / hooded / hoodie and
      cloak / cloaked.
    View, framing, pose, expression, background and lighting stay. The trigger word goes first and
    keeps its underscores.
  - The training set stores raw tags, caption, a hand-edited flag and the settings (trigger word,
    default `lmn_<first name>`; outfit switch).
  - Changing either setting rebuilds the captions. Hand-edited captions keep their words and only
    get the new trigger.
  - Captions tab: an editable table per kept image, the most common tags, find and replace, add or
    remove a tag on every caption, rebuild from tags. Export writes the trigger word alone for an
    image without a caption.
  - Measured (Qwen3-VL 8B Q4_K_M, llama.cpp, RTX 4060): 8 training images tagged in 57–141 s
    (6–18 s each). 36–40 identity tags were pruned.
  - What stays is pose, expression, framing, background, and clothes the sheet does not list
    (boots); the common-tags line shows those for one-click removal.
- [x] Trainer runtime (`runtime/Trainer`, `resources/runtimes/trainers.json`):
  - Pins:
    - uv 0.12.18 by its GitHub release digest;
    - CPython 3.10.21 (sd-scripts' tested version), installed by uv, which checks its own hashes;
    - sd-scripts v0.12.0 by commit `690ea7f` and the source archive's SHA-256;
    - the packages by hashed lock files (`uv pip compile --generate-hashes`, 68 packages each,
      PyPI plus the PyTorch index).
  - Two variants: CUDA 12.8 / PyTorch 2.8.0 (also RTX 50) and CUDA 12.4 / PyTorch 2.6.0. The
    newest one the NVIDIA driver supports is chosen.
  - Install job: uv → source → Python → venv → `uv pip install --require-hashes --no-deps` → a check
    that imports torch and the trainer's library and needs `torch.cuda.is_available()`. Then the
    staging folder becomes `runtimes/kohya/v0.12.0-cu128` and uv's cache (several GB) is deleted.
  - uv runs with its own Python and cache folders under `runtimes/tools`, and ignores user uv
    configuration and `VIRTUAL_ENV` / `PYTHONPATH`.
  - Runtime kind `kohya` (catalogue role "trainer"). Settings → Local servers → Download… lists it
    with the recommended variant, sizes and licence. No servers can be created from it.
  - Add existing… adopts an sd-scripts checkout or a kohya_ss install with its venv.
  - Commands go through `RuntimeStore::processRunner`, which tests replace with a simulation.
  - Measured (RTX 4060, 8 GB; driver CUDA 13.3): the real install took 370 s and needs 7.4 GB
    on disk. PyTorch 2.8.0+cu128 sees the GPU.
- [x] Training job (`runtime/LoraTraining`, `ui/CharacterTraining`):
  - `trainingArguments` builds the `sdxl_train_network.py` (or SD 1.5 `train_network.py`) command
    line, with no TOML file. Presets:
    - 8 GB: U-Net only, rank 16 / alpha 8, cached text-encoder outputs, Adafactor;
    - 12 GB: rank 32, batch 2;
    - 16 GB+: text encoders too, AdamW8bit, batch 2.
    All use bf16 / full_bf16, gradient checkpointing, SDPA, cached latents, 1024 buckets and a
    state per epoch (the last one kept).
  - Cached text-encoder outputs need fixed captions: the trainer refuses `--shuffle_caption` then,
    so captions are only shuffled (trigger kept first) when the text encoders train.
  - Repeats: images × repeats ≈ 200 steps per epoch (25 images → 8).
  - The job reserves the GPU (`ServerSupervisor::reserveGpu`): running servers stop, start()
    answers why, and servers that run all the time come back afterwards.
  - Progress comes from tqdm (step, epoch, loss, time left, s/step) and a known out-of-memory
    message. The trainer's own error line goes into the failure message. A `train.log` is kept per
    run. Cancel keeps the state for *Continue the stopped training*.
  - CharacterTraining runs under the Studio, so a training finishes after its window closes:
    1. it exports the kept images to `training/run-<time>/img/<repeats>_<trigger>`;
    2. it trains;
    3. it copies the LoRA into the library's `loras` folder with a sidecar (source "training",
       trigger, base model, epochs, images);
    4. it attaches the LoRA to the character, and an open Create session follows.
  - Training set → **Train** tab:
    - trainer (or *Install the trainer…*), base model (the image generator's model by default),
      preset by GPU size, epochs;
    - steps and an estimated time; a warning under 15 images;
    - start, stop, continue; progress and the result.
  - Disk: a finished run keeps its epoch files, test results, run.json and log; the trainer's
    states (93 MB each for an SDXL rank-16 LoRA, two per run) and the exported images are deleted.
    *Remove earlier trainings…* deletes every run but the newest (the library keeps its copies).
  - Tests: arguments per preset and family, progress parsing, outputs and state, GPU
    reservation; the Train tab end to end with a simulated trainer, the tidy-up and removing
    earlier runs.
  - Measured (RTX 4060 8 GB, WAI-Illustrious, 8-GB preset):
    - 3.1 s per step at 1024²;
    - 6.6 GB peak GPU memory (fp8 base not needed);
    - an 85 MB SDXL LoRA that Lumen's header reader recognises;
    - a 16-step run took 104 s including model loading and caching.
    - a full run (8 images × 25 repeats × 6 epochs = 1,200 steps) took 58 min: 2.84 s per step,
      6.7 GB peak GPU memory. Loss fell from 0.083 after epoch 1 to 0.063 at the end; six 85 MB
      epoch files plus two 93 MB training states.
- [x] Evaluation (`ui/LoraEvaluation`, Train tab → *Test the result*):
  - Four test shots (`Director::loraTestShots`): cheering outdoors, three-quarter on a street, side
    view running on a beach, a tilted portrait in a café. These poses, views and backgrounds are not
    in the training recipe. Every image gets the appearance tags, as shots planned in Create do;
    LoRA candidates add the trigger word.
  - The baseline is the same shots without a LoRA, with the reference through IP-Adapter: what
    the user had before training.
  - A first live run prompted the candidates with the trigger word alone. The 8-image LoRA kept
    hair and eyes but dropped the outfit in new scenes (bikini on the beach, hoodie in the café):
    5.7–6.6 against 8.9 for the baseline. That measured the LoRA in a way Lumen never uses it.
  - Live run with the fair prompts (RTX 4060 8 GB, WAI-Illustrious, the 8-image LoRA from the
    full training run, Qwen3-VL-8B as Critic): 24 images and 24 critiques in 45 min.
    - Means: baseline 9.08; epochs 4 / 5 / 6 at 0.8: 8.67 / 8.67 / 9.17; epoch 6 at 0.6 / 1.0:
      8.83 / 9.08. Recommended: epoch 6 at 0.8.
    - Identity alone: 9.75 with the LoRA against 9.5 for IP-Adapter. The LoRA rows look alike
      across scenes; the baseline added cat ears, pom-poms and an orange tint in one shot.
    - The margin is small: four shots, one Critic pass, and scores near the top of the scale. A
      25-image set and a stricter Critic prompt are needed to settle AC 2.
  - Acceptance run through the UI (`TestLive::characterLoraEndToEnd`, October 2026, RTX 4060 8 GB):
    - Create → Training set → *Plan training shots* (30) → generate: 42 min, none failed. The
      Critic kept 29 (scores 5–10), no near-duplicates; captions in 3.5 min.
    - Train tab, 10 epochs: 29 images × 7 repeats = 2,030 steps in 122 min (3.55 s per step while
      other work used the CPU). During training both servers were stopped and a start request was
      refused with the reason; afterwards the reservation was released and the test step started
      them again on demand.
    - Two bugs found and fixed on the way: the Train tab did not follow images added after the
      dialog opened ("Start training" stayed disabled), and refreshing it decoded the test grid's
      images on the GUI thread every progress tick.
    - Test step with the identity checklist: every scored image, with and without the LoRA, had
      identity 10 (all five features "match"). Scores differed only by the pose: baseline 8.83,
      epochs 8 / 9 / 10 at 0.8: 8.67 / 8.83 / 8.83. In the grid the LoRA rows are visibly more
      consistent (the baseline added pom-poms and an orange tint to the hair), which a checklist of
      listed features does not see.
    - One image generation was lost by the server and one Critic call failed (HTTP 500 from
      llama-server's router) while the two servers took turns on the GPU. The test then stopped
      but was saved as finished, with a recommendation from stage 1 only. Fixed: a failed
      generation or check is retried once and otherwise left unscored, and only a complete test
      is saved as finished.
  - Stage 1: the baseline and the newest three epochs at weight 0.8. Stage 2: the best epoch at
    0.6 and 1.0. That makes 24 images, with a fixed seed per shot so only the LoRA differs.
  - The Critic scores every image against the references: (2 × identity + pose) / 3. On a tie the
    earlier epoch and the lower weight win (less overtraining, more room for prompts).
  - Identity is a checklist (`Director::checkIdentity`), not the Create Critic's 0–10 guess, which
    gave 9–10 to anything close. The features are the face plus the sheet's appearance tags (subject
    tags like 1girl left out, at most 12). The vision model marks each as match, close, different or
    not visible; the schema has one enum per feature. Identity = 10 × (match + ½ close) / visible
    features, so a missing choker or another garment costs points. One call per image also returns
    the pose score. The grid's tooltips list the verdicts (✓ ≈ ✗ –).
  - The candidates are copied into the Lumen-run sd.cpp server's LoRA folder (`lumen-eval/`;
    the server rescans it per request) and removed afterwards. Images and scores are kept in
    `run-*/eval/eval.json`.
  - A grid of thumbnails and scores per epoch and weight, the recommendation, and *Use epoch N at
    weight W*. Another row can be picked. The chosen epoch goes into the library as
    `<trigger>-eNN.safetensors` with a sidecar and is attached to the character with that weight.
  - Tests: the whole test through the Train tab against a simulated sd.cpp server (`fake_server
    --fake-proxy` in front of a mock) and a scripted Critic: request contents, the recommendation,
    the grid, and applying a chosen row.

**AC**:
1. From one reference image of an anime character, a user builds a 25-image dataset, trains an
   SDXL LoRA on the 8 GB test laptop without leaving Lumen, and attaches it to the character.
2. Ten new images of that character in different poses (pose control) and expressions keep the
   same face, hair, eyes, outfit and palette. The Critic's identity score is higher than with
   IP-Adapter alone on the same shots.
3. The servers are back to normal after training.

Risks and open questions:
* 8 GB is the lower limit for SDXL LoRA training; if quality or time is not acceptable, offer
  training on a cloud GPU (e.g. fal.ai / Replicate LoRA trainers) as an option.
* Generated training images can carry artefacts into the LoRA; the Critic filter and the user's
  curation matter more than the image count.
* A LoRA inherits the licence terms of its base model (e.g. Illustrious and Pony derivatives).
* FLUX / Qwen-Image LoRAs need 16–24 GB and are out of scope for 8 GB machines.

### M14 — The character everywhere

Goal: once a character exists (sheet, references, LoRA), every image tool in Lumen draws that
character without the user repeating it. The Generate page, the editor's AI tools, Live2D part
completion and expression variants, and the assistant all use the character's LoRA when the image
back-end can load it, and its reference or description when it cannot. This closes the gap between
M13's goal ("from any page") and what M13 built: today only the Create page's shots use the LoRA.

#### 14.1 Where the character is missing today

Audit of every place that builds an `ImageRequest` (October 2026):

| Place | What it knows about the character | LoRA |
|---|---|---|
| Create page shots (`CreatePage::generateShot`) | session sheet, references, library entry | yes (M13) |
| Generate page (`GeneratePage::generate`, *Re-run*) | only references added by hand | no; *Re-run* also drops `loras` from the metadata |
| Editor AI panel: fill selection, variation, instruct edit, expand (`AiPanel::run`) | only references added by hand | no |
| Live2D part completion (`PartService::complete`) | a free-text "Character description" box | no |
| Expression variants (`PartService::generateExpression`) | the same text box | no |
| Assistant `generate_image`, `edit_image`, `inpaint_region` | reference ids the LLM passes, if any | no |
| Assistant `generate_shots` | via the Create page | yes |

Utilities (background removal, upscale, pose detection, segmentation) take no prompt and stay as
they are. The Live2D scenario (§1.1) suffers most: part completion and the eye and mouth variants
must look like the same character, and a sentence of description is the weakest way to get there.

#### 14.2 The active character

* One **active character** per project, chosen from the character library. It is stored in the
  project, so reopening a project restores it.
* A character chip in the prompt area of the Generate page, the editor's AI panel and the Parts page:
  name, thumbnail and a LoRA badge. It opens a picker (library characters, "None") and has a
  per-page switch "Use the character", on by default once a character is active.
* The Create page sets the active character when its session loads or saves a library character,
  so a character designed there is ready on the other pages.
* Parts page: a part set made from a Create image finds its character through the source asset's
  `director_session` metadata and that session's library character. It falls back to the active
  character. The character's sheet replaces the free-text box, which stays as an override.
* The assistant sees the active character in its state (name, summary, LoRA yes/no) and can set it
  (`set_active_character`).

#### 14.3 Applying a character to a request

One function replaces the Create page's own code: `CharacterContext::apply(ImageRequest &, caps,
mode)`. It goes down a ladder by what the back-end can do:
1. **LoRA** (sd.cpp, ComfyUI, WebUI with a model of the LoRA's family): trigger word first, the
   LoRA at the character's weight. A family mismatch skips the LoRA with a warning, as in M13.
2. **Reference image** (IP-Adapter, multi-reference edit APIs): the character's first reference,
   with the `Character` role.
3. **Description**: the appearance tags (tag-style back-ends) or the sheet summary (prose).

Rules per mode:
* txt2img, img2img, variations: the LoRA, else one reference; the appearance tags always go into
  the prompt (the first M13 live test showed that the trigger word alone can lose the outfit).
  No reference next to a LoRA: IP-Adapter pulls the reference's pose into the image.
* Inpaint and expand: the LoRA (or reference) only. The user's prompt describes the region; the
  appearance tags would repaint hair or clothes into a hand fix.
* Instruct edit: reference or description only; edit models do not take LoRAs.
* Background prompts ("remove", the parts page's key-colour fill) never get the character.
* Every request records the character id and how it was applied in its metadata (`describe()`),
  so *Re-run* and the Library show it, and *Re-run* restores the LoRA.
* Cloud back-ends without LoRAs say once per session what they use instead ("Gemini cannot load
  LoRAs: using Akane's reference image").

#### 14.4 Live2D parts with the character

* Part completion: the character's LoRA plus the part prompt, for the Inpaint and LayerDiffuse
  strategies. This needs a LoRA binding in the LayerDiffuse ComfyUI template; the standard graph
  already has one (M13).
* Expression variants: the LoRA keeps the eye shape and colour and the mouth style of the
  character. Each variant gets a check of the changed region against the source face: the identity
  checklist (M13) on a face crop, so a variant that changes the eye colour is flagged before the
  rigger sees it.
* Optional, if the measurements in step 6 show a gain: a LoRA weight lower for small regions
  (eyes, mouth) than for whole parts. Start with the character's own weight.

#### 14.5 Implementation steps
- [x] `CharacterContext` (`ai/CharacterContext`):
  - `applyCharacter(request, character, caps, use, style, modelFamily)` with four uses: `Full`,
    `Region`, `Edit`, and `Shot` for the Create page (LoRA and every reference that fits, no
    description: the planned prompt already has it). Nothing is added twice; references already
    in the request count against the provider's limit.
  - The family check compares the LoRA's family with the loaded checkpoint's; an unknown family
    does not block the LoRA.
  - `CharacterApplied` records what went in, as metadata (`character`, `lora`, `trigger`,
    `lora_weight`, `reference`, `description`, `lora_skipped`), and words the fallback note.
  - The Create page builds its shots through it (`Shot`) and stores the metadata; its tests are
    unchanged and pass.
  - Tests: every use on a LoRA back-end, a multi-reference cloud API and one with neither; tag and
    prose prompts; a family mismatch and an unknown family; no duplicates; the metadata.
- [x] Active character:
  - `Studio::activeCharacter()` / `setActiveCharacter()` with `activeCharacterChanged`; stored as
    the project record `active_character`, restored when the project opens, cleared when it closes
    or when the character leaves the library. `Studio::characterContext(id)` loads a library
    character (sheet, references, LoRA) for requests.
  - `CharacterChip` (`ui/widgets`): round thumbnail, name, "· LoRA"; its menu lists the library,
    "No character" and "Use the character on this page" (remembered per page, struck through when
    off). The pages get it in steps 3–5.
  - The Create page makes its session's library character active when it changes (saving or
    loading a library character, switching sessions), not when the page first shows a session at
    startup, so the restored choice stands.
  - Assistant: `get_state` includes `active_character`; new tools `list_characters` and
    `set_active_character`; the system prompt says to name the character only when none is active.
  - Test: the chip, the per-page switch, the menu, reopening the project, the Create page hook,
    the assistant tools, and a character removed from the library.
- [x] Generate page and *Re-run*:
  - The chip sits in the prompt header. With a character, `generate()` loads it
    (`Studio::characterContext`) and applies it with `CharacterUse::Full` and the checkpoint's
    family (`checkpointFamily`) before submitting; txt2img and img2img alike.
  - Metadata: `character` (what went in) and `user_prompt` (the words typed).
  - The fallback note is shown once per provider and character.
  - *Re-run* restores `user_prompt` (not the prompt with the trigger and tags) and the character,
    switched on here. It says when the character's LoRA has changed since or the character is
    gone. An image drawn without a character switches it off on this page, so the re-run matches.
  - Test: a mock sd.cpp server gets the trigger first, the description and the LoRA at the
    character's weight; the metadata; the per-page switch; *Re-run* both ways.
- [x] Editor AI panel:
  - The chip sits in the prompt header ("editor" switch).
  - `AiPanel::withCharacter` loads the character and applies it before the composite is taken.
    Uses: fill selection and a variation of a selection → `Region`; a variation of the whole
    image → `Full`; instruct edit → `Edit`; expand → `Region`. The edit's metadata records the
    character.
  - The selection bar's *Remove* calls `runFromCanvas(…, false)` and never gets the character.
  - Test against a mock sd.cpp server: fill selection (trigger and LoRA, no tags), remove (none),
    whole-image variation (LoRA and tags), expand (LoRA, no tags), switched off (none), metadata.
- [x] Parts page, character and LoRA:
  - `CompletionOptions` and `ExpressionOptions` carry the character and the checkpoint's family;
    `PartService` applies it as `Region` to every request (provider resolved like
    `GenerationService`: an empty id is the default provider) and records it in the part's
    metadata. The appearance words go in through `characterSummary` only; an empty description
    box falls back to the character's appearance tags.
  - LayerDiffuse needed no template change: `ComfyUiProvider::insertLoras` already rewires
    per-request templates, so the LoRA sits between the checkpoint and `LayeredDiffusionApply`.
  - Opening an image for slicing makes its character active: the image's `character` metadata,
    else the `library_character` of its `director_session` record (Create images before M14).
  - Tests: an expression variant on WebUI with the LoRA tag and trigger (description once); a
    LayerDiffuse completion whose graph has the LoraLoader feeding LayerDiffuse, with the trigger
    first and the metadata; the Parts page adopting the character from both kinds of image, and
    leaving it alone for an image without one.
- [x] Face check of expression variants (`ui/parts/ExpressionCheck`):
  - After each variant, the face area (three times the variant's size, at least 256 px) is cropped
    before and after; `Director::checkIdentity` gets the after crop, the before crop first among the
    references, then the character's reference. The shot names the planned expression, so the pose
    score says whether it shows.
  - Features marked "different", or a pose score of 4 or less, become the part's `warning`
    (persisted; shown with a warning sign and tooltip in the part list). Adding or choosing
    another result clears it. Without a character the description box gives the features.
  - "Check each variant with the Critic" on the Parts page, on by default; without a vision model
    the check does nothing.
  - Test: the crops; a changed eye colour, a clean check and a weak expression against a mock
    vision model (three images sent: result, face before, character reference); the warning
    cleared by a new result.
- [x] Assistant tools:
  - `generate_image` (`Full`), `edit_image` (`Edit`) and `inpaint_region` (`Region`, now on the
    default provider explicitly) go through `submitWithCharacter`, which applies the active
    character unless the call passes `character: false`, records it in the metadata, and tells
    the model in the result what went in (`character: {name, used, note}`).
  - The tool descriptions say that the character comes automatically, so prompts describe only
    the scene; each tool has the `character` parameter.
  - Test against a mock sd.cpp server: generate (LoRA and description, metadata, the result's
    `character`), `character: false`, inpaint (LoRA, no appearance words), edit (description, no
    LoRA), and no active character.
- [x] Live measurements (`TestLive::characterEverywhere`, RTX 4060 8 GB, WAI-Illustrious, the M13
  character "Akane" with its epoch-9 LoRA at 0.8; 57 min):
  - Generate page, 8 prompts that never name the character, same seeds: checklist identity 10.0
    on all 8 with the character, 0.41 on average without (AC 1).
  - Editor, *Fill selection* on hand, face, sleeve and hair: no new difference with the character;
    without it the face fix changed the eye colour (and tinted the hair orange, which the checklist
    did not catch) (AC 2).
  - Expression variants: not conclusive. The test placed the eye and mouth regions from the pose
    rig the portrait was generated with; the drawn face sits elsewhere, so the repaint mostly
    missed the eyes (expression scores 3–6 with and without). Needs real part masks
    (segmentation or painted) to measure; the requests carrying the LoRA are covered by the
    contract tests.
  - Found on the way: `promptStyleFor` treated stable-diffusion.cpp as a cloud model, so anime
    checkpoints on the default local back-end got prose prompts without quality tags; fixed.
  - Seen in the images: WAI-Illustrious sexualises some results unprompted (open cloak, swimwear),
    with and without the LoRA. Decided: no "nsfw" in the default negative, but a "Safe for work"
    switch (status bar shield and Settings → General, off by default) that
    `GenerationService::execute` applies to every request whose provider takes a negative prompt.
  - Not measured live: the assistant drawing the character (AC 4) and the cloud fallback (AC 5);
    covered by the mock tests.

**AC**:
1. With a trained character active, a Generate-page prompt that never names the character ("reading
   in a library, evening light") produces that character: the checklist identity is at least 2
   points higher than the same prompt without the character. *Re-run* repeats it with the same LoRA.
2. A hand fix and a face fix with *Fill selection* in the editor keep the character: the checklist
   finds no new "different" feature in the region.
3. For a Create-page image of the character, the Parts page completes the occluded parts and makes
   the five expression variants with the character's LoRA without any setting by the user. The
   requests carry the LoRA (contract tests), and no variant changes the eye colour or face shape
   (face-crop checklist).
4. "Draw Akane at the beach" in the assistant uses her LoRA through `generate_image`.
5. On a cloud back-end without LoRAs, the same prompts fall back to the reference image or the
   description, and Lumen says which.

Risks and open questions:
* One active character per project, or one per page? Per project is simpler and matches how people
  work on one character at a time; scenes with two characters need regional prompting and two
  LoRAs, which is a later milestone.
* Inpainting with a character LoRA can pull the whole style towards the training images; the
  measurements decide whether inpaint needs a lower weight.
* LayerDiffuse with a LoRA is untested on 8 GB; it may not fit next to the transparency model.
* Candidates for M15: cloud LoRA training (fal.ai, Replicate) for machines under 8 GB and for
  FLUX / Qwen-Image LoRAs; two characters in one image (M16).

### M15 — Cloud LoRA training (RunPod)

Goal: train a character LoRA on a rented cloud GPU in about 20–30 minutes instead of two hours on
the 8 GB laptop, and on machines without a suitable GPU at all. The LoRA is the same as a local
one: the same pinned trainer, the same recipe, the user's own base checkpoint.

#### 15.1 Why a rented GPU, not a hosted trainer

Hosted LoRA trainers (October 2026) train FLUX (fal.ai `fal-ai/flux-lora-fast-training`, Replicate
`fast-flux-trainer`) or base SDXL (community trainers on Replicate). None takes the user's own
checkpoint, and a LoRA should be trained on the family it is used with (Illustrious, Pony,
NoobAI with v-prediction). Lumen's characters are drawn with anime SDXL checkpoints, so Lumen runs
its own trainer (M13 §13.5) on a rented GPU. A hosted FLUX trainer can be a second back-end later.

#### 15.2 RunPod

* REST API v2 (`https://api.runpod.io/v2`, bearer key; v1 is retired on 15 November 2026):
  `POST /v2/pods` (`name`, `image`, `gpu {id, count, minCudaVersion}`, `cloud`, `disk`, `env`,
  `ports`, `cmd`), `GET /v2/pods/{id}` (`status`: PROVISIONING / STARTING / RUNNING / EXITED /
  ERROR / TERMINATED, `cost` per hour), `GET /v2/pods`, `DELETE /v2/pods/{id}`, and
  `GET /v2/catalog/gpus` (VRAM, `price.secure` / `price.community` per hour). Errors are RFC 9457
  problem objects. The field names come from the docs; the first live run confirms them.
* The pod runs a stock PyTorch image; HTTP ports are served at
  `https://<pod>-<port>.proxy.runpod.net` (HTTPS through Cloudflare, connections cut after
  100 s). So every request is short: uploads and downloads go in chunks, progress is polled.
* The API key lives in the OS credential store (SecretStore), never in settings or projects.

#### 15.3 The pod agent

A Python script shipped with Lumen (`resources/runtimes/cloud/pod_agent.py`, standard library
only), passed to the pod in an environment variable and started by the pod's command. It:
1. installs sd-scripts at the pinned commit (archive SHA-256 checked) and the hashed lock file
   (M13), in the background, and reports its phase;
2. accepts the training set as a zip and the base checkpoint either from a URL (Hugging Face or a
   public link, checked against its SHA-256) or as chunked uploads;
3. runs `sdxl_train_network.py` (or `train_network.py`) with the arguments Lumen builds
   (`trainingArguments`, with the pod's paths and the 24 GB preset), and streams the log;
4. serves the epoch files in chunks;
5. exits after a set lifetime or an idle hour even if Lumen never comes back.
Every request carries a random token generated for this pod (passed in the pod's environment).

#### 15.4 In Lumen

* `runtime/RunPod`: the API client (pods, GPU catalogue), with mock-server tests.
* `runtime/CloudTraining`: a Queue job like the local one (`startTraining`), with the same
  progress parsing: create the pod → wait for the agent → upload → train → download the epochs →
  terminate the pod. Cancel and failure terminate it too.
* Pods carry the name prefix `lumen-train-`; at start Lumen lists its own leftover pods and offers
  to terminate them, so a crash never leaves a paid GPU running.
* Train tab: *Where* — "This computer" or "RunPod cloud GPU". For the cloud: the GPU (from the
  catalogue, 24 GB or more), its price per hour and the estimated cost and time; a confirmation
  before every start, since it spends money.
* Settings → Cloud training: the API key (stored in the credential store), the preferred GPU,
  Secure or Community cloud.
* The base checkpoint's URL comes from its library sidecar when it was downloaded from Hugging Face
  (M12 phase E); otherwise Lumen uploads the file (6–7 GB, warned with the time it will take).

#### 15.5 Implementation steps
- [x] RunPod client (`runtime/RunPod`): `gpus()` (sorted by Secure price), `createPod`, `pod`,
  `pods(prefix)`, `terminate` (a 404 counts as terminated). Create and delete are never retried
  (a retry could start a second, paid pod). Errors carry RunPod's problem `detail` and validation
  `errors`; 401/403 say the key was refused. The key is read from the credential store
  (`lumen/runpod-api-key`); the Studio owns one client.
  - Settings → Cloud training (`ui/dialogs/CloudTrainingPage`): the key (save, forget, test),
    Secure or Community cloud, the preferred GPU from the catalogue (offered in that cloud, 24 GB
    or more, with its price per hour).
  - Tests: the client against a mock API (body of a new pod, statuses, listing by prefix,
    terminate twice, a validation problem, a refused key, no key); the settings page's GPU list,
    preferred GPU, cloud switch and error.
- [x] Pod agent (`resources/runtimes/cloud/pod_agent.py`, standard library) and its client
  (`runtime/PodAgent`):
  - The agent's setup mirrors the local install: uv 0.12.18 for Linux (release SHA-256 pinned in
    `trainers.json`), sd-scripts at the pinned commit (archive SHA-256), CPython 3.10.21 through
    uv, then `uv pip install --require-hashes` of a Linux lock. The lock
    (`kohya-v0.12.0-cu128-linux.txt`, 82 packages) was compiled from sd-scripts' requirements
    plus torch 2.8.0 / torchvision 0.23.0 cu128: the same versions as the Windows lock, plus the
    Linux NVIDIA wheels and triton. A check imports torch and the trainer's library and needs the
    GPU.
  - Requests carry the pod's token (constant-time compare). Uploads are offset-checked chunks
    (a 409 tells the size the agent has, so a repeated chunk is harmless); downloads are
    chunks verified by SHA-256 at the end; paths cannot leave the work folder; zips are checked
    for unsafe names. `{work}` in the trainer's arguments is replaced (and Windows separators
    turned around), so Lumen builds them with `trainingArguments`.
  - The agent exits after an idle hour or eight hours in any case.
  - `writeZip` (stored entries) packs the training set.
  - Tests start the real agent with the local Python (skipped without one): token refused,
    chunked upload with SHA-256, fetch from a URL with a good and a bad checksum, zip and unzip,
    training refused before setup, the fake trainer's log and two epochs downloaded and verified,
    a tampered checksum, a path outside the work folder, cancel, shutdown; and the Linux recipe.
- [x] Cloud training job (`runtime/CloudTraining`, Queue group "cloud-training"):
  - Pack the set (`writeZip`) → create the pod (`lumen-train-<name>-<time>`, the agent base64 in
    `LUMEN_AGENT` with a random 192-bit token, port 8000/http, the pod's command decodes and
    runs it) → wait for RUNNING → wait for the agent → upload the Linux lock and start the setup,
    which installs on the pod while the set (then unzipped) and the model travel; the model is
    uploaded, or fetched by the pod from a URL with its SHA-256 → wait for setup and model →
    train with `trainingArguments` built for `{work}` paths and the 24 GB preset → mirror the
    log to `train.log` and the progress (shared `describeTrainingProgress`) → download the
    epochs, verified → terminate the pod. The result map is the local one plus `pod` and
    `costPerHour`; the status shows the cost so far.
  - Every end terminates the pod: success, failure, cancel. Termination is tried twice; if it
    still fails the job carries a warning naming the pod. A pod whose creation answer arrives
    after a cancel is terminated on arrival. Polls tolerate six failures in a row (proxy
    hiccups) before giving up.
  - `leftoverCloudPods`: Lumen's pods not used by a training in this process and not terminated.
  - Tests with a fake RunPod API that starts the real agent here from the create request's
    environment, and a fake sd-scripts: a whole run (pod request, uploads, log, three epochs
    downloaded, pod terminated), cancel during training, a base model with a wrong checksum
    (failure, pod terminated) and one fetched by URL, and leftovers.
- [x] Train tab and costs:
  - *Where*: "This computer" or "Cloud GPU (RunPod, paid by the hour)", remembered. In the cloud
    the trainer and VRAM rows are hidden (the 24 GB preset is used, no local trainer needed); a
    GPU list loads from the catalogue (24 GB or more, offered in the chosen cloud, with its price;
    the preferred GPU from settings is preselected and a new pick becomes the preferred one).
    Without a key the tab links to Settings → Cloud training.
  - The summary adds the cost: steps × 1.2 s (estimate for a 24 GB RTX 4090-class GPU) plus about
    12 min to set up the GPU, at the GPU's price.
  - Every start asks first (`confirmCloudTraining`): the GPU, price per hour, time and cost
    estimate, whether the base model is uploaded (with its size) or downloaded by the pod from
    Hugging Face, and that RunPod bills every minute. Cancel rents nothing.
  - `CharacterTraining` with `cloud`, `gpuId`, `community` runs `startCloudTraining` and then the
    same post-processing as a local training (library, sidecar, `setLora`); `run.json` and the
    LoRA's sidecar note `where` (and the GPU). A cloud training cannot be continued, so the resume
    button is local only. `checkpointSource` gives the pod a Hugging Face URL and SHA-256 from the
    model's sidecar; Civitai models are uploaded (their token stays here).
  - Start-up (`MainWindow::checkLeftoverCloudPods`, 3 s after the window shows, with a saved key):
    Lumen's leftover pods are listed with their status and cost per hour, and shut down on
    confirmation.
  - Tests (mock RunPod): the cloud rows, the GPU list and saved choice, the cost line, the
    confirmation (text, Cancel rents nothing), the pod request (GPU, Secure cloud, name prefix),
    RunPod's refusal shown on the tab, `run.json`; the leftover-pod offer and the shutdown.
- [ ] One short live run on RunPod (the user's account): the 29-image Akane set, 10 epochs.
  **Postponed (13 October 2026)** at the user's request; everything is ready:
  - The key is saved and the live catalogue matches the client (`tst_live runPodPrices`,
    read-only): RTX 4090 $0.89/h Secure, $0.34/h Community; RTX 3090 $0.50 / $0.22;
    L40S $1.09 / $0.79. The estimate for the run: about 1,015 steps, 30–35 min with setup,
    about $0.50 on a Secure 4090.
  - The base model (WAI-Illustrious v17, 6.9 GB) has a byte-identical Hugging Face copy,
    `LyliaEngine/waiIllustriousSDXL_v170` (SHA-256 f116b0c7…), so the pod fetches it instead of
    an upload (`CharacterTraining::Settings::checkpointUrl`).
  - To run: `LUMEN_LIVE_RUNPOD=1 LUMEN_LIVE_CLOUD_CHARACTER=07de8f46f4dd4d41
    LUMEN_LIVE_CLOUD_CHECKPOINT=<…/waiIllustriousSDXL_v170.safetensors>
    LUMEN_LIVE_CLOUD_CHECKPOINT_URL=https://huggingface.co/LyliaEngine/waiIllustriousSDXL_v170/resolve/main/waiIllustriousSDXL_v170.safetensors
    LUMEN_LIVE_CLOUD_CHECKPOINT_SHA256=f116b0c78ff441467b0cdc8f1936e1ed18ea31e9997c7b132b1b8db533f0bd04
    tst_live cloudLora` (in the tst_live data folder).
  - Found with the live catalogue: it lists AMD GPUs (MI300X, MI350X), which cannot run the CUDA
    trainer; the GPU lists now leave them out (`RunPodGpu::runsTrainer`).

**AC**:
1. With a RunPod API key, a user trains the character LoRA from the Train tab on a cloud GPU; it
   arrives in the model library and on the character, like a local training.
2. Before starting, Lumen shows the GPU, its price per hour and the estimated total cost; it never
   starts a pod without the user's confirmation.
3. Cancel, a failed training and a closed Lumen never leave a pod running: the pod is terminated,
   or the next start finds and terminates it; the agent also exits on its own.
4. The API key is stored only in the credential store.

Risks and open questions:
* RunPod REST v2 is new; field names may still change. The client keeps them in one place.
* Upload speed decides whether a local checkpoint is practical (6.5 GB at 20 Mbit/s is about
  45 min); checkpoints from Hugging Face are downloaded by the pod instead.
* Civitai downloads need the user's Civitai token; passing it to a rented machine is a choice the
  user makes per run (off by default: upload instead).
* GPU availability varies; the catalogue's availability and a fallback list of 24 GB GPUs help.


### M16 — Two characters in one image

Goal: draw two of the user's characters together ("Akane and Bo at a café") so that each keeps
its own look, from the Generate page, the Create page, the editor and the assistant. Today a scene
with two characters either loses one of them or blends them: both LoRAs and both descriptions go
into one prompt and apply to the whole image.

#### 16.1 What assumes one character today

Survey of the code (October 2026):

| Part | Today | For two characters |
|---|---|---|
| Active character (`Studio::activeCharacter`, `CharacterChip`) | one id, one chip | a cast of up to two |
| `CharacterContext` / `applyCharacter` | one character per call; a second call would stack its LoRA and trigger on the whole prompt | per-character requests, each in its own region |
| `ImageRequest::loras` | a list, but every back-end applies all LoRAs to the whole image | LoRA per region |
| Poses (`pose::Pose::people`, `solvePose`, renderer, Pose page) | several people already; `Person::bounds()` | used as the layout |
| Director (`PlanInput.character`, shot schema, `poseForShot`) | one sheet, one rig per shot; "1girl, solo" in prompts, "2girls" in the negative | two sheets, a rig per person, subject tags for two |
| `GenerationService` regions (`RegionInput`, crop → generate → composite) | one region per spec | one refine pass per character |
| Identity check (`checkIdentity`, checklist) | one character per image | per-character crops, plus a swap check |
| Person regions | DetectPose returns several people (the Parts page keeps the largest); SegmentParts takes a text prompt | each character's mask in the image |

#### 16.2 How each back-end can do it

* **stable-diffusion.cpp** (the default local back-end) has no regional prompts or masked LoRAs.
  sd-server's LoRAs, prompt, IP-Adapter and ControlNet all apply to the whole image (checked
  upstream, October 2026: no feature, issue or code for regional conditioning).
* **ComfyUI** core (0.18.5 installed here) has the pieces, marked experimental: `CreateHookLora`,
  `SetClipHooks` and `ConditioningSetProperties` with a mask apply a LoRA and a prompt to one
  region, in one pass. The cost is a model pass per region per step.
* **SD WebUI / Forge**: only through extensions (Regional Prompter, Forge Couple); not planned.
* **Cloud edit APIs** (Gemini, OpenAI images): no LoRAs, but up to 10–15 reference images; the
  prompt can say which reference is which character.

So the main method must work with inpainting alone: **compose, then refine each character**.
1. **Layout.** Two people in the pose (rig per person, placed left and right by default; the Pose
   page can move them). Each person's box (`Person::bounds()`, grown) is that character's region.
2. **Base image.** One txt2img with both people's pose (one OpenPose map with two skeletons), the
   subject tags for two ("2girls" / "1boy, 1girl"), each character's description tied to its side
   ("on the left, red hair…; on the right, …"). No character LoRA: two global LoRAs blend the
   faces. The base sets the scene, composition and colours.
3. **Refine pass per character.** An inpaint of the character's region (the existing crop →
   generate → composite pipeline), with that character's LoRA, trigger and appearance tags, its
   pose crop as control, and its reference where the back-end takes one (sd.cpp's single
   IP-Adapter slot works here, since each pass has one character). Denoise about 0.5–0.6 keeps
   the composition. The second pass sees the first one's result.
4. **Check.** Each character's crop goes through the identity checklist against its own sheet,
   and against the other's (a swap or blend scores high on both). A failed character gets its
   refine pass again (Critic retries, M9), not the whole image.

On ComfyUI a **regional** strategy (one pass, masked hook LoRAs and prompts) is tried next to it;
the measurements decide which is the default there. Cloud edit APIs get a **multi-reference**
strategy: both references with their names, one request.

#### 16.3 The cast

* The active character becomes a **cast**: one or two characters, an order (left, right), stored
  as the project record `active_cast` (the old `active_character` is read as a cast of one).
* The character chip gets a "+" for a second character and shows both; its menu can swap the
  order and remove one. Pages without two-character support (Parts) use the first.
* The editor's AI panel with a cast of two: *Fill selection* asks which character the selection
  belongs to (preselected: the one whose region overlaps it most in an image made by Lumen).
* The assistant: `set_active_character` takes one or two ids; `generate_image` draws the cast.

#### 16.4 Director with two characters

* `PlanInput` takes a cast; the shot schema gets `people`: per person the character, a rig and a
  short action. The LLM is told which character is which and that every shot shows both (or says
  which one is alone, as a one-person shot of that character).
* Prompts: subject tags for the cast ("2girls", "1boy, 1girl"); "2girls" leaves the default
  negative when the cast has two.
* Shot cards show both poses; the pose editor edits both people.

#### 16.5 Implementation steps
- [x] Cast model and UI:
  - `Studio::activeCast()` / `setActiveCast()` (`kMaxCast` 2; empty, duplicate and unknown ids and
    a third are dropped); `activeCharacter()` is the first, so every page that draws one
    character works unchanged; `setActiveCharacter(id)` makes a cast of one (Create page,
    *Re-run* of a one-character image, Parts page). `activeCharacterChanged` fires for any change
    of the cast.
  - Stored as the project record `active_cast` (`characters`: ids in order). A project from before
    M16 is read from its `active_character` record, which the first save of a cast removes.
    A character removed from the library leaves the cast.
  - `CharacterChip`: two overlapping round thumbnails and "Akane & Bo"; the tooltip says who is
    left and right and who has a LoRA. The menu ticks both, and has *Add a second character* (a
    submenu; *Replace the second character* once there are two), *Swap left and right*, *Remove
    …* per member; choosing a character in the list still makes a cast of one. `castIds()` gives
    the pages the cast (step 5 uses it); `characterId()` stays the first.
  - Assistant: `get_state` has `active_cast` when there are two; `list_characters` returns `cast`;
    `set_active_character` takes `character_ids` (one or two, left first) besides `character_id`.
  - Test (`activeCast`): limits and order, no signal without a change, a cast of one, the chip's
    text, tooltip, ticks and menu (add, replace, swap, remove), the per-page switch, reopening the
    project, the migration from `active_character`, a character removed from the library, the
    assistant tools (two ids, too many, one id).
- [x] Two-person layout (`pose/CastLayout`, no AI or UI dependencies):
  - `arrangePeople(rigs, Arrangement)`: side by side (centres about 0.29 / 0.71, scale 0.82),
    close (0.39 / 0.61, scale 0.88: shoulders and hands meet) and front and back (the first in
    front, lower and bigger; the second behind, smaller). It sets position and scale only; the
    poses stay. One person is left as it is. Keys for JSON: `side_by_side`, `close`,
    `front_back`.
  - `personSilhouette`: a person's body as a filled shape (limbs 0.7 head sizes thick, hands past
    the wrists, the torso, the head with room for hair); the head size is nose to neck, or a
    seventh of the body for a back view.
  - `personRegions(pose, imageSize, grow)`: per person a box (keypoints, head and hair, grown by
    `grow` of its height, clamped to the image) and a mask, in refine order from back to front
    (lowest feet nearest). A mask leaves out the other people's bodies: all of a body in front,
    and of a body behind whatever this person's own body does not cover, so neither refine pass
    repaints the other character's arm; where bodies meet, the person in front has it.
  - `onlyPerson(pose, i)`: the pose of one person, for that pass's control map; the region
    pipeline already crops document-sized control maps.
  - Test (`tst_pose castLayout`): order and separation side by side (each region holds its own
    nose and ankles, not the other's neck), room for hair, close (overlap, the back region
    without the front body, the front region without the arm behind, the meeting hands to the
    front person), front and back (back first), the image edge, a back view's head, the
    silhouette, `onlyPerson`. With `LUMEN_SCREENSHOT_DIR` it saves the close layout with both
    regions tinted.
- [x] `ai/CastGeneration`: the compose-and-refine pipeline.
  - `CastRequest`: the scene (`ImageRequest` with the user's words and settings), the cast (one or
    two `CharacterContext`, left or front first), the arrangement or a pose, the refine strength
    (0.55), the checkpoint's family.
  - Base request (`castBaseRequest`): count tags from the sheets' gender presentation ("1boy,
    1girl", "2girls"; none when unknown), the scene, each member's appearance tied to its side
    ("on the left: …", "in front: …"; a sentence per side for prose models) with subject and count
    tags taken out of the appearance (`castAppearance`); the whole pose as OpenPose control; no
    LoRA, no reference.
  - Refine request per member (`castRefineRequest`): its subject tag, appearance and the scene,
    then `applyCharacter(Region)` (LoRA and trigger, else its reference: sd.cpp's one IP-Adapter
    slot is enough per pass); inpaint at the refine strength with that person's pose only
    (`onlyPerson`); seed + 1 + index.
  - `startCastGeneration`: one Queue job in the provider's group: the base through
    `GenerationService::execute`, then each refine as a `RegionInput` (the image so far, the
    region's mask, grow 2, feather 12, colour match) in refine order, each result laid over the
    image; then `GenerationService::storeOutput` (new: `submit`'s way of storing, for pipelines;
    `registry()` and `queue()` are public too). Metadata: `user_prompt` and `cast` (members with
    what each pass applied, region and seed; arrangement; refine strength; the pose as OpenPose
    JSON). A back-end that cannot inpaint fails with a message.
  - Depth order fix found here: with framings that cut the people off, box bottoms do not tell
    who is in front; `personRegions` now orders by head size (bigger is nearer), then the feet.
  - Test (`tst_services castGeneration`, mock sd.cpp server that paints the mask red for one
    LoRA and blue for the other): the base (count tags, both sides, no "solo", no LoRA, a skeleton
    on each half), each refine (its trigger first, its look only, its LoRA at its weight, mask,
    strength 0.55, seed, control), the result (each colour in its own region, the scene
    elsewhere), the stored asset and metadata; front and back (the one behind drawn first);
    an empty cast.
  - Found on the way: `img::encode` passed zlib levels to Qt 6, which takes 0–100 and stores
    anything up to 10 uncompressed, so every PNG Lumen wrote (assets, layers, thumbnails,
    character library, training sets, exports, images sent to providers) had no compression: a
    flat 2 MP image was 8 MB instead of 0.1 MB, and an sd.cpp inpaint request 11 MB. The level is
    now scaled (1 → 12, 6 → 67, 9 → 100); `tst_core pngIsCompressed`. Existing projects keep
    their large images until they are written again.
- [x] Per-character check and retry (in `CastGeneration`, `CastRequest::check`, off by default;
  the pages switch it on with the Critic in step 5):
  - After the repaints each member's crop (its region box) goes through `checkIdentity` twice:
    against its own sheet and references, and against the other member's distinguishing features
    (`castDistinguishingSheet`: its look without the tags both share, so a uniform both wear does
    not count) and references. Four vision calls for two members.
  - `castVerdict`: "swap" when the other score is at least 2 above the own one, "blend" when the
    other score is 5 or more and the own one less than 3 above it, "weak" below `minScore` (6),
    "ok", or "unchecked" when nothing showed.
  - A member that fails is repainted (seed + 1000 per retry, on the image so far) and checked
    again, up to `maxRetries` (1); the other member stays as it is. A member that still fails is
    kept with its verdict. Without a vision model (or when the check fails) the image stands as
    drawn, marked "unchecked".
  - Metadata per member: `check` {own, other, verdict, attempts, issues}; the members are also on
    the job (`cast_members`) when the result is not stored.
  - Test (`tst_services castCheck`, mock sd.cpp painting the region per LoRA, mock vision model
    matching the crop's colour against the checklist's character): the verdicts, the
    distinguishing sheet, a clean result (four checks), a swap of the first pass (only that
    member repainted with a new seed and checked again), a blend kept after one retry, and no
    vision model.
- [ ] Generate page and *Re-run* with a cast of two; editor *Fill selection* with the character
  choice. Tests against the mock server.
- [ ] Director and Create page: cast in `PlanInput`, `people` in the shot schema, two-person poses
  on the shot cards and in the pose editor, shots generated through `CastGeneration`. Tests:
  schema, prompts, a planned shot end to end with mocks.
- [ ] ComfyUI regional strategy: a graph with `CreateHookLora` → `ConditioningSetProperties`
  (mask per character) → combined conditioning, built per request like the standard graph.
  Contract test of the graph; the live step decides whether it becomes ComfyUI's default.
- [ ] Cloud multi-reference strategy (Gemini, OpenAI images): both references with their names in
  one edit request. Contract tests.
- [ ] Live measurements (RTX 4060 8 GB, WAI-Illustrious, sd.cpp): Akane and a second trained
  character, 8 scene prompts, same seeds. Compared: (a) both LoRAs on the whole image,
  (b) compose and refine, (c) on ComfyUI, regional. Per character: checklist identity on its
  crop; swaps and blends; time.

**AC**:
1. With a cast of two trained characters, a Generate-page prompt that names neither ("two friends
   sharing an umbrella in the rain") produces both: each character's crop scores at least 2
   checklist points higher than with both LoRAs on the whole image, and no image swaps or blends
   them (each crop scores at least 4 points lower against the other character).
2. A two-character image takes at most 2.5 times as long as a one-character image of the same size
   on the same back-end (8 GB GPU).
3. The Create page plans and draws shots of both characters with a pose per person; the pose editor
   moves either.
4. In the editor, *Fill selection* on one character of a two-character image keeps the other
   unchanged and uses the chosen character.
5. "Draw Akane and Bo at the beach" in the assistant uses both characters.
6. On a cloud edit API without LoRAs, the same prompt uses both references and says so.

Risks and open questions:
* A second character with a trained LoRA is needed for the live step: train one locally (about two
  hours on 8 GB), in the cloud (M15, about $0.50), or test with a public character LoRA of the
  same family.
* Characters that touch or overlap (holding hands, a hug): the regions overlap, and the second
  refine pass can repaint the first character's arm. Masks from the pose limbs, or SegmentParts
  ("person") inside the region, may be needed; start with the pose boxes and measure.
* The base image without LoRAs may get hair or outfit colours wrong enough that a refine at 0.5–0.6
  cannot fix them; a higher denoise loses the composition. The live step finds the range; the
  ComfyUI regional strategy avoids the question.
* Three or more characters: the design takes a list, but the UI and the measurements stop at two.
* ComfyUI's hooks are experimental; a ComfyUI update may change them. The graph is built in one
  place and has a contract test.

### M17 — Editor-first redesign

Goal: Lumen becomes an AI image editor that a person can use without reading a guide. Opening the
app shows a canvas. You open an image, select an area, and say in words what should change there
("colorize it, keep everything else", "remove the lamp", "replace the cup with a candle"). With
nothing selected, the words apply to the whole image. A new image starts from a prompt. Character
creation, poses, Live2D parts and LoRA training stay, but as optional plugins on top of this core.

Decisions (October 2026): restructure in place (the engine stays, the shell is rebuilt); the
character features become plugins, built in but off by default; the node filter editor covers
classic image operations and AI steps; guided local setup is built around the managed
stable-diffusion.cpp runtime, with ComfyUI as an advanced option.

#### 17.1 What is wrong today

Survey of the code, the app log and user testing (October 2026):

* **The editor is not the app.** The app opens on *Create* (the AI director). The navigation rail
  has six pages (Create, Generate, Edit, Poses, Parts, Library) and four more buttons (Assistant,
  Queue, Settings). Generating an image and editing it happen on different pages.
* **Setup is spread over five places**: Settings → Providers, Local servers, Models, the model hub
  and the local setup dialog. Every server option is a raw field, so a user can pick combinations
  that cannot work. From the log of a real session:
  * FLUX.2 klein 4B (a standalone diffusion model) was put in the *checkpoint* field, so sd-server
    was started with `-m` and no VAE or text encoder, and exited with
    `get sd version from file failed`.
  * An Anima checkpoint was started with an SDXL OpenPose ControlNet and an SDXL IP-Adapter, and
    exited with `model metadata validation failed`.
* **Automatic companion files exist for one family only.** `familyCompanions` (ModelHub.cpp)
  knows Anima's VAE and text encoder; for every other model the user fills in VAE, CLIP-L, T5 or
  LLM by hand. `companionServerOptions` never sets `clip_l`, which FLUX.1 needs.
* **Model settings ignore the family.** `PromptStyle` recognises FLUX but keeps CFG at 6.0;
  guidance-distilled models (FLUX.1 dev and schnell) need CFG 1, so images take twice as long
  and burn.
* **No local instruction editing.** `SdCppProvider` refuses `GenMode::Edit`, although the bundled
  sd-server supports reference images (`--ref-image-args`, `--img-cfg-scale`) for edit models
  (FLUX.1 Kontext, FLUX.2, Qwen-Image-Edit). Locally, "colorize this" can only be inpainting
  with a regular model.
* **Models live in a hidden folder.** The managed models folder is
  `%LOCALAPPDATA%\Lumen\Lumen Studio\models`; users keep models in ComfyUI or Downloads folders
  and add them one at a time.
* **Broken layouts** in some places (reported by the user; to be found in step 2).

#### 17.2 The editor shell

* **Start screen** (when no document is open): *Open image…*, *Paste from clipboard*, *New from
  prompt…*, *New blank canvas…*, recent documents with thumbnails, and a setup card while no image
  engine works ("Set up AI", §17.5).
* **One main window**: tools on the left; the canvas with document tabs in the centre; a right
  panel with *Layers*, *History* and *Filters* (§17.6); a status bar with zoom, the engine in use
  and running jobs (the Queue becomes a popup from there). The navigation rail goes away. Plugins
  add panels, menus and canvas actions (§17.7); they do not add pages to the core window.
* **The prompt bar** is always docked at the bottom of the canvas (it replaces the floating
  `ContextBar` and the AI panel's mode list):
  * With a selection: *Generate* changes only the selected area. The engine is the preset's
    edit model when it has one (instruction edit of a crop around the selection), else inpainting.
    The result is composited through the selection mask with feathering and colour matching (the
    region pipeline of §3.6). *Remove* fills the area from its surroundings.
  * Without a selection: *Generate* edits the whole image with an edit model ("colorize", "make it
    night"). Without an edit model, the bar says so and offers img2img at a chosen strength.
  * On a blank canvas, or from *New from prompt*: text-to-image at the canvas size.
  * Each run returns 1–4 variants in a strip above the bar; ←/→ cycles, Enter keeps one, Esc
    drops all. The kept result becomes a new layer named after the prompt, masked to the
    selection, so the original pixels stay underneath (non-destructive; the history has one step).
  * A *⋯* menu holds the settings most people never need: strength, steps, seed, number of
    variants, negative prompt (only when the model supports it), reference images.
* **Library** becomes *File → Browse project images* (a dialog), with batch background removal
  and upscaling kept. *Generate* becomes *New from prompt* plus the prompt bar.
* **Text and hints in plain words.** Every control says what it does, not which API field it
  sets; technical names appear only under *Advanced*.

#### 17.3 Models folder and model presets

* **One visible models folder**: by default `Documents\Lumen\Models` (moveable from Settings →
  Models; *Open models folder* in the Help menu and in the setup wizard). Its subfolders match
  ComfyUI's (`checkpoints`, `diffusion_models`, `vae`, `text_encoders`, `loras`, `controlnet`,
  `ipadapter`, `clip_vision`, `upscale_models`), so a ComfyUI install can point at the same tree.
  Existing ComfyUI, WebUI and LM Studio folders can be added as extra read-only roots (the
  existing `suggestedRoots`); files in the old `%LOCALAPPDATA%` folder are moved on first start,
  after asking.
* **Model presets** as data (`resources/presets/models/*.json`, one file per family), replacing
  the hard-coded Anima companions. A preset says:
  * how to recognise the family from the file header (the `ModelFormats` detection that exists);
  * the slots it needs: diffusion model or checkpoint, VAE, text encoders (`clip_l`, `clip_g`,
    `t5xxl`, `llm`), each with a pinned Hugging Face file (repo, revision, sha256, size) and the
    families that may stand in for it;
  * what it can do: generate, inpaint, instruction edit, number of reference images, negative
    prompt, LoRAs, which ControlNet and IP-Adapter families fit;
  * sampling defaults: CFG, distilled guidance, image CFG, steps, sampler, scheduler, native
    resolution, prompt dialect;
  * VRAM tiers (8, 12, 16, 24 GB): weight type (`--type`), `--offload-to-cpu`, `--clip-on-cpu`,
    VAE tiling, and which quantised file to download.
* First presets: FLUX.2 klein 4B (generate and edit; recommended for 8 GB), FLUX.1 Kontext dev
  (edit), FLUX.1 dev and schnell, Qwen-Image-Edit, SDXL, Illustrious / NoobAI, Pony, SD 1.5,
  Anima. Each file names its source, licence and the sd.cpp docs page it follows.
* **Choosing a model** (setup wizard, Settings → Models, or dropping a file into the folder): the
  family is detected, the preset fills every slot from the library or offers to download what is
  missing (one button, size shown), puts a standalone diffusion model in the `--diffusion-model`
  slot, and applies the sampling defaults and the VRAM tier of the detected GPU. Add-ons
  (ControlNet, IP-Adapter, LoRA) are filtered by the preset's families; an incompatible one cannot
  be chosen, and an old profile that has one gets a warning instead of a crash at start.
* A file without a preset still works by hand under *Advanced*, as today.

#### 17.4 Local instruction editing

* `SdCppProvider` supports `GenMode::Edit` when the preset says so: the init image (and further
  references) are sent as reference images, with the preset's image CFG; the field names come
  from the pinned sd-server's `api.md` and get a contract test against the mock server.
* Region edits: the selection's bounding box, grown for context, is cropped and scaled to the
  model's native size, edited, scaled back and composited through the feathered mask, so pixels
  outside the selection stay exactly as they were.
* CFG, distilled guidance and the negative prompt come from the preset, not from the global 6.0
  default (fixes FLUX.1 dev and schnell).

#### 17.5 Setting up AI

* **First-run wizard** (also *Help → Set up AI*), three choices:
  1. *Run on this computer*: detects the GPU and VRAM (`runtime/Hardware`), recommends one preset
     (FLUX.2 klein 4B on 8 GB; SDXL-class models as the lighter option), shows the download size,
     installs the sd.cpp runtime and the model files, starts the server and runs a test image.
  2. *Use an online service*: fal.ai, OpenAI or Gemini; one key field with a link to where to get
     a key, then a test image.
  3. *Connect a running server*: ComfyUI, Forge or another sd-server by address, found
     automatically on the default ports where possible.
* **Settings → AI** shows one list: the image engine (and an optional separate edit engine), with
  status, model and *Change…*; per-field server options move under *Advanced*. The language model
  is optional and only listed when a plugin needs one; the core editor works without an LLM.
* The existing supervisor and GPU release (`releaseGpu`) stay; the wizard does not start an LLM
  next to the image engine on an 8 GB GPU.

#### 17.6 Node filters

* **Core graph model** (`filters/`, no UI dependency): nodes with typed ports (image, mask,
  number, colour, text, choice), a validating graph, evaluation on a worker thread with a result
  cache per node, previews at reduced resolution while editing and full resolution on apply.
  Graphs are JSON files (`.lumenfilter`) in a `filters` folder next to the models folder.
* **Classic nodes**: input (layer or document, selection as mask), output; levels, curves,
  brightness/contrast, hue/saturation, colour balance, invert, desaturate, threshold, posterize;
  Gaussian blur, sharpen, unsharp mask, noise, median; blend (the layer blend modes), mix by mask;
  mask from luminance or alpha, grow/shrink/feather, invert; resize, crop, flip, rotate.
* **AI nodes** run through `GenerationService` as Queue jobs: prompt edit, inpaint, upscale,
  remove background. A graph with AI nodes previews only after an explicit *Run*.
* **Node editor**: a separate window (`QGraphicsView`), add nodes from a searchable menu, connect
  ports, edit parameters, live preview of the selected node. A graph can *publish* parameters;
  published ones appear as sliders when the filter is used.
* **In the editor**: the *Filters* panel lists saved graphs and a few built-in ones (e.g.
  *Colorize with AI*, *Clean up background*); applying runs on the active layer through the
  selection, as one undo step, onto a new layer by default.

#### 17.7 Plugins

* **Extension interface** (`ext/Extension.h`): id, name, description, and registration calls for
  dock panels, menu actions, prompt-bar actions, filter node types, provider types, project data
  records and request decorators (to change a generation request, e.g. add a character's LoRA).
  The core never includes a plugin header; CMake links plugins against the core, never the
  reverse, and a configure check fails on a reverse dependency.
* Built-in plugins are static libraries registered at start (no DLL loading yet); Settings →
  Plugins switches each on or off (restart needed at first). Off by default once the new shell
  lands; on until then, so the pages stay where users know them.
* Moved out of the core:
  * **Characters**: character library, active character and cast (M14, M16), Create page /
    Director, Assistant; the rest of M16 continues there.
  * **Poses**: pose editor and library, OpenPose control as a prompt-bar input.
  * **Live2D parts**: the Parts page (M11) and the Cubism preview.
  * **Character LoRA**: dataset builder, local and RunPod training (M13, M15).
* Project data written by a plugin that is off is kept unchanged when the project is saved.
* **First cut (step 3)**: the hooks that exist are rail pages (`PageSpec`: id, icon, label, order,
  a fixed default `Alt` key), rail panels over the page area (`PanelSpec`, the assistant) and
  Settings tabs (`SettingsTabSpec`); the other hooks come with the features that need them (prompt
  bar §17.2, filter nodes §17.6, request decorators with the Characters work below). Differences
  from the list above:
  * **Character LoRA** is part of **Characters**: the dataset builder is built on director
    sessions and only opens from the Create page.
  * Still in the core until request decorators exist: the character library, the active character
    and cast, `CharacterChip` and `CharacterLoraDialog` (the Generate page, the AI panel and the
    Parts page use them), and `ai::Director` (the Generate page's prompt enhancer uses it).
  * The pose canvas and the pose dialog stay in the core (the director's shot cards use them); the
    Poses plugin has the page.

#### 17.8 Implementation steps
- [x] Park M16: the open step-5 work (Generate page and *Re-run* with a cast) is in
  `git stash` ("M16 step 5 WIP … parked for M17"). The remaining M16 steps move to the Characters
  plugin.
- [x] Layout audit (`tests/tst_layout.cpp`): opens every page (with a document in the editor), the
  editor with the queue and the assistant, every Settings tab and the standalone dialogs, at the
  logical sizes 1280×720 (1080p at 150 %), 1536×864 (1080p at 125 %) and 1920×1080, on an
  offscreen 1920×1080 screen with the system fonts (`QT_QPA_FONTDIR`). Dialogs are checked against
  a 1280×680 screen. Findings: window below its minimum size, widgets squeezed below their minimum,
  cut text (labels, buttons, the current item of combo boxes), widgets clipped by a parent,
  overlapping siblings. Report-only; `LUMEN_LAYOUT_STRICT=1` fails on any finding. With
  `LUMEN_SCREENSHOT_DIR` it writes `layout/layout-report.md` and a PNG per state (the before set).
  Results in §17.9.
- [x] Extension interface and registry; move Create, Poses, Parts, LoRA training and Assistant
  behind it with no change in behaviour (pages still reachable while the old shell exists).
  Test: core builds and runs with every plugin off; project data of an off plugin round-trips.
  * `ui/ext/Extension.h`: `Extension` (id, name, description, `pages()`, `panels()`,
    `settingsTabs()`) and the registry `Extensions` (on/off in Settings `plugins/<id>/enabled`,
    read once per session).
  * `src/plugins`: `lumen_plugin_characters` (Create page, director, assistant, LoRA training,
    the Cloud training tab), `lumen_plugin_poses` (Poses page), `lumen_plugin_parts` (everything
    of `ui/parts`), and `lumen_plugins` (`registerBuiltInPlugins`, called by `main` and the UI
    tests). A configure check in `src/ui/CMakeLists.txt` fails when a core source includes
    `plugins/…`.
  * Pages are string ids (`pages::Editor`…; plugins: `create`, `poses`, `parts`) instead of the
    `Page` enum; `Studio::hasPage` tells the core whether a plugin page exists, and the core hides
    its ways in (the Generate page's pose *Edit…*, *Slice into parts* in the Library, the pose
    editor after *Detect pose*). `ui/page` stores the id; the old index is read once.
  * The Studio no longer holds the director's actions for the assistant; the Characters plugin
    keeps them per Studio (`directorActions(studio)`).
  * The rail order and `Alt+1`…`Alt+6` are unchanged, and a page keeps its key when another
    plugin is off. Rail labels keep their translation contexts.
  * Tests (`tst_canvas`): `pluginsOff` (three core pages, no assistant, editing works, the pose
    *Edit…* button hidden, no Cloud training tab, the Plugins tab switches a plugin for the next
    start, a `director` record survives a session without the plugin and a reopen);
    `legacyPageSetting` (the old index 3 opens Poses; fixed shortcuts).
- [x] Model presets: the JSON format, the loader, the first presets (§17.3), family detection to
  preset, slot filling from the library, download of missing files, add-on filtering, VRAM tiers.
  `familyCompanions` and the Anima special case are replaced. Tests: the two failing profiles
  from the log produce a correct command line or a clear message; FLUX.1 gets `clip_l` and
  `t5xxl`; an SDXL ControlNet is not offered for Anima or FLUX.
  * Detection (`ModelFormats`, inspector version 3): FLUX.2 from FLUX.1 by the input width (128 =
    32 latent channels × 2 × 2) and the shared modulation layers; the FLUX.2 variant by the text
    input width (7680 = 3 × Qwen3 4B: klein 4B; 12288: klein 9B; 15360: dev); FLUX.1 dev / schnell
    by the guidance embedding; CLIP-L / CLIP-G by width. GGUF shapes are kept in PyTorch's order.
    Checked against the headers of real files (FLUX.2 klein 4B fp8, its VAE, Qwen3 4B / 8B, Anima).
  * `runtime/ModelPresets`: `resources/presets/models/{anima,flux1,flux2,qwen-image,sd}.json`
    (pinned files with repo, commit, size, SHA-256 and licence, looked up through the Hugging Face
    API; component slots with the header family / variant a library file needs; what the model
    can do; sampling defaults from sd.cpp's docs at the pinned commit f89d9b1; VRAM overrides).
    Presets: FLUX.2 klein 4B / 9B and their base models, FLUX.1 dev / schnell / Kontext dev and
    single-file FLUX.1, Qwen-Image and Qwen-Image-Edit, Anima, Illustrious, Pony, SDXL, SD 1.5.
    Ungated sources: the FLUX.1 VAE is Black Forest Labs' `ae.safetensors` from Comfy-Org's copy.
  * `companionsOf` / `companionServerOptions` / `companionDownload` keep their signatures and
    take the components from the presets (a download comes from the model's own preset, so the
    same VAE keeps the licence of the right repository); `ModelCompanion::option` names the slot
    (fixes `clip_l`, which was never filled).
  * `modelServerOptions(library, file, tier)`: the file in its slot (the other cleared), its
    components, the preset's launch options. Used by the server dialog (Browse, the library menu,
    a typed path) and *Use with a server*.
  * `checkSdProfile`: a model in the wrong slot, a file that is gone or is not an image model, a
    missing or mismatched component (with the file and its size), a ControlNet / IP-Adapter of
    another family. The supervisor's new `preflight` runs it before every sd.cpp start, so the
    start fails with these messages instead of "exited (code 1)". The server dialog shows the
    preset, its components and the problems, offers *Download missing files*, and cannot be saved
    with a blocking problem (a missing component does not block).
  * Tests: `tst_runtime modelPresets` (every bundled file parses, all files pinned, detection and
    matching for each family, GGUF), `presetProfiles` (the klein-as-checkpoint and Anima + SDXL
    ControlNet / IP-Adapter profiles of the log; FLUX.1 with CLIP-L, not CLIP-G, and the 8 GB
    overrides; downloads from the ungated copy; missing files), `supervisorCrashAndFailures`
    (preflight); `tst_canvas presetServerDialog` (the dialog moves klein to its slot, fills VAE
    and encoder, refuses an SDXL ControlNet, offers the download).
  * Not yet: FLUX.2 dev (Mistral Small 3.2 24B encoder), Z-Image, SD 3 and split SDXL presets;
    *Use with a server* does not know the GPU size, so it applies no VRAM overrides; sampling
    defaults reach requests in the next step. ComfyUI's "mixed" fp8 files (with `weight_scale`
    tensors, such as `flux-2-klein-4b-fp8`) load and edit correctly in the pinned sd.cpp with the
    8 GB tier's `--type q8_0` (checked live, see the live check below); with `file` precision they
    are untested.
- [x] Models folder: the visible default, ComfyUI subfolders, moving it, the one-time move from
  `%LOCALAPPDATA%`, extra roots.
  * `paths::defaultModels()`: `Documents\Lumen\Models`; with relocated data (`LUMEN_DATA_DIR`, the
    portable ZIP) or in Qt's test mode `<data root>/models`, so tests and the portable ZIP stay
    self-contained. `ModelLibrary::defaultFolder()` is the folder in effect: the chosen one
    (`setFolder`; Studio keeps it in the setting `models/folder` on `folderChanged`) or
    `standardFolder()` (next to a test's data after `setDataDir`). Choosing the standard folder
    clears the choice, so the portable ZIP keeps no drive letter. Downloads (`destinationFor`,
    companions, starters), LoRA training output and the setup dialog all go through it.
  * Sub-folders keep ComfyUI's names (`ModelLibrary::subfolders()`); language models and their
    vision projectors share Lumen's own `llm`, because `findProjector` looks next to the model.
  * `runtime/ModelsFolder`: `planModelsFolderMove` takes model files with their `.lumen.json`
    sidecars and unfinished `.part` downloads (other files stay) and refuses no target, the same
    folder, a folder inside it, a file, files of the same names there, or too little space on
    another drive. Folders added to the library that lie inside Lumen's folder stay with all their
    files (`keep`; the overload taking the library fills it). It never goes into a symbolic link or
    a junction (`Models\checkpoints` linked to ComfyUI's): a link that leads to models moves as
    itself (`links`), by a rename, so ComfyUI's files and empty folders stay ComfyUI's; across
    drives that is refused. `moveModelFiles` renames on the same drive (`MoveFileExW` without
    copying, so a file in use is an error), and across drives copies in chunks under a temporary
    name, keeping the modification time (the index still knows the file), deleting the originals
    once every copy is complete; a cancel is still honoured after the last file. On an error or a
    cancel everything moved goes back and the folders the move made in the target are removed;
    after a move the folders left empty are removed (not links, not added folders).
    `relocatePath` maps a path; `movedPath` keeps the path of what stayed behind (still there, not
    in the target: a template next to a model, an added folder's file, a file written meanwhile),
    which `relocateOptions` (file and folder options, the llama.cpp model list), the library and
    the characters' LoRAs use. Only the extra arguments are read as text: a path starts after a
    space, quote, `=` or `,`, ends at its closing quote or the next space, and must be the folder
    or go on into it, so `D:\Models 2` or `"D:\Models backup"` are not `D:\Models`.
  * `moveModelsFolder` is a Queue job ("Move models folder", one at a time). It is refused while
    a download or a LoRA training (local or cloud, also a queued one: the LoRA is copied into
    `loras` at the end) is unfinished and, with files to move, while the GPU is reserved. While it
    runs, `ModelLibrary::isMoving()` (`beginMove` / `endMove`) makes downloads
    (`ModelHub::download`), trainings (`CharacterTraining::begin`) and the LoRA evaluation's copies
    (`LoraEvaluation::begin`, `apply`) refuse, and scans wait; a scan that ran when the move began
    is thrown away and done again from the moved entries, so no checksum is lost. A local training
    also refuses while anything else holds the GPU reservation (`TrainingRun::run`), whose release
    would start the servers under it. With files to move it reserves the GPU, so running servers
    stop (they hold files open) and none starts meanwhile; with none, nothing stops and nothing is
    waited for. Then it rewrites the server profiles, calls `ModelLibrary::relocate` (entries keep
    their checksums and Civitai lookups; added folders stay) and `setFolder`, and starts the
    stopped servers again. From there a cancel is ignored (`Job::ignoreCancel`), so a late one no
    longer ends a finished move as *Cancelled*. Model files that still turn up in the old folder are
    named in the job's status. Characters' LoRA files follow (`ModelLibrary::relocated`, in
    Studio). A stopped training continues with its base model where it is now
    (`CharacterTraining::findCheckpoint`: the library's file of the same name when the path in
    `run.json` is gone).
  * First start: `legacyModelsToAsk` finds model files (1 MB or more) in `<app data>/models` while
    no folder is chosen and that is not the standard folder. Studio keeps using it, unsaved, until
    `MainWindow::offerModelsFolderMove` (called by `main` at start-up, never by the tests; the
    question is a callback) has asked once through `settleLegacyModels` (Studio keeps
    `models/legacy_asked`): the old folder becomes the choice, then *Move models* runs the move to
    the standard folder (which clears the choice) and *Keep them where they are* stays; a move that
    cannot be done says why and keeps the old folder.
  * UI: Settings → Models shows the folder with *Open folder* and *Move…* (a confirmation with the
    number and size of the files; the result as a toast); *Help → Open models folder*; the local
    setup dialog says where models are saved. Extra roots (ComfyUI, WebUI, LM Studio) stay as they
    were: scanned in place, never moved or written; one that becomes the chosen folder is listed
    once.
  * Tests: `tst_runtime modelsFolderDefaults` (the default with relocated data and in test mode,
    `setDataDir`, a chosen folder and its sub-folders, back to the standard one, an added folder
    as the folder), `modelsFolderMove` (the plan with the sidecar and the `.part` file, an added
    ComfyUI folder inside that is kept, refusals, no move during a download, a local or cloud
    training or a GPU reservation, a file in use undoes the move and the server runs again; the
    move: files, a note and a template that stay, the added folder with its files, its empty
    sub-folder and its entry, folders left empty removed, a running and a stopped profile rewritten
    in every form while paths to what stayed and to similar names do not change, the server
    restarted with the new path, checksums kept through the next scan, a download refused while
    moving and landing in the moved folder after; with nothing to move a running server keeps
    running and nothing waits),
    `modelsFolderCopies` (a copy across drives, forced on one: cancelled half-way leaves no file
    and no new folder, a folder that was there stays; a cancel after the last file undoes; keeps
    the modification time, reports progress; path rewriting in options and text, with sibling
    folders whose names go on after a space), `modelsFolderLinks` (a junction to ComfyUI's
    checkpoints moves as a link and ComfyUI's file and empty folder stay; refused across drives;
    options through the link follow, a template left behind keeps its path),
    `legacyModelsFirstStart` (three cases without a question, keep, move with a server profile,
    a refused move), `loraTraining` (no training while the GPU is reserved); `tst_core
    jobCancelQueued` (`ignoreCancel`); `tst_canvas modelsFolderSettings` (the page shows the
    folder; a refused and a cancelled move; the move through the page updates the setting, the
    label, a profile, a character's LoRA and where downloads go; a stopped training finds its moved
    base model; no training starts while moving; and back, which clears the setting; the Help
    action; no question without an old folder).
  * Not yet: *Open models folder* in the setup wizard (§17.5); a move stops every running server,
    also those that use no file in the folder; a LoRA evaluation that runs when a move starts
    fails (its server stops); the start-up path with models in an old folder (`Studio` constructor,
    `MainWindow::offerModelsFolderMove`) is untested, because with `LUMEN_DATA_DIR` the old and the
    new folder are the same (only `settleLegacyModels` is); the German translation of the new
    texts.
- [x] Sampling defaults from presets (CFG, guidance, image CFG, negative prompt) through
  `PromptStyle` and the request; sd.cpp instruction edit (§17.4) with a mock-server contract test.
  * `ai::ModelDefaults`: the preset as the image back-end sees it; Studio passes it to the provider
    of a server Lumen runs as its `model_preset` setting (`applyServerBindings`; the AI layer does
    not depend on the runtime library).
  * `PromptStyle` takes the preset's CFG, steps, sampler, scheduler and dialect, and drops the
    negative prompt where the model takes none; `ImageRequest::stepsChosen` works like
    `cfgChosen` (the Generate page chooses both; it follows a preset's steps when the model
    changes, and goes back to 28 after it). FLUX.1 dev, schnell and klein no longer get CFG 6.
  * `SdCppProvider`: capabilities from the preset (edit, references, negative prompt, native
    size); the payload sends `distilled_guidance`, `img_cfg`, `flow_shift` and the preset's
    sampler (after the request's and the provider's); an edit sends its source and references as
    `ref_images` (no `init_image`, `strength` or mask), as the pinned sd-server's `api.md` says;
    without an IP-Adapter, references guide new images the same way.
  * `ImageCapabilities::regionEdit`: a selection stays an instruction edit of its crop (blended
    back through the feathered mask) instead of becoming inpainting; set for local edit models
    only, so cloud providers keep their behaviour.
  * Tests: `tst_providers sdcppPresetDefaultsAndEdit` (capabilities, prompt style, the edit and
    reference payloads, FLUX.1 guidance, Qwen-Image-Edit image CFG and flow shift, no edits
    without a preset), `tst_services regionEditMode` and `localRegionEditEndToEnd` (a klein edit of a
    selection: the request, and no pixel outside the feathered selection changes),
    `tst_canvas presetServerDialog` (a saved server's provider gets the preset).
  * The current editor uses this through the AI panel's *Instruct edit*; the prompt bar of §17.2
    picks it automatically later.
- [ ] New shell: start screen, the editor as the main window, status bar with jobs, *New from
  prompt*, *Browse project images*; the navigation rail and the Generate and Library pages go.
- [ ] Prompt bar (§17.2): selection, whole-image and blank-canvas modes, *Remove*, variants strip,
  results as masked layers. Test with the mock server: a selection edit leaves every pixel
  outside the feathered mask unchanged.
- [ ] Setup wizard and Settings → AI (§17.5).
- [x] Filter graph core and classic nodes; golden-image tests per node.
  * `src/filters` (`lumen_filters`, `lumen::filters`): on `lumen_core` (`ImageOps`, `Async`) and Qt
    Gui only, no UI, AI, document or runtime code; `lumen_ui` links it. No UI yet.
  * Model (`NodeType.h`, `Registry.h`, `Graph.h`): port types image (ARGB32 premultiplied), mask
    (Grayscale8), number, colour, text, choice; a mask may feed an image input (as grey), nothing
    else converts. `NodeTypeInfo`: stable id, name, category, description, input and output ports
    (inputs optional or required), parameters (default, range, decimals, unit, choices, an editor
    hint such as `curve`, and `pixels` for lengths that follow the preview scale), and the flags
    `readsSource` (input), `isOutput` and `expensive` (AI steps). `NodeRegistry` is thread-safe
    and keyed by id; `global()` adds the classic nodes on first use and other libraries `add()`
    theirs at start (the AI nodes from the ai layer). A graph's nodes hold their type, so
    evaluation never looks types up, and tests use a copy of the registry with their own types.
  * `Graph`: nodes (`n1`, `n2`…, parameter values coerced to their spec when set, values of
    unknown parameters from newer files kept and saved again, an editor position), links (one per
    input; `connect()` replaces it and refuses missing ports, types that do not fit and loops, with
    a message), published parameters (node, parameter, label, slider range). `validate()` gives
    errors for no output or several, loops, ports that do not fit (with a hint: “Put a ‘Mask from
    image’ node between them.”), unconnected required inputs of nodes that feed the output and
    published parameters that do not exist or whose slider range is empty or goes beyond the
    parameter's, and a warning for nodes that do not feed the output, so a half-built graph still
    previews.
  * Files: `.lumenfilter` JSON (`format`, `version` 1, name, description, nodes with type, position
    and the parameters that were set, links, published). A newer version, an unknown node type
    (“It may come from a plugin that is turned off.”) and links or ids that do not fit are load
    errors in plain words; type, loop and missing-link problems load and are left to `validate()`.
    `Library(folder)` lists a folder's filters by name, with the error of each one that does not
    load, and names new files after the filter; the folder is chosen by the caller.
  * Evaluation interface (the AI nodes and the node editor build on it):
    * `NodeType::evaluate(const NodeArgs &, EvalContext &)` returns a `NodeResult`: the outputs, or
      a `QFuture<Values>` that delivers them later. An AI node submits its Queue job (a queued call
      to the GUI thread) and returns the job's future at once; the evaluation then gives up its
      worker thread and continues on the pool when the future finishes (`then(pool)`, and
      `onCanceled` for a cancelled job). A node fails by throwing `lumen::Error`, or through its
      future; the message reads ““Blur”: …”. One type object serves every node of its type on
      several threads, so node types keep no mutable state.
    * `NodeArgs`: the connected inputs in their canonical formats (sizes may differ: nodes stretch
      a mask or a second image to the first image, the output node too) and the parameters,
      coerced, with pixel lengths already multiplied by the preview scale; a connected value input
      overrides the parameter of the same id and is resolved the same way (`resolveParam()`:
      clamped to the range, scaled when it is a length). `EvalContext`: node id, scale (always 1
      for an expensive node), `isCancelled()` (a node that works for long checks it; `Ops` takes it
      as `cancelCheck()`), `onCancel(handler)` (an AI node cancels its job), and the source and
      selection at the node's scale for the input node.
    * `Evaluator::evaluate(graph, {image, selection}, {scale, target, runExpensive})` returns a
      `QFuture<EvalResult>` (the output's image and mask, the target node's outputs, the nodes that
      ran, `needsRun`). It validates first (only what the target needs when one node is previewed)
      and runs the nodes in order on `async::pool()`.
    * Cache: each node keeps its last outputs at full size and at the preview scale (two lanes, so
      going from previewing to applying and back computes nothing again), each under a SHA-1 key of
      its type, its parameter values, the scale and the keys of the nodes feeding it, every field
      length-prefixed so that two text parameters cannot run together into the same key; the
      input node's key covers the source and the selection by `QImage::cacheKey()`. All keys are
      computed before anything runs, and nodes that only feed cached results do not run. A
      parameter change re-runs that node and what follows it; moving a node re-runs nothing;
      entries of removed nodes go at the next evaluation.
    * Preview: the source and selection are scaled once and pixel parameters with them, so a
      preview looks like the result. An expensive node always runs at full resolution, with the
      nodes feeding it, and a preview at any scale takes its result scaled down, so a *Run* while
      previewing gives the result that applying uses, without running the step again. With
      `runExpensive` off (previews of a graph with AI steps), the evaluation stops with `needsRun`
      before it runs anything when an expensive node is not cached; once it has run, sliders after
      it preview live from its cached result.
    * Cancellation: `Evaluator::cancel()` (and the evaluator's destructor) runs the `onCancel`
      handlers of the current node. A node that waits for a future has the future cancelled, and
      the evaluation's future finishes as cancelled at once; a running node stops at its next
      `isCancelled()` check (blur, unsharp mask and median check while they work; the other
      classic nodes finish their pass first) and the future finishes when it returns. Cancelling
      the future itself is seen at the same checks but does not reach a pending future. Results of
      interrupted nodes are not cached. `evaluateNode()` runs one node synchronously (tests, tools).
  * Classic nodes (`ClassicNodes.cpp` over the pure functions of `Ops.h`): input, output; levels
    (gamma, per channel), curves (a monotone cubic through “x y” points, no overshoot),
    brightness / contrast (GIMP's formula), hue / saturation / lightness (HSL), colour balance (per
    tone range, keeping the HSL lightness by default), invert, black and white (luminosity,
    average, lightness), threshold (Rec. 601 luminosity), posterize: lookup tables on
    unpremultiplied colours, alpha kept. Blur (Gaussian: exact kernel up to radius 3, three box
    passes above), sharpen (4-neighbour Laplacian), unsharp mask (with threshold), median (sliding
    histograms): on premultiplied float planes with the edge extended. Noise from a stateless hash
    of seed and position (the same seed gives the same grain), Gaussian or even, coloured or grey.
    Blend: the 13 layer modes in `doc::BlendMode`'s order with the same QPainter composition modes
    as `doc::flatten` (mirrored, `filters` does not link `doc`), opacity, optional mask; mix by
    mask. Mask from brightness or opacity, grow / shrink / feather, invert mask (`core/ImageOps`).
    Resize (percent or pixels; a height of 0 keeps the proportions), crop (a preview covers the
    preview pixels the rectangle touches, so a crop at the right or bottom edge still shows), flip,
    rotate clockwise (exact for multiples of 90°, the canvas grows otherwise). The output node
    stretches its mask to its image, so the result's image and mask always fit.
  * Tests (`tst_filters`, 35 functions): each node on small synthetic images, exactly where the
    formula is exact (invert, levels, posterize, threshold, curves through a line, flips, rotations
    by 90°, 180° and 270°, crop, nearest-pixel resize, mix, blend modes at black and white, grow /
    shrink, invert mask, hue 180° turning red into cyan) and by properties where it is not (a blur
    keeps the mean and the symmetry and blurs premultiplied, sharpening overshoots at an edge and
    leaves flat areas, noise is the same per seed and keeps the mean, the median removes salt and
    pepper and keeps a straight edge, colour balance and brightness / contrast at their extremes);
    curve tables from unsorted and repeated points. Graph tests: validation messages (also slider
    ranges beyond the parameter's), refused links, a JSON round-trip with every parameter type,
    file errors, the library listing, the cache (counted evaluations: after one parameter change
    only that node and the nodes after it run; text values that would join into the same bytes),
    the preview scale (equal to the full-size operation on the scaled image with half the radius;
    crop rectangles scale, crops at the edge and fixed small heights keep their shape), parameters
    driven by a number output (clamped, scaled), node errors named once, the output's mask at the
    image's size after a resize, cancellation (by the evaluator, by the future, by destruction),
    an asynchronous node (the only worker thread stays free while it waits; failure; no result;
    cancel), expensive nodes (a preview at half size after a *Run* at full size runs only what
    follows; a *Run* in a preview runs the step on the full-size image, and applying then reuses
    it), and a whole-graph golden image (`tests/data/filters/golden.lumenfilter` → `golden.png`,
    at most one level of difference; regenerated with `LUMEN_UPDATE_GOLDEN=1 tst_filters
    goldenImage`).
  * Not yet: the node editor, the Filters panel and the filters folder next to the models folder
    (next step); applying a result as one undo step onto a new layer; the AI nodes. The cache keeps
    one result per node and lane with no memory limit, the nodes of one evaluation run one after
    another, and each node uses one thread.
- [x] Node editor window, saved filters, the Filters panel.
  * Filters folder: `paths::defaultFilters()`, `Documents\Lumen\Filters` next to the models folder;
    with relocated data (`LUMEN_DATA_DIR`, the portable ZIP) or in Qt's test mode
    `<data root>/filters`, as for the models. Not a setting yet (no *Move…*); the Filters panel
    has *Open filters folder*.
  * `ui/filters/FilterCatalog`: the built-in filters (`resources/filters/*.lumenfilter` as
    `:/filters/builtin`, read-only; their names, descriptions and slider labels go through the
    translation context `lumen::ui::BuiltInFilters`) and the saved ones (the folder is watched, so a
    file put there by hand appears; one that does not load is listed with the reason). Create,
    save (a built-in path saves a new file), duplicate (`<name> copy`, the way to edit a built-in
    filter) and delete. Thumbnails: each classic filter on a drawn sample picture (sky, sun, hills, a
    house) at 64 × 48, in the background; filters with AI steps and broken ones show an icon. One
    catalog per Studio (`FilterCatalog::of`), so a save in the node editor refreshes every panel
    (`changed`, `saved`).
  * Built-in filters: Black and white, Warm tones, Cool tones, Sharpen, Soft focus, Film grain, Old
    photo, each with published settings (Strength, Amount, Edge width, Softness, Grain, Brown tint
    as sliders; *Brightness from* and *Grain* as lists). No vignette: there is no gradient node yet.
    The AI ones come with the AI nodes.
  * `EvalOptions::progress`: called on the worker thread before each node that runs (done, total,
    name) and at the end; the panel shows "Blur (step 2 of 3)".
  * `ui/filters/FilterRunner` (no widgets), the preview and *Apply* of the panel:
    * Preview: the whole document at reduced resolution (the canvas's size, 512–2048 px on the
      longest side) with the filter on the active layer, put together as *Apply* would (on a new
      layer, or into the layer); the layers are scaled once and kept while their pixels stay the
      same. `CanvasView::setPreview` shows it instead of the document's tiles (the selection's
      outline stays, its tint goes). A change previews after 40 ms; a stale preview stops at its
      next node. A graph with AI steps previews with `runExpensive` off, so nothing runs until *Run
      AI steps* (`run()`: progress, *Cancel*), which runs them at full resolution; sliders after them
      then preview live. A change while a step runs previews when it ends, instead of cutting it short.
    * Apply: the filter at full resolution on the active layer, with the selection as the graph's
      *Selection* input; the selection times the graph's own mask (the Output's *Where to apply*)
      says where the result goes. On a worker, the result is masked into a new layer above
      (transparent elsewhere, named after the filter, `meta.filter`) or mixed into the layer within
      the mask's bounds (an alpha-locked layer keeps its alpha; a locked layer is refused). A result
      of another size is stretched to the layer. One undo step ("Filter: …", `addLayer` or
      `applyPixels`). The preview and *Apply* share one `Evaluator`, so *Apply* after *Run* reuses
      the AI step. After *Apply* the preview stays off until a setting or the filter changes (it
      would filter the new layer again).
  * `ui/filters/FiltersPanel`, the *Filters* tab of the editor's right panel (`EditorPage` adds it
    after History; its four tabs shorten their names instead of scrolling): the list (*Built in*,
    *Your filters*, thumbnails, descriptions as tooltips); *New filter…*, *Edit…* (*Edit a copy…*
    for a built-in one), *Duplicate*, *Delete* (saved ones, asked first), *Open filters folder*;
    the chosen filter's published settings (`ParamField` with the published label and range);
    *Preview on the canvas* (`filters/preview`), *Result on* a new layer or this layer
    (`filters/target`), a line saying whether the selection or the whole layer changes, *Run AI
    steps* when needed, *Apply*, progress with *Cancel*, the outcome or the reason in plain words.
    The canvas shows the preview only while the tab is visible.
  * Node editor (`ui/filters/NodeEditorWindow`, `NodeScene`, `GraphModel`, `ParamWidgets`), a
    window of its own, owned by the main window:
    * `GraphModel`: the graph and a `QUndoStack` of whole-graph steps (`change(text, edit,
      mergeKey)`); slider drags and typing merge, a slider dragged back leaves no step.
    * Items: the title in the category's colour, ports in their type's colour (hollow: an optional
      input), the settings in short ("Radius: 8 px", five at most), a red or amber outline and
      tooltip for a problem; links as curves in their type's colour, selectable. An item whose id
      comes back with another type (another graph in the window) is made again.
    * Dragging from port to port connects; while dragging, the port under the mouse rings green or
      red. A refusal flashes the link red and the status bar says why (the type, with "Put a “Mask
      from image” node between them."; a loop; two inputs or two outputs). Dragging a connected input
      picks its link up; dropped on nothing, it is removed.
    * *Add node* (Shift+A, or a right-click on an empty place): a popup list by category, searched
      in names and descriptions; Enter adds the first match where the menu opened. Delete (key or
      button), moving (one step), rubber-band selection, wheel zoom, middle button or Space to pan,
      *Show all* (Home, at most 100 %).
    * Side panel: the node's settings (`SliderField`, a list, `ColorSwatch`, `CurveEditor` with the
      points as text; one driven by a connected input is disabled), each with a button that
      publishes it (label and range asked; `validate()` checks the range); the preview of the
      selected node's output (or the filter's result) on the active layer of the editor's document
      (the sample picture without one) at 480 px, with errors in plain words ("“Invert” has nothing
      connected to its “Image” input."), values as text, and *Run AI steps* / *Stop AI steps*; the
      problems of `validate()` (a click selects the node); the filter's name, description and
      published settings (click to select the node, ✕ to remove).
    * *Save* (Ctrl+S; a filter without a file asks for a name), *Save as…*, undo and redo (Ctrl+Z,
      Ctrl+Shift+Z). Closing with changes asks, also when the main window closes. A filter opens in
      one window at most (`openFilter` raises it); the panel opens built-in filters as a copy.
  * Tests: `tst_filters_ui` (new, 16 functions): the folder next to the models; every built-in
    file parses, validates without a warning, publishes a setting and changes a picture, with
    thumbnails; the catalog (create, copies, refusals, a file added by hand, a broken one,
    delete); compositing through the selection; the node editor's model (add, connect, a wrong type
    and a loop refused with their reasons, delete and undo, merged setting steps, the side panel
    after undo, another graph in the same window); mouse drags (two inputs, a mask, an image into a
    mask input refused, a link picked up and dropped, moving a node, the Delete key); publish and
    undo, a range beyond the setting's, Save as, Save, reopen, the close question; the add-node
    search; the node preview (unconnected, connected, the result, no output); AI steps on request
    and Stop; a colour setting through Save; the window it belongs to closing; the curve editor
    by mouse and text; the Filters panel (the list, published sliders and lists, a built-in filter
    edited as a copy; the live preview through the selection, a slider, a hidden tab; Apply onto a
    new layer: one undo step, pixels outside the selection unchanged, the original layer untouched;
    onto the layer; a locked layer; Cancel of a waiting AI step with progress, nothing changed; an AI
    filter previews after Run and Apply reuses it; New, Edit, Duplicate, Delete, the panel after a
    save in the editor); the tab in the editor page. `tst_layout filterTools`: the node editor at
    1280 × 720 and the panel at 300 × 600, no findings (the audit's 7 findings stay as they were).
  * Not yet: the filters folder cannot be moved; slider values are not remembered between sessions;
    the preview's resolution does not follow the zoom; the node editor has no copy and paste and
    does not link a new node to the selected one; a result of another size is stretched to the layer
    (a crop does not crop the document); the German translation of the new texts.
- [ ] AI nodes.
- [ ] Fix the layout checklist (§17.9); screenshots after; then the audit runs strict in CI.
- [ ] User guide and README rewritten around the editor; plugins documented separately.
- [ ] Short live check (RTX 4060 8 GB, FLUX.2 klein 4B through sd.cpp): one selection edit, one
  whole-image colorize, one new image. Everything before this step uses mock servers.
  * Done early for the selection edit (October 2026, `tst_live kleinEditServer`): the user's
    ComfyUI folder, the profile from the preset (`--diffusion-model`, `flux2-vae`, Qwen3 4B
    `qwen_3_4b`, found by their headers), the region pipeline, 4 steps at CFG 1 on a 1024 × 1024
    crop. "Make the red ball blue": blue, the rest unchanged; 38–45 s including the server start
    (sampling 21 s from the GGUF Q8_0 file, 28 s from the fp8 file converted to q8_0; weights in
    RAM). The ref_images request of the pinned sd-server works as mocked.
  * Found: the model redraws the crop's context with a drift that varies across it (paler sky,
    greener grass), which the global colour match cannot remove and which showed as a glow along
    the feathered edge. `img::matchEdgeColors` (after `colorMatch` in `compositeRegionResult`)
    spreads the smoothed context difference into the selection, fading out inside it: the edge
    difference around the ball fell from 5.5 to 4.1 per channel, the ball unchanged
    (`tst_masks edgeColorsRemoveDrift`). Left: light the model adds next to the new object, and a
    new shadow fading out at the selection's edge.
  * Still to check: a whole-image colorize and a new image, with the new shell.

Acceptance:
1. A new user installs Lumen, chooses *Run on this computer*, and edits a selected area with a
   prompt without opening Settings or reading the guide.
2. Choosing FLUX.2 klein, FLUX.1 dev, SDXL or Anima fills every required component; no
   combination that the setup allows makes the server exit at start.
3. "Colorize this photo" on a black-and-white image with nothing selected colours it; with a
   selection, pixels outside the selection are unchanged.
4. The app starts, edits and generates with every plugin off.
5. A saved filter graph (e.g. levels → mask by luminance → AI inpaint) runs from the Filters panel
   on a selection as one undo step.
6. No clipped or overlapping controls at 1280×720 or at 150 % scale.

Risks and open questions:
* Edit models are large: FLUX.2 klein 4B with its text encoder is the realistic choice on 8 GB,
  and is still slower than SDXL. The wizard states download size and expected speed.
* sd-server's reference-image API is newer than its txt2img API and may change between the
  pinned `master-N` builds; the contract test catches it.
* Restructuring touches most UI code; the old pages stay reachable until the new shell covers
  them, so every step builds and passes the tests.
* Moving the models folder must not break server profiles that point at old paths; profiles are
  rewritten in the same step, and kept as they are when the user declines the move.

#### 17.9 Layout audit results (October 2026)

100 automatic findings, checked against the screenshots, plus what only the screenshots show.
Root cause first; most findings follow from it.

1. **The main window cannot be smaller than 1559×885**, more than a 1080p screen at 125 %
   (1536×864) or 150 % (1280×720). The pages share one `QStackedWidget`, whose minimum is the
   largest of all pages, shown or not: Poses needs 1472 px width, Library 1365, Create 1285, Parts
   823 px height. On a laptop, the window is larger than the screen or every page is squeezed.
   Fix: each page fits 1193×658 (the content area at 1280×720); toolbars wrap or move items into
   an overflow menu; the stack must not take the minimum of hidden pages.
   **Fixed** (October 2026): `FlowLayout` (wrapping toolbars; groups that wrap as a whole, and
   right-aligned groups) for the Poses toolbars, the Library header and the Create page's plan
   options; `verticalScroll` for the Library inspector and the four Parts steps; `ElidedLabel` for
   the Parts step hint; the default window size is bounded by the screen. The window's minimum is
   now within 1280×720 on every page, with the queue and the assistant open
   (`tst_layout mainWindowFitsSmallScreen`, strict). This also fixed items 2, 4, 5 and 7, and item 3
   where it appeared (the Poses and Parts opacity fields had fixed widths below their minimum).
   The audit's findings went from 100 to 7: item 6 and item 8.
2. **Poses page** (at 1280×720): the top toolbar's mode buttons show only "…", and the canvas
   ratio and person combo boxes have no room for text. The bottom bar overlaps itself ("Underlay…",
   the opacity slider over "Detect…", "To document" and "Use in Generate" cut).
3. **`SliderField` at a fixed 150 px** (Poses opacity, at every size): its number box is cut to 44
   of 64 px and the slider runs under it.
4. **Library page**: the filter chips need 1365 px; the zoom slider is drawn over the search field;
   the detail panel's preview collapses and "Select an image" is drawn over the name field.
5. **Create page**: the shot-plan toolbar cuts the provider combo ("No provider c"), "Generate
   all" and the purpose combo ("General illus").
6. **`ProviderCombo`** is 7 px too narrow for "No provider configured" in the Generate page and the
   editor's AI panel at 1280 and 1536 px.
7. **Parts page**: the step hint after the stepper is cut at the window edge.
8. **Dialogs taller than a 1280×680 screen**: Settings (1020×700), the server profile dialog
   (640×720, sd.cpp and llama.cpp) and the pose edit dialog (760×860).
9. Only visible in the screenshots: the negative prompt fields (Generate page, AI panel) are one
   line high with a scroll bar; the status bar has no side margin ("No project" and "Idle" touch
   the window edges); the Generate page's aspect-ratio chips are unevenly aligned.

Items 2, 3, 5 and 7 are on pages that become plugins (§17.7), and the Generate and Library pages
are replaced by the new shell; their fixes belong to those steps. Item 1 stays relevant until the
new shell lands, and items 3, 6, 8 and 9 are shared widgets and dialogs that the redesign keeps.

---

## 5. Quality strategy

* Unit tests: math (FK/IK, projection), mask ops, inpaint crop/composite, SSE/multipart parsers,
  JSON schema, request mapping (golden JSON per adapter), PSD writer round-trip.
* Contract tests: mock servers emulating each provider's API including errors, 429 and streaming.
* UI smoke tests: main window, open/save, tool switching, generation flow with mock provider.
* Manual QA checklist per milestone against real providers.
* Performance budget checks: frame time while brushing, GUI-thread stall watchdog in debug builds.

## 6. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Character consistency varies by model | multiple strategies (§2.2), capability-based choice, critic loop, easy inpaint fixes |
| LLM structured output unreliable (small local models) | JSON schema / grammar, validation + repair loop, rig parameters instead of raw coordinates, preset fallback |
| Provider API drift | isolated adapters, contract tests, provider-specific "extras" passthrough, ComfyUI templates as data files |
| Memory pressure with large images | tiles, implicit sharing, bounded undo, thumbnail cache, lazy loading |
| Local server unavailable / slow | health checks, clear status indicators, queue persistence, cancellation |
| Part segmentation models are heavy (8–16 GB VRAM, Python) | out-of-process via ComfyUI / sidecar, cloud and heuristic fallbacks, every step editable by hand |
| Live2D Cubism SDK licence | built-in puppet preview needs no SDK; the Cubism Core is loaded at runtime from the user's own SDK copy |
| Downloaded runtimes / models could be tampered with or malicious | pinned GitHub repositories only, mandatory sha256 (release digests, HF LFS oids, Civitai hashes), nothing executed except known server executables, pickle formats refused by default |
| Upstream release naming or server API changes (llama.cpp daily builds, sd.cpp `master-N` tags) | catalogue as data with tag/asset patterns, adopted installs keep working, contract tests against a mock sd-server, rollback to the previous runtime |
| Local servers compete for one GPU | supervisor-level GPU arbitration on top of `releaseGpu()`, llama.cpp sleep-on-idle, sd-server offload or stop/start, VRAM presets |
| Missing optional Qt modules | CMake feature detection, graceful degradation (polling progress, PNG/JPEG only) |
