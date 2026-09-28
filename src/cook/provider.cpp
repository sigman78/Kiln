// src/cook/provider.cpp — cook-on-miss provider and its source poller (hot reload, see
// docs/design/hot-reload.md). Threading: see docs/design/threading-and-io.md.
//
// provider_cook() runs on workers, concurrently. A published Provider's settings never
// change; the only mutable state is the source record table (under recordMutex) and the
// poller's stop flag. Only install/uninstall touch the registry.
//
// Locks. While the source poller runs, per-source work (stat and read the source, cook,
// write the store, record) runs under one of kSourceLockStripes mutexes picked by the hash
// of the source path, both for an on-miss cook and for a poller re-cook. So the poller
// never re-cooks a source while an on-miss cook of the same source runs, and on-miss cooks
// of different sources still run in parallel unless their paths share a stripe.
// recordMutex is taken inside a stripe lock or on its own, never the other way round.
#include "kiln/cook/provider.h"

#include "kiln/cook/cook.h"
#include "kiln/io.h"
#include "kiln/log.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#include <direct.h>  // _mkdir
#include <windows.h> // GetFileAttributesExW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#else
#include <sys/stat.h> // mkdir, stat
#endif

namespace kiln::cook {

namespace {

/// Sources the provider remembers at most. The tables are sized once at install; sources
/// past this are cooked but not watched (one warning).
constexpr usize kMaxSources = 4096;
/// Initial size of the record string pool. It may still grow: records hold offsets.
constexpr usize kRecordStringBytes = kMaxSources * 96;
constexpr usize kSourceLockStripes = 64;

/// One source file that produced store files. Paths are offsets into Provider::strings.
struct SourceRecord {
    u32 pathOff = 0, pathLen = 0;     ///< the source file, as find_source() built it
    u32 assetOff = 0, assetLen = 0;   ///< the asset to re-cook: the mesh for a glb/gltf (even
                                      ///< when a texture request created the record), the
                                      ///< texture for a png/ktx2 of its own
    AssetKind kind = AssetKind::Mesh; ///< Mesh: cook_mesh_full; Texture: cook_texture_own_source
    IoStat stat;                      ///< the source as last cooked successfully
    IoStat failedStat;                ///< the source version whose re-cook last failed
    bool failed      = false;
    bool retryFailed = false; ///< that failure was IO (source read, store write): retry every round
};

/// A texture asset path that a glb record wrote to the store.
struct EmittedTexture {
    u32 record = 0;
    u32 off = 0, len = 0; ///< into Provider::strings
};

/// Allocated once by install_provider(); its settings are immutable once published.
struct Provider {
    ProviderDesc desc;
    CookSession session;
    MeshCookSettings resolvedMesh; ///< resolve_mesh() is session-static; per-texture
                                   ///< settings depend on SlotHint and are resolved per cook.
    Vec<char> storeDirBuf;         ///< owned copy of store_dir(ctx), NUL-terminated
    StrView storeDir;
    Vec<char> rootsBuf; ///< owned copies of source_roots(ctx), NUL-separated
    Vec<StrView> roots; ///< views into rootsBuf
    Allocator const* alloc = nullptr;
    JobSystem const* jobs  = nullptr; ///< the context's pool; provider_cook runs on one of its workers

    // Source records (Disk mode), under recordMutex. They never shrink, so indices are stable.
    std::mutex recordMutex;
    Vec<SourceRecord> records;
    Vec<EmittedTexture> emitted;
    Vec<char> strings;
    bool overflowWarned = false;

    std::mutex sourceLocks[kSourceLockStripes];

    // Source poller. `watching` is set before the provider is published and never changes;
    // on-miss cooks read it to decide whether to take the stripe lock.
    bool watching = false;
    std::thread poller;
    std::mutex pollMutex;
    std::condition_variable pollWake;
    std::atomic<bool> stopping{false};

