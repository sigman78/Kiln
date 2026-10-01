// src/cook/cli_main.cpp — the kiln-cook command line (kiln/cook/cli.h): cook glTF/GLB, PNG,
// JPEG, WebP, Radiance HDR and KTX2 sources into the store. Exit codes: 0 all cooked, 1 usage, 2 IO, 3 cook
// errors.
#include "kiln/cook/cli.h"

#include "cli.h"
#include "manifest_store.h"
#include "trace_writer.h"
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

#include <cerrno>
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

/// A run rewrites the manifest at most this often while it cooks.
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
    char const* store           = "cooked";
    char const* defaultRoot     = nullptr; ///< --root without a name; null: the input directory
    char const* map             = nullptr;
    char const* trace           = nullptr; ///< --trace: Chrome trace file
    bool check                  = false;
    bool verify                 = false;   ///< compare input content, not size and time
    bool watch                  = false;   ///< keep cooking what changes until --timeout
    u32 timeoutS                = 0;       ///< --watch: stop after this many seconds; 0 = never
    bool gc                     = false;   ///< delete unreferenced artifacts instead of cooking
    bool dryRun                 = false;   ///< --gc: report only
    char const* exportDir       = nullptr; ///< write a runtime-only copy of the store instead of cooking
    bool quiet                  = false;
    bool verbose                = false;
    u32 threads                 = 0; ///< cooking threads including the main one; 0 = auto, 1 = no pool
    char const* profile         = "default";
    char const* meshCompression = nullptr; ///< null: MeshCookSettings's default
    u32 meshZstd                = 0;       ///< 0: kDefaultZstdLevel when the scheme uses Zstd
    char const* targetName      = nullptr; ///< null: compat when cooking, every profile for --export
    char const* quality         = "normal";
    u32 zstd                    = kDefaultZstdLevel; ///< 0: texture levels stay plain
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

/// Adds the sources under `dir`, recursively; false when a directory could not be listed in full
/// (the list is then partial).
bool scan_dir(char const* dir, FileList& out) {
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
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (std::strcmp(fd.cFileName, ".") != 0 && std::strcmp(fd.cFileName, "..") != 0) push(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    bool complete = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return false;
    for (;;) {
        errno           = 0;
        dirent* const e = readdir(d);
        if (!e) break;
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) push(e->d_name);
    }
    bool complete = errno == 0;
    closedir(d);
#endif
    sort_names(names, offs);
    for (usize i = 0; i < offs.size(); ++i) {
        char path[1024];
        format(path, sizeof path, "%s/%s", dir, names.data() + offs[i]);
        if (is_dir(path))
            complete &= scan_dir(path, out);
        else if (is_source_ext(extension(StrView(path))))
            out.add(path);
    }
    return complete;
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
    Vec<char> paths{default_allocator(), Tag::General}; ///< the files it depends on, NUL-separated
    u64 stats  = 0;                                     ///< files_stats(paths) when it failed
    bool retry = false; ///< an IO failure (a file still being written): try every round, quietly
};

/// The size and time of every file in `paths` (NUL-separated), hashed: a failed source waits for
/// a change to one of them.
u64 files_stats(Vec<char> const& paths) {
    Xxh64State h;
    for (usize at = 0; at < paths.size();) {
        StrView const p(paths.data() + at, std::strlen(paths.data() + at));
        at += p.size + 1;
        IoStat st{};
        h.update_value(u8(stat_file(p, &st).ok()));
        h.update_value(st.size);
        h.update_value(st.mtimeNs);
    }
    return h.digest();
}

/// The files a failed cook of the source `path` depends on: the source, its sidecar, and every
/// input the attempt read (a `.gltf`'s buffers).
void failed_paths(char const* path, CookUnit const& unit, Vec<char>& out) {
    auto const add = [&out](StrView p) {
        out.append(Span<char const>(p.data, p.size));
        out.push_back('\0');
    };
    char side[1100];
    add(StrView(path));
    add(StrView(side, format(side, sizeof side, "%s%.*s", path, KILN_SV(kSidecarExt))));
    for (UnitInput const& in : unit.inputs)
        if (in.role == InputRole::Buffer) add(unit.str(in.pathOff, in.pathLen));
}

