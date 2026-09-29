# third_party/zstd/CMakeLists.txt
#
# Zstandard (BSD), vendored lib/ of v1.5.7 (third_party/README.md):
#   - kiln_zstd:     the decoder (common + decompress). kiln_runtime links it PRIVATE to read
#                    Zstd-supercompressed KTX2; it ships, so it is part of kilnRuntimeTargets.
#   - kiln_zstd_enc: the encoder (compress). Only kiln_cook links it.
# Single-threaded, no assembly (the x86-64 Huffman decoder in huf_decompress_amd64.S is not
# vendored), no legacy formats. Compiled quietly, like the other vendored C.

set(_zstd ${CMAKE_CURRENT_SOURCE_DIR}/lib)

add_library(kiln_zstd STATIC
    ${_zstd}/common/debug.c
    ${_zstd}/common/entropy_common.c
    ${_zstd}/common/error_private.c
    ${_zstd}/common/fse_decompress.c
    ${_zstd}/common/xxhash.c
    ${_zstd}/common/zstd_common.c
    ${_zstd}/decompress/huf_decompress.c
    ${_zstd}/decompress/zstd_ddict.c
    ${_zstd}/decompress/zstd_decompress.c
    ${_zstd}/decompress/zstd_decompress_block.c)
set(_kiln_zstd_targets kiln_zstd)

if(KILN_BUILD_COOK)
    add_library(kiln_zstd_enc STATIC
        ${_zstd}/compress/fse_compress.c
        ${_zstd}/compress/hist.c
        ${_zstd}/compress/huf_compress.c
        ${_zstd}/compress/zstd_compress.c
        ${_zstd}/compress/zstd_compress_literals.c
        ${_zstd}/compress/zstd_compress_sequences.c
        ${_zstd}/compress/zstd_compress_superblock.c
        ${_zstd}/compress/zstd_double_fast.c
        ${_zstd}/compress/zstd_fast.c
        ${_zstd}/compress/zstd_lazy.c
        ${_zstd}/compress/zstd_ldm.c
        ${_zstd}/compress/zstd_opt.c
        ${_zstd}/compress/zstd_preSplit.c)
    target_link_libraries(kiln_zstd_enc PUBLIC kiln_zstd)
    list(APPEND _kiln_zstd_targets kiln_zstd_enc)
endif()

foreach(t IN LISTS _kiln_zstd_targets)
    target_compile_definitions(${t} PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)
    if(CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
        target_compile_options(${t} PRIVATE -Wno-everything)
    elseif(MSVC)
        target_compile_options(${t} PRIVATE /W3)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${t} PRIVATE -w)
    endif()
endforeach()

# SYSTEM: zstd.h stays out of kiln's warning set.
target_include_directories(kiln_zstd SYSTEM INTERFACE $<BUILD_INTERFACE:${_zstd}>)

# A static kiln_runtime or kiln_cook needs its PRIVATE link dependencies in its export set.
if(KILN_INSTALL)
    install(TARGETS kiln_zstd
        EXPORT kilnRuntimeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(KILN_BUILD_COOK)
        install(TARGETS kiln_zstd_enc
            EXPORT kilnCookTargets
            COMPONENT cook
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
endif()
