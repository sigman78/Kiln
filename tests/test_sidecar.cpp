// tests/test_sidecar.cpp — `.kiln` sidecars: the TOML subset and the settings keys (cook-only).
#include "kiln_test.h"

#include "kiln/cook/sidecar.h"

#include <cstring>

using namespace kiln;
using namespace kiln::cook;

namespace {

struct DiagLast {
    u32 code  = 0;
    int count = 0;
    char where[128]{};

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagLast*>(user);
        self->code = d.code;
        ++self->count;
        format(self->where, sizeof self->where, "%.*s", KILN_SV(d.where));
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

} // namespace

KILN_TEST(Sidecar, TextureKeys) {
    StrView const text = "# wall_n.png.kiln\n"
                         "usage = \"normal\"   # trailing comment\n"
                         "colorSpace = 'linear'\n"
                         "\n"
                         "genMips = false\n"
                         "maxSize = 1024\n"
                         "flipGreen = true\n"
                         "normalRenormalize = false\n";
    TextureCookSettings s;
    KILN_REQUIRE(apply_sidecar(text, &s).ok());
    KILN_CHECK(s.usage == TextureUsage::Normal);
    KILN_CHECK(s.colorSpace == ColorSpace::Linear);
    KILN_CHECK(!s.genMips);
    KILN_CHECK_EQ(s.maxSize, 1024u);
    KILN_CHECK(s.flipGreen);
    KILN_CHECK(!s.normalRenormalize);
}

KILN_TEST(Sidecar, MeshKeys) {
    StrView const text = "profile = \"precise\"\ngenTangents = false\noptimize = false\n"
                         "useAuthoredLods = false\nposTolMm = 1\nweldTol = 0.5e-1\n";
    MeshCookSettings s;
    KILN_REQUIRE(apply_sidecar(text, &s).ok());
    KILN_CHECK(s.profile == VertexProfile::Precise);
    KILN_CHECK(!s.genTangents);
    KILN_CHECK(!s.optimize);
    KILN_CHECK(!s.useAuthoredLods);
    KILN_CHECK_EQ(f64(s.posTolMm), 1.0);
    KILN_CHECK_EQ(f64(s.weldTol), f64(0.05f));
    MeshCookSettings f;
    KILN_REQUIRE(apply_sidecar("profile = \"float\"\n", &f).ok());
    KILN_CHECK(f.profile == VertexProfile::Float);
}

KILN_TEST(Sidecar, KeysNotSetKeepTheirValues) {
    TextureCookSettings s;
    s.maxSize = 512;
    KILN_REQUIRE(apply_sidecar("genMips = false\n", &s).ok());
    KILN_CHECK_EQ(s.maxSize, 512u);
    KILN_REQUIRE(apply_sidecar("", &s).ok());
    KILN_REQUIRE(apply_sidecar("# only a comment", &s).ok());
}

KILN_TEST(Sidecar, BomCrlfAndEscapes) {
    StrView const text = "\xEF\xBB\xBFusage = \"nor\\u006dal\"\r\nmaxSize = +64\r\n";
    TextureCookSettings s;
    KILN_REQUIRE(apply_sidecar(text, &s).ok());
    KILN_CHECK(s.usage == TextureUsage::Normal);
    KILN_CHECK_EQ(s.maxSize, 64u);
}

KILN_TEST(Sidecar, SyntaxOutsideTheSubsetFails) {
    char const* const bad[] = {
        "usage \"normal\"",           // no '='
        "usage =",                    // no value
        "maxSize = [1, 2]",           // array
        "maxSize = {a = 1}",          // inline table
        "a.b = 1",                    // dotted key
        "\"usage\" = \"normal\"",     // quoted key
        "usage = \"\"\"normal\"\"\"", // multi-line string
        "maxSize = 1979-05-27",       // date
        "maxSize = 0x10",             // hex
        "maxSize = 01",               // leading zero
        "maxSize = 1_000",            // underscore
        "posTolMm = 1.",              // no digits after '.'
        "posTolMm = inf",             // inf
        "usage = \"normal\" junk",    // text after the value
        "[[target]]",                 // array of tables
        "[a]\n[a]",                   // table twice
        "maxSize = 1\nmaxSize = 2",   // key twice
        "usage = \"normal",           // unterminated
        "usage = \"\\q\"",            // unknown escape
        "usage = \"\\uD800\"",        // surrogate
    };
    for (char const* text : bad) {
        TextureCookSettings s;
        s.maxSize = 7;
        DiagLast d;
        DiagSink const sink = d.sink();
        Status const st     = apply_sidecar(StrView(text), &s, &sink, "t.kiln");
        KILN_CHECK_MSG(st.code == Code::ParseError, "accepted: %s", text);
        KILN_CHECK_MSG(d.code == kDiagSidecarSyntax, "%s: code %u", text, d.code);
        KILN_CHECK_EQ(s.maxSize, 7u); // unchanged on failure
    }
}

KILN_TEST(Sidecar, BadKeysAndValuesFail) {
    char const* const bad[] = {
        "usage = \"bump\"",             // unknown enum value
        "usage = 1",                    // wrong type
        "genMips = 1",                  // int for a bool
        "maxSize = -1",                 // out of range
        "maxSize = 1.5",                // float for an int
        "profile = \"precise\"",        // a mesh key on a texture
        "[target.mobile]\nmaxSize = 1", // tables are not used yet
    };
    for (char const* text : bad) {
        TextureCookSettings s;
        DiagLast d;
        DiagSink const sink = d.sink();
        Status const st     = apply_sidecar(StrView(text), &s, &sink, "t.kiln");
        KILN_CHECK_MSG(st.code == Code::InvalidArgument, "accepted: %s", text);
        KILN_CHECK_MSG(d.code == kDiagSidecarKey, "%s: code %u", text, d.code);
    }
}

KILN_TEST(Sidecar, DiagnosticNamesFileAndLine) {
    TextureCookSettings s;
    DiagLast d;
    DiagSink const sink = d.sink();
    KILN_CHECK(apply_sidecar("# header\ngenMips = true\nsize = 3\n", &s, &sink, "wall.png.kiln").failed());
    KILN_CHECK_EQ(StrView(d.where), StrView("wall.png.kiln:3"));
    KILN_CHECK_EQ(d.count, 1);
}

// ---------------------------------------------------------------------------
// Layered resolution: host settings, sidecar, inference, policy, resolve (settings.md)
// ---------------------------------------------------------------------------

namespace {

/// A test policy: records the usage it saw, then applies its own changes.
struct TestPolicy {
    TextureUsage seen   = TextureUsage::Auto;
    bool noMips         = false;
    bool makeNormal     = false;
    bool refuse         = false;
    bool meshNoOptimize = false;

