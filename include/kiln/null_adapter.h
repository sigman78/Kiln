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
    u32 bindlessSlots      = 4096;    ///< Adapter::bindlessSlots; 0 = not bindless
    u64 rowPitchAlign      = 1;       ///< reported copy constraint
    u64 offsetAlign        = 16;
    u32 maxObjects         = 4096;
};

struct NullAdapterStats {
    u32 beginUploads = 0, busyReturned = 0, commits = 0, completes = 0, uploadsFailed = 0, binds = 0,
        destroys      = 0;
    u64 bytesUploaded = 0;
    u32 liveObjects   = 0; ///< uploaded objects not yet destroyed
};

struct NullAdapter;

/// Fills `out`, which stays valid until null_adapter_destroy(). `out` must outlive its users.
[[nodiscard]] KILN_API Result<NullAdapter*> null_adapter_create(NullAdapterDesc const& desc,
                                                                Adapter* out) noexcept;
KILN_API void null_adapter_destroy(NullAdapter* na) noexcept;

/// The bytes kiln wrote for `obj` (empty if unknown or destroyed).
[[nodiscard]] KILN_API Span<u8 const> null_adapter_payload(NullAdapter* na, GpuObject obj) noexcept;
/// The object last bound to a bindless `slot` (null if never bound / not bindless).
[[nodiscard]] KILN_API GpuObject null_adapter_slot(NullAdapter* na, u32 slot) noexcept;
[[nodiscard]] KILN_API NullAdapterStats null_adapter_stats(NullAdapter* na) noexcept;
/// Testing: while `fail` is true, every upload committed reports UploadStatus::Failed.
KILN_API void null_adapter_fail_uploads(NullAdapter* na, bool fail) noexcept;

} // namespace kiln
