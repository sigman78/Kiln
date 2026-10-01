// src/cook/settings.cpp — settings resolution and field-by-field hashing.
// Design: docs/design/settings.md.
#include "kiln/cook/settings.h"

#include "kiln/cook/sidecar.h"

#include "cook_internal.h"

#include "kiln/log.h"

#include <bit>

namespace kiln::cook {

namespace {

bool is_block_encoding(TextureEncoding e) noexcept {
    return e != TextureEncoding::Auto && e != TextureEncoding::Uncompressed;
}

/// The usages an explicit encoding can store (bcn-encoding.md, "Formats per usage").
bool encoding_fits(TextureEncoding e, TextureUsage u) noexcept {
    switch (e) {
    case TextureEncoding::Auto:
    case TextureEncoding::Uncompressed: return true;
    case TextureEncoding::BC1:
    case TextureEncoding::BC3:
    case TextureEncoding::BC7:
        return u == TextureUsage::Color || u == TextureUsage::Ui || u == TextureUsage::Orm ||
               u == TextureUsage::Normal || u == TextureUsage::Mask;
    case TextureEncoding::BC4: return u == TextureUsage::Mask || u == TextureUsage::Height;
    case TextureEncoding::BC5: return u == TextureUsage::Normal || u == TextureUsage::Mask;
    case TextureEncoding::BC6H: return u == TextureUsage::Hdr;
    }
    return false;
}

} // namespace

namespace detail {

Format encoding_format(TextureEncoding e, bool srgb) noexcept {
    switch (e) {
    case TextureEncoding::BC1: return srgb ? Format::BC1_RGB_SRGB : Format::BC1_RGB_UNORM;
    case TextureEncoding::BC3: return srgb ? Format::BC3_SRGB : Format::BC3_UNORM;
    case TextureEncoding::BC4: return Format::BC4_UNORM;
    case TextureEncoding::BC5: return Format::BC5_UNORM;
    case TextureEncoding::BC6H: return Format::BC6H_UFLOAT;
    case TextureEncoding::BC7: return srgb ? Format::BC7_SRGB : Format::BC7_UNORM;
    case TextureEncoding::Auto:
    case TextureEncoding::Uncompressed: return Format::Undefined;
    }
    return Format::Undefined;
}

bool srgb_blocks(ColorSpace cs, TextureUsage usage) noexcept {
    return cs == ColorSpace::Srgb && (usage == TextureUsage::Color || usage == TextureUsage::Ui);
}

} // namespace detail

TargetProfile const* target_profile(StrView name) noexcept {
    for (TargetProfile const* t : {&kCompatTarget, &kDesktopTarget, &kUncompressedTarget})
        if (t->name == name) return t;
    return nullptr;
}

Result<TextureCookSettings> resolve_texture(TextureCookSettings const& overrides, SlotHint hint,
                                            TargetProfile const& target, CookSession const& session,
                                            DiagSink const* diag, StrView asset) noexcept {
    if (u8(overrides.usage) > u8(TextureUsage::Height) || u8(overrides.colorSpace) > u8(ColorSpace::Linear) ||
        u8(overrides.shape) > u8(CookShape::Array) || u8(overrides.encoding) > u8(TextureEncoding::BC7) ||
        u8(overrides.quality) > u8(EncodeQuality::High) ||
        u8(overrides.supercompression) > u8(Supercompression::Zstd)) {
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsEnumRange, Severity::Error, asset,
                     "texture",
                     "usage, colorSpace, shape, encoding, quality or supercompression holds a value outside "
                     "its enum range");
    }

    TextureCookSettings s = overrides;
    if (s.usage == TextureUsage::Auto) s.usage = usage_from_slot(hint);
    if (s.colorSpace == ColorSpace::Auto) s.colorSpace = color_space_for(s.usage);

