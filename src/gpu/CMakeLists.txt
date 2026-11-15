enable_language(C) # volk
include(LumenSlang)

# Vulkan, loaded at run time (third_party/vulkan/README.md).
add_library(lumen_volk STATIC "${PROJECT_SOURCE_DIR}/third_party/volk/volk.c")
target_include_directories(lumen_volk SYSTEM PUBLIC
    "${PROJECT_SOURCE_DIR}/third_party/volk"
    "${PROJECT_SOURCE_DIR}/third_party/vulkan/include"
    "${PROJECT_SOURCE_DIR}/third_party/vma/include")
target_compile_definitions(lumen_volk PUBLIC VK_NO_PROTOTYPES)

add_library(lumen_gpu STATIC
    Gpu.h Gpu.cpp
    Effects.h Effects.cpp
    Compiler.h Compiler.cpp
    Vulkan.h
    Vma.cpp
)
target_include_directories(lumen_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lumen_gpu PUBLIC lumen_core Qt6::Gui PRIVATE lumen_volk)
lumen_configure_target(lumen_gpu)
# Third-party code: its warnings are not Lumen's.
set_source_files_properties(Vma.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

lumen_add_slang(lumen_gpu shaders/color.slang ENTRIES decode encode)
# Built-in effects.
lumen_add_slang(lumen_gpu effects/develop.slang ENTRIES shrink across down main EFFECT)
lumen_add_slang(lumen_gpu effects/curve.slang ENTRIES main EFFECT)
lumen_add_slang(lumen_gpu effects/mixer.slang ENTRIES main EFFECT)
lumen_add_slang(lumen_gpu effects/grading.slang ENTRIES main EFFECT)
lumen_add_slang(lumen_gpu effects/lut.slang ENTRIES main EFFECT)
lumen_add_slang(lumen_gpu effects/local.slang ENTRIES shrinkFine fineAcross fineDown shrinkCoarse coarseAcross coarseDown findAir main EFFECT)
lumen_add_slang(lumen_gpu effects/detail.slang ENTRIES across down shrinkColour colourAcross colourDown main EFFECT)
lumen_add_slang(lumen_gpu effects/finish.slang ENTRIES main EFFECT)

# The modules your own effects import (plan M20 step 7): written out for slangc when one is compiled.
qt_add_resources(lumen_gpu "lumen_gpu_modules"
    PREFIX "/gpu/modules"
    BASE shaders
    FILES shaders/effect.slang shaders/colorspace.slang shaders/sampling.slang)
