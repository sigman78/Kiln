// tools/no_crash_dialogs.cpp — the Windows switches behind no_crash_dialogs().
#include "no_crash_dialogs.h"

#if defined(_WIN32)
#include <windows.h> // WIN32_LEAN_AND_MEAN / NOMINMAX set by kiln_apply_defaults

#include <crtdbg.h>
#include <cstdlib>
#endif

namespace kiln {

void no_crash_dialogs() noexcept {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // No-ops with a release CRT.
    int const types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (int type : types) {
        (void)_CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        (void)type; // the calls above vanish with a release CRT
    }
#endif
}

} // namespace kiln
