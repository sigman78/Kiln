// kiln/catalog.h — 128-bit content hashes (XXH3-128) for build keys and store catalogs
// (docs/design/store-catalog.md).
#pragma once

#include "kiln/core.h"

namespace kiln {

/// A 128-bit hash in XXH3's canonical byte order: the hex of the bytes is what `xxhsum -H2` prints.
struct Hash128 {
    u8 bytes[16] = {};

    [[nodiscard]] friend bool operator==(Hash128 const& a, Hash128 const& b) noexcept {
        return std::memcmp(a.bytes, b.bytes, sizeof a.bytes) == 0;
    }
    [[nodiscard]] bool is_zero() const noexcept { return *this == Hash128{}; }
};

/// XXH3-128 of `bytes`, seed 0. A checksum, not a signature.
[[nodiscard]] KILN_API Hash128 xxh3_128(Span<u8 const> bytes) noexcept;

/// 32 lowercase hex digits and a NUL into `out`.
KILN_API void hash128_hex(Hash128 const& h, char (&out)[33]) noexcept;

} // namespace kiln
