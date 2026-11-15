#pragma once

// Vulkan through volk (loaded at run time, so Lumen starts without it) and the Vulkan Memory
// Allocator, for lumen_gpu's own sources only.
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
