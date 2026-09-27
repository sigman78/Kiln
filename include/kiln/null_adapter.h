// kiln/null_adapter.h — CPU-only adapter for tests and tools. Uploads land in
// allocator memory, complete immediately (kSelfSubmitting), and every call is
// counted so tests can assert the contract. Part of kiln_runtime (no GPU API).
#pragma once

#include "kiln/adapter.h"
#include "kiln/alloc.h"

namespace kiln {

struct NullAdapterDesc {
    Allocator const* alloc = nullptr; ///< nullptr = default allocator (Tag::Payload)
    u32 busyEveryN         = 0;       ///< testing: every N-th begin_upload returns Busy (0 = never)
    u32 failEveryN         = 0;       ///< testing: every N-th begin_upload returns Unsupported (0 = never)
    bool bindless          = true;    ///< acquire() hands out slot indices; publish() rebinds them
    u64 rowPitchAlign      = 1;       ///< reported copy constraint
    u64 offsetAlign        = 16;
    u32 maxObjects         = 4096;
};

struct NullAdapterStats {
    u32 acquires = 0, beginUploads = 0, busyReturned = 0, commits = 0, completes = 0, publishes = 0,
        destroys      = 0;
    u64 bytesUploaded = 0;
    u32 liveObjects   = 0; ///< uploaded objects not yet destroyed
};

struct NullAdapter; ///< opaque

/// Create; the returned Adapter is valid until destroy. `out` must outlive users.
[[nodiscard]] KILN_API Result<NullAdapter*> null_adapter_create(NullAdapterDesc const& desc,
                                                                Adapter* out) noexcept;
KILN_API void null_adapter_destroy(NullAdapter* na) noexcept;

/// The bytes kiln wrote for `obj` (empty if unknown or destroyed).
[[nodiscard]] KILN_API Span<u8 const> null_adapter_payload(NullAdapter* na, GpuObject obj) noexcept;
/// The object currently bound to a bindless `slot` (null if none / not bindless).
[[nodiscard]] KILN_API GpuObject null_adapter_slot(NullAdapter* na, u32 slot) noexcept;
[[nodiscard]] KILN_API NullAdapterStats null_adapter_stats(NullAdapter* na) noexcept;
/// Objects passed to destroy_deferred are kept until this is called (simulates
/// frames in flight); returns how many were freed.
KILN_API u32 null_adapter_flush_deferred(NullAdapter* na) noexcept;

} // namespace kiln
