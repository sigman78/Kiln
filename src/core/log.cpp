#include "kiln/log.h"

#include <atomic>
#include <cstdio>

namespace kiln {

char const* log_level_name(LogLevel l) {
    switch (l) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    case LogLevel::Off: return "off";
    }
    return "?";
}

namespace {

void stderr_sink_fn(void* /*user*/, LogLevel level, StrView category, StrView message) {
    char line[kLogMessageMax + 64];
    format(line, sizeof line, "[%s] %.*s: %.*s\n", log_level_name(level), KILN_SV(category),
           KILN_SV(message));
    std::fputs(line, stderr);
}

struct LogState {
    std::atomic<decltype(LogSink::fn)> fn{&stderr_sink_fn};
    std::atomic<void*> user{nullptr};
    std::atomic<LogLevel> level{LogLevel::Info};
};

LogState& state() {
    static LogState s;
    return s;
}

} // namespace

LogSink stderr_log_sink() { return {&stderr_sink_fn, nullptr}; }

void set_log_sink(LogSink sink) {
    LogState& s = state();
    s.user.store(sink.user, std::memory_order_relaxed);
    s.fn.store(sink.fn, std::memory_order_release);
}

LogSink log_sink() {
    LogState& s = state();
    return {s.fn.load(std::memory_order_acquire), s.user.load(std::memory_order_relaxed)};
}

void set_log_level(LogLevel level) { state().level.store(level, std::memory_order_relaxed); }

LogLevel log_level() { return state().level.load(std::memory_order_relaxed); }

usize vformat(char* buf, usize cap, char const* fmt, va_list args) {
    if (cap == 0) return 0;
    int n = std::vsnprintf(buf, cap, fmt, args);
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return usize(n) < cap ? usize(n) : cap - 1;
}

usize format(char* buf, usize cap, char const* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    usize n = vformat(buf, cap, fmt, args);
    va_end(args);
    return n;
}

void vlog(LogLevel level, char const* category, char const* fmt, va_list args) {
    if (level < log_level()) return;
    LogSink sink = log_sink();
    if (!sink.fn) return;
    char buf[kLogMessageMax];
    usize n = vformat(buf, sizeof buf, fmt, args);
    sink.fn(sink.user, level, StrView(category), StrView(buf, n));
}

void log(LogLevel level, char const* category, char const* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level, category, fmt, args);
    va_end(args);
}

} // namespace kiln
