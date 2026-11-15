# Slang compute shaders, compiled when Lumen is built (plan M20). Each entry point becomes
# ":/gpu/<file>.<entry>.spv" and its reflection ":/gpu/<file>.<entry>.json" in the target's resources.
#
#   lumen_add_slang(<target> <file.slang> ENTRIES <entry>... [EFFECT] [DEPENDS <file>...])
#
# EFFECT marks the file as an effect: its entries are its passes, in the order given (the order in
# the file), listed in ":/gpu/<file>.passes". Shaders can `import effect;` (src/gpu/shaders).
#
# slangc comes with the Vulkan SDK (https://vulkan.lunarg.com); LUMEN_SLANGC names another one.
find_program(LUMEN_SLANGC slangc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "The Slang compiler for Lumen's GPU effects")
if(NOT LUMEN_SLANGC)
    message(FATAL_ERROR "slangc was not found. Install the Vulkan SDK (https://vulkan.lunarg.com), which sets VULKAN_SDK, "
                        "or set LUMEN_SLANGC to slangc.")
endif()
message(STATUS "Lumen: Slang compiler ${LUMEN_SLANGC}")

function(lumen_add_slang target source)
    cmake_parse_arguments(SLANG "EFFECT" "" "ENTRIES;DEPENDS" ${ARGN})
    set(include_dir "${PROJECT_SOURCE_DIR}/src/gpu/shaders")
    # The modules effects import (effect, colorspace…).
    file(GLOB modules CONFIGURE_DEPENDS "${include_dir}/*.slang")
    get_filename_component(source "${source}" ABSOLUTE)
    get_filename_component(name "${source}" NAME_WE)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/gpu")
    set(outputs)
    foreach(entry IN LISTS SLANG_ENTRIES)
        set(spv "${out_dir}/${name}.${entry}.spv")
        set(json "${out_dir}/${name}.${entry}.json")
        # The flags nx-gfx validates its shaders with; warning 39001 (two parameters on one binding) is
        # how one file serves several entries.
        add_custom_command(OUTPUT "${spv}" "${json}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
            COMMAND "${LUMEN_SLANGC}" "${source}" -I "${include_dir}" -target spirv -profile spirv_1_3
                    -emit-spirv-directly -fvk-use-entrypoint-name -entry ${entry} -stage compute -warnings-disable 39001
                    -o "${spv}" -reflection-json "${json}"
            DEPENDS "${source}" ${modules} ${SLANG_DEPENDS}
            COMMENT "Slang ${name}.${entry}"
            VERBATIM)
        list(APPEND outputs "${spv}" "${json}")
    endforeach()
    if(SLANG_EFFECT)
        set(passes "${out_dir}/${name}.passes")
        list(JOIN SLANG_ENTRIES "\n" lines)
        file(CONFIGURE OUTPUT "${passes}" CONTENT "${lines}\n")
        list(APPEND outputs "${passes}")
    endif()
    set_source_files_properties(${outputs} PROPERTIES GENERATED TRUE)
    qt_add_resources(${target} "${target}_${name}_slang"
        PREFIX "/gpu"
        BASE "${out_dir}"
        FILES ${outputs})
endfunction()
