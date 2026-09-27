// Single translation unit that compiles the wuffs release file exactly once,
// with only the modules kiln_cook's PNG decode path needs. This keeps the
// compiled object small: no GIF/BMP/JPEG/etc decoders are pulled in.
//
// See third_party/README.md for the exact release file and commit this was
// vendored from.
#define WUFFS_IMPLEMENTATION

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#include "wuffs-v0.4.c"
