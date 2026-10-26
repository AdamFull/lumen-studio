# Lumen Studio — User Guide

Lumen Studio is an image editor that changes pictures from what you describe: select an area (or
nothing) and say what should change, or start a new picture from a description. The image engine
runs on your own computer (stable-diffusion.cpp, ComfyUI, Stable Diffusion WebUI / Forge) or online
(fal.ai, OpenAI, Google Gemini, Stability AI). Character creation, poses and Live2D parts are
plugins (§3); some of them also use a language model (llama.cpp, Ollama, LM Studio and online ones).

## 1. Installing

* **Installer** (`LumenStudio-<version>-win64.exe`): installs to *Program Files* and adds Start-menu
  and desktop shortcuts. Settings and data are stored per user (see §12).
* **Portable ZIP** (`LumenStudio-<version>-win64-portable.zip`): unzip anywhere (for example a USB
  drive) and run `LumenStudio.exe`. The `portable.txt` file next to the executable keeps settings,
  logs, cache and projects in the `userdata` folder beside it. Delete `portable.txt` to use the
  per-user locations instead.

API keys are never written to disk by Lumen. They are kept in the Windows Credential Manager, also
in portable mode.

## 2. First run: Set up AI

Lumen needs an *image engine* to make and change images from your words. On the first start the
**Set up AI** window asks where it should run. It opens again from **Help → Set up AI…** and from
every *Set up AI…* button while no engine is set up: the start screen's card, the prompt bar, the
status bar's "No image engine – Set up AI" and New from prompt (over its window, which keeps your
words). Each choice says what will happen, how long it takes and how much it downloads:

* **Run on this computer** (free, private). Lumen checks your graphics card and suggests one model:
  *FLUX.2 [klein] 4B* on an NVIDIA or AMD card with 8 GB or more (it makes new images and changes
  them from a description: "colorize it, keep everything else", "remove the lamp"), or the lighter
  *RealVisXL 5.0* (SDXL) below 8 GB. Without an NVIDIA or AMD card it suggests an online service,
  because images would take many minutes. The *Model* box offers the other choice.
  * First it looks for the files on this computer: in the model library, in Downloads, in LM
    Studio's folder, in the folders of the models Lumen's servers use, and in ComfyUI and WebUI when
    Lumen knows them (ComfyUI Desktop, or a ComfyUI or WebUI added under Local servers). When the
    model and the files it needs (for klein: the FLUX.2 VAE and the Qwen3 4B text encoder) are
    there, they are used where they are and nothing is downloaded; their folder joins the model
    library. For a folder it does not know, such as a portable ComfyUI or a second drive, **Use
    models from a folder…** reads it and plans again.
  * A server of Lumen's that already has the model is used as it is when it works. When its
    settings cannot work (for example klein in the checkpoint field without its VAE and text
    encoder), the plan says so and **Set up** corrects them; the server keeps its name and port.
  * Otherwise the list shows what will be downloaded, with sizes and licences: about 11.8 GB for
    klein (the model as a 4.0 GB GGUF file, its VAE and text encoder), 6.5 GB for RealVisXL, plus the
    stable-diffusion.cpp program (up to 0.9 GB for the NVIDIA build) when it is not installed. The
    total says how long that takes at 100 Mbit/s and how much space is free.
  * **Download and set up** (or **Set up**) installs the program, downloads the files (in the
    Queue: they continue when you close the window and resume after an interruption, and are
    checked against their checksums), creates the server and makes it the image engine. Then it
    starts the server and makes a test image, with how far it is and **Stop**; the first start loads
    the model, which takes a minute or two. On an 8 GB card a klein image takes about 20–40 seconds
    after that; without an NVIDIA or AMD card several minutes. Closed while downloading, the setup
    still finishes; **Set up AI** opened again shows its progress.
  * No language model is set up: the editor works without one. While a plugin that needs one is on
    (Characters), a check box adds one (Qwen3-VL); it is off by default on cards with 8 GB, because
    it shares the graphics card. **Model set for Characters (pose control)…** opens the SDXL set
    the Characters plugin uses (pose ControlNet, IP-Adapter, upscaler, language model; see below).
  * **Open models folder** shows where models are saved.
* **Use an online service**: fal.ai, OpenAI or Google Gemini. Paste the key (the link shows where to
  create one); **Connect and make a test image** checks the key, makes the service the image engine
  and makes one image, which the service charges a few cents for. The key is kept in the Windows
  Credential Manager. A key the service refuses changes nothing: a key you had stays.
* **Connect a server that is already running**: ComfyUI, Forge / Stable Diffusion WebUI (started
  with `--api`) or stable-diffusion.cpp's sd-server. Lumen looks on the usual ports (ComfyUI 8188
  and 8000, WebUI 7860, Forge 7861, sd-server 1234 and 7861) and lists what answers (an sd-server
  also answers as WebUI and is listed once); **Use this** makes it the image engine. A server on
  another computer is connected by its address. ComfyUI gets its first checkpoint (change it under
  Providers). Then a test image.

**Settings → AI** (the first tab of **Edit → Settings…**, `Ctrl+,`; also a click on the engine in
the status bar) shows in one list what does the work: the *image engine* (new images: a blank
canvas, New from prompt), the *engine for changes* (what the editor uses to change a picture or a
selection: the prompt bar, the AI tab and menu, the AI steps of filters; usually the image engine,
choose another when it follows instructions such as "colorize" better) and, while a plugin needs
one, the *language model*. Each row shows the engine by the name the status bar uses, its model
(when the name does not say it) and whether it runs or answers; **Change…** picks another engine you
have set up or opens **Set up AI** (**Set up…** opens it directly while there is none). The editor
follows the engines chosen here or in **Set up AI** at once; one you picked in the editor (§6) stays
until you choose that engine here again, or another. The other tabs hold every setting for advanced
use; **Roles** (which language model does which task) shows only while a plugin needs a language
model.

Providers can also be added by hand under **Settings → Providers**, which lists every provider, also
the language models some plugins use (llama.cpp, Ollama, LM Studio and online ones); the editor
needs none. **Detect local** there (the *Find servers on this computer* window) looks for local
servers on their default ports (also llama.cpp 8080, Ollama 11434, LM Studio 1234, KoboldCpp 5001,
the Lumen part server 7870) and offers to add them; its *Use an online service…* and *Set up local
AI…* open Set up AI on those pages.