    explicit Provider(Allocator const* a) noexcept
        : storeDirBuf(a, Tag::Cook), rootsBuf(a, Tag::Cook), roots(a, Tag::Cook), alloc(a),
          records(a, Tag::Cook), emitted(a, Tag::Cook), strings(a, Tag::Cook) {}
};

// Context* -> Provider* registry, under registry_mutex().

std::mutex& registry_mutex() noexcept {
    static std::mutex m;
    return m;
}
HashMap<Context*, Provider*>& registry() noexcept {
    static HashMap<Context*, Provider*> reg(default_allocator(), Tag::Cook);
    return reg;
}

// Path helpers. kiln-cook has its own copy of make_dirs.

bool mkdir_one(char const* path) noexcept {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(path) == 0) return true;
#else
    if (::mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

/// mkdir -p over the forward-slash path in `buf[0..n)` (NUL-terminated at `n`).
void make_dirs(char* buf, usize n) noexcept {
    for (usize i = 1; i <= n; ++i) {
        if (i == n || buf[i] == '/') {
            char const saved = buf[i];
            buf[i]           = '\0';
            if (!(i == 2 && buf[1] == ':')) // skip a bare "C:" drive letter on Windows
                mkdir_one(buf);
            buf[i] = saved;
        }
    }
}

/// Directory portion of a store-relative asset path, e.g. "meshes/pbr_textures".
StrView dir_part(StrView assetPath) noexcept {
    usize const slash = assetPath.rfind('/');
    return slash == StrView::kNpos ? StrView{} : assetPath.substr(0, slash);
}
/// Leaf portion, e.g. "hull_albedo" out of "meshes/pbr_textures/hull_albedo".
StrView leaf_part(StrView assetPath) noexcept {
    usize const slash = assetPath.rfind('/');
    return slash == StrView::kNpos ? assetPath : assetPath.substr(slash + 1);
}
/// Directory containing a source file path, or "." if it names a bare file.
StrView source_dir(StrView sourcePath) noexcept {
    usize const slash = sourcePath.rfind('/');
    return slash == StrView::kNpos ? StrView(".") : sourcePath.substr(0, slash);
}

/// Writes `bytes` to `<storeDir>/<assetPath>.<ext>` (Named layout), creating
/// every intermediate directory kiln-cook's own default (named) mode would need.
/// `overwrite`: replace an existing file (re-cook) instead of leaving it (cook-on-miss).
Status write_to_store(StrView storeDir, StrView assetPath, char const* ext, Span<u8 const> bytes,
                      bool overwrite, DiagSink const* diag) noexcept {
    StrView const dp = dir_part(assetPath);
    StrView const lp = leaf_part(assetPath);

    char dirBuf[1024];
    usize const dn = dp.empty() ? format(dirBuf, sizeof dirBuf, "%.*s", KILN_SV(storeDir))
                                : format(dirBuf, sizeof dirBuf, "%.*s/%.*s", KILN_SV(storeDir), KILN_SV(dp));
    make_dirs(dirBuf, dn);

    char nameBuf[300];
    usize const nn = format(nameBuf, sizeof nameBuf, "%.*s.%s", KILN_SV(lp), ext);
    return store_write(StrView(dirBuf, dn), StrView(nameBuf, nn), bytes, diag, overwrite);
}

/// A source file found under one of the provider's roots.
struct FoundSource {
    char path[1024] = {};
    usize len       = 0;
};

/// Tries `<root>/<assetPath>.<ext1>` then `<root>/<assetPath>.<ext2>` for each
/// root in turn (root order outer, extension order inner).
bool find_source(Provider const& p, StrView assetPath, char const* ext1, char const* ext2,
                 FoundSource& out) noexcept {
    for (StrView const& root : p.roots) {
        usize n = format(out.path, sizeof out.path, "%.*s/%.*s.%s", KILN_SV(root), KILN_SV(assetPath), ext1);
        if (io_file_exists(StrView(out.path, n))) {
            out.len = n;
            return true;
        }
        n = format(out.path, sizeof out.path, "%.*s/%.*s.%s", KILN_SV(root), KILN_SV(assetPath), ext2);
        if (io_file_exists(StrView(out.path, n))) {
            out.len = n;
            return true;
        }
    }
    return false;
}

// Mesh URI resolver: external buffers/images relative to the source file.

struct UriResolverCtx {
    char baseDir[900] = {};
};

Status resolve_uri_fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) noexcept {
    auto const* ctx = static_cast<UriResolverCtx const*>(user);
    char path[1200];
    usize const n = format(path, sizeof path, "%s/%.*s", ctx->baseDir, KILN_SV(uri));
    StrView const pathView(path, n);
    if (!io_file_exists(pathView)) return make_status(Code::NotFound);
    return io_read_file(compat_io_backend(), pathView, alloc, out);
}

/// A texture with a source file of its own (no owning mesh involved).
Status cook_texture_own_source(Provider const& p, StrView sourcePath, StrView assetPath, bool overwrite,
                               Allocator const* alloc, Vec<u8>* out, DiagSink const* diag) noexcept {
    Vec<u8> bytes(alloc, Tag::Cook);
    KILN_TRY(io_read_file(compat_io_backend(), sourcePath, alloc, &bytes));

    Result<TextureCookSettings> rs =
        resolve_texture(p.desc.texture, SlotHint::None, p.desc.target, p.session, diag, assetPath);
    if (rs.failed()) return rs.status();

    TextureSource src{};
    src.bytes      = bytes.span();
    src.assetPath  = assetPath;
    src.sourcePath = sourcePath;
    Result<CookedTexture> r =
        cook_texture(src, *rs, p.desc.target, {.alloc = alloc, .diag = diag, .jobs = p.jobs});
    if (r.failed()) return r.status();

    if (p.desc.storeMode == StoreMode::Disk) {
        Status const st = write_to_store(p.storeDir, assetPath, "ktx2", r->file.span(), overwrite, diag);
        if (st.failed()) return st;
    }
    *out = std::move(r->file);
    return kOk;
}

/// What cook_mesh_full did with the mesh's textures.
struct MeshEmit {
    Vec<char> textures;        ///< asset paths written to the store, NUL-separated
    Status firstFailure = kOk; ///< first texture that could not be read, cooked or written
};

/// Cooks the mesh and every texture it references. `out` gets the mesh bytes (Mesh)
/// or the requested texture's bytes (Texture), taken from the in-process result,
/// never re-read from the store (Memory mode has none). `emit` is optional.
Status cook_mesh_full(Provider const& p, StrView meshAssetPath, StrView sourcePath, AssetKind requestedKind,
                      StrView requestedAssetPath, bool overwrite, Allocator const* alloc, Vec<u8>* out,
                      DiagSink const* diag, MeshEmit* emit) noexcept {
    Vec<u8> bytes(alloc, Tag::Cook);
    KILN_TRY(io_read_file(compat_io_backend(), sourcePath, alloc, &bytes));

    UriResolverCtx uctx;
    StrView const baseDir = source_dir(sourcePath);
    format(uctx.baseDir, sizeof uctx.baseDir, "%.*s", KILN_SV(baseDir));

    MeshSource src{};
    src.bytes      = bytes.span();
    src.assetPath  = meshAssetPath;
    src.sourcePath = sourcePath;
    src.resolver   = {&resolve_uri_fn, &uctx};

    Result<CookedMesh> r =
        cook_mesh(src, p.resolvedMesh, p.desc.target, {.alloc = alloc, .diag = diag, .jobs = p.jobs});
    if (r.failed()) return r.status();

    if (p.desc.storeMode == StoreMode::Disk) {
        Status const st = write_to_store(p.storeDir, meshAssetPath, "mesh", r->file.span(), overwrite, diag);
        if (st.failed()) return st;
    }

    bool requestedFound       = requestedKind == AssetKind::Mesh;
    Status requestedTexStatus = make_status(Code::NotFound);
    Vec<u8> requestedTexBytes(alloc, Tag::Cook);

    auto const noteFailure = [emit](Status const& st) noexcept {
        if (emit && emit->firstFailure.ok()) emit->firstFailure = st;
    };
    for (TextureRef const& t : r->textures) {
        bool const isRequested = requestedKind == AssetKind::Texture && t.assetPath == requestedAssetPath;

        Span<u8 const> texBytes;
        Vec<u8> uriBytes(alloc, Tag::Cook);
        if (!t.embedded.empty()) {
            texBytes = t.embedded;
        } else if (!t.uri.empty()) {
            char path[1200];
            usize const n = format(path, sizeof path, "%.*s/%.*s", KILN_SV(baseDir), KILN_SV(t.uri));
            StrView const pathView(path, n);
            Status const rr = io_read_file(compat_io_backend(), pathView, alloc, &uriBytes);
            if (rr.failed()) {
                diagf(diag, rr, 0, Severity::Error, t.assetPath, "texture", "cannot read external texture %s",
                      path);
                if (isRequested) requestedTexStatus = rr;
                noteFailure(rr);
                continue;
            }
            texBytes = uriBytes.span();
        } else {
            continue; // no usable source (the importer only emits refs with one)
        }

        Result<TextureCookSettings> rs =
            resolve_texture(p.desc.texture, t.slot, p.desc.target, p.session, diag, t.assetPath);
        if (rs.failed()) {
            if (isRequested) requestedTexStatus = rs.status();
            noteFailure(rs.status());
            continue;
        }
        TextureSource tsrc{};
        tsrc.bytes      = texBytes;
        tsrc.assetPath  = t.assetPath;
        tsrc.sourcePath = sourcePath;
        Result<CookedTexture> tr =
            cook_texture(tsrc, *rs, p.desc.target, {.alloc = alloc, .diag = diag, .jobs = p.jobs});
        if (tr.failed()) {
            if (isRequested) requestedTexStatus = tr.status();
            noteFailure(tr.status());
            continue;
        }

        if (p.desc.storeMode == StoreMode::Disk) {
            Status const st =
                write_to_store(p.storeDir, t.assetPath, "ktx2", tr->file.span(), overwrite, diag);
            if (st.failed()) {
                if (isRequested) requestedTexStatus = st;
                noteFailure(st);
                continue;
            }
            if (emit) {
                emit->textures.append(Span<char const>(t.assetPath.data, t.assetPath.size));
                emit->textures.push_back('\0');
            }
        }
        if (isRequested) {
            requestedTexBytes = std::move(tr->file);
            requestedFound    = true;
        }
    }

    if (requestedKind == AssetKind::Mesh) {
        *out = std::move(r->file);
        return kOk;
    }
    if (!requestedFound) return requestedTexStatus;
    *out = std::move(requestedTexBytes);
    return kOk;
}

// ---------------------------------------------------------------------------
// Source records
// ---------------------------------------------------------------------------

/// Size and modification time of a source. Uses the compat backend's `stat` when it has
/// one, else the same platform calls locally, so every stat in a process uses one clock.
Status stat_source(StrView path, IoStat* out) noexcept {
    IoBackend const* io = compat_io_backend();
    if (io && io->stat) return io->stat(io->user, path, out);

    char buf[1024];
    if (path.size + 1 > sizeof buf) return make_status(Code::InvalidArgument);
    std::memcpy(buf, path.data, path.size);
    buf[path.size] = '\0';
#if defined(KILN_OS_WINDOWS)
    wchar_t wbuf[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, int(sizeof wbuf / sizeof wbuf[0])) == 0)
        return make_status(Code::InvalidArgument);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(wbuf, GetFileExInfoStandard, &d)) {
        DWORD const err    = GetLastError();
        bool const missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        return make_status(missing ? Code::NotFound : Code::IoError, u16(err & 0xFFFFu));
    }
    out->size = (u64(d.nFileSizeHigh) << 32) | u64(d.nFileSizeLow);
    out->mtimeNs =
        ((u64(d.ftLastWriteTime.dwHighDateTime) << 32) | u64(d.ftLastWriteTime.dwLowDateTime)) * 100u;
#else
    struct stat st{};
    if (::stat(buf, &st) != 0) {
        int const e = errno;
        return make_status(e == ENOENT ? Code::NotFound : Code::IoError, u16(e & 0xFFFF));
    }
    out->size = u64(st.st_size);
#if defined(KILN_OS_MACOS)
    out->mtimeNs = u64(st.st_mtimespec.tv_sec) * 1000000000ull + u64(st.st_mtimespec.tv_nsec);
#else
    out->mtimeNs = u64(st.st_mtim.tv_sec) * 1000000000ull + u64(st.st_mtim.tv_nsec);
#endif
#endif
    return kOk;
}

