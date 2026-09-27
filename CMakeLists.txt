cmake_minimum_required(VERSION 3.25)

project(kiln
    VERSION 0.0.1
    DESCRIPTION "Graphics-API-agnostic asset cooking and async loading library"
    LANGUAGES CXX C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(kiln_warnings)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
option(KILN_BUILD_TESTS           "Build unit tests"                                 ${KILN_IS_TOP_LEVEL})
option(KILN_BUILD_EXAMPLES        "Build examples (null adapter, ...)"               OFF)
option(KILN_BUILD_VIEWER          "Build the Vulkan example viewer (needs Vulkan SDK)" OFF)
option(KILN_WARNINGS_AS_ERRORS    "Treat warnings as errors"                         ${KILN_IS_TOP_LEVEL})
option(KILN_INSTALL               "Generate install rules"                           ${KILN_IS_TOP_LEVEL})

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

# ---------------------------------------------------------------------------
# kiln_runtime — loading, .mesh/KTX2 readers, IO. Ships in release builds.
# Must never depend on kiln_cook.
# ---------------------------------------------------------------------------
add_library(kiln_runtime STATIC
    include/kiln/formats.h
    include/kiln/mesh.h
    src/formats/mesh_read.cpp
    include/kiln/ktx2.h
    src/formats/ktx2_read.cpp
    src/runtime/runtime.cpp
)
add_library(kiln::runtime ALIAS kiln_runtime)
set_target_properties(kiln_runtime PROPERTIES EXPORT_NAME runtime)
kiln_apply_defaults(kiln_runtime)
target_link_libraries(kiln_runtime PUBLIC kiln_core)
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
        include/kiln/cook/settings.h
        src/cook/settings.cpp
        include/kiln/cook/cook.h
        src/cook/cook.cpp
        src/cook/store.cpp
        include/kiln/cook/image.h
        src/cook/image.cpp
        src/cook/png_decode.cpp
        src/cook/texture_cook.cpp
        src/cook/cook_internal.h
        src/cook/gltf_import.cpp
        src/cook/mesh_cook.cpp
    )
    add_library(kiln::cook ALIAS kiln_cook)
set_target_properties(kiln_cook PROPERTIES EXPORT_NAME cook)
    kiln_apply_defaults(kiln_cook)
    target_link_libraries(kiln_cook PUBLIC kiln_runtime)
    target_link_libraries(kiln_cook PRIVATE kiln_third_party_cook)
endif()

# ---------------------------------------------------------------------------
# Tools, examples, tests
# ---------------------------------------------------------------------------
if(KILN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(KILN_BUILD_EXAMPLES)
    # examples/ arrives with M3 (null adapter) and M4 (viewer).
endif()

if(KILN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
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