* **SD WebUI / Forge**: start it with `--api`. Background removal needs the *rembg* extension; pose
  detection and pose control need *ControlNet*.
* **ComfyUI**: Lumen builds standard workflows for text-to-image, image-to-image and inpainting.
  Your own API-format workflows go in the folder opened by *Open ComfyUI workflow folder* in the
  ComfyUI provider's settings (Settings → Providers).
* **Cloud providers**: paste the API key; **Test connection** checks it. Each provider has a
  *Parallel requests* limit that the job queue respects.

Every generation runs as a job in the **Queue**, where it can be cancelled or retried: click the
jobs button at the right of the status bar, or **View → Job queue** (`Ctrl+Shift+Q`).

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

**The quick way: Set up local AI.** The button on the Local servers page opens **Set up AI** on
*Run on this computer* (§2). For the Characters plugin, **Model set for Characters (pose
control)…** there sets up a whole set at once:
* It suggests a model set for your graphics card: *Compact* below 8 GB, *Standard* for 8 GB, *Large*
  for 12 GB and more. Each set has a vision language model (Qwen3-VL) and an SDXL image model
  (Animagine XL 4.0) with pose ControlNet, IP-Adapter and an upscaler.
* The plan lists each program and file with its size and licence. Files you already have are
  reused, even in your ComfyUI folders.
* **Set up** downloads what is missing (about 17 GB for the standard set), then creates the two
  servers as providers. Image generation then uses the local image model.
* Downloads continue in the Queue if you close the window, and an interrupted download resumes.
* **Generate a test image** checks the result.

The steps below do the same by hand.

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

**Models that need more than one file (FLUX, Qwen-Image, Anima).** Lumen knows what these model
families need and fills it in when you choose the model, in the server settings or with *Use with a
server*:
* The model goes in the right field. A FLUX, Qwen-Image or Anima file is a *diffusion model*, not a
  checkpoint, even when it sits in a `checkpoints` folder.
* Its VAE and text encoders come from your model library: for FLUX.2 [klein] 4B the FLUX.2 VAE
  and the Qwen3 4B text encoder; for FLUX.1 the FLUX.1 VAE, CLIP-L and T5-XXL. Files are recognised
  by their contents, whatever they are called.
* What is missing is listed with its size, and **Download missing files** fetches it from a pinned
  Hugging Face repository. The server settings fill in when the download is done.
* A ControlNet or IP-Adapter that does not fit the model (an SDXL ControlNet with FLUX or Anima) is
  named, and the server cannot be saved with it.
* A server that cannot work says why when it is started, instead of "The server exited while
  starting".

Known families: FLUX.2 [klein] 4B and 9B (and their base models), FLUX.1 dev, schnell and Kontext
dev, Qwen-Image and Qwen-Image-Edit, Anima, SDXL (with Illustrious and Pony) and SD 1.5. Other
models work as before, with the fields filled in by hand.

**Language models with llama.cpp.**
* **Models:** a server can offer several models. Use *More models* for further ones; each has a
  name that roles and requests use. Choosing a vision model fills in its *Vision projector* (the
  `mmproj-…gguf` next to it).
* **Memory:** on GPUs with up to 12 GB only one model is loaded at a time. *Free the GPU for image
  generation* unloads it before Lumen generates images locally; the model reloads (a few seconds)
  on its next request.
* **Roles:** Lumen gives roles nobody handles yet to the server's vision model. On one GPU this is
  faster than splitting roles between a vision and a text model, because every switch reloads a
  model.

**ComfyUI and Stable Diffusion WebUI.** **Add existing…** also accepts ComfyUI (Desktop, portable
or a git checkout with its venv: pick `main.py` or the folder) and WebUI / Forge (pick `webui.bat`).
Lumen then starts and stops them like the other servers. ComfyUI Desktop keeps its own models,
extra model paths and custom nodes.

**Models.** Settings → Models lists the model files Lumen knows: its own models folder and the
folders you add. *Folders → Found on this computer* offers your ComfyUI, WebUI and LM Studio
folders, Downloads, and the folders your servers already use. Files stay where they are.

* **What each file is:** Lumen reads each file's header to tell the type (checkpoint, LoRA,
  ControlNet, language model…), family (SDXL, FLUX…) and precision, and whether it fits your GPU.
* **Civitai:** *Look up on Civitai* finds a file's page, base model, trigger words and a preview.
  This sends the file's checksum to civitai.com, so it only happens when you click it.
* **Using a model:** *Use with a server* adds a model to a server (or creates one). The model fields
  of the server settings have a library menu next to *Browse*.
* **Pickle files:** files in pickle formats (.ckpt, .pt, .pth, .bin) are marked; prefer
  .safetensors.

