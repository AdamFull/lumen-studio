# Lumen Studio

AI-assisted image generation and editing studio for the desktop, built with C++20 and Qt 6.

It covers text-to-image, multi-reference character generation, editable OpenPose poses, mask-based
local inpainting, LLM-driven shot planning and a layered editor. Local back-ends (Stable Diffusion
WebUI / Forge, ComfyUI, llama.cpp, Ollama, LM Studio) and cloud APIs (OpenAI, Gemini, Anthropic,
Stability AI, fal.ai, OpenRouter) both work.

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
