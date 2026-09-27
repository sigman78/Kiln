#include "kiln_test.h"

#include "kiln/cook/settings.h"

#include <limits>

using namespace kiln;
using namespace kiln::cook;

namespace {

struct DiagCapture {
    u32 code          = 0;
    Severity severity = Severity::Info;
    int count         = 0;

    static void fn(void* user, Diagnostic const& d) {
        auto* self     = static_cast<DiagCapture*>(user);
        self->code     = d.code;
        self->severity = d.severity;
        ++self->count;
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

} // namespace

// ---------------------------------------------------------------------------
// resolve_texture
// ---------------------------------------------------------------------------

KILN_TEST(Settings, TextureUsageFromSlotHint) {
    TargetProfile target{};
    CookSession session{};
    struct Case {
        SlotHint hint;
        TextureUsage usage;
        ColorSpace cs;
    };
    Case const cases[] = {
        {SlotHint::None,              TextureUsage::Color,  ColorSpace::Srgb  },
        {SlotHint::BaseColor,         TextureUsage::Color,  ColorSpace::Srgb  },
        {SlotHint::Emissive,          TextureUsage::Color,  ColorSpace::Srgb  },
        {SlotHint::Normal,            TextureUsage::Normal, ColorSpace::Linear},
        {SlotHint::MetallicRoughness, TextureUsage::Orm,    ColorSpace::Linear},
        {SlotHint::Occlusion,         TextureUsage::Orm,    ColorSpace::Linear},
    };
    for (auto const& c : cases) {
        Result<TextureCookSettings> r = resolve_texture({}, c.hint, target, session);
        if (!KILN_CHECK_MSG(r.ok(), "hint %s", slot_hint_name(c.hint))) continue;
        KILN_CHECK_MSG(r->usage == c.usage, "hint %s", slot_hint_name(c.hint));
        KILN_CHECK_MSG(r->colorSpace == c.cs, "hint %s", slot_hint_name(c.hint));
    }
}

KILN_TEST(Settings, TextureExplicitUsageWins) {
    TargetProfile target{};
    CookSession session{};
    TextureCookSettings overrides{};
    overrides.usage               = TextureUsage::Mask; // hint below implies Color; explicit wins
    Result<TextureCookSettings> r = resolve_texture(overrides, SlotHint::BaseColor, target, session);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Mask);
    KILN_CHECK(r->colorSpace == ColorSpace::Linear); // color_space_for(Mask)
}

KILN_TEST(Settings, TextureColorSpaceAutoPerUsage) {
    TargetProfile target{};
    CookSession session{};
    struct Case {
        TextureUsage usage;
        ColorSpace expected;
    };
    Case const cases[] = {
        {TextureUsage::Color,  ColorSpace::Srgb  },
        {TextureUsage::Normal, ColorSpace::Linear},
        {TextureUsage::Orm,    ColorSpace::Linear},
        {TextureUsage::Mask,   ColorSpace::Linear},
        {TextureUsage::Hdr,    ColorSpace::Linear},
        {TextureUsage::Ui,     ColorSpace::Srgb  },
        {TextureUsage::Lut,    ColorSpace::Linear},
        {TextureUsage::Height, ColorSpace::Linear},
    };
    for (auto const& c : cases) {
        TextureCookSettings overrides{};
        overrides.usage               = c.usage;
        Result<TextureCookSettings> r = resolve_texture(overrides, SlotHint::None, target, session);
        if (!KILN_CHECK_MSG(r.ok(), "usage %s", texture_usage_name(c.usage))) continue;
        KILN_CHECK_MSG(r->colorSpace == c.expected, "usage %s", texture_usage_name(c.usage));
    }
}

KILN_TEST(Settings, TextureMaxSizeZeroUsesTargetCap) {
    TargetProfile target{};
    CookSession session{};
    TextureCookSettings overrides{};
    overrides.normalRenormalize = false; // isolate maxSize from the usage/Normal warning below
    DiagCapture cap;
    DiagSink sink                 = cap.sink();
    Result<TextureCookSettings> r = resolve_texture(overrides, SlotHint::None, target, session, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->maxSize, target.maxTextureSize);
    KILN_CHECK_EQ(cap.count, 0); // the common case is silent
}

KILN_TEST(Settings, TextureMaxSizeClampedByTargetWarns) {
    TargetProfile target{}; // maxTextureSize defaults to 16384
    CookSession session{};
    TextureCookSettings overrides{};
    overrides.maxSize           = target.maxTextureSize * 2;
    overrides.normalRenormalize = false; // isolate maxSize from the usage/Normal warning below
    DiagCapture cap;
    DiagSink sink                 = cap.sink();
    Result<TextureCookSettings> r = resolve_texture(overrides, SlotHint::None, target, session, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->maxSize, target.maxTextureSize);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsClampedByTarget));
    KILN_CHECK(cap.severity == Severity::Warning);
}

