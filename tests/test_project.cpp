// tests/test_project.cpp — kiln.toml (kiln/cook/project.h): loading, globs, layers 3a to 3d, and
// kiln-cook's --project, [roots] and [project].
#include "kiln_test.h"

#include "kiln/cook/cli.h"
#include "kiln/cook/project.h"
#include "kiln/cook/settings.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

using namespace kiln;
using namespace kiln::cook;

namespace {

struct DiagLast {
    u32 code = 0;
    char where[256]{};

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<DiagLast*>(user);
        self->code = d.code;
        format(self->where, sizeof self->where, "%.*s", KILN_SV(d.where));
    }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

void fresh_dir(char const* name, char* out, usize cap) {
    format(out, cap, "%s/%s", test::sample_dir(), name);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
    std::filesystem::create_directories(out, ec);
}

bool write_text(char const* path, char const* text) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool const ok = std::fwrite(text, 1, std::strlen(text), f) == std::strlen(text);
    return std::fclose(f) == 0 && ok;
}

/// Loads `text` as <sample dir>/<name>/kiln.toml; `path` receives the file's path.
struct Loaded {
    Project* p = nullptr;
    DiagLast diag;
    char path[1024]{};

    Status load(char const* name, char const* text, StrView overrides = {}) {
        char dir[1024];
        fresh_dir(name, dir, sizeof dir);
        format(path, sizeof path, "%s/kiln.toml", dir);
        if (!write_text(path, text)) return make_status(Code::IoError);
        DiagSink const sink = diag.sink();
        Result<Project*> const r =
            load_project({.path = StrView(path), .overrides = overrides}, nullptr, &sink);
        if (r.failed()) return r.status();
        p = *r;
        return kOk;
    }
    ~Loaded() { free_project(p); }
};

Result<TextureCookSettings> resolve_tex(Project const* p, StrView name, StrView sidecar = {},
                                        TargetProfile const& target = kCompatTarget,
                                        SlotHint slot               = SlotHint::None) {
    return resolve_texture_layers(
        {
    },
        ResolveDesc{.asset     = {name, name, slot},
                    .project   = p,
                    .sidecar   = sidecar,
                    .nameRules = kDefaultNameRules,
                    .target    = target});
}

constexpr char kProjectText[] = R"(# kiln.toml
[roots]
default = "assets"
mods = "../mods"

[project]
store = "build/store"
target = "desktop"

[texture]
quality = "high"
maxSize = 4096

[mesh]
compression = "meshopt-zstd"

[texture.preset.ui]
usage = "ui"
genMips = false

[texture.preset.foliage]
alphaCutoff = 0.5

[[texture.rule]]
match = ["ui/**", "mods:hud/**"]
preset = "ui"

[[texture.rule]]
match = ["env/trees/**", "props/*.glb#*leaf*"]
preset = "foliage"
maxSize = 2048

[[texture.rule]]
match = ["env/**"]          # never reached for env/trees: the first match wins
maxSize = 512

[[texture.rule]]
match = ["sky/**"]
targets = ["compat"]
maxSize = 1024

[[mesh.rule]]
match = ["props/**"]
optimize = false
)";

} // namespace

KILN_TEST(Project, Globs) {
    KILN_CHECK(glob_match("ui/**", "ui/a.png"));
    KILN_CHECK(glob_match("ui/**", "ui/x/y/a.png"));
    KILN_CHECK(glob_match("**/a.png", "a.png")); // '**/' matches zero segments
    KILN_CHECK(glob_match("**/a.png", "x/y/a.png"));
    KILN_CHECK(glob_match("x/**/a.png", "x/a.png"));
    KILN_CHECK(glob_match("*.png", "a.png"));
    KILN_CHECK(!glob_match("*.png", "x/a.png")); // '*' stays in one segment
    KILN_CHECK(glob_match("a?.png", "ab.png"));
    KILN_CHECK(!glob_match("a?c", "a/c"));
    KILN_CHECK(glob_match("props/*.glb#*leaf*", "props/tree.glb#big_leaf_2"));
    KILN_CHECK(!glob_match("props/*.glb#*leaf*", "props/tree.glb#bark"));
    KILN_CHECK(glob_match("**", "anything/at/all.png"));
    // Roots: a pattern without one matches the default root only.
    KILN_CHECK(!glob_match("**", "m:a.png"));
    KILN_CHECK(glob_match("m:**", "m:a.png"));
    KILN_CHECK(!glob_match("m:**", "a.png"));
    KILN_CHECK(!glob_match("mm:**", "m:a.png"));
}