    u32 const cap = target.maxTextureSize;
    if (s.maxSize == 0) {
        s.maxSize = cap;
    } else if (cap != 0 && s.maxSize > cap) {
        (void)diagf(diag, kOk, kDiagSettingsClampedByTarget, Severity::Warning, asset, "maxSize",
                    "maxSize %u clamped to target %.*s cap %u", s.maxSize, KILN_SV(target.name), cap);
        s.maxSize = cap;
    }

    // Clear both Normal-only flags for other usages so the resolved struct, its hash and
    // the store key stay canonical. Only flipGreen warns: it defaults to false, so a set
    // flag signals intent.
    if (s.usage != TextureUsage::Normal) {
        if (s.flipGreen)
            (void)diagf(diag, kOk, kDiagSettingsInvalidCombo, Severity::Warning, asset, "usage",
                        "flipGreen ignored for usage %s", texture_usage_name(s.usage));
        s.normalRenormalize = false;
        s.flipGreen         = false;
    }

    // Alpha coverage: Auto that no layer filled is off. Only usages that keep alpha, and only with mips.
    if (s.alphaCutoff < 0.0f) s.alphaCutoff = 0.0f;
    if (!(s.alphaCutoff <= 1.0f)) // also NaN
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "alphaCutoff", "alphaCutoff %g is outside 0..1", double(s.alphaCutoff));
    if (s.alphaCutoff > 0.0f && s.usage != TextureUsage::Color && s.usage != TextureUsage::Ui) {
        (void)diagf(diag, kOk, kDiagSettingsInvalidCombo, Severity::Warning, asset, "alphaCutoff",
                    "alphaCutoff ignored for usage %s", texture_usage_name(s.usage));
        s.alphaCutoff = 0.0f;
    }
    if (!s.genMips) s.alphaCutoff = 0.0f;

    if (s.shape != CookShape::Array && s.slices != 0) {
        (void)diagf(diag, kOk, kDiagSettingsInvalidCombo, Severity::Warning, asset, "slices",
                    "slices ignored for shape %s", cook_shape_name(s.shape));
        s.slices = 0;
    }

    if (!encoding_fits(s.encoding, s.usage))
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "encoding", "encoding %s cannot store usage %s",
                     texture_encoding_name(s.encoding), texture_usage_name(s.usage));
    if ((s.encoding == TextureEncoding::BC4 || s.encoding == TextureEncoding::BC5 ||
         s.encoding == TextureEncoding::BC6H) &&
        s.colorSpace == ColorSpace::Srgb)
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "encoding", "encoding %s has no sRGB variant", texture_encoding_name(s.encoding));
    // The cooker never writes another format than the one asked for (target-profiles.md).
    if (is_block_encoding(s.encoding)) {
        Format const f = detail::encoding_format(s.encoding, detail::srgb_blocks(s.colorSpace, s.usage));
        if (!(target.blockFormats & block_format_bit(f)))
            return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                         asset, "encoding", "encoding %s: profile %.*s does not have %s",
                         texture_encoding_name(s.encoding), KILN_SV(target.name), format_name(f));
    }
    // Only the encoders read quality: without them it stays Normal, so the hash stays canonical.
    if (s.encoding == TextureEncoding::Uncompressed || target.blockFormats == 0)
        s.quality = EncodeQuality::Normal;
    else if (session.fastPreview)
        s.quality = EncodeQuality::Fast;
    else if (s.quality > session.maxQuality)
        s.quality = session.maxQuality;

    if (s.zstdLevel > kMaxZstdLevel)
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "zstdLevel", "zstdLevel %u is above %u", s.zstdLevel, kMaxZstdLevel);
    if (s.supercompression == Supercompression::None)
        s.zstdLevel = 0;
    else if (session.fastPreview)
        s.zstdLevel = kPreviewZstdLevel;
    else if (s.zstdLevel == 0)
        s.zstdLevel = kDefaultZstdLevel;

    return s;
}

