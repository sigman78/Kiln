// examples/adapter_support/staging_ring.h — FIFO allocator for upload staging memory, shared by the
// example adapters. Bookkeeping only: the adapter owns the memory and knows when a copy finished.
#pragma once

#include <kiln/alloc.h>
#include <kiln/containers.h>

namespace kiln::ex {

/// Hands out contiguous ranges of [0, size) in order and takes them back in any order: space
/// returns to the ring once every range reserved before it is released too. Not thread-safe:
/// the adapter guards it with its own lock.
class StagingRing {
public:
    struct Reservation {
        u64 offset = 0;
        u32 id     = kInvalid; ///< kInvalid: does not fit now
        [[nodiscard]] bool ok() const noexcept { return id != kInvalid; }
    };

    StagingRing() noexcept = default;
    /// `maxReservations` bounds the ranges held at once; a reserve() beyond it does not fit now.
    StagingRing(Allocator const* alloc, u64 size, u32 maxReservations);

    /// Could a range of `size` bytes ever fit (at alignment 1)? False means the upload is too
    /// large for this ring: fail it (Unsupported), do not retry it (Busy).
    [[nodiscard]] bool can_fit(u64 size) const noexcept { return size <= size_; }
    /// `size` bytes at a multiple of `align` (a power of two), or a Reservation that is not ok()
    /// when the ring is full now. Releases of earlier ranges make room. A zero size takes one byte.
    [[nodiscard]] Reservation reserve(u64 size, u64 align) noexcept;
    /// The range's data is no longer needed (its copy finished). Exactly once per reservation.
    void release(u32 id) noexcept;

    u64 size() const noexcept { return size_; }
    u32 held() const noexcept { return count_; } ///< ranges reserved and not reclaimed
    u64 used() const noexcept;                   ///< bytes not free, alignment gaps included

private:
    struct Range {
        u64 begin = 0, end = 0;
        bool released = false;
    };

    Vec<Range> ranges_; ///< circular, in reservation order
    u32 first_ = 0;
    u32 count_ = 0;
    u64 size_  = 0;
    u64 head_  = 0; ///< end of the newest range
};

} // namespace kiln::ex
