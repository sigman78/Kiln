// examples/adapter_support/commit_queue.cpp — the committed-upload queue (commit_queue.h).
#include "commit_queue.h"

#include <utility>

namespace kiln::ex {

CommitQueue::CommitQueue(Allocator const* alloc, u32 capacity)
    : pending_(alloc ? alloc : default_allocator(), Tag::Payload),
      taken_(alloc ? alloc : default_allocator(), Tag::Payload), capacity_(capacity) {
    pending_.reserve(capacity);
    taken_.reserve(capacity);
}

CommitQueue& CommitQueue::operator=(CommitQueue&& o) noexcept {
    pending_  = std::move(o.pending_);
    taken_    = std::move(o.taken_);
    capacity_ = o.capacity_;
    return *this;
}

void CommitQueue::push(u32 index) {
    std::lock_guard<std::mutex> const lock(mutex_);
    KILN_VERIFY(pending_.size() < capacity_ && "CommitQueue: more indices than upload records");
    pending_.push_back(index);
}

Span<u32 const> CommitQueue::take() {
    taken_.clear();
    {
        std::lock_guard<std::mutex> const lock(mutex_);
        std::swap(pending_, taken_); // both keep their reserved capacity
    }
    return taken_.span();
}

} // namespace kiln::ex