namespace {

/// A Radiance `.hdr` source is HDR by its format, whatever its name says.
bool has_hdr_extension(StrView name) noexcept {
    if (name.size < 4) return false;
    StrView const ext = name.substr(name.size - 4);
    auto const lower  = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    return ext[0] == '.' && lower(ext[1]) == 'h' && lower(ext[2]) == 'd' && lower(ext[3]) == 'r';
}

Status refused(ResolveDesc const& d, Status st) noexcept {
    return diagf(d.diag, st, kDiagPolicyRefused, Severity::Error, d.asset.name, "policy",
                 "the cook policy refused the asset (%s)", code_name(st.code));
}

} // namespace

Result<TextureCookSettings> resolve_texture_layers(TextureCookSettings const& base,
                                                   ResolveDesc const& d) noexcept {
    TextureCookSettings s = base;
    if (!d.sidecar.empty()) KILN_TRY(apply_sidecar(d.sidecar, &s, d.diag, d.sidecarPath));
    NameHints hints =
        d.asset.slot == SlotHint::None ? hints_from_name(d.asset.name, d.nameRules) : NameHints{};
    if (d.asset.slot == SlotHint::None && has_hdr_extension(d.asset.name)) hints.usage = TextureUsage::Hdr;
    if (s.usage == TextureUsage::Auto) {
        s.usage = d.asset.slot != SlotHint::None ? usage_from_slot(d.asset.slot) : hints.usage;
        if (s.usage == TextureUsage::Auto) s.usage = TextureUsage::Color;
    }
    if (s.shape == CookShape::Auto) s.shape = hints.shape;
    if (s.alphaCutoff < 0.0f) s.alphaCutoff = d.asset.alphaCutoff; // the Mask material's cutoff, or 0
    if (d.policy.texture) {
        Status const st = d.policy.texture(d.policy.user, d.asset, d.target, &s, d.diag);
        if (st.failed()) return refused(d, st);
    }
    return resolve_texture(s, d.asset.slot, d.target, d.session, d.diag, d.asset.name);
}

Result<MeshCookSettings> resolve_mesh_layers(MeshCookSettings const& base, ResolveDesc const& d) noexcept {
    MeshCookSettings s = base;
    if (!d.sidecar.empty()) KILN_TRY(apply_sidecar(d.sidecar, &s, d.diag, d.sidecarPath));
    if (d.policy.mesh) {
        Status const st = d.policy.mesh(d.policy.user, d.asset, d.target, &s, d.diag);
        if (st.failed()) return refused(d, st);
    }
    return resolve_mesh(s, d.target, d.session, d.diag, d.asset.name);
}

NameHints hints_from_name(StrView path, Span<NameRule const> rules) noexcept {
    usize const slash = path.rfind('/');
    StrView stem      = slash == StrView::kNpos ? path : path.substr(slash + 1);
    usize const dot   = stem.rfind('.');
    if (dot != StrView::kNpos && dot > 0) stem = stem.substr(0, dot);

    auto const lower          = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    constexpr usize kMaxRules = 64; // rules past this take part in the first round only
    bool used[kMaxRules]      = {};
    NameHints hints;
    for (bool matched = true; matched;) {
        matched = false;
        for (usize r = 0; r < rules.size && !matched; ++r) {
            NameRule const& rule = rules[r];
            if ((r < kMaxRules && used[r]) || rule.suffix.empty() || rule.suffix.size >= stem.size)
                continue; // a bare "_n.png" has no name
            StrView const tail = stem.substr(stem.size - rule.suffix.size);
            bool match         = true;
            for (usize i = 0; i < tail.size && match; ++i)
                match = lower(tail[i]) == lower(rule.suffix[i]);
            if (!match) continue;
            if (hints.usage == TextureUsage::Auto) hints.usage = rule.usage;
            if (hints.shape == CookShape::Auto) hints.shape = rule.shape;
            if (r < kMaxRules) used[r] = true;
            stem    = stem.substr(0, stem.size - rule.suffix.size);
            matched = r < kMaxRules;
        }
    }
    return hints;
}

TextureUsage usage_from_name(StrView path, Span<NameRule const> rules) noexcept {
    return hints_from_name(path, rules).usage;
}

