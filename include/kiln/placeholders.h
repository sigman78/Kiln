// kiln/placeholders.h — built-in placeholder images per texture kind and the Failed checker.
// Uploaded through the adapter at create() for each supported shape, under reserved ids 1..15.
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
KILN_API PlaceholderImage builtin_placeholder(TextureKind kind) noexcept;
/// The 8x8 magenta/black checker served by a Failed texture in dev builds.
KILN_API PlaceholderImage builtin_failed_placeholder() noexcept;

/// Reserved asset id of a kind placeholder: 1..12, four kinds per shape (Tex2D 1..4).
constexpr AssetId placeholder_asset_id(TextureKind kind, TextureShape shape = TextureShape::Tex2D) noexcept {
    return kFirstPlaceholderId + u64(shape) * u64(TextureKind::Count) + u64(kind);
}
/// Reserved asset id of the Failed placeholder of a shape: 13..15.
constexpr AssetId failed_placeholder_id(TextureShape shape = TextureShape::Tex2D) noexcept {
    return kFirstPlaceholderId + u64(TextureShape::Count) * u64(TextureKind::Count) + u64(shape);
}
static_assert(failed_placeholder_id(TextureShape(u32(TextureShape::Count) - 1)) == kLastPlaceholderId);

} // namespace kiln
