// Must not compile under warnings-as-errors: Result<T> is [[nodiscard]] (tests/CMakeLists.txt).
#include <kiln/result.h>

namespace {
kiln::Result<int> make() { return 1; }
} // namespace

void discard_result_probe() { make(); }
