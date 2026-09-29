// tests/test_catalog.cpp — build keys and cook units (docs/design/store-catalog.md); cook-only.
#include "kiln_test.h"

#include "../src/cook/unit.h"
#include "kiln/cook/catalog.h"
#include "kiln/cook/cook.h"

#include <cstring>

using namespace kiln;
using namespace kiln::cook;

namespace {

Hash128 content_of(char const* s) {
    return xxh3_128(Span<u8 const>(reinterpret_cast<u8 const*>(s), std::strlen(s)));
}

} // namespace

KILN_TEST(BuildKey, EveryFieldCounts) {
    BuildInput const inputs[] = {
        {InputRole::Source, "a.glb", content_of("glb")},
        {InputRole::Buffer, "a.bin", content_of("bin")},
    };
    BuildKeyDesc const base{
        .kind = AssetKind::Mesh, .name = "a.glb", .targetHash = 1, .settingsHash = 2, .inputs = inputs};
    Hash128 const k = build_key(base);
    KILN_CHECK(k == build_key(base));

    BuildKeyDesc d = base;
    d.kind         = AssetKind::Texture;
    KILN_CHECK(!(build_key(d) == k));
    d      = base;
    d.name = "b.glb";
    KILN_CHECK(!(build_key(d) == k));
    d            = base;
    d.targetHash = 3;
    KILN_CHECK(!(build_key(d) == k));
    d              = base;
    d.settingsHash = 3;
    KILN_CHECK(!(build_key(d) == k));
    d        = base;
    d.inputs = Span<BuildInput const>(inputs, 1);
    KILN_CHECK(!(build_key(d) == k));

    BuildInput changed[] = {inputs[0], inputs[1]};
    changed[1].content   = content_of("bin2");
    d                    = base;
    d.inputs             = changed;
    KILN_CHECK(!(build_key(d) == k));
    changed[1]      = inputs[1];
    changed[1].name = "c.bin";
    KILN_CHECK(!(build_key(d) == k));
    changed[1]      = inputs[1];
    changed[1].role = InputRole::Sidecar;
    KILN_CHECK(!(build_key(d) == k));

    // A length prefix keeps name boundaries apart.
    BuildInput const split[] = {
        {InputRole::Source, "ab", {}},
        {InputRole::Source, "c",  {}}
    };
    BuildInput const moved[] = {
        {InputRole::Source, "a",  {}},
        {InputRole::Source, "bc", {}}
    };
    d                = base;
    d.inputs         = split;
    Hash128 const ks = build_key(d);
    d.inputs         = moved;
    KILN_CHECK(!(build_key(d) == ks));
}

KILN_TEST(CookUnit, RecordsEveryInputFile) {
    char dir[1024], source[1024];
    format(dir, sizeof dir, "%s/../gltf/generated", kiln::test::corpus_dir());
    format(source, sizeof source, "%s/external_uri.gltf", dir);

    MeshCookSettings const mesh;
    TextureCookSettings const tex;
    CookUnit unit(default_allocator());
    UnitDesc const d{.kind            = AssetKind::Mesh,
                     .name            = "external_uri.gltf",
                     .sourcePath      = StrView(source),
                     .meshDefaults    = &mesh,
                     .textureDefaults = &tex,
                     .target          = &kCompatTarget,
                     .statInputs      = true};
    KILN_REQUIRE(cook_unit(d, &unit).ok());

    // The source, its absent sidecar, and the buffer; the image it references by URI is an asset.
    KILN_REQUIRE_EQ(unit.inputs.size(), usize(3));
    KILN_CHECK(unit.inputs[0].role == InputRole::Source);
    KILN_CHECK(unit.str(unit.inputs[0].nameOff, unit.inputs[0].nameLen) == "external_uri.gltf"_sv);
    KILN_CHECK(unit.inputs[0].stat.size > 0);
    KILN_CHECK(unit.inputs[1].role == InputRole::Sidecar);
    KILN_CHECK(!unit.inputs[1].present);
    KILN_CHECK(unit.inputs[2].role == InputRole::Buffer);
    KILN_CHECK(unit.str(unit.inputs[2].nameOff, unit.inputs[2].nameLen) == "external_uri.bin"_sv);
    KILN_CHECK(unit.inputs[2].present && !unit.inputs[2].content.is_zero());

    KILN_REQUIRE_EQ(unit.outputs.size(), usize(1));
    UnitOutput const& o = unit.outputs[0];
    KILN_CHECK(o.kind == AssetKind::Mesh && o.status.ok() && !o.bytes.empty());

    // The key is the build key of the recorded inputs.
    BuildInput inputs[3];
    unit_build_inputs(unit, inputs);
    KILN_CHECK(o.key == build_key({.kind         = AssetKind::Mesh,
                                   .name         = "external_uri.gltf",
                                   .targetHash   = hash_target(kCompatTarget),
                                   .settingsHash = o.settingsHash,
                                   .inputs       = inputs}));

    // The same cook gives the same key and bytes.
    CookUnit again(default_allocator());
    KILN_REQUIRE(cook_unit(d, &again).ok());
    KILN_CHECK(again.outputs[0].key == o.key);
    KILN_CHECK(again.outputs[0].bytes.size() == o.bytes.size() &&
               std::memcmp(again.outputs[0].bytes.data(), o.bytes.data(), o.bytes.size()) == 0);
}

KILN_TEST(CookUnit, EmbeddedImagesAreOutputs) {
    char source[1024];
    format(source, sizeof source, "%s/../gltf/generated/pbr_textures.glb", kiln::test::corpus_dir());
    MeshCookSettings const mesh;
    TextureCookSettings const tex;
    CookUnit unit(default_allocator());
    KILN_REQUIRE(cook_unit({.kind            = AssetKind::Mesh,
                            .name            = "pbr_textures.glb",
                            .sourcePath      = StrView(source),
                            .meshDefaults    = &mesh,
                            .textureDefaults = &tex,
                            .target          = &kCompatTarget},
                           &unit)
                     .ok());
    KILN_REQUIRE(unit.outputs.size() > usize(1));
    KILN_CHECK(unit.outputs[0].kind == AssetKind::Mesh);
    for (usize i = 1; i < unit.outputs.size(); ++i) {
        UnitOutput const& o = unit.outputs[i];
        KILN_CHECK(o.kind == AssetKind::Texture && o.status.ok());
        KILN_CHECK(unit.name(o).starts_with("pbr_textures.glb#"_sv));
        KILN_CHECK(!(o.key == unit.outputs[0].key));
        KILN_CHECK(unit.find(AssetKind::Texture, unit.name(o)) == &o);
    }
}
