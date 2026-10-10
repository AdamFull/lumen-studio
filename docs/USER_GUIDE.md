# Lumen Studio — User Guide

Lumen Studio is a desktop studio for generating and editing images with AI. It works with image
generators on your own machine (Stable Diffusion WebUI / Forge, ComfyUI) and with cloud services
(OpenAI, Google Gemini, Stability AI, fal.ai). Language models (llama.cpp, Ollama, LM Studio, OpenAI,
Anthropic, Gemini, OpenRouter) plan shots and edit poses.

## 1. Installing

* **Installer** (`LumenStudio-<version>-win64.exe`): installs to *Program Files* and adds Start-menu
  and desktop shortcuts. Settings and data are stored per user (see §12).
* **Portable ZIP** (`LumenStudio-<version>-win64-portable.zip`): unzip anywhere (for example a USB
  drive) and run `LumenStudio.exe`. The `portable.txt` file next to the executable keeps settings,
  logs, cache and projects in the `userdata` folder beside it. Delete `portable.txt` to use the
  per-user locations instead.

API keys are never written to disk by Lumen. They are kept in the Windows Credential Manager, also
in portable mode.

## 2. First run: providers

On the first start, the assistant looks for local servers on their default ports (SD WebUI 7860,
Forge 7861, ComfyUI 8188 / 8000, llama.cpp 8080, Ollama 11434, LM Studio 1234, KoboldCpp 5001, the
Lumen part server 7870) and offers to add them. You can open the
same list later with **Edit → Settings… → Providers** (`Ctrl+,`).

* **SD WebUI / Forge**: start it with `--api`. Background removal needs the *rembg* extension; pose
  detection and pose control need *ControlNet*.
* **ComfyUI**: Lumen builds standard workflows for text-to-image, image-to-image and inpainting.
  Your own API-format workflows go in the folder opened by **Help → Open ComfyUI workflow folder**.
* **Cloud providers**: paste the API key; **Test connection** checks it. Each provider has a
  *Parallel requests* limit that the job queue respects.

Every generation runs as a job in the **Queue** (`Ctrl+Shift+Q`), where it can be cancelled or
retried.

### One GPU for the language model and the image model

With 8-12 GB of VRAM, a local language model (llama.cpp, Ollama, LM Studio) and SDXL in ComfyUI do
not fit at the same time. Whichever loads second ends up mostly on the CPU and runs many times
slower. To let them take turns:

* start llama.cpp with `--sleep-idle-seconds 30` so it unloads its model when idle;
* turn on **Free VRAM when idle** in the ComfyUI provider settings.

Lumen also keeps them apart. The Critic reviews only after all shots of a batch are generated, and
retries start after all reviews. The assistant hands generated results back to the language model
only after ComfyUI has unloaded its models.

A tested setup on an 8 GB RTX 4060 laptop GPU:

```
llama-server -m Qwen3VL-8B-Instruct-Q4_K_M.gguf --mmproj mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf ^
  --port 8080 -ngl 99 -c 16384 -ctk q8_0 -ctv q8_0 -fa on --jinja --sleep-idle-seconds 30
```

ComfyUI ran an SDXL anime checkpoint (Animagine XL 4.0 or WAI-Illustrious) with the xinsir OpenPose
ControlNet. Qwen3-VL-8B handles the assistant, planning, reference analysis and the Critic.

### Local servers: let Lumen run llama.cpp and stable-diffusion.cpp

Instead of starting servers yourself, Lumen can install and run them. Open **Settings → Local
servers**.

1. **Get the program.** Use **Download…** for a release from GitHub. Lumen picks the build for your
   graphics card (CUDA for NVIDIA, Vulkan for AMD and Intel, CPU otherwise) and shows the download
   size. Files are checked against the checksums GitHub publishes. Alternatively, **Add existing…**
   lists llama-server and sd-server copies it finds in Program Files, your home folder and
   Downloads, or you can browse to one.
2. **Create a server.** **New server…** asks for the model files and the launch options. Defaults
   suit an 8 GB GPU. The dialog shows the full command line. Leave **Add it as a provider** on, and
   the server appears under Providers too.
3. **Use it.** A server set to start *When needed* starts on the first request: a chat message, a
   generation or a Critic review. The job shows "Starting …" meanwhile. Servers stop when Lumen
   closes, or after the idle time you set.

**Image generation with stable-diffusion.cpp.** Choose a checkpoint (an SDXL model such as
WAI-Illustrious or Animagine). For pose control add an OpenPose ControlNet (e.g. xinsir
`controlnet-openpose-sdxl-1.0`). For character references add the IP-Adapter
`ip-adapter_sdxl_vit-h.safetensors` and its image encoder `CLIP-ViT-H-14-laion2B-s32B-b79K`
(both from `h94/IP-Adapter` on Hugging Face). The defaults suit an 8 GB GPU:
* *Weight precision q8_0* halves the model with no visible change, so the model and the ControlNet
  fit in VRAM.
* *Keep weights in RAM* frees the GPU between images, so the language model can use it.

