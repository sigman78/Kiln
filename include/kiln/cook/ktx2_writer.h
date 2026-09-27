// kiln/cook/ktx2_writer.h — KTX2 writer for raw 2D textures (kiln_cook).
//
// Writes uncompressed, non-supercompressed 2D textures with a mip chain. Output is
// byte-identical for identical input: no timestamps, every padding byte is zero.
// The DFD matches what libktx's vk2dfd produces for the same vkFormat.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/ktx2.h"

namespace kiln::ktx2 {

struct WriteDesc {
    Format format = Format::Undefined; ///< v0.5: uncompressed formats only
    u32 width     = 0;
    u32 height    = 0;
    /// Level 0 (largest) first. Each level holds tightly packed rows and exactly
    /// format_image_bytes(format, max(width >> i, 1), max(height >> i, 1)) bytes.
    Span<Span<u8 const> const> levels;
    /// Value of the KTXwriter key. A fixed string, never a timestamp, so output is
    /// deterministic. Must not contain NUL.
    StrView writerTag       = "kiln-cook";
    bool premultipliedAlpha = false; ///< sets the DFD alpha-premultiplied flag
};

/// Serialize a KTX2 file into a new buffer allocated from `alloc` (Tag::Cook;
/// nullptr means default_allocator()). Invalid input returns InvalidArgument and
/// emits one K41xx diagnostic.
[[nodiscard]] KILN_API Result<Vec<u8>> write(WriteDesc const& desc, Allocator const* alloc,
                                             DiagSink const* diag = nullptr) noexcept;

} // namespace kiln::ktx2
