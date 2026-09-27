# kiln_warnings.cmake
#
# Centralized compiler warning / target-default configuration for kiln.
# See docs/HANDOFF.md, section 2 ("Style"), for the policy this implements.

include_guard(GLOBAL)

# kiln_apply_warnings(target)
#
# Applies kiln's curated warning set to `target` as PRIVATE compile options.
# Options are wrapped in a COMPILE_LANGUAGE generator expression so they only
# apply to C++ translation units.
function(kiln_apply_warnings target)
    set(kiln_gnu_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat
        -Wformat-security
        -Wimplicit-fallthrough
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl: MSVC driver, clang diagnostics. /W4 maps to -Wall -Wextra; the
        # explicit list keeps Windows builds aligned with Linux clang/gcc. Flags
        # that cl.exe needs but clang-cl does not know (/Zc:preprocessor) are
        # omitted because they trip -Wunused-command-line-argument under /WX.
        # NOTE: under clang-cl a bare -Wall means -Weverything, so the explicit
        # list is used without -Wall/-Wextra (which /W4 already implies).
        set(kiln_clangcl_warnings ${kiln_gnu_warnings} -Wnull-dereference)
        list(REMOVE_ITEM kiln_clangcl_warnings -Wall -Wextra)
        set(kiln_flags
            /W4
            /permissive-
            /utf-8
            /EHsc
            ${kiln_clangcl_warnings}
            -Wno-language-extension-token   # MSVC headers use __int64 etc.
        )
        if(KILN_WARNINGS_AS_ERRORS)
            list(APPEND kiln_flags /WX)
        endif()
    elseif(MSVC)
        set(kiln_flags
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /EHsc
            /wd4324 # "structure was padded due to alignment specifier" -- acceptable
        )
        if(KILN_WARNINGS_AS_ERRORS)
            list(APPEND kiln_flags /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        set(kiln_flags ${kiln_gnu_warnings})
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # -Wnull-dereference is clang-only: gcc's version depends on optimizer
            # state and reports false positives at -O2/-O3 (seen on HashMap::find).
            list(APPEND kiln_flags -Wduplicated-cond -Wlogical-op)
            if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
                # gcc 13's -Wdangling-reference (in -Wall) misfires on the common
                # `Span<T> by value` + `operator[]` returning a reference pattern.
                list(APPEND kiln_flags -Wno-dangling-reference)
            endif()
        else()
            list(APPEND kiln_flags -Wnull-dereference)
        endif()
        if(KILN_WARNINGS_AS_ERRORS)
            list(APPEND kiln_flags -Werror)
        endif()
    endif()

    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${kiln_flags}>
    )
endfunction()

# kiln_apply_defaults(target)
#
# Applies kiln's baseline target setup: the warning set above, the C++23
# language standard with compiler extensions disabled, and a few MSVC-only
# compile definitions that avoid CRT and macro friction.
#
# Exceptions and RTTI are intentionally left enabled here -- KILN_NO_EXCEPTIONS
# / KILN_NO_RTTI are a later (v1.0 hardening) addition, per docs/HANDOFF.md.
function(kiln_apply_defaults target)
    kiln_apply_warnings(${target})

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(MSVC)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
endfunction()