KILN_TEST(Settings, TextureNormalFlagsIgnoredForOtherUsageWarns) {
    TargetProfile target{};
    CookSession session{};
    TextureCookSettings overrides{};
    overrides.usage             = TextureUsage::Color;
    overrides.normalRenormalize = true; // meaningless for Color: cleared silently (it defaults to true)
    overrides.flipGreen         = true; // meaningless for Color: cleared with a warning (explicit intent)
    DiagCapture cap;
    DiagSink sink                 = cap.sink();
    Result<TextureCookSettings> r = resolve_texture(overrides, SlotHint::None, target, session, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!r->normalRenormalize);
    KILN_CHECK(!r->flipGreen);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsInvalidCombo));
    KILN_CHECK(cap.severity == Severity::Warning);

    // Defaults alone (normalRenormalize == true) must not warn for non-normal usages.
    DiagCapture quiet;
    DiagSink qsink = quiet.sink();
    Result<TextureCookSettings> q =
        resolve_texture(TextureCookSettings{}, SlotHint::BaseColor, target, session, &qsink);
    KILN_REQUIRE(q.ok());
    KILN_CHECK(!q->normalRenormalize);
    KILN_CHECK_EQ(quiet.code, 0u);
}

KILN_TEST(Settings, TextureFastPreviewKeepsMips) {
    TargetProfile target{};
    CookSession session{};
    session.fastPreview           = true;
    Result<TextureCookSettings> r = resolve_texture({}, SlotHint::None, target, session);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->genMips);
}

KILN_TEST(Settings, TextureEnumOutOfRangeIsInvalidArgument) {
    TargetProfile target{};
    CookSession session{};
    TextureCookSettings overrides{};
    overrides.usage               = TextureUsage(200);
    Result<TextureCookSettings> r = resolve_texture(overrides, SlotHint::None, target, session);
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::InvalidArgument);
}

// ---------------------------------------------------------------------------
// resolve_mesh
// ---------------------------------------------------------------------------

KILN_TEST(Settings, MeshGenLodsUnsupported) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.genLods = true;
    DiagCapture cap;
    DiagSink sink              = cap.sink();
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session, &sink);
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::Unsupported);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsUnsupported));
    KILN_CHECK(cap.severity == Severity::Error);
}

KILN_TEST(Settings, MeshCompressionBasicUnsupported) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.compression = CompressionScheme::Basic;
    DiagCapture cap;
    DiagSink sink              = cap.sink();
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session, &sink);
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::Unsupported);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsUnsupported));
}

KILN_TEST(Settings, MeshBlobChunkSizeUnsupported) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.blobChunkSize = 64;
    DiagCapture cap;
    DiagSink sink              = cap.sink();
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session, &sink);
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::Unsupported);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsUnsupported));
}

KILN_TEST(Settings, MeshZstdLevelIgnoredWithNoneWarns) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.zstdLevel = 5; // compression stays None
    DiagCapture cap;
    DiagSink sink              = cap.sink();
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsInvalidCombo));
    KILN_CHECK(cap.severity == Severity::Warning);
}

KILN_TEST(Settings, MeshProfileClampedByTargetWarns) {
    TargetProfile target{};
    target.maxVertexProfile = VertexProfile::Default;
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.profile = VertexProfile::Precise;
    DiagCapture cap;
    DiagSink sink              = cap.sink();
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->profile == VertexProfile::Default);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsClampedByTarget));
    KILN_CHECK(cap.severity == Severity::Warning);
}

