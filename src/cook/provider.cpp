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

#include "catalog_store.h"
#include "unit.h"

#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/cook/sidecar.h"
#include "kiln/io.h"
#include "kiln/log.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#include <direct.h>  // _mkdir
#include <windows.h> // FindFirstFileW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
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

/// A source file and its `.kiln` sidecar, which is part of the source: editing, adding or
/// removing it re-cooks. A missing sidecar reads as zeros.
struct SourceStat {
    IoStat file;
    IoStat sidecar;
};

/// One source file that produced store files. Paths are offsets into Provider::strings.
struct SourceRecord {
    u32 pathOff = 0, pathLen = 0;     ///< the source file, as find_source() built it
    u32 assetOff = 0, assetLen = 0;   ///< the asset to re-cook: the mesh for a glb/gltf (even
                                      ///< when a texture request created the record), the
                                      ///< texture for a png/ktx2 of its own
    AssetKind kind = AssetKind::Mesh; ///< the unit: a mesh source, or a texture of its own
    SourceStat stat;                  ///< the source as last cooked successfully
    SourceStat failedStat;            ///< the source version whose re-cook last failed
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
    Vec<char> storeDirBuf; ///< owned copy of store_dir(ctx), NUL-terminated
    StrView storeDir;
    Vec<char> rootsBuf;      ///< owned copies of roots(ctx), NUL-separated
    Vec<Root> roots;         ///< views into rootsBuf
    Vec<char> ruleStrings;   ///< owned copies of the name rule suffixes
    Vec<NameRule> nameRules; ///< suffixes point into ruleStrings
    Allocator const* alloc = nullptr;
    JobSystem const* jobs  = nullptr; ///< the context's pool; provider_cook runs on one of its workers
    Context* ctx           = nullptr; ///< the registry key

    // Catalog layout, Disk mode: the profile's catalog writer (it holds the lock).
    CatalogStore* store = nullptr;
    u64 hostDigest      = 0; ///< host_digest() of desc

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
        : storeDirBuf(a, Tag::Cook), rootsBuf(a, Tag::Cook), roots(a, Tag::Cook), ruleStrings(a, Tag::Cook),
          nameRules(a, Tag::Cook), alloc(a), records(a, Tag::Cook), emitted(a, Tag::Cook),
          strings(a, Tag::Cook) {}
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

/// Writes `bytes` to the store file of `name` (store_file_path), creating its directories.
/// `overwrite`: replace an existing file (re-cook) instead of leaving it (cook-on-miss).
Status write_to_store(StrView storeDir, StrView name, AssetKind kind, Span<u8 const> bytes, bool overwrite,
                      DiagSink const* diag) noexcept {
    char path[1024];
    usize const n = store_file_path(storeDir, kind, name, path, sizeof path);
    if (n >= sizeof path - 1) return make_status(Code::InvalidArgument);
    usize const slash = StrView(path, n).rfind('/');
    if (slash == StrView::kNpos) return store_write(StrView("."), StrView(path, n), bytes, diag, overwrite);
    path[slash] = '\0';
    make_dirs(path, slash);
    return store_write(StrView(path, slash), StrView(path + slash + 1, n - slash - 1), bytes, diag,
                       overwrite);
}

/// A source file found in one of the provider's roots.
struct FoundSource {
    char path[1024] = {};
    usize len       = 0;
};

[[nodiscard]] bool ext_is(StrView path, char const* ext) noexcept {
    usize const dot   = path.rfind('.');
    usize const slash = path.rfind('/');
    if (dot == StrView::kNpos || (slash != StrView::kNpos && slash > dot)) return false;
    StrView const e = path.substr(dot + 1);
    StrView const want(ext);
    if (e.size != want.size) return false;
    for (usize i = 0; i < e.size; ++i) {
        char c = e[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != want[i]) return false;
    }
    return true;
}

[[nodiscard]] bool is_model(StrView path) noexcept { return ext_is(path, "glb") || ext_is(path, "gltf"); }
[[nodiscard]] bool is_image(StrView path) noexcept {
    return ext_is(path, "png") || ext_is(path, "jpg") || ext_is(path, "jpeg") || ext_is(path, "hdr") ||
           (webp_decode_enabled() && ext_is(path, "webp")) || ext_is(path, "ktx2");
}

/// The source file of the (sub-asset free) name `owner`: `<root root>/<path>`. NotFound
/// without a diagnostic when no such file exists, so the runtime reports a store miss.
Status find_source(Provider const& p, StrView owner, FoundSource& out, DiagSink const* diag) noexcept {
    AssetNameParts const parts = split_asset_name(owner);
    Root const* root           = nullptr;
    for (Root const& m : p.roots)
        if (m.name == parts.root) root = &m;
    if (!root) {
        if (parts.root.empty())
            return diagf(diag, make_status(Code::InvalidArgument), kDiagUnknownRoot, Severity::Error, owner,
                         "request", "no default root: the name needs a 'root:' prefix");
        return diagf(diag, make_status(Code::InvalidArgument), kDiagUnknownRoot, Severity::Error, owner,
                     "request", "unknown root '%.*s'", KILN_SV(parts.root));
    }
    out.len = format(out.path, sizeof out.path, "%.*s/%.*s", KILN_SV(root->dir), KILN_SV(parts.path));
    if (out.len >= sizeof out.path - 1) return make_status(Code::InvalidArgument);
    if (!io_file_exists(StrView(out.path, out.len))) return make_status(Code::NotFound);
    if (!source_case_matches(root->dir, parts.path))
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSourceCase, Severity::Error, owner,
                     "request", "the file on disk differs in case: %.*s", int(out.len), out.path);
    return kOk;
}

