#include "kiln_test.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace kiln::test {

namespace {

TestCase* g_head        = nullptr;
TestCase* g_tail        = nullptr;
int g_count             = 0;
int g_failures          = 0; // in current test
char const* g_sampleDir = KILN_TEST_SAMPLES_DIR;
char const* g_corpusDir = KILN_TEST_CORPUS_DIR;
char const* g_goldenDir = KILN_TEST_GOLDEN_DIR;
bool g_updateGolden     = false;
char const* g_filter    = nullptr;

} // namespace

char const* sample_dir() noexcept { return g_sampleDir; }
char const* corpus_dir() noexcept { return g_corpusDir; }
char const* golden_dir() noexcept { return g_goldenDir; }
bool update_golden() noexcept { return g_updateGolden; }
bool selected_exactly(char const* fullName) noexcept {
    return g_filter && fullName && std::strcmp(g_filter, fullName) == 0;
}

Registrar::Registrar(TestCase* tc) noexcept {
    tc->next = nullptr;
    if (g_tail)
        g_tail->next = tc;
    else
        g_head = tc;
    g_tail = tc;
    ++g_count;
}

bool fail(char const* file, int line, char const* expr, char const* fmt, ...) noexcept {
    ++g_failures;
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vformat(msg, sizeof msg, fmt, args);
    va_end(args);
    std::fprintf(stderr, "  FAILED %s(%d): %s\n%s%s", file, line, expr, msg, msg[0] ? "\n" : "");
    return false;
}

int current_failures() noexcept { return g_failures; }

usize to_str(char* buf, usize cap, bool v) noexcept { return format(buf, cap, "%s", v ? "true" : "false"); }
usize to_str(char* buf, usize cap, char v) noexcept { return format(buf, cap, "'%c' (%d)", v, int(v)); }
usize to_str(char* buf, usize cap, i32 v) noexcept { return format(buf, cap, "%d", v); }
usize to_str(char* buf, usize cap, u32 v) noexcept { return format(buf, cap, "%u (0x%x)", v, v); }
usize to_str(char* buf, usize cap, i64 v) noexcept {
    return format(buf, cap, "%lld", static_cast<long long>(v));
}
usize to_str(char* buf, usize cap, u64 v) noexcept {
    return format(buf, cap, "%llu (0x%llx)", static_cast<unsigned long long>(v),
                  static_cast<unsigned long long>(v));
}
usize to_str(char* buf, usize cap, f64 v) noexcept { return format(buf, cap, "%.9g", v); }
usize to_str(char* buf, usize cap, StrView v) noexcept { return format(buf, cap, "\"%.*s\"", KILN_SV(v)); }
usize to_str(char* buf, usize cap, char const* v) noexcept {
    return v ? format(buf, cap, "\"%s\"", v) : format(buf, cap, "nullptr");
}
usize to_str(char* buf, usize cap, void const* v) noexcept { return format(buf, cap, "%p", v); }

namespace {

void test_log_sink(void*, LogLevel level, StrView category, StrView message) {
    if (level < LogLevel::Warn) return;
    std::fprintf(stderr, "  [%s] %.*s: %.*s\n", log_level_name(level), KILN_SV(category), KILN_SV(message));
}

void test_panic(void*, char const* file, int line, char const* msg) {
    std::fprintf(stderr, "  PANIC %s(%d): %s\n", file, line, msg);
    std::fflush(stderr);
    std::abort();
}

bool dir_usable(char const* flag, char const* path, bool create) {
    std::error_code ec;
    if (create) std::filesystem::create_directories(path, ec);
    if (std::filesystem::is_directory(path, ec)) return true;
    std::fprintf(stderr, "kiln_tests: %s directory '%s' does not exist\n", flag, path);
    return false;
}

bool matches(TestCase const* tc, char const* filter) {
    if (!filter) return true;
    char full[512];
    format(full, sizeof full, "%s.%s", tc->suite, tc->name);
    return std::strstr(full, filter) != nullptr;
}

} // namespace

int run_all(int argc, char** argv) noexcept {
    char const* filter = nullptr;
    bool list          = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0)
            list = true;
        else if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0)
            set_log_level(LogLevel::Trace);
        else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc)
            g_sampleDir = argv[++i];
        else if (std::strcmp(argv[i], "--corpus") == 0 && i + 1 < argc)
            g_corpusDir = argv[++i];
        else if (std::strcmp(argv[i], "--golden") == 0 && i + 1 < argc)
            g_goldenDir = argv[++i];
        else if (std::strcmp(argv[i], "--update-golden") == 0)
            g_updateGolden = true;
        else
            filter = argv[i];
    }
    g_filter = filter;
    set_log_sink({&test_log_sink, nullptr});
    set_panic_handler(&test_panic, nullptr);

    if (list) {
        for (TestCase* tc = g_head; tc; tc = tc->next)
            if (matches(tc, filter)) std::printf("%s.%s\n", tc->suite, tc->name);
        return 0;
    }

    int badDirs = 0; // report every bad directory, not just the first
    badDirs += !dir_usable("--samples", g_sampleDir, true);
    badDirs += !dir_usable("--corpus", g_corpusDir, false);
    badDirs += !dir_usable("--golden", g_goldenDir, false);
    if (badDirs) return 2;

    int ran = 0, failed = 0;
    for (TestCase* tc = g_head; tc; tc = tc->next) {
        if (!matches(tc, filter)) continue;
        ++ran;
        g_failures = 0;
        tc->fn();
        if (g_failures) {
            ++failed;
            std::fprintf(stderr, "[FAIL] %s.%s (%d check%s)\n", tc->suite, tc->name, g_failures,
                         g_failures == 1 ? "" : "s");
        }
    }
    std::printf("%d/%d tests passed", ran - failed, ran);
    if (filter) std::printf(" (filter: %s, %d total)", filter, g_count);
    std::printf("\n");
    std::fflush(stdout);
    return failed ? 1 : 0;
}

} // namespace kiln::test

int main(int argc, char** argv) { return kiln::test::run_all(argc, argv); }
