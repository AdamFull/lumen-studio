# Lumen Studio

AI-assisted image generation and editing studio for the desktop, built with C++20 and Qt 6.

It covers:
* text-to-image and multi-reference character generation;
* editable OpenPose poses, with a Live2D preset category;
* mask-based local inpainting, with a prompt bar that appears next to the selection on the canvas;
* LLM-driven shot planning;
* a layered editor (selection, brush, fill, crop and free transform tools);
* an asset library with batch background removal and upscaling;
* character part slicing for Live2D.

The interface is available in English and German, with dark, light and high-contrast themes.

Local back-ends (Stable Diffusion WebUI / Forge, ComfyUI, llama.cpp, Ollama, LM Studio) and cloud
APIs (OpenAI, Gemini, Anthropic, Stability AI, fal.ai, OpenRouter) both work.

The [user guide](docs/USER_GUIDE.md) explains every page, tool and shortcut. See
[docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) for the architecture, the technology stack and the roadmap.

## Building (Windows, MinGW)

Requirements:

* Qt 6.9 (MinGW 64-bit kit) with the optional modules **Qt WebSockets** and **Qt Image Formats**
* MinGW-w64 13 (shipped with Qt), CMake ≥ 3.25 and Ninja

```bash
# QT_ROOT must point at the Qt kit; MinGW, CMake and Ninja must be on PATH.
cmake --preset mingw-debug
cmake --build --preset mingw-debug
scripts/run-tests.sh build/mingw-debug
```

To keep machine-specific paths out of the repository, put them in a `CMakeUserPresets.json`
(it is git-ignored) that inherits `mingw-debug` and sets `QT_ROOT` and `PATH`.

### Packaging

```bash
scripts/package.sh
```

It builds a release, deploys Qt and the MinGW runtime with `windeployqt` into
`dist/LumenStudio-<version>-win64/`, and writes:

* `dist/LumenStudio-<version>-win64-portable.zip`. Its `portable.txt` marker keeps settings, cache
  and projects in a `userdata` folder next to the executable.
* `dist/LumenStudio-<version>-win64.exe`, an NSIS installer, when `makensis` is installed.

`scripts/make-icon.py` regenerates the application icon.

### Translations

`cmake --build build/<dir> --target update_translations` refreshes `translations/*.ts` from the
sources. Edit them with Qt Linguist, then run `python scripts/check-translations.py`; it reports
unfinished entries and broken placeholders, tags or accelerators. Prompts sent to AI models are kept
out of `tr()` on purpose.

## Character slicing for Live2D

The **Parts** page (Alt+5) turns a finished character illustration into named, ordered layers for
Live2D Cubism, in four steps:

1. **Source**: pick an image, optionally remove its background, and choose the part layout.
2. **Masks**: segment automatically, then fix the masks.
   * Segmenters: see-through through the [Lumen Part Server](tools/partserver/README.md), a ComfyUI
     segmentation workflow, Gemini, or the built-in skeleton segmenter.
   * Mask tools: brush, lasso, magic wand, cut line, split (left/right, islands, skeleton) and merge.
3. **Generate**: complete the hidden areas of every part, the selected parts, or a single part.
   Methods: inpainting with any image provider, see-through model layers, or cut-out only. Expression
   variants (eyes closed, mouth shapes) can be generated for blinking and lip-sync.
   With ComfyUI and the ComfyUI-layerdiffuse nodes, the LayerDiffuse method generates the hidden areas
   with real transparency.
4. **Preview & export**: animate the result as a 2.5D puppet, then export a layered PSD, a PNG folder
   with `parts.json`, or editor layers. The *Cubism model* tab previews a model rigged in Cubism Editor
   (`.model3.json`). It uses the Cubism Core library from your own copy of the Live2D Cubism SDK for
   Native, which Lumen loads at runtime. Physics (`physics3.json`) are simulated, with optional wind.
   Every motion group can be played (idle motions resume afterwards), expressions (`exp3.json`) crossfade
   in and out, and `pose3.json` part switching is applied.

A layered PSD can also start a session (*Open layered PSD…*): every visible layer becomes a complete
part. The editor opens and exports layered PSDs too.

Sessions are saved in the project. To start one from the command line:

```bash
LumenStudio --slice character.png
```

To preview a Cubism model directly:

```bash
LumenStudio --cubism path/to/model.model3.json
```

## Repository layout

| Path | Contents |
|---|---|
| `src/core` | logging, settings, secrets, jobs, async helpers, image operations, JSON schema |
| `src/net` | HTTP client (retry, streaming, multipart), SSE parser |
| `src/pose` | OpenPose skeleton, renderer, FK/IK editing, 3D generation rig, presets |
| `src/doc` | projects (SQLite), assets, layered documents, undo |
| `src/ai` | provider framework, provider adapters, director pipeline, part slicing |
| `src/live2d` | Cubism Core runtime binding, model/motion loading, motion/expression/pose playback, physics, software renderer |
| `src/ui` | main window, canvas, tools, panels, dialogs |
| `src/app` | executable entry point |
| `tests` | unit and contract tests (mock HTTP server) |
| `resources/workflows` | bundled ComfyUI workflow templates (copied to `<bin>/data`) |
| `tools/partserver` | reference Lumen Part Server (see-through sidecar) |
