# Changelog

## 0.2.1 - 2026-11-10

- **OpenAI's current image models.** OpenAI shut down `gpt-image-1`, Lumen's default, on 23 October 2026.
  Lumen now uses GPT Image 2.6 Sunburst and moves a saved engine to it by itself, with a note. GPT
  Image 2.6 Flare (faster) and 2.5 are there to choose, and results keep the picture's own shape
  instead of one of three fixed sizes.
- Google's newer Nano Banana models (2.1, 2, 2 Lite and Pro) can be chosen for Gemini.
- Models a service has shut down are no longer offered, and saved ones move to their replacement.
- Fixed: a row of the LoRA test results could not be deselected.

## 0.2.0 - 2026-11-01

The first public release of Lumen Studio, a desktop image editor for Windows that changes pictures from
what you describe. The AI runs on your own computer or through an online service.

- **An editor first:** layers, history, selection tools, brush, fill, crop and free transform. Opens
  and saves PNG, JPEG, WebP, TIFF, BMP and layered PSD.
- **Describe a change:** select an area and say what should change. Pixels outside the selection stay
  exactly as they were. *Select → By description…* finds the area from your words, such as "the black
  lines" or "the cup".
- **New images** from a description, with 1 to 4 results to compare.
- **Fine control when you want it:** the **⋯** button next to Generate holds how much may change
  (denoise), LoRAs, references, CFG, steps, sampler and seed, showing only what the model uses.
- **Plan this change:** a language model that sees pictures can propose what to select and what to ask
  for, and you can change the plan before anything runs.
- **Filters:** built-in ones, filters with AI steps, and your own in a node editor.
- **Easy setup:** *Set up AI* runs a model on this computer (it suggests one for your graphics card and
  downloads only what it does not find), uses an online service (fal.ai, OpenAI, Google Gemini), or
  connects a server you already run (ComfyUI, Forge / WebUI, stable-diffusion.cpp).
- **Plugins**, off by default: character creation with an AI director, two characters in one image, a
  pose editor, and Live2D part slicing.
- **Updates:** Lumen tells you about new versions and installs them with a click. You can turn the
  check off in *Settings → General*.
- English and German; dark, light and high-contrast themes.

**Install:** run `LumenStudio-0.2.0-win64.exe`, or unzip the portable version and run `LumenStudio.exe`.
Lumen is not code-signed yet, so Windows warns about the download: choose *More info → Run anyway*.
