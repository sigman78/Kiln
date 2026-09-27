# kiln unit tests — own minimal runner (tests/kiln_test.h), no third-party deps.

add_executable(kiln_tests
    kiln_test.h
    test_main.cpp
    test_core.cpp
    test_alloc.cpp
    test_result.cpp
    test_hash.cpp
    test_containers.cpp
    test_formats.cpp
    test_mesh.cpp
    test_ktx2.cpp
)
kiln_apply_defaults(kiln_tests)
target_include_directories(kiln_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_tests PRIVATE kiln_core kiln_runtime)
if(TARGET kiln_cook)
    target_link_libraries(kiln_tests PRIVATE kiln_cook)
endif()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/samples)

add_test(NAME kiln_tests COMMAND kiln_tests --samples ${CMAKE_CURRENT_BINARY_DIR}/samples)
set_tests_properties(kiln_tests PROPERTIES FIXTURES_SETUP kiln_samples)

# kiln-info coverage: feed it the sample .mesh/.ktx2 files kiln_tests just wrote.
if(TARGET kiln-info)
    add_test(NAME kiln_info_mesh_raw
             COMMAND $<TARGET_FILE:kiln-info> samples/sample_raw.mesh --blobs --check)
    add_test(NAME kiln_info_mesh_padded
             COMMAND $<TARGET_FILE:kiln-info> samples/sample_padded.mesh --blobs --check)
    add_test(NAME kiln_info_mesh_split
             COMMAND $<TARGET_FILE:kiln-info> samples/sample_split.mesh --blobs --check)
    add_test(NAME kiln_info_ktx2_srgb
             COMMAND $<TARGET_FILE:kiln-info> samples/sample_rgba8_srgb.ktx2 --check)
    add_test(NAME kiln_info_ktx2_snorm
             COMMAND $<TARGET_FILE:kiln-info> samples/sample_r16g16_snorm.ktx2 --check)
    add_test(NAME kiln_info_bad_file
             COMMAND $<TARGET_FILE:kiln-info> ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
    add_test(NAME kiln_info_usage COMMAND $<TARGET_FILE:kiln-info>)

    set_tests_properties(
        kiln_info_mesh_raw kiln_info_mesh_padded kiln_info_mesh_split
        kiln_info_ktx2_srgb kiln_info_ktx2_snorm
        kiln_info_bad_file kiln_info_usage
        PROPERTIES FIXTURES_REQUIRED kiln_samples
    )
    set_tests_properties(
        kiln_info_mesh_raw kiln_info_mesh_padded kiln_info_mesh_split
        kiln_info_ktx2_srgb kiln_info_ktx2_snorm
        PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(kiln_info_bad_file kiln_info_usage PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(kiln_info_mesh_raw PROPERTIES
        PASS_REGULAR_EXPRESSION "check: payload decoded")
    set_tests_properties(kiln_info_ktx2_srgb PROPERTIES
        PASS_REGULAR_EXPRESSION "transfer sRGB;check: 3 levels")
endif()
