#include "kiln/profile.h"

#include <chrono>

namespace kiln {

u64 profile_now_ns() noexcept {
    auto const t = std::chrono::steady_clock::now().time_since_epoch();
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

} // namespace kiln
