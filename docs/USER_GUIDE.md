# Lumen Studio — User Guide

Lumen Studio is an image editor that changes pictures from what you describe. Open a picture,
select an area (or nothing, for the whole picture) and say in your own words what should change:
"colorize it, keep everything else", "remove the lamp", "replace the cup with a candle". Or start a
new picture from a description.

The AI work is done by an *image engine*: a model that runs on your own computer, an online service,
or a server you already run. Lumen helps you set one up the first time it starts.

Character creation, poses and Live2D parts are optional plugins, off until you switch them on. They
are described in [PLUGINS.md](PLUGINS.md).

Contents:
1. [Installing](#1-installing)
2. [The first start: Set up AI](#2-the-first-start-set-up-ai)
3. [The window](#3-the-window)
4. [Selecting an area](#4-selecting-an-area)
5. [Describing a change](#5-describing-a-change)
6. [New images](#6-new-images)
7. [Layers and history](#7-layers-and-history)
8. [Filters](#8-filters)
9. [Opening, saving and exporting](#9-opening-saving-and-exporting)
10. [Project images and projects](#10-project-images-and-projects)
11. [Settings](#11-settings)
12. [Keyboard shortcuts](#12-keyboard-shortcuts)
13. [Troubleshooting](#13-troubleshooting)
14. [Where data is stored](#14-where-data-is-stored)
15. [Command line](#15-command-line)

## 1. Installing

* **Installer** (`LumenStudio-<version>-win64.exe`): installs to *Program Files* and adds Start-menu
  and desktop shortcuts. Settings and data are stored per user (§14).
* **Portable ZIP** (`LumenStudio-<version>-win64-portable.zip`): unzip anywhere (for example a USB
  drive) and run `LumenStudio.exe`. The `portable.txt` file next to the program keeps settings,
  logs, projects, models and filters in the `userdata` folder beside it. Delete `portable.txt` to use
  the per-user folders instead.

Lumen never writes API keys into its own files: they are kept in the Windows Credential Manager, also
in the portable version.

## 2. The first start: Set up AI

To change and make pictures from words, Lumen needs an image engine. At the first start the **Set up
AI** window asks where it should run. It opens again from **Help → Set up AI…** and from every
*Set up AI…* button you see while no engine is set up (the start screen, the prompt bar, the status
bar's "No image engine – Set up AI", New from prompt). **Skip for now** closes it; the editor works
without an engine, only the AI parts wait.

The first page offers three ways, each with what it costs in time, download size and money.

### Run on this computer

Free and private: the model runs on your graphics card and your pictures stay on your computer.

* Lumen looks at your graphics card and suggests one model:
  * **FLUX.2 [klein] 4B** on an NVIDIA or AMD card with about 8 GB of memory or more. It makes new
    images and changes them from a description, with or without a selection.
  * A lighter **SDXL** model (RealVisXL 5.0) on smaller cards. It makes new images and repaints a
    selected area, but follows instructions such as "colorize" less well.
  * Without an NVIDIA or AMD card it suggests an online service, because each image would take
    several minutes. The *Model* list offers the other choice anyway.
* **Lumen first looks for files you already have**: in the model library, Downloads, LM Studio's
  folder, the folders your servers use, and ComfyUI or WebUI when Lumen knows them. A model needs
  more than one file (klein needs its VAE and the Qwen3 4B text encoder); files that are found are
  used where they are, and their folder joins the model library. For a folder Lumen does not know,
  such as a portable ComfyUI or a second drive, **Use models from a folder…** reads it and plans
  again.
* The plan lists each file with its size, where it comes from and its licence, the total download
  (about 11.8 GB for klein, 6.5 GB for RealVisXL, plus the stable-diffusion.cpp program, up to 0.9 GB)
  with how long it takes at 100 Mbit/s, the free space, and where models are saved (**Open models
  folder**).
* **Download and set up** (or **Set up** when nothing is missing) installs the program, downloads
  the files, creates a local server and makes it your image engine. Downloads run in the Queue: you
  can close the window, they resume after an interruption, and each file is checked against its
  checksum. If a server of yours already has the model but with settings that cannot work (for
  example the model in the wrong field), the plan says so and **Set up** corrects it, keeping its
  name and port.
* Then Lumen starts the server and makes a **test image**, with its progress and **Stop**. The first
  start loads the model, which takes a minute or two. After that a klein image takes about 20–40
  seconds on an 8 GB card.

The editor needs no language model. A language model is offered only while a plugin that uses one
is on (see [PLUGINS.md](PLUGINS.md)).

### Use an online service

fal.ai, OpenAI or Google Gemini. Create a key with the service (the link shows where), paste it, and
click **Connect and make a test image**. Lumen checks the key, makes the service your image engine
and makes one test image, which the service charges a few cents for. A key the service refuses
changes nothing: a key you had stays as it was.

### Connect a server that is already running

ComfyUI, Forge / Stable Diffusion WebUI (started with `--api`) or stable-diffusion.cpp's sd-server.
Lumen looks on the usual ports (ComfyUI 8188 and 8000, WebUI 7860, Forge 7861, sd-server 1234 and
7861) and lists what answers; **Use this** makes it your image engine and makes a test image. A
server on another computer is connected by typing its address. ComfyUI gets its first checkpoint;
you can change it later under Settings → Providers.

Every AI task runs as a job in the **Queue** (the jobs button at the right of the status bar, or
**View → Job queue**, `Ctrl+Shift+Q`), where it can be cancelled or retried.

## 3. The window

### Start screen

While no picture is open, the window shows how to begin:

* **Open image…** (`Ctrl+O`), **Paste from clipboard** (the copied picture as a new document),
  **New from prompt…** and **New blank canvas…** (`Ctrl+N`);
* **Recent files**: pictures you opened or saved as, with small previews (the same list as **File →
  Open recent**);
* **Recent in this project**: the project's documents (marked *Layers*: they keep their layers) and
  images; click one to open it, or **Browse project images…** for all of them;
* a **Set up AI** card while no image engine is set up;
* an image file dropped anywhere on the window opens.

### The editor

* **The canvas** in the middle, with a tab per open picture. A tab shows `*` after the name while
  the picture has changes that are not in its file.
* **Tools** on the left (§12 lists them with their keys), with the foreground and background colour
  below. The bar above the canvas shows the chosen tool's options; the line under the canvas shows
  the picture's size, a hint for the tool and the zoom.
* **The right panel** has the tabs **Layers**, **History** and **Filters**. The **AI** tab with every
  AI option opens from the prompt bar's **⋯** menu or the **AI** menu (✕ on the tab closes it); while
  it is open, the other tabs show only their icons when their names do not fit (hover one for its
  name).
* **The prompt bar** under the canvas is where you describe a change (§5). The results of a run
  appear in a strip just above it.
* **The status bar** shows the project, the image engine the prompt bar uses now (click it for
  Settings → AI; without an engine it reads "No image engine – Set up AI"), the local servers (start,
  stop, restart, the server's log), **Safe for work: on** or **off** (click to switch it) and the jobs
  (click for the Queue).

### Menus

* **File**: New blank canvas…, New from prompt… (`Alt+2`), Open image…, Open recent, Paste from
  clipboard, Browse project images… (`Alt+6`); Save, Save as…, Export a copy…, Close document (§9);
  Save to project, Save a copy to project images (§10); New project…, Open project…, Recent
  projects; Quit (`Ctrl+Q`).
* **Edit**: Undo, Redo; **Cut** (`Ctrl+X`) and **Copy** (`Ctrl+C`) of the active layer's selected
  pixels (Copy without a selection takes the whole layer), **Copy merged** (`Ctrl+Shift+C`, every
  visible layer), **Paste as layer** (`Ctrl+V`), Import image as layer…; **Settings…** (`Ctrl+,`).
* **Select**: Select all, Deselect, Invert selection, Delete selected pixels.
* **Image**: **Image size…** (`Ctrl+Alt+I`: the whole picture scaled, its proportions kept unless
  you untick it), **Canvas size…** (`Ctrl+Alt+C`: the canvas cut or extended; choose where the
  picture stays: in the middle, at a side or in a corner), **Rotate 90° clockwise**, **Rotate 90°
  counter-clockwise**, **Rotate 180°**, **Flip horizontally**, **Flip vertically**. Each is one step
  in the history.
* **View**: zoom in and out, Fit to window, Actual pixels (100 %); **Filters** (the Filters tab),
  **Job queue**; the panels of plugins; **Theme** (Automatic, Dark, Light, High contrast, as in
  Settings → General; it changes at the next start).
* **AI**: what is not a description of a change: **Upscale** (2×, 3×, 4×), **Remove background**,
  **Expand canvas…**, **Variation**, and **All AI options…** (§5). Upscale and Remove background
  need an image engine that can do them; with none, they are greyed out and their tooltip says what
  would help (§5).
* **Tools**: **New filter…** and **Filter editor…** (§8), the tools of plugins that are on, and
  **Plugins…**.
* **Help**: **Set up AI…**, **User guide** (`F1`: this guide, built into Lumen), **Keyboard
  shortcuts** (Settings → Shortcuts), **Open models folder**, **Open log folder**, **About Lumen
  Studio**.

The editor's commands in the menus are the same actions as their keys, so a key you change in
Settings → Shortcuts shows there too. Commands that are not about the picture in front (New, Open,
New from prompt, Browse project images, the projects, Settings, the Queue, the plugins' tools, Quit)
also work while another Lumen window is in front, such as the filter editor or Browse project
images. That window's own keys stay its own: in the filter editor `Ctrl+S` saves the filter.

## 4. Selecting an area

A selection says where a change goes. A picture opens with the **Rectangle selection** tool chosen,
because selecting is where changing an area with words starts.

* **Rectangle**, **Ellipse** and **Lasso selection** (`M`, `Shift+M`, `L`): drag on the picture.
  The bar above the canvas chooses what a drag does: **New** replaces the selection, **Add** adds to
  it, **Subtract** takes away, **Intersect** keeps only the overlap. For one drag, Shift adds, Alt
  takes away and Shift+Alt keeps the overlap. **Feather** gives a new selection a soft edge, in
  pixels; a change from the prompt bar fades out over it. A click deselects.
* **Selection brush** (`Q`): paint over what should change, for shapes that are hard to draw.
  Alt or the right mouse button takes paint away. *Tint the selection* in the tool's options shows
  what is painted.
* **Select** menu: **Select all** (`Ctrl+A`), **Deselect** (`Ctrl+D`), **Invert selection**
  (`Ctrl+Shift+I`), **Delete selected pixels** (`Del`).

The selection is used by the prompt bar (§5), the filters (§8) and the transform and delete
commands. Without one, the prompt bar and the filters work on the whole picture.

## 5. Describing a change

Type what you want in the prompt bar and press `Enter` (or click **Generate**; `Ctrl+Enter` works
anywhere in the editor; `Shift+Enter` starts a new line). The line above the words always says what
Generate does now.

### A selected area

"Changes the selected area." Only that area changes. The result is blended in through the selection
with a soft edge and matched to the colours around it, so every pixel outside the selection stays
exactly as it was.

* With an engine that edits from instructions (such as FLUX.2 [klein]), describe the change:
  "colorize it", "replace the cup with a candle". The area and some of its surroundings are edited
  together and blended back.
* With other engines the area is painted anew: describe what should be there ("a red scarf", "an
  empty table").
* **Remove object** removes what is in the selected area and fills it in from its surroundings.

### The whole picture

With nothing selected, "Changes the whole image": "colorize this photo", "make it night". To colorize
a black-and-white photo, open it, make sure nothing is selected, type "colorize this photo" and press
Enter.

The model works on a copy of about one megapixel; the result comes back at your picture's size with
the picture's own fine detail wherever the model kept its shapes, so a colorized photo stays as sharp
as it was.

An engine that cannot change a picture from words says so. Its main button is then **Make a
variation** (the whole picture redrawn, keeping its layout, guided by your words if any; how much
may change is in ⋯), and **Select an area** picks the rectangle selection so you can change a part
instead.

### Choosing a result

A run makes 1 to 4 results (2 at first). While it works, the bar shows its progress and Generate
becomes **Cancel**. The results appear in a strip above the bar, and the canvas shows the current
one over your picture without changing it yet ("Result 1 of 4 · ←/→ to compare").

* `←`/`→` or a click on a thumbnail shows another result.
* **Keep** (`Enter`) adds the result shown as a **new layer** on top, named after your words. A
  result of a selection covers only that area, so the original stays underneath. One **Undo** takes
  it away.
* **Discard** (`Esc`) drops them all; the picture stays as it was.
* Generating again replaces results you did not keep. Each document keeps its own results while you
  switch tabs; closing a document stops its run.

When results arrive while you are typing (or using a slider or list), the keyboard stays where it is;
click a result to choose with the keys. On the canvas the tool comes first: the move tool's arrow
keys still nudge, and the transform and crop tools still use Enter and Esc.

A run that failed or was cancelled can be retried in the Queue; its results then come to the strip
as well.

### More options (⋯)

The **⋯** button next to Generate holds what most people never need, and only what applies:

* **How much may change** (in per cent), when the area is painted anew or for a variation;
* **Results per run** (1–4);
* **Leave out**: things that should not be in the result, for models that take this;
* **LoRAs**: small add-ons to the model for a style, a character or a look (see below);
* **References**: pictures the engine follows for a character, a style or an object (they are added
  in the AI tab);
* the engine: *Engine for changes* on a picture, *Image engine* on a blank canvas. A choice here
  stays until you choose another engine for that use in Settings → AI or Set up AI;
* **Advanced**: *Steps* (the model's choice unless you set them) and *Seed* (random, or fixed to get
  the same result again);
* **All AI options…** opens the AI tab.

**LoRAs.** *Add a LoRA…* lists the LoRAs the engine can load. Those made for the engine's model come
first; the others are greyed out under *Made for another model*, with what they were made for (a LoRA
works only with the model it was trained on, and for FLUX.2 [klein] only with the same size: a 9B
LoRA does nothing on klein 4B). Lumen tells this from the file itself, not from its name. Each chosen
LoRA has a tick box (used while ticked), a strength (1 is full; 0.6 to 1 suits most) and ✕ to remove
it; its trigger words, when the file names them, are added to your description. The choice is kept
for each engine, also in **New from prompt** under *More options*. You do not type `<lora:…>` tags.

For a server Lumen runs, the LoRAs come from the server's **LoRA folder** (Settings → Local servers),
read without starting the server. Without one the list says so and *Server settings…* takes you there.
A server that runs elsewhere is asked for its list; there Lumen cannot tell what each was made for.

### The AI tab and the AI menu

The **AI** tab on the right has the same options as ⋯ (they are shared), the reference images, and
how a selection is blended back: *Context* (how much of the surroundings the model sees), *Blend
edge* and *Match colours to the surroundings*. Its actions, also in the **AI** menu, are what is not a
description of a change:

* **Variation**: the picture (or the selection) imagined again, keeping its layout; the words in the
  prompt bar guide it.
* **Expand canvas**: choose the sides and how far; the engine paints the new area.
* **Upscale** 2×, 3× or 4×: a larger, sharper copy as a new document.
* **Remove background**: the subject cut out onto a transparent layer.

Not every image engine can do the last two. When none of yours can, the AI menu greys them out and
the AI tab says what would help: for upscaling, an upscaler folder for your local server (Settings →
Local servers) or fal.ai or Stability AI; for background removal, fal.ai or Stability AI (Settings →
Providers). Setting up AI again does not help: the local models it offers do neither.

### Edits on your own computer

With an edit model running in stable-diffusion.cpp on your computer (FLUX.2 [klein], FLUX.1 Kontext
or Qwen-Image-Edit), all of this runs locally. Steps and guidance follow the model: FLUX.2 [klein]
uses 4 steps and takes no *Leave out*. Reference images are passed to the model too (up to three
besides the picture for FLUX.2 [klein]).

## 6. New images

### New from prompt

**File → New from prompt…** (`Alt+2`, also on the start screen) makes a new picture from a
description:

1. Describe the image.
2. Choose the size: *Square*, portrait or landscape shapes, or *Custom size* (width × height). The
   engine draws at the closest size it supports, and the image is scaled to your size.
3. Choose how many images to make (1–4) and, if you like, another image engine.
4. Click **Create** (`Ctrl+Enter`).

Lumen makes a document of that size and draws into it. The images are shown one at a time over the
new document, as the prompt bar's results are: `←`/`→` compares them, `Enter` keeps one as a layer.
*More options* has *Leave out* (for models that take it) and the seed. The shape and the number of
images are remembered. Without an image engine the window has a **Set up AI…** button; Set up AI opens
over it and your words are kept.

### A blank canvas

**File → New blank canvas…** (`Ctrl+N`) asks for a name, a size (common sizes or your own) and a
background (white, black, transparent or grey). On a blank canvas the prompt bar says "Creates an
image of … px": describe the picture and press Enter; the result comes in the canvas's shape. Select
an area first to draw only there.

## 7. Layers and history

**Layers** (the first tab on the right): each layer has a visibility eye and a lock; above the list
are the *Blend mode* and *Opacity* of the active layer; below are **New layer**, **Duplicate**,
**Merge down**, **Move up** / **Move down** and **Delete**. Double-click a layer to rename it;
right-click for *Lock transparency* and *Flatten image*. Kept AI results and filters you apply go onto new layers, so
the original stays underneath until you merge.

**History** lists every step of the picture; click one to go back to it. **Undo** (`Ctrl+Z`) and
**Redo** (`Ctrl+Shift+Z` or `Ctrl+Y`) step through it. A kept result or an applied filter is one
step.

The drawing tools: **Brush** and **Eraser** (`B`, `E`; `[` and `]` change the size, pen pressure is
supported, Shift draws a straight line), **Fill** (`G`, with a *Tolerance* and *Sample all layers*),
**Eyedropper** (`I`; Alt+click picks the background colour), **Move layer** (`V`; arrow keys nudge,
Shift = 10 px), **Free transform** (`T`), **Crop / canvas size** (`C`; drag beyond the image to make
the canvas larger) and **Pan** (`H`, or hold Space).

**Free transform**: drag inside the box to move, a handle to scale (Shift keeps the shape, Alt scales
from the centre), outside the box to rotate (Shift snaps to 15°); the options bar has flip and ±90°.
With a selection only the selected pixels move. Enter applies, Esc reverts.

## 8. Filters

### The Filters panel

The **Filters** tab on the right (also **View → Filters**) lists:

* **Built in**: *Black and white*, *Warm tones*, *Cool tones*, *Sharpen*, *Soft focus*, *Film grain*,
  *Old photo*;
* **Built in, with AI**: *Colorize with AI*, *Clean up background*, *Remove background*, *Upscale
  ×2*;
* **Your filters**: the ones you made.

Click a filter to see its settings (sliders and lists). With **Preview on the canvas** on, the
picture shows the filter while you move the sliders, at a lower resolution so it stays quick; nothing
changes yet. **Apply** runs it at full resolution:

* on the active layer, or, for a filter that reads the whole picture (the built-in ones with AI), on
  everything you see;
* with a selection only the selected area changes;
* **Result on**: *A new layer* (the default; the original stays underneath) or *This layer*.

The line above **Apply** says what will change. One **Undo** takes it back. While it works a bar
shows the step that runs, and **Cancel** stops it without changing anything. While the prompt bar's
results wait to be chosen, the canvas shows them instead of the filter, and Apply waits until you
keep or discard them.

### Filters with AI

Filters with AI steps take a while, so their preview runs only when you click **Run AI steps**;
**Apply** then uses what the run made. Each AI step is a job in the Queue. They use the *engine for
changes* of Settings → AI. When no engine can do a step, the panel says so in red before anything
runs, and where to fix it.

* **Colorize with AI** gives a black-and-white photo natural colours and keeps everything else as it
  is. With a selection only that area is coloured. It needs an engine that edits from a description,
  such as FLUX.2 [klein]. Its *Instruction* can be changed ("colour the dress red").
* **Clean up background** tidies the background (clutter, stray objects, spots) and keeps the main
  subject. Select the background to change only that.
* **Remove background** cuts out the main subject; the layers under the cut-out are hidden (still
  there), so the transparency shows. It needs an engine that removes backgrounds: Stable Diffusion
  WebUI with its background removal extension, fal.ai or Stability AI.
* **Upscale ×2** makes the whole picture twice as large with an AI upscaler. A stable-diffusion.cpp
  server needs an upscale model in its settings; ComfyUI, WebUI, fal.ai and Stability AI have one.

### Making your own filters

**New filter…** (in the panel or the **Tools** menu) opens the **filter editor**, a window of its
own. **Edit…** opens one of your filters; a built-in filter stays as it is, and **Edit a copy…**
makes your own copy first. Under **⋯** next to them are **Duplicate**, **Delete** and **Open filters
folder**.

A filter is a chain of steps (nodes) from **Input** to **Output**:

* The **Input** gives the picture and the selection (as a mask). Its *Reads* setting is *Active
  layer* or *Whole picture*.
* **Add node** (`Shift+A`, or right-click an empty place) lists the nodes by kind: colour (levels,
  curves, brightness and contrast, hue and saturation, black and white…), detail (blur, sharpen,
  noise, remove specks), combining (blend, mix by mask), masks (mask from image, grow, shrink and
  feather), transforms (resize, crop, flip, rotate) and AI. Type to search; Enter adds the first
  match.
* Drag from a node's output (right) to another node's input (left) to connect them. The dots are
  coloured by what they carry (orange: a picture, grey: a mask). A link that does not fit turns red
  and the bar at the bottom says why. Drag a link off an input to remove it.
* Select a node to change its settings on the right. The preview under them shows that node's
  result on your picture, and **Problems** says in plain words what keeps the filter from working
  (click a problem to select its node).
* The slider button next to a setting shows it in the Filters panel, with a label and range you
  choose, so the filter can be adjusted there.
* Connect a mask to the Output's *Where to apply* to limit where the result goes.
* `Delete` removes what is selected, the wheel zooms, the middle mouse button (or Space and drag)
  moves around, **Show all** (`Home`) brings every node into view; `Ctrl+Z` and `Ctrl+Shift+Z` undo
  and redo inside the editor.
* **Save** (`Ctrl+S`) and **Save as…** write the filter into the filters folder; the Filters panel
  shows it at once.

**AI steps** in the filter editor: **Edit with AI** (changes the picture as you describe it; connect
a mask such as the Input's selection to its *Area* to change only that area), **Fill area with AI**
(paints an area anew from a description), **Upscale with AI** and **Remove background**. Each has an
*Image engine* setting: *The one in use* (Settings → AI's engine for changes) or one you choose. For
colorizing, set *Colours of an area* to *As the AI made them*. AI steps run on **Run AI steps**, and
what they made is used again when you apply the saved filter to the same layer.

Filters are `.lumenfilter` files in `Documents\Lumen\Filters`; a file put there appears in the list.

## 9. Opening, saving and exporting

Lumen saves pictures the way other image editors do.

* **Open**: **File → Open image…** (`Ctrl+O`), **File → Open recent**, the start screen, or drag files
  onto the window. PNG, JPEG, WebP, BMP, GIF (its first frame), TIFF and Photoshop files (with their
  layers and groups) open; a GIF saves to another format, since Lumen does not write GIF.
  A picture that is open already is shown instead of opened twice.
* **Save** (`Ctrl+S`) writes the picture back to its file, in the file's own format and quality (a
  JPEG stays a JPEG).
* The first time saving would lose something (the layers merged into one, transparency turning
  white, a JPEG's small loss of quality, or what Lumen cannot keep: 16 bits per colour, a colour
  profile, the camera's details such as date and place, Photoshop text layers, effects and
  adjustment layers), Lumen asks first, in plain words: **Save over**, **Save as…** or **Cancel**.
  **Don't ask again for this picture** stops the question for that picture. If another program
  changed the file in the meantime, Save always asks.
* **Save as…** (`Ctrl+Shift+S`) chooses a new file: PNG, JPEG, BMP or a layered Photoshop file
  (PSD), and WebP and TIFF when the dialog lists them. From then on the picture saves to that file
  and its tab takes the file's name. A new picture (from a prompt, a blank canvas, the clipboard) asks
  for a file the first time you save.
* **Export a copy…** (`Ctrl+Shift+E`) writes a copy in any of those formats; the picture keeps saving
  to its own file.
* Saving writes a temporary file first and replaces the old one only when it is complete, so a
  failed save leaves your file as it was.
* A photo in wide colours (Display P3 from a phone, Adobe RGB) is shown and saved in standard
  colours (sRGB); only colours more vivid than sRGB become a little duller.
* **Close document** (`Ctrl+W`, or ✕ on the tab) and **Quit** ask **Save**, **Don't save** or
  **Cancel** for pictures with unsaved changes (one question for all when you quit). **Don't save**
  leaves the file as it was; the changed picture stays in the project, and the start screen lists it
  under *Recent in this project*.

Besides the picture's file, the project keeps every document with its layers (§10), so nothing is
lost when you close Lumen without saving the file.

## 10. Project images and projects

### Projects

Everything you do lives in a project file (`.lumen`): its documents with their layers and its
images. Documents are saved into the project on their own a few seconds after your last change,
regularly (every 60 seconds by default; Settings → General → *Autosave interval*) and when the
project closes. The documents that were open come back when the project opens again, also after a
crash; Lumen then says so, and a crash report is in the log folder.

* **File → Save to project** saves the open documents into the project now.
* **File → New project…**, **Open project…** (`Ctrl+Shift+O`) and **Recent projects** switch
  projects; the last one opens when Lumen starts.
* Opening a file again whose document the project keeps (unchanged on both sides) gives you that
  document back with its layers.

### Browse project images

**File → Browse project images…** (`Alt+6`, also on the start screen) shows every generated, edited,
imported and kept image of the project, in a window of its own.

* Filter by kind or favourites, or search the words. The panel on the right shows the picture, what
  can be done with it, the words it was made from, and under **Details** how it was made (engine and
  model, the kind of change, the size, steps, seed, what was left out).
* **Open in editor** (or a double-click) opens it in the main window.
* **Use as reference** adds it to the reference images in the AI tab; **Reuse prompt** puts its words
  into the prompt bar; **Make another like this** opens New from prompt with its words, size and
  seed.
* Select several and right-click to open them, use them as references, export them, or run **Remove
  background** or **Upscale 2×** on all of them at once. Each result becomes a new image linked to
  its source.
* **Import** adds pictures from disk; **Documents** lists the layered documents of the project.
* **File → Save a copy to project images** adds the picture in front, merged, to this list.

## 11. Settings

**Edit → Settings…** (`Ctrl+,`) lists its pages on the left. Most people only need the first one.

### AI

**Settings → AI** shows what does the work:

* **Image engine**: makes new images (a blank canvas, New from prompt).
* **Engine for changes**: what the editor uses to change a selection or the whole picture (the
  prompt bar, the AI tab and menu, the AI steps of filters). Usually the same as the image engine;
  choose another when it follows instructions such as "colorize" better.
* **Language model**: only while a plugin that needs one is on.

Each row shows the engine, its model and whether it runs or answers. **Change…** picks another engine
you have set up or **Set up a new engine…**; **Set up…** opens Set up AI directly while there is none.
The editor follows the engines chosen here at once. A click on the engine in the status bar opens
this page.

The other pages are for advanced use.

### Local servers

Lumen can install and run stable-diffusion.cpp (images) and llama.cpp (language models for plugins)
for you. **Set up AI** does all of this; this page is for doing it by hand.

* **Set up local AI…** opens Set up AI on *Run on this computer*.
* **Download…** gets a program from GitHub; Lumen picks the build for your graphics card (CUDA for
  NVIDIA, Vulkan for AMD and Intel, CPU otherwise) and checks the files. **Add existing…** uses a
  copy you already have, also ComfyUI (Desktop, portable or a git checkout) and WebUI / Forge, which
  Lumen then starts and stops too. **Check for updates** downloads newer releases; the previous
  version stays installed.
* **New server…** creates a server. Leave **Add it as a provider** on so it can be used as an
  engine.

**The server window for stable-diffusion.cpp** starts with the **Model type**, and shows only the
fields that type needs:

* The types: Stable Diffusion 1.5; SDXL (also Illustrious, NoobAI and Pony); FLUX.1 dev, schnell or
  Kontext as separate files; FLUX.1 as a single file; FLUX.2 [klein] 4B; FLUX.2 [klein] 9B;
  Qwen-Image / Qwen-Image-Edit; Anima; and *Other (every field)* for a model Lumen does not know.
* **Choosing a model file sets everything else.** Lumen reads the file to tell what it is, sets the
  type, puts the model in its place (*Checkpoint* or *Diffusion model*), and fills in the files it
  needs from your models: for FLUX.2 [klein] 4B the *VAE (FLUX.2)* and *Text encoder (Qwen3 4B)*; for
  FLUX.1 the *VAE (FLUX.1)*, *Text encoder (CLIP-L)* and *Text encoder (T5-XXL)*. Files are
  recognised by their contents, whatever they are called.
* Under each file a line says whether it is found, will be downloaded, is missing (with its size) or
  does not fit and why ("This is a T5 text encoder; FLUX.2 [klein] 4B needs a Qwen3 4B text
  encoder"). **Download missing files** fetches what is missing from a pinned Hugging Face
  repository. A server with a file that does not fit cannot be saved.
* To set a file by hand: type or paste its path, drop the file on the field, **Browse…**, or **Choose
  from the model library** (files that fit come first).
* Add-ons the type can use: ControlNet and IP-Adapter for SD 1.5 and SDXL, the LoRA folder and the
  upscaler folder. One that does not fit the model cannot be chosen.
* **Graphics card** applies memory settings for your card (up to 8 GB, 12 GB, 16 GB or more). The
  model's settings (steps, guidance, sampler) come from Lumen's preset for it.
* **Advanced** holds the launch settings (weight precision, keeping weights in RAM, the memory
  budget, flash attention, text encoders on the CPU, CPU threads, extra arguments), when the server
  starts and stops, and the full command line.

A server set to start *When needed* starts on the first request; it stops when Lumen closes or after
the idle time you set. A server whose settings cannot work says why when it starts.

### Models

**Settings → Models** lists the model files Lumen knows: its models folder and folders you add.

* **The models folder** is `Documents\Lumen\Models` (in the portable version `userdata\models`). Its
  sub-folders have ComfyUI's names (`checkpoints`, `diffusion_models`, `vae`, `text_encoders`,
  `loras`, `controlnet`, `ipadapter`, `clip_vision`, `upscale_models`, `embeddings`), so ComfyUI can
  use the same folder; language models go into `llm`. You can put model files there yourself.
  **Open folder** (or **Help → Open models folder**) opens it.
* **Move…** moves the folder, for example to a larger drive, as a job in the Queue. Server settings
  that use the files are updated and running servers restart. If something goes wrong, or you
  cancel, the files go back. Earlier versions kept models in `%LOCALAPPDATA%\Lumen\Lumen Studio\models`;
  Lumen offers once to move them.
* **Folders → Found on this computer** offers your ComfyUI, WebUI and LM Studio folders, Downloads
  and the folders your servers use. Added folders are only read: Lumen never moves or changes files
  there.
* For each file Lumen shows its kind (checkpoint, LoRA, ControlNet…), its family (SDXL, FLUX…), its
  precision and whether it fits your graphics card. **Use with a server** adds it to a server with
  everything it needs. **Look up on Civitai** finds its page and trigger words (it sends the file's
  checksum to civitai.com, so only when you click). Files in pickle formats (.ckpt, .pt, .pth, .bin)
  are marked; prefer .safetensors.
* **Download…** searches Hugging Face and Civitai, starting with image models. Files go into the
  models folder, sorted by kind, and are checked against the site's checksum. Files a model cannot
  work without (a VAE, a text encoder) come along. **Accounts…** stores the access tokens some
  downloads need (in the Credential Manager); Civitai leaves out adult content unless you tick *Adult
  content*. Check a model's licence before you use what you make with it.

**Model presets.** Lumen knows these families and what each needs: FLUX.2 [klein] 4B and 9B (and
their base models), FLUX.1 [dev], [schnell] and Kontext [dev], Qwen-Image and Qwen-Image-Edit, Anima,
SDXL, Illustrious / NoobAI, Pony and Stable Diffusion 1.5. For these, choosing the model is enough:
its companion files, its sampling settings and the memory settings for your card follow. Other models
work by choosing the type whose files they need, or *Other*, and filling in the fields by hand.

### Providers

**Settings → Providers** lists every engine connection, with every setting, for advanced use: local
servers you run yourself (SD WebUI / Forge, ComfyUI, sd-server), online services (fal.ai, OpenAI,
Google Gemini, Stability AI) and the language models some plugins use.

* **Add provider** adds one by hand; **Test connection** checks it; *Parallel requests* limits how
  many jobs run at once.
* **Detect local** (the *Find servers on this computer* window) looks for servers on their usual
  ports and offers to add them.
* **SD WebUI / Forge**: start it with `--api`. Background removal needs its *rembg* extension.
* **ComfyUI**: Lumen builds standard workflows for new images, changes and painting areas anew. Your
  own API-format workflows go into the folder that *Open ComfyUI workflow folder* opens.

### General, Shortcuts and Plugins

* **General**: *Theme* (*Automatic* uses the dark theme, or high contrast when Windows uses it;
  *Dark*, *Light*, *High contrast*) and *Language* (English or German; "System default" follows
  Windows), both after a restart; *Worker threads*, *Autosave interval*, *Use the GPU for the
  canvas*, and **Safe for work**. Every button and field has a name for screen readers, and the
  themes meet the WCAG 2.1 contrast ratios for text.
* **German**: every window is translated, the setup wizard, the prompt bar and the filters included.
  What you describe goes to the model as you wrote it; the instructions inside the built-in AI
  filters (*Colorize with AI*, *Clean up background*) stay in English, which the models follow best.
* **Safe for work** (also the shield in the status bar) adds "nsfw, nude, nudity, explicit, cleavage,
  underwear, lingerie" to what is left out of every image, for models that take this. Some anime
  models draw nudity unasked. It is off by default; online services filter on their own.
* **Shortcuts**: search for an action, click its *Shortcut* or *Alternative* field and press the new
  keys. A key used elsewhere moves to the new action (the page says which one lost it); **Reset to
  default** and **Reset all…** restore the defaults.
* **Plugins**: switches the optional plugins on and off, from the next start (also **Tools →
  Plugins…**). See [PLUGINS.md](PLUGINS.md).

## 12. Keyboard shortcuts

Every shortcut can be changed in Settings → Shortcuts. The defaults:

| Tool | Key |
|---|---|
| Move layer | `V` |
| Free transform | `T` |
| Rectangle / ellipse / lasso selection | `M` / `Shift+M` / `L` |
| Selection brush | `Q` |
| Brush / eraser | `B` / `E` (`[` `]` size) |
| Fill | `G` |
| Eyedropper | `I` |
| Crop / canvas size | `C` |
| Pan | `H`, or hold Space |

| Action | Keys |
|---|---|
| Generate from the prompt bar | `Enter` in its words, `Ctrl+Enter` anywhere in the editor |
| Results: compare / keep / discard | `←` `→` / `Enter` / `Esc` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Shift+Z` or `Ctrl+Y` |
| Select all / deselect / invert | `Ctrl+A` / `Ctrl+D` / `Ctrl+Shift+I` |
| Delete selected pixels | `Del` |
| New layer / duplicate / merge down | `Ctrl+Shift+N` / `Ctrl+J` / `Ctrl+E` |
| Cut / copy / paste as layer / copy merged | `Ctrl+X` / `Ctrl+C` / `Ctrl+V` / `Ctrl+Shift+C` |
| Image size / canvas size | `Ctrl+Alt+I` / `Ctrl+Alt+C` |
| User guide | `F1` |
| Fit / 100 % / zoom in / zoom out | `Ctrl+0` / `Ctrl+1` / `Ctrl+=` / `Ctrl+-` |
| Swap colours | `X` |
| New blank canvas / open image / close document | `Ctrl+N` / `Ctrl+O` / `Ctrl+W` |
| Save / save as / export a copy | `Ctrl+S` / `Ctrl+Shift+S` / `Ctrl+Shift+E` |
| New from prompt / browse project images | `Alt+2` / `Alt+6` |
| Open project / settings / job queue / quit | `Ctrl+Shift+O` / `Ctrl+,` / `Ctrl+Shift+Q` / `Ctrl+Q` |
| Filter editor: add node / show all | `Shift+A` / `Home` |

## 13. Troubleshooting

* **"No image engine"**: there is nothing to do the AI work yet. Click *Set up AI…* (or **Help → Set
  up AI…**) and choose one of the three ways (§2).
* **"This engine cannot change the whole picture from words"**: the engine paints areas but does not
  follow instructions. Select an area and describe what should be there, make a variation, or set up
  an engine with an edit model such as FLUX.2 [klein] and choose it as the *engine for changes* in
  Settings → AI.
* **A model is missing a file** (the server says it needs a VAE or a text encoder, or it will not
  start): open the server in Settings → Local servers. The line under each file says what is missing
  or does not fit; **Download missing files** fetches it, or choose the right file from the model
  library. Choosing the model file again fills in everything Lumen finds.
* **Out of memory** (the server's log mentions memory, or it stops while making an image): in the
  server's settings choose the right **Graphics card** (*Up to 8 GB of memory* for 8 GB cards), keep
  *Weight precision* at q8_0 and *Keep weights in RAM* on (under **Advanced**), close other programs
  that use the graphics card, and make smaller images.
* **The first image is slow**: the first image after a start loads the model, which takes a minute
  or two. The next ones are much faster (about 20–40 seconds for FLUX.2 [klein] on an 8 GB card).
  Without an NVIDIA or AMD graphics card the model runs on the processor and every image takes
  several minutes; an online service is faster then.
* **A server is not found**: check that it runs and was started with its API on (`--api` for WebUI,
  `--listen` when it runs on another computer).
* **An online request fails**: the job in the Queue shows the service's message; check the key,
  your balance and the service's content rules.
* **A local server does not start**: the server button in the status bar opens its log; the last
  lines usually name the cause (a file that is not found, out of memory, a port already in use).
* **Logs**: **Help → Open log folder**. Include the newest log when you report a problem.

## 14. Where data is stored

| What | Installed | Portable |
|---|---|---|
| Settings, providers, logs, character library | `%LOCALAPPDATA%\Lumen\Lumen Studio` | `userdata\` |
| Server programs Lumen downloaded, server settings | `…\Lumen Studio\runtimes`, `servers.json` | `userdata\runtimes`, `userdata\servers.json` |
| Server logs | `…\Lumen Studio\logs\servers` | `userdata\logs\servers` |
| Cache (thumbnails) | `%LOCALAPPDATA%\Lumen\Lumen Studio\cache` | `userdata\cache` |
| Projects | `Documents\Lumen Studio` | `userdata\projects` |
| Models | `Documents\Lumen\Models` (Settings → Models can move it) | `userdata\models` |
| Your filters | `Documents\Lumen\Filters` | `userdata\filters` |
| API keys | Windows Credential Manager | Windows Credential Manager |

The environment variable `LUMEN_DATA_DIR` moves all of it into one folder (useful for separate
profiles); the models and filters folders are then `<that folder>\models` and `<that folder>\filters`.

## 15. Command line

```
LumenStudio [--project file.lumen] [--image picture.png|layers.psd] [--tool transform]
            [--page generate|library|create|poses|parts]
```

`--image` opens a picture, `--tool` chooses an editor tool (for example `transform`, `brush`,
`crop`). `--page generate` opens New from prompt and `--page library` Browse project images;
`create`, `poses` and `parts` open the window of that plugin when it is on. The Live2D parts plugin
adds `--slice` and `--cubism` ([PLUGINS.md](PLUGINS.md)).
