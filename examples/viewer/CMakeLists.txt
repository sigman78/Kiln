# examples/viewer — the example Vulkan 1.4 adapter and viewer (docs/design/viewer.md).
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
# GLFW 3.5.1: window, input and the Vulkan surface. X11 only on Linux (fewer build deps).
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 70a9bb3881fe80fd483236e2b203cb451c6ecf40
    SYSTEM)
FetchContent_MakeAvailable(VulkanHeaders volk glfw)

# ---------------------------------------------------------------------------
# Shaders: the committed .spv are embedded as uint32_t arrays. `viewer-shaders`
# rebuilds them from the GLSL when a compiler is on PATH; it is never part of ALL.
# ---------------------------------------------------------------------------
set(KILN_VIEWER_SHADERS mesh.vert mesh.frag)
set(KILN_VIEWER_SHADER_HEADERS)
foreach(shader IN LISTS KILN_VIEWER_SHADERS)
    set(spv ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.spv)
    string(REPLACE "." "_" ident ${shader})
    set(header ${CMAKE_CURRENT_BINARY_DIR}/shaders/${ident}_spv.h)
    file(READ ${spv} hex HEX)
    string(LENGTH "${hex}" hexlen)
    math(EXPR words "${hexlen} / 8")
    # little-endian bytes -> uint32 words
    string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1," body "${hex}")
    file(WRITE ${header}
        "// Generated from shaders/${shader}.spv by examples/viewer/CMakeLists.txt. Do not edit.\n"
        "#pragma once\n#include <cstdint>\n"
        "inline constexpr uint32_t k_${ident}_spv[${words}] = {${body}};\n")
    list(APPEND KILN_VIEWER_SHADER_HEADERS ${header})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${spv})
endforeach()

find_program(KILN_GLSLANG NAMES glslang glslangValidator)
find_program(KILN_GLSLC NAMES glslc)
if(KILN_GLSLANG OR KILN_GLSLC)
    set(cmds)
    foreach(shader IN LISTS KILN_VIEWER_SHADERS)
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
        if(KILN_GLSLANG)
            list(APPEND cmds COMMAND ${KILN_GLSLANG} -V --target-env vulkan1.3 -o ${src}.spv ${src})
        else()
            list(APPEND cmds COMMAND ${KILN_GLSLC} --target-env=vulkan1.3 -o ${src}.spv ${src})
        endif()
    endforeach()
    add_custom_target(viewer-shaders ${cmds} COMMENT "Rebuilding examples/viewer/shaders/*.spv")
endif()

# ---------------------------------------------------------------------------
# kiln-viewer and the adapter smoke test
# ---------------------------------------------------------------------------
add_executable(kiln-viewer
    main.cpp
    vk_device.h vk_device.cpp
    vk_adapter.h vk_adapter.cpp
    ${KILN_VIEWER_SHADER_HEADERS})
kiln_apply_defaults(kiln-viewer)
target_include_directories(kiln-viewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(kiln-viewer PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(kiln-viewer PRIVATE kiln_runtime kiln_cli volk glfw)
if(TARGET kiln_cook)
    target_link_libraries(kiln-viewer PRIVATE kiln_cook)
    target_compile_definitions(kiln-viewer PRIVATE KILN_VIEWER_HAS_COOK=1)
else()
    target_compile_definitions(kiln-viewer PRIVATE KILN_VIEWER_HAS_COOK=0)
endif()

# Loads the golden store through the real adapter with no window; runs only by hand
# (needs a Vulkan driver), so it is not a CTest.
add_executable(kiln-vk-smoke adapter_smoke.cpp vk_device.cpp vk_adapter.cpp)
kiln_apply_defaults(kiln-vk-smoke)
target_link_libraries(kiln-vk-smoke PRIVATE kiln_runtime kiln_cli volk)