Result<MeshCookSettings> resolve_mesh(MeshCookSettings const& overrides, TargetProfile const& target,
                                      CookSession const& session, DiagSink const* diag,
                                      StrView asset) noexcept {
    if (u8(overrides.profile) > u8(VertexProfile::Float) ||
        u8(overrides.compression) > u8(CompressionScheme::MeshoptZstd)) {
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsEnumRange, Severity::Error, asset,
                     "mesh", "profile or compression holds a value outside its enum range");
    }

    MeshCookSettings s = overrides;

    if (s.genLods) {
        return diagf(diag, make_status(Code::Unsupported), kDiagSettingsUnsupported, Severity::Error, asset,
                     "genLods", "LOD generation is reserved for v0.6; use useAuthoredLods instead");
    }
    if (s.blobChunkSize != 0) {
        return diagf(diag, make_status(Code::Unsupported), kDiagSettingsUnsupported, Severity::Error, asset,
                     "blobChunkSize", "blob chunking is reserved for v0.6; blobChunkSize must be 0");
    }
    bool const usesZstd = s.compression == CompressionScheme::MeshoptZstd;
    if (s.zstdLevel > kMaxZstdLevel)
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "zstdLevel", "zstdLevel %u is above %u", s.zstdLevel, kMaxZstdLevel);
    if (!usesZstd)
        s.zstdLevel = 0;
    else if (s.zstdLevel == 0)
        s.zstdLevel = kDefaultZstdLevel;

    if (u8(s.profile) > u8(target.maxVertexProfile)) {
        (void)diagf(diag, kOk, kDiagSettingsClampedByTarget, Severity::Warning, asset, "profile",
                    "profile %s clamped to target %.*s cap %s", vertex_profile_name(s.profile),
                    KILN_SV(target.name), vertex_profile_name(target.maxVertexProfile));
        s.profile = target.maxVertexProfile;
    }

    if (!(s.posTolMm > 0.0f)) { // false for <= 0 and for NaN
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "posTolMm", "posTolMm must be a positive, finite number");
    }
    if (!(s.weldTol >= 0.0f)) { // false for < 0 and for NaN
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsInvalidCombo, Severity::Error,
                     asset, "weldTol", "weldTol must be >= 0");
    }

    if (session.fastPreview) {
        s.optimize    = false;
        s.genTangents = false;
    }

    return s;
}

namespace {

/// Floats are hashed by bit pattern after folding -0.0f to 0.0f.
u32 hashable_float_bits(f32 v) noexcept {
    if (v == 0.0f) v = 0.0f;
    return std::bit_cast<u32>(v);
}

} // namespace

u64 hash_settings(TextureCookSettings const& s) noexcept {
    Xxh64State h;
    h.update_value(kTextureSettingsSchema);
    h.update_value(u8(s.colorSpace));
    h.update_value(u8(s.usage));
    h.update_value(u8(s.genMips));
    h.update_value(u8(s.normalRenormalize));
    h.update_value(s.maxSize);
    h.update_value(u8(s.flipGreen));
    h.update_value(u8(s.shape));
    h.update_value(s.slices);
    // Hashed only when not the default, with a tag each, so the keys of cooks predating these
    // fields stay valid.
    if (s.encoding != TextureEncoding::Auto) h.update_value(u16(0x100u | u8(s.encoding)));
    if (s.quality != EncodeQuality::Normal) h.update_value(u16(0x200u | u8(s.quality)));
    if (s.supercompression != Supercompression::None) {
        h.update_value(u16(0x300u | u8(s.supercompression)));
        h.update_value(u16(0x400u | s.zstdLevel));
    }
    if (s.alphaCutoff > 0.0f) {
        h.update_value(u16(0x500u));
        h.update_value(hashable_float_bits(s.alphaCutoff));
    }
    return h.digest();
}

