# Third-party notices

Lumen Studio uses the software below. Each part keeps its own licence.

## Shipped with Lumen Studio

| Component | Licence | Where |
|---|---|---|
| [Qt 6](https://www.qt.io) (Core, Gui, Widgets, Network, Sql, Svg, Concurrent, OpenGL, OpenGLWidgets, and its plugins) | [LGPL-3.0](https://www.gnu.org/licenses/lgpl-3.0.html) | `Qt6*.dll` and the plugin folders; linked dynamically, so these files can be replaced with your own build of the same Qt version. The source is at <https://download.qt.io/official_releases/qt/> |
| GCC runtime (MinGW-w64: `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) | GPL-3.0 with the GCC Runtime Library Exception; winpthreads: MIT and BSD | next to the program |
| Mesa llvmpipe software OpenGL (`opengl32sw.dll`, from Qt's deployment) | MIT | next to the program |
| Microsoft DirectX Shader Compiler and D3D compiler (`dxcompiler.dll`, `dxil.dll`, `D3Dcompiler_47.dll`, from Qt's deployment) | Microsoft redistributable components; DXC: University of Illinois / NCSA | next to the program |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) C API headers (compiled in; the runtime itself is downloaded, below) | MIT | `third_party/onnxruntime` in the source |

## Downloaded only when you ask for them

Lumen Studio downloads these from their publishers, into its data or models folder, when you choose a
feature that needs them. They are not part of the installer.

| Component | Licence | Used for |
|---|---|---|
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | MIT | Smart selection, objects by name |
| [SAM 2.1 tiny](https://github.com/facebookresearch/sam2) (ONNX) | Apache-2.0 | Smart selection, objects by name |
| [Grounding DINO tiny](https://github.com/IDEA-Research/GroundingDINO) (ONNX) | Apache-2.0 | Objects by name |
| [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) | MIT | Running image models on this computer |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | MIT | Running language models on this computer |
| [ComfyUI](https://github.com/comfyanonymous/ComfyUI) | GPL-3.0 | An image server, when you install it from Lumen |
| [Stable Diffusion WebUI](https://github.com/AUTOMATIC1111/stable-diffusion-webui) | AGPL-3.0 | An image server, when you install it from Lumen |
| [kohya sd-scripts](https://github.com/kohya-ss/sd-scripts) and [uv](https://github.com/astral-sh/uv) | Apache-2.0; MIT or Apache-2.0 | LoRA training (Characters plugin) |

Image and language **models** have licences of their own, some of them non-commercial. The model hub
and the setup show a model's licence where its publisher states one; what you make with a model is
subject to that licence.

Online services (fal.ai, OpenAI, Google Gemini, Stability AI, RunPod and others) are used under your
own account and their terms.

## Not included

Live2D Cubism Core is proprietary and is not shipped. The Live2D parts plugin uses a copy you provide
yourself, under Live2D's licence.