struct Ctx {
    Options const& opt;
    DiagState& ds;
    DiagSink sink;
    CookSession session;
    std::FILE* map              = nullptr;
    JobSystem const* jobs       = nullptr; ///< null: single-threaded
    u32 maxThreads              = 0;       ///< CookEnv::maxThreads; 0 = no cap
    ProfileHooks const* profile = nullptr; ///< --trace
    CookPolicy policy           = {};
    ManifestStore* store        = nullptr; ///< the store writer; null with --check
    u64 hostDigest              = 0;
    char const* defaultRootDir  = nullptr;                ///< the default root of this run, if one is known
    Vec<char> scanned{default_allocator(), Tag::General}; ///< inputs scanned this round, NUL-separated
    u32 dropped     = 0;                                  ///< units whose source is gone
    bool scanFailed = false; ///< a root or input could not be scanned in full this round
    bool scanWarned = false; ///< --watch: that was reported in an earlier round
    bool fromTable  = false; ///< the inputs came from the store's root table
    bool rescan     = false; ///< a --watch round: quiet about sources that did not change
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

/// Publishes the outputs of `unit` that cooked as artifacts (nothing with --check); false if any
/// failed.
bool emit_unit(Ctx& c, CookUnit& unit) {
    if (c.store && publish_unit(c.store, unit, c.hostDigest, &c.sink).failed()) return false;
    bool ok = true;
    for (UnitOutput const& o : unit.outputs) {
        if (o.status.failed()) {
            ok = false;
            continue;
        }
        if (c.store) {
            char file[1200];
            (void)artifact_file_path(StrView(c.opt.store), o.key, file, sizeof file);
            report(c, unit.name(o), file, o.key, o.bytes.size());
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
        .env        = {.diag = &c.sink, .jobs = c.jobs, .maxThreads = c.maxThreads, .profile = c.profile},
        .statInputs = c.store != nullptr,
    };
    if (c.store && record_is_current(c.store, d, c.hostDigest, c.opt.verify)) {
        if (c.opt.verbose && !c.rescan) std::printf("  %-48s up to date\n", assetPath);
        ++c.skipped;
        return;
    }
    // --watch: a source that failed is cooked again when it changes, or every round after an IO
    // failure, reporting only the first failure of one version.
    u64 const pathHash      = hash_name(StrView(path));
    FailedSource const* was = c.failedSources.find(pathHash);
    bool const same         = was && files_stats(was->paths) == was->stats;
    if (same && !was->retry) return;
    c.ds.mute = same;

    Status st = cook_unit(d, &unit);
    if (st.failed() && unit.inputs.empty() && !c.ds.mute)
        std::fprintf(stderr, "kiln-cook: cannot read %s\n", path);
    bool const ok = st.ok() && emit_unit(c, unit);
    c.ds.mute     = false;
    if (ok) {
        ++c.cooked;
        c.failedSources.erase(pathHash);
        // A long run publishes as it goes, so an app watching the store fills in meanwhile.
        if (c.store) (void)commit_manifest(c.store, &c.sink, kCommitIntervalMs);
        return;
    }
    ++c.failed;
    if (st.ok()) st = unit.first_failure();
    bool const retry = st.code == Code::IoError || st.code == Code::IoEof || st.code == Code::NotFound;
    if (!c.opt.watch) return;
    FailedSource f;
    failed_paths(path, unit, f.paths);
    f.stats = files_stats(f.paths);
    f.retry = retry;
    c.failedSources.insert(pathHash, std::move(f));
}

/// An input could not be scanned in full: the run fails and drops nothing under it.
void scan_failed(Ctx& c, char const* input, char const* why) {
    c.scanFailed = true;
    if (!c.scanWarned) std::fprintf(stderr, "kiln-cook: %s: %s; nothing under it is dropped\n", input, why);
}

/// Drops the units whose source lay under a scanned input and is gone. Their artifacts stay for --gc.
void drop_vanished(Ctx& c) {
    if (!c.store) return;
    Vec<char> units(default_allocator(), Tag::General);
    unit_names(c.store, &units);
    for (usize at = 0; at < units.size();) {
        StrView const name(units.data() + at, std::strlen(units.data() + at));
        at += name.size + 1;
        AssetNameParts const parts = split_asset_name(name);
        char const* rootDir        = parts.root.empty() ? c.defaultRootDir : nullptr;
        for (NamedRoot const& r : c.opt.roots)
            if (parts.root == StrView(r.name)) rootDir = r.dir;
        if (!rootDir) continue; // a root this run does not know
        char src[2100];
        format(src, sizeof src, "%s/%.*s", rootDir, KILN_SV(parts.path));
        bool inScan = false;
        for (usize s = 0; s < c.scanned.size() && !inScan;) {
            char const* in = c.scanned.data() + s;
            inScan         = std::strcmp(src, in) == 0 || under(src, in);
            s += std::strlen(in) + 1;
        }
        // Only a source known to be gone drops its unit; an unreadable one (IO error) keeps it.
        IoStat st;
        if (!inScan || stat_file(StrView(src), &st).code != Code::NotFound) continue;
        drop_unit(c.store, name);
        ++c.dropped;
        if (!c.opt.quiet) std::printf("  %-48.*s    source gone, dropped\n", KILN_SV(name));
    }
}

/// Cooks every source under the inputs (cook_file() skips those that are up to date), then drops
/// the units whose source under a scanned input is gone.
void cook_inputs(Ctx& c) {
    c.scanned.clear();
    c.scanWarned = c.scanFailed && c.rescan; // a watch reports an unscannable input once
    c.scanFailed = false;
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
            // A directory counts as scanned, so its gone sources drop, only when listed in full.
            if (scan_dir(in, files))
                c.scanned.append(Span<char const>(in, std::strlen(in) + 1));
            else
                scan_failed(c, in, "cannot be listed in full");
            if (files.size() == 0 && !c.rescan) std::fprintf(stderr, "kiln-cook: no sources under %s\n", in);
            for (usize i = 0; i < files.size(); ++i)
                cook_file(c, files.at(i), root);
        } else if (file_exists(in)) {
            c.scanned.append(Span<char const>(in, std::strlen(in) + 1));
            cook_file(c, in, root);
        } else if (c.rescan || c.fromTable) {
            scan_failed(c, in, "missing");
        } else {
            cook_file(c, in, root); // reports that it cannot be read
        }
    }
    drop_vanished(c);
}

