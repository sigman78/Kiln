# examples/common — shared by the windowed examples: GLFW, fetched once at a pinned commit, and
# kiln_example_common (logging, orbit camera, PNG dumps). Nothing here reaches the library.

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

# GLFW 3.5.1: window, input, GL contexts and the Vulkan surface. X11 only on Linux (fewer build deps).
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
FetchContent_MakeAvailable(glfw)

add_library(kiln_example_common STATIC example_math.h example_app.h example_app.cpp)
kiln_apply_defaults(kiln_example_common)
target_include_directories(kiln_example_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                               PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(kiln_example_common PUBLIC GLFW_INCLUDE_NONE)
target_link_libraries(kiln_example_common PUBLIC kiln_runtime glfw)