u64 hash_settings(MeshCookSettings const& s) noexcept {
    Xxh64State h;
    h.update_value(kMeshSettingsSchema);
    h.update_value(u8(s.profile));
    h.update_value(u8(s.genTangents));
    h.update_value(u8(s.optimize));
    h.update_value(u8(s.useAuthoredLods));
    h.update_value(u8(s.genLods));
    h.update_value(hashable_float_bits(s.posTolMm));
    h.update_value(hashable_float_bits(s.weldTol));
    h.update_value(u8(s.compression));
    // Hash zstdLevel as 0 unless the scheme uses Zstd, so changing an unused field
    // never misses the store.
    bool const usesZstd = s.compression == CompressionScheme::MeshoptZstd;
    h.update_value(u8(usesZstd ? s.zstdLevel : 0));
    h.update_value(s.blobChunkSize);
    return h.digest();
}

u64 hash_target(TargetProfile const& t) noexcept {
    Xxh64State h;
    h.update_value(kTargetSchema);
    h.update(t.name);
    h.update_value(t.maxTextureSize);
    h.update_value(u8(t.maxVertexProfile));
    // maxArrayLayers only rejects inputs and never changes an output, so it is not hashed.
    h.update_value(t.blockFormats);
    return h.digest();
}

char const* texture_usage_name(TextureUsage u) noexcept {
    switch (u) {
    case TextureUsage::Auto: return "auto";
    case TextureUsage::Color: return "color";
    case TextureUsage::Normal: return "normal";
    case TextureUsage::Orm: return "orm";
    case TextureUsage::Mask: return "mask";
    case TextureUsage::Hdr: return "hdr";
    case TextureUsage::Ui: return "ui";
    case TextureUsage::Lut: return "lut";
    case TextureUsage::Height: return "height";
    }
    return "?";
}

char const* color_space_name(ColorSpace c) noexcept {
    switch (c) {
    case ColorSpace::Auto: return "auto";
    case ColorSpace::Srgb: return "srgb";
    case ColorSpace::Linear: return "linear";
    }
    return "?";
}

char const* vertex_profile_name(VertexProfile p) noexcept {
    switch (p) {
    case VertexProfile::Default: return "default";
    case VertexProfile::Precise: return "precise";
    case VertexProfile::Float: return "float";
    }
    return "?";
}

char const* slot_hint_name(SlotHint h) noexcept {
    switch (h) {
    case SlotHint::None: return "none";
    case SlotHint::BaseColor: return "baseColor";
    case SlotHint::Normal: return "normal";
    case SlotHint::MetallicRoughness: return "metallicRoughness";
    case SlotHint::Occlusion: return "occlusion";
    case SlotHint::Emissive: return "emissive";
    }
    return "?";
}

char const* cook_shape_name(CookShape s) noexcept {
    switch (s) {
    case CookShape::Auto: return "auto";
    case CookShape::Tex2D: return "2d";
    case CookShape::Cube: return "cube";
    case CookShape::Array: return "array";
    }
    return "?";
}

char const* texture_encoding_name(TextureEncoding e) noexcept {
    switch (e) {
    case TextureEncoding::Auto: return "auto";
    case TextureEncoding::Uncompressed: return "uncompressed";
    case TextureEncoding::BC1: return "bc1";
    case TextureEncoding::BC3: return "bc3";
    case TextureEncoding::BC4: return "bc4";
    case TextureEncoding::BC5: return "bc5";
    case TextureEncoding::BC6H: return "bc6h";
    case TextureEncoding::BC7: return "bc7";
    }
    return "?";
}

char const* encode_quality_name(EncodeQuality q) noexcept {
    switch (q) {
    case EncodeQuality::Fast: return "fast";
    case EncodeQuality::Normal: return "normal";
    case EncodeQuality::High: return "high";
    }
    return "?";
}

char const* supercompression_name(Supercompression s) noexcept {
    switch (s) {
    case Supercompression::None: return "none";
    case Supercompression::Zstd: return "zstd";
    }
    return "?";
}

} // namespace kiln::cook
