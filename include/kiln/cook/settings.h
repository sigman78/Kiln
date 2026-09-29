// kiln/cook/settings.h — v0.5 cook settings structs, resolution and hashing.
// Design: docs/design/settings.md.
#pragma once

#include "kiln/hash.h"
#include "kiln/result.h"

namespace kiln::cook {

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

enum class ColorSpace : u8 { Auto = 0, Srgb, Linear };

/// What a texture *is*. Drives color space, channel layout, mip filtering and
/// encoding. Inferred from the glTF material slot when Auto.
enum class TextureUsage : u8 { Auto = 0, Color, Normal, Orm, Mask, Hdr, Ui, Lut, Height };
/// The shape to cook (docs/design/texture-shapes.md). Auto stays Auto after resolution: it
/// means "from the source", i.e. the shape of a KTX2 source, else Tex2D.
enum class CookShape : u8 { Auto = 0, Tex2D, Cube, Array };

/// The stored texel format (docs/design/bcn-encoding.md). Auto follows the usage table for the
/// target's block family (none: uncompressed) and stays Auto after resolution, because a mask's
/// BC4 or BC5 depends on the source's channel count. BC1 drops alpha; BC6H is unsigned (UFLOAT).
enum class TextureEncoding : u8 { Auto = 0, Uncompressed, BC1, BC3, BC4, BC5, BC6H, BC7 };
/// Encoder effort. Fast is for previews; High costs several times Normal for a small gain.
enum class EncodeQuality : u8 { Fast = 0, Normal, High };
/// Lossless compression of the stored levels (docs/design/bcn-encoding.md, "Zstd"). It shrinks
/// the file and the bytes read at load; GPU memory stays the same.
enum class Supercompression : u8 { None = 0, Zstd };
/// Resolved zstdLevel when it is 0, and under CookSession::fastPreview.
inline constexpr u8 kDefaultZstdLevel = 3;
inline constexpr u8 kPreviewZstdLevel = 1;
inline constexpr u8 kMaxZstdLevel     = 19;

struct TextureCookSettings {
    ColorSpace colorSpace  = ColorSpace::Auto;   ///< Auto: sRGB for Color/Ui, else Linear
    TextureUsage usage     = TextureUsage::Auto; ///< Auto: inferred from the glTF slot, else Color
    bool genMips           = true;
    bool normalRenormalize = true; ///< only for usage == Normal; cleared otherwise by resolve_texture
    u32 maxSize            = 0;    ///< 0 = no limit (the target cap still applies)
    bool flipGreen = false; ///< DirectX-style normal maps; only for Normal (warning + cleared otherwise)
    /// Cube and Array cut the source image into a vertical strip of slices, slice 0 at the top.
    CookShape shape = CookShape::Auto;
    u32 slices = 0; ///< Array: layers in the strip; 0 = square slices. Only for Array (cleared otherwise)
    TextureEncoding encoding          = TextureEncoding::Auto;
    EncodeQuality quality             = EncodeQuality::Normal; ///< resolved to Normal when nothing is encoded
    Supercompression supercompression = Supercompression::Zstd;
    u8 zstdLevel                      = 0; ///< 1..19; 0 = kDefaultZstdLevel. Resolved to 0 without Zstd
    // reserved: alphaMode, premultiply, dilation, residentMips
};

/// The glTF material slot a texture was referenced from (for usage inference).
enum class SlotHint : u8 { None = 0, BaseColor, Normal, MetallicRoughness, Occlusion, Emissive };

/// Name rule (resolution layer 5) for a standalone texture source: a file whose stem ends
/// with `suffix` has `usage` and `shape` (Auto: the rule says nothing about it). Suffixes stack:
/// after a match the suffix is removed and the rules match again, each rule at most once, so
/// `rock_array_n` is an Array of Normal. Matching ignores ASCII case; in each round the first
/// matching rule wins, and the first rule that sets a field wins it.
struct NameRule {
    StrView suffix     = {};
    TextureUsage usage = TextureUsage::Auto;
    CookShape shape    = CookShape::Auto;
};

/// Built-in name rules. Hosts replace or extend them through ProviderDesc::nameRules.
inline constexpr NameRule kDefaultNameRules[] = {
    {"_n", TextureUsage::Normal},
    {"_nrm", TextureUsage::Normal},
    {"_normal", TextureUsage::Normal},
    {"_orm", TextureUsage::Orm},
    {"_arm", TextureUsage::Orm},
    {"_mr", TextureUsage::Orm},
    {"_metallicroughness", TextureUsage::Orm},
    {"_roughnessmetallic", TextureUsage::Orm},
    {"_occlusion", TextureUsage::Orm},
    {"_ao", TextureUsage::Orm},
    {"_mask", TextureUsage::Mask},
    {"_height", TextureUsage::Height},
    {"_basecolor", TextureUsage::Color},
    {"_albedo", TextureUsage::Color},
    {"_diffuse", TextureUsage::Color},
    {"_emissive", TextureUsage::Color},
    {"_cube", TextureUsage::Auto, CookShape::Cube},
    {"_array", TextureUsage::Auto, CookShape::Array},
};

/// The usage of hints_from_name(): Auto if no rule sets one.
[[nodiscard]] KILN_API TextureUsage usage_from_name(StrView path, Span<NameRule const> rules) noexcept;

struct NameHints {
    TextureUsage usage = TextureUsage::Auto;
    CookShape shape    = CookShape::Auto;
};
/// Usage and shape from the stacked suffixes of the file stem of `path` (see NameRule).
[[nodiscard]] KILN_API NameHints hints_from_name(StrView path, Span<NameRule const> rules) noexcept;

// ---------------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------------

/// Default: quantized; Precise: float positions and UVs; Float: every attribute float except color
/// (mesh-format-spec §6). Ordered by size: a target caps the profile (TargetProfile).
enum class VertexProfile : u8 { Default = 0, Precise, Float };

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

/// The block-compressed formats a target's GPUs sample. None: textures stay uncompressed.
enum class BlockFamily : u8 { None = 0, BC };

struct TargetProfile {
    StrView name                   = "desktop";
    BlockFamily blockFamily        = BlockFamily::BC;
    u32 maxTextureSize             = 16384;
    VertexProfile maxVertexProfile = VertexProfile::Float; ///< highest profile the target accepts
    u32 maxArrayLayers             = 2048;                 ///< more layers is an error (K2004), not a clamp
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
    kDiagSettingsInvalidCombo = 3002,    ///< fields contradict each other or hold invalid values (e.g.
                                         ///< flipGreen with a non-Normal usage: Warning)
    kDiagSettingsClampedByTarget = 3003, ///< profile, size or encoding clamped by the target (Warning)
    kDiagSettingsEnumRange       = 3004, ///< an enum field holds a value outside its range
    kDiagSidecarSyntax           = 3005, ///< a `.kiln` sidecar is outside the TOML subset (ParseError)
    kDiagSidecarKey =
        3006, ///< a sidecar key is unknown, or its value has the wrong type or range (InvalidArgument)
    kDiagPolicyRefused = 3007, ///< the CookPolicy refused the asset (its status)
};

// ---------------------------------------------------------------------------
// Resolution: the last stage, after every layer (resolve_*_layers call these)
// ---------------------------------------------------------------------------

/// Resolve texture settings: an Auto usage from `hint`, an Auto color space from the usage,
/// Normal-only flags cleared for other usages, target caps. Every Auto field except `shape` and
/// `encoding` is concrete on return. An enum value out of range returns
/// InvalidArgument (K3004); an encoding the usage cannot take, InvalidArgument (K3002). An
/// explicit BC encoding on a target without the BC family becomes
/// Uncompressed (K3003). `session.fastPreview` sets `quality` to Fast and a Zstd level to
/// kPreviewZstdLevel. A zstdLevel above kMaxZstdLevel returns InvalidArgument (K3002).
KILN_API Result<TextureCookSettings> resolve_texture(TextureCookSettings const& overrides, SlotHint hint,
                                                     TargetProfile const& target, CookSession const& session,
                                                     DiagSink const* diag = nullptr,
                                                     StrView asset        = {}) noexcept;

/// Resolve mesh settings: validation, session, target caps.
KILN_API Result<MeshCookSettings> resolve_mesh(MeshCookSettings const& overrides, TargetProfile const& target,
                                               CookSession const& session, DiagSink const* diag = nullptr,
                                               StrView asset = {}) noexcept;

// ---------------------------------------------------------------------------
// Layered resolution (docs/design/settings.md, "Resolution layers")
// ---------------------------------------------------------------------------

/// The asset being cooked, as a CookPolicy sees it.
struct CookAssetInfo {
    StrView name       = {};             ///< the asset name, e.g. "props/chair.glb#wood"
    StrView sourcePath = {};             ///< the file the cook reads; for an embedded image, its model
    SlotHint slot      = SlotHint::None; ///< textures: the glTF slot of an embedded image
};

/// The host's last word on settings (layer 6), called once per cooked asset after every other
/// layer and before validation. It may change any field, or refuse the asset with a failed
/// Status (K3007). It runs on worker threads, possibly concurrently, so it must be thread-safe.
/// It must be deterministic: the same asset, settings and target give the same result.
/// A null function leaves that kind unchanged.
struct CookPolicy {
    Status (*texture)(void* user, CookAssetInfo const& asset, TargetProfile const& target,
                      TextureCookSettings* s, DiagSink const* diag) = nullptr;
    Status (*mesh)(void* user, CookAssetInfo const& asset, TargetProfile const& target, MeshCookSettings* s,
                   DiagSink const* diag)                            = nullptr;
    void* user                                                      = nullptr;
};

struct ResolveDesc {
    CookAssetInfo asset;
    StrView sidecar                = {}; ///< the sidecar text (layer 4); empty: none
    StrView sidecarPath            = {}; ///< names the sidecar in diagnostics
    Span<NameRule const> nameRules = {}; ///< layer 5 for a texture with no slot
    CookPolicy policy              = {}; ///< layer 6
    TargetProfile target           = {};
    CookSession session            = {};
    DiagSink const* diag           = nullptr;
};

/// Runs layers 4 to 6 over `base` (layers 1 to 3: the host's settings), then resolve_texture:
/// the sidecar sets the keys it names; a still-Auto usage comes from the slot, else the name
/// rules, else Color; the policy runs; then derived fields, validation and target caps.
KILN_API Result<TextureCookSettings> resolve_texture_layers(TextureCookSettings const& base,
                                                            ResolveDesc const& d) noexcept;
/// Layers 4 and 6 over `base`, then resolve_mesh. Meshes have no inference layer.
KILN_API Result<MeshCookSettings> resolve_mesh_layers(MeshCookSettings const& base,
                                                      ResolveDesc const& d) noexcept;

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
// Hashing (field by field, schema-versioned)
// ---------------------------------------------------------------------------

inline constexpr u32 kTextureSettingsSchema = 2; ///< bump when a field is added or changes meaning
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
[[nodiscard]] KILN_API char const* cook_shape_name(CookShape s) noexcept;
[[nodiscard]] KILN_API char const* texture_encoding_name(TextureEncoding e) noexcept;
[[nodiscard]] KILN_API char const* encode_quality_name(EncodeQuality q) noexcept;
[[nodiscard]] KILN_API char const* supercompression_name(Supercompression s) noexcept;
[[nodiscard]] KILN_API char const* block_family_name(BlockFamily f) noexcept;

} // namespace kiln::cook
