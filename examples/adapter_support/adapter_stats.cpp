// examples/adapter_support/adapter_stats.cpp — log_adapter_stats (adapter_stats.h).
#include "adapter_stats.h"

#include <kiln/log.h>

namespace kiln::ex {

void log_adapter_stats(char const* who, AdapterStats const& s) noexcept {
    using ull = unsigned long long;
    KILN_INFO(who,
              "adapter: %u live objects, %u uploads pending, %u failed, %u discarded, %llu bytes committed, "
              "staging %llu of %llu bytes, busy: %u staging, %u heap, %u upload records",
              s.liveObjects, s.uploadsPending, s.uploadsFailed, s.uploadsDiscarded, ull(s.bytesCommitted),
              ull(s.stagingUsed), ull(s.stagingSize), s.busyStaging, s.busyHeap, s.busyUploads);
}

} // namespace kiln::ex
