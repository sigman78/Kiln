// Module selection for the vendored wuffs release file. Included before "wuffs-v0.4.c" by
// wuffs_impl.c (implementation) and by kiln_cook's image decoder (declarations only).
// KILN_WEBP comes from the CMake option of the same name.
#pragma once

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE

// PNG: DEFLATE inside zlib, ADLER32 for zlib, CRC32 for chunk checksums.
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#define WUFFS_CONFIG__MODULE__JPEG

#if defined(KILN_WEBP) && KILN_WEBP
// VP8 is the lossy WebP bitstream; the WEBP module does lossless itself.
#define WUFFS_CONFIG__MODULE__VP8
#define WUFFS_CONFIG__MODULE__WEBP
#endif
