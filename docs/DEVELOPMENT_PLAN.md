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
- [ ] Tool-calling assistant (plan, generate, edit, pose) — **AC**: reference scenario (§1.1) completes with SD WebUI + llama.cpp and with a cloud provider.

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
| Missing optional Qt modules | CMake feature detection, graceful degradation (polling progress, PNG/JPEG only) |
