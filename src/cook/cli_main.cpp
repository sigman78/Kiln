// src/cook/cli_main.cpp — the kiln-cook command line (kiln/cook/cli.h): cook glTF/GLB, PNG,
// JPEG, WebP, Radiance HDR and KTX2 sources into the store. Exit codes: 0 all cooked, 1 usage, 2 IO, 3 cook
// errors.
#include "kiln/cook/cli.h"

#include "cli.h"
#include "kiln/assets.h"
#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/cook/provider.h"
#include "kiln/cook/settings.h"
#include "kiln/cook/sidecar.h"
#include "kiln/hash.h"
#include "kiln/io.h"
#include "kiln/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#if defined(KILN_OS_WINDOWS)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

using namespace kiln;
using namespace kiln::cook;

namespace {

/// `--root <name>=<dir>`: files under `dir` get names `name:<path in dir>`.
struct NamedRoot {
    char name[64];
    char dir[1024];
};

struct Options {
    Vec<char const*> inputs{default_allocator(), Tag::General};
    Vec<NamedRoot> roots{default_allocator(), Tag::General};
    char const* store       = "cooked";
    char const* defaultRoot = nullptr; ///< --root without a name; null: the input directory
    char const* map         = nullptr;
    bool check              = false;
    bool hashed             = false; ///< false (default): Named layout, <store>/<assetPath>.<ext>
    bool quiet              = false;
    bool verbose            = false;
    u32 threads             = 0; ///< cooking threads including the main one; 0 = auto, 1 = no pool
    char const* profile     = "default";
    char const* targetName  = "desktop";
    MeshCookSettings mesh;
    TextureCookSettings tex;
    TargetProfile target;
};

// Tool-local path and file helpers (they predate the kiln IO layer, kiln/io.h).

bool read_file(char const* path, Vec<u8>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(usize(n));
    usize got = n ? std::fread(out.data(), 1, usize(n), f) : 0;
    std::fclose(f);
    return got == usize(n);
}

bool is_dir(char const* path) {
    struct stat st;
    return ::stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

void normalize_slashes(char* s) {
    for (; *s; ++s)
        if (*s == '\\') *s = '/';
}

StrView extension(StrView path) {
    usize dot = path.rfind('.'), slash = path.rfind('/');
    if (dot == StrView::kNpos || (slash != StrView::kNpos && dot < slash)) return {};
    return path.substr(dot + 1);
}

bool iequals(StrView a, char const* b) {
    StrView bv(b);
    if (a.size != bv.size) return false;
    for (usize i = 0; i < a.size; ++i) {
        char x = a[i], y = bv[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool is_source_ext(StrView ext) {
    return iequals(ext, "glb") || iequals(ext, "gltf") || iequals(ext, "png") || iequals(ext, "jpg") ||
           iequals(ext, "hdr") || iequals(ext, "jpeg") || (iequals(ext, "webp") && webp_decode_enabled()) ||
           iequals(ext, "ktx2");
}

/// mkdir -p for forward-slash paths.
bool make_dirs(char const* path) {
    char buf[1024];
    usize n = format(buf, sizeof buf, "%s", path);
    if (n >= sizeof buf - 1) return false;
    for (usize i = 1; i <= n; ++i) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i]     = '\0';
            if (!(i == 2 && buf[1] == ':')) { // skip "C:" on Windows
#if defined(KILN_OS_WINDOWS)
                _mkdir(buf);
#else
                ::mkdir(buf, 0755);
#endif
            }
            buf[i] = saved;
        }
    }
    return is_dir(path);
}

/// Collect source files under `dir` recursively (sorted per directory for determinism).
struct FileList {
    Vec<char> pool{default_allocator(), Tag::General}; ///< NUL-separated paths
    Vec<usize> offsets{default_allocator(), Tag::General};

    void add(char const* path) {
        offsets.push_back(pool.size());
        usize n        = std::strlen(path);
        Span<char> dst = pool.append_uninit(n + 1);
        std::memcpy(dst.data, path, n + 1);
    }
    [[nodiscard]] char const* at(usize i) const { return pool.data() + offsets[i]; }
    [[nodiscard]] usize size() const { return offsets.size(); }
};

void sort_names(Vec<char>& names, Vec<usize>& offs) {
    // insertion sort by strcmp (directory listings are small)
    for (usize i = 1; i < offs.size(); ++i) {
        usize key = offs[i];
        usize j   = i;
        while (j > 0 && std::strcmp(names.data() + offs[j - 1], names.data() + key) > 0) {
            offs[j] = offs[j - 1];
            --j;
        }
        offs[j] = key;
    }
}

void scan_dir(char const* dir, FileList& out) {
    Vec<char> names{default_allocator(), Tag::General};
    Vec<usize> offs{default_allocator(), Tag::General};
    auto push = [&](char const* name) {
        offs.push_back(names.size());
        usize n        = std::strlen(name);
        Span<char> dst = names.append_uninit(n + 1);
        std::memcpy(dst.data, name, n + 1);
    };
#if defined(KILN_OS_WINDOWS)
    char pattern[1024];
    format(pattern, sizeof pattern, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (std::strcmp(fd.cFileName, ".") != 0 && std::strcmp(fd.cFileName, "..") != 0) push(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* e = readdir(d))
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) push(e->d_name);
    closedir(d);
#endif
    sort_names(names, offs);
    for (usize i = 0; i < offs.size(); ++i) {
        char path[1024];
        format(path, sizeof path, "%s/%s", dir, names.data() + offs[i]);
        if (is_dir(path))
            scan_dir(path, out);
        else if (is_source_ext(extension(StrView(path))))
            out.add(path);
    }
}

/// True if `path` lies under the directory `root`.
bool under(char const* path, char const* root) {
    usize const rl = std::strlen(root);
    return rl && std::strncmp(path, root, rl) == 0 && path[rl] == '/';
}

/// The asset name of the source `path`: `root:<path in root>` for the named root with the
/// longest root that holds it, else the path relative to `root` (the default root), with
/// its extension. `*rootLen` gets the length of the root the name is relative to.
void asset_name_of(Options const& o, char const* path, char const* root, char* out, usize cap,
                   usize* rootLen) {
    NamedRoot const* best = nullptr;
    for (NamedRoot const& m : o.roots)
        if (under(path, m.dir) && (!best || std::strlen(m.dir) > std::strlen(best->dir))) best = &m;
    if (best) {
        *rootLen = std::strlen(best->dir);
        format(out, cap, "%s:%s", best->name, path + *rootLen + 1);
        return;
    }
    char const* rel = path;
    *rootLen        = 0;
    if (under(path, root)) {
        *rootLen = std::strlen(root);
        rel      = path + *rootLen + 1;
    } else if (char const* slash = std::strrchr(path, '/')) {
        *rootLen = usize(slash - path);
        rel      = slash + 1;
    }
    format(out, cap, "%s", rel);
}

struct DiagState {
    bool quiet;
    bool verbose;
    u32 errors   = 0;
    u32 warnings = 0;
};

void diag_fn(void* user, Diagnostic const& d) {
    DiagState& s = *static_cast<DiagState*>(user);
    if (d.severity == Severity::Error) ++s.errors;
    if (d.severity == Severity::Warning) ++s.warnings;
    if (s.quiet && d.severity != Severity::Error) return;
    if (!s.verbose && d.severity == Severity::Info) return;
    std::fprintf(stderr, "%s: K%04u %.*s%s%.*s%s%.*s\n", severity_name(d.severity), unsigned(d.code),
                 KILN_SV(d.asset), d.asset.empty() ? "" : " ", KILN_SV(d.where), d.where.empty() ? "" : ": ",
                 KILN_SV(d.message));
}

// ---------------------------------------------------------------------------
// Cooking
// ---------------------------------------------------------------------------

struct Ctx {
    Options const& opt;
    DiagState& ds;
    DiagSink sink;
    CookSession session;
    std::FILE* map        = nullptr;
    JobSystem const* jobs = nullptr; ///< null: single-threaded
    u32 maxThreads        = 0;       ///< CookEnv::maxThreads; 0 = no cap
    CookPolicy policy     = {};
    u32 cooked = 0, skipped = 0, failed = 0;
    HashMap<u64, u8> doneTextures{default_allocator(), Tag::General}; ///< by asset path hash
};

Status resolve_uri_fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) {
    char const* baseDir = static_cast<char const*>(user);
    char path[1024];
    format(path, sizeof path, "%s/%.*s", baseDir, KILN_SV(uri));
    Vec<u8> bytes(alloc, Tag::Cook);
    if (!read_file(path, bytes)) return make_status(Code::NotFound);
    *out = std::move(bytes);
    return kOk;
}

bool emit(Ctx& c, StrView assetPath, AssetKind kind, u64 key, Span<u8 const> bytes) {
    if (c.opt.check) return true;
    char const* ext = kind == AssetKind::Mesh ? "mesh" : "ktx2";
    char name[1200];
    char dir[1200];
    format(dir, sizeof dir, "%s", c.opt.store);
    if (c.opt.hashed) {
        store_file_name(key, StrView(ext), name, sizeof name);
    } else {
        // Named layout (default, kiln/assets.h StoreLayout::Named).
        if (store_file_path(StrView(c.opt.store), kind, assetPath, dir, sizeof dir) >= sizeof dir - 1) {
            std::fprintf(stderr, "kiln-cook: %.*s: store path too long\n", KILN_SV(assetPath));
            return false;
        }
        char* slash = std::strrchr(dir, '/');
        format(name, sizeof name, "%s", slash + 1);
        *slash = '\0';
        make_dirs(dir);
    }
    Status st = store_write(StrView(dir), StrView(name), bytes, &c.sink);
    if (st.failed()) {
        std::fprintf(stderr, "kiln-cook: cannot write %s/%s (%s)\n", dir, name, code_name(st.code));
        return false;
    }
    if (c.map)
        std::fprintf(c.map, "%.*s\t%s\t%016llx\n", KILN_SV(assetPath), name,
                     static_cast<unsigned long long>(key));
    if (!c.opt.quiet)
        std::printf("  %-48.*s -> %s (%llu B)\n", KILN_SV(assetPath), name,
                    static_cast<unsigned long long>(bytes.size));
    return true;
}

// --verbose per-stage timings (rollout step 1, docs/design/cook-kernels.md). Zero
// fields are stages the cook did not run (e.g. a KTX2 pass-through has none).
void print_texture_stats(CookStats const& s) {
    std::printf("  stats: decode %.1f ms, prepare %.1f ms, mips %.1f ms, write %.1f ms (total %.1f ms)\n",
                double(s.decodeUs) / 1000.0, double(s.prepareUs) / 1000.0, double(s.mipsUs) / 1000.0,
                double(s.writeUs) / 1000.0, double(s.totalUs) / 1000.0);
}

void print_mesh_stats(CookStats const& s) {
    std::printf("  stats: import %.1f ms, build %.1f ms, tangents %.1f ms, optimize %.1f ms, pack %.1f ms, "
                "write %.1f ms (total %.1f ms)\n",
                double(s.importUs) / 1000.0, double(s.buildUs) / 1000.0, double(s.tangentsUs) / 1000.0,
                double(s.optimizeUs) / 1000.0, double(s.packUs) / 1000.0, double(s.writeUs) / 1000.0,
                double(s.totalUs) / 1000.0);
}

/// The resolution inputs of one asset; the sidecar text, if any, lives in `text`.
struct Layers {
    ResolveDesc desc;
    Vec<u8> text{default_allocator(), Tag::Io};
    char path[1100] = {};
};

/// Fills `l` for `assetPath`. With `readSidecar`, reads `<sourcePath>.kiln` when it exists.
/// False if the sidecar cannot be read.
bool prepare_layers(Ctx& c, StrView assetPath, StrView sourcePath, SlotHint slot, bool readSidecar,
                    Layers& l) {
    l.desc = ResolveDesc{
        .asset     = {assetPath, sourcePath, slot},
        .nameRules = kDefaultNameRules,
        .policy    = c.policy,
        .target    = c.opt.target,
        .session   = c.session,
        .diag      = &c.sink,
    };
    if (!readSidecar) return true;
    usize const n = format(l.path, sizeof l.path, "%.*s%.*s", KILN_SV(sourcePath), KILN_SV(kSidecarExt));
    if (!io_file_exists(StrView(l.path, n))) return true;
    if (!read_file(l.path, l.text)) {
        std::fprintf(stderr, "kiln-cook: cannot read %s\n", l.path);
        return false;
    }
    l.desc.sidecar     = StrView(reinterpret_cast<char const*>(l.text.data()), l.text.size());
    l.desc.sidecarPath = StrView(l.path, n);
    return true;
}

bool cook_one_texture(Ctx& c, Span<u8 const> bytes, StrView assetPath, StrView sourcePath, SlotHint hint) {
    u64 pathHash = hash_name(assetPath);
    if (c.doneTextures.contains(pathHash)) return true;
    c.doneTextures.insert(pathHash, 1);

    // Only a file of its own has a sidecar; an embedded image has a slot instead.
    Layers layers;
    if (!prepare_layers(c, assetPath, sourcePath, hint, hint == SlotHint::None, layers)) return false;
    Result<TextureCookSettings> rs = resolve_texture_layers(c.opt.tex, layers.desc);
    if (rs.failed()) return false;
    TextureSource src{};
    src.bytes      = bytes;
    src.assetPath  = assetPath;
    src.sourcePath = sourcePath;
    Result<CookedTexture> r =
        cook_texture(src, *rs, c.opt.target, {.diag = &c.sink, .jobs = c.jobs, .maxThreads = c.maxThreads});
    if (r.failed()) return false;
    u64 key = store_key(r->sourceHash, hash_settings(*rs), hash_target(c.opt.target));
    if (!emit(c, assetPath, AssetKind::Texture, key, r->file.span())) return false;
    if (c.opt.verbose) print_texture_stats(r->stats);
    return true;
}

bool cook_one_mesh(Ctx& c, Span<u8 const> bytes, StrView assetPath, char const* sourcePath) {
    Layers layers;
    if (!prepare_layers(c, assetPath, StrView(sourcePath), SlotHint::None, true, layers)) return false;
    Result<MeshCookSettings> rs = resolve_mesh_layers(c.opt.mesh, layers.desc);
    if (rs.failed()) return false;

    char baseDir[1024];
    format(baseDir, sizeof baseDir, "%s", sourcePath);
    if (char* slash = std::strrchr(baseDir, '/'))
        *slash = '\0';
    else
        std::strcpy(baseDir, ".");

    MeshSource src{};
    src.bytes      = bytes;
    src.assetPath  = assetPath;
    src.sourcePath = StrView(sourcePath);
    src.resolver   = {&resolve_uri_fn, baseDir};
    Result<CookedMesh> r =
        cook_mesh(src, *rs, c.opt.target, {.diag = &c.sink, .jobs = c.jobs, .maxThreads = c.maxThreads});
    if (r.failed()) return false;
    u64 key = store_key(r->sourceHash, hash_settings(*rs), hash_target(c.opt.target));
    if (!emit(c, assetPath, AssetKind::Mesh, key, r->file.span())) return false;
    if (c.opt.verbose) print_mesh_stats(r->stats);

    // Embedded images only; files the mesh references by URI are cooked as inputs of their own.
    bool ok = true;
    for (TextureRef const& t : r->textures)
        ok &= cook_one_texture(c, t.embedded, t.assetPath, src.sourcePath, t.slot);
    return ok;
}

void cook_file(Ctx& c, char const* path, char const* root) {
    char assetPath[1024];
    usize rootLen = 0;
    asset_name_of(c.opt, path, root, assetPath, sizeof assetPath, &rootLen);
    StrView ext = extension(StrView(path));
    if (char const* why = check_asset_name(StrView(assetPath))) {
        std::fprintf(stderr, "kiln-cook: %s: '%s' is not a valid asset name (%s)\n", path, assetPath, why);
        ++c.failed;
        return;
    }
    if (!source_case_matches(StrView(path, rootLen), StrView(path + rootLen + (rootLen ? 1 : 0)))) {
        std::fprintf(stderr, "kiln-cook: %s: the name differs in case from the file on disk\n", path);
        ++c.failed;
        return;
    }

    Vec<u8> bytes(default_allocator(), Tag::Io);
    if (!read_file(path, bytes)) {
        std::fprintf(stderr, "kiln-cook: cannot read %s\n", path);
        ++c.failed;
        return;
    }
    if (c.opt.verbose) std::printf("cooking %s (%s)\n", path, assetPath);

    bool ok;
    if (iequals(ext, "glb") || iequals(ext, "gltf"))
        ok = cook_one_mesh(c, bytes.span(), StrView(assetPath), path);
    else
        ok = cook_one_texture(c, bytes.span(), StrView(assetPath), StrView(path), SlotHint::None);
    if (ok)
        ++c.cooked;
    else
        ++c.failed;
}

bool add_input(void* user, char const* arg) {
    static_cast<Options*>(user)->inputs.push_back(arg);
    return true;
}

/// `--root [<name>=]<dir>`. A prefix before `=` that is a valid root name names the root;
/// otherwise the whole argument is the directory of the default root.
bool add_root(void* user, char const* arg) {
    auto* o              = static_cast<Options*>(user);
    char const* const eq = std::strchr(arg, '=');
    NamedRoot m{};
    bool const named =
        eq && usize(eq - arg) < sizeof m.name && !check_root_name(StrView(arg, usize(eq - arg)));
    if (!named) {
        if (o->defaultRoot) {
            std::fprintf(stderr, "kiln-cook: --root: the default root is given twice\n");
            return false;
        }
        o->defaultRoot = arg;
        return true;
    }
    if (eq[1] == '\0') {
        std::fprintf(stderr, "kiln-cook: --root: '%s' has no directory\n", arg);
        return false;
    }
    format(m.name, sizeof m.name, "%.*s", int(eq - arg), arg);
    format(m.dir, sizeof m.dir, "%s", eq + 1);
    normalize_slashes(m.dir);
    usize n = std::strlen(m.dir);
    while (n > 1 && m.dir[n - 1] == '/')
        m.dir[--n] = '\0';
    for (NamedRoot const& other : o->roots)
        if (std::strcmp(other.name, m.name) == 0) {
            std::fprintf(stderr, "kiln-cook: --root: '%s' is given twice\n", m.name);
            return false;
        }
    o->roots.push_back(m);
    return true;
}

char const* const kProfiles[] = {"default", "precise", "float", nullptr};
char const* const kTargets[]  = {"desktop", nullptr};

} // namespace

