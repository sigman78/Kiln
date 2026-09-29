# fuzz — libFuzzer targets for the parsers of untrusted bytes (KILN_FUZZ=ON, clang). Local only, not
# in CI. Run one with a seed corpus, e.g.: kiln_fuzz_mesh_read -max_total_time=60 corpus/ tests/golden/mesh
# libFuzzer writes new inputs into the first directory: make it a scratch one, never tests/golden.
# On Windows (clang with the GNU driver), put LLVM's lib/clang/<ver>/lib/windows on PATH for the ASan DLL.
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
