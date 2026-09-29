# fuzz — libFuzzer targets for the parsers of untrusted bytes (KILN_FUZZ=ON, clang). Run one with
# a seed corpus, e.g.: kiln_fuzz_mesh_read -max_total_time=60 corpus/ tests/golden/mesh
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "KILN_FUZZ needs clang (libFuzzer)")
endif()
if(NOT KILN_BUILD_COOK)
    message(FATAL_ERROR "KILN_FUZZ needs KILN_BUILD_COOK (fuzz_hdr_decode)")
endif()

foreach(target mesh_read ktx2_read hdr_decode)
    add_executable(kiln_fuzz_${target} fuzz_${target}.cpp)
    target_link_libraries(kiln_fuzz_${target} PRIVATE kiln_runtime kiln_cook)
    target_link_options(kiln_fuzz_${target} PRIVATE -fsanitize=fuzzer)
    kiln_apply_warnings(kiln_fuzz_${target})
endforeach()