bool same_stat(IoStat const& a, IoStat const& b) noexcept {
    return a.size == b.size && a.mtimeNs == b.mtimeNs;
}

std::mutex& source_lock(Provider& p, StrView sourcePath) noexcept {
    return p.sourceLocks[fnv1a64(sourcePath) % kSourceLockStripes];
}

StrView pool_view(Vec<char> const& pool, u32 off, u32 len) noexcept {
    return StrView(pool.data() + off, len);
}

/// Appends `s` to the string pool and returns its offset. Under recordMutex.
u32 pool_add(Provider& p, StrView s) noexcept {
    u32 const off = u32(p.strings.size());
    p.strings.append(Span<char const>(s.data, s.size));
    return off;
}

/// Adds the NUL-separated texture asset paths in `list` to record `idx`. With `replace`,
/// entries of that record not in `list` are dropped first (a re-cook: the glb may have lost
/// textures). Under recordMutex.
void set_emitted(Provider& p, u32 idx, Vec<char> const& list, bool replace) noexcept {
    auto const inList = [&list](StrView path) noexcept {
        for (usize at = 0; at < list.size();) {
            usize const n = std::strlen(list.data() + at);
            if (StrView(list.data() + at, n) == path) return true;
            at += n + 1;
        }
        return false;
    };
    if (replace) {
        for (usize i = p.emitted.size(); i-- > 0;) {
            EmittedTexture const& e = p.emitted[i];
            if (e.record == idx && !inList(pool_view(p.strings, e.off, e.len))) p.emitted.erase_unordered(i);
        }
    }
    for (usize at = 0; at < list.size();) {
        usize const n = std::strlen(list.data() + at);
        StrView const path(list.data() + at, n);
        at += n + 1;
        bool known = false;
        for (EmittedTexture const& e : p.emitted)
            if (e.record == idx && pool_view(p.strings, e.off, e.len) == path) known = true;
        if (!known) p.emitted.push_back(EmittedTexture{idx, pool_add(p, path), u32(n)});
    }
}

