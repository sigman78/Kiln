// kiln/hash.h — constexpr hashing: FNV-1a 64 (names, asset ids), XXH64 (content), XXH32 (checksums),
// fourcc, and hash_of() overloads used by HashMap.
#pragma once

#include "kiln/core.h"

namespace kiln {

// ---------------------------------------------------------------------------
// fourcc
// ---------------------------------------------------------------------------

/// Little-endian fourcc: fourcc('O','M','S','H') stores as bytes "OMSH".
[[nodiscard]] constexpr u32 fourcc(char a, char b, char c, char d) noexcept {
    return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}
[[nodiscard]] constexpr u32 fourcc(char const (&s)[5]) noexcept { return fourcc(s[0], s[1], s[2], s[3]); }

/// Writes the four characters of `id` into `out` (5 bytes, null-terminated).
constexpr void fourcc_str(u32 id, char out[5]) noexcept {
    out[0] = char(id & 0xFF);
    out[1] = char((id >> 8) & 0xFF);
    out[2] = char((id >> 16) & 0xFF);
    out[3] = char((id >> 24) & 0xFF);
    out[4] = '\0';
}

// ---------------------------------------------------------------------------
// FNV-1a 64 — used for names and asset ids (see mesh-format-spec.md §3)
// ---------------------------------------------------------------------------

inline constexpr u64 kFnv1a64Offset = 0xcbf29ce484222325ull;
inline constexpr u64 kFnv1a64Prime  = 0x100000001b3ull;

[[nodiscard]] constexpr u64 fnv1a64(u8 const* data, usize len, u64 seed = kFnv1a64Offset) noexcept {
    u64 h = seed;
    for (usize i = 0; i < len; ++i) {
        h ^= data[i];
        h *= kFnv1a64Prime;
    }
    return h;
}
[[nodiscard]] constexpr u64 fnv1a64(char const* data, usize len, u64 seed = kFnv1a64Offset) noexcept {
    u64 h = seed;
    for (usize i = 0; i < len; ++i) {
        h ^= u8(data[i]);
        h *= kFnv1a64Prime;
    }
    return h;
}
[[nodiscard]] constexpr u64 fnv1a64(StrView s) noexcept { return fnv1a64(s.data, s.size); }
[[nodiscard]] constexpr u64 fnv1a64(Span<u8 const> b) noexcept { return fnv1a64(b.data, b.size); }

/// Name hash per the .mesh spec: FNV-1a 64 over exact bytes, no terminator.
[[nodiscard]] constexpr u64 hash_name(StrView s) noexcept { return fnv1a64(s); }

inline namespace literals {
/// "meshes/ship"_h → FNV-1a 64 at compile time.
[[nodiscard]] constexpr u64 operator""_h(char const* s, usize n) noexcept { return fnv1a64(s, n); }
} // namespace literals

// ---------------------------------------------------------------------------
// XXH64 — used for content hashing (source bytes, settings, store keys)
// ---------------------------------------------------------------------------

namespace detail {
inline constexpr u64 kXxhP1 = 0x9E3779B185EBCA87ull;
inline constexpr u64 kXxhP2 = 0xC2B2AE3D27D4EB4Full;
inline constexpr u64 kXxhP3 = 0x165667B19E3779F9ull;
inline constexpr u64 kXxhP4 = 0x85EBCA77C2B2AE63ull;
inline constexpr u64 kXxhP5 = 0x27D4EB2F165667C5ull;

[[nodiscard]] constexpr u64 xxh_read64(u8 const* p) noexcept {
    if (std::is_constant_evaluated()) {
        u64 v = 0;
        for (int i = 0; i < 8; ++i)
            v |= u64(p[i]) << (8 * i);
        return v;
    }
    u64 v;
    std::memcpy(&v, p, 8);
    return v;
}
[[nodiscard]] constexpr u32 xxh_read32(u8 const* p) noexcept {
    if (std::is_constant_evaluated()) {
        u32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= u32(p[i]) << (8 * i);
        return v;
    }
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
[[nodiscard]] constexpr u64 xxh_round(u64 acc, u64 input) noexcept {
    acc += input * kXxhP2;
    acc = std::rotl(acc, 31);
    acc *= kXxhP1;
    return acc;
}
[[nodiscard]] constexpr u64 xxh_merge(u64 acc, u64 val) noexcept {
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * kXxhP1 + kXxhP4;
    return acc;
}
[[nodiscard]] constexpr u64 xxh_avalanche(u64 h) noexcept {
    h ^= h >> 33;
    h *= kXxhP2;
    h ^= h >> 29;
    h *= kXxhP3;
    h ^= h >> 32;
    return h;
}
} // namespace detail

/// XXH64, bit-exact with the reference implementation.
[[nodiscard]] constexpr u64 xxh64(u8 const* p, usize len, u64 seed = 0) noexcept {
    using namespace detail;
    u8 const* const end = p + len;
    u64 h;
    if (len >= 32) {
        u8 const* const limit = end - 32;
        u64 v1                = seed + kXxhP1 + kXxhP2;
        u64 v2                = seed + kXxhP2;
        u64 v3                = seed + 0;
        u64 v4                = seed - kXxhP1;
        do {
            v1 = xxh_round(v1, xxh_read64(p));
            p += 8;
            v2 = xxh_round(v2, xxh_read64(p));
            p += 8;
            v3 = xxh_round(v3, xxh_read64(p));
            p += 8;
            v4 = xxh_round(v4, xxh_read64(p));
            p += 8;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed + kXxhP5;
    }
    h += u64(len);
    while (end - p >= 8) {
        u64 k1 = xxh_round(0, xxh_read64(p));
        h ^= k1;
        h = std::rotl(h, 27) * kXxhP1 + kXxhP4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= u64(xxh_read32(p)) * kXxhP1;
        h = std::rotl(h, 23) * kXxhP2 + kXxhP3;
        p += 4;
    }
    while (p < end) {
        h ^= u64(*p) * kXxhP5;
        h = std::rotl(h, 11) * kXxhP1;
        ++p;
    }
    return xxh_avalanche(h);
}
[[nodiscard]] constexpr u64 xxh64(Span<u8 const> b, u64 seed = 0) noexcept {
    return xxh64(b.data, b.size, seed);
}
/// Runtime only (needs reinterpret_cast). For compile-time name hashes use fnv1a64 / ""_h.
[[nodiscard]] inline u64 xxh64(StrView s, u64 seed = 0) noexcept {
    return xxh64(reinterpret_cast<u8 const*>(s.data), s.size, seed);
}
[[nodiscard]] inline u64 xxh64(void const* p, usize len, u64 seed = 0) noexcept {
    return xxh64(static_cast<u8 const*>(p), len, seed);
}

// ---------------------------------------------------------------------------
// XXH32 — used for small checksums (e.g. .mesh PayloadBlob.checksum)
// ---------------------------------------------------------------------------

namespace detail {
inline constexpr u32 kXxh32P1 = 2654435761u;
inline constexpr u32 kXxh32P2 = 2246822519u;
inline constexpr u32 kXxh32P3 = 3266489917u;
inline constexpr u32 kXxh32P4 = 668265263u;
inline constexpr u32 kXxh32P5 = 374761393u;

[[nodiscard]] constexpr u32 xxh32_round(u32 acc, u32 input) noexcept {
    acc += input * kXxh32P2;
    acc = std::rotl(acc, 13);
    acc *= kXxh32P1;
    return acc;
}
} // namespace detail

/// XXH32, bit-exact with the reference implementation.
[[nodiscard]] constexpr u32 xxh32(u8 const* p, usize len, u32 seed = 0) noexcept {
    using namespace detail;
    u8 const* const end = p + len;
    u32 h;
    if (len >= 16) {
        u32 v1 = seed + kXxh32P1 + kXxh32P2;
        u32 v2 = seed + kXxh32P2;
        u32 v3 = seed + 0;
        u32 v4 = seed - kXxh32P1;
        do {
            v1 = xxh32_round(v1, xxh_read32(p));
            v2 = xxh32_round(v2, xxh_read32(p + 4));
            v3 = xxh32_round(v3, xxh_read32(p + 8));
            v4 = xxh32_round(v4, xxh_read32(p + 12));
            p += 16;
        } while (end - p >= 16);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    } else {
        h = seed + kXxh32P5;
    }
    h += u32(len);
    while (end - p >= 4) {
        h += xxh_read32(p) * kXxh32P3;
        h = std::rotl(h, 17) * kXxh32P4;
        p += 4;
    }
    while (p < end) {
        h += u32(*p) * kXxh32P5;
        h = std::rotl(h, 11) * kXxh32P1;
        ++p;
    }
    h ^= h >> 15;
    h *= kXxh32P2;
    h ^= h >> 13;
    h *= kXxh32P3;
    h ^= h >> 16;
    return h;
}
[[nodiscard]] constexpr u32 xxh32(Span<u8 const> b, u32 seed = 0) noexcept {
    return xxh32(b.data, b.size, seed);
}
[[nodiscard]] inline u32 xxh32(StrView s, u32 seed = 0) noexcept {
    return xxh32(reinterpret_cast<u8 const*>(s.data), s.size, seed);
}
[[nodiscard]] inline u32 xxh32(void const* p, usize len, u32 seed = 0) noexcept {
    return xxh32(static_cast<u8 const*>(p), len, seed);
}

/// Streaming XXH64 for hashing several buffers (settings structs, file chunks).
class KILN_API Xxh64State {
public:
    explicit Xxh64State(u64 seed = 0) noexcept { reset(seed); }
    void reset(u64 seed = 0) noexcept;
    void update(void const* data, usize len) noexcept;
    void update(Span<u8 const> b) noexcept { update(b.data, b.size); }
    void update(StrView s) noexcept { update(s.data, s.size); }
    /// Hash one scalar (integer, float, enum, bool). Deliberately not offered for
    /// structs: their padding bytes are indeterminate, so hash structs field by
    /// field (see docs/design/settings.md).
    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void update_value(T const& v) noexcept {
        update(&v, sizeof(T));
    }
    [[nodiscard]] u64 digest() const noexcept;

private:
    u64 acc_[4];
    u8 buf_[32];
    u32 bufLen_;
    u64 total_;
    u64 seed_;
};

// ---------------------------------------------------------------------------
// hash_of() — customization point for HashMap keys; 64-bit, well mixed
// ---------------------------------------------------------------------------

/// Finalizer for integer keys (splitmix64 mix).
[[nodiscard]] constexpr u64 mix64(u64 x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

/// Combine two hashes.
[[nodiscard]] constexpr u64 hash_combine(u64 a, u64 b) noexcept {
    return mix64(a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)));
}

template <std::integral T> [[nodiscard]] constexpr u64 hash_of(T v) noexcept { return mix64(u64(v)); }
template <class T> [[nodiscard]] constexpr u64 hash_of(T* p) noexcept {
    return mix64(u64(reinterpret_cast<std::uintptr_t>(p)));
}
[[nodiscard]] constexpr u64 hash_of(StrView s) noexcept { return fnv1a64(s); }
template <class Tag> [[nodiscard]] constexpr u64 hash_of(Handle<Tag> h) noexcept { return mix64(h.bits()); }
template <class E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr u64 hash_of(E e) noexcept {
    return mix64(u64(static_cast<std::underlying_type_t<E>>(e)));
}

/// Default hasher used by HashMap: dispatches to hash_of(key) via ADL.
struct DefaultHash {
    template <class K> [[nodiscard]] constexpr u64 operator()(K const& k) const noexcept {
        return hash_of(k);
    }
};

} // namespace kiln