    static Status texture(void* user, CookAssetInfo const&, TargetProfile const&, TextureCookSettings* s,
                          DiagSink const*) noexcept {
        auto* self = static_cast<TestPolicy*>(user);
        self->seen = s->usage;
        if (self->refuse) return make_status(Code::Unsupported);
        if (self->noMips) s->genMips = false;
        if (self->makeNormal) s->usage = TextureUsage::Normal;
        return kOk;
    }
    static Status mesh(void* user, CookAssetInfo const&, TargetProfile const&, MeshCookSettings* s,
                       DiagSink const*) noexcept {
        if (static_cast<TestPolicy*>(user)->meshNoOptimize) s->optimize = false;
        return kOk;
    }
    CookPolicy policy() noexcept { return CookPolicy{&texture, &mesh, this}; }
};

ResolveDesc desc_for(StrView name, StrView sidecar = {}, SlotHint slot = SlotHint::None) {
    return ResolveDesc{
        .asset = {name, name, slot},
          .sidecar = sidecar, .nameRules = kDefaultNameRules
    };
}

} // namespace

KILN_TEST(Layers, SidecarBeatsHostSettings) {
    TextureCookSettings host;
    host.genMips                  = false;
    Result<TextureCookSettings> r = resolve_texture_layers(host, desc_for("wall.png", "genMips = true\n"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->genMips);
    // Keys the sidecar does not name keep the host's value.
    host.maxSize = 512;
    r            = resolve_texture_layers(host, desc_for("wall.png", "genMips = true\n"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->maxSize, 512u);
}

KILN_TEST(Layers, InferenceFillsOnlyAutoUsage) {
    // Name rule, then slot, then Color.
    Result<TextureCookSettings> r = resolve_texture_layers({}, desc_for("wall_n.png"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Normal && r->colorSpace == ColorSpace::Linear);
    r = resolve_texture_layers({}, desc_for("chair.glb#wall_n", {}, SlotHint::BaseColor));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Color);
    r = resolve_texture_layers({}, desc_for("wall.png"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Color && r->colorSpace == ColorSpace::Srgb);
    // An explicit usage, from the sidecar or the host, is never replaced.
    r = resolve_texture_layers({}, desc_for("wall_n.png", "usage = \"color\"\n"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Color);
    TextureCookSettings host;
    host.usage = TextureUsage::Height;
    r          = resolve_texture_layers(host, desc_for("wall_n.png"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Height);
}

KILN_TEST(Layers, PolicyIsTheLastWord) {
    TestPolicy tp;
    tp.noMips                     = true;
    ResolveDesc d                 = desc_for("wall_n.png", "genMips = true\n");
    d.policy                      = tp.policy();
    Result<TextureCookSettings> r = resolve_texture_layers({}, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(!r->genMips);
    KILN_CHECK(tp.seen == TextureUsage::Normal); // the policy sees the inferred usage
}

KILN_TEST(Layers, DerivedFieldsFollowThePolicy) {
    TestPolicy tp;
    tp.makeNormal                 = true;
    ResolveDesc d                 = desc_for("wall.png");
    d.policy                      = tp.policy();
    Result<TextureCookSettings> r = resolve_texture_layers({}, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Normal);
    KILN_CHECK(r->colorSpace == ColorSpace::Linear); // not the sRGB of the inferred Color
    KILN_CHECK(r->normalRenormalize);
}

KILN_TEST(Layers, PolicyRefusalFails) {
    TestPolicy tp;
    tp.refuse = true;
    DiagLast dl;
    DiagSink const sink           = dl.sink();
    ResolveDesc d                 = desc_for("wall.png");
    d.policy                      = tp.policy();
    d.diag                        = &sink;
    Result<TextureCookSettings> r = resolve_texture_layers({}, d);
    KILN_CHECK_EQ(r.code(), Code::Unsupported);
    KILN_CHECK_EQ(dl.code, u32(kDiagPolicyRefused));
}

KILN_TEST(Layers, MeshSidecarThenPolicy) {
    MeshCookSettings host;
    host.genTangents           = false;
    ResolveDesc d              = desc_for("chair.glb", "genTangents = true\noptimize = true\n");
    Result<MeshCookSettings> r = resolve_mesh_layers(host, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->genTangents && r->optimize);
    TestPolicy tp;
    tp.meshNoOptimize = true;
    d.policy          = tp.policy();
    r                 = resolve_mesh_layers(host, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->genTangents && !r->optimize);
}

KILN_TEST(Layers, NameSuffixesStack) {
    NameHints h = hints_from_name("tex/rock_array_n.png", kDefaultNameRules);
    KILN_CHECK(h.usage == TextureUsage::Normal && h.shape == CookShape::Array);
    h = hints_from_name("sky_CUBE.png", kDefaultNameRules);
    KILN_CHECK(h.usage == TextureUsage::Auto && h.shape == CookShape::Cube);
    h = hints_from_name("sky_cube_albedo.png", kDefaultNameRules);
    KILN_CHECK(h.usage == TextureUsage::Color && h.shape == CookShape::Cube);
    h = hints_from_name("_cube.png", kDefaultNameRules); // no name left: no match
    KILN_CHECK(h.shape == CookShape::Auto);
    // The shape comes from the name only when no earlier layer set it.
    Result<TextureCookSettings> r = resolve_texture_layers({}, desc_for("sky_cube.png"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->shape == CookShape::Cube && r->usage == TextureUsage::Color);
    r = resolve_texture_layers({}, desc_for("sky_cube.png", "shape = \"2d\"\n"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->shape == CookShape::Tex2D);
}

KILN_TEST(Layers, ShapeAndSlicesKeys) {
    TextureCookSettings s;
    KILN_REQUIRE(apply_sidecar("shape = \"array\"\nslices = 16\n", &s).ok());
    KILN_CHECK(s.shape == CookShape::Array && s.slices == 16);
    KILN_CHECK(apply_sidecar("shape = \"volume\"\n", &s).failed());
    // slices means nothing for another shape: cleared with a warning, so the hash stays canonical.
    DiagLast dl;
    DiagSink const sink           = dl.sink();
    ResolveDesc d                 = desc_for("sky.png", "shape = \"cube\"\nslices = 4\n");
    d.diag                        = &sink;
    Result<TextureCookSettings> r = resolve_texture_layers({}, d);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->slices, 0u);
    KILN_CHECK_EQ(dl.code, u32(kDiagSettingsInvalidCombo));
}

// A .hdr source is HDR by its format: usage Hdr and a linear color space, whatever its name says.
KILN_TEST(Layers, HdrExtensionImpliesHdr) {
    Result<TextureCookSettings> r = resolve_texture_layers({}, desc_for("sky/env_albedo.HDR"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Hdr && r->colorSpace == ColorSpace::Linear);
    r = resolve_texture_layers({}, desc_for("sky/env.hdr", "usage = \"color\"\n"));
    KILN_REQUIRE(r.ok());
    KILN_CHECK(r->usage == TextureUsage::Color); // an explicit usage still wins (and the cook rejects it)
}
