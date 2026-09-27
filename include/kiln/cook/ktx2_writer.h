// kiln/cook/ktx2_writer.h — KTX2 writer for uncompressed 2D textures with mips.
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
    Format format = Format::Undefined; ///< v0.5: uncompressed formats only
    u32 width     = 0;
    u32 height    = 0;
    /// Level 0 (largest) first. Each level holds tightly packed rows and exactly
    /// format_image_bytes(format, max(width >> i, 1), max(height >> i, 1)) bytes.
    Span<Span<u8 const> const> levels;
    /// Value of the KTXwriter key. Must not contain NUL.
    StrView writerTag       = "kiln-cook";
    bool premultipliedAlpha = false; ///< sets the DFD alpha-premultiplied flag
    /// Additional key/value entries (e.g. kiln.sourceHash). Keys must not collide
    /// with KTXwriter or each other. At most 15.
    Span<KeyValue const> extraKeys = {};
};

/// Serialize a KTX2 file into a new buffer allocated from `alloc` (Tag::Cook;
/// nullptr means default_allocator()). Invalid input returns InvalidArgument and
/// emits one K41xx diagnostic.
[[nodiscard]] KILN_API Result<Vec<u8>> write(WriteDesc const& desc, Allocator const* alloc,
                                             DiagSink const* diag = nullptr) noexcept;

} // namespace kiln::ktx2