KILN_TEST(Project, LoadsTablesRootsAndProject) {
    Loaded l;
    Status const st = l.load("project_load", kProjectText);
    if (!KILN_CHECK_MSG(st.ok(), "K%u at %s", l.diag.code, l.diag.where)) return;
    Span<Root const> const roots = project_roots(l.p);
    KILN_REQUIRE_EQ(roots.size, usize(2));
    KILN_CHECK(roots[0].name.empty());
    KILN_CHECK(roots[0].dir.ends_with("project_load/assets"));
    KILN_CHECK(roots[1].name == "mods");
    KILN_CHECK(roots[1].dir.ends_with("project_load/../mods"));
    KILN_CHECK(project_store(l.p).ends_with("project_load/build/store"));
    KILN_CHECK(project_target(l.p) == "desktop");
}

KILN_TEST(Project, RootsDropTrailingSeparators) {
    Loaded l;
    KILN_REQUIRE(l.load("project_root_trim", "[roots]\ndefault = \"src//\"\nmm = \"mods\\\\\"\n").ok());
    Span<Root const> const roots = project_roots(l.p);
    KILN_REQUIRE_EQ(roots.size, usize(2));
    KILN_CHECK(roots[0].dir.ends_with("project_root_trim/src"));
    KILN_CHECK(roots[1].dir.ends_with("project_root_trim/mods"));
}

#ifdef _WIN32
// The last separator of either kind ends the project file's directory.
KILN_TEST(Project, MixedSeparatorsInTheFilePath) {
    char dir[1024], sub[1100], file[1100];
    fresh_dir("project_mixed_sep", dir, sizeof dir);
    format(sub, sizeof sub, "%s/configs", dir);
    std::error_code ec;
    std::filesystem::create_directories(sub, ec);
    format(file, sizeof file, "%s/kiln.toml", sub);
    KILN_REQUIRE(write_text(file, "[roots]\ndefault = \"src\"\n"));
    format(file, sizeof file, "%s\\kiln.toml", sub);
    Result<Project*> const r = load_project({.path = StrView(file)});
    KILN_REQUIRE(r.ok());
    KILN_CHECK(project_roots(*r)[0].dir.ends_with("configs/src"));
    free_project(*r);
}
#endif