On an RTX 4060 laptop a 832×1216 image takes about 30–40 s, and 75–80 s with pose control and a
reference. A strong IP-Adapter also copies the reference's pose, so next to a pose map its weight
is limited to 0.3 (provider setting *IP-Adapter weight with pose control*). An image that is
already generating cannot be stopped by the server: cancelling ends the job in Lumen at once, and
the server finishes that image in the background.

The server button in the status bar shows what runs and offers start, stop, restart and the
server's log. **Check for updates** downloads newer releases. Servers switch to them at their next
start, and the previous version stays installed, so you can switch back in the server's settings.

## 3. Pages

The left bar switches pages (`Alt+1` … `Alt+6`).

| Page | Use it to |
|---|---|
| **Create** | chat with the director: describe a character or a story, attach references, get a character sheet and a shot plan with editable poses, then generate every shot |
| **Generate** | classic prompt → images, with references, a control pose, size, seed, steps and batch |
| **Edit** | the layered editor with AI editing (§5, §6) |
| **Poses** | edit OpenPose skeletons, use presets (including the Live2D category), detect poses from images, ask the LLM to change a pose |
| **Parts** | slice a character into parts for Live2D (§8) |
| **Library** | every generated, imported and edited image with its full parameters (§7) |

`Ctrl+Return` starts the generation on the Create and Generate pages.

### Reusing characters across projects

In the **Character** card of the Create page, **Library → Save character to library…** keeps the
character sheet and its reference images outside the project. **Library → Load character from
library…** brings them into any other project: the references are added to the project and to the
session, and the sheet is set. Saving under an existing name updates that character.

### Critic

With **Critic** turned on, a vision model reviews every generated shot. It compares the result with
the references, the character sheet and the planned pose and framing, and scores it out of 10.
When no result of a shot reaches the minimum score, the shot is generated again with the critic's
improved prompt, up to the number of retries you set. Result thumbnails get a green border when
accepted and an amber one otherwise; hover them to read the scores and the issues. The verdict is also
stored in the image's library entry. The critic uses the model assigned to *Vision analysis*
(Settings → Models & roles).

## 4. The assistant

**Assistant** in the left rail (`Ctrl+Shift+A`) opens a chat that works on every page. Describe
what you want in your own words, and the assistant acts through the studio's tools:

* look at the project and the library;
* generate images, with references for character consistency;
* change an image by instruction or repaint a region;
* remove backgrounds, upscale, and open results in the editor;
* run the director: analyse references, plan shots, adjust poses and generate the shots, with
  Critic retries when the Critic is on.

Every step shows as a card with its result; click a thumbnail to open the image in the editor. Results
are new images in the library, and originals are never overwritten. **Stop** interrupts a run;
**+** starts a new chat.

The assistant uses the model assigned to the *Assistant* role (Settings → Models & roles). Models
with tool calling are used natively (llama.cpp started with `--jinja`, Ollama, LM Studio, OpenAI,
Anthropic, Gemini). Others follow a JSON protocol; for those, turn off *Tool calling* in the
provider settings.

## 5. The editor

Open images with **File → Open image…** (`Ctrl+O`) or drag them onto the window. PSD files open with their layers;
**File → Export image…** (`Ctrl+Shift+E`) writes PNG, JPEG, WebP or a layered PSD.

### Tools

| Tool | Key | Notes |
|---|---|---|
| Move layer | `V` | arrow keys nudge (Shift = 10 px) |
| Free transform | `T` | drag inside to move, a handle to scale (Shift keeps the aspect, Alt scales from the centre), outside the box to rotate (Shift snaps to 15°); Flip and ±90° buttons in the options bar. With a selection only the selected pixels move, and the selection follows. Enter applies, Esc reverts the whole transform |
| Rectangle / ellipse / lasso selection | `M` / `Shift+M` / `L` | Shift adds, Alt subtracts, Shift+Alt intersects |
| Mask brush | `Q` | paints the area to regenerate; Alt or the right button erases |
| Brush / eraser | `B` / `E` | `[` and `]` change the size; pen pressure is supported |
| Fill | `G` | tolerance and sample-merged options |
| Eyedropper | `I` | Alt+click picks the background colour |
| Crop / canvas size | `C` | drag beyond the image to enlarge the canvas (for outpainting) |
| Pan | `H` | or hold Space |

### Shortcuts

Every shortcut can be changed in **Edit → Settings… → Shortcuts**: search for an action, click the
*Shortcut* or *Alternative* field and press the new keys. A key that another action in the same
place uses moves to the new action (the page tells you which one lost it); *Reset to default* and
*Reset all* restore the defaults. The defaults are:

| Action | Keys |
|---|---|
| Undo / redo | `Ctrl+Z` / `Ctrl+Shift+Z` or `Ctrl+Y` |
| Select all / deselect / invert | `Ctrl+A` / `Ctrl+D` / `Ctrl+Shift+I` |
| Delete selected pixels | `Del` |
| New layer / duplicate / merge down | `Ctrl+Shift+N` / `Ctrl+J` / `Ctrl+E` |
| Paste as layer / copy merged | `Ctrl+V` / `Ctrl+Shift+C` |
| Fit / 100 % / zoom in / zoom out | `Ctrl+0` / `Ctrl+1` / `Ctrl+=` / `Ctrl+-` |
| Swap colours | `X` |

