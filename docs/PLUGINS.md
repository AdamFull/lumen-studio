# Lumen Studio — Plugins

Lumen Studio is an image editor first (see the [user guide](USER_GUIDE.md)). Features that only some
people need come as **plugins**: character creation and the assistant, poses, and Live2D parts. They
are built into Lumen but **off until you switch them on**, so the editor stays simple.

Contents:
1. [Turning plugins on and off](#1-turning-plugins-on-and-off)
2. [Characters](#2-characters)
3. [Poses](#3-poses)
4. [Live2D parts](#4-live2d-parts)
5. [Writing a plugin](#5-writing-a-plugin)

## 1. Turning plugins on and off

**Tools → Plugins…** (the same as **Settings → Plugins**) lists each plugin with what it does. Tick
or untick it; the change takes effect the next time Lumen Studio starts (the page says "Turns on when
Lumen Studio starts again").

* A plugin that is on adds its tools to the **Tools** menu, each opening in a window of its own
  (which comes back where you left it), its panels to the **View** menu, and its pages to Settings.
* A plugin that is off leaves no trace in the menus. What it saved in a project is kept unchanged
  until it is on again.
* Closing a plugin's window keeps its work for the session; closing the main window closes it too.
* The main window's keys (New from prompt, the Queue, Settings…) also work while a plugin's window is
  in front.

| Plugin | Adds |
|---|---|
| **Characters** | **Tools → Create** (`Alt+1`), **View → Assistant** (`Ctrl+Shift+A`), character LoRA training, **Settings → Cloud training** |
| **Poses** | **Tools → Poses** (`Alt+4`), **AI → Detect pose** |
| **Live2D parts** | **Tools → Parts** (`Alt+5`), *Slice into parts* in Browse project images, the *Live2D sheet* canvas size |

## 2. Characters

The Characters plugin keeps one character looking the same across many images: an AI director that
plans sets of shots from reference images, an assistant you talk to, a character library, and
character LoRA training.

### A language model

The director, the Critic and the assistant need a **language model** (one that can see images, for
the references). The editor itself needs none. While the Characters plugin is on:

* **Set up AI** offers a check box to set up one on your computer (Qwen3-VL). It is off by default on
  8 GB graphics cards, because it shares the card with the image model and makes images slower.
* **Settings → AI** shows a *Language model* row, and **Settings → Roles** chooses which model does
  which task (planning, the assistant, *Vision analysis* for the Critic and the references).
* Online language models work too (OpenAI, Anthropic, Gemini, OpenRouter), as do llama.cpp, Ollama
  and LM Studio; add them under **Settings → Providers**.

**Model set for Characters (pose control)…** on Set up AI's *Run on this computer* page (also reached
with **Set up local AI…** under Settings → Local servers) sets up a whole set for character work at
once: a vision language model (Qwen3-VL)
and an SDXL image model (Animagine XL 4.0) with a pose ControlNet, an IP-Adapter and an upscaler. It
suggests *Compact* below 8 GB, *Standard* for 8 GB and *Large* for 12 GB and more (about 17 GB to
download for the standard set), reuses files you have, and **Generate a test image** checks the
result.

**One graphics card for both.** With 8–12 GB, a local language model and an SDXL model do not fit
at the same time; whichever loads second runs mostly on the processor and is many times slower. To
let them take turns:

* a llama.cpp server run by Lumen has *Free the GPU for image generation* (on by default), which
  unloads the model before Lumen makes images locally; it reloads in a few seconds when needed;
* start your own llama.cpp with `--sleep-idle-seconds 30`, so it unloads its model when idle;
* turn on **Free VRAM when idle** in the ComfyUI provider's settings.

Lumen also keeps them apart: the Critic reviews only after all shots of a batch are made, and the
assistant hands results back to the language model only after ComfyUI has unloaded its models.

A tested setup on an 8 GB RTX 4060 laptop GPU:

```
llama-server -m Qwen3VL-8B-Instruct-Q4_K_M.gguf --mmproj mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf ^
  --port 8080 -ngl 99 -c 16384 -ctk q8_0 -ctv q8_0 -fa on --jinja --sleep-idle-seconds 30
```

with ComfyUI running an SDXL anime checkpoint (Animagine XL 4.0 or WAI-Illustrious) and the xinsir
OpenPose ControlNet.

**llama.cpp servers run by Lumen** (Settings → Local servers → **New server…**): a server can offer
several models (*More models*, each with a name the roles use); choosing a vision model fills in its
*Vision projector* (the `mmproj-…gguf` next to it). On cards with up to 12 GB only one model is
loaded at a time. Roles nobody handles go to the server's vision model, which on one card is faster
than switching between a vision and a text model.

### Create: the director

**Tools → Create** opens the director in a window of its own.

1. Attach reference images of your character in the chat and describe the images you need. **Analyze
   references** writes a **character sheet** (its look, outfit and colours); **Edit sheet** corrects
   it. The sheet goes with every plan.
2. The director plans a **shot plan**: one card per image, with a pose, framing and background. Set
   what the images are for, how many shots, and how many images per shot. Adjust a pose on its card.
3. **Generate all** makes every shot (with pose control and the character reference when the image
   engine has them), or generate shots one by one.

**Critic.** With *Critic* on, a vision model reviews every result against the references, the
character sheet and the planned pose and framing, and scores it out of 10. When no result of a shot
reaches the minimum score, the shot is made again with the Critic's improved prompt, up to the
number of retries you set. Thumbnails get a green border when accepted and an amber one otherwise;
hover them to read the scores and the issues. The verdict is also stored with the image (Browse
project images).

### The character library and the project's character

In the **Character** card of the Create window, **Library → Save character to library…** keeps the
character sheet and its reference images outside the project; **Library → Load character from
library…** brings them into any other project. Saving under an existing name updates that character.

A project has one **active character**. The Create window makes a character active when you save it
to the library or load it from there. The character button (round picture and name, "· LoRA" when it
has one) is in the editor's AI tab (prompt bar → ⋯ → *All AI options…*) and in the Parts window; its
menu picks another character or none.

* With a character active, the words only need the scene ("reading in a library, evening light").
  Lumen adds the character: its **LoRA** and trigger word when the image server can load LoRAs of
  that model family, otherwise its **reference image** (IP-Adapter or a model that takes
  references), and its **appearance** (hair, eyes, outfit) as words. When the LoRA cannot be used
  (an online service, a model of another family), Lumen says once what it uses instead.
* In the editor, the character goes in as far as the change needs it: an area painted anew and
  **Expand** use its LoRA (or reference) but not its appearance words, so a fixed hand does not get
  hair painted into it; a **Variation** and a new image on a blank canvas also use its appearance;
  an instruction edit uses its reference or description (edit models take no LoRA); **Remove
  object** never uses it. New from prompt draws no character.
* **Use the character on this page** in the menu switches it off for one window; the name is then
  struck through.
* **Add a second character** makes a cast of two, shown as "Akane & Bo": the first stands on the
  left, the second on the right. *Swap left and right* changes the order, *Remove* takes one out, and
  picking a character in the list makes it the only one again.
* **Both in one image.** With a cast of two, a new image on a blank canvas draws both, each keeping
  its own look: first the scene with a pose per person and both described by their side ("on the
  left: red hair…"), without the characters' LoRAs, then each character repainted in its own region
  with its LoRA (or reference). This takes three runs of the image server per result. With the
  Critic on (Create page), each character's crop is checked by the vision model and repainted once
  when it looks like the other one or like neither. The image server must be able to paint an area
  anew.
* **An area of a two-character image.** When you change a selected area, Lumen asks which character
  it shows, with the one whose region it lies in already chosen. Other changes (a variation, an
  instruction edit) use the first character.
* **Make another like this** on a two-character image brings that cast back, opens a blank canvas of
  the image's size and puts its words in the prompt bar; press Enter to draw them again.

### The assistant

**View → Assistant** (`Ctrl+Shift+A`) opens a chat over the right side of the editor, above the
prompt bar. Describe what you want in your own words, and the assistant acts through the studio's
tools: it looks at the project and its images; makes images with references; changes an image by
instruction or repaints an area; removes backgrounds, upscales and opens results in the editor; and
runs the director (analyse references, plan shots, adjust poses, make the shots, with Critic retries
when the Critic is on). It knows the active character and can switch it ("use Akane", "use Akane and
Bo"), so "draw her at the beach" is enough.

Every step shows as a card with its result; click a thumbnail to open the image in the editor.
Results are new images of the project; originals are never overwritten. **Stop** interrupts a run;
**+** starts a new chat.

The assistant uses the model of the *Assistant* role (Settings → Roles). Models with tool calling are
used natively (llama.cpp started with `--jinja`, Ollama, LM Studio, OpenAI, Anthropic, Gemini).
Others follow a JSON protocol; for those, turn off *Tool calling* in the provider's settings.

### Character LoRA: the same look in every image

A LoRA is a small add-on to an image model (tens of MB) trained on one character. With it, the face,
hair, eyes, outfit and colours stay the same while pose, framing and background change.

* **Attach**: Create window → Character → **LoRA → Attach a LoRA…**. Pick one from the model library
  (Settings → Models; *Download…* finds LoRAs on Civitai and Hugging Face) or browse to a file.
* **Trigger word**: filled in from the file or its Civitai page; it goes first in every shot's
  prompt.
* **Weight**: 0.6–1.0 usually works. Lower it when images look stiff or copy the training pictures.
* **Where it works**: stable-diffusion.cpp, ComfyUI and WebUI / Forge use LoRAs; online services do
  not.
* **Warnings**: the Character card warns when the LoRA is made for another model family (an SD 1.5
  LoRA does not work with an SDXL model) and when the server cannot find the file; for a server
  Lumen runs, *Use the LoRA's folder* fixes that.
* The LoRA is saved with the character in the character library.

**Building a training set.** To train a LoRA for your own character, Lumen first needs 20–40 good
images of it:

1. Load or create the character in the Create window, then choose **LoRA → Training set…**. The
   character is saved to the character library if it is not there yet; the training set belongs to
   it.
2. **Plan training shots** fills the shot plan with 24 varied shots (front, three-quarter, side and
   back views; full body to portrait; several expressions and arm poses; plain backgrounds). Generate
   them in the Create window; two variants per shot give the best choice.
3. **Add session results**, then **Score with Critic**: a vision model compares each image with the
   references and rejects those below the minimum. **Find near-duplicates** drops repeats.
4. Look through the rest yourself: double-click keeps or rejects an image. Only images where the
   character looks exactly right should stay; mistakes in the set end up in the LoRA.
5. **Captions** tab → **Write captions**: the vision model tags each kept image, leaving out the
   character's fixed look (hair and eye colour, hairstyle and, with *Outfit is part of the
   character*, the clothes), which the LoRA then ties to the trigger word. Click a caption to edit
   it; *Remove tag from all…* drops a tag that belongs to the character.
6. **Train** tab: choose the base model (the one you generate with is preselected; the LoRA works
   with it and others of its family), the epochs (10 is a good start) and **Start training**, which
   shows the steps and the estimated time (about two hours for 25 images on an 8 GB card). The local
   servers stop meanwhile and start again afterwards. **Stop** keeps the progress; **Continue the
   stopped training** carries on later. When it is done, the LoRA is in the model library and
   attached to the character. **Remove earlier trainings…** deletes the files of older trainings.
7. **Test the LoRA** (under *Test the result*) finds the best epoch: the newest three epochs draw four
   new scenes, the same scenes are drawn without the LoRA for comparison, and the Critic checks each
   image feature by feature. **Use epoch N at weight W** attaches the one recommended. This needs a
   stable-diffusion.cpp server run by Lumen and takes about 45 minutes on an 8 GB card.
8. **Export for training…** writes the kept images at training sizes with their captions, for
   another trainer.

**The LoRA trainer.** Settings → Local servers → **Download…** also offers **kohya sd-scripts (LoRA
trainer)**. Lumen installs it with its own Python and PyTorch in its data folder (about 4 GB to
download, 7.5 GB on disk; your system Python is not touched), preselects the build for your NVIDIA
driver and checks every file against pinned checksums. *Add existing…* also accepts an sd-scripts
folder or a kohya_ss install with its venv.

**Training on a cloud GPU.** Without a fast graphics card, or to keep the computer free, the LoRA can
be trained on a GPU rented from RunPod (runpod.io), billed by the minute:

1. Create a RunPod account, add credit and create an API key (runpod.io → Settings → API Keys).
2. **Settings → Cloud training**: paste the key and save it (it is kept in the Credential Manager).
   Choose *Secure cloud* (reliable) or *Community cloud* (cheaper) and the preferred GPU; GPUs with
   24 GB or more are listed with their price per hour.
3. Train tab → **Where** → *Cloud GPU*. The summary shows the estimated time and cost; 25 images and
   10 epochs take roughly 30–40 minutes including setup.
4. **Start training** asks first, with the price. Lumen rents the GPU, installs the trainer there,
   sends the training set and the base model, trains and downloads the LoRA, which arrives in the
   model library and on the character.

Lumen shuts the GPU down when the training ends, fails or is stopped. If Lumen was closed in between,
it lists the GPUs still rented at the next start and offers to shut them down; you can also stop
them at runpod.io. A stopped cloud training cannot be continued; start it again.

## 3. Poses

**Tools → Poses** opens the pose editor and library in a window of its own.

* Pose one or more people with a 3D rig (sliders for the torso, head, arms and legs, and the camera)
  or joint by joint on the canvas; *Mirror*, *Add person*, *Remove person*.
* **Presets**: click one to apply it to the selected person; **Save as preset** keeps your own.
* **Detect…** finds the pose of the people in an image (also **AI → Detect pose** in the editor,
  which makes it the document's *control pose*). It needs an engine that detects poses, such as
  Stable Diffusion WebUI with the ControlNet extension.
* **Describe pose** asks the director's language model to pose the selected person ("sitting
  cross-legged, waving with the left hand"); it needs a language model (see Characters).
* **Underlay…** shows a reference image under the skeleton.
* **To document** makes the pose the control pose of the picture in the editor. With an engine that
  has a pose ControlNet, the prompt bar's ⋯ then offers *Follow the document's control pose*.
* **New image with this pose** opens New from prompt with the pose: the image is drawn in it (with an
  engine that has a pose ControlNet), and the pose stays with the new document.
* Import and export OpenPose JSON, and export the pose map as a PNG.

## 4. Live2D parts

**Tools → Parts** turns a finished character illustration into named, ordered layers for rigging in
Live2D Cubism, in four steps:

1. **Source**: pick an image (an A-pose works best), optionally remove its background, and choose the
   part layout; or **Open layered PSD…**, where every visible layer becomes a part.
2. **Masks**: segment automatically (see-through through the [Lumen Part
   Server](../tools/partserver/README.md), a ComfyUI segmentation workflow, Gemini, or the built-in
   skeleton split), then fix the masks with the brush, lasso, magic wand (`W`), cut line (`C`),
   skeleton (`K`) and the split and merge commands.
3. **Generate**: complete the hidden areas of every part, the selected parts or one part. Methods:
   inpainting with any image engine, see-through model layers, LayerDiffuse (ComfyUI with the
   ComfyUI-layerdiffuse nodes, with real transparency) or cut-out only. Expression variants (eyes
   closed, mouth shapes) can be made for blinking and lip-sync. **Check each variant with the
   Critic** (on by default; needs a vision language model) compares the face before and after each
   variant and marks one that changes the eye colour or the face, or barely shows the expression.
4. **Preview & export**: animate the result as a 2.5D puppet, then export a layered PSD
   (Cubism-friendly names and order), a PNG folder with `parts.json`, or editor layers.

With a character of the Characters plugin active, part completion and the expression variants use
its LoRA, so hidden parts keep the character's look; an empty *Character description* box uses the
character's appearance. Sessions are saved in the project.

**Cubism model preview.** The *Cubism model* tab previews a model rigged in Cubism Editor
(`.model3.json`). Lumen cannot ship the Live2D Cubism Core: click **Locate Cubism Core…** and pick
`Live2DCubismCore.dll` from your own *Cubism SDK for Native*. The preview plays every motion group
(idle motions resume afterwards), crossfades expressions, applies poses and physics (with optional
wind), follows the mouse, can lip-sync a talking loop, and shows every parameter as a slider.

**Command line** (with the plugin on):

```
LumenStudio --slice character.png [--slice-step 1-4]
LumenStudio --cubism path/to/model.model3.json
```

`--slice` starts a slicing session for the image (`--slice-step` opens a given step); `--cubism`
previews a model. While the plugin is off, both only say so.

## 5. Writing a plugin

Plugins are C++ static libraries built with Lumen (there is no loading of DLLs yet). The interface is
`src/ui/ext/Extension.h`:

* Derive from `ext::Extension` and give it an `id()`, a `name()` and a `description()` (shown in
  Settings → Plugins).
* Contribute what you need:
  * `pages()`: `PageSpec`s, each a **Tools** menu action that opens your widget in a window of its
    own (id, icon, label, tooltip, order, a default key, and a `create` function);
  * `panels()`: `PanelSpec`s, panels over the right of the editor, switched from the **View** menu
    (the assistant is one);
  * `settingsTabs()`: `SettingsTabSpec`s, pages of the Settings dialog (the core's are ordered ai 5,
    providers 10, roles 20, servers 30, models 40, general 60, shortcuts 70, plugins 80);
  * `needsLanguageModel()`: return true when your features need a language model, so Set up AI and
    Settings → AI offer one.
* Register it at start with `ext::Extensions::instance().add(...)`; the built-in plugins do this in
  `src/plugins/BuiltInPlugins.cpp` (`registerBuiltInPlugins`, called by `main` and the UI tests). Add
  your library next to them in `src/plugins/CMakeLists.txt`, linked against `lumen_ui`.
* Which plugins are on is stored as `plugins/<id>/enabled` and read once per session, since the
  menus and panels are built with the main window. Plugins are off by default
  (`Extensions::kEnabledByDefault`).
* **The core never includes a plugin header**: `src/ui` and below know plugins only through
  `Extension.h`, and a configure check in `src/ui/CMakeLists.txt` fails when a core source includes
  `plugins/…`. Ask the core whether a plugin's page exists with `Studio::hasPage(id)`, never by name.
* Project data a plugin writes is kept unchanged while the plugin is off, so turning it off loses
  nothing.
* Test that the core still builds, runs and edits with your plugin off (`tst_canvas pluginsOff`), and
  add your windows to the layout audit (`tests/tst_layout.cpp`), which must fit 1280 × 720.
