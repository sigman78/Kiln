# third_party/meshopt_decode/CMakeLists.txt
#
# meshoptimizer's vertex, index and filter codecs (MIT), vendored from the commit the cooker fetches
# (third_party/README.md): kiln_runtime decodes meshopt-compressed .mesh payloads with them. Every
# public function is renamed kiln_meshopt_* (kiln_meshopt_prefix.h), so a program that also links a
# full meshoptimizer (kiln_cook does) gets no duplicate symbols. Compiled quietly, like the other
# vendored code.

add_library(kiln_meshopt_dec STATIC vertexcodec.cpp indexcodec.cpp vertexfilter.cpp)
set_target_properties(kiln_meshopt_dec PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(kiln_meshopt_dec PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/kiln_meshopt_prefix.h
                                                    $<$<CXX_COMPILER_ID:Clang>:-Wno-everything>
                                                    $<$<NOT:$<CXX_COMPILER_ID:Clang>>:/W0>)
else()
    target_compile_options(kiln_meshopt_dec PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/kiln_meshopt_prefix.h
                                                    $<IF:$<CXX_COMPILER_ID:GNU>,-w,-Wno-everything>)
endif()
# SYSTEM: meshoptimizer.h stays out of kiln's warning set.
target_include_directories(kiln_meshopt_dec SYSTEM INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# A static kiln_runtime needs its PRIVATE link dependencies in its export set.
if(KILN_INSTALL)
    install(TARGETS kiln_meshopt_dec
        EXPORT kilnRuntimeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
