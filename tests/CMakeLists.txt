# kiln unit tests — own minimal runner (tests/kiln_test.h), no third-party deps.
#
# Reader-only sources always build and run, so KILN_BUILD_COOK=OFF (shipping:
# kiln_core + kiln_runtime only) still gets a real test suite. Writer round-trips
# (test_mesh.cpp, test_ktx2.cpp) and the sample-file / kiln-info coverage that
# depends on them only join in when kiln_cook is built.
#
# The corpus (corpus/ktx2, corpus/gltf), the goldens (golden/) and a scratch dir
# (<build>/tests/samples) are compiled in as defaults, so running kiln_tests by hand runs
# every test; --corpus, --golden and --samples override them. The KTX2 corpus is read in
# both configurations; the writer round trip over it needs kiln_cook.
#
# Golden-file tests: tests/golden/README.md. Set
# -DKILN_UPDATE_GOLDEN=ON to regenerate the committed goldens instead of comparing
# against them — inspect the diff, then commit deliberately (bump
# kiln::cook::kCookerVersion first if the cooked bytes are meant to change for the
# same input/settings). Equivalent to passing `--update-golden` directly.

set(KILN_KTX2_CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/ktx2)
set(KILN_GOLDEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/golden)

option(KILN_UPDATE_GOLDEN "Write tests/golden/* instead of comparing against them" OFF)
set(KILN_TEST_ARGS "")
if(KILN_UPDATE_GOLDEN)
    list(APPEND KILN_TEST_ARGS --update-golden)
endif()

add_executable(kiln_tests
    kiln_test.h
    test_main.cpp
    test_core.cpp
    test_alloc.cpp
    test_result.cpp
    test_hash.cpp
    test_containers.cpp
    test_formats.cpp
    test_mesh_read.cpp
    test_ktx2_read.cpp
    ktx2_corpus.h
    test_ktx2_corpus.cpp
    test_null_adapter.cpp
    test_io.cpp
    test_runtime.cpp
)
kiln_apply_defaults(kiln_tests)
target_include_directories(kiln_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_tests PRIVATE kiln_core kiln_runtime)
target_compile_definitions(kiln_tests PRIVATE
    KILN_TEST_CORPUS_DIR="${KILN_KTX2_CORPUS}"
    KILN_TEST_GOLDEN_DIR="${KILN_GOLDEN_DIR}"
    KILN_TEST_SAMPLES_DIR="${CMAKE_CURRENT_BINARY_DIR}/samples")

