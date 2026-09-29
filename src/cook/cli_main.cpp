// src/cook/cli_main.cpp — the kiln-cook command line (kiln/cook/cli.h): cook glTF/GLB, PNG,
// JPEG, WebP, Radiance HDR and KTX2 sources into the store. Exit codes: 0 all cooked, 1 usage, 2 IO, 3 cook
// errors.
#include "kiln/cook/cli.h"

#include "catalog_store.h"
#include "cli.h"
#include "unit.h"

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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <thread>

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

/// Catalog layout: a run rewrites the catalog at most this often while it cooks.
constexpr u32 kCommitIntervalMs = 1000;
/// --watch: the pause between two scans of the sources.
constexpr u32 kWatchPollMs = 500;

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
    char const* layout      = "catalog"; ///< StoreLayout: catalog or named
    bool verify             = false;     ///< catalog: compare input content, not size and time
    bool watch              = false;     ///< catalog: keep cooking what changes until --timeout
    u32 timeoutS            = 0;         ///< --watch: stop after this many seconds; 0 = never
    bool quiet              = false;
    bool verbose            = false;
    u32 threads             = 0; ///< cooking threads including the main one; 0 = auto, 1 = no pool
    char const* profile     = "default";
    char const* targetName  = "compat";
    char const* quality     = "normal";
    u32 zstd                = kDefaultZstdLevel; ///< 0: texture levels stay plain
    MeshCookSettings mesh;
    TextureCookSettings tex;
    TargetProfile target;
};

// Tool-local path and file helpers (they predate the kiln IO layer, kiln/io.h).

bool file_exists(char const* path) {
    struct stat st;
    return ::stat(path, &st) == 0 && (st.st_mode & S_IFDIR) == 0;
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
    bool mute    = false; ///< --watch: a retry of a failure already reported
    u32 errors   = 0;
    u32 warnings = 0;
};

