# examples/nga — kiln-nga: kiln over NoGraphicsAPI (github.com/sebbbi/NoGraphicsAPI), GPU pointers
# and descriptor heaps on Vulkan (docs/design/integration-examples.md). Running it needs
# VK_EXT_descriptor_heap and friends (RTX 30+, RDNA 3+); building it does not.
# Fetched at pinned commits: NoGraphicsAPI, Vulkan-Headers and the Vulkan loader (built from source,
# so no SDK is needed), and a Slang release for the shaders.

if(APPLE)
    message(STATUS "kiln-nga: skipped on Apple (the Metal backend needs metallib shaders)")
    return()
endif()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

# Vulkan-Headers: vulkan-sdk-1.4.357.0, as the viewer (the first declaration wins).
FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG e3b1eec08173d6b825cd3ac88c885a63b621504a
    SYSTEM)
# Vulkan-Loader: vulkan-sdk-1.4.357.0. NoGraphicsAPI links the loader, where kiln's other examples use volk.
FetchContent_Declare(VulkanLoader
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Loader.git
    GIT_TAG 5f157b62e333c63260d05d81bf66faa216ab0fb8
    SYSTEM)
# NoGraphicsAPI: main of 2026-09-28. Sources only (SOURCE_SUBDIR points nowhere): its CMake project
# wants the SDK's Vulkan package and exports install targets, so kiln builds its one source file below.
# The example calls its destroy_* without null checks: at this commit they accept null pointers and empty
# heaps. Recheck when the pin moves.
FetchContent_Declare(NoGraphicsAPI
    GIT_REPOSITORY https://github.com/sebbbi/NoGraphicsAPI.git
    GIT_TAG ae017a2f545abc0847e546cc7e84139bf3cc4241
    SOURCE_SUBDIR sources-only)

