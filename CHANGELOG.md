# Changelog

## 0.3.0 - 2026-11-15

Colour grading on the graphics card, and effects of your own.

- **Develop**, a new tab on the right, grades the active layer like a photo editor, live on the
  graphics card. It has white balance, exposure, contrast, highlights and shadows, whites and blacks,
  vibrance and saturation. Highlights and Shadows work by area, so a dark coat keeps its folds. It
  also has a tone curve, a colour mixer for eight colours, colour grading wheels for shadows,
  midtones and highlights, and LUTs (`.cube` files). Then come texture, clarity and dehaze,
  sharpening and noise reduction, and a vignette and film grain.
- The canvas shows the change at once while a slider moves; the full-size result follows in the
  background, as one step to undo. The grading stays editable, also after saving the project:
  **Reset** takes it off, **Before** shows the picture as it was, **Bake** makes it part of the
  layer so you can paint on it.
- **Presets:** five looks to start from (*Black and white*, *Cool and crisp*, *Soft matte*, *Vivid*,
  *Warm film*) and your own. **Copy** and **Paste** carry settings from one picture to another.
  *Apply to picture files…* grades many photos at once and saves copies, leaving the originals as
  they are.
- **In the filter editor,** each Develop group is a step of its own. Several in a row run as one on
  the graphics card, so a long row costs about as much as a short one.
- **Your own effects:** *Tools → Effect editor…* is for writing effects in Slang. Your source is
  compiled as you type, with mistakes underlined where they are, and a live preview with your
  effect's sliders. It works like a code editor, with colours, completion, notes on Lumen's functions
  when you hover over them, find and replace, and the usual keys. Saved effects become filter steps.
  You can share them as `.slang` files; when you open one from someone else, you see its source
  before anything is added. The Slang compiler is a download of about 65 MB, the first time you
  write an effect.
- **OpenAI's current image models.** OpenAI shut down `gpt-image-1`, Lumen's default, on 23 October
  2026. Lumen now uses GPT Image 2.6 Sunburst and moves a saved engine to it by itself, with a note.
  GPT Image 2.6 Flare (faster) and 2.5 are there to choose, and results keep the picture's own
  shape instead of one of three fixed sizes.
- Google's newer Nano Banana models (2.1, 2, 2 Lite and Pro) can be chosen for Gemini.
- Models a service has shut down are no longer offered, and saved ones move to their replacement.
- Fixed: a row of the LoRA test results could not be deselected.

The Develop tab and effects need a graphics card with Vulkan 1.3, which most cards from the last
several years have; without one, the tab says so and everything else works as before.

**Install:** run `LumenStudio-0.3.0-win64.exe`, or unzip the portable version and run
`LumenStudio.exe`. Lumen 0.2.0 offers this update by itself. Lumen is not code-signed yet, so
Windows warns about the download: choose *More info → Run anyway*.

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
