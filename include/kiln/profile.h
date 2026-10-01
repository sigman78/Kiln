// kiln/profile.h — hooks that report kiln's work to the host's profiler (Tracy, Superluminal,
// Perfetto, a trace file). Off unless the host sets them. Design: docs/design/cook-tracing.md.
#pragma once

#include "kiln/core.h"

namespace kiln {

/// Callbacks into the host's profiler; each one is optional. kiln calls them from any of its
/// threads, so they must be thread-safe, and they must not call kiln. `name` is a static string
/// (for example "kiln.meta" or "cook.encode", listed in the design note); `asset` is the asset name,
/// valid only during the call, and may be empty.
struct ProfileHooks {
    /// A zone on the calling thread. Zones nest per thread and end in reverse order.
    void (*zone_begin)(void* user, char const* name, StrView asset) = nullptr;
    void (*zone_end)(void* user, char const* name, StrView asset)   = nullptr;
    /// A finished span between two profile_now_ns() readings, which may have been taken on other
    /// threads: a wait in a queue, a GPU copy, a whole load. Reported once, after `endNs`.
    void (*interval)(void* user, char const* name, StrView asset, u64 beginNs, u64 endNs) = nullptr;
    void* user                                                                            = nullptr;
};

/// Nanoseconds of the steady clock that `interval` spans use.
KILN_API u64 profile_now_ns();

/// zone_begin now, zone_end at scope exit. With null hooks it costs one branch.
class ProfileZone {
public:
    ProfileZone(ProfileHooks const* hooks, char const* name, StrView asset = {})
        : hooks_(hooks && hooks->zone_begin ? hooks : nullptr), name_(name), asset_(asset) {
        if (hooks_) hooks_->zone_begin(hooks_->user, name_, asset_);
    }
    ~ProfileZone() {
        if (hooks_ && hooks_->zone_end) hooks_->zone_end(hooks_->user, name_, asset_);
    }
    ProfileZone(ProfileZone const&)            = delete;
    ProfileZone& operator=(ProfileZone const&) = delete;

private:
    ProfileHooks const* hooks_;
    char const* name_;
    StrView asset_;
};

/// Reports an interval if the hooks take intervals.
inline void profile_interval(ProfileHooks const* hooks, char const* name, StrView asset, u64 beginNs,
                             u64 endNs) {
    if (hooks && hooks->interval) hooks->interval(hooks->user, name, asset, beginNs, endNs);
}

} // namespace kiln
