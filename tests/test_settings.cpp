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

KILN_TEST(Settings, UsageFromName) {
    struct Case {
        char const* path;
        TextureUsage usage;
    };
    Case const cases[] = {
        {"tex/wood_n.png",                TextureUsage::Normal},
        {"Wood_Normal.PNG",               TextureUsage::Normal},
        {"wood_n",                        TextureUsage::Normal}, // no extension
        {"Lantern_roughnessMetallic.png", TextureUsage::Orm   },
        {"crate_orm.jpg",                 TextureUsage::Orm   },
        {"crate_albedo.png",              TextureUsage::Color },
        {"wood.png",                      TextureUsage::Auto  },
        {"wood_nx.png",                   TextureUsage::Auto  },
        {"_n.png",                        TextureUsage::Auto  }, // the suffix alone is not a name
        {"dir_n/wood.png",                TextureUsage::Auto  }, // only the file stem counts
        {"wood_n.v2.png",                 TextureUsage::Auto  }, // the stem is "wood_n.v2"
    };
    for (Case const& c : cases)
        KILN_CHECK_MSG(usage_from_name(c.path, kDefaultNameRules) == c.usage, "%s -> %s", c.path,
                       texture_usage_name(usage_from_name(c.path, kDefaultNameRules)));

    KILN_CHECK(usage_from_name("wood_n.png", {}) == TextureUsage::Auto);
    NameRule const mine[] = {
        {"_n", TextureUsage::Mask  },
        {"_n", TextureUsage::Normal}, // shadowed: the first match wins
    };
    KILN_CHECK(usage_from_name("wood_N.png", mine) == TextureUsage::Mask);
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

// Every scheme resolves; a Zstd level 0 becomes the default where the scheme uses Zstd, and 0 elsewhere.
KILN_TEST(Settings, MeshCompressionResolves) {
    struct Case {
        CompressionScheme scheme;
        u8 level, want;
    };
    Case const cases[] = {
        {CompressionScheme::None,        5, 0                },
        {CompressionScheme::MeshoptZstd, 9, 9                },
        {CompressionScheme::Meshopt,     9, 0                },
        {CompressionScheme::MeshoptZstd, 0, kDefaultZstdLevel},
    };
    for (Case const& c : cases) {
        DiagCapture cap;
        DiagSink sink              = cap.sink();
        Result<MeshCookSettings> r = resolve_mesh({.compression = c.scheme, .zstdLevel = c.level},
                                                  TargetProfile{}, CookSession{}, &sink);
        KILN_REQUIRE(r.ok());
        KILN_CHECK_EQ(r->zstdLevel, c.want);
        KILN_CHECK_EQ(cap.count, 0);
    }
    DiagCapture cap;
    DiagSink sink = cap.sink();
    KILN_CHECK(resolve_mesh({.compression = CompressionScheme::MeshoptZstd, .zstdLevel = 20}, TargetProfile{},
                            CookSession{}, &sink)
                   .code() == Code::InvalidArgument);
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

KILN_TEST(Settings, TextureHashIdenticalStructsEqual) {
    TextureCookSettings a{};
    TextureCookSettings b{};
    KILN_CHECK_EQ(hash_settings(a), hash_settings(b));
}

// Pinned against a reference build. A deliberate TextureCookSettings change must also change the
// hash (bump kTextureSettingsSchema, or hash the new field with a tag; docs/design/settings.md), so
// old store entries miss instead of misreading.
KILN_TEST(Settings, TextureDefaultHashIsPinned) {
    TextureCookSettings defaults{};
    KILN_CHECK_EQ(hash_settings(defaults), u64(0x9982984a473fe444ull)); // schema 2 + Zstd by default
    // Without Zstd the hash is the one from before supercompression: those files did not change.
    defaults.supercompression = Supercompression::None;
    KILN_CHECK_EQ(hash_settings(defaults), u64(0x7ef735bb784af79aull));
}

KILN_TEST(Settings, TextureZstdLevelResolution) {
    TargetProfile const target{};
    Result<TextureCookSettings> r = resolve_texture({}, SlotHint::None, target, {});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->supercompression == Supercompression::Zstd);
    KILN_CHECK_EQ(u32(r->zstdLevel), u32(kDefaultZstdLevel));

    r = resolve_texture({.zstdLevel = 9}, SlotHint::None, target, {});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(u32(r->zstdLevel), 9u);

    r = resolve_texture({.zstdLevel = 9}, SlotHint::None, target, {.fastPreview = true});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(u32(r->zstdLevel), u32(kPreviewZstdLevel));

    r = resolve_texture({.supercompression = Supercompression::None, .zstdLevel = 9}, SlotHint::None, target,
                        {});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(u32(r->zstdLevel), 0u); // canonical: the level of no compression is 0

    r = resolve_texture({.zstdLevel = kMaxZstdLevel + 1}, SlotHint::None, target, {});
    KILN_CHECK(r.code() == Code::InvalidArgument);

    r = resolve_texture({.supercompression = Supercompression(7)}, SlotHint::None, target, {});
    KILN_CHECK(r.code() == Code::InvalidArgument);
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
TextureCookSettings tex_mut_supercompression(TextureCookSettings s) {
    s.supercompression = Supercompression::None;
    return s;
}
TextureCookSettings tex_mut_zstdLevel(TextureCookSettings s) {
    s.zstdLevel = 9;
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
    {"supercompression",  &tex_mut_supercompression },
    {"zstdLevel",         &tex_mut_zstdLevel        },
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

KILN_TEST(Settings, TargetHashCoversBlockFormats) {
    KILN_CHECK(hash_target(kCompatTarget) != hash_target(kDesktopTarget));
    KILN_CHECK(hash_target(kCompatTarget) != hash_target(kUncompressedTarget));
    TargetProfile same = kCompatTarget;
    same.blockFormats |= block_format_bit(Format::BC4_UNORM); // same name, other formats
    KILN_CHECK(hash_target(kCompatTarget) != hash_target(same));
}

// alphaCutoff: Auto takes the Mask material's cutoff (or nothing); explicit values beat it; usages without
// alpha warn and clear; out of range is an error; no mips, no coverage. Off hashes as before the field.
KILN_TEST(Settings, AlphaCutoffResolves) {
    ResolveDesc d{
        .asset = {
                  .name = "m.glb#leaves", .sourcePath = "m.glb", .slot = SlotHint::BaseColor, .alphaCutoff = 0.5f}
    };
    Result<TextureCookSettings> r = resolve_texture_layers(TextureCookSettings{}, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->alphaCutoff, 0.5f);

    TextureCookSettings off{};
    off.alphaCutoff = 0.0f;
    r               = resolve_texture_layers(off, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->alphaCutoff, 0.0f);

    ResolveDesc sidecar = d;
    sidecar.sidecar     = "alphaCutoff = 0.3";
    r                   = resolve_texture_layers(TextureCookSettings{}, sidecar);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->alphaCutoff, 0.3f);

    ResolveDesc plain       = d;
    plain.asset.alphaCutoff = 0.0f;
    r                       = resolve_texture_layers(TextureCookSettings{}, plain);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->alphaCutoff, 0.0f);
    Result<TextureCookSettings> const before =
        resolve_texture({.alphaCutoff = 0.0f}, SlotHint::BaseColor, {}, {});
    KILN_REQUIRE(before.ok());
    KILN_CHECK_EQ(hash_settings(*r), hash_settings(*before));
    KILN_CHECK(hash_settings(*r) != hash_settings(*resolve_texture_layers(TextureCookSettings{}, d)));

    DiagCapture cap;
    DiagSink sink = cap.sink();
    r = resolve_texture({.usage = TextureUsage::Normal, .alphaCutoff = 0.5f}, SlotHint::None, {}, {}, &sink);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->alphaCutoff, 0.0f);
    KILN_CHECK_EQ(cap.code, u32(kDiagSettingsInvalidCombo));
    KILN_CHECK(
        resolve_texture({.usage = TextureUsage::Color, .alphaCutoff = 1.5f}, SlotHint::None, {}, {}).code() ==
        Code::InvalidArgument);
    r = resolve_texture({.usage = TextureUsage::Color, .genMips = false, .alphaCutoff = 0.5f}, SlotHint::None,
                        {}, {});
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->alphaCutoff, 0.0f);
}