/// After a successful cook-on-miss in Disk mode: remember the source and its stat from
/// before the cook read it. A source already recorded (a texture of a glb whose mesh was
/// cooked before) joins its record; the stat stays, since this cook left existing store
/// files untouched.
void record_source(Provider& p, StrView sourcePath, IoStat const& st, AssetKind kind, StrView assetPath,
                   Vec<char> const* emittedTextures) noexcept {
    std::lock_guard<std::mutex> const lock(p.recordMutex);
    for (usize i = 0; i < p.records.size(); ++i) {
        SourceRecord const& r = p.records[i];
        if (pool_view(p.strings, r.pathOff, r.pathLen) != sourcePath) continue;
        if (emittedTextures) set_emitted(p, u32(i), *emittedTextures, false);
        return;
    }
    if (p.records.size() >= kMaxSources) {
        if (!p.overflowWarned) {
            KILN_WARN("cook", "more than %zu cooked sources: %.*s and later ones are not watched for changes",
                      kMaxSources, KILN_SV(sourcePath));
            p.overflowWarned = true;
        }
        return;
    }
    SourceRecord r;
    r.pathOff  = pool_add(p, sourcePath);
    r.pathLen  = u32(sourcePath.size);
    r.assetOff = pool_add(p, assetPath);
    r.assetLen = u32(assetPath.size);
    r.kind     = kind;
    r.stat     = st;
    p.records.push_back(r);
    if (emittedTextures) set_emitted(p, u32(p.records.size() - 1), *emittedTextures, false);
}

