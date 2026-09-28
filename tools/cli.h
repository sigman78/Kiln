// tools/cli.h — option-table argument parser shared by the kiln tools and examples.
// Not part of the library and never installed. No allocation, no exceptions; errors
// are one line on stderr and a false result, so the caller keeps its exit codes.
#pragma once

#include "kiln/core.h"
#include "kiln/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kiln::cli {

/// One option. Exactly one of flag / str / number / real / each receives the value. A value
/// is given as "--name value" or "--name=value".
struct Option {
    char const* name;                     ///< "--threads"
    char const* alt            = nullptr; ///< alias, e.g. "-o"
    char const* arg            = nullptr; ///< "<n>": present means the option takes a value
    char const* help           = "";
    bool* flag                 = nullptr;     ///< set to true when present
    char const** str           = nullptr;     ///< the value as given (or the matching choice)
    u32* number                = nullptr;     ///< decimal, 0..max
    double* real               = nullptr;     ///< decimal floating point
    u32 max                    = 0xFFFFFFFFu; ///< upper bound for `number`
    char const* const* choices = nullptr;     ///< null-terminated; the value must match one
    /// Receives the value each time the option is given; false rejects it.
    bool (*each)(void* user, char const* value) = nullptr;
    void* user                                  = nullptr;
};

struct Spec {
    char const* program;  ///< "kiln-cook", used in messages
    char const* synopsis; ///< "<input>... [options]"
    Span<Option const> options;
    char const* footer = nullptr; ///< printed after the options, e.g. exit codes
    /// Receives each argument that is not an option, in order. Returns false to reject
    /// it. Null means positional arguments are an error.
    bool (*positional)(void* user, char const* arg) = nullptr;
    void* user                                      = nullptr;
};

struct Result {
    bool ok         = false; ///< every argument was accepted
    bool help       = false; ///< --help or -h was given; usage went to stdout
    u32 positionals = 0;
};

inline void usage(Spec const& spec, std::FILE* out) {
    std::fprintf(out, "usage: %s %s\n", spec.program, spec.synopsis);
    usize width = 0;
    for (Option const& o : spec.options) {
        usize w =
            std::strlen(o.name) + (o.alt ? std::strlen(o.alt) + 2 : 0) + (o.arg ? std::strlen(o.arg) + 1 : 0);
        if (w > width) width = w;
    }
    for (Option const& o : spec.options) {
        char left[96];
        usize n = format(left, sizeof left, "%s%s%s%s%s", o.name, o.alt ? ", " : "", o.alt ? o.alt : "",
                         o.arg ? " " : "", o.arg ? o.arg : "");
        std::fprintf(out, "  %s%*s  %s", left, int(width - n), "", o.help);
        if (o.choices) {
            std::fputs(" (", out);
            for (char const* const* c = o.choices; *c; ++c)
                std::fprintf(out, "%s%s", c == o.choices ? "" : "|", *c);
            std::fputs(")", out);
        }
        std::fputc('\n', out);
    }
    std::fprintf(out, "  %-*s  %s\n", int(width), "--help, -h", "show this help");
    if (spec.footer) std::fprintf(out, "%s\n", spec.footer);
}

namespace detail {

inline bool matches(Option const& o, char const* arg, usize n) {
    return (std::strlen(o.name) == n && std::memcmp(o.name, arg, n) == 0) ||
           (o.alt && std::strlen(o.alt) == n && std::memcmp(o.alt, arg, n) == 0);
}

inline bool apply(Spec const& spec, Option const& o, char const* value) {
    if (o.choices) {
        char const* const* c = o.choices;
        for (; *c; ++c)
            if (std::strcmp(*c, value) == 0) break;
        if (!*c) {
            std::fprintf(stderr, "%s: %s: '%s' is not one of", spec.program, o.name, value);
            for (c = o.choices; *c; ++c)
                std::fprintf(stderr, " %s", *c);
            std::fputc('\n', stderr);
            return false;
        }
        value = *c;
    }
    if (o.str) {
        *o.str = value;
        return true;
    }
    if (o.each) return o.each(o.user, value);
    char* end = nullptr;
    if (o.number) {
        unsigned long const v = std::strtoul(value, &end, 10);
        if (*value == '-' || end == value || *end != '\0' || v > o.max) {
            std::fprintf(stderr, "%s: %s: expected a number in 0..%u, got '%s'\n", spec.program, o.name,
                         o.max, value);
            return false;
        }
        *o.number = u32(v);
        return true;
    }
    if (o.real) {
        double const v = std::strtod(value, &end);
        if (end == value || *end != '\0') {
            std::fprintf(stderr, "%s: %s: expected a number, got '%s'\n", spec.program, o.name, value);
            return false;
        }
        *o.real = v;
        return true;
    }
    KILN_PANIC("cli option with a value but no destination");
}

} // namespace detail

/// Parses argv[1..]. "--" ends option parsing; everything after it is positional.
inline Result parse(Spec const& spec, int argc, char** argv) {
    Result r;
    bool optionsDone = false;
    for (int i = 1; i < argc; ++i) {
        char const* a = argv[i];
        if (optionsDone || a[0] != '-' || a[1] == '\0') {
            if (!spec.positional) {
                std::fprintf(stderr, "%s: unexpected argument '%s'\n", spec.program, a);
                return r;
            }
            if (!spec.positional(spec.user, a)) return r;
            ++r.positionals;
            continue;
        }
        if (std::strcmp(a, "--") == 0) {
            optionsDone = true;
            continue;
        }
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage(spec, stdout);
            r.help = true;
            return r;
        }
        char const* eq      = std::strchr(a, '=');
        usize const n       = eq ? usize(eq - a) : std::strlen(a);
        Option const* match = nullptr;
        for (Option const& o : spec.options)
            if (detail::matches(o, a, n)) {
                match = &o;
                break;
            }
        if (!match) {
            std::fprintf(stderr, "%s: unknown option '%.*s'\n", spec.program, int(n), a);
            return r;
        }
        if (!match->arg) {
            if (eq) {
                std::fprintf(stderr, "%s: %s takes no value\n", spec.program, match->name);
                return r;
            }
            KILN_VERIFY(match->flag != nullptr);
            *match->flag = true;
            continue;
        }
        char const* value = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : nullptr);
        if (!value) {
            std::fprintf(stderr, "%s: %s needs a value %s\n", spec.program, match->name, match->arg);
            return r;
        }
        if (!detail::apply(spec, *match, value)) return r;
    }
    r.ok = true;
    return r;
}

} // namespace kiln::cli
