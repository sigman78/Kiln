// tools/no_crash_dialogs.h — for kiln's own programs (tests, tools, examples), never the library: a
// fatal error ends the process with a message and an exit code instead of a dialog box.
#pragma once

namespace kiln {

/// Call first in main(). On Windows: crashes skip Windows Error Reporting, abort() (KILN_PANIC) shows
/// no box, and debug CRT asserts print to stderr. Does nothing elsewhere. Process-wide.
void no_crash_dialogs();

} // namespace kiln
