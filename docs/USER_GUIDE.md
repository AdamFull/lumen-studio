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

**The quick way: Set up local AI.** The button on the Local servers page (and in the welcome
dialog) does everything at once:
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
    its own negative prompt, and CFG 4.5 (the Generate page's CFG slider moves there when you
    switch to an Anima model; your own value then stays). With *Safe for work* on, Lumen also
    adds Anima's "safe" rating tag.
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

### Plugins

Create, Poses and Parts, the assistant and character LoRA training are plugins. **Settings →
Plugins** switches each one on or off; the change takes effect the next time Lumen Studio starts.
With a plugin off, its page and buttons are gone, and what it saved in a project is kept until it
is on again. The plugins are:

* **Characters**: the Create page, the assistant, and character LoRA training (including
  **Settings → Cloud training**).
* **Poses**: the Poses page. Without it, the Generate page still takes pose presets, but has no
  *Edit…* button for the pose.
* **Live2D parts**: the Parts page and *Slice into parts* in the Library.

A page keeps its `Alt` number whichever plugins are on (Generate is always `Alt+2`).

**Safe for work.** Some anime models draw nudity or underwear unasked. The shield in the status bar
(also Settings → General) adds "nsfw, nude, nudity, explicit, cleavage, underwear, lingerie" to the
negative prompt of every image request: Generate, Create, the editor, Parts, the assistant and
the LoRA test. It is off by default and works where the model takes a negative prompt; cloud
APIs filter on their own.

### Reusing characters across projects

In the **Character** card of the Create page, **Library → Save character to library…** keeps the
character sheet and its reference images outside the project. **Library → Load character from
library…** brings them into any other project: the references are added to the project and to the
session, and the sheet is set. Saving under an existing name updates that character.

### The project's character

A project has one **active character** from the character library. The character button (round
picture and name, "· LoRA" when it has one) sits above the prompt on the Generate page, in the
editor's AI panel and on the Parts page. Its menu picks another character or none.

* The Create page makes a character active when you save it to the library or load it from there.
* With a character active, the prompt only needs the scene ("reading in a library, evening light").
  Lumen adds the character:
  * its **LoRA** and trigger word, when the image server can load LoRAs of that model family;
  * otherwise its **reference image** (IP-Adapter or a multi-reference model);
  * and its **appearance** (hair, eyes, outfit) as tags or a sentence.
* When the LoRA cannot be used (a cloud API, a model of another family), Lumen says once what it
  uses instead.
* In the editor, the character goes in as far as the action needs it:
  * **Fill selection** and **Expand** use its LoRA (or reference) but not its appearance words, so
    a fixed hand does not get hair painted into it;
  * a **Variation** of the whole image also uses its appearance;
  * **Instruct edit** uses its reference or description (edit models take no LoRA);
  * **Remove** (the selection bar) never uses it.
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
* **Re-run** (Library) brings back the words you wrote and the character. If the image was drawn
  without one, the character is switched off on the Generate page.
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

**Instruction edits on your own computer.** With a model that edits from an instruction running
in stable-diffusion.cpp (FLUX.2 [klein], FLUX.1 Kontext or Qwen-Image-Edit), **Instruct edit** in
the AI tab works locally: "colorize it", "make it night", "replace the cup with a candle".
* With a selection, the area around it is edited as a whole and blended back through the
  selection, so the rest of the image stays exactly as it was.
* Without a selection, the whole image is edited.
* Reference images are passed to the model too (up to three besides the image for FLUX.2 [klein]).
* Steps, CFG and the sampler follow the model: 4 steps at CFG 1 for FLUX.2 [klein], with no negative
  prompt. Values you set on the Generate page still win there.

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
