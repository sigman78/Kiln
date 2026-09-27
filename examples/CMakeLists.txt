# Examples. kiln-headless needs only kiln_runtime; cook-on-miss is compiled in when kiln_cook exists.

add_executable(kiln-headless headless/main.cpp)
kiln_apply_defaults(kiln-headless)
target_link_libraries(kiln-headless PRIVATE kiln_runtime)
if(TARGET kiln_cook)
    target_link_libraries(kiln-headless PRIVATE kiln_cook)
    target_compile_definitions(kiln-headless PRIVATE KILN_HEADLESS_HAS_COOK=1)
else()
    target_compile_definitions(kiln-headless PRIVATE KILN_HEADLESS_HAS_COOK=0)
endif()

# Smoke test against the committed golden files; the delays make the output show real progress.
if(KILN_BUILD_TESTS)
    add_test(NAME example_headless
             COMMAND kiln-headless --store ${CMAKE_SOURCE_DIR}/tests/golden --slow 200 --latency 5
                     mesh/Box.mesh mesh/BoxTextured.mesh ktx2/color_srgb.ktx2 ktx2/normal.ktx2)
endif()
