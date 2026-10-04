# examples/vk — kiln over Vulkan 1.4 (docs/design/viewer.md, integration-examples.md):
#   kiln_example_vk  device bring-up, the example adapter and the frame plumbing, shared by every
#                    Vulkan executable (kiln-viewer and kiln-vk-smoke live in examples/viewer)
#   kiln-vk-basic    one model and a sky without bindless: descriptor sets per material (KILN_EXAMPLE_VK_BASIC)
#   kiln-vk-array    a texture array from separate tiles, with sets or --bindless    (KILN_EXAMPLE_VK_BASIC)
# Example-only dependencies, fetched at pinned commits; nothing here reaches the library.

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

# Vulkan-Headers: vulkan-sdk-1.4.357.0
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG e3b1eec08173d6b825cd3ac88c885a63b621504a
    SYSTEM)
# volk: 1.4.364 (loads the Vulkan entry points at runtime; no loader link)
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 7f46f79751d7e3b3a6df20e38d3e3986585bcdf4
    SYSTEM)
# GLFW comes from examples/common.
FetchContent_MakeAvailable(VulkanHeaders volk)

# ---------------------------------------------------------------------------
# Shaders: shaders/<set>/<name>.spv are committed and embedded as uint32_t arrays named
# k_<prefix><name>_spv (in <build>/shaders/<prefix><name>_spv.h). `vk-shaders` rebuilds every .spv
# from its GLSL when a compiler is on PATH; it is never part of ALL.
# ---------------------------------------------------------------------------
find_program(KILN_GLSLANG NAMES glslang glslangValidator)
find_program(KILN_GLSLC NAMES glslc)
set(KILN_VK_SHADER_COMMANDS)

function(kiln_vk_embed_shaders out set prefix)
    set(headers)
    set(cmds ${KILN_VK_SHADER_COMMANDS})
    foreach(shader IN LISTS ARGN)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${set}/${shader})
        string(REPLACE "." "_" ident ${prefix}${shader})
        set(header ${CMAKE_CURRENT_BINARY_DIR}/shaders/${ident}_spv.h)
        file(READ ${src}.spv hex HEX)
        string(LENGTH "${hex}" hexlen)
        math(EXPR words "${hexlen} / 8")
        string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," body "${hex}") # little-endian words
        file(WRITE ${header}
            "// Generated from shaders/${set}/${shader}.spv by examples/vk/CMakeLists.txt. Do not edit.\n"
            "#pragma once\n#include <cstdint>\n"
            "inline constexpr uint32_t k_${ident}_spv[${words}] = {${body}};\n")
        list(APPEND headers ${header})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src}.spv)
        if(KILN_GLSLANG)
            list(APPEND cmds COMMAND ${KILN_GLSLANG} -V --target-env vulkan1.3 -o ${src}.spv ${src})
        elseif(KILN_GLSLC)
            list(APPEND cmds COMMAND ${KILN_GLSLC} --target-env=vulkan1.3 -o ${src}.spv ${src})
        endif()
    endforeach()
    set(${out} ${headers} PARENT_SCOPE)
    set(KILN_VK_SHADER_COMMANDS ${cmds} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# kiln_example_vk; its default shaders are the viewer's bindless ones.
# ---------------------------------------------------------------------------
kiln_vk_embed_shaders(KILN_VK_BINDLESS_HEADERS bindless "" mesh.vert mesh.frag sky.vert sky.frag)
add_library(kiln_example_vk STATIC
    vk_render.h vk_render.cpp vk_background.h
    vk_device.h vk_device.cpp
    vk_adapter.h vk_adapter.cpp
    ${KILN_VK_BINDLESS_HEADERS})
kiln_apply_defaults(kiln_example_vk)
target_include_directories(kiln_example_vk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kiln_example_vk PUBLIC kiln_runtime kiln_example_common kiln_example_adapter_support
                                             volk)

function(kiln_vk_example name source)
    add_executable(${name} ${source} ${ARGN})
    kiln_apply_defaults(${name})
    target_link_libraries(${name} PRIVATE kiln_example_vk kiln_example_common kiln_cli)
    if(TARGET kiln_cook)
        target_link_libraries(${name} PRIVATE kiln_cook)
        target_compile_definitions(${name} PRIVATE KILN_VK_HAS_COOK=1)
    else()
        target_compile_definitions(${name} PRIVATE KILN_VK_HAS_COOK=0)
    endif()
endfunction()

if(KILN_EXAMPLE_VK_BASIC)
    kiln_vk_embed_shaders(KILN_VK_BASIC_HEADERS basic basic_ mesh.vert mesh.frag sky.vert sky.frag)
    kiln_vk_example(kiln-vk-basic main_basic.cpp ${KILN_VK_BASIC_HEADERS})
    kiln_vk_embed_shaders(KILN_VK_ARRAY_HEADERS array array_ floor.vert floor_sets.frag floor_bindless.frag)
    kiln_vk_example(kiln-vk-array main_array.cpp ${KILN_VK_ARRAY_HEADERS})
endif()

if(KILN_VK_SHADER_COMMANDS)
    add_custom_target(vk-shaders ${KILN_VK_SHADER_COMMANDS}
        COMMENT "Rebuilding examples/vk/shaders/*/*.spv")
endif()
