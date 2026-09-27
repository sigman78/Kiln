// kiln/placeholders.h — built-in placeholder images per texture kind and the Failed checker.
// Uploaded through the adapter at create(), under reserved asset ids 1..15.
// See docs/design/handles-and-states.md (Placeholders).
#pragma once

#include "kiln/adapter.h"

namespace kiln {

/// A built-in placeholder's pixel data: one level, one layer, tightly packed.
struct PlaceholderImage {
    Format format = Format::Undefined;
    u32 width     = 0;
    u32 height    = 0;
    Span<u8 const> pixels;
};

/// The 1x1 placeholder texel for a texture kind (handles-and-states.md table).
[[nodiscard]] KILN_API PlaceholderImage builtin_placeholder(TextureKind kind) noexcept;
/// The 8x8 magenta/black checker served by a Failed texture in dev builds.
[[nodiscard]] KILN_API PlaceholderImage builtin_failed_placeholder() noexcept;

/// Reserved asset id for a kind placeholder: BaseColor=1 .. Emissive=4.
[[nodiscard]] constexpr AssetId placeholder_asset_id(TextureKind kind) noexcept {
    return kFirstPlaceholderId + u64(kind);
}
/// Reserved asset id for the Failed placeholder.
inline constexpr AssetId kFailedPlaceholderId = kLastPlaceholderId;

} // namespace kiln
