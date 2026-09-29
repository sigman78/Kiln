// XXH3-128 from the xxhash that zstd vendors, inlined into this file only.
#include "formats_internal.h"

#include "kiln/catalog.h"

#define KILN_XXH3 1
#define XXH_INLINE_ALL
#include <common/xxhash.h>

namespace kiln {

Hash128 xxh3_128(Span<u8 const> bytes) noexcept {
    XXH128_canonical_t c;
    XXH128_canonicalFromHash(&c, XXH3_128bits(bytes.data, bytes.size));
    Hash128 h;
    std::memcpy(h.bytes, c.digest, sizeof h.bytes);
    return h;
}

namespace fmt {

Hash128 xxh3_128_zeroed(Span<u8 const> bytes, u64 zeroOff, u64 zeroLen) noexcept {
    KILN_ASSERT(zeroOff <= bytes.size && zeroLen <= bytes.size - zeroOff);
    constexpr u8 kZeros[64] = {};
    KILN_ASSERT(zeroLen <= sizeof kZeros);
    XXH3_state_t st;
    XXH3_INITSTATE(&st);
    (void)XXH3_128bits_reset(&st);
    (void)XXH3_128bits_update(&st, bytes.data, usize(zeroOff));
    (void)XXH3_128bits_update(&st, kZeros, usize(zeroLen));
    (void)XXH3_128bits_update(&st, bytes.data + zeroOff + zeroLen, usize(bytes.size - zeroOff - zeroLen));
    XXH128_canonical_t c;
    XXH128_canonicalFromHash(&c, XXH3_128bits_digest(&st));
    Hash128 h;
    std::memcpy(h.bytes, c.digest, sizeof h.bytes);
    return h;
}

} // namespace fmt

void hash128_hex(Hash128 const& h, char (&out)[33]) noexcept {
    constexpr char kDigits[] = "0123456789abcdef";
    for (usize i = 0; i < 16; ++i) {
        out[2 * i]     = kDigits[h.bytes[i] >> 4];
        out[2 * i + 1] = kDigits[h.bytes[i] & 15];
    }
    out[32] = '\0';
}

} // namespace kiln
