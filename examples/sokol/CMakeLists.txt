# examples/sokol — kiln-sokol: kiln with sokol_gfx and sokol_app (docs/design/integration-examples.md).
# sokol (headers) and sokol-shdc (a prebuilt shader compiler) are fetched at pinned commits;
# scene.glsl and array.glsl are compiled into headers at build time. Backends: D3D11 on Windows, Metal on
# macOS, GL 4.3 core elsewhere.

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

# sokol: master of 2026-09-15. SOURCE_SUBDIR points nowhere: only the headers are used.
FetchContent_Declare(sokol
    GIT_REPOSITORY https://github.com/floooh/sokol.git
    GIT_TAG 2e75443dbd4940b5aa8d76a8e479f8e4b270b9a3
    SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(sokol)

# sokol-shdc from sokol-tools-bin, 2026-08-29, one binary per host, checked by hash.
set(_shdc_commit 11d0cf678105d614d675e6d9bd2aaf3eeff12f8c)
if(CMAKE_HOST_WIN32)
    set(_shdc_path win32/sokol-shdc.exe)
    set(_shdc_sha256 bd616287f9ea689d53c6d260e443ee733e61ae1b73a9b37adc482ead0364d561)
elseif(CMAKE_HOST_APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64")
    set(_shdc_path osx_arm64/sokol-shdc)
    set(_shdc_sha256 92db37975ad7ff3c3c9bc27cba1503287377cb287ebabf60d1c6b597abfa3244)
elseif(CMAKE_HOST_APPLE)
    set(_shdc_path osx/sokol-shdc)
    set(_shdc_sha256 8b4a6ac1172ec0d90dd41d611067d5e87a51e78dc28acb216cfc341d880b1d78)
else()
    set(_shdc_path linux/sokol-shdc)
    set(_shdc_sha256 ed35e89ef381d521a499096ed4ada85e4d135d8011e151cca6b7d893c43b21df)
endif()
get_filename_component(_shdc_name ${_shdc_path} NAME)
set(KILN_SOKOL_SHDC ${CMAKE_BINARY_DIR}/_deps/sokol-shdc/${_shdc_name})
if(NOT EXISTS ${KILN_SOKOL_SHDC})
    file(DOWNLOAD https://raw.githubusercontent.com/floooh/sokol-tools-bin/${_shdc_commit}/bin/${_shdc_path}
         ${KILN_SOKOL_SHDC} EXPECTED_HASH SHA256=${_shdc_sha256} STATUS _shdc_status)
    list(GET _shdc_status 0 _shdc_code)
    if(NOT _shdc_code EQUAL 0)
        message(FATAL_ERROR "sokol-shdc download failed: ${_shdc_status}")
    endif()
    file(CHMOD ${KILN_SOKOL_SHDC} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
endif()

set(_shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${_shader_dir})
foreach(_shader scene array)
    add_custom_command(
        OUTPUT ${_shader_dir}/${_shader}.glsl.h
        COMMAND ${KILN_SOKOL_SHDC} --input ${CMAKE_CURRENT_SOURCE_DIR}/${_shader}.glsl
                --output ${_shader_dir}/${_shader}.glsl.h --slang glsl430:hlsl5:metal_macos
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_shader}.glsl
        COMMENT "sokol-shdc ${_shader}.glsl")
endforeach()

# The sokol implementation: third-party C, compiled quietly (third_party/CMakeLists.txt does the same).
add_library(kiln_sokol_impl STATIC sokol_impl.c)
target_include_directories(kiln_sokol_impl SYSTEM PUBLIC ${sokol_SOURCE_DIR} ${sokol_SOURCE_DIR}/util)
target_compile_definitions(kiln_sokol_impl PUBLIC SOKOL_NO_ENTRY)
if(WIN32)
    target_compile_definitions(kiln_sokol_impl PUBLIC SOKOL_D3D11)
elseif(APPLE)
    target_compile_definitions(kiln_sokol_impl PUBLIC SOKOL_METAL)
    enable_language(OBJC) # the Metal backend is Objective-C; the project enables only C and C++
    set_source_files_properties(sokol_impl.c PROPERTIES LANGUAGE OBJC)
    target_link_libraries(kiln_sokol_impl PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework Metal"
                                                 "-framework MetalKit")
else()
    target_compile_definitions(kiln_sokol_impl PUBLIC SOKOL_GLCORE)
    find_package(Threads REQUIRED)
    target_link_libraries(kiln_sokol_impl PUBLIC X11 Xi Xcursor GL dl m Threads::Threads)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(kiln_sokol_impl PRIVATE -Wno-everything)
elseif(MSVC)
    target_compile_options(kiln_sokol_impl PRIVATE /W0)
else()
    target_compile_options(kiln_sokol_impl PRIVATE -w)
endif()

# The adapter and the readback, shared by both programs.
add_library(kiln_sokol_example STATIC sokol_adapter.h sokol_adapter.cpp readback.h readback.cpp)
kiln_apply_defaults(kiln_sokol_example)
target_include_directories(kiln_sokol_example PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_sokol_example PUBLIC kiln_runtime kiln_example_adapter_support kiln_sokol_impl)

function(kiln_sokol_example name main shader)
    add_executable(${name} ${main} ${shader}.glsl ${_shader_dir}/${shader}.glsl.h)
    kiln_apply_defaults(${name})
    target_include_directories(${name} SYSTEM PRIVATE ${_shader_dir}) # generated code: not held to kiln's warnings
    target_link_libraries(${name} PRIVATE kiln_sokol_example kiln_example_common)
    if(TARGET kiln_cook)
        target_link_libraries(${name} PRIVATE kiln_cook)
        target_compile_definitions(${name} PRIVATE KILN_SOKOL_HAS_COOK=1)
    else()
        target_compile_definitions(${name} PRIVATE KILN_SOKOL_HAS_COOK=0)
    endif()
endfunction()

kiln_sokol_example(kiln-sokol main.cpp scene)
# kiln-sokol-array: one texture array assembled from separate tiles (runtime-texture-arrays.md)
kiln_sokol_example(kiln-sokol-array main_array.cpp array)
target_link_libraries(kiln-sokol-array PRIVATE kiln_cli)