void diag_fn(void* user, Diagnostic const& d) {
    DiagState& s = *static_cast<DiagState*>(user);
    if (s.mute) return;
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

struct FailedSource {
    u64 stats  = 0;     ///< source_stats() when it failed
    bool retry = false; ///< an IO failure (a file still being written): try every round, quietly
};

/// The size and time of a source and its sidecar, hashed: a failed source waits for a change.
u64 source_stats(char const* path) {
    Xxh64State h;
    char side[1100];
    for (StrView const p :
         {StrView(path), StrView(side, format(side, sizeof side, "%s%.*s", path, KILN_SV(kSidecarExt)))}) {
        IoStat st{};
        h.update_value(u8(stat_file(p, &st).ok()));
        h.update_value(st.size);
        h.update_value(st.mtimeNs);
    }
    return h.digest();
}

struct Ctx {
    Options const& opt;
    DiagState& ds;
    DiagSink sink;
    CookSession session;
    std::FILE* map        = nullptr;
    JobSystem const* jobs = nullptr; ///< null: single-threaded
    u32 maxThreads        = 0;       ///< CookEnv::maxThreads; 0 = no cap
    CookPolicy policy     = {};
    CatalogStore* store   = nullptr; ///< the catalog layout's writer; null: named layout or --check
    u64 hostDigest        = 0;
    bool rescan           = false; ///< a --watch round: quiet about sources that did not change
    /// --watch: sources whose cook failed, by path hash: the stats they failed with.
    HashMap<u64, FailedSource> failedSources{default_allocator(), Tag::General};
    u32 cooked = 0, skipped = 0, failed = 0;
};

/// Reports one written output.
void report(Ctx& c, StrView assetPath, char const* file, Hash128 const& key, u64 bytes) {
    char hex[33];
    hash128_hex(key, hex);
    if (c.map) std::fprintf(c.map, "%.*s\t%s\t%s\n", KILN_SV(assetPath), file, hex);
    if (!c.opt.quiet)
        std::printf("  %-48.*s -> %s (%llu B)\n", KILN_SV(assetPath), file,
                    static_cast<unsigned long long>(bytes));
}

/// Named layout: writes `<store>/<name>.<ext>`.
bool emit(Ctx& c, StrView assetPath, AssetKind kind, Hash128 const& key, Span<u8 const> bytes) {
    if (c.opt.check) return true;
    char dir[1200];
    if (store_file_path(StrView(c.opt.store), kind, assetPath, dir, sizeof dir) >= sizeof dir - 1) {
        std::fprintf(stderr, "kiln-cook: %.*s: store path too long\n", KILN_SV(assetPath));
        return false;
    }
    char* slash = std::strrchr(dir, '/');
    char name[1200];
    format(name, sizeof name, "%s", slash + 1);
    *slash = '\0';
    make_dirs(dir);
    Status st = store_write(StrView(dir), StrView(name), bytes, &c.sink);
    if (st.failed()) {
        std::fprintf(stderr, "kiln-cook: cannot write %s/%s (%s)\n", dir, name, code_name(st.code));
        return false;
    }
    report(c, assetPath, name, key, bytes.size);
    return true;
}

// --verbose per-stage timings (rollout step 1, docs/design/cook-kernels.md). Zero
// fields are stages the cook did not run (e.g. a KTX2 pass-through has none).
void print_texture_stats(CookStats const& s) {
    std::printf("  stats: decode %.1f ms, prepare %.1f ms, mips %.1f ms, encode %.1f ms, write %.1f ms "
                "(total %.1f ms)\n",
                double(s.decodeUs) / 1000.0, double(s.prepareUs) / 1000.0, double(s.mipsUs) / 1000.0,
                double(s.encodeUs) / 1000.0, double(s.writeUs) / 1000.0, double(s.totalUs) / 1000.0);
}

void print_mesh_stats(CookStats const& s) {
    std::printf("  stats: import %.1f ms, build %.1f ms, tangents %.1f ms, optimize %.1f ms, pack %.1f ms, "
                "write %.1f ms (total %.1f ms)\n",
                double(s.importUs) / 1000.0, double(s.buildUs) / 1000.0, double(s.tangentsUs) / 1000.0,
                double(s.optimizeUs) / 1000.0, double(s.packUs) / 1000.0, double(s.writeUs) / 1000.0,
                double(s.totalUs) / 1000.0);
}

/// Writes the outputs of `unit` that cooked; false if any failed. The catalog layout publishes
/// them as artifacts.
bool emit_unit(Ctx& c, CookUnit& unit) {
    if (c.store && publish_unit(c.store, unit, c.hostDigest, &c.sink).failed()) return false;
    bool ok = true;
    for (UnitOutput const& o : unit.outputs) {
        bool written = o.status.ok();
        if (written && c.store) {
            char file[1200];
            (void)artifact_file_path(StrView(c.opt.store), o.kind, o.key, file, sizeof file);
            report(c, unit.name(o), file, o.key, o.bytes.size());
        } else if (written) {
            written = emit(c, unit.name(o), o.kind, o.key, o.bytes.span());
        }
        if (!written) {
            ok = false;
            continue;
        }
        if (!c.opt.verbose) continue;
        if (o.kind == AssetKind::Mesh)
            print_mesh_stats(o.stats);
        else
            print_texture_stats(o.stats);
    }
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

    if (c.opt.verbose) std::printf("cooking %s (%s)\n", path, assetPath);

    bool const mesh = iequals(ext, "glb") || iequals(ext, "gltf");
    CookUnit unit(default_allocator());
    UnitDesc d{
        .kind            = mesh ? AssetKind::Mesh : AssetKind::Texture,
        .name            = StrView(assetPath),
        .sourcePath      = StrView(path),
        .meshDefaults    = &c.opt.mesh,
        .textureDefaults = &c.opt.tex,
        .nameRules       = kDefaultNameRules,
        .policy          = c.policy,
        .target          = &c.opt.target,
        .session         = c.session,
        .env             = {.diag = &c.sink, .jobs = c.jobs, .maxThreads = c.maxThreads},
        .statInputs      = c.store != nullptr,
    };
    if (c.store && record_is_current(c.store, d, c.hostDigest, c.opt.verify)) {
        if (c.opt.verbose && !c.rescan) std::printf("  %-48s up to date\n", assetPath);
        ++c.skipped;
        return;
    }
    // --watch: a source that failed is cooked again when it changes, or every round after an IO
    // failure, reporting only the first failure of one version.
    u64 const pathHash      = hash_name(StrView(path));
    u64 const stats         = c.opt.watch ? source_stats(path) : 0;
    FailedSource const* was = c.failedSources.find(pathHash);
    if (was && was->stats == stats && !was->retry) return;
    c.ds.mute = was && was->stats == stats;

    Status st = cook_unit(d, &unit);
    if (st.failed() && unit.inputs.empty() && !c.ds.mute)
        std::fprintf(stderr, "kiln-cook: cannot read %s\n", path);
    bool const ok = st.ok() && emit_unit(c, unit);
    c.ds.mute     = false;
    if (ok) {
        ++c.cooked;
        c.failedSources.erase(pathHash);
        // A long run publishes as it goes, so an app watching the store fills in meanwhile.
        if (c.store) (void)commit_catalog(c.store, &c.sink, kCommitIntervalMs);
        return;
    }
    ++c.failed;
    if (st.ok()) st = unit.first_failure();
    bool const retry = st.code == Code::IoError || st.code == Code::IoEof || st.code == Code::NotFound;
    if (c.opt.watch) c.failedSources.insert(pathHash, FailedSource{stats, retry});
}

/// Cooks every source under the inputs (cook_file() skips those that are up to date).
void cook_inputs(Ctx& c) {
    for (char const* input : c.opt.inputs) {
        char in[1024];
        format(in, sizeof in, "%s", input);
        normalize_slashes(in);
        usize n = std::strlen(in);
        while (n > 1 && in[n - 1] == '/')
            in[--n] = '\0';

        char root[1024];
        if (c.opt.defaultRoot) {
            format(root, sizeof root, "%s", c.opt.defaultRoot);
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
            if (files.size() == 0 && !c.rescan) std::fprintf(stderr, "kiln-cook: no sources under %s\n", in);
            for (usize i = 0; i < files.size(); ++i)
                cook_file(c, files.at(i), root);
        } else if (!c.rescan || file_exists(in)) {
            cook_file(c, in, root);
        }
    }
}

/// --watch: cooks what changed or appeared, twice a second, until the timeout (or forever). Each
/// round writes the catalog once, so an app with a store poller reloads the round together.
void watch_inputs(Ctx& c) {
    auto const start = std::chrono::steady_clock::now();
    c.rescan         = true;
    if (!c.opt.quiet) std::printf("watching the sources; Ctrl+C stops\n");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kWatchPollMs));
        if (c.opt.timeoutS &&
            std::chrono::steady_clock::now() - start >= std::chrono::seconds(c.opt.timeoutS))
            return;
        u32 const cooked = c.cooked, failed = c.failed;
        cook_inputs(c);
        if (Status const st = commit_catalog(c.store, &c.sink); st.failed())
            std::fprintf(stderr, "kiln-cook: cannot write the catalog (%s); retrying\n", code_name(st.code));
        if (!c.opt.quiet && (c.cooked != cooked || c.failed != failed)) {
            std::printf("watch: %u cooked, %u failed\n", c.cooked - cooked, c.failed - failed);
            std::fflush(stdout);
        }
    }
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

