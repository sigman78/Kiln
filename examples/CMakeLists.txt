# Examples. kiln-headless needs only kiln_runtime; cook-on-miss is compiled in when kiln_cook exists.

add_executable(kiln-headless headless/main.cpp)
kiln_apply_defaults(kiln-headless)
target_link_libraries(kiln-headless PRIVATE kiln_runtime kiln_cli)
if(TARGET kiln_cook)
    target_link_libraries(kiln-headless PRIVATE kiln_cook)
    target_compile_definitions(kiln-headless PRIVATE KILN_HEADLESS_HAS_COOK=1)
else()
    target_compile_definitions(kiln-headless PRIVATE KILN_HEADLESS_HAS_COOK=0)
endif()

# Smoke test against the committed golden files; the delays make the output show real progress.
if(KILN_BUILD_TESTS)
    add_test(NAME example_headless
             COMMAND kiln-headless --store ${CMAKE_SOURCE_DIR}/tests/golden --layout named --slow 200 --latency 5
                     mesh/Box.mesh mesh/BoxTextured.mesh ktx2/color_srgb.ktx2 ktx2/normal.ktx2)

    # Cook-on-miss from two source roots into an empty catalog store: the default root and a named
    # root `gen:`. kiln-info then checks the catalog and every artifact it names.
    if(TARGET kiln_cook AND KILN_MESH)
        set(_store ${CMAKE_BINARY_DIR}/example-roots-store)
        add_test(NAME example_headless_roots_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${_store})
        set_tests_properties(example_headless_roots_clean PROPERTIES FIXTURES_SETUP headless_roots)
        add_test(NAME example_headless_roots
                 COMMAND kiln-headless --store ${_store}
                         --source ${CMAKE_SOURCE_DIR}/tests/corpus/gltf/khronos
                         --root gen=${CMAKE_SOURCE_DIR}/tests/corpus/gltf/generated
                         Box.glb.mesh "gen:external_uri.gltf.mesh" "gen:external_uri_albedo.png.ktx2"
                         "gen:pbr_textures.glb#hull_albedo.ktx2")
        set_tests_properties(example_headless_roots PROPERTIES FIXTURES_SETUP headless_roots_store
                                                               FIXTURES_REQUIRED headless_roots)
        add_test(NAME example_headless_roots_layout
                 COMMAND $<TARGET_FILE:kiln-info> "${_store}/catalogs/compat.kcat" --check)
        set_tests_properties(example_headless_roots_layout PROPERTIES FIXTURES_REQUIRED headless_roots_store)
    endif()
endif()

# CPU-only helpers for the example adapters, built with every example set so their tests run in
# the default CI.
add_subdirectory(adapter_support)

# The windowed examples share GLFW and examples/common (docs/design/integration-examples.md).
if(KILN_BUILD_VIEWER OR KILN_EXAMPLE_GL OR KILN_EXAMPLE_GL_BINDLESS OR KILN_EXAMPLE_SOKOL OR KILN_EXAMPLE_VK_BASIC OR KILN_EXAMPLE_NGA)
    add_subdirectory(common)
endif()
if(KILN_BUILD_VIEWER OR KILN_EXAMPLE_VK_BASIC)
    add_subdirectory(viewer) # kiln_example_vk, and kiln-viewer with KILN_BUILD_VIEWER
endif()
if(KILN_EXAMPLE_VK_BASIC)
    add_subdirectory(vk-basic)
endif()
if(KILN_EXAMPLE_NGA)
    add_subdirectory(nga)
endif()
if(KILN_EXAMPLE_GL OR KILN_EXAMPLE_GL_BINDLESS)
    add_subdirectory(gl)
endif()
if(KILN_EXAMPLE_SOKOL)
    add_subdirectory(sokol)
endif()