// ---------------------------------------------------------------------------
// Cook on miss
// ---------------------------------------------------------------------------

/// The stripe lock for `sourcePath` while the poller runs, else nothing.
std::unique_lock<std::mutex> lock_source_if_watching(Provider& p, StrView sourcePath) noexcept {
    if (!p.watching) return {};
    return std::unique_lock<std::mutex>(source_lock(p, sourcePath));
}

Status cook_mesh_on_miss(Provider& p, StrView meshAssetPath, StrView sourcePath, AssetKind requestedKind,
                         StrView requestedAssetPath, Allocator const* alloc, Vec<u8>* out,
                         DiagSink const* diag) noexcept {
    std::unique_lock<std::mutex> const lock = lock_source_if_watching(p, sourcePath);

    // Stat before the cook reads the source, so an edit during the cook is seen later.
    bool const disk = p.desc.storeMode == StoreMode::Disk;
    IoStat st{};
    bool const haveStat = disk && stat_source(sourcePath, &st).ok();
    MeshEmit emit;
    emit.textures.init(alloc, Tag::Cook);
    Status const s = cook_mesh_full(p, meshAssetPath, sourcePath, requestedKind, requestedAssetPath, false,
                                    alloc, out, diag, disk ? &emit : nullptr);
    if (s.ok() && haveStat) record_source(p, sourcePath, st, AssetKind::Mesh, meshAssetPath, &emit.textures);
    return s;
}