int kiln::cook::cook_cli_main(int argc, char** argv, CookPolicy const& policy) noexcept {
    Options o;
    bool noTangents = false, noOptimize = false, noMips = false, noLods = false;
    cli::Option const opts[] = {
        {.name = "--store",
         .alt  = "-o",
         .arg  = "<dir>",
         .help = "store directory (default: cooked)",
         .str  = &o.store},
        {.name = "--root",
         .arg  = "[<name>=]<dir>",
         .help = "repeatable; <name>=<dir> names <name>:<path>, a bare <dir> is the default root",
         .each = &add_root,
         .user = &o},
        {.name = "--check", .help = "validate only: cook in memory, write nothing", .flag = &o.check},
        {.name = "--hashed",
         .help = "content-hash file names instead of <store>/<asset name>.<ext>",
         .flag = &o.hashed},
        {.name = "--map",
         .arg  = "<file>",
         .help = "append \"<assetPath>\\t<file name>\\t<key hex>\" per output",
         .str  = &o.map},
        {.name    = "--target",
         .arg     = "<name>",
         .help    = "target profile",
         .str     = &o.targetName,
         .choices = kTargets},
        {.name    = "--profile",
         .arg     = "<name>",
         .help    = "vertex profile",
         .str     = &o.profile,
         .choices = kProfiles},
        {.name = "--no-tangents", .help = "skip MikkTSpace tangents", .flag = &noTangents},
        {.name = "--no-optimize", .help = "skip the meshoptimizer passes", .flag = &noOptimize},
        {.name = "--no-mips", .help = "no mip chain for textures", .flag = &noMips},
        {.name = "--no-lods", .help = "ignore authored LODs", .flag = &noLods},
        {.name   = "--threads",
         .arg    = "<n>",
         .help   = "cooking threads including the main one: 0 = one per core, 1 = single-threaded",
         .number = &o.threads,
         .max    = 256},
        {.name = "--quiet", .alt = "-q", .help = "errors only", .flag = &o.quiet},
        {.name = "--verbose",
         .alt  = "-v",
         .help = "infos, diagnostics and per-asset stats",
         .flag = &o.verbose},
    };
    cli::Spec const spec{
        .program    = "kiln-cook",
        .synopsis   = "<input>... [options]",
        .options    = {opts, countof(opts)},
        .footer     = "Exit codes: 0 all inputs cooked, 1 usage, 2 IO failure, 3 one or more cook errors.",
        .positional = &add_input,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || o.inputs.empty()) {
        cli::usage(spec, stderr);
        return 1;
    }
    o.mesh.genTangents     = !noTangents;
    o.mesh.optimize        = !noOptimize;
    o.mesh.useAuthoredLods = !noLods;
    o.tex.genMips          = !noMips;
    o.mesh.profile         = std::strcmp(o.profile, "float") == 0     ? VertexProfile::Float
                             : std::strcmp(o.profile, "precise") == 0 ? VertexProfile::Precise
                                                                      : VertexProfile::Default;

    DiagState ds{o.quiet, o.verbose};
    Ctx c{
        o, ds, DiagSink{&diag_fn,                                    &ds  },
          CookSession{o.check ? StoreMode::None : StoreMode::Disk, false}
    };
    c.policy = policy;
    if (o.map && !o.check) {
        c.map = std::fopen(o.map, "ab");
        if (!c.map) {
            std::fprintf(stderr, "kiln-cook: cannot open map file %s\n", o.map);
            return 2;
        }
    }
    if (!o.check && !make_dirs(o.store)) {
        std::fprintf(stderr, "kiln-cook: cannot create store directory %s\n", o.store);
        return 2;
    }

    // The main thread cooks too, so the pool gets one worker fewer than --threads.
    JobSystem pool;
    c.maxThreads = o.threads;
    if (o.threads != 1) {
        Result<JobSystem> const created = create_thread_pool({.threads = o.threads ? o.threads - 1 : 0});
        if (created.ok()) {
            pool   = *created;
            c.jobs = &pool;
        } else {
            std::fprintf(stderr, "kiln-cook: cannot start a thread pool; cooking single-threaded\n");
        }
    }

    for (char const* input : o.inputs) {
        char in[1024];
        format(in, sizeof in, "%s", input);
        normalize_slashes(in);
        usize n = std::strlen(in);
        while (n > 1 && in[n - 1] == '/')
            in[--n] = '\0';

        char root[1024];
        if (o.defaultRoot) {
            format(root, sizeof root, "%s", o.defaultRoot);
            normalize_slashes(root);
        } else if (is_dir(in)) {
            format(root, sizeof root, "%s", in);
        } else {
            format(root, sizeof root, "%s", in);
            if (char* slash = std::strrchr(root, '/'))
                *slash = '\0';
            else
                root[0] = '\0';
        }

        if (is_dir(in)) {
            FileList files;
            scan_dir(in, files);
            if (files.size() == 0) std::fprintf(stderr, "kiln-cook: no sources under %s\n", in);
            for (usize i = 0; i < files.size(); ++i)
                cook_file(c, files.at(i), root);
        } else {
            cook_file(c, in, root);
        }
    }
    if (c.map) std::fclose(c.map);
    if (c.jobs) destroy_thread_pool(pool);

    if (!o.quiet)
        std::printf("%s: %u cooked, %u failed, %u warning(s)\n", o.check ? "check" : "cook", c.cooked,
                    c.failed, ds.warnings);
    if (c.failed) return 3;
    return 0;
}
