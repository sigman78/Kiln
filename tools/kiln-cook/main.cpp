// tools/kiln-cook/main.cpp — cook glTF/GLB, PNG and KTX2 sources into the store. Options: README.md.
// Exit codes: 0 all inputs cooked, 1 usage, 2 IO failure, 3 one or more cook errors.
#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/settings.h"
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

struct Options {
    Vec<char const*> inputs{default_allocator(), Tag::General};
    char const* store = "cooked";
    char const* root  = nullptr;
    char const* map   = nullptr;
    bool check        = false;
    bool hashed       = false; ///< false (default): Named layout, <store>/<assetPath>.<ext>
    bool quiet        = false;
    bool verbose      = false;
    u32 threads       = 0; ///< cooking threads including the main one; 0 = auto, 1 = no pool
    MeshCookSettings mesh;
    TextureCookSettings tex;
    TargetProfile target;
};

int usage() {
    std::fputs(
        "usage: kiln-cook <input>... [-o <store>] [--root <dir>] [--check] [--hashed] [--map <file>]\n"
        "                 [--target <name>] [--profile default|precise] [--no-tangents]\n"
        "                 [--no-optimize] [--no-mips] [--no-lods] [--threads <n>] [--quiet] [--verbose]\n"
        "  --threads <n>  cooking threads including the main one: 0 (default) = one per core,\n"
        "                 1 = single-threaded. Cooked bytes are identical for every value.\n",
        stderr);
    return 1;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        char const* a = argv[i];
        auto next     = [&](char const*& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (std::strcmp(a, "-o") == 0 || std::strcmp(a, "--store") == 0) {
            if (!next(o.store)) return false;
        } else if (std::strcmp(a, "--root") == 0) {
            if (!next(o.root)) return false;
        } else if (std::strcmp(a, "--map") == 0) {
            if (!next(o.map)) return false;
        } else if (std::strcmp(a, "--target") == 0) {
            char const* t;
            if (!next(t)) return false;
            if (std::strcmp(t, "desktop") != 0) {
                std::fprintf(stderr, "kiln-cook: unknown target '%s' (v0.5 has only 'desktop')\n", t);
                return false;
            }
        } else if (std::strcmp(a, "--profile") == 0) {
            char const* p;
            if (!next(p)) return false;
            if (std::strcmp(p, "default") == 0)
                o.mesh.profile = VertexProfile::Default;
            else if (std::strcmp(p, "precise") == 0)
                o.mesh.profile = VertexProfile::Precise;
            else
                return false;
        } else if (std::strcmp(a, "--threads") == 0) {
            char const* n;
            if (!next(n)) return false;
            char* end        = nullptr;
            long const value = std::strtol(n, &end, 10);
            if (end == n || *end != '\0' || value < 0 || value > 256) return false;
            o.threads = u32(value);
        } else if (std::strcmp(a, "--check") == 0)
            o.check = true;
        else if (std::strcmp(a, "--hashed") == 0)
            o.hashed = true;
        else if (std::strcmp(a, "--no-tangents") == 0)
            o.mesh.genTangents = false;
        else if (std::strcmp(a, "--no-optimize") == 0)
            o.mesh.optimize = false;
        else if (std::strcmp(a, "--no-lods") == 0)
            o.mesh.useAuthoredLods = false;
        else if (std::strcmp(a, "--no-mips") == 0)
            o.tex.genMips = false;
        else if (std::strcmp(a, "--quiet") == 0 || std::strcmp(a, "-q") == 0)
            o.quiet = true;
        else if (std::strcmp(a, "--verbose") == 0 || std::strcmp(a, "-v") == 0)
            o.verbose = true;
        else if (a[0] == '-')
            return false;
        else
            o.inputs.push_back(a);
    }
    return !o.inputs.empty();
}

// Tool-local path and file helpers (they predate the kiln IO layer, kiln/io.h).
Vec<u8> g_scratch{default_allocator(), Tag::Io};

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
    return iequals(ext, "glb") || iequals(ext, "gltf") || iequals(ext, "png") || iequals(ext, "ktx2");
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