Status cook_texture_on_miss(Provider& p, StrView sourcePath, StrView assetPath, Allocator const* alloc,
                            Vec<u8>* out, DiagSink const* diag) noexcept {
    std::unique_lock<std::mutex> const lock = lock_source_if_watching(p, sourcePath);

    IoStat st{};
    bool const haveStat = p.desc.storeMode == StoreMode::Disk && stat_source(sourcePath, &st).ok();
    Status const s      = cook_texture_own_source(p, sourcePath, assetPath, false, alloc, out, diag);
    if (s.ok() && haveStat) record_source(p, sourcePath, st, AssetKind::Texture, assetPath, nullptr);
    return s;
}

Status provider_cook(void* user, AssetKind kind, StrView assetPath, Allocator const* alloc, Vec<u8>* out,
                     DiagSink const* diag) noexcept {
    auto* p = static_cast<Provider*>(user);

    if (kind == AssetKind::Texture) {
        FoundSource tex;
        if (find_source(*p, assetPath, "png", "ktx2", tex))
            return cook_texture_on_miss(*p, StrView(tex.path, tex.len), assetPath, alloc, out, diag);

        // No source of its own: it must be embedded in its owning mesh
        // ("<mesh assetPath>/<image stem>", cook/cook.h's TextureRef::assetPath).
        usize const slash = assetPath.rfind('/');
        if (slash == StrView::kNpos) return make_status(Code::NotFound);
        StrView const meshAssetPath = assetPath.substr(0, slash);

        FoundSource mesh;
        if (!find_source(*p, meshAssetPath, "glb", "gltf", mesh)) return make_status(Code::NotFound);
        return cook_mesh_on_miss(*p, meshAssetPath, StrView(mesh.path, mesh.len), AssetKind::Texture,
                                 assetPath, alloc, out, diag);
    }

    FoundSource mesh;
    if (!find_source(*p, assetPath, "glb", "gltf", mesh)) return make_status(Code::NotFound);
    return cook_mesh_on_miss(*p, assetPath, StrView(mesh.path, mesh.len), AssetKind::Mesh, assetPath, alloc,
                             out, diag);
}

// ---------------------------------------------------------------------------
// Source poller
// ---------------------------------------------------------------------------

/// Re-cook diagnostics go to the log: there is no pump thread to replay them on. Info is dropped.
struct LogDiag {
    bool quiet = false; ///< a retry of a source version already reported as failing

