// kiln/alloc.h — allocator interface, allocation tags and statistics, arena.
// All kiln allocation goes through an `Allocator`. The only global one is default_allocator().
#pragma once

#include "kiln/core.h"

namespace kiln {

/// Accounting tag attached to every allocation. Stats are queryable per tag.
enum class Tag : u8 {
    General = 0, ///< untagged / misc
    Core,        ///< containers, strings and other core-owned memory
    Io,          ///< read buffers, IO backend state
    Cook,        ///< cook temporaries and outputs
    Registry,    ///< asset registry, handles, request bookkeeping
    Payload,     ///< decoded asset payloads held in CPU memory (metadata blobs, placeholders)
    Jobs,        ///< thread pool / job system
    Test,        ///< reserved for tests
    Count
};

KILN_API char const* tag_name(Tag tag);

inline constexpr usize kDefaultAlign = alignof(std::max_align_t);

/// Allocator interface: function pointers plus a user pointer. Hosts derive from nothing.
///  - `alloc` returns at least `size` bytes aligned to `align` (a power of two), or
///    nullptr on failure. `size == 0` is allowed and may return nullptr.
///  - `free` receives the size, align and tag passed to `alloc`. Size-aware allocators need no header.
///  - Both must be thread-safe.
struct Allocator {
    void* (*alloc)(void* user, usize size, usize align, Tag tag)          = nullptr;
    void (*free)(void* user, void* ptr, usize size, usize align, Tag tag) = nullptr;
    void* user                                                            = nullptr;
};

/// The built-in allocator: malloc/free (aligned) plus per-tag statistics.
KILN_API Allocator const* default_allocator();

/// Per-tag statistics for the default allocator.
struct AllocStats {
    u64 bytesCurrent = 0;
    u64 bytesPeak    = 0;
    u64 allocCount   = 0; ///< total successful allocations (monotonic)
    u64 freeCount    = 0; ///< total frees (monotonic)
};

/// Stats for one tag, or the sum over all tags when `tag == Tag::Count`.
KILN_API AllocStats default_alloc_stats(Tag tag = Tag::Count);

/// Allocate or return nullptr.
[[nodiscard]] inline void* try_alloc(Allocator const* a, usize size, usize align, Tag tag) {
    KILN_ASSERT(a && a->alloc);
    return a->alloc(a->user, size, align, tag);
}

/// Allocate or panic. Out-of-memory is not recoverable.
[[nodiscard]] KILN_API void* alloc(Allocator const* a, usize size, usize align, Tag tag);

inline void free(Allocator const* a, void* ptr, usize size, usize align, Tag tag) {
    if (!ptr) return;
    KILN_ASSERT(a && a->free);
    a->free(a->user, ptr, size, align, tag);
}

/// Allocate + construct one object. Panics on OOM.
template <class T, class... Args> [[nodiscard]] T* new_object(Allocator const* a, Tag tag, Args&&... args) {
    void* mem = alloc(a, sizeof(T), alignof(T), tag);
    return ::new (mem) T(std::forward<Args>(args)...);
}

/// Destroy + free one object allocated with new_object. Null is a no-op.
template <class T> void delete_object(Allocator const* a, T* obj, Tag tag) {
    if (!obj) return;
    obj->~T();
    free(a, obj, sizeof(T), alignof(T), tag);
}

/// Allocate an uninitialized array of `count` T. Panics on OOM and when the size overflows.
template <class T> [[nodiscard]] T* alloc_array(Allocator const* a, usize count, Tag tag) {
    usize bytes = 0;
    KILN_VERIFY(checked_mul(count, sizeof(T), bytes) && "alloc_array: count * sizeof(T) overflows");
    return static_cast<T*>(alloc(a, bytes, alignof(T), tag));
}
template <class T> void free_array(Allocator const* a, T* p, usize count, Tag tag) {
    free(a, p, count * sizeof(T), alignof(T), tag);
}

/// Bump allocator for per-cook / per-load temporaries, over blocks from a backing
/// Allocator. Individual frees are no-ops. `reset()` frees everything at once.
/// Not thread-safe: use one arena per job.
class KILN_API Arena {
public:
    struct Desc {
        Allocator const* backing = nullptr;   ///< nullptr → default_allocator()
        usize blockSize          = 64 * 1024; ///< minimum size of each backing block
        Tag tag                  = Tag::General;
    };

    Arena() = default;
    explicit Arena(Desc const& desc);
    ~Arena();
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    Arena(Arena const&)            = delete;
    Arena& operator=(Arena const&) = delete;

    void init(Desc const& desc);

    /// Allocate `size` bytes aligned to `align`. Panics on OOM. Returns a non-null
    /// pointer for size 0.
    [[nodiscard]] void* alloc(usize size, usize align = kDefaultAlign);

    template <class T> [[nodiscard]] T* alloc_array(usize count) {
        return static_cast<T*>(alloc(count * sizeof(T), alignof(T)));
    }
    template <class T, class... Args> [[nodiscard]] T* create(Args&&... args) {
        return ::new (alloc(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    /// Copy a string into the arena, null-terminated. Returns a view of the copy.
    StrView copy(StrView s);

    /// Free all allocations. The first block is retained for reuse.
    void reset();
    /// Free all allocations and all blocks.
    void release();

    usize bytes_used() const { return used_; }
    usize bytes_reserved() const { return reserved_; }

    /// View this arena as an Allocator (free is a no-op). The Arena must outlive users.
    Allocator as_allocator();

private:
    struct Block;
    void* alloc_slow(usize size, usize align);

    Desc desc_{};
    Block* head_    = nullptr; ///< current block (most recent)
    u8* cur_        = nullptr;
    u8* end_        = nullptr;
    usize used_     = 0;
    usize reserved_ = 0;
};

} // namespace kiln