**The models folder.** Models that Lumen downloads, and LoRAs it trains, go into
`Documents\Lumen\Models` (in the portable version `userdata\models`). Its sub-folders have
ComfyUI's names (`checkpoints`, `diffusion_models`, `vae`, `text_encoders`, `loras`, `controlnet`,
`ipadapter`, `clip_vision`, `upscale_models`, `embeddings`), plus `llm` for language models and
their vision projectors, so ComfyUI can use the same folder. You can put model files there yourself.
* **Open folder** at the top of the Models page, or *Help → Open models folder*, opens it.
* **Move…** moves it to another folder, for example on a larger drive. The model files move with
  what Lumen knows about them, as a job in the Queue: on the same drive in a moment, to another
  drive by copying (the originals are deleted once every copy is complete). Running servers stop
  meanwhile and start again afterwards. Server settings and characters' LoRAs that use the files
  are updated, and new downloads go to the new folder. Other files in the folder stay where they
  are, and so do folders you added that lie inside it. A linked folder (a junction or symbolic
  link, for example to ComfyUI's `checkpoints`) moves as a link, which works only on the same
  drive. Lumen does not move into a folder inside the current one, or into one that already has
  files of the same names, and not while models download or a LoRA is trained; downloads and
  trainings do not start while it moves. If something goes wrong, or you cancel, the files go back.
* Folders you add (ComfyUI, WebUI, LM Studio…) are only read: Lumen never moves or changes files
  there.
* **Earlier versions** kept the models in `%LOCALAPPDATA%\Lumen\Lumen Studio\models`. If that
  folder has models, Lumen asks once at start: **Move models** moves them to the new folder (with
  the server settings), **Keep them where they are** keeps using the old folder. You can move it
  later with *Move…*.

**Downloading models.** **Download…** on the Models page searches Hugging Face and Civitai:
* Pick a model, then a file. For language models Lumen suggests the best quantisation that fits
  your GPU and also downloads the vision projector, so the model can see images.
* Files go into Lumen's models folder, sorted by type, and are checked against the hub's checksum.
  Lumen remembers where each file came from, its version, licence and trigger words.
* **Files a model needs:** some Civitai models are not complete checkpoints. Their page also
  offers a VAE or a text encoder, which the model cannot work without. Lumen downloads those
  together with the model, and skips any already in your library. Files the creator marks as
  required always come along; for the others, untick *Also download the files that come with it*.
  * The model's details on the Models page list these files and say if one is missing;
    **Download missing files** fetches them.
  * For a model you got elsewhere (for example in your ComfyUI folder), *Look up on Civitai*
    finds the files of its version too.
  * *Use with a server* and the server's model field fill in the model's VAE and text encoder.
* **Anima models** (anime models built on circlestone-labs' Anima) never contain their VAE and
  text encoder, and Civitai often does not list them. Lumen recognises an Anima model from the
  file itself, whatever its name. It then looks in your library for a Qwen-Image VAE and the
  Qwen3 0.6B text encoder (by what the files are, not their names), or offers them from the
  official Anima repository on Hugging Face (about 1.4 GB, non-commercial licence).
  * The sd.cpp server runs Anima with the usual 8 GB settings; on an RTX 4060 laptop the first
    image takes about a minute and a half, including starting the server.
  * Prompts for Anima use its tag style with "masterpiece, best quality, score_7" in front and
    its own negative prompt, and CFG 4.5. With *Safe for work* on, Lumen also adds Anima's
    "safe" rating tag.
* **Accounts…** stores access tokens in the Windows Credential Manager. Hugging Face needs a token
  for gated models such as FLUX.1 dev (accept the terms on the model's page first); Civitai asks
  for an API key for most downloads. A token is only sent to its own site.
* Civitai results leave out adult content unless you tick *Adult content*.
* Check a model's licence before you use what you make with it; Civitai shows commercial-use
  and credit rules.

**LoRA trainer.** *Download…* also offers **kohya sd-scripts (LoRA trainer)**, which trains
character LoRAs:
* Lumen installs it with its own Python and PyTorch in its data folder: about 4 GB to download
  and 7.5 GB on disk. Your system Python is not touched.
* The build that suits your NVIDIA driver is preselected.
* Every file is checked against pinned checksums.
* *Add existing…* also accepts an sd-scripts folder or a kohya_ss install that has its venv.

**Tuned for.** New servers start with options for the GPU Lumen detected (up to 8 GB, 12 GB, or
16 GB and more). Change *Tuned for* to apply another set.

The server button in the status bar shows what runs and offers start, stop, restart and the
server's log. **Check for updates** downloads newer releases. Servers switch to them at their next
start, and the previous version stays installed, so you can switch back in the server's settings.

## 3. The window

Lumen Studio opens as an image editor: tools on the left, the canvas with a tab per document in
the middle and the **prompt bar** under it (§6), and the **Layers**, **History** and **Filters**
tabs on the right (§5). The **AI** tab with every AI option opens from the prompt bar's ⋯ menu or
the AI menu; while it is open, the other three tabs show only their icons when their names do not
fit (hover one for its name).

**Start screen.** While no document is open, the window shows how to start:

* **Open image…** (`Ctrl+O`), **Paste from clipboard** (the copied picture as a new document),
  **New from prompt…** and **New blank canvas…** (`Ctrl+N`);
* **Recent files**: the pictures you opened from disk or saved as (the same as **File → Open
  recent**), with small previews; a file that is not there any more is left out;
* the project's recent documents (marked *Layers*: they keep their layers) and images, without the
  results of a run that were not kept; click one to open it; **Browse project images…** shows all
  of them;
* **Set up AI** while no image engine is set up yet (§2);
* an image file dropped anywhere on it opens.

**New from prompt** (**File → New from prompt…**, `Alt+2`) makes a new image from a description:
write what you want, pick the shape (square, portrait or landscape ratios, or a custom width ×
height) and how many images to make (1 to 4), and click **Create** (`Ctrl+Enter`). Lumen makes a
document of that size and draws into it with the image engine (Settings → AI's engine for new
images, unless you choose another, here or in the prompt bar's ⋯ on a blank canvas: one choice for
both). The variants are shown over the new
document one at a time, as the prompt bar's results are (§6): ←/→ compares them and Enter keeps
one as a layer. *More options* has the seed and, for models that take one, a negative prompt
("leave out"). Steps, guidance and the sampler follow the model. The shape and the number of
images are kept for the next time. Without an image engine the dialog has a **Set up AI…** button;
Set up AI opens over it, and once an engine is set up the dialog shows it, with your words kept.

**Browse project images** (**File → Browse project images…**, `Alt+6`) opens every image of the
project in a window of its own (§7).

**The status bar** shows the project; the image engine **Generate** uses now, by the name every
engine list uses ("FLUX.2 [klein] 4B on this computer" for a server Lumen runs, else the provider's
name): the engine for changes for a picture, the engine for new images on a blank canvas or the
start screen (the one New from prompt uses), or the one you chose in the editor for that, which its
tooltip says (click it to choose them in Settings → AI; without one it reads "No image engine – Set
up AI", and a click opens Set up AI); the local servers, **Safe for
work** and the jobs (click for the Queue, which says when nothing is running). The Queue and the
panels of plugins lie over the right of the editor, above the prompt bar, so they never hide it.

**Menus.** *File*: new, open, **Open recent** (a file that is gone is greyed; **Clear the list**),
paste, browse; **Save** (`Ctrl+S`), **Save as…** (`Ctrl+Shift+S`), **Export a copy…**
(`Ctrl+Shift+E`) and **Close document** (`Ctrl+W`) for the picture's file (§5); **Save to project**
and **Save a copy to project images**; projects; **Quit** (`Ctrl+Q`).
*Edit*: **Undo** and **Redo**, copy and paste, importing an image as a layer, **Settings…**
(`Ctrl+,`). *Select*: **Select all**, **Deselect**, **Invert selection**, **Delete selected
pixels**. *View*: zoom in and out, fit to the window, actual pixels, **Filters** (the Filters tab,
§5), the Queue, the panels of plugins (the assistant), the theme. *AI*: what is not a description of
a change: **Upscale** (2×, 3×, 4×), **Remove background**, **Expand canvas…**, **Variation**,
**Detect pose** (with the Poses plugin), and **All AI options…** (§6). *Tools*: **New filter…** and
**Filter editor…** (§5), the tools of plugins (Create, Poses, Parts), each in a window of its own,
and **Plugins…**. *Help*: **Set up AI…** (§2), the models and log folders, *About*. The editor's
commands in the menus are the same as their keys: a key changed in Settings → Shortcuts shows there
too.

**Other windows.** The keys of the commands that are not about the picture in the editor (New,
Open, New from prompt, Browse project images, Open project, Settings, the Queue, the assistant, the
tools of plugins, Quit) work the same while one of Lumen's other windows is in front: a plugin's
tool, the filter editor or Browse project images. A window's own keys stay its own: in the filter
editor `Ctrl+S` saves the filter and `Ctrl+Z` undoes its last step.

### Plugins

Character creation (Create), poses, Live2D parts, the assistant and character LoRA training are
plugins. They are off at first: **Tools → Plugins…** (Settings → Plugins) switches each one on or
off, and the change takes effect the next time Lumen Studio starts. With a plugin off, its menu
entries and windows are gone, and what it saved in a project is kept until it is on again. The
plugins are:

* **Characters**: **Tools → Create** (`Alt+1`), **View → Assistant** (`Ctrl+Shift+A`), and
  character LoRA training (including **Settings → Cloud training**).
* **Poses**: **Tools → Poses** (`Alt+4`). **New image with this pose** there opens New from
  prompt with the pose: the image is drawn in it (with an image engine that has a pose ControlNet)
  and the pose stays with the new document.
* **Live2D parts**: **Tools → Parts** (`Alt+5`) and *Slice into parts* in Browse project images.

Each tool opens in a window of its own, which comes back where you left it. Closing it keeps its
work for the session; closing the main window closes it too (and the filter editor's windows).

**Safe for work.** Some anime models draw nudity or underwear unasked. The shield in the status bar
(also Settings → General) adds "nsfw, nude, nudity, explicit, cleavage, underwear, lingerie" to the
negative prompt of every image request: New from prompt, the editor, Create, Parts, the assistant
and the LoRA test. It is off by default and works where the model takes a negative prompt; cloud
APIs filter on their own.

### Reusing characters across projects

In the **Character** card of the Create page, **Library → Save character to library…** keeps the
character sheet and its reference images outside the project. **Library → Load character from
library…** brings them into any other project: the references are added to the project and to the
session, and the sheet is set. Saving under an existing name updates that character.

### The project's character

A project has one **active character** from the character library. The character button (round
picture and name, "· LoRA" when it has one) sits in the editor's AI tab (prompt bar → ⋯ → *All AI
options…*) and on the Parts page. Its menu picks another character or none. New from prompt draws
no character.

* The Create page makes a character active when you save it to the library or load it from there.
* With a character active, the prompt only needs the scene ("reading in a library, evening light").
  Lumen adds the character:
  * its **LoRA** and trigger word, when the image server can load LoRAs of that model family;
  * otherwise its **reference image** (IP-Adapter or a multi-reference model);
  * and its **appearance** (hair, eyes, outfit) as tags or a sentence.
* When the LoRA cannot be used (a cloud API, a model of another family), Lumen says once what it
  uses instead.
* In the editor (the prompt bar and the AI tab), the character goes in as far as the change needs
  it:
  * a selected area painted anew (inpainting) and **Expand** use its LoRA (or reference) but not
    its appearance words, so a fixed hand does not get hair painted into it;
  * a **Variation** of the whole image and a new image on a blank canvas also use its appearance;
  * an instruction edit (with an edit model) uses its reference or description (edit models take
    no LoRA);
  * **Remove** never uses it.
* On the **Parts** page, opening an image drawn for a library character makes that character the
  project's one. Part completion (including LayerDiffuse on ComfyUI) and the expression variants
  then use its LoRA, so hidden parts and closed eyes or a smile keep the character's look. An empty
  *Character description* box uses the character's appearance; text you type there replaces it.
* **Check each variant with the Critic** (Parts page, under *Expression variants*; on by default)
  has the vision model compare the face before and after each expression variant, feature by
  feature. A variant that changes the eye colour or the face, or barely shows the expression, gets
  a warning sign in the part list; hover it for the reason. Choosing or generating another result
  clears it.
* **Use the character on this page** in the menu switches it off for one page; the name is then
  struck through.
* **A second character** (*Add a second character* in the menu) makes a cast of two, shown as
  "Akane & Bo": the first stands on the left, the second on the right. *Swap left and right*
  changes the order, *Remove* takes one out, and picking a character in the list makes it the
  only one again. The cast is stored in the project. Drawing both in one image is being built
  (M16); until then the pages draw the first character.
* The assistant knows the active character (and a cast of two) and can switch it ("use Akane",
  "use Akane and Bo"). Its images, edits and
  region fixes draw the character the same way as the pages do, so "draw her at the beach" is
  enough; it says what it used (LoRA, reference image or description).

### Character LoRA: the same look in every image

A LoRA is a small add-on to an image model (tens of MB) trained on one character. With it, the
face, hair, eyes, outfit and colours stay the same while pose, framing and background change.

* **Attach:** Create page → Character → **LoRA…**. Pick a LoRA from the model library (Settings →
  Models; *Download…* finds LoRAs on Civitai and Hugging Face) or browse to a file.
* **Trigger word:** Lumen fills it in from the file or its Civitai page; it goes first in every
  shot's prompt.
* **Weight:** 0.6–1.0 usually works. Lower it when images look stiff or copy the training
  pictures.
* **Where it works:** stable-diffusion.cpp, ComfyUI and WebUI / Forge use LoRAs; cloud image APIs
  do not.
* **Warnings:** the Character card warns when the LoRA is made for another model family (an SD 1.5
  LoRA does not work with an SDXL model). It also warns when the server cannot find the file. For
  a server Lumen runs, *Use the LoRA's folder* fixes that.
* **Saving:** the LoRA is saved with the character in the character library.

**Building a training set.** To train a LoRA for your own character, Lumen first needs 20–40 good
images of it:
1. Load or create the character on the Create page (references and a character sheet), then
   choose **LoRA → Training set…**. The character is saved to the character library if it is not
   there yet; the training set belongs to it.
2. **Plan training shots** fills the shot plan with 24 varied shots: front, three-quarter, side
   and back views; full body to portrait; several expressions and arm poses; plain backgrounds.
   Generate them on the Create page, with pose control and the character reference. Two variants
   per shot give the best choice.
3. **Add session results**, then **Score with Critic**. A vision model compares each image with
   the references; images below the minimum are rejected. **Find near-duplicates** drops repeats.
4. Look through the rest yourself: double-click keeps or rejects an image. Only images where the
   character looks exactly right should stay; mistakes in the set end up in the LoRA.
5. **Captions** tab → **Write captions**: the vision model tags each kept image. Tags for the
   character's fixed look are left out (hair and eye colour, hairstyle and, with *Outfit is part of
   the character*, the clothes). The LoRA then ties those to the **trigger word**, which goes first.
   Pose, view, expression and background tags stay, so you can still change them in prompts.
   * Turn *Outfit is part of the character* off to keep outfit tags, if you want to change the
     character's clothes later.
   * Click a caption to edit it. The common-tags line shows what the captions say most often;
     *Remove tag from all…* drops a tag that belongs to the character (for example boots it
     always wears).
6. **Train** tab:
   * Choose the base model: the one you generate with, which is preselected. The LoRA works with
     that model and others of its family.
   * The settings for your GPU are preselected.
   * Choose the number of epochs; 10 is a good start.
   * **Start training** shows the number of steps and the estimated time. On an 8 GB GPU 25 images
     take about two hours.
   * While training runs, the local servers are stopped (they need the same GPU) and start again
     afterwards.
   * **Stop** keeps the progress; **Continue the stopped training** carries on later.
   * When the training is done, the LoRA is in the model library and attached to the character,
     with its trigger word. Lumen then deletes what only a stopped training needs (the trainer's
     saved state and the exported images, about 100 MB each); the epoch files stay for testing.
   * **Remove earlier trainings…** deletes the files of older trainings of the character (epochs,
     test images, logs). The LoRAs in the model library and the newest training stay.
   * **Test the LoRA** (under *Test the result*) checks which epoch works best:
     * The newest three epochs draw four new scenes with the LoRA.
     * For comparison the same scenes are drawn without it, with the reference image instead.
     * The Critic checks each image feature by feature: the face, and each item of the character's
       look (hair, eyes, every piece of clothing and accessory). Each is marked as matching, close,
       different or not visible, and the score counts the visible ones. Hover a cell to see the
       checklist. The best epoch is then tried at weights 0.6 and 1.0.
     * The grid shows every image and score; double-click an image to open it.
     * **Use epoch N at weight W** attaches the recommended epoch, or the row you select.
     * This needs a stable-diffusion.cpp server run by Lumen and takes about 45 minutes on an
       8 GB GPU.
7. **Export for training…** writes the kept images at training sizes with their captions, for
   an external trainer.

**Training on a cloud GPU.** Without a fast GPU, or to keep the computer free, the LoRA can be
trained on a GPU rented from RunPod (runpod.io), billed by the minute.
1. Create a RunPod account, add credit and create an API key (runpod.io → Settings → API Keys).
2. Settings → **Cloud training**: paste the key and save it. It is kept in the system's credential
   store, not in Lumen's settings. Choose Secure cloud (reliable) or Community cloud (cheaper),
   and the preferred GPU; GPUs with 24 GB or more are listed with their price per hour.
3. Train tab → **Where** → *Cloud GPU*. Pick the GPU; the summary shows the estimated time and
   cost. A training of 25 images and 10 epochs takes roughly 30–40 minutes including setup.
4. **Start training** asks first, with the price and the estimate. Lumen then rents the GPU,
   installs the trainer there, sends the training set and the base model, trains and downloads
   the LoRA. Base models downloaded from Hugging Face are fetched by the GPU itself; others are
   uploaded from this computer first, which can take a while on a slow connection.
5. The LoRA arrives in the model library and on the character, as with a local training, and can
   be tested the same way.

Lumen shuts the GPU down when the training ends, fails or is stopped. If Lumen was closed or
crashed in between, it lists the GPUs that are still rented at the next start and offers to shut
them down; you can also stop them at runpod.io. A stopped cloud training cannot be continued; start
it again.

### Critic

With **Critic** turned on, a vision model reviews every generated shot. It compares the result with
the references, the character sheet and the planned pose and framing, and scores it out of 10.
When no result of a shot reaches the minimum score, the shot is generated again with the critic's
improved prompt, up to the number of retries you set. Result thumbnails get a green border when
accepted and an amber one otherwise; hover them to read the scores and the issues. The verdict is also
stored with the image (Browse project images). The critic uses the model assigned to *Vision
analysis* (Settings → Roles).

## 4. The assistant

**View → Assistant** (`Ctrl+Shift+A`; the Characters plugin, §3) opens a chat over the right side
of the editor. Describe what you want in your own words, and the assistant acts through the
studio's tools:

* look at the project and its images;
* generate images, with references for character consistency;
* change an image by instruction or repaint a region;
* remove backgrounds, upscale, and open results in the editor;
* run the director: analyse references, plan shots, adjust poses and generate the shots, with
  Critic retries when the Critic is on.

Every step shows as a card with its result; click a thumbnail to open the image in the editor. Results
are new images of the project, and originals are never overwritten. **Stop** interrupts a run;
**+** starts a new chat.

The assistant uses the model assigned to the *Assistant* role (Settings → Roles). Models
with tool calling are used natively (llama.cpp started with `--jinja`, Ollama, LM Studio, OpenAI,
Anthropic, Gemini). Others follow a JSON protocol; for those, turn off *Tool calling* in the
provider settings.

## 5. The editor

Open images with **File → Open image…** (`Ctrl+O`), **File → Open recent**, **File → Paste from
clipboard**, the start screen, or drag them onto the window. PSD files open with their layers. A
picture that is open already is shown rather than opened twice. Closing the last document shows the
start screen again. A document opens with the rectangle selection, the start of changing an area
with words.

**Saving.** A picture opened from a file remembers that file, as in other editors:

* **File → Save** (`Ctrl+S`) writes the picture back to its file, in the file's own format: a JPEG
  stays a JPEG, at the quality it had. Its tab shows `*` after the name while the picture has
  changes that are not in the file; undoing back to what was saved takes the `*` away.
* The first time you save over a picture in a way that changes the file (the layers merged into one,
  transparent parts that become white, the small loss of quality of a JPEG, or what Lumen cannot
  keep of the file: 16 bits per colour, a colour profile, the camera's details of a photo such as
  its date and place, Photoshop layer masks, text layers, layer effects and adjustment layers),
  Lumen asks first, in plain words: **Save over**, **Save as…** or **Cancel**. **Don't ask again for
  this picture** stops the question for it, also next time.
* If another program changed the file since Lumen opened or saved it, **Save** always asks first,
  so its changes are not lost by surprise.
* A photo in colours of its own (Display P3 from a phone, Adobe RGB) is shown and saved in standard
  colours (sRGB): it looks as it did, only colours more vivid than sRGB can show become a little
  duller. A Photoshop file is saved with its layers and their groups.
* A new picture (from a prompt, a blank canvas, the clipboard) has no file yet: **Save** asks for
  one, as **File → Save as…** (`Ctrl+Shift+S`) does. Save as writes PNG, JPEG, WebP and TIFF (where
  your copy of Qt can write them) or a layered Photoshop file (PSD), and from then on the picture
  saves to that file and its tab takes the file's name. A format Lumen cannot write (GIF) goes to
  Save as too.
* **File → Export a copy…** (`Ctrl+Shift+E`) writes a copy in any of those formats ("photo
  copy.png" is suggested); the picture keeps saving to its own file. Choosing the picture's own file
  there or in Save as is the same as **Save**, with its question.
* A picture is written to a temporary file first, which replaces the old one only once it is
  complete: if saving fails, the file stays as it was.
* Closing a picture (**File → Close document**, `Ctrl+W`, or the ✕ on its tab) or quitting while it
  has changes not saved to its file asks **Save**, **Don't save** or **Cancel** (one question for
  all of them when you quit). Pictures without a file close as before.
* The project keeps every document with its layers either way (§9). Opening a file again whose
  document the project keeps, unchanged since it was saved to the file or read from it, gives you
  that document back with its layers; a file that another program changed opens as it is now.
* **File → Save to project** saves the open documents with their layers in the project now (it also
  happens on its own), and **File → Save a copy to project images** adds the merged picture to the
  project's images.

### Tools

| Tool | Key | Notes |
|---|---|---|
| Move layer | `V` | arrow keys nudge (Shift = 10 px) |
| Free transform | `T` | drag inside to move, a handle to scale (Shift keeps the aspect, Alt scales from the centre), outside the box to rotate (Shift snaps to 15°); Flip and ±90° buttons in the options bar. With a selection only the selected pixels move, and the selection follows. Enter applies, Esc reverts the whole transform |
| Rectangle / ellipse / lasso selection | `M` / `Shift+M` / `L` | Shift adds, Alt subtracts, Shift+Alt intersects |
| Selection brush | `Q` | paints over what should change (the selection the prompt bar uses); Alt or the right button takes it out of the selection; *Tint the selection* shows it |
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
| Generate from the prompt bar | `Enter` in its words, `Ctrl+Enter` anywhere in the editor |
| Results: compare / keep / drop | `←` `→` / `Enter` / `Esc` |
| Save / save as / export a copy | `Ctrl+S` / `Ctrl+Shift+S` / `Ctrl+Shift+E` |
| Open image / new blank canvas / close document | `Ctrl+O` / `Ctrl+N` / `Ctrl+W` |

### Filters

The **Filters** tab on the right (also **View → Filters**) lists the built-in filters (*Black and
white*, *Warm tones*, *Cool tones*, *Sharpen*, *Soft focus*, *Film grain*, *Old photo*), the built-in
filters with AI steps (marked with a sparkle) and, under *Your filters*, the ones you made, each with
a small preview.

* The first filter is chosen when the panel opens; click another to see its settings (the line
  between the list and the settings can be dragged). With **Preview on the canvas** on, the picture
  shows the filter while you move its sliders, at a lower resolution so it stays quick. Nothing
  changes yet. While you paint or move something, the canvas shows your picture as it is; the
  preview comes back when you stop. After **Apply** the preview stays off until you change a setting
  or choose another filter (or turn **Preview on the canvas** off and on), so it does not show the
  filter twice. While the prompt bar's results wait to be chosen (§6), the canvas shows them instead
  and the panel says so; **Apply** and **Run AI steps** wait too, since the filter would change a
  layer under the results. The preview comes back when you discard them, or for the picture with the
  result once you keep one.
* **Apply** (always at the bottom of the panel, with the line that says what will change) runs the
  filter at full resolution on the active layer, or on the whole picture (every visible layer, as
  you see it) for a filter that reads it, such as the built-in ones with AI. With a selection only
  the selected area changes, without one the whole layer or picture. **Result on**: *A new layer*
  (the default: the result goes on a new layer above, named after the filter, and the original stays
  underneath; for a filter of the whole picture, above all the layers) or *This layer*. One
  **Undo** takes it back. While it works, a bar shows the step that runs, and **Cancel** stops it
  without changing anything. You can keep working meanwhile: what you paint on the layer outside
  the area the filter changes stays.
* Filters with AI steps show their preview only after **Run AI steps**, because those take a while;
  **Apply** then uses what the run made.

**Filters with AI.** They use the engine for changes (Settings → AI: your image engine unless you
chose another for changes; an upscale or a background removal goes to another engine that can do
it when that one cannot); each AI step is a job in the Queue, where you see how far it is and can
cancel it (or press **Cancel** in the panel). To run a step again, apply the filter again: the Queue
has no **Retry** for it. When no engine can do a step, the panel says so in red before anything
runs, and where to change that: Help → Set up AI… without an engine, Settings → AI's engine for
changes when your model cannot change a picture from a description. If you remove an engine while a
step of it runs, the step stops and says so. The built-in filters with AI read the whole picture, as
the AI menu does, and put their result on a new layer above all the others.

* **Colorize with AI** gives a black-and-white photo natural colours and keeps everything else as
  it is. With a selection only the selected area is coloured: the area around it is sent to the
  model at its working size and blended back, so every pixel outside the selection stays as it
  was. It needs a model that edits from a description, such as FLUX.2 [klein] on your computer.
  Its **Instruction** can be changed in the panel ("colour the dress red").
* **Clean up background** tidies the background (clutter, stray objects, spots) and keeps the main
  subject. Select the background to change only that.
* **Remove background** cuts out the main subject. On a new layer, the layers are hidden under the
  cut-out (still there, unchanged), so the transparency shows. It needs an engine that
  removes backgrounds: Stable Diffusion WebUI with its background removal extension, fal.ai or
  Stability AI.
* **Upscale ×2** makes the whole picture twice as large with an AI upscaler (every layer, in one
  undo step). With a selection, only the selected area gets the upscaler's added detail and the
  rest is only resized. After **Run AI steps** the panel says the size the picture becomes. A
  stable-diffusion.cpp server needs an upscale model in its settings; ComfyUI, Stable Diffusion
  WebUI, fal.ai and Stability AI have one.

### Building your own filters

**New filter…** (in the panel or the **Tools** menu) opens the filter editor, a window of its own;
**Tools → Filter editor…** (with a document open) shows the Filters tab and opens the filter chosen
there. A filter is a chain of nodes from **Input** to **Output** (the result; connect a mask to its
*Where to apply* to limit where it goes). The Input gives the selection as a mask and, as its
**Reads** setting says, the *Active layer* or the *Whole picture* (every visible layer merged); a
filter of the whole picture puts its result above all the layers.

* **Add node** (`Shift+A`, or right-click on an empty place) lists the nodes by kind; type to
  search and press `Enter` to add the first one found.
* Drag from a node's output (on its right) to another node's input (on its left) to connect them.
  The dots are coloured by what they carry (orange: a picture, grey: a mask). A link that does not
  fit turns red and the bar at the bottom says why, for example that a *Mask from image* node
  belongs between a picture and a mask input. Drag a link off an input to remove it.
* Select a node to change its settings on the right: sliders, lists, colours, and a curve for
  *Curves* (drag a point, click to add one, double-click or right-click to remove one). The preview
  under them shows that node's result on your layer, and **Problems** says in plain words what keeps
  the filter from working; click a problem to select its node.
* The slider button next to a setting shows it in the Filters panel, with a label and range you
  choose, so the filter can be adjusted there without opening the editor.
* `Delete` removes what is selected, the wheel zooms, the middle button (or `Space` and drag) moves
  around, **Show all** (`Home`) brings every node into view. **Undo** and **Redo** (`Ctrl+Z`,
  `Ctrl+Shift+Z`) work inside the editor.
* The **AI** nodes: **Edit with AI** (changes the picture as you describe it; connect a mask, such
  as the Input's *Selection*, to its *Area* to change only that area), **Fill area with AI**
  (paints the area anew from a description), **Upscale with AI** and **Remove background** (its
  *Subject* output is the cut-out's outline as a mask). Each has an **Image engine** setting: *The
  one in use* (the engine for changes of Settings → AI; for an upscale or a background removal
  another engine that can, when that one cannot) or one you choose. *The one in use* does not follow
  an engine chosen in the prompt bar's ⋯. For an edit of an area,
  *Colours of an area* matches its colours to the picture around it; choose *As the AI made them*
  for changes of colour on purpose, such as colorizing. AI steps run when you click **Run AI steps** (or
  **Apply** in the panel), and a step that cannot run is outlined and listed under **Problems**.
  What **Run AI steps** made in the editor is used again when you apply the saved filter to the
  same layer from the Filters panel, so the AI step does not run twice.
* **Save** (`Ctrl+S`) and **Save as…** write the filter into the filters folder; the Filters panel
  shows the change at once. A name another of your filters has gets a number ("Mine 2").

**Edit…** opens one of your filters again. Built-in filters stay as they are: **Edit a copy…** makes
your own copy first ("Sharpen copy"), or opens the copy you made before while you have not changed
it. Under **⋯** next to them, **Duplicate** copies a filter, **Delete** removes one of yours, and
**Open filters folder** opens `Documents\Lumen\Filters`. Filters are `.lumenfilter` files: one put
into that folder appears in the list.

## 6. AI editing

Under the canvas is the **prompt bar**: describe in your own words what you want, and press Enter
(or click **Generate**). Its first line says what Generate does now:

* **With a selection** (drawn, or painted with the selection brush): "Changes the selected area". Only that area changes;
  the result is blended in through the selection with a soft edge and matched to the colours
  around it, so every pixel outside it stays exactly as it was.
  * With a model that edits from an instruction, describe the change ("colorize it", "replace the
    cup with a candle"); the area and its surroundings are edited as a whole and blended back.
  * With another model the area is painted anew: describe what should be there ("a red scarf").
  * **Remove object** fills the area from its surroundings, as if the object had never been there.
* **Without a selection**: "Changes the whole image" with an edit model ("colorize this photo",
  "make it night"). The model works on a copy of about one megapixel; the result comes back at the
  picture's size with the picture's own fine detail wherever the model kept its shapes, so a
  colorized photo stays as sharp as it was (where the model changed what is there, its result shows
  as it made it). An engine that cannot edit from words says so: its main button is then **Make a
  variation** (the whole image redrawn, keeping its layout, guided by your words if any; Enter does
  it too; *How much may change* in ⋯), and **Select an area** picks the rectangle selection.
* **On a blank canvas** (a new blank canvas, or a document from New from prompt): "Creates an
  image" of the canvas's size, from your description.
* Without an image engine, it says so, and the main button is **Set up AI…**.

While a run goes, the bar shows its progress and Generate becomes **Cancel** (from the click on, so
a second Enter does not start another run). A run that failed or was cancelled can be retried in
the Queue; its results then come to the strip as well.

**Choosing a result.** A run makes 1 to 4 results (2 at first; ⋯ → *Results per run*). They appear
in a strip above the bar, and the canvas shows the current one over the image without changing it,
as *Keep* would add it (the result of a selection covers only that area; a picture fitted into the
window stays whole while the strip is there). The strip's line says "Result 1 of 4 · ←/→ to
compare"; its tooltip says what Keep, Discard and Generate do with them.
* `←`/`→` or a click on a thumbnail shows another one. The keys work while the strip or the canvas
  has the keyboard: when the results arrive while you type the next words, the keyboard stays in
  the words (and Enter runs them), and so it does on a list, a slider or a number (a filter's
  settings); click a result to choose with the keys. On the canvas the tool
  comes first: the move tool's arrows still nudge, the transform and crop tools' Enter and Esc
  still apply and revert.
* **Enter** or **Keep** adds it as a new layer on top, named after your words. A result of a
  selection covers only that area, so the original stays underneath; one Undo takes it away.
* **Esc** or **Discard** drops them all; the image stays as it was.
* Each document keeps its own results while you switch tabs, and every result is also saved with
  the project's images (§7). Generate again replaces the results that were not kept.

**⋯ (more options)** has what most people never need, only where it applies: *How much may change*
(in per cent, for a painted area or a variation), *Results per run*, what to leave out (a negative
prompt, for models that take one), reference images (they are in the AI tab), the document's
control pose (with a pose ControlNet), the engine, named as in Settings → AI: on a picture the
*Engine for changes* (unless you choose one, Settings → AI's engine for changes), on a blank canvas
the *Image engine* (Settings → AI's, for new images). A choice here is for that use only, shared
with the AI tab (changes) or New from prompt (new images); it stays until you choose the engine
Settings → AI names again, or choose an engine for that use in Settings → AI or Set up AI (the last
choice wins). Under *Advanced* are the steps (the model's choice unless you set them) and the seed
(random, or fixed to repeat a result), then **All AI options…**.

**The AI tab and the AI menu.** *All AI options…* opens the **AI** tab on the right (✕ on the tab
closes it). It has the same options as ⋯, by the same names (they are shared: *How much may change*
for a variation and *Area may change* for a selected area, in per cent, *Results per run*; its
*Engine for changes* is Settings → AI's unless you chose one), the project's character button (with
the Characters plugin), the reference images, and the settings of a selection's blending: *Context*
(how much of the surroundings the model sees), *Blend edge* and colour matching. Its actions, also
in the **AI** menu, are what is not a description of a change: **Variation**, **Expand canvas**
(choose the sides and how far), **Upscale** (2×, 3× or 4×, as a new document), **Remove background**
(a cut-out layer) and, with the Poses plugin, **Detect pose** (the document's control pose).
Variation and Expand use the words in the prompt bar, if any.

**Instruction edits on your own computer.** With a model that edits from an instruction running
in stable-diffusion.cpp (FLUX.2 [klein], FLUX.1 Kontext or Qwen-Image-Edit), the prompt bar edits
locally: "colorize it", "make it night", "replace the cup with a candle".
* With a selection, the area around it is edited as a whole and blended back through the
  selection, so the rest of the image stays exactly as it was.
* Without a selection, the whole image is edited.
* Reference images are passed to the model too (up to three besides the image for FLUX.2 [klein]).
* Steps, CFG and the sampler follow the model: 4 steps at CFG 1 for FLUX.2 [klein], with no negative
  prompt.

## 7. Project images

**File → Browse project images…** (`Alt+6`, also on the start screen) shows every generated,
imported and edited image of the project, in a window of its own.

* Filter by kind (Poses and Parts with their plugins), favourites or text. The inspector shows the
  picture, what can be done with it, the words it was made from, and under **Details** how it was
  made in plain lines (engine and model, the kind of change, the size it was drawn at, steps,
  seed, what was left out; their tooltip has the settings as other tools write them).
* **Open in editor** (or a double-click) opens the image in the main window.
* **Use as reference** adds it to the references in the editor's AI tab; **Reuse prompt** puts its
  words into the prompt bar (with no document open, into New from prompt); **Make another like
  this** opens New from prompt with the image's words, negative prompt, size and seed.
* Right-click selected images to open them in the editor, use them as references, slice them into
  parts (with the Live2D parts plugin), export them, or run **Remove background** / **Upscale 2×**
  on all of them at once. Each result becomes a new project image linked to its source.
* **Documents** lists the layered documents saved in the project.

## 8. Character part slicing (Live2D)

**Tools → Parts** (the Live2D parts plugin, §3) turns a character illustration into a layered PSD
for rigging:

1. **Source**: pick an image (A-pose works best), or open a layered PSD.
2. **Masks**: automatic segmentation (the Lumen part server, a ComfyUI segmentation workflow or
   Gemini; a skeleton-based split without one), then refine the masks
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

Everything lives in a project file (`.lumen`): the project's images and the editor documents.
Modified documents are saved into the project a few seconds after your last change, every 60
seconds, and when the project closes. The documents that were open come back when the project
opens again, also after a crash. In that case Lumen tells you, and a crash report (`crash-*.txt`
with a `.dmp` minidump) is in the log folder. Closed documents are on the start screen and in the
**Documents** view of Browse project images. A document opened from a file remembers that file in
the project, and whether it has changes that are not in it (File → Save, §5).
**File → Open project…** (`Ctrl+Shift+O`) switches projects; the last one opens on start.

## 10. Command line

```
LumenStudio [--project file.lumen] [--page generate|library|create|poses|parts]
            [--image picture.png|layers.psd] [--tool transform] [--slice picture.png]
            [--cubism model.model3.json]
```

`--page generate` opens New from prompt, `--page library` Browse project images, and `create`,
`poses` or `parts` the window of that plugin (when it is on). `--slice` and `--cubism` are the
Live2D parts plugin's; while it is off they only say so.

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
| Server programs downloaded by Lumen, server settings | `…\Lumen Studio\runtimes`, `servers.json` | `userdata\runtimes` |
| Server logs | `…\Lumen Studio\logs\servers` | `userdata\logs\servers` |
| Cache (thumbnails) | `%LOCALAPPDATA%\Lumen\Lumen Studio\cache` | `userdata\cache` |
| Projects | `Documents\Lumen Studio` | `userdata\projects` |
| Models (downloads, trained LoRAs) | `Documents\Lumen\Models` (Settings → Models can move it) | `userdata\models` |
| Filters you saved | `Documents\Lumen\Filters` | `userdata\filters` |
| API keys | Windows Credential Manager | Windows Credential Manager |

The environment variable `LUMEN_DATA_DIR` overrides both (useful for separate profiles); the models
and filters folders are then `<that folder>\models` and `<that folder>\filters`.

## 13. Troubleshooting

* **"No image engine"**: set one up with **Help → Set up AI…** (or the status bar's "No image engine
  – Set up AI"), or add one in Settings → Providers and use **Test connection**.
* **A local server is not found**: check that it is running and started with its API enabled
  (`--api` for SD WebUI, `--listen` when it runs on another machine).
* **A cloud request fails**: the job card shows the server's message; check the key, your balance
  and the provider's content policy.
* **A local server fails to start**: open its log from the status-bar button. The last lines
  usually name the cause: a model file that is not found, CUDA out of memory (lower *GPU layers* or
  the context size), or a port already in use.
* **Logs**: **Help → Open log folder**. Include the latest log when reporting a problem.