/// The directory of the default root an input implies: the input itself, or a file's directory.
void implied_root(char const* input, char* out, usize cap) {
    format(out, cap, "%s", input);
    normalize_slashes(out);
    usize n = std::strlen(out);
    while (n > 1 && out[n - 1] == '/')
        out[--n] = '\0';
    if (is_dir(out)) return;
    if (char* slash = std::strrchr(out, '/'))
        *slash = '\0';
    else
        format(out, cap, ".");
}

void print_gc(void* user, StrView file, u64 bytes) {
    if (*static_cast<bool const*>(user)) return;
    std::printf("  %-48.*s %llu B\n", KILN_SV(file), static_cast<unsigned long long>(bytes));
}

/// --watch: cooks what changed or appeared, twice a second, until the timeout (or forever). Each
/// round writes the manifest once, so an app with a store poller reloads the round together.
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
        if (Status const st = commit_manifest(c.store, &c.sink); st.failed())
            std::fprintf(stderr, "kiln-cook: cannot write the manifest (%s); retrying\n", code_name(st.code));
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

char const* const kProfiles[]         = {"default", "precise", "float", nullptr};
char const* const kMeshCompressions[] = {"none", "basic", "meshopt", "meshopt-zstd", nullptr};
char const* const kQualities[]        = {"fast", "normal", "high", nullptr};

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
        {.name = "--watch",
         .help = "after cooking, keep cooking sources that change or appear (Ctrl+C stops)",
         .flag = &o.watch},
        {.name   = "--timeout",
         .arg    = "<s>",
         .help   = "--watch: stop after this many seconds (default: never)",
         .number = &o.timeoutS},
        {.name = "--gc",
         .help = "instead of cooking: delete the artifacts no profile references, and leftover "
                 "temporary files", .flag = &o.gc},
        {.name = "--dry-run", .help = "--gc: list what would be deleted, delete nothing", .flag = &o.dryRun},
        {.name = "--export",
         .arg  = "<dir>",
         .help = "instead of cooking: write a runtime-only store (manifest.dir and its artifacts) into an "
                 "empty <dir>; --target picks one profile, default every profile", .str  = &o.exportDir},
        {.name = "--verify",
         .help = "check sources by their content, not by size and time (CI, shipping)",
         .flag = &o.verify},
        {.name = "--map",
         .arg  = "<file>",
         .help = "append \"<assetPath>\\t<file>\\t<build key>\" per written output",
         .str  = &o.map},
        {.name = "--trace",
         .arg  = "<file>",
         .help = "write a Chrome trace of the cook stages (chrome://tracing, ui.perfetto.dev) and a summary",
         .str  = &o.trace},
        {.name = "--target",
         .arg  = "<name>",
         .help = "target profile: compat, desktop or uncompressed to cook (default compat); with "
                 "--export, any profile of the store", .str  = &o.targetName},
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
        {.name    = "--mesh-compression",
         .arg     = "<scheme>",
         .help    = "mesh payload compression (default meshopt; docs/design/mesh-compression.md)",
         .str     = &o.meshCompression,
         .choices = kMeshCompressions},
        {.name   = "--mesh-zstd",
         .arg    = "<level>",
         .help   = "Zstd level of the basic and meshopt-zstd schemes, 1..19 (default 3)",
         .number = &o.meshZstd,
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
        .synopsis = "[<input>...] [options]",
        .options  = {opts, countof(opts)},
        .footer =
            "Inputs are source files or directories. The store records the roots a run used; with no\n"
            "inputs, kiln-cook scans those roots again (new and changed sources cook, gone ones leave).\n"
            "A root that is missing or cannot be listed in full drops nothing and makes the run exit 2.\n"
            "Exit codes: 0 all inputs cooked, 1 usage, 2 IO failure, 3 one or more cook errors (--watch: "
            "sources still failing at the end).",
        .positional = &add_input,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || (o.gc && o.exportDir) || (o.dryRun && !o.gc) ||
        ((o.gc || o.exportDir) && (!o.inputs.empty() || o.check || o.watch)) ||
        (o.check && o.inputs.empty())) {
        cli::usage(spec, stderr);
        return 1;
    }
    if (o.targetName) {
        if (char const* why = check_profile_name(StrView(o.targetName))) {
            std::fprintf(stderr, "kiln-cook: --target '%s': %s\n", o.targetName, why);
            return 1;
        }
        // Cooking needs the profile's definition: only the built-in ones have one here.
        if (!o.gc && !o.exportDir && !target_profile(StrView(o.targetName))) {
            std::fprintf(stderr,
                         "kiln-cook: --target '%s' is not a built-in profile (compat, desktop, "
                         "uncompressed); a custom profile cooks through the host's own tool\n",
                         o.targetName);
            return 1;
        }
    }
    DiagState ds{o.quiet, o.verbose};
    DiagSink const sink{&diag_fn, &ds};
    if (o.gc) {
        GcResult gc;
        bool quietList = o.quiet;
        Status const st =
            collect_store_garbage(StrView(o.store), o.dryRun, &print_gc, &quietList, &gc, &sink);
        if (!o.quiet)
            std::printf("gc%s: %u artifact(s), %u temporary file(s), %llu B%s\n",
                        o.dryRun ? " (dry run)" : "", gc.artifacts, gc.temporaries,
                        static_cast<unsigned long long>(gc.bytes),
                        o.dryRun ? " would be deleted" : " deleted");
        return st.ok() ? 0 : 2;
    }
    if (o.exportDir) {
        ExportResult ex;
        Status const st = export_store(StrView(o.store), StrView(o.exportDir),
                                       StrView(o.targetName ? o.targetName : ""), &ex, &sink);
        if (st.failed()) return 2;
        if (!o.quiet)
            std::printf("export: %u profile(s), %u artifact(s), %llu B into %s\n", ex.profiles, ex.artifacts,
                        static_cast<unsigned long long>(ex.bytes), o.exportDir);
        return 0;
    }
    o.mesh.genTangents     = !noTangents;
    o.mesh.optimize        = !noOptimize;
    o.mesh.useAuthoredLods = !noLods;
    o.tex.genMips          = !noMips;
    o.target      = *target_profile(StrView(o.targetName ? o.targetName : "compat")); // checked above
    o.tex.quality = std::strcmp(o.quality, "fast") == 0   ? EncodeQuality::Fast
                    : std::strcmp(o.quality, "high") == 0 ? EncodeQuality::High
                                                          : EncodeQuality::Normal;
    o.tex.supercompression = o.zstd != 0 ? Supercompression::Zstd : Supercompression::None;
    o.tex.zstdLevel        = u8(o.zstd);
    if (o.meshCompression)
        o.mesh.compression = std::strcmp(o.meshCompression, "basic") == 0     ? CompressionScheme::Basic
                             : std::strcmp(o.meshCompression, "meshopt") == 0 ? CompressionScheme::Meshopt
                             : std::strcmp(o.meshCompression, "meshopt-zstd") == 0
                                 ? CompressionScheme::MeshoptZstd
                                 : CompressionScheme::None;
    o.mesh.zstdLevel = u8(o.meshZstd);
    o.mesh.profile   = std::strcmp(o.profile, "float") == 0     ? VertexProfile::Float
                       : std::strcmp(o.profile, "precise") == 0 ? VertexProfile::Precise
                                                                : VertexProfile::Default;

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
    if (o.watch && o.check) {
        std::fprintf(stderr, "kiln-cook: --watch writes the store; it does not go with --check\n");
        return 1;
    }
    // What the options and the store's root table point into; they outlive the cooking.
    Vec<char> recorded(default_allocator(), Tag::General);
    char implied[1024] = {};
    if (!o.check) {
        Status const opened = open_manifest_store(
            {.storeDir = StrView(o.store), .target = &o.target, .diag = &c.sink}, &c.store);
        if (opened.failed()) return 2;
        // No inputs: the roots the store recorded. Given --root entries win over recorded ones.
        if (o.inputs.empty()) {
            store_roots(c.store, &recorded);
            for (usize at = 0; at < recorded.size();) {
                char const* name = recorded.data() + at;
                at += std::strlen(name) + 1;
                char const* dir = recorded.data() + at;
                at += std::strlen(dir) + 1;
                if (!*name) {
                    if (!o.defaultRoot) o.defaultRoot = dir;
                    continue;
                }
                bool given = false;
                for (NamedRoot const& r : o.roots)
                    given |= std::strcmp(r.name, name) == 0;
                if (given) continue;
                NamedRoot r{};
                format(r.name, sizeof r.name, "%s", name);
                format(r.dir, sizeof r.dir, "%s", dir);
                o.roots.push_back(r);
            }
            c.fromTable = true;
            if (o.defaultRoot) o.inputs.push_back(o.defaultRoot);
            for (NamedRoot const& r : o.roots)
                o.inputs.push_back(r.dir);
            if (o.inputs.empty()) {
                std::fprintf(stderr, "kiln-cook: no inputs, and %s records no roots: give the inputs once\n",
                             o.store);
                close_manifest_store(c.store);
                return 1;
            }
        }
        // The default root: --root <dir>, or the one directory every input implies.
        if (o.defaultRoot) {
            format(implied, sizeof implied, "%s", o.defaultRoot);
            normalize_slashes(implied);
            c.defaultRootDir = implied;
        } else {
            implied_root(o.inputs[0], implied, sizeof implied);
            c.defaultRootDir = implied;
            for (char const* input : o.inputs) {
                char other[1024];
                implied_root(input, other, sizeof other);
                if (std::strcmp(other, implied) != 0) c.defaultRootDir = nullptr;
            }
            if (!c.defaultRootDir && !o.quiet)
                std::fprintf(stderr,
                             "kiln-cook: the inputs lie in different directories; the store records no "
                             "default root for them\n");
        }
        Vec<Root> used(default_allocator(), Tag::General);
        if (c.defaultRootDir) used.push_back(Root{StrView(), StrView(c.defaultRootDir)});
        for (NamedRoot const& r : o.roots)
            used.push_back(Root{StrView(r.name), StrView(r.dir)});
        record_store_roots(c.store, used.span());

        UnitDesc const host{.meshDefaults    = &o.mesh,
                            .textureDefaults = &o.tex,
                            .nameRules       = kDefaultNameRules,
                            .policy          = policy,
                            .target          = &o.target,
                            .session         = c.session};
        c.hostDigest = host_digest(host, policyVersion);
    }

    cli::TraceWriter trace;
    ProfileHooks const traceHooks = trace.hooks();
    if (o.trace) c.profile = &traceHooks;

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
        if (Status const st = commit_manifest(c.store, &c.sink); st.failed())
            std::fprintf(stderr, "kiln-cook: cannot write the manifest (%s); retrying\n", code_name(st.code));
        std::fflush(stdout);
        watch_inputs(c);
    }
    if (c.map) std::fclose(c.map);
    if (c.jobs) destroy_thread_pool(pool);
    if (o.trace) {
        if (!o.quiet) trace.log_summary();
        if (!trace.write(o.trace)) std::fprintf(stderr, "kiln-cook: cannot write the trace %s\n", o.trace);
    }
    if (c.store) {
        Status const committed = commit_manifest(c.store, &c.sink);
        close_manifest_store(c.store);
        if (committed.failed()) {
            std::fprintf(stderr, "kiln-cook: cannot write the manifest of %s (%s)\n", o.store,
                         code_name(committed.code));
            return 2;
        }
    }

    if (o.watch) {
        // What counts at the end of a watch is what still fails, not what failed on the way.
        u32 const failing = u32(c.failedSources.size());
        if (!o.quiet) std::printf("watch: %u source(s) failing\n", failing);
        return c.scanFailed ? 2 : failing ? 3 : 0;
    }
    if (!o.quiet)
        std::printf("%s: %u cooked, %u up to date, %u dropped, %u failed, %u warning(s)\n",
                    o.check ? "check" : "cook", c.cooked, c.skipped, c.dropped, c.failed, ds.warnings);
    if (c.scanFailed) return 2;
    if (c.failed) return 3;
    return 0;
}
