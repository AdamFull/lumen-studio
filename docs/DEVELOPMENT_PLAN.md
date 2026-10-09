# Lumen Studio — Development Plan

AI-assisted image generation and editing desktop application built with C++20 and Qt 6.

> Status: living document. Each milestone has a checklist; tick items as they land.

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
| **ComfyUI** | REST `/prompt`, `/upload/image`, `/history`, `/view`, `/object_info`, `/interrupt`, **WebSocket `/ws`** | workflow templates (API format) with declarative bindings: SDXL/Illustrious + ControlNet Union + IP-Adapter, FLUX.1 Kontext (multi-reference), Qwen-Image-Edit (multi-image + pose), inpaint (InpaintModelConditioning), upscale, DWPose preprocessor, background removal |
| **OpenAI Images** | `/v1/images/generations`, `/v1/images/edits` (multipart, multiple `image[]`, mask) | GPT-Image models: text-to-image, multi-reference edit, masked edit, transparent background |
| **Google Gemini / Imagen** | `generateContent` (inline image parts), Imagen `predict` | multi-image reference editing, instruction edits |
| **Stability AI** | REST v2beta `stable-image/*` (multipart) | generate (core/ultra/SD3.5), inpaint, outpaint, search-and-replace, control/structure, sketch, remove-background, upscale |
| **fal.ai** | queue REST API | FLUX (pro/dev/Kontext), other hosted models; generic JSON mapping |

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
    ai/          lumen_ai        provider interfaces + registry, request model, providers/*,
                                 comfy workflow templates, director pipeline, prompt templates
    ui/          lumen_ui        main window, theme, canvas, tools, panels, dialogs, chat, pose editor
    app/         lumen (exe)     main.cpp, resources, icons, translations
  resources/     icons (SVG), QSS theme, presets, comfy workflows, prompt templates
  tests/         unit + contract + ui smoke tests, mock servers
  docs/          plan, architecture notes, provider guides, user guide
```

Dependency direction: `app → ui → ai, doc, pose → net, core`. No module depends on `ui` except `app`.

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
- [ ] Git repo, `.gitignore`, `.gitattributes`, README, this plan
- [ ] CMake project, presets (`mingw-debug`, `mingw-release`), warnings, module libraries
- [ ] Application skeleton: `QApplication`, organisation/app names, high-DPI, single main window
- [ ] Theme engine: dark/light palettes, QSS, SVG icon set tinted to theme
- [ ] Logging (categories, rotating file log, redaction), crash-safe log flush
- [ ] Settings service, paths (AppData, cache, logs)
- [ ] Test harness (Qt Test, CTest) — **AC**: `cmake --preset mingw-debug && ctest` green, app launches with themed empty window.

### M1 — Core infrastructure
- [ ] Async utilities (`runAsync`, `then` on GUI thread, cancellation token)
- [ ] Job & JobQueue with per-provider concurrency, cancel, retry
- [ ] HTTP client: JSON helpers, timeouts, retry/back-off, multipart, SSE parser, error mapping
- [ ] SecretStore (wincred) + fallback interface
- [ ] Image ops: async decode/encode, base64, scaling, mask dilate/erode/feather, compositing
- [ ] JSON-Schema-subset validator
- [ ] Mock HTTP server for tests — **AC**: unit tests for SSE, multipart, retry, mask ops, schema validator.

### M2 — Provider framework + first adapters
- [ ] Interfaces, request/response model, capabilities, registry, settings schema
- [ ] SD WebUI adapter (txt2img/img2img/inpaint/ControlNet/progress/interrupt/discovery/upscale)
- [ ] OpenAI-compatible LLM adapter (streaming, vision, json_schema, tools, model list)
- [ ] Providers settings dialog (auto-generated forms, test connection, refresh models)
- [ ] Auto-detection of local servers on default ports — **AC**: contract tests against mock servers; manual test with real A1111 and llama.cpp.

### M3 — Canvas & editor core
- [ ] Tiled raster layer, document, composite cache
- [ ] GPU canvas view: zoom/pan/rotate, fit, checkerboard, pixel grid, overlays
- [ ] Tools: move, brush, eraser, rect/ellipse/lasso selection, mask brush/eraser, eyedropper, fill, crop, transform
- [ ] Layers panel (opacity, blend, visibility, lock, reorder, duplicate, merge)
- [ ] Undo/redo with tile-delta commands, history panel
- [ ] Open/import/export (PNG/JPEG/WebP), clipboard, drag & drop — **AC**: 60 fps brush on 4096² doc, undo memory bounded.

### M4 — Generation workspace
- [ ] Project store (SQLite) + asset library + thumbnails (async)
- [ ] Generation panel: provider/model picker, prompt/negative, size presets, seed, steps, cfg, sampler, batch, references, controls — capability-driven
- [ ] Queue panel with live previews, cancel/retry
- [ ] Gallery: virtualised grid, filters, favourites, metadata inspector, "send to editor/use as reference/re-run"
- [ ] PNG metadata (A1111 `parameters` chunk) read/write — **AC**: end-to-end txt2img with SD WebUI, UI never stalls.

### M5 — AI editing
- [ ] Inpaint pipeline (§3.6) for all image providers; outpaint; img2img variations
- [ ] Instruction edit (edit-capable providers), upscale, background removal
- [ ] Contextual generative toolbar on the canvas (prompt field near selection) — **AC**: inpainting a 4096² image modifies only the masked area; result on new layer.

### M6 — Pose system
- [ ] Skeleton model, OpenPose renderer, JSON import/export
- [ ] Pose editor: drag/FK/IK, mirror, presets, multi-person, hands/face toggles, reference underlay
- [ ] 3D FK generation rig + projection; preset library
- [ ] Pose detection from image (provider-based; ONNX optional) — **AC**: pose map works as ControlNet OpenPose input in A1111 and ComfyUI.

### M7 — Chat & Director
- [ ] Chat panel: threads, streaming markdown, attachments (drag & drop, paste), image previews
- [ ] Vision analyst → Character Sheet (editable, stored in library)
- [ ] Director → Shot Plan (schema-validated, repair loop), purpose profiles (Live2D, sticker set, turnaround sheet, portrait set…)
- [ ] Pose drafts board; per-shot approve/edit/regenerate pose
- [ ] Batch generation with reference strategies; review board
- [ ] Tool-calling assistant (plan, generate, edit, pose) — **AC**: reference scenario (§1.1) completes with SD WebUI + llama.cpp and with a cloud provider.

### M8 — Additional providers
- [ ] ComfyUI adapter (workflow templates + bindings, uploads, WebSocket progress/previews, interrupt, object_info discovery) + bundled workflows
- [ ] OpenAI Images, Google Gemini (LLM + image), Anthropic LLM, Stability AI, fal.ai — **AC**: contract tests for every adapter; manual smoke test per provider.

### M9 — Live2D & advanced
- [ ] Layered PSD export (RLE), import of PSD layers
- [ ] Expression/mouth/eye variant generation via masked inpainting
- [ ] Background removal batch, part-separation assistant (mask per part → layers)
- [ ] Character library reuse across projects; Critic loop

### M10 — Polish & release
- [ ] Onboarding wizard (detect local servers, add cloud keys), keyboard shortcuts editor
- [ ] Autosave/crash recovery hardening, performance profiling pass, latency watchdog
- [ ] i18n (Qt Linguist), accessibility names, high-contrast check
- [ ] Packaging (windeployqt, NSIS installer, portable ZIP), user guide

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
| Missing optional Qt modules | CMake feature detection, graceful degradation (polling progress, PNG/JPEG only) |
