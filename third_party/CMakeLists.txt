# third_party/CMakeLists.txt
#
# Cook-only third-party dependencies (docs/design/dependencies.md, M2):
#   - cgltf (MIT)         -- glTF import: vendored header, one-line .c TU
#   - MikkTSpace (zlib)   -- reference tangent-space generation: vendored .c/.h
#   - wuffs (Apache-2.0)  -- PNG/JPEG (+ WebP with KILN_WEBP) decode: vendored single-file release .c,
#                            modules listed in wuffs/wuffs_modules.h
#   - meshoptimizer (MIT) -- FetchContent, pinned to a commit hash
#
# All of it is compiled into one static library, kiln_third_party_cook, which
# kiln_cook links PRIVATE. None of this may reach kiln_runtime, the shipping
# install, or kiln_cook's own installed INTERFACE -- see
# docs/design/shipping-split.md ("nothing here may touch kiln_runtime") and
# the header rule in dependencies.md ("third-party header never appears in
# include/kiln/").
#
# This directory is only added when KILN_BUILD_COOK is ON (see root
# CMakeLists.txt), so a shipping configure (KILN_BUILD_COOK=OFF) never even
# evaluates this file: no FetchContent network access, no third-party
# compilation, in a shipping build.

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# meshoptimizer -- pinned to the commit the v1.3 tag resolves to (never a
# floating branch or bare tag; see docs/design/dependencies.md rules).
# ---------------------------------------------------------------------------
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG 9e1f07b159d3cb777f1c67ed31fc11fd117986f4 # v1.3
)
FetchContent_MakeAvailable(meshoptimizer)

# ---------------------------------------------------------------------------
# kiln_third_party_cook -- one static lib for the vendored C dependencies.
# ---------------------------------------------------------------------------
add_library(kiln_third_party_cook STATIC
    cgltf/cgltf.c
    mikktspace/mikktspace.c
    wuffs/wuffs_impl.c
)

# Third-party code is compiled quietly, never with kiln's own warning set
# (cmake/kiln_warnings.cmake, kiln_apply_warnings/kiln_apply_defaults are
# intentionally not called here) and never with warnings-as-errors,
# regardless of KILN_WARNINGS_AS_ERRORS.
# Checked in an order where any Clang (including clang-cl, whose compiler ID
# is also "Clang") takes -Wno-everything ahead of the plain MSVC branch, so
# only real cl.exe gets /W3.
if(CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    target_compile_options(kiln_third_party_cook PRIVATE -Wno-everything)
elseif(MSVC)
    target_compile_options(kiln_third_party_cook PRIVATE /W3)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(kiln_third_party_cook PRIVATE -w)
endif()

if(MSVC)
    target_compile_definitions(kiln_third_party_cook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(KILN_WEBP)
    target_compile_definitions(kiln_third_party_cook PRIVATE KILN_WEBP=1)
endif()

# Include dirs are an INTERFACE usage requirement of this target, but
# kiln_cook links kiln_third_party_cook PRIVATE (below in root CMakeLists.txt),
# so they reach kiln_cook's own .cpp files and stop there: they never leak to
# kiln_runtime (which never links this target at all) or to kiln_cook's own
# installed/consumer-facing INTERFACE.
target_include_directories(kiln_third_party_cook INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cgltf>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mikktspace>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wuffs>
)

# meshoptimizer is linked PUBLIC so kiln_cook picks it up transitively through
# kiln_third_party_cook (kiln_cook's own link to kiln_third_party_cook is
# PRIVATE, so this still does not escape kiln_cook's own build).
target_link_libraries(kiln_third_party_cook PUBLIC meshoptimizer)

# ---------------------------------------------------------------------------
# Install / export.
#
# kiln_cook is installed as a static lib in the "cook" export set
# (kilnCookTargets, root CMakeLists.txt). A static library's PRIVATE link
# dependencies still have to be installed and exported themselves, or
# install(EXPORT kilnCookTargets ...) fails when it tries to describe
# kiln_cook's link interface. Both third-party static libs are added to that
# same export set here; CMake accumulates an EXPORT set across multiple
# install(TARGETS ... EXPORT kilnCookTargets ...) calls, and this directory
# is a descendant of the directory that later calls
# install(EXPORT kilnCookTargets ...), so this is legal.
#
# Neither target declares an $<INSTALL_INTERFACE:...> for its include dirs
# (only $<BUILD_INTERFACE:...>, mirroring meshoptimizer's own upstream
# CMakeLists.txt), so nothing here reintroduces a build-tree path into an
# installed kiln::cook consumer's include path.
# ---------------------------------------------------------------------------
if(KILN_INSTALL)
    install(TARGETS kiln_third_party_cook meshoptimizer
        EXPORT kilnCookTargets
        COMPONENT cook
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
