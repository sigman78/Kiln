# third_party/meshoptimizer.cmake -- meshoptimizer for kiln_runtime (its vertex and index codecs decode
# compressed .mesh payloads) and kiln_cook (optimization, encoding). Pinned to the commit the v1.3 tag
# resolves to (never a floating branch or bare tag; docs/design/dependencies.md). A host that already
# has a `meshoptimizer` target keeps it: it must be v1.3 or newer, since an older decoder cannot read
# the codec streams v1.3 writes. Static libraries link only the objects kiln uses.

if(TARGET meshoptimizer)
    set(KILN_OWNS_MESHOPTIMIZER OFF)
    return()
endif()
set(KILN_OWNS_MESHOPTIMIZER ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG 9e1f07b159d3cb777f1c67ed31fc11fd117986f4 # v1.3
)
FetchContent_MakeAvailable(meshoptimizer)
