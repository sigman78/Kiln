#include "kiln/core.h"
#include "kiln/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdlib>

namespace kiln {

namespace {

void default_panic(void* /*user*/, char const* file, int line, char const* msg) {
    char buf[kLogMessageMax];
    format(buf, sizeof buf, "%s(%d): %s", file, line, msg);
    LogSink sink = log_sink();
    if (sink.fn) {
        sink.fn(sink.user, LogLevel::Error, StrView("panic"), StrView(buf));
    } else {
        LogSink fallback = stderr_log_sink();
        fallback.fn(fallback.user, LogLevel::Error, StrView("panic"), StrView(buf));
    }
    KILN_DEBUGBREAK();
    std::abort();
}

struct PanicState {
    std::atomic<PanicHandler> handler{&default_panic};
    std::atomic<void*> user{nullptr};
};

PanicState& panic_state() noexcept {
    static PanicState s;
    return s;
}

} // namespace

void set_panic_handler(PanicHandler handler, void* user) noexcept {
    PanicState& s = panic_state();
    s.user.store(user, std::memory_order_relaxed);
    s.handler.store(handler ? handler : &default_panic, std::memory_order_release);
}

void panic(char const* file, int line, char const* fmt, ...) noexcept {
    char buf[kLogMessageMax];
    va_list args;
    va_start(args, fmt);
    vformat(buf, sizeof buf, fmt, args);
    va_end(args);

    PanicState& s  = panic_state();
    PanicHandler h = s.handler.load(std::memory_order_acquire);
    h(s.user.load(std::memory_order_relaxed), file, line, buf);
    // A user handler must not return; if it does, make sure we still stop.
    std::abort();
}

} // namespace kiln
