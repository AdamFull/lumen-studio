# Vulkan for the GPU effects

Lumen runs its effects as compute shaders on Vulkan (plan M20, `src/gpu`). Three projects are kept
here as they are, at the versions nx-gfx (the reference for `lumen_gpu`) uses:

| Folder | Project | Version | Licence |
|---|---|---|---|
| `third_party/vulkan` | [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), the C headers only (`vulkan.h`, `vk_platform.h`, `vulkan_core.h`, `vk_video/`) | v1.4.350 (8864cdc) | Apache-2.0 or MIT (`LICENSE.md`, `LICENSES/`) |
| `third_party/volk` | [volk](https://github.com/zeux/volk), the Vulkan loader | 1.4.350 (3ca312a) | MIT (`LICENSE.md`) |
| `third_party/vma` | [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | v3.4.0 (3aa9212) | MIT (`LICENSE.txt`) |

Lumen does not link Vulkan: volk loads `vulkan-1.dll`, which comes with the graphics driver, when an
effect first needs the GPU, so the program starts on computers without Vulkan.
