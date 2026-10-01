// examples/adapter_support/commit_queue.h — the handoff of committed uploads from kiln's workers
// (commit_upload) to the adapter's flush on the pump thread, shared by the example adapters.
#pragma once

#include <kiln/alloc.h>
#include <kiln/containers.h>

#include <mutex>

namespace kiln::ex {

/// Upload indices in commit order. Workers push; flush takes them all at once and does its GPU
/// work on them outside the lock. Holds at most `capacity` indices, one per upload record, and
/// never allocates after construction.
class CommitQueue {
public:
    CommitQueue() noexcept = default;
    CommitQueue(Allocator const* alloc, u32 capacity);

    CommitQueue(CommitQueue const&)            = delete;
    CommitQueue& operator=(CommitQueue const&) = delete;
    CommitQueue& operator=(CommitQueue&& o) noexcept; ///< before use only: moves the buffers, not the lock

    /// Any thread: upload `index` waits for the next take(). The pump thread pushes an index again
    /// to retry it at the next flush.
    void push(u32 index);
    /// Pump thread: everything pushed so far, in push order. Valid until the next take().
    [[nodiscard]] Span<u32 const> take();

private:
    std::mutex mutex_;
    Vec<u32> pending_; ///< guarded by mutex_
    Vec<u32> taken_;   ///< pump thread only
    u32 capacity_ = 0;
};

} // namespace kiln::ex
