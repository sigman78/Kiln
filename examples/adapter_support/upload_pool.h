// examples/adapter_support/upload_pool.h — upload records behind kiln's tokens, shared by the example
// adapters: one state per upload, and generation-checked tokens so a stale one finds nothing.
#pragma once

#include <kiln/adapter.h>
#include <kiln/alloc.h>
#include <kiln/containers.h>

namespace kiln::ex {

/// Where an upload is. Free -> Writing (acquire) -> Committed -> [InFlight ->] Complete or Failed
/// -> Free (release). InFlight is for adapters whose copy runs on the GPU after flush; an adapter
/// that finishes in flush goes from Committed straight to a terminal state.
enum class UploadState : u8 {
    Free = 0,
    Writing,   ///< begin_upload handed it out; kiln is writing the bytes
    Committed, ///< commit_upload; waiting for the adapter's GPU work
    InFlight,  ///< the copy is submitted; waiting for the GPU
    Complete,
    Failed,
};

[[nodiscard]] constexpr bool can_advance(UploadState from, UploadState to) {
    using S = UploadState;
    switch (from) {
    case S::Writing: return to == S::Committed;
    case S::Committed: return to == S::InFlight || to == S::Complete || to == S::Failed;
    case S::InFlight: return to == S::Complete || to == S::Failed;
    default: return false; // Free leaves through acquire(), terminal states through release()
    }
}

/// What upload_status reports for a record in `s`.
constexpr UploadStatus status_of(UploadState s) {
    return s == UploadState::Complete ? UploadStatus::Complete
           : s == UploadState::Failed ? UploadStatus::Failed
                                      : UploadStatus::Pending;
}

/// A fixed number of upload records holding the adapter's own data `T`. A token is the record's
/// generation and index; release() bumps the generation, so the token then finds nothing. Not
/// thread-safe: the adapter guards acquire() and release() with its own lock.
template <class T> class UploadPool {
public:
    UploadPool() noexcept = default;
    UploadPool(Allocator const* alloc, u32 capacity)
        : records_(alloc ? alloc : default_allocator(), Tag::Payload),
          free_(alloc ? alloc : default_allocator(), Tag::Payload) {
        records_.resize(capacity);
        free_.reserve(capacity);
        for (u32 i = capacity; i-- > 0;)
            free_.push_back(i); // index 0 is handed out first
    }

    /// A free record, reset to T{} and Writing, or kInvalid when every record is in use (Busy:
    /// uploads in flight give theirs back).
    [[nodiscard]] u32 acquire() noexcept {
        if (free_.empty()) return kInvalid;
        u32 const i = free_.back();
        free_.pop_back();
        records_[i].data  = T{};
        records_[i].state = UploadState::Writing;
        return i;
    }
    /// Back to the free list; the token of the record goes stale. From a terminal state only.
    void release(u32 index) noexcept {
        KILN_ASSERT(records_[index].state == UploadState::Complete ||
                    records_[index].state == UploadState::Failed);
        free_record(index);
    }
    /// discard_upload: kiln never committed the record, so it goes straight back from Writing.
    void discard(u32 index) noexcept {
        KILN_ASSERT(records_[index].state == UploadState::Writing);
        free_record(index);
    }

    u64 token(u32 index) const noexcept { return (u64(records_[index].gen) << 32) | (index + 1); }
    /// The record `token` names, or kInvalid if it is stale, free or not a token of this pool.
    u32 index_of(u64 token) const noexcept {
        u32 const i = u32(token & 0xFFFFFFFFu) - 1; // token 0 wraps to kInvalid
        if (i >= records_.size()) return kInvalid;
        Record const& r = records_[i];
        return r.state != UploadState::Free && r.gen == u32(token >> 32) ? i : kInvalid;
    }

    UploadState state(u32 index) const noexcept { return records_[index].state; }
    void advance(u32 index, UploadState to) noexcept {
        KILN_ASSERT(can_advance(records_[index].state, to) && "UploadPool: illegal transition");
        records_[index].state = to;
    }

    T& operator[](u32 index) noexcept { return records_[index].data; }
    T const& operator[](u32 index) const noexcept { return records_[index].data; }
    u32 capacity() const noexcept { return u32(records_.size()); }
    u32 in_use() const noexcept { return capacity() - u32(free_.size()); }
    [[nodiscard]] bool full() const noexcept { return free_.empty(); } ///< acquire() would fail

private:
    void free_record(u32 index) noexcept {
        Record& r = records_[index];
        r.state   = UploadState::Free;
        r.gen     = r.gen + 1 == 0 ? 1 : r.gen + 1;
        free_.push_back(index);
    }

    struct Record {
        T data{};
        UploadState state = UploadState::Free;
        u32 gen           = 1;
    };
    Vec<Record> records_;
    Vec<u32> free_;
};

} // namespace kiln::ex
