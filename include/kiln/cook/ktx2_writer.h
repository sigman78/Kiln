// kiln/cook/ktx2_writer.h — KTX2 writer for uncompressed and BC1-BC7 2D, cube and array textures
// with mips, optionally Zstd-supercompressed.
// Identical input gives byte-identical output (no timestamps, zeroed padding).
// The DFD matches what libktx's vk2dfd produces for the same vkFormat.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/ktx2.h"

namespace kiln::ktx2 {

/// Extra key/value entry. The writer appends the terminating NUL to the value
/// (KTX convention for string values) and sorts entries by key.
struct KeyValue {
    StrView key   = {}; ///< ASCII, no NUL, unique
    StrView value = {}; ///< bytes; a NUL is appended
};

struct WriteDesc {
    Format format = Format::Undefined; ///< uncompressed or BC1-BC7 (not ETC2 / ASTC yet)
    u32 width     = 0;
    u32 height    = 0;
    u32 layers    = 1;     ///< array layers; more than 1 needs isArray
    u32 faces     = 1;     ///< 1, or 6 for a cube (+X, -X, +Y, -Y, +Z, -Z; square)
    bool isArray  = false; ///< writes layerCount = layers (an array, possibly of one layer)
    /// Level 0 (largest) first. Each level holds tightly packed rows of every layer, and in each
    /// layer every face: layers * faces * format_image_bytes(format, max(width >> i, 1),
    /// max(height >> i, 1)) bytes.
    Span<Span<u8 const> const> levels = {};
    /// Value of the KTXwriter key. Must not contain NUL.
    StrView writerTag       = "kiln-cook";
    bool premultipliedAlpha = false; ///< sets the DFD alpha-premultiplied flag
    /// Additional key/value entries (e.g. kiln.sourceHash). Keys must not collide
    /// with KTXwriter or each other. At most 15.
    Span<KeyValue const> extraKeys = {};
    /// 1..22: each level is one Zstd frame of this level (supercompressionScheme 2), packed
    /// without padding. 0 stores the levels as they are.
    u32 zstdLevel = 0;
    /// With zstdLevel: the levels stay plain unless Zstd shrinks the file by at least this
    /// fraction, counted in 4 KiB disk blocks (so a file under 4 KiB stays plain). In [0, 1);
    /// 0 keeps Zstd whatever it saves.
    f32 zstdMinSaving = 0.0f;
};

/// Serialize a KTX2 file into a new buffer allocated from `alloc` (Tag::Cook;
/// nullptr means default_allocator()); the Zstd encoder allocates from it too. Invalid input
/// returns InvalidArgument and emits one K41xx diagnostic.
KILN_API Result<Vec<u8>> write(WriteDesc const& desc, Allocator const* alloc,
                               DiagSink const* diag = nullptr) noexcept;

} // namespace kiln::ktx2
