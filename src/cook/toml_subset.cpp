// src/cook/toml_subset.cpp — strict TOML subset: `key = value`, `#` comments, `[a.b]` headers,
// string, integer, float and boolean values. Every file it accepts is valid TOML.
#include "toml_subset.h"

#include "kiln/cook/settings.h"

#include "kiln/log.h"

#include <charconv>

namespace kiln::cook::detail {

namespace {

struct Parser {
    Arena& arena;
    Vec<TomlEntry>& out;
    DiagSink const* diag;
    StrView file;
    u32 line = 0;

    Status fail(char const* what) const {
        char where[1100];
        format(where, sizeof where, "%.*s:%u", KILN_SV(file), line);
        return diagf(diag, make_status(Code::ParseError), kDiagSidecarSyntax, Severity::Error, file, where,
                     "%s", what);
    }
};

bool is_ws(char c) { return c == ' ' || c == '\t'; }
bool is_bare(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }

void skip_ws(StrView s, usize& at) {
    while (at < s.size && is_ws(s[at]))
        ++at;
}

/// After a value or header: only whitespace and a comment may follow.
bool rest_is_empty(StrView s, usize at) {
    skip_ws(s, at);
    return at == s.size || s[at] == '#';
}

StrView bare_key(StrView s, usize& at) {
    usize const start = at;
    while (at < s.size && is_bare(s[at]))
        ++at;
    return s.substr(start, at - start);
}

usize put_utf8(char* p, u32 cp) {
    if (cp < 0x80) {
        p[0] = char(cp);
        return 1;
    }
    if (cp < 0x800) {
        p[0] = char(0xC0 | (cp >> 6));
        p[1] = char(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        p[0] = char(0xE0 | (cp >> 12));
        p[1] = char(0x80 | ((cp >> 6) & 0x3F));
        p[2] = char(0x80 | (cp & 0x3F));
        return 3;
    }
    p[0] = char(0xF0 | (cp >> 18));
    p[1] = char(0x80 | ((cp >> 12) & 0x3F));
    p[2] = char(0x80 | ((cp >> 6) & 0x3F));
    p[3] = char(0x80 | (cp & 0x3F));
    return 4;
}

/// A "basic" or 'literal' string starting at `at` (on the quote).
Status parse_string(Parser& ps, StrView s, usize& at, StrView& value) {
    char const quote = s[at];
    if (at + 2 < s.size && s[at + 1] == quote && s[at + 2] == quote)
        return ps.fail("multi-line strings are not supported in .kiln files");
    ++at;
    char* buf = ps.arena.alloc_array<char>(s.size - at + 1);
    usize n   = 0;
    for (;;) {
        if (at >= s.size) return ps.fail("unterminated string");
        char const c = s[at++];
        if (c == quote) break;
        if (u8(c) < 0x20 && c != '\t') return ps.fail("control character in a string");
        if (c != '\\' || quote == '\'') {
            buf[n++] = c;
            continue;
        }
        if (at >= s.size) return ps.fail("unterminated escape");
        char const e = s[at++];
        switch (e) {
        case 'b': buf[n++] = '\b'; break;
        case 't': buf[n++] = '\t'; break;
        case 'n': buf[n++] = '\n'; break;
        case 'f': buf[n++] = '\f'; break;
        case 'r': buf[n++] = '\r'; break;
        case '"': buf[n++] = '"'; break;
        case '\\': buf[n++] = '\\'; break;
        case 'u':
        case 'U': {
            usize const digits = e == 'u' ? 4 : 8;
            if (at + digits > s.size) return ps.fail("short unicode escape");
            u32 cp       = 0;
            auto const r = std::from_chars(s.data + at, s.data + at + digits, cp, 16);
            if (r.ptr != s.data + at + digits || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return ps.fail("invalid unicode escape");
            at += digits;
            n += put_utf8(buf + n, cp);
            break;
        }
        default: return ps.fail("unknown escape in a string");
        }
    }
    buf[n] = '\0';
    value  = StrView(buf, n);
    return kOk;
}

/// Decimal integer or float in TOML's grammar, without underscores, hex, inf or nan.
Status parse_number(Parser& ps, StrView tok, TomlEntry& e) {
    usize at = 0;
    if (at < tok.size && (tok[at] == '+' || tok[at] == '-')) ++at;
    usize const intStart = at;
    while (at < tok.size && is_digit(tok[at]))
        ++at;
    usize const intLen = at - intStart;
    if (intLen == 0) return ps.fail("unsupported value (expected a string, number, true or false)");
    if (intLen > 1 && tok[intStart] == '0') return ps.fail("leading zeros are not allowed");
    bool isFloat = false;
    if (at < tok.size && tok[at] == '.') {
        isFloat = true;
        ++at;
        usize const frac = at;
        while (at < tok.size && is_digit(tok[at]))
            ++at;
        if (at == frac) return ps.fail("a float needs digits after '.'");
    }
    if (at < tok.size && (tok[at] == 'e' || tok[at] == 'E')) {
        isFloat = true;
        ++at;
        if (at < tok.size && (tok[at] == '+' || tok[at] == '-')) ++at;
        usize const exp = at;
        while (at < tok.size && is_digit(tok[at]))
            ++at;
        if (at == exp) return ps.fail("a float needs digits in its exponent");
    }
    if (at != tok.size) return ps.fail("unsupported value (expected a string, number, true or false)");

    char const* const first = tok.data + (tok[0] == '+' ? 1 : 0);
    char const* const last  = tok.data + tok.size;
    if (isFloat) {
        e.type       = TomlType::Float;
        auto const r = std::from_chars(first, last, e.f);
        if (r.ec != std::errc{} || r.ptr != last) return ps.fail("float out of range");
    } else {
        e.type       = TomlType::Int;
        auto const r = std::from_chars(first, last, e.i);
        if (r.ec != std::errc{} || r.ptr != last) return ps.fail("integer out of range");
    }
    return kOk;
}

Status parse_value(Parser& ps, StrView s, usize& at, TomlEntry& e) {
    if (at >= s.size || s[at] == '#') return ps.fail("missing value after '='");
    char const c = s[at];
    if (c == '"' || c == '\'') {
        e.type = TomlType::String;
        return parse_string(ps, s, at, e.str);
    }
    if (c == '[') return ps.fail("arrays are not supported in .kiln files");
    if (c == '{') return ps.fail("inline tables are not supported in .kiln files");
    usize const start = at;
    while (at < s.size && !is_ws(s[at]) && s[at] != '#')
        ++at;
    StrView const tok = s.substr(start, at - start);
    if (tok == "true" || tok == "false") {
        e.type = TomlType::Bool;
        e.b    = tok == "true";
        return kOk;
    }
    return parse_number(ps, tok, e);
}

Status parse_header(Parser& ps, StrView s, usize at, StrView& section) {
    ++at; // '['
    if (at < s.size && s[at] == '[') return ps.fail("arrays of tables are not supported in .kiln files");
    char* buf = ps.arena.alloc_array<char>(s.size + 1);
    usize n   = 0;
    for (;;) {
        skip_ws(s, at);
        if (at < s.size && (s[at] == '"' || s[at] == '\''))
            return ps.fail("quoted keys are not supported in .kiln files");
        StrView const seg = bare_key(s, at);
        if (seg.empty()) return ps.fail("empty or invalid table name");
        if (n) buf[n++] = '.';
        std::memcpy(buf + n, seg.data, seg.size);
        n += seg.size;
        skip_ws(s, at);
        if (at < s.size && s[at] == '.') {
            ++at;
            continue;
        }
        if (at < s.size && s[at] == ']') break;
        return ps.fail("expected ']' after the table name");
    }
    ++at;
    if (!rest_is_empty(s, at)) return ps.fail("unexpected text after the table header");
    buf[n]  = '\0';
    section = StrView(buf, n);
    return kOk;
}

Status parse_line(Parser& ps, StrView s, StrView& section, Vec<StrView>& sections) {
    usize at = 0;
    skip_ws(s, at);
    if (at == s.size || s[at] == '#') return kOk;
    if (s[at] == '[') {
        KILN_TRY(parse_header(ps, s, at, section));
        for (StrView const& seen : sections)
            if (seen == section) return ps.fail("table defined twice");
        sections.push_back(section);
        return kOk;
    }
    if (s[at] == '"' || s[at] == '\'') return ps.fail("quoted keys are not supported in .kiln files");

    TomlEntry e;
    e.section = section;
    e.line    = ps.line;
    e.key     = bare_key(s, at);
    if (e.key.empty()) return ps.fail("expected a key");
    skip_ws(s, at);
    if (at < s.size && s[at] == '.') return ps.fail("dotted keys are not supported in .kiln files");
    if (at >= s.size || s[at] != '=') return ps.fail("expected '=' after the key");
    ++at;
    skip_ws(s, at);
    KILN_TRY(parse_value(ps, s, at, e));
    if (!rest_is_empty(s, at)) return ps.fail("unexpected text after the value");
    for (TomlEntry const& seen : ps.out)
        if (seen.section == e.section && seen.key == e.key) return ps.fail("key defined twice");
    ps.out.push_back(e);
    return kOk;
}

} // namespace

Status parse_toml_subset(StrView text, Arena& arena, Vec<TomlEntry>& out, DiagSink const* diag,
                         StrView file) {
    Parser ps{arena, out, diag, file};
    if (text.size >= 3 && u8(text[0]) == 0xEF && u8(text[1]) == 0xBB && u8(text[2]) == 0xBF)
        text = text.substr(3);

    Vec<StrView> sections(default_allocator(), Tag::Cook);
    StrView section;
    usize at = 0;
    while (at <= text.size) {
        ++ps.line;
        usize end = at;
        while (end < text.size && text[end] != '\n')
            ++end;
        StrView lineText = text.substr(at, end - at);
        if (!lineText.empty() && lineText[lineText.size - 1] == '\r')
            lineText = lineText.substr(0, lineText.size - 1);
        KILN_TRY(parse_line(ps, lineText, section, sections));
        if (end == text.size) break;
        at = end + 1;
    }
    return kOk;
}

} // namespace kiln::cook::detail
