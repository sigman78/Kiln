# examples/adapter_support — CPU-only helpers the example adapters share (no graphics API, no GLFW).
# Candidates for a supported library component once their interfaces prove themselves.

add_library(kiln_example_adapter_support STATIC staging_ring.h staging_ring.cpp upload_pool.h commit_queue.h
                                                   commit_queue.cpp)
kiln_apply_defaults(kiln_example_adapter_support)
target_include_directories(kiln_example_adapter_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_example_adapter_support PUBLIC kiln_core)

if(KILN_BUILD_TESTS)
    # The unit-test runner of tests/, with its own main and the same data-dir defaults (unused here).
    add_executable(kiln_adapter_support_tests staging_ring_test.cpp upload_pool_test.cpp commit_queue_test.cpp
                                              ${CMAKE_SOURCE_DIR}/tests/test_main.cpp)
    kiln_apply_defaults(kiln_adapter_support_tests)
    target_include_directories(kiln_adapter_support_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(kiln_adapter_support_tests PRIVATE kiln_example_adapter_support)
    target_compile_definitions(kiln_adapter_support_tests PRIVATE
        KILN_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus/ktx2"
        KILN_TEST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden"
        KILN_TEST_SAMPLES_DIR="${CMAKE_CURRENT_BINARY_DIR}/samples")
    add_test(NAME adapter_support COMMAND kiln_adapter_support_tests)
endif()
