// kiln/alloc.h — allocator interface, allocation tags and statistics, arena.
//
// All allocation in kiln goes through an `Allocator`. There are no hidden globals
// except the default allocator returned by default_allocator().
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

[[nodiscard]] KILN_API char const* tag_name(Tag tag) noexcept;

inline constexpr usize kDefaultAlign = alignof(std::max_align_t);

/// The allocator interface: a plain struct of function pointers plus a user pointer,
/// so hosts can wire in any allocator without deriving from anything.
///
/// Contract:
///  - `alloc` returns memory of at least `size` bytes aligned to `align` (a power of
///    two), or nullptr on failure. `size == 0` is allowed and may return nullptr.
///  - `free` receives the same size/align/tag that were passed to `alloc`, so
///    size-aware allocators need no header.
///  - Both must be thread-safe.
struct Allocator {
    void* (*alloc)(void* user, usize size, usize align, Tag tag)          = nullptr;
    void (*free)(void* user, void* ptr, usize size, usize align, Tag tag) = nullptr;
    void* user                                                            = nullptr;
};

/// The built-in allocator: malloc/free (aligned) plus per-tag statistics.
[[nodiscard]] KILN_API Allocator const* default_allocator() noexcept;

/// Per-tag statistics for the default allocator.
struct AllocStats {
    u64 bytesCurrent = 0;
    u64 bytesPeak    = 0;
    u64 allocCount   = 0; ///< total successful allocations (monotonic)
    u64 freeCount    = 0; ///< total frees (monotonic)
};

/// Stats for one tag, or the sum over all tags when `tag == Tag::Count`.
[[nodiscard]] KILN_API AllocStats default_alloc_stats(Tag tag = Tag::Count) noexcept;

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

/// Allocate or return nullptr.
[[nodiscard]] inline void* try_alloc(Allocator const* a, usize size, usize align, Tag tag) noexcept {
    KILN_ASSERT(a && a->alloc);
    return a->alloc(a->user, size, align, tag);
}

/// Allocate or panic. Out-of-memory is non-recoverable by policy (HANDOFF §2).
[[nodiscard]] KILN_API void* alloc(Allocator const* a, usize size, usize align, Tag tag) noexcept;

inline void free(Allocator const* a, void* ptr, usize size, usize align, Tag tag) noexcept {
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
template <class T> void delete_object(Allocator const* a, T* obj, Tag tag) noexcept {
    if (!obj) return;
    obj->~T();
    free(a, obj, sizeof(T), alignof(T), tag);
}

/// Allocate an uninitialized array of `count` T. Panics on OOM.
template <class T> [[nodiscard]] T* alloc_array(Allocator const* a, usize count, Tag tag) noexcept {
    return static_cast<T*>(alloc(a, count * sizeof(T), alignof(T), tag));
}
template <class T> void free_array(Allocator const* a, T* p, usize count, Tag tag) noexcept {
    free(a, p, count * sizeof(T), alignof(T), tag);
}

// ---------------------------------------------------------------------------
// Arena: linear allocator for per-cook / per-load temporaries, reset in bulk
// ---------------------------------------------------------------------------

/// Bump allocator over blocks obtained from a backing Allocator. Individual frees
/// are no-ops; `reset()` reclaims everything at once and keeps the first block.
/// Not thread-safe: one arena per job.
class KILN_API Arena {
public:
    struct Desc {
        Allocator const* backing = nullptr;   ///< nullptr → default_allocator()
        usize blockSize          = 64 * 1024; ///< minimum size of each backing block
        Tag tag                  = Tag::General;
    };

    Arena() noexcept = default;
    explicit Arena(Desc const& desc) noexcept;
    ~Arena() noexcept;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    Arena(Arena const&)            = delete;
    Arena& operator=(Arena const&) = delete;

    void init(Desc const& desc) noexcept;

    /// Allocate `size` bytes aligned to `align`. Panics on OOM. Returns a non-null
    /// pointer for size 0.
    [[nodiscard]] void* alloc(usize size, usize align = kDefaultAlign) noexcept;

    template <class T> [[nodiscard]] T* alloc_array(usize count) noexcept {
        return static_cast<T*>(alloc(count * sizeof(T), alignof(T)));
    }
    template <class T, class... Args> [[nodiscard]] T* create(Args&&... args) {
        return ::new (alloc(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    /// Copy a string into the arena, null-terminated. Returns a view of the copy.
    [[nodiscard]] StrView copy(StrView s) noexcept;

    /// Free all allocations. The first block is retained for reuse.
    void reset() noexcept;
    /// Free all allocations and all blocks.
    void release() noexcept;

    [[nodiscard]] usize bytes_used() const noexcept { return used_; }
    [[nodiscard]] usize bytes_reserved() const noexcept { return reserved_; }

    /// View this arena as an Allocator (free is a no-op). The Arena must outlive users.
    [[nodiscard]] Allocator as_allocator() noexcept;

private:
    struct Block;
    void* alloc_slow(usize size, usize align) noexcept;

    Desc desc_{};
    Block* head_    = nullptr; ///< current block (most recent)
    u8* cur_        = nullptr;
    u8* end_        = nullptr;
    usize used_     = 0;
    usize reserved_ = 0;
};

} // namespace kiln
