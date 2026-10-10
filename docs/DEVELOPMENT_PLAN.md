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
the servers load. Downloading models from Hugging Face and Civitai comes in a later phase. Remote
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
- [ ] llama-server profiles. Done: model + mmproj, context, `-ngl`, KV type, FA, `--jinja`,
  sleep-idle, reasoning budget, and the first local LLM becomes the assistant. Still to do: VRAM
  presets, `--fit`, and role defaults (a vision profile for the analyst and the Critic).
- [ ] Adopted ComfyUI / SD WebUI start-stop through the supervisor.

Phase D — model library
- [ ] Index (scan roots, GGUF / safetensors header parsing, lazy sha256), browser UI, fit hints,
  Civitai by-hash enrichment (opt-in), starter sets and "Set up local studio".

Phase E — hub downloads
- [ ] Hugging Face search and download (revision pinning, gated token, quant picker, mmproj
  pairing).
- [ ] Civitai search and download (token, NSFW filter, hashes, licence fields).
- [ ] Onboarding: a "Set up local AI" path (hardware → recommended runtimes + starter set with
  sizes → install → start → test generation).

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
