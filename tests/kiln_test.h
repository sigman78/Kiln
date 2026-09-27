// Minimal test runner for kiln. No exceptions, no RTTI, no iostreams.
//
//   KILN_TEST(suite, name) { KILN_CHECK(x == 1); KILN_CHECK_EQ(a, b); }
//
// KILN_CHECK_* record a failure and continue; KILN_REQUIRE_* return from the test.
// Run: kiln_tests [filter-substring] [--list] [--samples <dir>] [--corpus <dir>]
//                  [--golden <dir>] [--update-golden]
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
    Registrar(TestCase* tc) noexcept; // NOLINT(google-explicit-constructor)
};

/// Report a failed check. Returns false for use in expressions.
bool fail(char const* file, int line, char const* expr, char const* fmt, ...) noexcept KILN_PRINTF(4, 5);

/// Number of failures recorded in the currently running test.
int current_failures() noexcept;

int run_all(int argc, char** argv) noexcept;

/// Directory passed via `--samples <dir>`, or nullptr if the flag was not given.
[[nodiscard]] char const* sample_dir() noexcept;

/// Directory passed via `--corpus <dir>` (the KTX2 corpus root holding manifest.txt),
/// or nullptr if the flag was not given.
[[nodiscard]] char const* corpus_dir() noexcept;

/// Directory passed via `--golden <dir>` (holds golden/mesh/*.mesh, golden/ktx2/*.ktx2),
/// or nullptr if the flag was not given. See tests/golden/README.md.
[[nodiscard]] char const* golden_dir() noexcept;

/// True if `--update-golden` was given: golden-file tests write the golden instead
/// of comparing against it. See tests/golden/README.md.
[[nodiscard]] bool update_golden() noexcept;

// Value → string helpers for KILN_CHECK_EQ messages.
usize to_str(char* buf, usize cap, bool v) noexcept;
usize to_str(char* buf, usize cap, char v) noexcept;
usize to_str(char* buf, usize cap, i32 v) noexcept;
usize to_str(char* buf, usize cap, u32 v) noexcept;
usize to_str(char* buf, usize cap, i64 v) noexcept;
usize to_str(char* buf, usize cap, u64 v) noexcept;
usize to_str(char* buf, usize cap, f64 v) noexcept;
usize to_str(char* buf, usize cap, StrView v) noexcept;
usize to_str(char* buf, usize cap, char const* v) noexcept;
usize to_str(char* buf, usize cap, void const* v) noexcept;
inline usize to_str(char* buf, usize cap, f32 v) noexcept { return to_str(buf, cap, f64(v)); }
#if defined(KILN_OS_WINDOWS) || (defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 4)
inline usize to_str(char* buf, usize cap, long v) noexcept { return to_str(buf, cap, i64(v)); }
inline usize to_str(char* buf, usize cap, unsigned long v) noexcept { return to_str(buf, cap, u64(v)); }
#endif
template <class E>
    requires std::is_scoped_enum_v<E>
usize to_str(char* buf, usize cap, E v) noexcept {
    return to_str(buf, cap, i64(std::to_underlying(v)));
}
template <class T> usize to_str(char* buf, usize cap, T* v) noexcept {
    return to_str(buf, cap, static_cast<void const*>(v));
}
/// Fallback for anything else: prints "<?>" so the macro still compiles.
template <class T>
    requires(!std::is_enum_v<T>)
usize to_str(char* buf, usize cap, T const&) noexcept {
    return format(buf, cap, "<?>");
}

template <class A, class B>
bool check_eq(char const* file, int line, char const* expr, A const& a, B const& b) noexcept {
    if (a == b) return true;
    char sa[256], sb[256];
    to_str(sa, sizeof sa, a);
    to_str(sb, sizeof sb, b);
    return fail(file, line, expr, "  left:  %s\n  right: %s", sa, sb);
}
template <class A, class B>
bool check_ne(char const* file, int line, char const* expr, A const& a, B const& b) noexcept {
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
