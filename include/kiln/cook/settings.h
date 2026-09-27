// kiln/cook/settings.h — v0.5 cook settings structs, resolution and hashing.
// Design: docs/design/settings.md. Only the C++ structs exist in v0.5; config
// files, presets, rules and sidecars land in v0.6 on top of them.
#pragma once

#include "kiln/hash.h"
#include "kiln/result.h"

namespace kiln::cook {

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

enum class ColorSpace : u8 { Auto = 0, Srgb, Linear };

/// What a texture *is*. Drives color space, channel layout, mip filtering and
/// (later) encoding. Inferred from the glTF material slot when Auto.
enum class TextureUsage : u8 { Auto = 0, Color, Normal, Orm, Mask, Hdr, Ui, Lut, Height };

struct TextureCookSettings {
    ColorSpace colorSpace  = ColorSpace::Auto;   ///< Auto: sRGB for Color/Ui, else Linear
    TextureUsage usage     = TextureUsage::Auto; ///< Auto: inferred from the glTF slot, else Color
    bool genMips           = true;
    bool normalRenormalize = true; ///< only for usage == Normal; cleared otherwise by resolve_texture
    u32 maxSize            = 0;    ///< 0 = no limit (the target cap still applies)
    bool flipGreen = false; ///< DirectX-style normal maps; only for Normal (warning + cleared otherwise)
    // reserved: alphaMode, premultiply, dilation, encoding, supercompression, residentMips, shape
};

/// The glTF material slot a texture was referenced from (for usage inference).
enum class SlotHint : u8 { None = 0, BaseColor, Normal, MetallicRoughness, Occlusion, Emissive };

// ---------------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------------

enum class VertexProfile : u8 { Default = 0, Precise };

enum class CompressionScheme : u8 {
    None        = 0, ///< every blob codec None; the cooker sets kPayloadRaw
    Basic       = 1, ///< Zstd + ByteShuffle (vertex), Zstd (index)      (post-v0.5)
    Meshopt     = 2, ///< MeshoptVertex / MeshoptIndex                    (post-v0.5)
    MeshoptZstd = 3, ///< Meshopt + kBlobOuterZstd                        (post-v0.5)
};

struct MeshCookSettings {
    VertexProfile profile = VertexProfile::Default;
    bool genTangents      = true;
    bool optimize         = true;  ///< vertex cache, overdraw, vertex fetch
    bool useAuthoredLods  = true;  ///< pass through `_lodN` nodes
    bool genLods          = false; ///< reserved: simplifier lands in v0.6; true is a K3xxx error
    f32 posTolMm          = 0.1f;  ///< quantization tolerance before falling back to float positions
    f32 weldTol           = 0.0f;  ///< 0 = exact-match welding only

    // Compression group (reserved, post-v0.5; mesh-format-spec §5.9)
    CompressionScheme compression = CompressionScheme::None; ///< only None accepted in v0.5
    u8 zstdLevel                  = 0;                       ///< 0 = library default
    u32 blobChunkSize             = 0;                       ///< reserved; non-zero is a K3xxx error in v0.5
    // reserved: indexWidthPolicy, unit/axis override, name prefixes to strip
};

// ---------------------------------------------------------------------------
// Target and session
// ---------------------------------------------------------------------------

struct TargetProfile {
    StrView name                   = "desktop";
    u32 maxTextureSize             = 16384;
    VertexProfile maxVertexProfile = VertexProfile::Precise; ///< highest profile the target accepts
};

enum class StoreMode : u8 { Disk = 0, Memory, None }; ///< store / cache-less / validate only

struct CookSession {
    StoreMode storeMode = StoreMode::Disk;
    bool fastPreview    = false; ///< cheaper settings for cache-less previews (no optimize, no tangents)
};

// ---------------------------------------------------------------------------
// Diagnostics (K3000-K3999: settings resolution). See docs/diagnostics.md.
// ---------------------------------------------------------------------------

enum SettingsDiagCode : u32 {
    kDiagSettingsUnsupported =
        3001, ///< a reserved feature was requested (genLods, compression != None, blobChunkSize)
    kDiagSettingsInvalidCombo = 3002,    ///< fields contradict each other (e.g. normalRenormalize with a
                                         ///< non-normal usage is ignored: Warning)
    kDiagSettingsClampedByTarget = 3003, ///< profile or size clamped by the target (Warning)
    kDiagSettingsEnumRange       = 3004, ///< an enum field holds a value outside its range
};

// ---------------------------------------------------------------------------
// Resolution (HANDOFF §5.1 layers 1, 2 and 7 exist in v0.5)
// ---------------------------------------------------------------------------

/// Resolve texture settings: defaults <- inference from `hint` <- explicit non-Auto
/// fields of `overrides` <- session <- target caps. Every Auto field is concrete on
/// return. Invalid combinations return InvalidArgument with a K3xxx diagnostic.
KILN_API Result<TextureCookSettings> resolve_texture(TextureCookSettings const& overrides, SlotHint hint,
                                                     TargetProfile const& target, CookSession const& session,
                                                     DiagSink const* diag = nullptr,
                                                     StrView asset        = {}) noexcept;

/// Resolve mesh settings: defaults <- overrides <- session <- target caps.
KILN_API Result<MeshCookSettings> resolve_mesh(MeshCookSettings const& overrides, TargetProfile const& target,
                                               CookSession const& session, DiagSink const* diag = nullptr,
                                               StrView asset = {}) noexcept;

/// Usage inferred from a glTF slot (None -> Color).
[[nodiscard]] constexpr TextureUsage usage_from_slot(SlotHint hint) noexcept {
    switch (hint) {
    case SlotHint::Normal: return TextureUsage::Normal;
    case SlotHint::MetallicRoughness:
    case SlotHint::Occlusion: return TextureUsage::Orm;
    case SlotHint::None:
    case SlotHint::BaseColor:
    case SlotHint::Emissive: return TextureUsage::Color;
    }
    return TextureUsage::Color;
}
/// Default color space for a usage.
[[nodiscard]] constexpr ColorSpace color_space_for(TextureUsage u) noexcept {
    return (u == TextureUsage::Color || u == TextureUsage::Ui) ? ColorSpace::Srgb : ColorSpace::Linear;
}

// ---------------------------------------------------------------------------
// Hashing (field by field, schema-versioned; never memcpy of a struct)
// ---------------------------------------------------------------------------

inline constexpr u32 kTextureSettingsSchema = 1; ///< bump when a field is added or changes meaning
inline constexpr u32 kMeshSettingsSchema    = 1;
inline constexpr u32 kTargetSchema          = 1;

[[nodiscard]] KILN_API u64 hash_settings(TextureCookSettings const& s) noexcept;
[[nodiscard]] KILN_API u64 hash_settings(MeshCookSettings const& s) noexcept;
[[nodiscard]] KILN_API u64 hash_target(TargetProfile const& t) noexcept;

/// Enum <-> string for tools and diagnostics.
[[nodiscard]] KILN_API char const* texture_usage_name(TextureUsage u) noexcept;
[[nodiscard]] KILN_API char const* color_space_name(ColorSpace c) noexcept;
[[nodiscard]] KILN_API char const* vertex_profile_name(VertexProfile p) noexcept;
[[nodiscard]] KILN_API char const* slot_hint_name(SlotHint h) noexcept;

} // namespace kiln::cook
