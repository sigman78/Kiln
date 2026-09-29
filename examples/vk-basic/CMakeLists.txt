# examples/vk-basic — kiln-vk-basic: kiln over Vulkan 1.4 without bindless: a descriptor set per
# material, rewritten on kiln's events (docs/design/integration-examples.md). The device, the adapter
# (bindless off) and the frame plumbing come from kiln_example_vk (examples/viewer).

# The committed SPIR-V becomes uint32_t arrays named k_basic_<shader>_spv (the viewer's are k_<shader>_spv).
set(KILN_VK_BASIC_SHADERS mesh.vert mesh.frag sky.vert sky.frag)
set(_headers)
foreach(shader IN LISTS KILN_VK_BASIC_SHADERS)
    set(spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.spv)
    string(REPLACE "." "_" ident basic_${shader})
    set(header ${CMAKE_CURRENT_BINARY_DIR}/shaders/${ident}_spv.h)
    file(READ ${spv} hex HEX)
    string(LENGTH "${hex}" hexlen)
    math(EXPR words "${hexlen} / 8")
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," body "${hex}") # little-endian words
    file(WRITE ${header}
        "// Generated from shaders/${shader}.spv by examples/vk-basic/CMakeLists.txt. Do not edit.\n"
        "#pragma once\n#include <cstdint>\n"
        "inline constexpr uint32_t k_${ident}_spv[${words}] = {${body}};\n")
    list(APPEND _headers ${header})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${spv})
endforeach()

find_program(KILN_GLSLANG NAMES glslang glslangValidator)
if(KILN_GLSLANG)
    set(cmds)
    foreach(shader IN LISTS KILN_VK_BASIC_SHADERS)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
        list(APPEND cmds COMMAND ${KILN_GLSLANG} -V --target-env vulkan1.3 -o ${src}.spv ${src})
    endforeach()
    add_custom_target(vk-basic-shaders ${cmds} COMMENT "Rebuilding examples/vk-basic/shaders/*.spv")
endif()

add_executable(kiln-vk-basic main.cpp ${_headers})
kiln_apply_defaults(kiln-vk-basic)
target_include_directories(kiln-vk-basic PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kiln-vk-basic PRIVATE kiln_example_vk kiln_example_common)
if(TARGET kiln_cook)
    target_link_libraries(kiln-vk-basic PRIVATE kiln_cook)
    target_compile_definitions(kiln-vk-basic PRIVATE KILN_VK_BASIC_HAS_COOK=1)
else()
    target_compile_definitions(kiln-vk-basic PRIVATE KILN_VK_BASIC_HAS_COOK=0)
endif()
