#include "kiln/result.h"
#include "kiln/log.h"

#include <cstdarg>

namespace kiln {

char const* code_name(Code c) {
    switch (c) {
    case Code::Ok: return "ok";
    case Code::Unknown: return "unknown";
    case Code::InvalidArgument: return "invalid_argument";
    case Code::OutOfMemory: return "out_of_memory";
    case Code::NotFound: return "not_found";
    case Code::AlreadyExists: return "already_exists";
    case Code::Unsupported: return "unsupported";
    case Code::IoError: return "io_error";
    case Code::IoEof: return "io_eof";
    case Code::ParseError: return "parse_error";
    case Code::ValidationFailed: return "validation_failed";
    case Code::Corrupt: return "corrupt";
    case Code::VersionMismatch: return "version_mismatch";
    case Code::Busy: return "busy";
    case Code::NotReady: return "not_ready";
    case Code::Cancelled: return "cancelled";
    case Code::Timeout: return "timeout";
    case Code::Internal: return "internal";
    case Code::Count: break;
    }
    return "?";
}

char const* severity_name(Severity s) {
    switch (s) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return "?";
}

Status diagf(DiagSink const* sink, Status status, u32 code, Severity severity, StrView asset, StrView where,
             char const* fmt, ...) {
    if (!sink || !sink->fn) return status;
    char buf[kLogMessageMax];
    va_list args;
    va_start(args, fmt);
    usize n = vformat(buf, sizeof buf, fmt, args);
    va_end(args);
    Diagnostic d{};
    d.code     = code;
    d.severity = severity;
    d.status   = status;
    d.asset    = asset;
    d.where    = where;
    d.message  = StrView(buf, n);
    sink->fn(sink->user, d);
    return status;
}

namespace {
void log_diag_fn(void* /*user*/, Diagnostic const& d) {
    LogLevel level = d.severity == Severity::Error     ? LogLevel::Error
                     : d.severity == Severity::Warning ? LogLevel::Warn
                                                       : LogLevel::Info;
    if (d.code != 0) {
        KILN_LOG(level, "diag", "K%04u %.*s%s%.*s%s%.*s (%s)", unsigned(d.code), KILN_SV(d.asset),
                 d.asset.empty() ? "" : " ", KILN_SV(d.where), d.where.empty() ? "" : ": ",
                 KILN_SV(d.message), code_name(d.status.code));
    } else {
        KILN_LOG(level, "diag", "%.*s%s%.*s%s%.*s (%s)", KILN_SV(d.asset), d.asset.empty() ? "" : " ",
                 KILN_SV(d.where), d.where.empty() ? "" : ": ", KILN_SV(d.message), code_name(d.status.code));
    }
}
} // namespace

DiagSink log_diag_sink() { return {&log_diag_fn, nullptr}; }

} // namespace kiln
