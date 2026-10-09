# Lumen Studio

AI-assisted image generation and editing studio for the desktop, built with C++20 and Qt 6.

It covers:
* text-to-image and multi-reference character generation;
* editable OpenPose poses, with a Live2D preset category;
* mask-based local inpainting;
* LLM-driven shot planning;
* a layered editor;
* character part slicing for Live2D.

Local back-ends (Stable Diffusion WebUI / Forge, ComfyUI, llama.cpp, Ollama, LM Studio) and cloud
APIs (OpenAI, Gemini, Anthropic, Stability AI, OpenRouter) both work.

See [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) for the architecture, the technology stack and the roadmap.

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
4. **Preview & export**: animate the result as a 2.5D puppet, then export a layered PSD, a PNG folder
   with `parts.json`, or editor layers.

Sessions are saved in the project. To start one from the command line:

```bash
LumenStudio --slice character.png
```

## Repository layout

| Path | Contents |
|---|---|
| `src/core` | logging, settings, secrets, jobs, async helpers, image operations, JSON schema |
| `src/net` | HTTP client (retry, streaming, multipart), SSE parser |
| `src/pose` | OpenPose skeleton, renderer, FK/IK editing, 3D generation rig, presets |
| `src/doc` | projects (SQLite), assets, layered documents, undo |
| `src/ai` | provider framework, provider adapters, director pipeline |
| `src/ui` | main window, canvas, tools, panels, dialogs |
| `src/app` | executable entry point |
| `tests` | unit and contract tests (mock HTTP server) |
| `resources/workflows` | bundled ComfyUI workflow templates (copied to `<bin>/data`) |
| `tools/partserver` | reference Lumen Part Server (see-through sidecar) |
