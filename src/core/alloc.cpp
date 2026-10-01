#include "kiln/alloc.h"

#include <atomic>
#include <cstdlib>

#if defined(KILN_OS_WINDOWS)
#include <malloc.h>
#endif

namespace kiln {

char const* tag_name(Tag tag) {
    switch (tag) {
    case Tag::General: return "general";
    case Tag::Core: return "core";
    case Tag::Io: return "io";
    case Tag::Cook: return "cook";
    case Tag::Registry: return "registry";
    case Tag::Payload: return "payload";
    case Tag::Jobs: return "jobs";
    case Tag::Test: return "test";
    case Tag::Count: break;
    }
    return "?";
}

namespace {

struct TagStats {
    std::atomic<u64> bytesCurrent{0};
    std::atomic<u64> bytesPeak{0};
    std::atomic<u64> allocCount{0};
    std::atomic<u64> freeCount{0};
};

TagStats g_stats[usize(Tag::Count)];

void* sys_alloc(usize size, usize align) {
    if (size == 0) size = 1;
    if (align < kDefaultAlign) align = kDefaultAlign;
#if defined(KILN_OS_WINDOWS)
    return _aligned_malloc(size, align);
#else
    // aligned_alloc requires size to be a multiple of align.
    return std::aligned_alloc(align, align_up(size, align));
#endif
}

void sys_free(void* p) {
#if defined(KILN_OS_WINDOWS)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* default_alloc(void* /*user*/, usize size, usize align, Tag tag) {
    KILN_ASSERT(is_pow2(align));
    KILN_ASSERT(tag < Tag::Count);
    void* p = sys_alloc(size, align);
    if (!p) return nullptr;
    TagStats& s = g_stats[usize(tag)];
    u64 cur     = s.bytesCurrent.fetch_add(size, std::memory_order_relaxed) + size;
    u64 pk      = s.bytesPeak.load(std::memory_order_relaxed);
    while (cur > pk && !s.bytesPeak.compare_exchange_weak(pk, cur, std::memory_order_relaxed)) {
    }
    s.allocCount.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void default_free(void* /*user*/, void* ptr, usize size, usize /*align*/, Tag tag) {
    if (!ptr) return;
    KILN_ASSERT(tag < Tag::Count);
    TagStats& s = g_stats[usize(tag)];
    s.bytesCurrent.fetch_sub(size, std::memory_order_relaxed);
    s.freeCount.fetch_add(1, std::memory_order_relaxed);
    sys_free(ptr);
}

constexpr Allocator g_default{&default_alloc, &default_free, nullptr};

} // namespace

Allocator const* default_allocator() { return &g_default; }

AllocStats default_alloc_stats(Tag tag) {
    AllocStats out;
    auto add = [&](TagStats const& s) {
        out.bytesCurrent += s.bytesCurrent.load(std::memory_order_relaxed);
        out.bytesPeak += s.bytesPeak.load(std::memory_order_relaxed);
        out.allocCount += s.allocCount.load(std::memory_order_relaxed);
        out.freeCount += s.freeCount.load(std::memory_order_relaxed);
    };
    if (tag == Tag::Count) {
        for (auto const& s : g_stats)
            add(s);
    } else {
        add(g_stats[usize(tag)]);
    }
    return out;
}

void* alloc(Allocator const* a, usize size, usize align, Tag tag) {
    KILN_ASSERT(a && a->alloc);
    KILN_VERIFY(is_pow2(align) && "alloc: alignment must be a power of two");
    void* p = a->alloc(a->user, size, align, tag);
    if (KILN_UNLIKELY(!p && size != 0)) {
        KILN_PANIC("out of memory: %llu bytes (align %llu, tag %s)", static_cast<unsigned long long>(size),
                   static_cast<unsigned long long>(align), tag_name(tag));
    }
    return p;
}

struct Arena::Block {
    Block* prev;
    usize size; // includes this header
};

// Block is private here, so the header size comes from its layout (two pointer-sized
// fields), rounded up so the payload after it is kDefaultAlign-aligned.
static constexpr usize kBlockHeader = align_up(usize(2 * sizeof(void*)), kDefaultAlign);

Arena::Arena(Desc const& desc) { init(desc); }

Arena::~Arena() noexcept { release(); }

Arena::Arena(Arena&& o) noexcept
    : desc_(o.desc_), head_(o.head_), cur_(o.cur_), end_(o.end_), used_(o.used_), reserved_(o.reserved_) {
    o.head_ = nullptr;
    o.cur_ = o.end_ = nullptr;
    o.used_ = o.reserved_ = 0;
}

Arena& Arena::operator=(Arena&& o) noexcept {
    if (this != &o) {
        release();
        desc_     = o.desc_;
        head_     = o.head_;
        cur_      = o.cur_;
        end_      = o.end_;
        used_     = o.used_;
        reserved_ = o.reserved_;
        o.head_   = nullptr;
        o.cur_ = o.end_ = nullptr;
        o.used_ = o.reserved_ = 0;
    }
    return *this;
}

void Arena::init(Desc const& desc) {
    KILN_ASSERT(head_ == nullptr && "Arena::init on a live arena");
    desc_ = desc;
    if (!desc_.backing) desc_.backing = default_allocator();
    if (desc_.blockSize < 256) desc_.blockSize = 256;
}

void* Arena::alloc(usize size, usize align) {
    KILN_ASSERT(is_pow2(align));
    u8* p = reinterpret_cast<u8*>(align_up(reinterpret_cast<std::uintptr_t>(cur_), std::uintptr_t(align)));
    if (KILN_LIKELY(cur_ && p + size <= end_)) {
        used_ += usize(p + size - cur_);
        cur_ = p + size;
        return p;
    }
    return alloc_slow(size, align);
}

void* Arena::alloc_slow(usize size, usize align) {
    if (!desc_.backing) init(desc_);
    // Blocks form one chain with no spare blocks to reuse, so always allocate a new
    // block big enough for this request plus alignment slack.
    usize need  = kBlockHeader + size + align;
    usize bytes = need > desc_.blockSize ? need : desc_.blockSize;
    Block* b    = static_cast<Block*>(kiln::alloc(desc_.backing, bytes, kDefaultAlign, desc_.tag));
    b->prev     = head_;
    b->size     = bytes;
    head_       = b;
    cur_        = reinterpret_cast<u8*>(b) + kBlockHeader;
    end_        = reinterpret_cast<u8*>(b) + bytes;
    reserved_ += bytes;

    u8* p = reinterpret_cast<u8*>(align_up(reinterpret_cast<std::uintptr_t>(cur_), std::uintptr_t(align)));
    KILN_ASSERT(p + size <= end_);
    used_ += usize(p + size - cur_);
    cur_ = p + size;
    return p;
}

StrView Arena::copy(StrView s) {
    char* p = static_cast<char*>(alloc(s.size + 1, 1));
    if (s.size) std::memcpy(p, s.data, s.size);
    p[s.size] = '\0';
    return {p, s.size};
}

void Arena::reset() noexcept {
    if (!head_) return;
    // Free every block except the oldest (bottom of the chain) and rewind into it.
    Block* b = head_;
    while (b->prev) {
        Block* prev = b->prev;
        reserved_ -= b->size;
        kiln::free(desc_.backing, b, b->size, kDefaultAlign, desc_.tag);
        b = prev;
    }
    head_ = b;
    cur_  = reinterpret_cast<u8*>(b) + kBlockHeader;
    end_  = reinterpret_cast<u8*>(b) + b->size;
    used_ = 0;
}

void Arena::release() noexcept {
    Block* b = head_;
    while (b) {
        Block* prev = b->prev;
        kiln::free(desc_.backing, b, b->size, kDefaultAlign, desc_.tag);
        b = prev;
    }
    head_ = nullptr;
    cur_ = end_ = nullptr;
    used_ = reserved_ = 0;
}

namespace {
void* arena_alloc_thunk(void* user, usize size, usize align, Tag /*tag*/) {
    return static_cast<Arena*>(user)->alloc(size, align);
}
void arena_free_thunk(void*, void*, usize, usize, Tag) {}
} // namespace

Allocator Arena::as_allocator() noexcept { return {&arena_alloc_thunk, &arena_free_thunk, this}; }

} // namespace kiln
