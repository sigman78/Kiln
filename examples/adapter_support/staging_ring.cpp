// examples/adapter_support/staging_ring.cpp — the staging ring (staging_ring.h).
#include "staging_ring.h"

namespace kiln::ex {

StagingRing::StagingRing(Allocator const* alloc, u64 size, u32 maxReservations)
    : ranges_(alloc ? alloc : default_allocator(), Tag::Payload), size_(size) {
    ranges_.resize(max(maxReservations, 1u));
}

StagingRing::Reservation StagingRing::reserve(u64 size, u64 align) noexcept {
    KILN_ASSERT(is_pow2(align));
    u32 const cap = u32(ranges_.size());
    if (count_ == cap) return {};
    size = max<u64>(size, 1);
    if (size > size_) return {};
    if (count_ == 0) head_ = 0; // empty: start over at the bottom
    u64 const tail = count_ ? ranges_[first_].begin : 0;
    // With ranges held, the free space is [head, tail) once the ring has wrapped (head <= tail),
    // else [head, size) followed by [0, tail).
    bool const wrapped = count_ && head_ <= tail;
    u64 start          = 0;
    if (!checked_add(head_, align - 1, start)) return {};
    start &= ~(align - 1);
    if (wrapped) {
        if (start > tail || size > tail - start) return {};
    } else if (start > size_ || size > size_ - start) {
        start = 0; // wrap to the bottom
        if (count_ && size > tail) return {};
    }
    u32 const id = (first_ + count_) % cap;
    ranges_[id]  = Range{start, start + size, false};
    ++count_;
    head_ = start + size;
    return {start, id};
}

void StagingRing::release(u32 id) noexcept {
    u32 const cap = u32(ranges_.size());
    KILN_ASSERT(id < cap && (id + cap - first_) % cap < count_ && !ranges_[id].released);
    ranges_[id].released = true;
    while (count_ && ranges_[first_].released) {
        first_ = (first_ + 1) % cap;
        --count_;
    }
}

u64 StagingRing::used() const noexcept {
    if (count_ == 0) return 0;
    u64 const tail = ranges_[first_].begin;
    return head_ > tail ? head_ - tail : size_ - tail + head_;
}

} // namespace kiln::ex