/// Asset path: `path` relative to `root`, forward slashes, no extension.
void asset_path_of(char const* path, char const* root, char* out, usize cap) {
    usize rl        = std::strlen(root);
    char const* rel = path;
    if (rl && std::strncmp(path, root, rl) == 0 && (path[rl] == '/' || path[rl] == '\0'))
        rel = path + rl + (path[rl] == '/' ? 1 : 0);
    else {
        char const* slash = std::strrchr(path, '/');
        rel               = slash ? slash + 1 : path;
    }
    usize n     = std::strlen(rel);
    StrView ext = extension(StrView(rel, n));
    if (!ext.empty()) n -= ext.size + 1;
    if (n >= cap) n = cap - 1;
    std::memcpy(out, rel, n);
    out[n] = '\0';
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

bool emit(Ctx& c, StrView assetPath, char const* ext, u64 key, Span<u8 const> bytes) {
    if (c.opt.check) return true;
    char name[1200];
    if (c.opt.hashed) {
        store_file_name(key, StrView(ext), name, sizeof name);
    } else {
        // Named layout (default, kiln/assets.h StoreLayout::Named): <store>/<assetPath>.<ext>.
        format(name, sizeof name, "%.*s.%s", KILN_SV(assetPath), ext);
        char dir[1200];
        format(dir, sizeof dir, "%s/%.*s", c.opt.store, KILN_SV(assetPath));
        if (char* slash = std::strrchr(dir, '/')) {
            *slash = '\0';
            make_dirs(dir);
        }
    }
    Status st = store_write(StrView(c.opt.store), StrView(name), bytes, &c.sink);
    if (st.failed()) {
        std::fprintf(stderr, "kiln-cook: cannot write %s/%s (%s)\n", c.opt.store, name, code_name(st.code));
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

bool cook_one_texture(Ctx& c, Span<u8 const> bytes, StrView assetPath, StrView sourcePath, SlotHint hint) {
    u64 pathHash = hash_name(assetPath);
    if (c.doneTextures.contains(pathHash)) return true; // shared between meshes
    c.doneTextures.insert(pathHash, 1);

    Result<TextureCookSettings> rs =
        resolve_texture(c.opt.tex, hint, c.opt.target, c.session, &c.sink, assetPath);
    if (rs.failed()) return false;
    TextureSource src{};
    src.bytes               = bytes;
    src.assetPath           = assetPath;
    src.sourcePath          = sourcePath;
    Result<CookedTexture> r = cook_texture(src, *rs, c.opt.target, {.diag = &c.sink, .jobs = c.jobs});
    if (r.failed()) return false;
    u64 key = store_key(r->sourceHash, hash_settings(*rs), hash_target(c.opt.target));
    if (!emit(c, assetPath, "ktx2", key, r->file.span())) return false;
    if (c.opt.verbose) print_texture_stats(r->stats);
    return true;
}

bool cook_one_mesh(Ctx& c, Span<u8 const> bytes, StrView assetPath, char const* sourcePath) {
    Result<MeshCookSettings> rs = resolve_mesh(c.opt.mesh, c.opt.target, c.session, &c.sink, assetPath);
    if (rs.failed()) return false;

    char baseDir[1024];
    format(baseDir, sizeof baseDir, "%s", sourcePath);
    if (char* slash = std::strrchr(baseDir, '/'))
        *slash = '\0';
    else
        std::strcpy(baseDir, ".");

    MeshSource src{};
    src.bytes            = bytes;
    src.assetPath        = assetPath;
    src.sourcePath       = StrView(sourcePath);
    src.resolver         = {&resolve_uri_fn, baseDir};
    Result<CookedMesh> r = cook_mesh(src, *rs, c.opt.target, {.diag = &c.sink, .jobs = c.jobs});
    if (r.failed()) return false;
    u64 key = store_key(r->sourceHash, hash_settings(*rs), hash_target(c.opt.target));
    if (!emit(c, assetPath, "mesh", key, r->file.span())) return false;
    if (c.opt.verbose) print_mesh_stats(r->stats);

    // Referenced textures: embedded bytes or files next to the source.
    bool ok = true;
    for (TextureRef const& t : r->textures) {
        if (!t.embedded.empty()) {
            ok &= cook_one_texture(c, t.embedded, t.assetPath, src.sourcePath, t.slot);
        } else if (!t.uri.empty()) {
            Vec<u8> img(default_allocator(), Tag::Io);
            char path[1024];
            format(path, sizeof path, "%s/%.*s", baseDir, KILN_SV(t.uri));
            if (!read_file(path, img)) {
                std::fprintf(stderr, "kiln-cook: %.*s: cannot read texture %s\n", KILN_SV(assetPath), path);
                ok = false;
                continue;
            }
            ok &= cook_one_texture(c, img.span(), t.assetPath, StrView(path), t.slot);
        }
    }
    return ok;
}

void cook_file(Ctx& c, char const* path, char const* root) {
    char assetPath[1024];
    asset_path_of(path, root, assetPath, sizeof assetPath);
    StrView ext = extension(StrView(path));

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

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) return usage();

    DiagState ds{o.quiet, o.verbose};
    Ctx c{
        o, ds, DiagSink{&diag_fn,                                    &ds  },
          CookSession{o.check ? StoreMode::None : StoreMode::Disk, false}
    };
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
        if (o.root) {
            format(root, sizeof root, "%s", o.root);
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
