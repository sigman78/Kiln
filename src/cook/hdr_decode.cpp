// src/cook/hdr_decode.cpp — kiln's own Radiance .hdr (RGBE) decoder and the half float
// conversion (docs/design/hdr-textures.md). No dependency; every read is bounds-checked.
#include "kiln/cook/image.h"

#include "kiln/log.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace kiln::cook {

namespace {

constexpr u32 kMaxDimension    = 16384;
constexpr usize kMaxHeaderLine = 4096;

Status fail(DiagSink const* diag, StrView asset, Code code, u32 k, char const* fmt, unsigned long long a = 0,
            unsigned long long b = 0) {
    return diagf(diag, make_status(code), k, Severity::Error, asset, "hdr", fmt, a, b);
}

/// The next header line (without its '\n'); false at the end of the bytes or on a line that
/// is too long.
bool next_line(Span<u8 const> bytes, usize& pos, StrView* line) {
    usize const begin = pos;
    while (pos < bytes.size && bytes[pos] != '\n') {
        if (pos - begin >= kMaxHeaderLine) return false;
        ++pos;
    }
    if (pos >= bytes.size) return false;
    *line = StrView(reinterpret_cast<char const*>(bytes.data) + begin, pos - begin);
    ++pos;
    return true;
}

/// Parses `prefix` followed by a decimal number at `at`; advances `at` past it.
bool parse_field(StrView s, usize& at, StrView prefix, u32* out) {
    if (s.substr(at).starts_with(prefix) == false) return false;
    at += prefix.size;
    u64 v    = 0;
    usize n0 = at;
    while (at < s.size && s[at] >= '0' && s[at] <= '9' && v <= kMaxDimension)
        v = v * 10 + u64(s[at++] - '0');
    if (at == n0) return false;
    *out = u32(min<u64>(v, u64(kMaxDimension) + 1));
    return true;
}

/// One RGBE texel to float RGB, as Radiance's colr_color does.
void rgbe_to_float(u8 const* p, f32* out) {
    if (p[3] == 0) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    f32 const scale = std::ldexp(1.0f, int(p[3]) - 136);
    for (u32 c = 0; c < 3; ++c)
        out[c] = (f32(p[c]) + 0.5f) * scale; // exact: a small integer plus 0.5, times a power of two
}

} // namespace

bool is_hdr(Span<u8 const> bytes) {
    auto const starts = [bytes](char const* sig) {
        usize const n = std::strlen(sig);
        return bytes.size >= n && std::memcmp(bytes.data, sig, n) == 0;
    };
    return starts("#?RADIANCE") || starts("#?RGBE");
}