KILN_TEST(Project, DefaultsPresetsAndFirstMatchingRule) {
    Loaded l;
    KILN_REQUIRE(l.load("project_rules", kProjectText).ok());

    Result<TextureCookSettings> plain = resolve_tex(l.p, "rock.png");
    KILN_REQUIRE(plain.ok());
    KILN_CHECK(plain->quality == EncodeQuality::High); // [texture]
    KILN_CHECK_EQ(plain->maxSize, 4096u);
    KILN_CHECK(plain->usage == TextureUsage::Color);

    Result<TextureCookSettings> ui = resolve_tex(l.p, "ui/button.png");
    KILN_REQUIRE(ui.ok());
    KILN_CHECK(ui->usage == TextureUsage::Ui && !ui->genMips); // the preset
    KILN_CHECK(ui->quality == EncodeQuality::High);            // defaults still apply below the rule
    Result<TextureCookSettings> hud = resolve_tex(l.p, "mods:hud/icon.png");
    KILN_REQUIRE(hud.ok());
    KILN_CHECK(hud->usage == TextureUsage::Ui);

    Result<TextureCookSettings> leaf =
        resolve_tex(l.p, "props/tree.glb#big_leaf", {}, kCompatTarget, SlotHint::BaseColor);
    KILN_REQUIRE(leaf.ok());
    KILN_CHECK_EQ(leaf->alphaCutoff, 0.5f); // embedded images match rules too
    KILN_CHECK_EQ(leaf->maxSize, 2048u);    // the rule's key after its preset

    Result<TextureCookSettings> tree = resolve_tex(l.p, "env/trees/oak.png");
    KILN_REQUIRE(tree.ok());
    KILN_CHECK_EQ(tree->maxSize, 2048u); // the first match wins: env/** never applies
    Result<TextureCookSettings> env = resolve_tex(l.p, "env/rock.png");
    KILN_REQUIRE(env.ok());
    KILN_CHECK_EQ(env->maxSize, 512u);

    // `targets`: the rule applies only when cooking for a listed profile.
    Result<TextureCookSettings> skyCompat  = resolve_tex(l.p, "sky/day.png", {}, kCompatTarget);
    Result<TextureCookSettings> skyDesktop = resolve_tex(l.p, "sky/day.png", {}, kDesktopTarget);
    KILN_REQUIRE(skyCompat.ok() && skyDesktop.ok());
    KILN_CHECK_EQ(skyCompat->maxSize, 1024u);
    KILN_CHECK_EQ(skyDesktop->maxSize, 4096u);

    Result<MeshCookSettings> mesh =
        resolve_mesh_layers({}, ResolveDesc{.asset = {"props/chair.glb"}, .project = l.p});
    KILN_REQUIRE(mesh.ok());
    KILN_CHECK(mesh->compression == CompressionScheme::MeshoptZstd);
    KILN_CHECK(!mesh->optimize);
    Result<MeshCookSettings> other =
        resolve_mesh_layers({}, ResolveDesc{.asset = {"chair.glb"}, .project = l.p});
    KILN_REQUIRE(other.ok());
    KILN_CHECK(other->optimize);
}

KILN_TEST(Project, OverridesBeatTheFileAndSidecarsBeatOverrides) {
    Loaded l;
    KILN_REQUIRE(
        l.load("project_overrides", kProjectText, "[texture]\nquality = \"fast\"\nmaxSize = 256\n").ok());
    Result<TextureCookSettings> s = resolve_tex(l.p, "ui/button.png");
    KILN_REQUIRE(s.ok());
    KILN_CHECK(s->quality == EncodeQuality::Fast);
    KILN_CHECK_EQ(s->maxSize, 256u);
    KILN_CHECK(s->usage == TextureUsage::Ui); // the rule still applies where the flags say nothing
    Result<TextureCookSettings> side = resolve_tex(l.p, "ui/button.png", "maxSize = 128\n");
    KILN_REQUIRE(side.ok());
    KILN_CHECK_EQ(side->maxSize, 128u);
}