char const* const kProfiles[]  = {"default", "precise", "float", nullptr};
char const* const kTargets[]   = {"compat", "desktop", "uncompressed", nullptr};
char const* const kQualities[] = {"fast", "normal", "high", nullptr};
char const* const kLayouts[]   = {"catalog", "named", nullptr};

} // namespace

int kiln::cook::cook_cli_main(int argc, char** argv, CookPolicy const& policy, u32 policyVersion) noexcept {
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
        {.name    = "--layout",
         .arg     = "<layout>",
         .help    = "catalog (default): artifacts and catalogs/<target>.kcat; named: <store>/<name>.<ext>",
         .str     = &o.layout,
         .choices = kLayouts},
        {.name = "--watch",
         .help = "catalog: after cooking, keep cooking sources that change or appear (Ctrl+C stops)",
         .flag = &o.watch},
        {.name   = "--timeout",
         .arg    = "<s>",
         .help   = "--watch: stop after this many seconds (default: never)",
         .number = &o.timeoutS},
        {.name = "--verify",
         .help = "catalog: check sources by their content, not by size and time (CI, shipping)",
         .flag = &o.verify},
        {.name = "--map",
         .arg  = "<file>",
         .help = "append \"<assetPath>\\t<file>\\t<build key>\" per written output",
         .str  = &o.map},
        {.name    = "--target",
         .arg     = "<name>",
         .help    = "target profile: the block formats it samples (default compat)",
         .str     = &o.targetName,
         .choices = kTargets},
        {.name    = "--quality",
         .arg     = "<level>",
         .help    = "block encoder effort (default normal)",
         .str     = &o.quality,
         .choices = kQualities},
        {.name   = "--zstd",
         .arg    = "<level>",
         .help   = "Zstd level of texture files, 1..19 (default 3); 0 stores them plain",
         .number = &o.zstd,
         .max    = kMaxZstdLevel},
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
        .program  = "kiln-cook",
        .synopsis = "<input>... [options]",
        .options  = {opts, countof(opts)},
        .footer =
            "Exit codes: 0 all inputs cooked, 1 usage, 2 IO failure, 3 one or more cook errors (--watch: "
            "sources still failing at the end).",
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
    o.target               = *target_profile(StrView(o.targetName)); // one of kTargets
    o.tex.quality          = std::strcmp(o.quality, "fast") == 0   ? EncodeQuality::Fast
                             : std::strcmp(o.quality, "high") == 0 ? EncodeQuality::High
                                                                   : EncodeQuality::Normal;
    o.tex.supercompression = o.zstd != 0 ? Supercompression::Zstd : Supercompression::None;
    o.tex.zstdLevel        = u8(o.zstd);
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
    bool const catalog = std::strcmp(o.layout, "catalog") == 0;
    if ((o.verify || o.watch) && !catalog) {
        std::fprintf(stderr, "kiln-cook: --verify and --watch need the catalog layout\n");
        return 1;
    }
    if (o.watch && o.check) {
        std::fprintf(stderr, "kiln-cook: --watch writes the store; it does not go with --check\n");
        return 1;
    }
    if (!o.check && catalog) {
        Status const opened = open_catalog_store(
            {.storeDir = StrView(o.store), .target = &o.target, .diag = &c.sink}, &c.store);
        if (opened.failed()) {
            if (opened.code == Code::InvalidArgument)
                std::fprintf(stderr, "kiln-cook: %s is not a catalog store; --layout named cooks into it\n",
                             o.store);
            return 2;
        }
        UnitDesc const host{.meshDefaults    = &o.mesh,
                            .textureDefaults = &o.tex,
                            .nameRules       = kDefaultNameRules,
                            .policy          = policy,
                            .target          = &o.target,
                            .session         = c.session};
        c.hostDigest = host_digest(host, policyVersion);
    }
    // A named store holds files of one profile (docs/design/target-profiles.md).
    if (!o.check && !catalog && bind_store_profile(StrView(o.store), o.target, &c.sink).failed()) return 2;

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

    cook_inputs(c);
    if (o.watch) {
        if (!o.quiet)
            std::printf("cook: %u cooked, %u up to date, %u failed, %u warning(s)\n", c.cooked, c.skipped,
                        c.failed, ds.warnings);
        if (Status const st = commit_catalog(c.store, &c.sink); st.failed())
            std::fprintf(stderr, "kiln-cook: cannot write the catalog (%s); retrying\n", code_name(st.code));
        std::fflush(stdout);
        watch_inputs(c);
    }
    if (c.map) std::fclose(c.map);
    if (c.jobs) destroy_thread_pool(pool);
    if (c.store) {
        Status const committed = commit_catalog(c.store, &c.sink);
        close_catalog_store(c.store);
        if (committed.failed()) {
            std::fprintf(stderr, "kiln-cook: cannot write the catalog of %s (%s)\n", o.store,
                         code_name(committed.code));
            return 2;
        }
    }

    if (o.watch) {
        // What counts at the end of a watch is what still fails, not what failed on the way.
        u32 const failing = u32(c.failedSources.size());
        if (!o.quiet) std::printf("watch: %u source(s) failing\n", failing);
        return failing ? 3 : 0;
    }
    if (!o.quiet)
        std::printf("%s: %u cooked, %u up to date, %u failed, %u warning(s)\n", o.check ? "check" : "cook",
                    c.cooked, c.skipped, c.failed, ds.warnings);
    if (c.failed) return 3;
    return 0;
}