void sidecar_path(StrView sourcePath, char (&buf)[1100], StrView& out) noexcept {
    out = StrView(buf, format(buf, sizeof buf, "%.*s%.*s", KILN_SV(sourcePath), KILN_SV(kSidecarExt)));
}

/// The unit of the source `sourcePath`, named `name` (a mesh, or a texture of its own).
UnitDesc unit_desc(Provider const& p, AssetKind kind, StrView name, StrView sourcePath,
                   Allocator const* alloc, DiagSink const* diag) noexcept {
    return UnitDesc{
        .kind            = kind,
        .name            = name,
        .sourcePath      = sourcePath,
        .meshDefaults    = &p.desc.meshDefaults,
        .textureDefaults = &p.desc.textureDefaults,
        .nameRules       = Span<NameRule const>(p.nameRules.data(), p.nameRules.size()),
        .policy          = p.desc.policy,
        .target          = &p.desc.target,
        .session         = p.session,
        .env             = {.alloc = alloc, .diag = diag, .jobs = p.jobs},
    };
}

/// Cooks a source and, in Disk mode, writes every output that cooked to the store. A texture that
/// cannot be written keeps the failure in its output; the first output failing fails the call.
/// `emitted` (optional) gets the names of the textures written, NUL-separated.
Status cook_to_store(Provider const& p, UnitDesc const& d, bool overwrite, CookUnit* unit,
                     Vec<char>* emitted) noexcept {
    KILN_TRY(cook_unit(d, unit));
    if (p.desc.storeMode != StoreMode::Disk) return kOk;
    for (usize i = 0; i < unit->outputs.size(); ++i) {
        UnitOutput& o = unit->outputs[i];
        if (o.status.failed()) continue;
        StrView const name = unit->name(o);
        Status const st    = write_to_store(p.storeDir, name, o.kind, o.bytes.span(), overwrite, d.env.diag);
        if (st.failed()) {
            if (i == 0) return st;
            o.status = st;
            continue;
        }
        if (emitted && o.kind == AssetKind::Texture) {
            emitted->append(Span<char const>(name.data, name.size));
            emitted->push_back('\0');
        }
    }
    return kOk;
}

