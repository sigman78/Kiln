// Must not compile under warnings-as-errors: Status is [[nodiscard]] (tests/CMakeLists.txt).
#include <kiln/result.h>

namespace {
kiln::Status make() { return kiln::make_status(kiln::Code::IoError); }
} // namespace

void discard_status_probe() { make(); }