// Usage sections apply after inference, beat the project defaults, and fill only what rules,
// flags, sidecars and inference left.
KILN_TEST(Project, UsageSections) {
    Loaded l;
    KILN_REQUIRE(l.load("project_usage",
                        "[texture]\nmaxSize = 4096\n"
                        "[texture.usage.normal]\nmaxSize = 2048\nquality = \"fast\"\nshape = \"2d\"\n"
                        "[[texture.rule]]\nmatch = [\"hero/**\"]\nmaxSize = 512\n")
                     .ok());
    Result<TextureCookSettings> slot =
        resolve_tex(l.p, "props/a.glb#nrm", {}, kCompatTarget, SlotHint::Normal); // usage from the glTF slot
    KILN_REQUIRE(slot.ok());
    KILN_CHECK_EQ(slot->maxSize, 2048u);
    KILN_CHECK(slot->quality == EncodeQuality::Fast);
    Result<TextureCookSettings> named = resolve_tex(l.p, "rock_n.png"); // usage from a name rule
    KILN_REQUIRE(named.ok());
    KILN_CHECK_EQ(named->maxSize, 2048u);
    Result<TextureCookSettings> hero = resolve_tex(l.p, "hero/face_n.png"); // the rule's key stays
    KILN_REQUIRE(hero.ok());
    KILN_CHECK_EQ(hero->maxSize, 512u);
    KILN_CHECK(hero->quality == EncodeQuality::Fast);
    Result<TextureCookSettings> side = resolve_tex(l.p, "rock_n.png", "maxSize = 256\n");
    KILN_REQUIRE(side.ok());
    KILN_CHECK_EQ(side->maxSize, 256u);
    Result<TextureCookSettings> color = resolve_tex(l.p, "rock.png");
    KILN_REQUIRE(color.ok());
    KILN_CHECK_EQ(color->maxSize, 4096u);
    KILN_CHECK(color->quality == EncodeQuality::Normal);
    // A shape the name implies is inference (layer 5): the usage section does not replace it.
    Result<TextureCookSettings> array = resolve_tex(l.p, "rock_array_n.png");
    KILN_REQUIRE(array.ok());
    KILN_CHECK(array->shape == CookShape::Array);
    KILN_CHECK(named->shape == CookShape::Tex2D);

    Loaded bad1, bad2, bad3;
    KILN_CHECK(bad1.load("project_usage_bad1", "[texture.usage.bump]\nmaxSize = 1\n").failed());
    KILN_CHECK(bad2.load("project_usage_bad2", "[texture.usage.normal]\nusage = \"color\"\n").failed());
    KILN_CHECK(bad3.load("project_usage_bad3", "[texture.usage.auto]\n").failed());
    KILN_CHECK_EQ(bad3.diag.code, u32(kDiagSidecarKey));
}

KILN_TEST(Project, OverridesAreTables) {
    Loaded l;
    KILN_CHECK(l.load("project_over_array", "", "[[texture]]\nmaxSize = 4\n").failed());
    KILN_CHECK_EQ(l.diag.code, u32(kDiagSidecarKey));
}

KILN_TEST(Project, OverridesWithoutAFile) {
    DiagLast d;
    DiagSink const sink = d.sink();
    Result<Project*> r = load_project({.overrides = "[mesh]\noptimize = false\n[texture]\n"}, nullptr, &sink);
    KILN_REQUIRE(r.ok());
    Result<MeshCookSettings> mesh = resolve_mesh_layers({}, ResolveDesc{.asset = {"a.glb"}, .project = *r});
    KILN_REQUIRE(mesh.ok());
    KILN_CHECK(!mesh->optimize);
    KILN_CHECK_EQ(project_roots(*r).size, usize(0));
    free_project(*r);
}

KILN_TEST(Project, DigestFollowsSettingsNotComments) {
    Loaded a, b, c;
    KILN_REQUIRE(a.load("project_digest_a", "[texture]\nmaxSize = 1024\n").ok());
    KILN_REQUIRE(b.load("project_digest_b",
                        "# a comment\n[roots]\ndefault = \"x\"\n[texture]\nmaxSize = 1024 # same\n")
                     .ok());
    KILN_REQUIRE(c.load("project_digest_c", "[texture]\nmaxSize = 2048\n").ok());
    KILN_CHECK_EQ(project_digest(a.p), project_digest(b.p));
    KILN_CHECK(project_digest(a.p) != project_digest(c.p));
}

