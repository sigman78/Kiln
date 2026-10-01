// formats_internal.h — helpers shared by the src/formats/*.cpp readers and writers.
#pragma once

#include "kiln/alloc.h"
#include "kiln/manifest.h"

struct ZSTD_DCtx_s;

namespace kiln::fmt {

constexpr u32 gcd(u32 a, u32 b) noexcept {
    while (b != 0) {
        u32 t = a % b;
        a     = b;
        b     = t;
    }
    return a;
}
constexpr u32 lcm(u32 a, u32 b) noexcept { return a / gcd(a, b) * b; }

/// xxh3_128 of `bytes` as if `[zeroOff, zeroOff + zeroLen)` were zero (at most 64 bytes): a
/// checksum stored inside the bytes it covers.
[[nodiscard]] Hash128 xxh3_128_zeroed(Span<u8 const> bytes, u64 zeroOff, u64 zeroLen) noexcept;

/// KTX 2.0 level alignment without supercompression: lcm(texel block size, 4).
constexpr u32 ktx2_level_align(u32 bytesPerBlock) noexcept { return lcm(bytesPerBlock, 4u); }

/// The opaque pointer of a zstd custom allocator: every zstd allocation goes to `alloc`.
struct ZstdMem {
    Allocator const* alloc = nullptr;
    Tag tag                = Tag::Io;
};
void* zstd_alloc(void* mem, usize size) noexcept;
void zstd_free(void* mem, void* ptr) noexcept;

/// The decompressed size a Zstd frame's header states, or ~0 when it states none or `src` is not a
/// frame.
[[nodiscard]] u64 zstd_content_size(Span<u8 const> src) noexcept;

/// A Zstd decoder. Its context is made on the first decode, from `alloc` (Tag::Io), and reused.
/// Not thread-safe: one per job.
class ZstdDecoder {
public:
    explicit ZstdDecoder(Allocator const* alloc) noexcept : mem_{alloc, Tag::Io} {}
    ~ZstdDecoder() noexcept;
    ZstdDecoder(ZstdDecoder const&)            = delete;
    ZstdDecoder& operator=(ZstdDecoder const&) = delete;

    /// True when `src` is exactly one Zstd frame of `out.size` bytes, now in `out`. Else false,
    /// and error() says why.
    [[nodiscard]] bool decode(Span<u8 const> src, Span<u8> out) noexcept;
    [[nodiscard]] char const* error() const noexcept { return error_; }

private:
    ZstdMem mem_;
    ZSTD_DCtx_s* ctx_  = nullptr;
    char const* error_ = "";
};

} // namespace kiln::fmt
