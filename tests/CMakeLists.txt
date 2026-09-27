# kiln unit tests — own minimal runner (tests/kiln_test.h), no third-party deps.

add_executable(kiln_tests
    kiln_test.h
    test_main.cpp
    test_core.cpp
    test_alloc.cpp
    test_result.cpp
    test_hash.cpp
    test_containers.cpp
)
kiln_apply_defaults(kiln_tests)
target_include_directories(kiln_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_tests PRIVATE kiln_core kiln_runtime)
if(TARGET kiln_cook)
    target_link_libraries(kiln_tests PRIVATE kiln_cook)
endif()

add_test(NAME kiln_tests COMMAND kiln_tests)