KILN_TEST(Project, ErrorsNameFileAndLine) {
    struct Case {
        char const* text;
        u32 code;
        u32 line;
    };
    Case const cases[] = {
        {"maxSize = 1",                                                kDiagSidecarKey,    1}, // keys belong in a table
        {"[textures]",                                                 kDiagSidecarKey,    1}, // unknown table
        {"[texture]\nbogus = 1",                                       kDiagSidecarKey,    2},
        {"[mesh]\nmaxSize = 1",                                        kDiagSidecarKey,    2}, // a texture key on meshes
        {"[[texture.rule]]\npreset = \"ui\"",                          kDiagSidecarKey,    2}, // no such preset
        {"[[texture.rule]]\nmaxSize = 1",                              kDiagSidecarKey,    1}, // no match
        {"[[texture.rule]]\nmatch = []",                               kDiagSidecarKey,    2},
        {"[[texture.rule]]\nmatch = [\"a**\"]",                        kDiagProjectGlob,   2},
        {"[[texture.rule]]\nmatch = [\"Bad:**\"]",                     kDiagProjectGlob,   2},
        {"[[texture.rule]]\nmatch = [\"**\"]\ntargets = [\"mobile\"]", kDiagSidecarKey,    3},
        {"[texture.preset]\nx = 1",                                    kDiagSidecarKey,    2},
        {"[texture.preset.ui.typo]\nnonsense = true",                  kDiagSidecarKey,    1}, // not a preset
        {"[[texture]]\nmaxSize = 4\n[[texture]]\ngenMips = false",     kDiagSidecarKey,    1},
        {"[[texture.preset.ui]]\nmaxSize = 4",                         kDiagSidecarKey,    1},
        {"[[project]]\nstore = \"x\"",                                 kDiagSidecarKey,    1},
        {"[texture.rule]\nmatch = [\"**\"]",                           kDiagSidecarKey,    1},
        {"[target.lowend]",                                            kDiagSidecarKey,    1}, // reserved
        {"[roots]\nBad = \"x\"",                                       kDiagSidecarKey,    2},
        {"[roots]\nm = 1",                                             kDiagSidecarKey,    2},
        {"[project]\nstores = \"x\"",                                  kDiagSidecarKey,    2},
        {"[texture]\nmaxSize = [1]",                                   kDiagSidecarKey,    2},
        {"[texture]\nmaxSize = 1\nmaxSize = 2",                        kDiagSidecarSyntax, 3},
    };
    int i = 0;
    for (Case const& c : cases) {
        char name[32];
        format(name, sizeof name, "project_err_%d", i++);
        Loaded l;
        KILN_CHECK_MSG(l.load(name, c.text).failed(), "accepted: %s", c.text);
        KILN_CHECK_MSG(l.diag.code == c.code, "%s: code %u", c.text, l.diag.code);
        char suffix[32];
        format(suffix, sizeof suffix, "kiln.toml:%u", c.line);
        KILN_CHECK_MSG(StrView(l.diag.where).ends_with(StrView(suffix)), "%s: where %s", c.text,
                       l.diag.where);
    }
    DiagLast d;
    DiagSink const sink = d.sink();
    KILN_CHECK(load_project({.path = "no/such/kiln.toml"}, nullptr, &sink).failed());
    KILN_CHECK_EQ(d.code, u32(kDiagProjectRead));
}

namespace {

/// The layer and place that last set each key.
struct TraceLog {
    struct Row {
        char key[32], layer[64], where[512];
    };
    Row rows[32] = {};
    u32 count    = 0;

    static void fn(void* user, StrView key, StrView layer, StrView where) {
        auto* self = static_cast<TraceLog*>(user);
        Row* row   = nullptr;
        for (u32 i = 0; i < self->count && !row; ++i)
            if (StrView(self->rows[i].key) == key) row = &self->rows[i];
        if (!row) row = &self->rows[self->count++];
        format(row->key, sizeof row->key, "%.*s", KILN_SV(key));
        format(row->layer, sizeof row->layer, "%.*s", KILN_SV(layer));
        format(row->where, sizeof row->where, "%.*s", KILN_SV(where));
    }
    Row const* find(StrView key) const {
        for (u32 i = 0; i < count; ++i)
            if (StrView(rows[i].key) == key) return &rows[i];
        return nullptr;
    }
};

} // namespace