KILN_TEST(Settings, MeshFastPreviewDisablesOptimizeAndTangents) {
    TargetProfile target{};
    CookSession session{};
    session.fastPreview        = true;
    Result<MeshCookSettings> r = resolve_mesh({}, target, session);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!r->optimize);
    KILN_CHECK(!r->genTangents);
}

KILN_TEST(Settings, MeshBadPosTolMm) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};

    overrides.posTolMm = 0.0f;
    KILN_CHECK(resolve_mesh(overrides, target, session).failed());

    overrides.posTolMm = -1.0f;
    KILN_CHECK(resolve_mesh(overrides, target, session).failed());

    overrides.posTolMm                 = std::numeric_limits<f32>::quiet_NaN();
    Result<MeshCookSettings> nanResult = resolve_mesh(overrides, target, session);
    KILN_CHECK(nanResult.failed());
    KILN_CHECK(nanResult.code() == Code::InvalidArgument);
}

KILN_TEST(Settings, MeshNegativeWeldTolInvalid) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.weldTol          = -0.5f;
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session);
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::InvalidArgument);
}

KILN_TEST(Settings, MeshEnumOutOfRangeIsInvalidArgument) {
    TargetProfile target{};
    CookSession session{};
    MeshCookSettings overrides{};
    overrides.compression      = CompressionScheme(200);
    Result<MeshCookSettings> r = resolve_mesh(overrides, target, session);
    KILN_CHECK(r.failed());
    KILN_CHECK(r.code() == Code::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

KILN_TEST(Settings, TextureHashIdenticalStructsEqual) {
    TextureCookSettings a{};
    TextureCookSettings b{};
    KILN_CHECK_EQ(hash_settings(a), hash_settings(b));
}

// Pinned against a reference build. A change here means either a bug or a
// deliberate field/meaning change to TextureCookSettings, which must also bump
// kTextureSettingsSchema (docs/design/settings.md) so old store entries miss
// rather than being silently misread.
KILN_TEST(Settings, TextureDefaultHashIsPinned) {
    TextureCookSettings defaults{};
    KILN_CHECK_EQ(hash_settings(defaults), u64(0x38efc66c33c35cd0ull));
}

namespace {
TextureCookSettings tex_mut_colorSpace(TextureCookSettings s) {
    s.colorSpace = ColorSpace::Srgb;
    return s;
}
TextureCookSettings tex_mut_usage(TextureCookSettings s) {
    s.usage = TextureUsage::Color;
    return s;
}
TextureCookSettings tex_mut_genMips(TextureCookSettings s) {
    s.genMips = !s.genMips;
    return s;
}
TextureCookSettings tex_mut_normalRenormalize(TextureCookSettings s) {
    s.normalRenormalize = !s.normalRenormalize;
    return s;
}
TextureCookSettings tex_mut_maxSize(TextureCookSettings s) {
    s.maxSize = 123;
    return s;
}
TextureCookSettings tex_mut_flipGreen(TextureCookSettings s) {
    s.flipGreen = !s.flipGreen;
    return s;
}

struct TexMutation {
    char const* field;
    TextureCookSettings (*apply)(TextureCookSettings);
};

TexMutation const kTexMutations[] = {
    {"colorSpace",        &tex_mut_colorSpace       },
    {"usage",             &tex_mut_usage            },
    {"genMips",           &tex_mut_genMips          },
    {"normalRenormalize", &tex_mut_normalRenormalize},
    {"maxSize",           &tex_mut_maxSize          },
    {"flipGreen",         &tex_mut_flipGreen        },
};
} // namespace

KILN_TEST(Settings, TextureHashChangesPerField) {
    TextureCookSettings base{};
    u64 const baseHash = hash_settings(base);
    for (auto const& m : kTexMutations) {
        u64 const mutatedHash = hash_settings(m.apply(base));
        KILN_CHECK_MSG(mutatedHash != baseHash, "field '%s' did not change the texture hash", m.field);
    }
}

KILN_TEST(Settings, MeshHashIdenticalStructsEqual) {
    MeshCookSettings a{};
    MeshCookSettings b{};
    KILN_CHECK_EQ(hash_settings(a), hash_settings(b));
}

namespace {
MeshCookSettings mesh_mut_profile(MeshCookSettings s) {
    s.profile = VertexProfile::Precise;
    return s;
}
MeshCookSettings mesh_mut_genTangents(MeshCookSettings s) {
    s.genTangents = !s.genTangents;
    return s;
}
MeshCookSettings mesh_mut_optimize(MeshCookSettings s) {
    s.optimize = !s.optimize;
    return s;
}
MeshCookSettings mesh_mut_useAuthoredLods(MeshCookSettings s) {
    s.useAuthoredLods = !s.useAuthoredLods;
    return s;
}
MeshCookSettings mesh_mut_genLods(MeshCookSettings s) {
    s.genLods = !s.genLods;
    return s;
}
MeshCookSettings mesh_mut_posTolMm(MeshCookSettings s) {
    s.posTolMm = s.posTolMm + 1.0f;
    return s;
}
MeshCookSettings mesh_mut_weldTol(MeshCookSettings s) {
    s.weldTol = s.weldTol + 1.0f;
    return s;
}
MeshCookSettings mesh_mut_compression(MeshCookSettings s) {
    s.compression = CompressionScheme::None;
    return s;
}
MeshCookSettings mesh_mut_zstdLevel(MeshCookSettings s) {
    s.zstdLevel = u8(s.zstdLevel + 1);
    return s;
}
MeshCookSettings mesh_mut_blobChunkSize(MeshCookSettings s) {
    s.blobChunkSize = s.blobChunkSize + 64;
    return s;
}

struct MeshMutation {
    char const* field;
    MeshCookSettings (*apply)(MeshCookSettings);
};

MeshMutation const kMeshMutations[] = {
    {"profile",         &mesh_mut_profile        },
    {"genTangents",     &mesh_mut_genTangents    },
    {"optimize",        &mesh_mut_optimize       },
    {"useAuthoredLods", &mesh_mut_useAuthoredLods},
    {"genLods",         &mesh_mut_genLods        },
    {"posTolMm",        &mesh_mut_posTolMm       },
    {"weldTol",         &mesh_mut_weldTol        },
    {"compression",     &mesh_mut_compression    },
    {"zstdLevel",       &mesh_mut_zstdLevel      },
    {"blobChunkSize",   &mesh_mut_blobChunkSize  },
};
} // namespace

KILN_TEST(Settings, MeshHashChangesPerField) {
    // compression starts as MeshoptZstd (not a v0.5-valid resolved value, but
    // hash_settings hashes whatever it is given) so zstdLevel is "live" and its
    // own mutation below is expected to change the hash.
    MeshCookSettings base{};
    base.compression   = CompressionScheme::MeshoptZstd;
    base.zstdLevel     = 7;
    u64 const baseHash = hash_settings(base);
    for (auto const& m : kMeshMutations) {
        u64 const mutatedHash = hash_settings(m.apply(base));
        KILN_CHECK_MSG(mutatedHash != baseHash, "field '%s' did not change the mesh hash", m.field);
    }
}

// docs/design/settings.md: fields the resolved settings do not use hash as 0, so
// changing them elsewhere never misses the store. zstdLevel is unused while
// compression is None.
KILN_TEST(Settings, MeshHashMasksZstdLevelWhenCompressionNone) {
    MeshCookSettings a{};
    a.compression      = CompressionScheme::None;
    a.zstdLevel        = 5;
    MeshCookSettings b = a;
    b.zstdLevel        = 9;
    KILN_CHECK_EQ(hash_settings(a), hash_settings(b));
}

KILN_TEST(Settings, TargetHashDiffersByName) {
    TargetProfile a{};
    a.name = "desktop";
    TargetProfile b{};
    b.name = "mobile";
    KILN_CHECK_NE(hash_target(a), hash_target(b));
}

KILN_TEST(Settings, TargetHashIdenticalProfilesEqual) {
    TargetProfile a{};
    TargetProfile b{};
    KILN_CHECK_EQ(hash_target(a), hash_target(b));
}