## 6. AI editing

Make a selection or paint a mask, then either:

* type in the **prompt bar** that appears under the selection and press Enter (**Generate**), or
  click **Remove** to fill the area from its surroundings. The bar hides while you draw; ✕ hides it
  until the selection changes, and **View → Generative toolbar on the canvas** turns it off; or
* use the **AI** tab on the right for all options: fill selection, variation, instruction edit,
  expand (outpainting), upscale, remove background and pose detection, with references, strength,
  padding, feather and colour matching.

Results are added as new layers, so the original stays untouched. Only the masked area changes:
Lumen crops the region around the mask, generates it at the model's working resolution and
blends it back.

## 7. Library

* Filter by kind, favourites or text; the inspector shows the prompt and all parameters.
* Right-click selected images to open them in the editor, use them as references, slice them into
  parts, export them, or run **Remove background** / **Upscale 2×** on all of them at once. Each
  result becomes a new library image linked to its source.

## 8. Character part slicing (Live2D)

The **Parts** page turns a character illustration into a layered PSD for rigging:

1. **Source**: pick an image (A-pose works best), or open a layered PSD.
2. **Masks**: automatic segmentation (part server, ComfyUI or SD WebUI), then refine the masks
   with the brush, lasso, magic wand (`W`), cut line (`C`), skeleton (`K`) and split/merge commands.
3. **Generate**: complete hidden areas of every part, the selected parts or one part at a time.
   Methods: inpainting with any image provider, see-through layer models, LayerDiffuse (ComfyUI)
   or cut-out only. Expression variants (closed eyes, open mouth) can be generated too.
4. **Preview & export**: animate the result with the built-in puppet, then export a layered PSD
   (Cubism-friendly names and order) or a PNG folder with `parts.json`.

### Cubism model preview

The **Cubism model** tab previews a model rigged in Cubism Editor (`.model3.json`). Lumen cannot ship
the Live2D Cubism Core; click **Locate Cubism Core…** and pick `Live2DCubismCore.dll` from your own
*Cubism SDK for Native*. The preview plays every motion group (click a motion; idle motions resume afterwards),
crossfades expressions, applies poses and physics (with optional wind), follows the mouse, can
lip-sync a talking loop, and exposes every parameter as a slider.

## 9. Projects

Everything lives in a project file (`.lumen`): the library images and the editor documents. Modified
documents are saved into the project a few seconds after your last change, every 60 seconds, and
when the project closes. The documents that were open come back when the project opens again, also
after a crash. In that case Lumen tells you, and a crash report (`crash-*.txt` with a `.dmp`
minidump) is in the log folder. Closed documents stay in the **Documents** view of the Library.
**File → Open project…** (`Ctrl+Shift+O`) switches projects; the last one opens on start.

## 10. Command line

```
LumenStudio [--project file.lumen] [--page editor|generate|create|poses|parts|library]
            [--image picture.png|layers.psd] [--tool transform] [--slice picture.png]
            [--cubism model.model3.json]
```

## 11. Language, themes and accessibility

* **Language**: Lumen Studio is available in English and German. **Settings → General → Language**
  picks one ("System default" follows Windows); restart to apply. Numbers follow the language's format.
* **Theme**: *Automatic* (the default) uses the dark theme, or the high-contrast theme when Windows high
  contrast is on. *Dark*, *Light* and *High contrast* can be chosen explicitly (restart to apply). The
  high-contrast theme uses the Windows high-contrast colours. All themes meet the WCAG 2.1 contrast
  ratios for text (4.5:1, 7:1 in high contrast).
* **Screen readers**: every button and field has an accessible name, including icon-only buttons.

## 12. Where data is stored

| What | Installed | Portable |
|---|---|---|
| Settings, provider configuration, logs, character library | `%LOCALAPPDATA%\Lumen\Lumen Studio` | `userdata\` |
| Server programs downloaded by Lumen, server settings | `…\Lumen Studio
untimes`, `servers.json` | `userdata
untimes` |
| Server logs | `…\Lumen Studio\logs\servers` | `userdata\logs\servers` |
| Cache (thumbnails) | `%LOCALAPPDATA%\Lumen\Lumen Studio\cache` | `userdata\cache` |
| Projects | `Documents\Lumen Studio` | `userdata\projects` |
| API keys | Windows Credential Manager | Windows Credential Manager |

The environment variable `LUMEN_DATA_DIR` overrides both (useful for separate profiles).

## 13. Troubleshooting

* **"No provider configured"**: add one in Settings → Providers, then use **Test connection**.
* **A local server is not found**: check that it is running and started with its API enabled
  (`--api` for SD WebUI, `--listen` when it runs on another machine).
* **A cloud request fails**: the job card shows the server's message; check the key, your balance
  and the provider's content policy.
* **A local server fails to start**: open its log from the status-bar button. The last lines
  usually name the cause: a model file that is not found, CUDA out of memory (lower *GPU layers* or
  the context size), or a port already in use.
* **Logs**: **Help → Open log folder**. Include the latest log when reporting a problem.
