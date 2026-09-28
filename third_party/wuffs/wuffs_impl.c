// Compiles the wuffs release file once, with only the modules in wuffs_modules.h.
// See third_party/README.md for the vendored release file and commit.
#define WUFFS_IMPLEMENTATION
#include "wuffs_modules.h"

#include "wuffs-v0.4.c"
