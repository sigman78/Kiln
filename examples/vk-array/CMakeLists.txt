# examples/vk-array — kiln-vk-array: a texture array assembled at load time from separate tiles
# (docs/design/runtime-texture-arrays.md), drawn through Vulkan 1.4 with a descriptor set of the
# host's own or, with --bindless, through kiln's slot. The device, the adapter and the frame plumbing
# come from kiln_example_vk (examples/viewer).

# The committed SPIR-V becomes uint32_t arrays named k_array_<shader>_spv.
set(KILN_VK_ARRAY_SHADERS floor.vert floor_sets.frag floor_bindless.frag)
set(_headers)
foreach(shader IN LISTS KILN_VK_ARRAY_SHADERS)
    set(spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.spv)
    string(REPLACE "." "_" ident array_${shader})
    set(header ${CMAKE_CURRENT_BINARY_DIR}/shaders/${ident}_spv.h)
    file(READ ${spv} hex HEX)
    string(LENGTH "${hex}" hexlen)
    math(EXPR words "${hexlen} / 8")
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," body "${hex}") # little-endian words
    file(WRITE ${header}
        "// Generated from shaders/${shader}.spv by examples/vk-array/CMakeLists.txt. Do not edit.\n"
        "#pragma once\n#include <cstdint>\n"
        "inline constexpr uint32_t k_${ident}_spv[${words}] = {${body}};\n")
    list(APPEND _headers ${header})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${spv})
endforeach()

find_program(KILN_GLSLANG NAMES glslang glslangValidator)
if(KILN_GLSLANG)
    set(cmds)
    foreach(shader IN LISTS KILN_VK_ARRAY_SHADERS)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
        list(APPEND cmds COMMAND ${KILN_GLSLANG} -V --target-env vulkan1.3 -o ${src}.spv ${src})
    endforeach()
    add_custom_target(vk-array-shaders ${cmds} COMMENT "Rebuilding examples/vk-array/shaders/*.spv")
endif()

add_executable(kiln-vk-array main.cpp ${_headers})
kiln_apply_defaults(kiln-vk-array)
target_include_directories(kiln-vk-array PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kiln-vk-array PRIVATE kiln_example_vk kiln_example_common kiln_cli)
target_compile_definitions(kiln-vk-array PRIVATE
    KILN_EXAMPLE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/examples/assets"
    KILN_EXAMPLE_STORE_DIR="${CMAKE_BINARY_DIR}/example-store")
if(TARGET kiln_cook)
    target_link_libraries(kiln-vk-array PRIVATE kiln_cook)
    target_compile_definitions(kiln-vk-array PRIVATE KILN_VK_ARRAY_HAS_COOK=1)
else()
    target_compile_definitions(kiln-vk-array PRIVATE KILN_VK_ARRAY_HAS_COOK=0)
endif()
