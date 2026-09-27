// Shared helpers for the KTX2 corpus tests (test_ktx2_corpus.cpp, test_ktx2_corpus_rt.cpp):
// load a whole file with C stdio and parse tests/corpus/ktx2/manifest.txt. Reader-side
// only; no kiln/cook/ includes. No iostreams: a hand-rolled parser over a Vec<char>.
#pragma once

#include "kiln_test.h"

#include "kiln/containers.h"
#include "kiln/formats.h"
#include "kiln/ktx2.h"

#include <cstdio>

namespace kiln::test::corpus {

/// One manifest line. StrViews point into the Manifest's text buffer.
struct Entry {
    u32 line = 0;
    StrView path;
    bool expectOk = false;
    u32 code      = 0; ///< expected K41xx diagnostic when !expectOk
    StrView formatName;
    Format format        = Format::Undefined; ///< Undefined for "-"
    u32 width            = 0;
    u32 height           = 0;
    u32 depth            = 0; ///< header pixelDepth
    u32 layers           = 0; ///< header layerCount
    u32 faces            = 0;
    u32 levels           = 0; ///< header levelCount
    u32 supercompression = 0;
    StrView keys; ///< comma-separated, empty for "-"
};

struct Manifest {
    Vec<char> text{default_allocator(), Tag::Test};
    Vec<Entry> entries{default_allocator(), Tag::Test};
};

/// Read the whole file at `path` into `out`. Returns false if it cannot be opened or read.
inline bool read_file(char const* path, Vec<u8>& out) noexcept {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    bool ok         = std::fseek(f, 0, SEEK_END) == 0;
    long const size = ok ? std::ftell(f) : -1;
    ok              = ok && size >= 0 && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(usize(size));
        ok = size == 0 || std::fread(out.data(), 1, usize(size), f) == usize(size);
    }
    std::fclose(f);
    return ok;
}

inline bool parse_u32(StrView s, u32& out) noexcept {
    if (s.empty() || s.size > 10) return false;
    u64 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + u64(c - '0');
    }
    if (v > 0xFFFFFFFFull) return false;
    out = u32(v);
    return true;
}

inline Format format_by_name(StrView name) noexcept {
    for (FormatInfo const& e : detail::kFormatTable)
        if (StrView(e.name) == name) return e.format;
    return Format::Undefined;
}

/// Call fn(user, item) for each comma-separated item of `list` (empty items skipped).
template <class Fn> void for_each_item(StrView list, Fn&& fn) noexcept {
    usize pos = 0;
    while (pos < list.size) {
        usize comma = list.find(',', pos);
        if (comma == StrView::kNpos) comma = list.size;
        if (comma > pos) fn(list.substr(pos, comma - pos));
        pos = comma + 1;
    }
}

/// Parse one non-comment line into `e`. Returns an error message or nullptr.
inline char const* parse_line(StrView line, Entry& e) noexcept {
    StrView fields[12];
    usize n   = 0;
    usize pos = 0;
    for (;;) {
        usize bar = line.find('|', pos);
        StrView f = line.substr(pos, (bar == StrView::kNpos ? line.size : bar) - pos);
        if (n == 12) return "more than 12 fields";
        fields[n++] = f;
        if (bar == StrView::kNpos) break;
        pos = bar + 1;
    }
    if (n != 12) return "expected 12 '|'-separated fields";
    e.path = fields[0];
    if (fields[1] == "ok")
        e.expectOk = true;
    else if (fields[1] == "unsupported")
        e.expectOk = false;
    else
        return "expect must be 'ok' or 'unsupported'";
    if (!parse_u32(fields[2], e.code)) return "bad code";
    if (e.expectOk != (e.code == 0)) return "code must be 0 exactly when expect is ok";
    e.formatName = fields[3];
    if (e.formatName != "-") {
        e.format = format_by_name(e.formatName);
        if (e.format == Format::Undefined) return "unknown format name";
    }
    u32* const nums[] = {&e.width, &e.height, &e.depth, &e.layers, &e.faces, &e.levels, &e.supercompression};
    for (usize i = 0; i < countof(nums); ++i)
        if (!parse_u32(fields[4 + i], *nums[i])) return "bad number";
    e.keys = fields[11] == "-" ? StrView() : fields[11];
    return nullptr;
}

/// Load and parse `<dir>/manifest.txt`. Reports failures through KILN_CHECK_MSG and
/// returns false if the manifest could not be read or has a malformed line.
inline bool load_manifest(char const* dir, Manifest& m) noexcept {
    char path[1024];
    format(path, sizeof path, "%s/manifest.txt", dir);
    Vec<u8> raw(default_allocator(), Tag::Test);
    if (!KILN_CHECK_MSG(read_file(path, raw), "cannot read %s", path)) return false;
    m.text.resize(raw.size());
    if (!raw.empty()) std::memcpy(m.text.data(), raw.data(), raw.size());

    StrView const all(m.text.data(), m.text.size());
    bool ok    = true;
    usize pos  = 0;
    u32 lineNo = 0;
    while (pos < all.size) {
        usize eol = all.find('\n', pos);
        if (eol == StrView::kNpos) eol = all.size;
        StrView line = all.substr(pos, eol - pos);
        pos          = eol + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size - 1);
        if (line.empty() || line.front() == '#') continue;
        Entry e;
        e.line          = lineNo;
        char const* err = parse_line(line, e);
        if (!KILN_CHECK_MSG(err == nullptr, "%s:%u: %s: %.*s", path, lineNo, err ? err : "", KILN_SV(line)))
            ok = false;
        else
            m.entries.push_back(e);
    }
    if (!ok) return false;
    return KILN_CHECK_MSG(!m.entries.empty(), "%s: no entries", path);
}

/// Captures every diagnostic; keeps the first error's code and message.
struct DiagCapture {
    u32 code      = 0;
    int count     = 0;
    int errors    = 0;
    char msg[512] = {};

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagCapture*>(user);
        ++self->count;
        if (d.severity == Severity::Error && self->errors++ == 0) self->code = d.code;
        if (self->count == 1)
            format(self->msg, sizeof self->msg, "K%u %.*s: %.*s", d.code, KILN_SV(d.where),
                   KILN_SV(d.message));
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

inline bool bytes_equal(Span<u8 const> a, Span<u8 const> b) noexcept {
    return a.size == b.size && (a.size == 0 || std::memcmp(a.data, b.data, a.size) == 0);
}

} // namespace kiln::test::corpus
