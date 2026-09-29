// store_profile.cpp — reads a store's kiln-store.txt (docs/design/target-profiles.md).
#include "kiln/assets.h"
#include "kiln/io.h"

namespace kiln {

namespace {

/// The next space-separated word of `line` from `*at`; empty at the end.
StrView next_word(StrView line, usize* at) noexcept {
    while (*at < line.size && line[*at] == ' ')
        ++*at;
    usize const start = *at;
    while (*at < line.size && line[*at] != ' ')
        ++*at;
    return line.substr(start, *at - start);
}

bool parse_u64(StrView s, u32 base, u64* out) noexcept {
    if (s.empty() || s.size > 20) return false;
    u64 v = 0;
    for (char c : s) {
        u32 d = 0;
        if (c >= '0' && c <= '9')
            d = u32(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = u32(c - 'a' + 10);
        else
            return false;
        if (!checked_mul(v, u64(base), v) || !checked_add(v, u64(d), v)) return false;
    }
    *out = v;
    return true;
}

} // namespace

Status parse_store_profile(Span<u8 const> text, StoreProfile* out) noexcept {
    StoreProfile p;
    bool header = false, name = false, hash = false, formats = false;
    StrView const all(reinterpret_cast<char const*>(text.data), text.size);
    usize pos = 0;
    while (pos < all.size) {
        usize end = pos;
        while (end < all.size && all[end] != '\n')
            ++end;
        StrView line = all.substr(pos, end - pos);
        if (!line.empty() && line[line.size - 1] == '\r') line = line.substr(0, line.size - 1);
        pos               = end + 1;
        usize at          = 0;
        StrView const key = next_word(line, &at);
        if (key.empty()) continue;
        if (key == "kiln-store") {
            header = next_word(line, &at) == "1";
        } else if (key == "profile") {
            StrView const n = next_word(line, &at);
            if (n.empty() || n.size >= sizeof p.name) return make_status(Code::ParseError);
            std::memcpy(p.name, n.data, n.size);
            name = true;
        } else if (key == "hash") {
            hash = parse_u64(next_word(line, &at), 16, &p.hash);
        } else if (key == "formats") {
            formats = true;
            for (StrView w = next_word(line, &at); !w.empty(); w = next_word(line, &at)) {
                u64 v = 0;
                if (!parse_u64(w, 10, &v) || v > 0xFFFFFFFFu || !block_format_bit(Format(u32(v))))
                    return make_status(Code::ParseError);
                p.blockFormats |= block_format_bit(Format(u32(v)));
            }
        } // other keys are left for later versions
    }
    if (!header || !name || !hash || !formats) return make_status(Code::ParseError);
    *out = p;
    return kOk;
}

Status read_store_profile(IoBackend const* io, StrView storeDir, StoreProfile* out) noexcept {
    char path[1024];
    usize const n = format(path, sizeof path, "%.*s/%s", KILN_SV(storeDir), kStoreProfileFile);
    if (n + 1 >= sizeof path) return make_status(Code::InvalidArgument);
    IoBackend const* const b = io ? io : compat_io_backend();
    IoFile f{};
    KILN_TRY(b->open(b->user, StrView(path, n), &f));
    u64 size  = 0;
    Status st = b->size(b->user, f, &size);
    if (st.ok() && size > 4096) st = make_status(Code::ParseError); // a descriptor is a few lines
    u8 buf[4096];
    if (st.ok()) st = b->read_range(b->user, f, 0, size, buf);
    b->close(b->user, f);
    if (st.failed()) return st;
    return parse_store_profile(Span<u8 const>(buf, usize(size)), out);
}

} // namespace kiln