    static void fn(void* user, Diagnostic const& d) noexcept {
        auto const* self = static_cast<LogDiag const*>(user);
        if (self->quiet || d.severity == Severity::Info) return;
        if (d.severity == Severity::Warning)
            KILN_WARN("cook", "K%04u %.*s: %.*s", d.code, KILN_SV(d.asset), KILN_SV(d.message));
        else
            KILN_ERROR("cook", "K%04u %.*s: %.*s", d.code, KILN_SV(d.asset), KILN_SV(d.message));
    }
};

/// Re-cooks one changed source into the store, overwriting, and updates record `idx`.
/// `rec` and `strings` are the poller's snapshot of the record table.
void recook_source(Provider& p, u32 idx, SourceRecord const& rec, Vec<char> const& strings,
                   IoStat const& now) noexcept {
    StrView const sourcePath = pool_view(strings, rec.pathOff, rec.pathLen);
    StrView const assetPath  = pool_view(strings, rec.assetOff, rec.assetLen);

    std::lock_guard<std::mutex> const lock(source_lock(p, sourcePath));

    LogDiag logDiag;
    logDiag.quiet = rec.failed && same_stat(now, rec.failedStat);
    DiagSink const sink{&LogDiag::fn, &logDiag};

    Vec<u8> out(p.alloc, Tag::Cook);
    MeshEmit emit;
    emit.textures.init(p.alloc, Tag::Cook);
    Status s = kOk;
    if (rec.kind == AssetKind::Mesh) {
        s = cook_mesh_full(p, assetPath, sourcePath, AssetKind::Mesh, assetPath, true, p.alloc, &out, &sink,
                           &emit);
        if (s.ok() && emit.firstFailure.failed()) s = emit.firstFailure;
    } else {
        s = cook_texture_own_source(p, sourcePath, assetPath, true, p.alloc, &out, &sink);
    }

    std::lock_guard<std::mutex> const rlock(p.recordMutex);
    SourceRecord& r = p.records[idx];
    if (rec.kind == AssetKind::Mesh) set_emitted(p, idx, emit.textures, true);
    if (s.ok()) {
        r.stat   = now;
        r.failed = false;
        KILN_INFO("cook", "re-cooked %.*s from %.*s", KILN_SV(assetPath), KILN_SV(sourcePath));
        return;
    }
    // The record's stat stays, so the source still reads as changed. An IO failure (a
    // blocked rename, a source still being written) retries every round; a cook failure
    // waits for the next edit instead of re-cooking a broken file four times a second.
    bool const retry = s.code == Code::IoError || s.code == Code::IoEof || s.code == Code::NotFound;
    if (!logDiag.quiet) {
        KILN_ERROR("cook", "re-cook of %.*s from %.*s failed (%s); the store keeps the previous files, %s",
                   KILN_SV(assetPath), KILN_SV(sourcePath), code_name(s.code),
                   retry ? "retrying" : "waiting for the next change");
    }
    r.failed      = true;
    r.failedStat  = now;
    r.retryFailed = retry;
}

void poller_main(Provider* p) noexcept {
    Vec<SourceRecord> snap(p->alloc, Tag::Cook);
    Vec<char> snapStrings(p->alloc, Tag::Cook);
    auto const period = std::chrono::milliseconds(p->desc.pollMs > 0 ? p->desc.pollMs : 1u);

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(p->pollMutex);
            if (p->pollWake.wait_for(lock, period, [p] { return p->stopping.load(); })) return;
        }
        {
            std::lock_guard<std::mutex> const lock(p->recordMutex);
            snap.clear();
            snap.append(p->records.span());
            snapStrings.clear();
            snapStrings.append(p->strings.span());
        }
        for (usize i = 0; i < snap.size(); ++i) {
            if (p->stopping.load()) return;
            SourceRecord const& rec = snap[i];
            IoStat now{};
            // Missing (an editor mid-save) or unreadable: look again next round.
            if (stat_source(pool_view(snapStrings, rec.pathOff, rec.pathLen), &now).failed()) continue;
            if (same_stat(now, rec.stat)) continue;
            if (rec.failed && !rec.retryFailed && same_stat(now, rec.failedStat)) continue;
            recook_source(*p, u32(i), rec, snapStrings, now);
        }
    }
}

