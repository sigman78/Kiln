// kiln/log.h — user-supplied log sink, log level and a small printf-style formatter.
#pragma once

#include "kiln/core.h"

#include <cstdarg> // va_list for vlog/vformat

namespace kiln {

enum class LogLevel : u8 { Trace = 0, Debug, Info, Warn, Error, Off };

[[nodiscard]] KILN_API char const* log_level_name(LogLevel l) noexcept;

/// The sink receives fully formatted, null-terminated messages (message.data[message.size] == '\0').
/// `category` is a short static string such as "io", "cook", "registry".
struct LogSink {
    void (*fn)(void* user, LogLevel level, StrView category, StrView message) = nullptr;
    void* user                                                                = nullptr;
};

/// Install the process-wide sink. Null fn disables logging. Not synchronized with
/// concurrent log() calls: set it once during startup.
KILN_API void set_log_sink(LogSink sink) noexcept;
[[nodiscard]] KILN_API LogSink log_sink() noexcept;

/// Messages below this level are dropped before formatting. Default: Info.
KILN_API void set_log_level(LogLevel level) noexcept;
[[nodiscard]] KILN_API LogLevel log_level() noexcept;

/// The built-in sink: writes "[level] category: message\n" to stderr via fputs.
[[nodiscard]] KILN_API LogSink stderr_log_sink() noexcept;

/// Format and deliver a message. Messages are truncated to an internal buffer
/// (kLogMessageMax bytes including terminator). Never allocates.
KILN_API void log(LogLevel level, char const* category, char const* fmt, ...) noexcept KILN_PRINTF(3, 4);
KILN_API void vlog(LogLevel level, char const* category, char const* fmt, va_list args) noexcept
    KILN_PRINTF(3, 0);

inline constexpr usize kLogMessageMax = 1024;

/// snprintf wrappers with kiln's conventions: always null-terminate when cap > 0,
/// return the number of characters written (excluding terminator), clamped to cap-1.
KILN_API usize format(char* buf, usize cap, char const* fmt, ...) noexcept KILN_PRINTF(3, 4);
KILN_API usize vformat(char* buf, usize cap, char const* fmt, va_list args) noexcept KILN_PRINTF(3, 0);

/// Convenience for StrView arguments in printf formats: `"%.*s", KILN_SV(view)`.
#define KILN_SV(sv) int((sv).size), (sv).data

} // namespace kiln

#define KILN_LOG(level, category, ...)                                                                       \
    do {                                                                                                     \
        if ((level) >= ::kiln::log_level()) ::kiln::log((level), (category), __VA_ARGS__);                   \
    } while (0)

#define KILN_TRACE(category, ...) KILN_LOG(::kiln::LogLevel::Trace, category, __VA_ARGS__)
#define KILN_DEBUG_LOG(category, ...) KILN_LOG(::kiln::LogLevel::Debug, category, __VA_ARGS__)
#define KILN_INFO(category, ...) KILN_LOG(::kiln::LogLevel::Info, category, __VA_ARGS__)
#define KILN_WARN(category, ...) KILN_LOG(::kiln::LogLevel::Warn, category, __VA_ARGS__)
#define KILN_ERROR(category, ...) KILN_LOG(::kiln::LogLevel::Error, category, __VA_ARGS__)