// SettingsTrace reports the layer that set each key last: --explain's source.
KILN_TEST(Project, TraceNamesTheLayers) {
    Loaded l;
    KILN_REQUIRE(l.load("project_trace", kProjectText, "[texture]\nquality = \"fast\"\n").ok());
    TraceLog log;
    SettingsTrace const trace{&TraceLog::fn, &log};
    Result<TextureCookSettings> r = resolve_texture_layers(
        {
    },
        ResolveDesc{.asset       = {"env/trees/oak.png", "oak.png"},
                    .project     = l.p,
                    .sidecar     = "zstdLevel = 9\n",
                    .sidecarPath = "oak.png.kiln",
                    .trace       = &trace});
    KILN_REQUIRE(r.ok());
    auto layer = [&](StrView key) {
        return log.find(key) ? StrView(log.find(key)->layer) : StrView("default");
    };
    KILN_CHECK(layer("alphaCutoff") == "preset foliage (rule #2)");
    KILN_CHECK(layer("maxSize") == "rule #2");
    KILN_CHECK(StrView(log.find("maxSize")->where).ends_with("kiln.toml:31"));
    KILN_CHECK(layer("quality") == "kiln-cook flags");
    KILN_CHECK(layer("zstdLevel") == "sidecar");
    KILN_CHECK(StrView(log.find("zstdLevel")->where) == "oak.png.kiln:1");
    KILN_CHECK(layer("usage") == "inferred");
    KILN_CHECK(layer("colorSpace") == "resolve");
    KILN_CHECK(layer("genMips") == "default");
}

KILN_TEST(Project, DefaultIsAReservedRootName) {
    KILN_CHECK(check_root_name("default") != nullptr);
    KILN_CHECK(check_root_name("defaults") == nullptr);
}

namespace {

int run_cli(std::initializer_list<char const*> args) {
    char storage[12][1100];
    char* argv[12];
    int argc = 0;
    for (char const* a : args) {
        format(storage[argc], sizeof storage[argc], "%s", a);
        argv[argc] = storage[argc];
        ++argc;
    }
    return cook_cli_main(argc, argv);
}

/// The build key the map file `map` lists for `asset`, or empty.
void key_in_map(char const* map, char const* asset, char* out, usize cap) {
    out[0]       = '\0';
    std::FILE* f = std::fopen(map, "rb");
    if (!f) return;
    char line[2048];
    while (std::fgets(line, sizeof line, f)) {
        usize const n = std::strlen(asset);
        if (std::strncmp(line, asset, n) != 0 || line[n] != '\t') continue;
        char const* key = std::strrchr(line, '\t') + 1;
        format(out, cap, "%.*s", int(std::strcspn(key, "\r\n")), key);
    }
    std::fclose(f);
}

} // namespace

// kiln-cook takes roots, store and settings from --project; its flags beat the file.
KILN_TEST(ProjectCli, RootsStoreAndFlags) {
    char dir[1024], src[1100], file[1100], mapA[1100], mapB[1100], storeB[1100], from[1100], to[1100];
    fresh_dir("project_cli", dir, sizeof dir);
    format(src, sizeof src, "%s/src", dir);
    std::error_code ec;
    std::filesystem::create_directories(src, ec);
    format(from, sizeof from, "%s/../gltf/generated/external_uri_albedo.png", test::corpus_dir());
    format(to, sizeof to, "%s/albedo.png", src);
    std::filesystem::copy_file(from, to, ec);
    KILN_REQUIRE(!ec);
    format(file, sizeof file, "%s/kiln.toml", dir);
    KILN_REQUIRE(write_text(file, "[roots]\ndefault = \"src\"\n[project]\nstore = \"store\"\n"
                                  "[texture]\ngenMips = false\nquality = \"high\"\n"));
    format(mapA, sizeof mapA, "%s/a.map", dir);
    format(mapB, sizeof mapB, "%s/b.map", dir);
    format(storeB, sizeof storeB, "%s/store_b", dir);

    // The project: no inputs, no --store; --quality fast beats the file's high.
    KILN_REQUIRE_EQ(run_cli({"kiln-cook", "--project", file, "--quality", "fast", "--map", mapA, "-q"}), 0);
    char store[1100];
    format(store, sizeof store, "%s/store/manifest.dir", dir);
    KILN_CHECK(std::filesystem::exists(store));
    // The same settings from flags alone give the same build key.
    KILN_REQUIRE_EQ(
        run_cli({"kiln-cook", src, "-o", storeB, "--no-mips", "--quality", "fast", "--map", mapB, "-q"}), 0);
    char keyA[128], keyB[128];
    key_in_map(mapA, "albedo.png", keyA, sizeof keyA);
    key_in_map(mapB, "albedo.png", keyB, sizeof keyB);
    KILN_CHECK(keyA[0] != '\0');
    KILN_CHECK(StrView(keyA) == StrView(keyB));
}