/// std::thread's constructor may throw on resource exhaustion. Third-party throws are
/// caught at the call site (as in src/io/thread_pool.cpp).
bool start_poller(Provider* p) noexcept {
#if KILN_HAS_EXCEPTIONS
    try {
        p->poller = std::thread(&poller_main, p);
    } catch (...) {
        return false;
    }
#else
    p->poller = std::thread(&poller_main, p);
#endif
    return true;
}

void stop_poller(Provider* p) noexcept {
    if (!p->poller.joinable()) return;
    {
        std::lock_guard<std::mutex> const lock(p->pollMutex);
        p->stopping.store(true);
    }
    p->pollWake.notify_all();
    p->poller.join();
}

} // namespace

Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept {
    Span<StrView const> const srcRoots = source_roots(ctx);
    if (srcRoots.empty()) return make_status(Code::InvalidArgument);

    ProviderDesc effective = desc;
    if (effective.storeMode == StoreMode::None) {
        KILN_WARN("cook", "ProviderDesc.storeMode == StoreMode::None is not meaningful for a cook "
                          "provider; treating it as StoreMode::Memory");
        effective.storeMode = StoreMode::Memory;
    }

    Allocator const* alloc = allocator(ctx);
    if (!alloc) alloc = default_allocator();

    Provider* p = new_object<Provider>(alloc, Tag::Cook, alloc);
    p->desc     = effective;
    p->jobs     = jobs(ctx);
    p->session  = CookSession{effective.storeMode, effective.fastPreview};

    StrView const dir = store_dir(ctx);
    p->storeDirBuf.resize(dir.size + 1);
    std::memcpy(p->storeDirBuf.data(), dir.data, dir.size);
    p->storeDirBuf[dir.size] = '\0';
    p->storeDir              = StrView(p->storeDirBuf.data(), dir.size);

    usize total = 0;
    for (StrView const& r : srcRoots)
        total += r.size + 1;
    p->rootsBuf.resize(total);
    p->roots.reserve(srcRoots.size);
    usize offset = 0;
    for (StrView const& r : srcRoots) {
        std::memcpy(p->rootsBuf.data() + offset, r.data, r.size);
        p->rootsBuf[offset + r.size] = '\0';
        p->roots.push_back(StrView(p->rootsBuf.data() + offset, r.size));
        offset += r.size + 1;
    }

    Result<MeshCookSettings> rm = resolve_mesh(effective.mesh, effective.target, p->session);
    if (rm.failed()) {
        delete_object(alloc, p, Tag::Cook);
        return rm.status();
    }
    p->resolvedMesh = rm.value();

    if (effective.storeMode == StoreMode::Disk) {
        p->records.reserve(kMaxSources);
        p->emitted.reserve(kMaxSources);
        p->strings.reserve(kRecordStringBytes);
    }
    if (effective.watchSources) {
        if (effective.storeMode != StoreMode::Disk) {
            KILN_WARN("cook", "ProviderDesc.watchSources needs StoreMode::Disk; sources are not watched");
        } else {
            // Set before the thread starts and before the provider is published.
            p->watching = true;
            if (!start_poller(p)) {
                p->watching = false;
                KILN_WARN("cook", "could not start the source poller thread; sources are not watched");
            }
        }
    }

    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        registry().insert(ctx, p);
    }

    CookProvider provider{};
    provider.cook = &provider_cook;
    provider.user = p;
    set_cook_provider(ctx, provider);
    return kOk;
}

void uninstall_provider(Context* ctx) noexcept {
    Provider* p = nullptr;
    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        if (Provider** found = registry().find(ctx)) {
            p = *found;
            registry().erase(ctx);
        }
    }
    if (p) stop_poller(p); // joined before anything it reads is freed
    set_cook_provider(ctx, CookProvider{});
    if (p) delete_object(p->alloc, p, Tag::Cook);
}

} // namespace kiln::cook
