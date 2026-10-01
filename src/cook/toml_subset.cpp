// src/cook/toml_subset.cpp — strict TOML subset: `key = value`, `#` comments, table headers, string,
// integer, float, boolean and (project files) array values. Every file it accepts is valid TOML.
#include "toml_subset.h"

#include "kiln/cook/settings.h"

#include "kiln/log.h"

#include <charconv>
#include <cstring>

namespace kiln::cook::detail {

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t'; }
bool is_bare(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
/// Control characters TOML forbids outside strings' escapes (tab is allowed).
bool is_control(char c) { return (u8(c) < 0x20 && c != '\t') || u8(c) == 0x7F; }

struct Parser {
    StrView s;
    TomlSyntax syntax;
    Arena& arena;
    TomlDoc& out;
    DiagSink const* diag;
    StrView file;
    usize at  = 0;
    u32 line  = 1;
    u32 table = 0;

    Status fail(char const* what) {
        char where[1100];
        format(where, sizeof where, "%.*s:%u", KILN_SV(file), line);
        return diagf(diag, make_status(Code::ParseError), kDiagSidecarSyntax, Severity::Error, file, where,
                     "%s", what);
    }
    Status unsupported(char const* what) {
        char msg[128];
        format(msg, sizeof msg, "%s are not supported%s", what,
               syntax == TomlSyntax::Sidecar ? " in .kiln sidecars" : "");
        return fail(msg);
    }

    bool eof() const { return at >= s.size; }
    bool at_eol() const {
        return at >= s.size || s[at] == '\n' || (s[at] == '\r' && at + 1 < s.size && s[at + 1] == '\n');
    }
    void skip_ws() {
        while (at < s.size && is_ws(s[at]))
            ++at;
    }
    /// The characters left on this line, a bound for decoded strings and names.
    usize line_rest() const {
        usize end = at;
        while (end < s.size && s[end] != '\n')
            ++end;
        return end - at;
    }
    /// On '#': consumes the comment up to the line end.
    Status skip_comment() {
        while (!at_eol()) {
            if (is_control(s[at])) return fail("control character in a comment");
            ++at;
        }
        return kOk;
    }
    /// After a value or header: whitespace, an optional comment, then the line end.
    Status end_line(char const* what) {
        skip_ws();
        if (at < s.size && s[at] == '#') KILN_TRY(skip_comment());
        if (!at_eol()) return fail(what);
        newline();
        return kOk;
    }
    void newline() {
        if (at >= s.size) return;
        at += s[at] == '\r' ? 2u : 1u;
        ++line;
    }

    StrView bare_key() {
        usize const start = at;
        while (at < s.size && is_bare(s[at]))
            ++at;
        return s.substr(start, at - start);
    }

    Status parse_string(StrView& value);
    Status parse_number(StrView tok, TomlValue& v);
    Status parse_scalar(TomlValue& v);
    Status parse_array(TomlEntry& e);
    Status parse_header();
    Status parse_key_value();
    Status check_key_table_conflicts();
};

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

/// A "basic" or 'literal' string starting on its quote; it ends on the same line.
Status Parser::parse_string(StrView& value) {
    char const quote = s[at];
    if (at + 2 < s.size && s[at + 1] == quote && s[at + 2] == quote) return unsupported("multi-line strings");
    ++at;
    char* buf = arena.alloc_array<char>(line_rest() + 1); // decoding never grows the text
    usize n   = 0;
    for (;;) {
        if (at >= s.size || s[at] == '\n' || s[at] == '\r') return fail("unterminated string");
        char const c = s[at++];
        if (c == quote) break;
        if (is_control(c)) return fail("control character in a string");
        if (c != '\\' || quote == '\'') {
            buf[n++] = c;
            continue;
        }
        if (at >= s.size) return fail("unterminated escape");
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
            if (at + digits > s.size) return fail("short unicode escape");
            u32 cp       = 0;
            auto const r = std::from_chars(s.data + at, s.data + at + digits, cp, 16);
            if (r.ptr != s.data + at + digits || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return fail("invalid unicode escape");
            at += digits;
            n += put_utf8(buf + n, cp);
            break;
        }
        default: return fail("unknown escape in a string");
        }
    }
    buf[n] = '\0';
    value  = StrView(buf, n);
    return kOk;
}

/// Decimal integer or float in TOML's grammar, without underscores, hex, inf or nan.
Status Parser::parse_number(StrView tok, TomlValue& v) {
    usize i = 0;
    if (i < tok.size && (tok[i] == '+' || tok[i] == '-')) ++i;
    usize const intStart = i;
    while (i < tok.size && is_digit(tok[i]))
        ++i;
    usize const intLen = i - intStart;
    if (intLen == 0) return fail("unsupported value (expected a string, number, true or false)");
    if (intLen > 1 && tok[intStart] == '0') return fail("leading zeros are not allowed");
    bool isFloat = false;
    if (i < tok.size && tok[i] == '.') {
        isFloat = true;
        ++i;
        usize const frac = i;
        while (i < tok.size && is_digit(tok[i]))
            ++i;
        if (i == frac) return fail("a float needs digits after '.'");
    }
    if (i < tok.size && (tok[i] == 'e' || tok[i] == 'E')) {
        isFloat = true;
        ++i;
        if (i < tok.size && (tok[i] == '+' || tok[i] == '-')) ++i;
        usize const exp = i;
        while (i < tok.size && is_digit(tok[i]))
            ++i;
        if (i == exp) return fail("a float needs digits in its exponent");
    }
    if (i != tok.size) return fail("unsupported value (expected a string, number, true or false)");

    char const* const first = tok.data + (tok[0] == '+' ? 1 : 0);
    char const* const last  = tok.data + tok.size;
    if (isFloat) {
        v.type       = TomlType::Float;
        auto const r = std::from_chars(first, last, v.f);
        if (r.ec != std::errc{} || r.ptr != last) return fail("float out of range");
    } else {
        v.type       = TomlType::Int;
        auto const r = std::from_chars(first, last, v.i);
        if (r.ec != std::errc{} || r.ptr != last) return fail("integer out of range");
    }
    return kOk;
}

Status Parser::parse_scalar(TomlValue& v) {
    if (at_eol() || s[at] == '#') return fail("missing value");
    char const c = s[at];
    if (c == '"' || c == '\'') {
        v.type = TomlType::String;
        return parse_string(v.str);
    }
    if (c == '{') return unsupported("inline tables");
    usize const start = at;
    while (at < s.size && !is_ws(s[at]) && s[at] != '#' && s[at] != ',' && s[at] != ']' && s[at] != '\n' &&
           s[at] != '\r')
        ++at;
    StrView const tok = s.substr(start, at - start);
    if (tok == "true" || tok == "false") {
        v.type = TomlType::Bool;
        v.b    = tok == "true";
        return kOk;
    }
    return parse_number(tok, v);
}

/// `[a, b, ...]` of scalars of one type; it may span lines, with comments and a trailing comma.
Status Parser::parse_array(TomlEntry& e) {
    ++at; // '['
    Vec<TomlValue> items(out.entries.allocator(), Tag::Cook);
    auto skip_gaps = [&]() -> Status {
        for (;;) {
            skip_ws();
            if (at < s.size && s[at] == '#') KILN_TRY(skip_comment());
            if (at >= s.size || !at_eol()) return kOk;
            newline();
        }
    };
    for (;;) {
        KILN_TRY(skip_gaps());
        if (at >= s.size) return fail("unterminated array");
        if (s[at] == ']') break;
        if (s[at] == '[') return unsupported("nested arrays");
        TomlValue v;
        KILN_TRY(parse_scalar(v));
        if (!items.empty() && v.type != items[0].type) return fail("an array must hold values of one type");
        items.push_back(v);
        KILN_TRY(skip_gaps());
        if (at < s.size && s[at] == ',') {
            ++at;
            continue;
        }
        if (at < s.size && s[at] == ']') break;
        return fail("expected ',' or ']' in an array");
    }
    ++at; // ']'
    e.type = TomlType::Array;
    if (!items.empty()) {
        TomlValue* copy = arena.alloc_array<TomlValue>(items.size());
        std::memcpy(copy, items.data(), items.size() * sizeof(TomlValue));
        e.items = Span<TomlValue const>(copy, items.size());
    }
    return kOk;
}

/// "a.b" is a strict prefix of "a.b.c" (not of "a.bc").
bool is_strict_prefix(StrView prefix, StrView name) {
    return name.size > prefix.size && name.starts_with(prefix) && name[prefix.size] == '.';
}

Status Parser::parse_header() {
    u32 const headerLine = line;
    ++at; // '['
    bool const array = at < s.size && s[at] == '[';
    if (array) {
        if (syntax == TomlSyntax::Sidecar) return unsupported("arrays of tables");
        ++at;
    }
    char* buf = arena.alloc_array<char>(line_rest() + 1);
    usize n   = 0;
    for (;;) {
        skip_ws();
        StrView seg;
        if (at < s.size && (s[at] == '"' || s[at] == '\'')) {
            if (syntax == TomlSyntax::Sidecar) return unsupported("quoted keys");
            KILN_TRY(parse_string(seg));
            if (seg.empty() || seg.find('.') != StrView::kNpos)
                return fail("a quoted table name may not be empty or contain '.'");
        } else {
            seg = bare_key();
            if (seg.empty()) return fail("empty or invalid table name");
        }
        if (n) buf[n++] = '.';
        std::memcpy(buf + n, seg.data, seg.size);
        n += seg.size;
        skip_ws();
        if (at < s.size && s[at] == '.') {
            ++at;
            continue;
        }
        if (at < s.size && s[at] == ']') break;
        return fail("expected ']' after the table name");
    }
    ++at;
    if (array) {
        if (at >= s.size || s[at] != ']') return fail("expected ']]' after the table name");
        ++at;
    }
    buf[n] = '\0';
    StrView const name(buf, n);

    for (TomlTable const& t : out.tables) {
        if (t.array && is_strict_prefix(t.name, name)) return unsupported("tables inside an array of tables");
        if (t.name == name && (!array || !t.array)) return fail("table defined twice");
        if (array && is_strict_prefix(name, t.name)) return fail("an array of tables would replace a table");
    }
    KILN_TRY(end_line("unexpected text after the table header"));
    table = u32(out.tables.size());
    out.tables.push_back(TomlTable{name, array, headerLine});
    return kOk;
}

Status Parser::parse_key_value() {
    if (s[at] == '"' || s[at] == '\'') return unsupported("quoted keys");
    TomlEntry e;
    e.table   = table;
    e.section = out.tables[table].name;
    e.line    = line;
    e.key     = bare_key();
    if (e.key.empty()) return fail("expected a key");
    skip_ws();
    if (at < s.size && s[at] == '.') return unsupported("dotted keys");
    if (at >= s.size || s[at] != '=') return fail("expected '=' after the key");
    ++at;
    skip_ws();
    if (at < s.size && s[at] == '[') {
        if (syntax == TomlSyntax::Sidecar) return unsupported("arrays");
        KILN_TRY(parse_array(e));
    } else {
        KILN_TRY(parse_scalar(e));
    }
    for (TomlEntry const& seen : out.entries)
        if (seen.table == e.table && seen.key == e.key) {
            line = e.line;
            return fail("key defined twice");
        }
    out.entries.push_back(e);
    return end_line("unexpected text after the value");
}

/// TOML forbids a key and a table with the same path: `a = 1` and `[a]`, or `[a] b = 1` and `[a.b.c]`.
Status Parser::check_key_table_conflicts() {
    char path[1024];
    for (TomlEntry const& e : out.entries) {
        usize const n = e.section.empty()
                            ? format(path, sizeof path, "%.*s", KILN_SV(e.key))
                            : format(path, sizeof path, "%.*s.%.*s", KILN_SV(e.section), KILN_SV(e.key));
        if (n >= sizeof path - 1) continue; // longer than any table name the check could match
        StrView const p(path, n);
        for (TomlTable const& t : out.tables)
            if (t.name == p || is_strict_prefix(p, t.name)) {
                line = e.line;
                return fail("a key and a table share a name");
            }
    }
    return kOk;
}

} // namespace

Status parse_toml_subset(StrView text, TomlSyntax syntax, Arena& arena, TomlDoc& out, DiagSink const* diag,
                         StrView file) {
    if (text.size >= 3 && u8(text[0]) == 0xEF && u8(text[1]) == 0xBB && u8(text[2]) == 0xBF)
        text = text.substr(3);
    out.tables.clear();
    out.entries.clear();
    out.tables.push_back(TomlTable{});
    Parser ps{text, syntax, arena, out, diag, file};
    while (!ps.eof()) {
        ps.skip_ws();
        if (ps.at_eol()) {
            ps.newline();
            continue;
        }
        char const c = ps.s[ps.at];
        if (c == '#') {
            KILN_TRY(ps.skip_comment());
            ps.newline();
        } else if (c == '[') {
            KILN_TRY(ps.parse_header());
        } else {
            KILN_TRY(ps.parse_key_value());
        }
    }
    return ps.check_key_table_conflicts();
}

} // namespace kiln::cook::detail
