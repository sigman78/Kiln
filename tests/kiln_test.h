// tests/kiln_test.h — minimal test runner: no exceptions, no RTTI, no iostreams.
// KILN_CHECK_* record a failure and continue; KILN_REQUIRE_* return from the test.
// Run: kiln_tests [filter] [--list] [--samples|--corpus|--golden <dir>] [--update-golden]
#pragma once

#include "kiln/core.h"
#include "kiln/log.h"

namespace kiln::test {

using TestFn = void (*)();

struct TestCase {
    char const* suite;
    char const* name;
    TestFn fn;
    TestCase* next;
};

struct Registrar {
    Registrar(TestCase* tc); // NOLINT(google-explicit-constructor)
};

/// Report a failed check. Returns false for use in expressions.
bool fail(char const* file, int line, char const* expr, char const* fmt, ...) KILN_PRINTF(4, 5);

/// Number of failures recorded in the currently running test.
int current_failures();

int run_all(int argc, char** argv);

// The three directories default to paths compiled in by CMake; the flags override them.
// They are never null: run_all() exits before any test if one is unusable.

/// Scratch directory for files tests write (`--samples <dir>`). Created if missing.
char const* sample_dir();

/// The KTX2 corpus root holding manifest.txt (`--corpus <dir>`); the glTF corpus is `../gltf`.
char const* corpus_dir();

/// Golden files: mesh/*.mesh, ktx2/*.ktx2 (`--golden <dir>`). See tests/golden/README.md.
char const* golden_dir();

/// True if `--update-golden` was given: golden-file tests write the golden instead
/// of comparing against it. See tests/golden/README.md.
[[nodiscard]] bool update_golden();

/// True if the runner's filter argument is exactly `fullName` ("Suite.Name"). Tests
/// that must only run on request (e.g. ones that panic on purpose and are registered
/// as separate CTest entries) return early unless this holds.
[[nodiscard]] bool selected_exactly(char const* fullName);

// Value → string helpers for KILN_CHECK_EQ messages.
usize to_str(char* buf, usize cap, bool v);
usize to_str(char* buf, usize cap, char v);
usize to_str(char* buf, usize cap, i32 v);
usize to_str(char* buf, usize cap, u32 v);
usize to_str(char* buf, usize cap, i64 v);
usize to_str(char* buf, usize cap, u64 v);
usize to_str(char* buf, usize cap, f64 v);
usize to_str(char* buf, usize cap, StrView v);
usize to_str(char* buf, usize cap, char const* v);
usize to_str(char* buf, usize cap, void const* v);
inline usize to_str(char* buf, usize cap, f32 v) { return to_str(buf, cap, f64(v)); }
#if defined(KILN_OS_WINDOWS) || (defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 4)
inline usize to_str(char* buf, usize cap, long v) { return to_str(buf, cap, i64(v)); }
inline usize to_str(char* buf, usize cap, unsigned long v) { return to_str(buf, cap, u64(v)); }
#endif
template <class E>
    requires std::is_scoped_enum_v<E>
usize to_str(char* buf, usize cap, E v) {
    return to_str(buf, cap, i64(std::to_underlying(v)));
}
template <class T> usize to_str(char* buf, usize cap, T* v) {
    return to_str(buf, cap, static_cast<void const*>(v));
}
/// Fallback for anything else: prints "<?>" so the macro still compiles.
template <class T>
    requires(!std::is_enum_v<T>)
usize to_str(char* buf, usize cap, T const&) {
    return format(buf, cap, "<?>");
}

template <class A, class B>
bool check_eq(char const* file, int line, char const* expr, A const& a, B const& b) {
    if (a == b) return true;
    char sa[256], sb[256];
    to_str(sa, sizeof sa, a);
    to_str(sb, sizeof sb, b);
    return fail(file, line, expr, "  left:  %s\n  right: %s", sa, sb);
}
template <class A, class B>
bool check_ne(char const* file, int line, char const* expr, A const& a, B const& b) {
    if (!(a == b)) return true;
    char sa[256];
    to_str(sa, sizeof sa, a);
    return fail(file, line, expr, "  both:  %s", sa);
}

} // namespace kiln::test

#define KILN_TEST(suite, name)                                                                               \
    static void kiln_test_##suite##_##name();                                                                \
    static ::kiln::test::TestCase kiln_tc_##suite##_##name{#suite, #name, &kiln_test_##suite##_##name,       \
                                                           nullptr};                                         \
    static ::kiln::test::Registrar kiln_reg_##suite##_##name{&kiln_tc_##suite##_##name};                     \
    static void kiln_test_##suite##_##name()

#define KILN_CHECK(cond) ((cond) ? true : ::kiln::test::fail(__FILE__, __LINE__, #cond, "%s", ""))
#define KILN_CHECK_EQ(a, b) ::kiln::test::check_eq(__FILE__, __LINE__, #a " == " #b, (a), (b))
#define KILN_CHECK_NE(a, b) ::kiln::test::check_ne(__FILE__, __LINE__, #a " != " #b, (a), (b))
#define KILN_CHECK_MSG(cond, ...) ((cond) ? true : ::kiln::test::fail(__FILE__, __LINE__, #cond, __VA_ARGS__))

#define KILN_REQUIRE(cond)                                                                                   \
    do {                                                                                                     \
        if (!KILN_CHECK(cond)) return;                                                                       \
    } while (0)
#define KILN_REQUIRE_EQ(a, b)                                                                                \
    do {                                                                                                     \
        if (!KILN_CHECK_EQ(a, b)) return;                                                                    \
    } while (0)

// Brackets tests of deprecated API: they stay covered until it is removed.
#if defined(_MSC_VER) && !defined(__clang__)
#define KILN_TEST_DEPRECATED_BEGIN _Pragma("warning(push)") _Pragma("warning(disable : 4996)")
#define KILN_TEST_DEPRECATED_END _Pragma("warning(pop)")
#else
#define KILN_TEST_DEPRECATED_BEGIN                                                                           \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#define KILN_TEST_DEPRECATED_END _Pragma("GCC diagnostic pop")
#endif
