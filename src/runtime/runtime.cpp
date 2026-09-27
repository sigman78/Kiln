// kiln_runtime — placeholder translation unit for M0. The registry, request
// scheduler, IO backend and sources arrive in M3 (docs/HANDOFF.md §11.3).
#include "kiln/core.h"

namespace kiln {

char const* runtime_version() noexcept { return "0.0.1-m0"; }

} // namespace kiln