FetchContent_MakeAvailable(VulkanHeaders)
set(VulkanHeaders_VERSION 1.4.357) # the loader compares it; a fetched Vulkan-Headers does not set it
set(BUILD_TESTS OFF)
set(BUILD_WSI_XCB_SUPPORT OFF CACHE BOOL "" FORCE) # headless on Linux: no X11/Wayland dev packages
set(BUILD_WSI_XLIB_SUPPORT OFF CACHE BOOL "" FORCE)
set(BUILD_WSI_XLIB_XRANDR_SUPPORT OFF CACHE BOOL "" FORCE)
set(BUILD_WSI_WAYLAND_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(VulkanLoader)

FetchContent_MakeAvailable(NoGraphicsAPI)

# NoGraphicsAPI as NoGraphicsAPI's own CMake builds it: C++20, no exceptions, no RTTI; third-party
# code, so compiled quietly (third_party/CMakeLists.txt does the same).
add_library(kiln_nga_lib STATIC ${nographicsapi_SOURCE_DIR}/src/NoGraphicsAPI.cpp)
target_include_directories(kiln_nga_lib SYSTEM PUBLIC ${nographicsapi_SOURCE_DIR}/include)
target_compile_features(kiln_nga_lib PUBLIC cxx_std_20)
target_link_libraries(kiln_nga_lib PUBLIC Vulkan::Headers PRIVATE vulkan)
if(MSVC)
    target_compile_options(kiln_nga_lib PRIVATE /EHs-c- /GR- /W0)
    target_compile_definitions(kiln_nga_lib PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(kiln_nga_lib PRIVATE -fno-exceptions -fno-rtti -w)
endif()

# Slang 2026.18.3 (shader compiler), checked by hash, unpacked into the build tree.
set(_slang_version 2026.18.3)
if(WIN32)
    set(_slang_asset slang-${_slang_version}-windows-x86_64.zip)
    set(_slang_sha256 1339d7b3050ae680c11152b8ccff977173591ce8e9c1d2a5cff2eb904d52e50d)
    set(_slangc_name slangc.exe)
else()
    set(_slang_asset slang-${_slang_version}-linux-x86_64-glibc-2.28.tar.gz)
    set(_slang_sha256 40ee04ec13f8ba26444a53a001ec6926d04040b3fbad7036c9759d3db5756d5c)
    set(_slangc_name slangc)
endif()
set(_slang_dir ${CMAKE_BINARY_DIR}/_deps/slang-${_slang_version})
set(KILN_SLANGC ${_slang_dir}/bin/${_slangc_name})
if(NOT EXISTS ${KILN_SLANGC})
    file(DOWNLOAD https://github.com/shader-slang/slang/releases/download/v${_slang_version}/${_slang_asset}
         ${_slang_dir}/${_slang_asset} EXPECTED_HASH SHA256=${_slang_sha256} STATUS _slang_status)
    list(GET _slang_status 0 _slang_code)
    if(NOT _slang_code EQUAL 0)
        message(FATAL_ERROR "Slang download failed: ${_slang_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${_slang_dir}/${_slang_asset} DESTINATION ${_slang_dir})
    file(REMOVE ${_slang_dir}/${_slang_asset})
endif()
if(NOT EXISTS ${KILN_SLANGC})
    message(FATAL_ERROR "Slang: ${KILN_SLANGC} not found after unpacking ${_slang_asset}")
endif()

# The shaders: Slang to SPIR-V at build time, loaded by the example at run time. The options are the
# ones NoGraphicsAPI's own examples use, except column-major matrices (kiln's math) and no spirv-val.
set(_spv_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(_spv_files)
foreach(entry IN ITEMS "scene.slang meshVertex vertex" "scene.slang meshFragment fragment"
                       "sky.slang skyVertex vertex" "sky.slang skyFragment fragment"
                       "floor.slang floorVertex vertex" "floor.slang floorFragment fragment")
    separate_arguments(entry)
    list(GET entry 0 _src)
    list(GET entry 1 _name)
    list(GET entry 2 _stage)
    add_custom_command(
        OUTPUT ${_spv_dir}/${_name}.spv
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_spv_dir}
        COMMAND ${KILN_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/${_src} -target spirv -profile spirv_1_5
                -emit-spirv-directly -fvk-use-entrypoint-name -entry ${_name} -stage ${_stage}
                -fvk-use-c-layout -matrix-layout-column-major -I ${nographicsapi_SOURCE_DIR}/include
                -capability spvDescriptorHeapEXT -o ${_spv_dir}/${_name}.spv
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_src} ${nographicsapi_SOURCE_DIR}/include/NoGraphicsAPI/shader.slang
        VERBATIM
        COMMENT "slangc ${_src} ${_name}")
    list(APPEND _spv_files ${_spv_dir}/${_name}.spv)
endforeach()

add_executable(kiln-nga main.cpp nga_adapter.h nga_adapter.cpp nga_root.h scene.slang sky.slang ${_spv_files})
set_source_files_properties(scene.slang sky.slang PROPERTIES HEADER_FILE_ONLY TRUE)
kiln_apply_defaults(kiln-nga)
target_link_libraries(kiln-nga PRIVATE kiln_runtime kiln_example_common kiln_example_adapter_support kiln_nga_lib)
if(TARGET kiln_cook)
    target_link_libraries(kiln-nga PRIVATE kiln_cook)
    target_compile_definitions(kiln-nga PRIVATE KILN_NGA_HAS_COOK=1)
else()
    target_compile_definitions(kiln-nga PRIVATE KILN_NGA_HAS_COOK=0)
endif()
# The loader built here, next to the executable, so the pinned one is the one that runs.
if(WIN32)
    add_custom_command(TARGET kiln-nga POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vulkan> $<TARGET_FILE_DIR:kiln-nga>)
endif()

# kiln-nga-array: one texture array assembled from separate tiles (runtime-texture-arrays.md)
add_executable(kiln-nga-array main_array.cpp nga_adapter.h nga_adapter.cpp floor.slang ${_spv_files})
set_source_files_properties(floor.slang PROPERTIES HEADER_FILE_ONLY TRUE)
kiln_apply_defaults(kiln-nga-array)
target_link_libraries(kiln-nga-array PRIVATE kiln_runtime kiln_example_common kiln_example_adapter_support
                                             kiln_nga_lib kiln_cli)
if(TARGET kiln_cook)
    target_link_libraries(kiln-nga-array PRIVATE kiln_cook)
    target_compile_definitions(kiln-nga-array PRIVATE KILN_NGA_HAS_COOK=1)
else()
    target_compile_definitions(kiln-nga-array PRIVATE KILN_NGA_HAS_COOK=0)
endif()
if(WIN32)
    add_custom_command(TARGET kiln-nga-array POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:vulkan> $<TARGET_FILE_DIR:kiln-nga-array>)
endif()
