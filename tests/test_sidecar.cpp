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