namespace {

/// <sample dir>/<name> with src/albedo.png and a kiln.toml holding `text`.
bool project_dir(char const* name, char const* text, char* dir, usize dirCap, char* file, usize fileCap) {
    char src[1100], from[1100], to[1100];
    fresh_dir(name, dir, dirCap);
    format(src, sizeof src, "%s/src", dir);
    std::error_code ec;
    std::filesystem::create_directories(src, ec);
    format(from, sizeof from, "%s/../gltf/generated/external_uri_albedo.png", test::corpus_dir());
    format(to, sizeof to, "%s/albedo.png", src);
    std::filesystem::copy_file(from, to, ec);
    format(file, fileCap, "%s/kiln.toml", dir);
    return !ec && write_text(file, text);
}

/// Runs `kiln-cook --project <file> --watch` for 3 s and writes `edit` to the file after 1 s.
int watch_and_edit(char const* file, char const* edit) {
    int code = -1;
    std::thread cook(
        [&] { code = run_cli({"kiln-cook", "--project", file, "-q", "--watch", "--timeout", "3"}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    bool const written = write_text(file, edit);
    cook.join();
    return written ? code : -1;
}

} // namespace

// A reload frees the old project: the store and roots it gave must outlive it.
KILN_TEST(ProjectCli, WatchReloadKeepsRootsAndStore) {
    char dir[1024], file[1100];
    KILN_REQUIRE(
        project_dir("project_watch_reload",
                    "[roots]\ndefault = \"src\"\n[project]\nstore = \"store\"\n[texture]\nmaxSize = 64\n",
                    dir, sizeof dir, file, sizeof file));
    KILN_CHECK_EQ(watch_and_edit(file, "[roots]\ndefault = \"src\"\n[project]\nstore = \"store\"\n"
                                       "[texture]\nmaxSize = 128\n"),
                  0);
}

// A source that failed under the old settings cooks again when a reload changes them.
KILN_TEST(ProjectCli, WatchReloadRetriesFailedSources) {
    char dir[1024], file[1100];
    KILN_REQUIRE(project_dir(
        "project_watch_retry",
        "[roots]\ndefault = \"src\"\n[project]\nstore = \"store\"\n[texture]\nencoding = \"bc6h\"\n", dir,
        sizeof dir, file, sizeof file));
    KILN_CHECK_EQ(watch_and_edit(file, "[roots]\ndefault = \"src\"\n[project]\nstore = \"store\"\n"
                                       "[texture]\nencoding = \"uncompressed\"\n"),
                  0);
}

// A trailing separator does not change a root: "src/" names the directory "src" does.
KILN_TEST(ProjectCli, RootsDropTrailingSeparators) {
    char dir[1024], file[1100], map[1100], key[128];
    KILN_REQUIRE(project_dir("project_root_slash", "[roots]\nmods = \"src/\"\n[project]\nstore = \"store\"\n",
                             dir, sizeof dir, file, sizeof file));
    format(map, sizeof map, "%s/a.map", dir);
    KILN_REQUIRE_EQ(run_cli({"kiln-cook", "--project", file, "--map", map, "-q"}), 0);
    key_in_map(map, "mods:albedo.png", key, sizeof key);
    KILN_CHECK(key[0] != '\0');
}
