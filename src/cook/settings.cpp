// src/cook/settings.cpp — settings resolution and field-by-field hashing.
// Design: docs/design/settings.md.
#include "kiln/cook/settings.h"

#include "kiln/log.h"

#include <bit>

namespace kiln::cook {

Result<TextureCookSettings> resolve_texture(TextureCookSettings const& overrides, SlotHint hint,
                                            TargetProfile const& target, CookSession const& session,
                                            DiagSink const* diag, StrView asset) noexcept {
    if (u8(overrides.usage) > u8(TextureUsage::Height) || u8(overrides.colorSpace) > u8(ColorSpace::Linear)) {
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsEnumRange, Severity::Error, asset,
                     "texture", "usage or colorSpace holds a value outside its enum range");
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

    // fastPreview does not change texture settings yet: mips are cheap, and skipping
    // them would add resolved-state variance.
    (void)session;

    return s;
}

TextureUsage usage_from_name(StrView path, Span<NameRule const> rules) noexcept {
    usize const slash = path.rfind('/');
    StrView stem      = slash == StrView::kNpos ? path : path.substr(slash + 1);
    usize const dot   = stem.rfind('.');
    if (dot != StrView::kNpos && dot > 0) stem = stem.substr(0, dot);

    auto const lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (NameRule const& r : rules) {
        if (r.suffix.empty() || r.suffix.size >= stem.size) continue; // a bare "_n.png" has no name
        StrView const tail = stem.substr(stem.size - r.suffix.size);
        bool match         = true;
        for (usize i = 0; i < tail.size && match; ++i)
            match = lower(tail[i]) == lower(r.suffix[i]);
        if (match) return r.usage;
    }
    return TextureUsage::Auto;
}

Result<MeshCookSettings> resolve_mesh(MeshCookSettings const& overrides, TargetProfile const& target,
                                      CookSession const& session, DiagSink const* diag,
                                      StrView asset) noexcept {
    if (u8(overrides.profile) > u8(VertexProfile::Precise) ||
        u8(overrides.compression) > u8(CompressionScheme::MeshoptZstd)) {
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSettingsEnumRange, Severity::Error, asset,
                     "mesh", "profile or compression holds a value outside its enum range");
    }

    MeshCookSettings s = overrides;

    if (s.genLods) {
        return diagf(diag, make_status(Code::Unsupported), kDiagSettingsUnsupported, Severity::Error, asset,
                     "genLods", "LOD generation is reserved for v0.6; use useAuthoredLods instead");
    }
    if (s.compression != CompressionScheme::None) {
        return diagf(diag, make_status(Code::Unsupported), kDiagSettingsUnsupported, Severity::Error, asset,
                     "compression", "compression scheme %u is reserved for v0.6; only None (0) is accepted",
                     u32(s.compression));
    }
    if (s.blobChunkSize != 0) {
        return diagf(diag, make_status(Code::Unsupported), kDiagSettingsUnsupported, Severity::Error, asset,
                     "blobChunkSize", "blob chunking is reserved for v0.6; blobChunkSize must be 0");
    }
    if (s.zstdLevel != 0) {
        (void)diagf(diag, kOk, kDiagSettingsInvalidCombo, Severity::Warning, asset, "zstdLevel",
                    "zstdLevel ignored: compression is None");
    }

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
[[nodiscard]] u32 hashable_float_bits(f32 v) noexcept {
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
    bool const usesZstd =
        s.compression == CompressionScheme::Basic || s.compression == CompressionScheme::MeshoptZstd;
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

} // namespace kiln::cook