if(TARGET kiln_cook)
    target_sources(kiln_tests PRIVATE test_mesh.cpp test_ktx2.cpp test_ktx2_corpus_rt.cpp
                                       test_settings.cpp test_sidecar.cpp test_store.cpp
                                       png_writer.h hdr_writer.h image_fixtures.h test_image.cpp
                                       test_texture_cook.cpp test_hdr.cpp test_texture_golden.cpp
                                       test_provider.cpp test_parallel.cpp test_texture_bc.cpp)
    target_link_libraries(kiln_tests PRIVATE kiln_cook)
    # bcdec decodes the BC encoders' output (test_texture_bc.cpp).
    target_link_libraries(kiln_tests PRIVATE kiln_bcdec)
    # KILN_MESH=OFF (docs/design/texture-only.md): the mesh cooker tests leave the build, and
    # the mixed test files switch their mesh cases for checks that meshes fail with K1021.
    if(KILN_MESH)
        target_sources(kiln_tests PRIVATE test_mesh_cook.cpp test_mesh_golden.cpp)
        target_compile_definitions(kiln_tests PRIVATE KILN_MESH=1)
    else()
        target_compile_definitions(kiln_tests PRIVATE KILN_MESH=0)
    endif()

    add_test(NAME kiln_tests COMMAND kiln_tests ${KILN_TEST_ARGS})
    set_tests_properties(kiln_tests PROPERTIES FIXTURES_SETUP kiln_samples)

    # kiln-info coverage: feed it the sample .mesh/.ktx2 files kiln_tests just wrote.
    if(TARGET kiln-info)
        add_test(NAME kiln_info_mesh_raw
                 COMMAND $<TARGET_FILE:kiln-info> samples/sample_raw.mesh --blobs --check)
        add_test(NAME kiln_info_mesh_padded
                 COMMAND $<TARGET_FILE:kiln-info> samples/sample_padded.mesh --blobs --check)
        add_test(NAME kiln_info_ktx2_srgb
                 COMMAND $<TARGET_FILE:kiln-info> samples/sample_rgba8_srgb.ktx2 --check)
        add_test(NAME kiln_info_ktx2_snorm
                 COMMAND $<TARGET_FILE:kiln-info> samples/sample_r16g16_snorm.ktx2 --check)
        add_test(NAME kiln_info_bad_file
                 COMMAND $<TARGET_FILE:kiln-info> ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
        add_test(NAME kiln_info_usage COMMAND $<TARGET_FILE:kiln-info>)

        set_tests_properties(
            kiln_info_mesh_raw kiln_info_mesh_padded
            kiln_info_ktx2_srgb kiln_info_ktx2_snorm
            kiln_info_bad_file kiln_info_usage
            PROPERTIES FIXTURES_REQUIRED kiln_samples
        )
        set_tests_properties(
            kiln_info_mesh_raw kiln_info_mesh_padded
            kiln_info_ktx2_srgb kiln_info_ktx2_snorm
            PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
        set_tests_properties(kiln_info_bad_file kiln_info_usage PROPERTIES WILL_FAIL TRUE)
        set_tests_properties(kiln_info_mesh_raw PROPERTIES
            PASS_REGULAR_EXPRESSION "check: payload decoded")
        set_tests_properties(kiln_info_ktx2_srgb PROPERTIES
            PASS_REGULAR_EXPRESSION "transfer sRGB;check: 3 levels")
    endif()
else()
    add_test(NAME kiln_tests COMMAND kiln_tests ${KILN_TEST_ARGS})
endif()

# kiln_bench_image: manual image-kernel benchmark (rollout step 1, docs/design/cook-kernels.md).
# Deliberately not add_test()'d: it prints timings, not pass/fail, and is meant to be run
# by hand for a before/after comparison around the step 2 SIMD kernel work.
#   build/<preset>/tests/kiln_bench_image [--max 2048|4096|8192] [--repeat N] [--threads N]
if(TARGET kiln_cook)
    add_executable(kiln_bench_image bench_image.cpp png_writer.h)
    kiln_apply_defaults(kiln_bench_image)
    target_include_directories(kiln_bench_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(kiln_bench_image PRIVATE kiln_cook)
endif()

# wait() misuse must panic (never hang) with a K5007 message (test_runtime.cpp). These
# cases panic on purpose, so they run as their own processes, selected by exact name.
# PASS_REGULAR_EXPRESSION alone decides (CTest ignores the exit code then; combining it
# with WILL_FAIL would invert the match). Without the panic the output has no "K5007".
foreach(_case WaitOffThread WaitNotSelfSubmitting)
    add_test(NAME runtime_panic_${_case} COMMAND kiln_tests RuntimePanic.${_case} ${KILN_TEST_ARGS})
    set_tests_properties(runtime_panic_${_case} PROPERTIES PASS_REGULAR_EXPRESSION "PANIC[^
]*K5007[^
]*wait")
endforeach()

# ---------------------------------------------------------------------------
# Reference validation with the Khronos `ktx` CLI (KTX-Software 4.x), if installed:
#   ktx_validate_<name>         kiln writer output: sample_*.ktx2 and the corpus round
#                               trips rt_*.ktx2 (written by kiln_tests, cook builds only)
#   ktx_validate_corpus_<name>  every corpus file the manifest expects kiln to read,
#                               documenting that the inputs themselves are conformant
# ---------------------------------------------------------------------------
find_program(KILN_KTX_TOOL ktx)
if(KILN_KTX_TOOL)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KILN_KTX2_CORPUS}/manifest.txt)
    file(STRINGS ${KILN_KTX2_CORPUS}/manifest.txt _kiln_manifest REGEX "^[^#].*[|]")
    set(_kiln_rt_names "")
    foreach(_line IN LISTS _kiln_manifest)
        string(REPLACE "|" ";" _f "${_line}")
        list(GET _f 0 _path)
        list(GET _f 1 _expect)
        list(GET _f 3 _format)
        list(GET _f 6 _depth)
        list(GET _f 7 _layers)
        list(GET _f 8 _faces)
        list(GET _f 10 _sc)
        if(NOT _expect STREQUAL "ok")
            continue()
        endif()
        get_filename_component(_base ${_path} NAME)
        get_filename_component(_stem ${_path} NAME_WE)
        add_test(NAME ktx_validate_corpus_${_stem} COMMAND ${KILN_KTX_TOOL} validate ${KILN_KTX2_CORPUS}/${_path})
        # Same eligibility rule as test_ktx2_corpus_rt.cpp: uncompressed or BC; 2D, cube or array.
        if(_depth EQUAL 0 AND (_faces EQUAL 1 OR (_faces EQUAL 6 AND _layers EQUAL 0)) AND _sc EQUAL 0
           AND NOT _format MATCHES "^(-|ETC2_|EAC_|ASTC_)")
            list(APPEND _kiln_rt_names ${_base})
        endif()
    endforeach()

    if(TARGET kiln_cook)
        foreach(_file sample_rgba8_srgb.ktx2 sample_r16g16_snorm.ktx2 sample_cube_rgba8.ktx2
                      sample_array3_rgba8.ktx2 sample_array1_rgba8.ktx2 sample_bc7_srgb_mip.ktx2
                      sample_bc5_unorm_cube.ktx2)
            get_filename_component(_stem ${_file} NAME_WE)
            add_test(NAME ktx_validate_${_stem} COMMAND ${KILN_KTX_TOOL} validate samples/${_file})
            list(APPEND _kiln_validate_tests ktx_validate_${_stem})
        endforeach()
        foreach(_base IN LISTS _kiln_rt_names)
            get_filename_component(_stem ${_base} NAME_WE)
            add_test(NAME ktx_validate_rt_${_stem} COMMAND ${KILN_KTX_TOOL} validate samples/rt_${_base})
            list(APPEND _kiln_validate_tests ktx_validate_rt_${_stem})
        endforeach()
        # cook_texture output (test_texture_cook.cpp writes samples/cooked_<case>.ktx2)
        foreach(_case color_srgb color_linear normal height16 mask_rg nomips maxsize cube_strip array_strip
                      hdr)
            add_test(NAME ktx_validate_cooked_${_case}
                     COMMAND ${KILN_KTX_TOOL} validate samples/cooked_${_case}.ktx2)
            list(APPEND _kiln_validate_tests ktx_validate_cooked_${_case})
        endforeach()
        # BC cooks (test_texture_bc.cpp writes samples/cooked_bc_<case>.ktx2)
        foreach(_case color ui orm normal mask mask_rg npot cube hdr)
            add_test(NAME ktx_validate_cooked_bc_${_case}
                     COMMAND ${KILN_KTX_TOOL} validate samples/cooked_bc_${_case}.ktx2)
            list(APPEND _kiln_validate_tests ktx_validate_cooked_bc_${_case})
        endforeach()
        set_tests_properties(${_kiln_validate_tests} PROPERTIES
            FIXTURES_REQUIRED kiln_samples
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()
else()
    message(STATUS "ktx tool not found; skipping ktx validate tests")
endif()