Result<Image> decode_hdr(Span<u8 const> bytes, Allocator const* alloc, DiagSink const* diag, StrView asset) {
    if (!is_hdr(bytes))
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "not a Radiance .hdr file");

    // Header: lines up to an empty one. Only FORMAT matters; EXPOSURE and the rest are ignored.
    usize pos = 0;
    StrView line;
    if (!next_line(bytes, pos, &line))
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "truncated header");
    for (;;) {
        if (!next_line(bytes, pos, &line))
            return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "truncated header");
        if (line.empty()) break;
        if (line.starts_with("FORMAT=") && line != StrView("FORMAT=32-bit_rle_rgbe"))
            return fail(diag, asset, Code::Unsupported, kDiagImageUnsupported,
                        "only FORMAT=32-bit_rle_rgbe is supported (XYZE is not)");
    }

    // Resolution line: only the standard orientation, top row first.
    if (!next_line(bytes, pos, &line))
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "missing resolution line");
    u32 w = 0, h = 0;
    usize at = 0;
    // Any "<sign><axis> N <sign><axis> N" line is well-formed; only "-Y N +X N" is supported.
    char axis[4] = {};
    for (u32 k = 0; k < 2; ++k) {
        if (k == 1) {
            if (at >= line.size || line[at] != ' ')
                return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed,
                            "malformed resolution line");
            ++at;
        }
        if (at + 3 > line.size || (line[at] != '-' && line[at] != '+') ||
            (line[at + 1] != 'X' && line[at + 1] != 'Y') || line[at + 2] != ' ')
            return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "malformed resolution line");
        axis[k * 2]     = line[at];
        axis[k * 2 + 1] = line[at + 1];
        at += 3;
        if (!parse_field(line, at, {}, k == 0 ? &h : &w))
            return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "malformed resolution line");
    }
    if (at != line.size)
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "malformed resolution line");
    if (std::memcmp(axis, "-Y+X", 4) != 0)
        return fail(diag, asset, Code::Unsupported, kDiagImageUnsupported,
                    "only the standard orientation -Y H +X W is supported");
    if (w == 0 || h == 0)
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "empty image %llux%llu", w, h);
    if (w > kMaxDimension || h > kMaxDimension)
        return fail(diag, asset, Code::Unsupported, kDiagImageTooLarge, "extent %llux%llu exceeds 16384", w,
                    h);
    // The smallest scanline: flat is 4 bytes per texel; run-length (8 <= w < 32768) is 4 bytes, then per
    // channel one 2-byte run per 127 texels. A header that claims more texels than the file can hold
    // fails here, before the image is allocated.
    u64 const flatBytes = u64(w) * 4;
    u64 const rleBytes  = 4 + 4 * 2 * ((u64(w) + 126) / 127);
    u64 const minLine   = w >= 8 && w < 32768 ? min(flatBytes, rleBytes) : flatBytes;
    if (u64(bytes.size - pos) < minLine * h)
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "truncated pixel data");

    Allocator const* const a = alloc ? alloc : default_allocator();
    Image img;
    img.width          = w;
    img.height         = h;
    img.channels       = 3;
    img.bitsPerChannel = 32;
    img.pixels.init(a, Tag::Cook);
    img.pixels.resize(usize(img.byte_size()));
    Vec<u8> rgbe(a, Tag::Cook);
    rgbe.resize(usize(w) * 4);

    auto const truncated = [&]() {
        return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed, "truncated pixel data");
    };
    for (u32 y = 0; y < h; ++y) {
        // A run-length scanline starts with 2, 2 and the width; anything else is a flat scanline.
        bool const rle = w >= 8 && w < 32768 && bytes.size - pos >= 4 && bytes[pos] == 2 &&
                         bytes[pos + 1] == 2 && (bytes[pos + 2] & 0x80) == 0;
        if (rle) {
            if ((u32(bytes[pos + 2]) << 8 | bytes[pos + 3]) != w)
                return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed,
                            "scanline %llu: run-length width does not match the image", y);
            pos += 4;
            for (u32 c = 0; c < 4; ++c) {
                for (u32 x = 0; x < w;) {
                    if (pos >= bytes.size) return truncated();
                    u32 count = bytes[pos++];
                    if (count > 128) {
                        count -= 128;
                        if (x + count > w)
                            return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed,
                                        "scanline %llu: run overflows the scanline", y);
                        if (pos >= bytes.size) return truncated();
                        u8 const v = bytes[pos++];
                        for (u32 i = 0; i < count; ++i)
                            rgbe[usize(x++) * 4 + c] = v;
                    } else {
                        if (count == 0 || x + count > w)
                            return fail(diag, asset, Code::ParseError, kDiagImageDecodeFailed,
                                        "scanline %llu: bad literal run", y);
                        if (bytes.size - pos < count) return truncated();
                        for (u32 i = 0; i < count; ++i)
                            rgbe[usize(x++) * 4 + c] = bytes[pos++];
                    }
                }
            }
        } else {
            if (bytes.size - pos < usize(w) * 4) return truncated();
            std::memcpy(rgbe.data(), bytes.data + pos, usize(w) * 4);
            pos += usize(w) * 4;
        }
        auto* row = reinterpret_cast<f32*>(img.pixels.data() + usize(y) * usize(img.row_bytes()));
        for (u32 x = 0; x < w; ++x)
            rgbe_to_float(rgbe.data() + usize(x) * 4, row + usize(x) * 3);
    }
    return img;
}

u16 float_to_half(f32 v) {
    u32 const bits = std::bit_cast<u32>(v);
    u16 const sign = u16((bits >> 16) & 0x8000u);
    u32 const mag  = bits & 0x7FFFFFFFu;
    if (mag > 0x7F800000u) return 0;               // NaN
    if (mag >= 0x477FF000u) return sign | 0x7BFFu; // >= 65520 would round to infinity: saturate
    if (mag < 0x38800000u) {                       // below 2^-14: a half subnormal or zero
        u32 const e = mag >> 23;
        if (e < 102) return sign; // below 2^-25: rounds to zero
        u32 const m     = (mag & 0x7FFFFFu) | 0x800000u;
        u32 const shift = 126 - e; // units of 2^-24, the half subnormal step
        u32 q           = m >> shift;
        u32 const rem   = m & ((1u << shift) - 1);
        u32 const half  = 1u << (shift - 1);
        if (rem > half || (rem == half && (q & 1u))) ++q;
        return u16(sign | q);
    }
    // Normal: rebias the exponent and round the mantissa from 23 to 10 bits, ties to even.
    u32 const odd = (mag >> 13) & 1u;
    u32 const r   = mag + 0xC8000FFFu + odd; // (15 - 127) << 23 plus 0xFFF, mod 2^32
    return u16(sign | min(r >> 13, 0x7BFFu));
}

f32 half_to_float(u16 h) {
    u32 const sign = u32(h & 0x8000u) << 16;
    u32 const e    = (h >> 10) & 0x1Fu;
    u32 const m    = h & 0x3FFu;
    if (e == 0) {
        f32 const v = std::ldexp(f32(m), -24);
        return (h & 0x8000u) ? -v : v;
    }
    if (e == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (m << 13));
    return std::bit_cast<f32>(sign | ((e + 112) << 23) | (m << 13));
}

} // namespace kiln::cook
