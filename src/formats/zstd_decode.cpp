// Zstd decoding of KTX2 levels, with zstd's memory from a kiln Allocator.
#include "formats_internal.h"

// The custom-allocator API is stable only within one zstd version; the vendored one is pinned.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace kiln::fmt {

namespace {

// zstd frees without a size, and a kiln Allocator needs it: a header in front of each block
// keeps it. 16 bytes keep the block as aligned as malloc's.
constexpr usize kHeader = 16;

} // namespace

void* zstd_alloc(void* mem, usize size) {
    ZstdMem const* m = static_cast<ZstdMem const*>(mem);
    usize total      = 0;
    if (!checked_add(size, kHeader, total)) return nullptr;
    u8* p = static_cast<u8*>(try_alloc(m->alloc, total, kHeader, m->tag));
    if (!p) return nullptr;
    std::memcpy(p, &total, sizeof total);
    return p + kHeader;
}

void zstd_free(void* mem, void* ptr) {
    if (!ptr) return;
    ZstdMem const* m = static_cast<ZstdMem const*>(mem);
    u8* p            = static_cast<u8*>(ptr) - kHeader;
    usize total      = 0;
    std::memcpy(&total, p, sizeof total);
    free(m->alloc, p, total, kHeader, m->tag);
}

u64 zstd_content_size(Span<u8 const> src) {
    unsigned long long const n = ZSTD_getFrameContentSize(src.data, src.size);
    return n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR ? ~u64(0) : u64(n);
}

ZstdDecoder::~ZstdDecoder() {
    if (ctx_) ZSTD_freeDCtx(ctx_);
}

bool ZstdDecoder::decode(Span<u8 const> src, Span<u8> out) {
    if (!ctx_) {
        if (!mem_.alloc) mem_.alloc = default_allocator();
        ctx_ = ZSTD_createDCtx_advanced(ZSTD_customMem{&zstd_alloc, &zstd_free, &mem_});
        if (!ctx_) {
            error_ = "out of memory for the Zstd context";
            return false;
        }
    }
    // Exactly one frame: trailing bytes or a second frame would be ignored or appended.
    usize const frame = ZSTD_findFrameCompressedSize(src.data, src.size);
    if (ZSTD_isError(frame) || frame != src.size) {
        error_ = ZSTD_isError(frame) ? ZSTD_getErrorName(frame) : "bytes after the Zstd frame";
        return false;
    }
    usize const n = ZSTD_decompressDCtx(ctx_, out.data, out.size, src.data, src.size);
    if (ZSTD_isError(n)) {
        error_ = ZSTD_getErrorName(n);
        return false;
    }
    if (n != out.size) {
        error_ = "the Zstd frame is shorter than the level";
        return false;
    }
    return true;
}

} // namespace kiln::fmt
