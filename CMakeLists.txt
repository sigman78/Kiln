cmake_minimum_required(VERSION 3.25)

project(kiln
    VERSION 0.5.0
    DESCRIPTION "Graphics-API-agnostic asset cooking and async loading library"
    LANGUAGES CXX C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(kiln_warnings)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED) # kiln_runtime's built-in thread pool (src/io/thread_pool.cpp)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(KILN_IS_TOP_LEVEL ON)
else()
    set(KILN_IS_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(KILN_BUILD_COOK            "Build kiln_cook (cooker library)"                 ON)
option(KILN_BUILD_TOOLS           "Build kiln-cook / kiln-info CLI tools"            ON)
option(KILN_HOT_RELOAD            "Compile hot-reload support into kiln_runtime"     ON)
option(KILN_WEBP                  "Decode WebP texture sources in kiln_cook"         OFF)
option(KILN_MESH                  "Cook meshes in kiln_cook (glTF import, cgltf, MikkTSpace, meshoptimizer)" ON)
option(KILN_BUILD_TESTS           "Build unit tests"                                 ${KILN_IS_TOP_LEVEL})
option(KILN_BUILD_EXAMPLES        "Build examples (null adapter, ...)"               OFF)
option(KILN_BUILD_VIEWER          "Build the Vulkan example viewer (headers fetched; no SDK)" OFF)
option(KILN_EXAMPLE_GL            "Build the OpenGL 4.6 integration example (kiln-gl)"  OFF)
option(KILN_EXAMPLE_GL_BINDLESS   "Build the bindless OpenGL example (kiln-gl-bindless)" OFF)
option(KILN_EXAMPLE_SOKOL         "Build the sokol_gfx example (kiln-sokol; fetches sokol and sokol-shdc)" OFF)
option(KILN_EXAMPLE_VK_BASIC      "Build the non-bindless Vulkan example (kiln-vk-basic)" OFF)
option(KILN_EXAMPLE_NGA           "Build the NoGraphicsAPI example (kiln-nga; fetches NoGraphicsAPI, the Vulkan loader, Slang)" OFF)
option(KILN_WARNINGS_AS_ERRORS    "Treat warnings as errors"                         ${KILN_IS_TOP_LEVEL})
option(KILN_INSTALL               "Generate install rules"                           ${KILN_IS_TOP_LEVEL})
set(KILN_SANITIZE "" CACHE STRING "Sanitizers for every target, e.g. address,undefined (clang or gcc)")
option(KILN_FUZZ                  "Build the libFuzzer targets in fuzz/ (clang)"      OFF)

# Sanitizers apply to every target, third-party code included, so containers and readers are
# checked across library boundaries. The fuzz targets add libFuzzer's coverage on top.
if(KILN_SANITIZE OR KILN_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR "KILN_SANITIZE and KILN_FUZZ need clang or gcc with the GNU driver")
    endif()
    if(KILN_SANITIZE)
        add_compile_options(-fsanitize=${KILN_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${KILN_SANITIZE})
    endif()
    if(KILN_FUZZ)
        add_compile_options(-fsanitize=fuzzer-no-link)
        if(WIN32) # LLVM's Windows libFuzzer is built against the static C runtime
            set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
        endif()
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# kiln_core — vocabulary types, allocators, logging. Depends on nothing.
# ---------------------------------------------------------------------------
add_library(kiln_core STATIC
    include/kiln/core.h
    include/kiln/alloc.h
    include/kiln/result.h
    include/kiln/hash.h
    include/kiln/log.h
    include/kiln/containers.h
    src/core/panic.cpp
    src/core/alloc.cpp
    src/core/log.cpp
    src/core/result.cpp
    src/core/hash.cpp
)
add_library(kiln::core ALIAS kiln_core)
set_target_properties(kiln_core PROPERTIES EXPORT_NAME core)
kiln_apply_defaults(kiln_core)
target_include_directories(kiln_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(kiln_core PUBLIC cxx_std_23)

# zstd: the runtime decodes Zstd-supercompressed KTX2 levels; the cook also encodes them.
add_subdirectory(third_party/zstd)

# ---------------------------------------------------------------------------
# kiln_runtime — loading, .mesh/KTX2 readers, IO. Ships in release builds.
# Must never depend on kiln_cook.
# ---------------------------------------------------------------------------
add_library(kiln_runtime STATIC
    include/kiln/formats.h
    include/kiln/mesh.h
    src/formats/mesh_read.cpp
    include/kiln/ktx2.h
    src/formats/formats_internal.h
    src/formats/ktx2_read.cpp
    src/formats/zstd_decode.cpp
    include/kiln/manifest.h
    src/formats/xxh3.cpp
    src/formats/manifest_read.cpp
    include/kiln/assets.h
    src/runtime/runtime_internal.h
    src/runtime/context.cpp
    src/runtime/registry.cpp
    src/runtime/names.cpp
    src/runtime/loader.cpp
    src/runtime/pump.cpp
    src/runtime/register.cpp
    src/runtime/watch.cpp
    include/kiln/adapter.h
    include/kiln/null_adapter.h
    src/runtime/null_adapter.cpp
    include/kiln/placeholders.h
    src/runtime/placeholders.cpp
    include/kiln/io.h
    src/io/thread_pool.cpp
    src/io/compat_io.cpp
)
add_library(kiln::runtime ALIAS kiln_runtime)
set_target_properties(kiln_runtime PROPERTIES EXPORT_NAME runtime)
kiln_apply_defaults(kiln_runtime)
target_link_libraries(kiln_runtime PUBLIC kiln_core Threads::Threads)
target_link_libraries(kiln_runtime PRIVATE kiln_zstd)
if(KILN_HOT_RELOAD)
    target_compile_definitions(kiln_runtime PUBLIC KILN_HOT_RELOAD=1)
endif()

# ---------------------------------------------------------------------------
# kiln_cook — importers, encoders, settings, store writer. Dev builds and tools.
# ---------------------------------------------------------------------------
if(KILN_BUILD_COOK)
    # Cook-only third-party deps (cgltf, MikkTSpace, wuffs, meshoptimizer):
    # see docs/design/dependencies.md and third_party/CMakeLists.txt. Never
    # evaluated in a shipping configure (KILN_BUILD_COOK=OFF).
    add_subdirectory(third_party)

    add_library(kiln_cook STATIC
        include/kiln/cook/mesh_writer.h
        src/formats/mesh_write.cpp
        include/kiln/cook/ktx2_writer.h
        src/formats/ktx2_write.cpp
        src/formats/manifest_write.cpp
        include/kiln/cook/settings.h
        src/cook/settings.cpp
        include/kiln/cook/sidecar.h
        src/cook/sidecar.cpp
        src/cook/toml_subset.h
        src/cook/toml_subset.cpp
        include/kiln/cook/cook.h
        src/cook/store.cpp
        include/kiln/cook/image.h
        src/cook/image.cpp
        src/cook/kernels.h
        src/cook/parallel.h
        src/cook/parallel.cpp
        src/cook/image_decode.cpp
        src/cook/hdr_decode.cpp
        src/cook/texture_cook.cpp
        src/cook/bc_encode.h
        src/cook/bc_encode.cpp
        src/cook/cook_internal.h
        include/kiln/cook/provider.h
        src/cook/provider.cpp
        src/cook/unit.h
        src/cook/unit.cpp
        src/cook/manifest_store.h
        src/cook/manifest_store.cpp
        include/kiln/cook/manifest.h
        src/cook/build_key.cpp
        include/kiln/cook/cli.h
        src/cook/cli_main.cpp
    )
    add_library(kiln::cook ALIAS kiln_cook)
set_target_properties(kiln_cook PROPERTIES EXPORT_NAME cook)
    kiln_apply_defaults(kiln_cook)
    target_link_libraries(kiln_cook PUBLIC kiln_runtime)
    target_link_libraries(kiln_cook PRIVATE kiln_third_party_cook kiln_zstd_enc)
    # tools/cli.h: the option parser of cook_cli_main; header-only and never installed.
    target_link_libraries(kiln_cook PRIVATE $<BUILD_INTERFACE:kiln_cli>)
    if(KILN_WEBP)
        target_compile_definitions(kiln_cook PRIVATE KILN_WEBP=1)
    endif()
    # KILN_MESH=OFF: cook_mesh is a stub that reports K1021 (docs/design/texture-only.md).
    if(KILN_MESH)
        target_sources(kiln_cook PRIVATE src/cook/gltf_import.cpp src/cook/mesh_cook.cpp)
    else()
        target_sources(kiln_cook PRIVATE src/cook/mesh_cook_off.cpp)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tools, examples, tests
# ---------------------------------------------------------------------------
# Option-table argument parser shared by the tools and examples (header-only, not installed).
add_library(kiln_cli INTERFACE tools/cli.h)
target_include_directories(kiln_cli INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(kiln_cli INTERFACE kiln_core)

if(KILN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(KILN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KILN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(KILN_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Install / export: find_package(kiln) and add_subdirectory both work.
# ---------------------------------------------------------------------------
if(KILN_INSTALL)
    # Two export sets so a shipping install carries only the read-only side:
    #   kilnRuntimeTargets: kiln::core, kiln::runtime          (always)
    #   kilnCookTargets:    kiln::cook                         (component "cook")
    install(TARGETS kiln_core kiln_runtime
        EXPORT kilnRuntimeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/kiln DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "cook" EXCLUDE)
    install(EXPORT kilnRuntimeTargets
        NAMESPACE kiln::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kiln)

    if(KILN_BUILD_COOK)
        install(TARGETS kiln_cook
            EXPORT kilnCookTargets
            COMPONENT cook
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY include/kiln/cook DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kiln COMPONENT cook)
        install(EXPORT kilnCookTargets
            NAMESPACE kiln::
            COMPONENT cook
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kiln)
    endif()

    configure_package_config_file(
        cmake/kiln-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/kiln-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kiln)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/kiln-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/kiln-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/kiln-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kiln)
endif()