/// Moves the bytes of the output `name` into `out`, or returns why there are none.
Status take_output(CookUnit& unit, AssetKind kind, StrView name, Vec<u8>* out) noexcept {
    UnitOutput* o = unit.find(kind, name);
    if (!o) return make_status(Code::NotFound);
    if (o->status.failed()) return o->status;
    *out = std::move(o->bytes);
    return kOk;
}

// ---------------------------------------------------------------------------
// Source records
// ---------------------------------------------------------------------------

/// Fails only if the source itself cannot be stat'ed.
Status stat_source(StrView path, SourceStat* out) noexcept {
    KILN_TRY(stat_file(path, &out->file));
    char buf[1100];
    StrView side;
    sidecar_path(path, buf, side);
    if (stat_file(side, &out->sidecar).failed()) out->sidecar = {};
    return kOk;
}

bool same_stat(IoStat const& a, IoStat const& b) noexcept {
    return a.size == b.size && a.mtimeNs == b.mtimeNs;
}
bool same_stat(SourceStat const& a, SourceStat const& b) noexcept {
    return same_stat(a.file, b.file) && same_stat(a.sidecar, b.sidecar);
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
void record_source(Provider& p, StrView sourcePath, SourceStat const& st, AssetKind kind, StrView assetPath,
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

/// Cooks the source of a missed request and returns the output `name` of `kind`.
Status cook_on_miss(Provider& p, AssetKind unitKind, StrView unitName, StrView sourcePath, AssetKind kind,
                    StrView name, Allocator const* alloc, Vec<u8>* out, DiagSink const* diag) noexcept {
    std::unique_lock<std::mutex> const lock = lock_source_if_watching(p, sourcePath);

    // Stat before the cook reads the source, so an edit during the cook is seen later.
    bool const disk = p.desc.storeMode == StoreMode::Disk;
    SourceStat st{};
    bool const haveStat = disk && stat_source(sourcePath, &st).ok();
    CookUnit unit(alloc);
    Vec<char> emitted(alloc, Tag::Cook);
    KILN_TRY(cook_to_store(p, unit_desc(p, unitKind, unitName, sourcePath, alloc, diag), false, &unit,
                           disk ? &emitted : nullptr));
    Status const s = take_output(unit, kind, name, out);
    if (s.ok() && haveStat)
        record_source(p, sourcePath, st, unitKind, unitName,
                      unitKind == AssetKind::Mesh ? &emitted : nullptr);
    return s;
}

/// A request, resolved to the source whose cook makes it (the unit).
struct UnitRequest {
    StrView owner;                        ///< the unit's name: `name` without `#<image>`
    AssetKind unitKind = AssetKind::Mesh; ///< Mesh: a glb/gltf source; Texture: an image of its own
    FoundSource src;
    [[nodiscard]] StrView source_path() const noexcept { return {src.path, src.len}; }
};

Status resolve_request(Provider const& p, AssetKind kind, StrView name, UnitRequest* out,
                       DiagSink const* diag) noexcept {
    AssetNameParts const parts = split_asset_name(name);
    bool const embedded        = !parts.sub.empty();
    bool const kindOk          = kind == AssetKind::Mesh ? !embedded && is_model(parts.path)
                                                         : (embedded ? is_model(parts.path) : is_image(parts.path));
    if (!kindOk)
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSourceKind, Severity::Error, name,
                     "request", "the extension does not name a %s source",
                     kind == AssetKind::Mesh ? "mesh" : "texture");
    // "<mesh>#<image>": an image embedded in that mesh's source.
    out->owner    = embedded ? name.substr(0, name.size - parts.sub.size - 1) : name;
    out->unitKind = kind == AssetKind::Texture && !embedded ? AssetKind::Texture : AssetKind::Mesh;
    return find_source(p, out->owner, out->src, diag);
}

Status provider_cook(void* user, AssetKind kind, StrView name, Allocator const* alloc, Vec<u8>* out,
                     DiagSink const* diag) noexcept {
    auto* p = static_cast<Provider*>(user);
    UnitRequest r;
    KILN_TRY(resolve_request(*p, kind, name, &r, diag));
    return cook_on_miss(*p, r.unitKind, r.owner, r.source_path(), kind, name, alloc, out, diag);
}

// ---------------------------------------------------------------------------
// Catalog layout: prepare
// ---------------------------------------------------------------------------

/// Cooks a unit and, with a store, publishes it and rewrites the catalog. `unit` keeps the outputs.
Status cook_and_publish(Provider& p, UnitDesc d, DiagSink const* diag, CookUnit* unit) noexcept {
    d.statInputs = true;
    KILN_TRY(cook_unit(d, unit));
    if (!p.store) return kOk;
    KILN_TRY(publish_unit(p.store, *unit, p.hostDigest, diag));
    // The entry is in memory; a failed rewrite (a reader blocking the rename) is retried later.
    if (Status const st = commit_catalog(p.store, diag); st.failed())
        KILN_WARN("cook", "cannot rewrite the catalog (%s); retrying later", code_name(st.code));
    return kOk;
}

/// True if the unit's record still describes it: the same source path, every input with its
/// recorded size and time, and the host's settings giving the recorded keys.
bool record_is_current(Provider& p, UnitDesc const& d) noexcept {
    CookUnit rec(d.env.alloc);
    u64 digest = 0;
    if (!copy_input_record(p.store, d.name, &rec, &digest) || rec.inputs.empty()) return false;
    UnitInput const& source = rec.inputs[0];
    if (source.role != InputRole::Source || rec.str(source.pathOff, source.pathLen) != d.sourcePath)
        return false;
    if (!recorded_inputs_unchanged(rec)) return false;
    if (digest == p.hostDigest) return true;
    if (!recorded_keys_match(d, rec)) return false;
    set_record_digest(p.store, d.name, p.hostDigest);
    return true;
}

Status provider_prepare(void* user, AssetKind kind, StrView name, Allocator const* alloc, Vec<u8>* out,
                        Hash128* key, DiagSink const* diag) noexcept {
    auto* p = static_cast<Provider*>(user);
    UnitRequest r;
    KILN_TRY(resolve_request(*p, kind, name, &r, diag));
    StrView const sourcePath = r.source_path();
    // One check or cook per source at a time: a mesh and its images often arrive together.
    std::lock_guard<std::mutex> const lock(source_lock(*p, sourcePath));
    UnitDesc const d = unit_desc(*p, r.unitKind, r.owner, sourcePath, alloc, diag);

    if (p->store && (is_fresh(p->store, r.owner) || record_is_current(*p, d))) {
        mark_fresh(p->store, r.owner);
        if (catalog_find(p->store, kind, name, key)) return kOk;
        // Fresh, but without this output (an image that failed): cook again and report why.
    }
    CookUnit unit(alloc);
    KILN_TRY(cook_and_publish(*p, d, diag, &unit));
    UnitOutput* o = unit.find(kind, name);
    if (!o) return make_status(Code::NotFound);
    if (o->status.failed()) return o->status;
    *key = p->store ? o->key : Hash128{};
    *out = std::move(o->bytes);
    return kOk;
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
                   SourceStat const& now) noexcept {
    StrView const sourcePath = pool_view(strings, rec.pathOff, rec.pathLen);
    StrView const assetPath  = pool_view(strings, rec.assetOff, rec.assetLen);

    std::lock_guard<std::mutex> const lock(source_lock(p, sourcePath));

    LogDiag logDiag;
    logDiag.quiet = rec.failed && same_stat(now, rec.failedStat);
    DiagSink const sink{&LogDiag::fn, &logDiag};

    CookUnit unit(p.alloc);
    Vec<char> emitted(p.alloc, Tag::Cook);
    Status s = cook_to_store(p, unit_desc(p, rec.kind, assetPath, sourcePath, p.alloc, &sink), true, &unit,
                             &emitted);
    if (s.ok()) s = unit.first_failure();

    std::lock_guard<std::mutex> const rlock(p.recordMutex);
    SourceRecord& r = p.records[idx];
    if (rec.kind == AssetKind::Mesh) set_emitted(p, idx, emitted, true);
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

/// A unit whose last re-cook failed, by name hash: `inputs` digests the input stats it failed on.
struct FailedUnit {
    u64 inputs = 0;
    bool retry = false; ///< an IO failure: retry every round
};

/// The size and time (or absence) of every recorded input, as they are now.
u64 inputs_digest(CookUnit const& rec) noexcept {
    Xxh64State h;
    for (UnitInput const& in : rec.inputs) {
        IoStat now{};
        h.update_value(u8(stat_file(rec.str(in.pathOff, in.pathLen), &now).ok()));
        h.update_value(now.size);
        h.update_value(now.mtimeNs);
    }
    return h.digest();
}

/// Catalog layout: re-cooks every fresh unit whose inputs changed, then rewrites the catalog once.
/// False when asked to stop.
bool poll_catalog_round(Provider* p, Vec<char>& units, HashMap<u64, FailedUnit>& failed) noexcept {
    fresh_units(p->store, &units);
    CookUnit rec(p->alloc);
    for (usize at = 0; at < units.size();) {
        if (p->stopping.load()) return false;
        StrView const name(units.data() + at, std::strlen(units.data() + at));
        at += name.size + 1;
        u64 digest = 0;
        if (!copy_input_record(p->store, name, &rec, &digest) || rec.outputs.empty() || rec.inputs.empty() ||
            recorded_inputs_unchanged(rec))
            continue;
        u64 const now          = inputs_digest(rec);
        FailedUnit const* last = failed.find(hash_name(name));
        if (last && !last->retry && last->inputs == now) continue;

        StrView const sourcePath = rec.str(rec.inputs[0].pathOff, rec.inputs[0].pathLen);
        std::lock_guard<std::mutex> const lock(source_lock(*p, sourcePath));
        LogDiag logDiag;
        logDiag.quiet = last && last->inputs == now;
        DiagSink const sink{&LogDiag::fn, &logDiag};
        CookUnit unit(p->alloc);
        UnitDesc d   = unit_desc(*p, rec.outputs[0].kind, name, sourcePath, p->alloc, &sink);
        d.statInputs = true;
        Status s     = cook_unit(d, &unit);
        if (s.ok()) s = publish_unit(p->store, unit, p->hostDigest, &sink);
        if (s.ok()) {
            failed.erase(hash_name(name));
            if (Status const images = unit.first_failure(); images.failed())
                KILN_ERROR("cook", "re-cooked %.*s, but an embedded image failed (%s)", KILN_SV(name),
                           code_name(images.code));
            else
                KILN_INFO("cook", "re-cooked %.*s", KILN_SV(name));
            continue;
        }
        // The record keeps its stats, so the unit still reads as changed. An IO failure (a
        // source still being written) retries every round; a cook failure waits for the next edit.
        bool const retry = s.code == Code::IoError || s.code == Code::IoEof || s.code == Code::NotFound;
        if (!logDiag.quiet)
            KILN_ERROR("cook", "re-cook of %.*s failed (%s); the catalog keeps the previous entry, %s",
                       KILN_SV(name), code_name(s.code), retry ? "retrying" : "waiting for the next change");
        failed.insert(hash_name(name), FailedUnit{now, retry});
    }
    // Once per round: this round's re-cooks and any records whose keys were checked again.
    if (Status const st = commit_catalog(p->store, nullptr); st.failed())
        KILN_WARN("cook", "cannot rewrite the catalog (%s); retrying", code_name(st.code));
    return true;
}

void poller_main(Provider* p) noexcept {
    Vec<SourceRecord> snap(p->alloc, Tag::Cook);
    Vec<char> snapStrings(p->alloc, Tag::Cook);
    HashMap<u64, FailedUnit> failed(p->alloc, Tag::Cook);
    auto const period = std::chrono::milliseconds(p->desc.pollMs > 0 ? p->desc.pollMs : 1u);

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(p->pollMutex);
            if (p->pollWake.wait_for(lock, period, [p] { return p->stopping.load(); })) return;
        }
        if (p->store) {
            if (!poll_catalog_round(p, snapStrings, failed)) return;
            continue;
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
            SourceStat now{};
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

bool source_case_matches(StrView root, StrView path) noexcept {
#if defined(KILN_OS_WINDOWS)
    // FindFirstFileW returns the name as stored on disk; compare it segment by segment.
    if (root.empty()) root = StrView(".");
    char buf[1024];
    usize const n = format(buf, sizeof buf, "%.*s/%.*s", KILN_SV(root), KILN_SV(path));
    if (n >= sizeof buf - 1) return false;
    usize seg = root.size + 1;
    for (usize i = seg; i <= n; ++i) {
        if (i < n && buf[i] != '/') continue;
        char const saved = buf[i];
        buf[i]           = '\0';
        wchar_t full[1024], leaf[512];
        int const leafLen = MultiByteToWideChar(CP_UTF8, 0, buf + seg, int(i - seg), leaf, 511);
        if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, full, 1024) == 0 || leafLen <= 0) return false;
        leaf[leafLen] = L'\0';
        WIN32_FIND_DATAW fd;
        HANDLE const h = FindFirstFileW(full, &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        FindClose(h);
        if (std::wcscmp(fd.cFileName, leaf) != 0) return false;
        buf[i] = saved;
        seg    = i + 1;
    }
    return true;
#else
    (void)root;
    (void)path;
    return true;
#endif
}

namespace {

/// Frees a provider: from uninstall_provider, or from destroy() when the host forgot it.
void provider_release(void* user) noexcept {
    auto* p = static_cast<Provider*>(user);
    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        if (Provider** found = registry().find(p->ctx); found && *found == p) registry().erase(p->ctx);
    }
    stop_poller(p); // joined before anything it reads is freed
    if (p->store) {
        if (Status const st = commit_catalog(p->store, nullptr); st.failed())
            KILN_WARN("cook", "cannot rewrite the catalog (%s)", code_name(st.code));
        close_catalog_store(p->store);
    }
    delete_object(p->alloc, p, Tag::Cook);
}

} // namespace

Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept {
    Span<Root const> const ctxRoots = roots(ctx);
    if (ctxRoots.empty()) return make_status(Code::InvalidArgument);

    ProviderDesc effective = desc;
    if (effective.storeMode == StoreMode::None) {
        KILN_WARN("cook", "ProviderDesc.storeMode == StoreMode::None is not meaningful for a cook "
                          "provider; treating it as StoreMode::Memory");
        effective.storeMode = StoreMode::Memory;
    }

    Allocator const* alloc = allocator(ctx);
    if (!alloc) alloc = default_allocator();

    Provider* p = new_object<Provider>(alloc, Tag::Cook, alloc);
    p->ctx      = ctx;
    p->desc     = effective;
    p->jobs     = jobs(ctx);
    p->session  = CookSession{effective.storeMode, effective.fastPreview};

    StrView const dir = store_dir(ctx);
    p->storeDirBuf.resize(dir.size + 1);
    std::memcpy(p->storeDirBuf.data(), dir.data, dir.size);
    p->storeDirBuf[dir.size] = '\0';
    p->storeDir              = StrView(p->storeDirBuf.data(), dir.size);

    // Roots lose a trailing separator: find_source() adds its own.
    usize total = 0;
    for (Root const& m : ctxRoots)
        total += m.name.size + 1 + m.dir.size + 1;
    p->rootsBuf.resize(total);
    p->roots.reserve(ctxRoots.size);
    usize offset    = 0;
    auto const copy = [p, &offset](StrView s) noexcept {
        if (s.size) std::memcpy(p->rootsBuf.data() + offset, s.data, s.size);
        p->rootsBuf[offset + s.size] = '\0';
        StrView const v(p->rootsBuf.data() + offset, s.size);
        offset += s.size + 1;
        return v;
    };
    for (Root const& m : ctxRoots) {
        StrView root = m.dir;
        while (root.size > 1 && (root[root.size - 1] == '/' || root[root.size - 1] == '\\'))
            --root.size;
        p->roots.push_back(Root{copy(m.name), copy(root)});
    }

    usize ruleBytes = 0;
    for (NameRule const& r : effective.nameRules)
        ruleBytes += r.suffix.size;
    p->ruleStrings.resize(ruleBytes);
    p->nameRules.reserve(effective.nameRules.size);
    usize ruleAt = 0;
    for (NameRule const& r : effective.nameRules) {
        if (r.suffix.size) std::memcpy(p->ruleStrings.data() + ruleAt, r.suffix.data, r.suffix.size);
        p->nameRules.push_back(
            NameRule{StrView(p->ruleStrings.data() + ruleAt, r.suffix.size), r.usage, r.shape});
        ruleAt += r.suffix.size;
    }
    p->desc.nameRules = {}; // the host's span may not outlive install_provider

    // Invalid host defaults fail here instead of on every cook.
    Result<MeshCookSettings> rm = resolve_mesh(effective.meshDefaults, effective.target, p->session);
    if (rm.failed()) {
        delete_object(alloc, p, Tag::Cook);
        return rm.status();
    }

    bool const catalog = store_layout(ctx) == StoreLayout::Catalog;
    if (catalog) {
        StrView const profile = store_profile(ctx);
        if (profile != effective.target.name) {
            delete_object(alloc, p, Tag::Cook);
            return diagf(diag_sink(ctx), make_status(Code::InvalidArgument), kDiagStoreProfileMismatch,
                         Severity::Error, profile, "install",
                         "the context reads the catalog of profile '%.*s'; the provider cooks for '%.*s'",
                         KILN_SV(profile), KILN_SV(effective.target.name));
        }
        p->hostDigest =
            host_digest(unit_desc(*p, AssetKind::Mesh, {}, {}, alloc, nullptr), effective.policyVersion);
        if (effective.storeMode == StoreMode::Disk) {
            Status const opened = open_catalog_store(
                {.storeDir = p->storeDir, .target = &p->desc.target, .alloc = alloc, .diag = diag_sink(ctx)},
                &p->store);
            if (opened.failed()) {
                delete_object(alloc, p, Tag::Cook);
                return opened;
            }
        }
    } else if (effective.storeMode == StoreMode::Disk) {
        // A store holds files of one profile (docs/design/target-profiles.md).
        Status const bound = bind_store_profile(p->storeDir, effective.target, diag_sink(ctx));
        if (bound.failed()) {
            delete_object(alloc, p, Tag::Cook);
            return bound;
        }
    }

    if (effective.storeMode == StoreMode::Disk && !catalog) {
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
    provider.user    = p;
    provider.release = &provider_release;
    if (catalog)
        provider.prepare = &provider_prepare;
    else
        provider.cook = &provider_cook;
    set_cook_provider(ctx, provider);
    return kOk;
}

void uninstall_provider(Context* ctx) noexcept {
    Provider* p = nullptr;
    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        if (Provider** found = registry().find(ctx)) p = *found;
    }
    set_cook_provider(ctx, CookProvider{});
    if (p) provider_release(p);
}

} // namespace kiln::cook
