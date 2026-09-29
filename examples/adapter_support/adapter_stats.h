// examples/adapter_support/adapter_stats.h — the counters every example adapter reports, so their
// numbers compare across graphics APIs.
#pragma once

#include <kiln/core.h>

namespace kiln::ex {

struct AdapterStats {
    u32 liveObjects      = 0; ///< objects made and not yet destroyed
    u32 uploadsPending   = 0; ///< begun and not yet reported Complete or Failed
    u32 uploadsFailed    = 0; ///< reported Failed
    u32 uploadsDiscarded = 0; ///< discard_upload: kiln's write failed before commit
    u64 bytesCommitted   = 0; ///< bytes of every committed upload
    u64 stagingUsed      = 0; ///< staging bytes held now
    u64 stagingSize      = 0; ///< 0: no fixed staging memory (per-upload CPU memory)
    // Why begin_upload said Busy (each is a retry kiln made later).
    u32 busyStaging = 0; ///< the staging ring was full
    u32 busyHeap    = 0; ///< a GPU heap had no room (mesh heap, texture heap)
    u32 busyUploads = 0; ///< every upload record was in use
};

/// One Info line with every counter, tagged `who` (the example's log tag).
void log_adapter_stats(char const* who, AdapterStats const& s) noexcept;

} // namespace kiln::ex
