// src/cook/provider.cpp — the cook provider: checks each asset against its recorded inputs, cooks
// what changed into the store, and polls the sources for hot reload
// (docs/design/store-manifest.md, docs/design/hot-reload.md).
//
// provider_prepare() runs on workers, concurrently. A published Provider's settings never change;
// the mutable state is the ManifestStore (its own mutexes) and the poller's stop flag. Only
// install/uninstall touch the registry.
//
// Locks. Per-source work (check, cook, publish) runs under one of kSourceLockStripes mutexes picked
// by the hash of the source path, so a mesh and its images requested together, or a request and
// the poller, never cook one source twice at once; different sources still cook in parallel unless
// their paths share a stripe. The ManifestStore mutexes are taken inside a stripe lock or on their
// own, never the other way.
#include "kiln/cook/provider.h"

#include "manifest_store.h"
#include "unit.h"

#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/cook/project.h"
#include "kiln/io.h"
#include "kiln/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#include <windows.h> // FindFirstFileW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#endif

namespace kiln::cook {

namespace {

constexpr usize kSourceLockStripes = 64;
/// A cook on a request rewrites the manifest at most this often.
constexpr u32 kCommitIntervalMs = 1000;

/// The project and the host digest it gives. Requests hold a reference while they cook, so the
/// poller can swap in an edited project (docs/design/project-config.md §6).
struct ProjectVersion {
    Allocator const* alloc = nullptr;
    Project* project       = nullptr; ///< null: no project file
    u64 hostDigest         = 0;       ///< host_digest() of the desc with this project
    std::atomic<u32> refs{1};         ///< the provider's own reference while it is current
};

void release_version(ProjectVersion* v) {
    if (!v || v->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    free_project(v->project);
    delete_object(v->alloc, v, Tag::Cook);
}

/// A unit the store has no record of for the poller to watch: in Disk mode a request whose cook
/// failed, which the poller cooks again; in Memory mode every unit cooked this session, whose assets
/// the poller reloads. Both when one of its files or the project changes.
struct UnrecordedUnit {
    Vec<char> strings; ///< the unit name, its source path, then failed_unit_paths()
    Vec<char> reloads; ///< Memory mode: the assets to reload, each '0' + kind, a name and a NUL
    AssetKind kind = AssetKind::Mesh;
    u64 stats      = 0;     ///< paths_stats() of the files when it cooked or failed
    bool retry     = false; ///< Disk mode, a transient failure: try every round
    bool resolved  = false; ///< Disk mode: the poller cooked it

    explicit UnrecordedUnit(Allocator const* a) : strings(a, Tag::Cook), reloads(a, Tag::Cook) {}
    StrView name() const { return StrView(strings.data(), std::strlen(strings.data())); }
    StrView source() const {
        char const* s = strings.data() + name().size + 1;
        return StrView(s, std::strlen(s));
    }
    Span<char const> paths() const {
        usize const at = name().size + source().size + 2;
        return Span<char const>(strings.data() + at, strings.size() - at);
    }
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
    Allocator const* alloc      = nullptr;
    JobSystem const* jobs       = nullptr; ///< the context's pool; provider_prepare runs on its workers
    ProfileHooks const* profile = nullptr; ///< the context's hooks
    Context* ctx                = nullptr; ///< the registry key

    // Disk mode: the store writer for the profile (it holds the store lock). Null in Memory mode.
    ManifestStore* store = nullptr;

    Vec<char> projectPath; ///< owned copy of desc.projectFile, NUL-terminated; empty: none
    std::mutex versionMutex;
    ProjectVersion* version = nullptr; ///< current; swapped by the poller under versionMutex

    std::mutex sourceLocks[kSourceLockStripes];

    std::thread poller;
    bool watching = false; ///< the poller runs; set before requests can arrive
    std::mutex unrecordedMutex;
    HashMap<u64, UnrecordedUnit> unrecorded; ///< by unit name hash; under unrecordedMutex
    std::mutex pollMutex;
    std::condition_variable pollWake;
    std::atomic<bool> stopping{false};

    explicit Provider(Allocator const* a)
        : storeDirBuf(a, Tag::Cook), rootsBuf(a, Tag::Cook), roots(a, Tag::Cook), ruleStrings(a, Tag::Cook),
          nameRules(a, Tag::Cook), alloc(a), projectPath(a, Tag::Cook), unrecorded(a, Tag::Cook) {}

    /// The current version with a reference the caller releases (release_version).
    ProjectVersion* acquire_version() {
        std::lock_guard<std::mutex> const lock(versionMutex);
        version->refs.fetch_add(1, std::memory_order_relaxed);
        return version;
    }
    /// RAII for acquire_version().
    struct VersionRef {
        ProjectVersion* v;
        explicit VersionRef(Provider& p) : v(p.acquire_version()) {}
        VersionRef(VersionRef const&)            = delete;
        VersionRef& operator=(VersionRef const&) = delete;
        ~VersionRef() { release_version(v); }
    };
};

// Context* -> Provider* registry, under registry_mutex().

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
HashMap<Context*, Provider*>& registry() {
    static HashMap<Context*, Provider*> reg(default_allocator(), Tag::Cook);
    return reg;
}

/// A source file found in one of the provider's roots.
struct FoundSource {
    char path[1024] = {};
    usize len       = 0;
};

[[nodiscard]] bool ext_is(StrView path, char const* ext) {
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

[[nodiscard]] bool is_model(StrView path) { return ext_is(path, "glb") || ext_is(path, "gltf"); }
[[nodiscard]] bool is_image(StrView path) {
    return ext_is(path, "png") || ext_is(path, "jpg") || ext_is(path, "jpeg") || ext_is(path, "hdr") ||
           (webp_decode_enabled() && ext_is(path, "webp")) || ext_is(path, "ktx2");
}

/// The source file of the (sub-asset free) name `owner`: `<root root>/<path>`. NotFound
/// without a diagnostic when no such file exists, so the runtime reports a store miss.
Status find_source(Provider const& p, StrView owner, FoundSource& out, DiagSink const* diag) {
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

/// The unit of the source `sourcePath`, named `name` (a mesh, or a texture of its own).
UnitDesc unit_desc(Provider const& p, ProjectVersion const& v, AssetKind kind, StrView name,
                   StrView sourcePath, Allocator const* alloc, DiagSink const* diag) {
    return UnitDesc{
        .kind            = kind,
        .name            = name,
        .sourcePath      = sourcePath,
        .meshDefaults    = &p.desc.meshDefaults,
        .textureDefaults = &p.desc.textureDefaults,
        .nameRules       = Span<NameRule const>(p.nameRules.data(), p.nameRules.size()),
        .project         = v.project,
        .policy          = p.desc.policy,
        .target          = &p.desc.target,
        .session         = p.session,
        .env             = {.alloc = alloc, .diag = diag, .jobs = p.jobs, .profile = p.profile},
    };
}

std::mutex& source_lock(Provider& p, StrView sourcePath) {
    return p.sourceLocks[fnv1a64(sourcePath) % kSourceLockStripes];
}

/// A request, resolved to the source whose cook makes it (the unit).
struct UnitRequest {
    StrView owner;                        ///< the unit's name: `name` without `#<image>`
    AssetKind unitKind = AssetKind::Mesh; ///< Mesh: a glb/gltf source; Texture: an image of its own
    FoundSource src;
    StrView source_path() const { return {src.path, src.len}; }
};

Status resolve_request(Provider const& p, AssetKind kind, StrView name, UnitRequest* out,
                       DiagSink const* diag) {
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

// ---------------------------------------------------------------------------
// Prepare
// ---------------------------------------------------------------------------

/// Cooks a unit and, with a store, publishes it and rewrites the manifest. `unit` keeps the outputs.
Status cook_and_publish(Provider& p, u64 hostDigest, UnitDesc d, DiagSink const* diag, CookUnit* unit) {
    d.statInputs = true;
    KILN_TRY(cook_unit(d, unit));
    if (!p.store) return kOk;
    KILN_TRY(publish_unit(p.store, *unit, hostDigest, diag));
    // The entry is in memory and `prepare` answers from it. Rewriting the manifest at most once a
    // second keeps a first session's many misses from rewriting it per cook; the poller and
    // release write the rest. A failed rewrite is retried later.
    if (Status const st = commit_manifest(p.store, diag, kCommitIntervalMs); st.failed())
        KILN_WARN("cook", "cannot rewrite the manifest (%s); retrying later", code_name(st.code));
    return kOk;
}

/// True if `list` (NUL-terminated entries) holds `entry`.
bool has_entry(Vec<char> const& list, StrView entry) {
    for (usize at = 0; at < list.size();) {
        StrView const e(list.data() + at, std::strlen(list.data() + at));
        if (e == entry) return true;
        at += e.size + 1;
    }
    return false;
}

void add_reload(Vec<char>& out, AssetKind kind, StrView name) {
    out.push_back(char('0' + u8(kind)));
    out.append(Span<char const>(name.data, name.size));
    out.push_back('\0');
}

/// Lets the poller watch a unit without a record: in Disk mode one whose cook failed on a request, in
/// Memory mode any. `asset` is the request's; its reload covers a failed cook with no outputs.
void note_unrecorded(Provider& p, AssetKind kind, StrView name, StrView sourcePath, CookUnit const& unit,
                     Status st, AssetKind assetKind, StrView asset) {
    if (!p.watching) return;
    UnrecordedUnit f(p.alloc);
    f.kind = kind;
    f.strings.append(Span<char const>(name.data, name.size));
    f.strings.push_back('\0');
    f.strings.append(Span<char const>(sourcePath.data, sourcePath.size));
    f.strings.push_back('\0');
    Vec<char> paths(p.alloc, Tag::Cook);
    failed_unit_paths(sourcePath, unit, &paths);
    f.strings.append(paths.span());
    f.stats = paths_stats(paths.span());
    f.retry = failure_is_transient(st);
    add_reload(f.reloads, assetKind, asset);
    for (UnitOutput const& o : unit.outputs)
        add_reload(f.reloads, o.kind, unit.str(o.nameOff, o.nameLen));
    std::lock_guard<std::mutex> const lock(p.unrecordedMutex);
    // Memory mode cooks one output per request: keep the reloads of the unit's earlier requests.
    if (UnrecordedUnit const* old = p.unrecorded.find(hash_name(name)))
        for (usize at = 0; at < old->reloads.size();) {
            StrView const entry(old->reloads.data() + at, std::strlen(old->reloads.data() + at));
            at += entry.size + 1;
            if (!has_entry(f.reloads, entry)) {
                f.reloads.append(Span<char const>(entry.data, entry.size));
                f.reloads.push_back('\0');
            }
        }
    p.unrecorded.insert(hash_name(name), std::move(f));
}

/// Disk mode: the unit has a record now.
void forget_failed(Provider& p, StrView name) {
    if (!p.watching || !p.store) return;
    std::lock_guard<std::mutex> const lock(p.unrecordedMutex);
    p.unrecorded.erase(hash_name(name));
}

/// The context reloads every output of `unit`, without waiting for its store poller.
void post_outputs(Provider const& p, CookUnit const& unit) {
    for (UnitOutput const& o : unit.outputs)
        post_reload(p.ctx, o.kind, unit.str(o.nameOff, o.nameLen));
}

Status provider_prepare(void* user, AssetKind kind, StrView name, PrepareMode mode, Allocator const* alloc,
                        Vec<u8>* out, Hash128* key, DiagSink const* diag) {
    auto* p = static_cast<Provider*>(user);
    UnitRequest r;
    KILN_TRY(resolve_request(*p, kind, name, &r, diag));
    StrView const sourcePath = r.source_path();
    // One check or cook per source at a time: a mesh and its images often arrive together.
    std::lock_guard<std::mutex> const lock(source_lock(*p, sourcePath));
    Provider::VersionRef const ref(*p);
    UnitDesc d = unit_desc(*p, *ref.v, r.unitKind, r.owner, sourcePath, alloc, diag);
    if (!p->store) d.only = name; // no record to fill: the other outputs would be thrown away

    // Recheck: the host saw a change the session's earlier check cannot know of.
    bool const fresh = p->store && mode == PrepareMode::Normal && is_fresh(p->store, r.owner);
    if (p->store && (fresh || record_is_current(p->store, d, ref.v->hostDigest, false))) {
        mark_fresh(p->store, r.owner, sourcePath);
        forget_failed(*p, r.owner);
        if (manifest_find(p->store, kind, name, key)) return kOk;
        // Fresh, but without this output (an image that failed): cook again and report why.
    }
    CookUnit unit(alloc);
    if (Status const st = cook_and_publish(*p, ref.v->hostDigest, d, diag, &unit); st.failed()) {
        note_unrecorded(*p, r.unitKind, r.owner, sourcePath, unit, st, kind, name);
        return st;
    }
    if (p->store)
        forget_failed(*p, r.owner);
    else
        note_unrecorded(*p, r.unitKind, r.owner, sourcePath, unit, kOk, kind, name);
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
    bool quiet    = false; ///< a retry of a source version already reported as failing
    StrView asset = {};    ///< the unit's name: cook diagnostics name the source file

    static void fn(void* user, Diagnostic const& d) {
        auto const* self = static_cast<LogDiag const*>(user);
        if (self->quiet || d.severity == Severity::Info) return;
        StrView const who = self->asset.empty() ? d.asset : self->asset;
        if (d.severity == Severity::Warning)
            KILN_WARN("cook", "K%04u %.*s: %.*s", d.code, KILN_SV(who), KILN_SV(d.message));
        else
            KILN_ERROR("cook", "K%04u %.*s: %.*s", d.code, KILN_SV(who), KILN_SV(d.message));
    }
};

/// A unit whose last re-cook failed, by name hash: `inputs` digests the input stats it failed on.
struct FailedUnit {
    u64 inputs = 0;
    bool retry = false; ///< an IO failure: retry every round
};

/// The size and time (or absence) of every recorded input next to `sourcePath`, as they are now.
u64 inputs_digest(CookUnit const& rec, StrView sourcePath) {
    Xxh64State h;
    for (UnitInput const& in : rec.inputs) {
        char path[1200];
        usize const n = input_path(sourcePath, in.role, rec.str(in.nameOff, in.nameLen), path, sizeof path);
        IoStat now{};
        h.update_value(u8(n < sizeof path - 1 && stat_file(StrView(path, n), &now).ok()));
        h.update_value(now.size);
        h.update_value(now.mtimeNs);
    }
    return h.digest();
}

/// The project file as the poller last saw it.
struct ProjectWatch {
    bool present  = false;
    IoStat stat   = {};
    bool reported = false; ///< its last load failed and was logged; wait for the next change
    bool init     = false;
};

/// Loads the project file again when its size or time changed, and makes it current. True when it
/// was swapped in: every unit is then checked against the new settings.
bool poll_project(Provider* p, ProjectWatch& w) {
    if (p->projectPath.empty()) return false;
    StrView const path(p->projectPath.data(), p->projectPath.size() - 1);
    IoStat now{};
    bool const present = stat_file(path, &now).ok();
    bool const changed =
        !w.init || present != w.present || now.size != w.stat.size || now.mtimeNs != w.stat.mtimeNs;
    bool const first = !w.init;
    w.init           = true;
    w.present        = present;
    w.stat           = now;
    if (!changed || first) return false;
    LogDiag logDiag;
    DiagSink const sink{&LogDiag::fn, &logDiag};
    Result<Project*> const loaded = load_project({.path = path}, p->alloc, &sink);
    if (loaded.failed()) {
        if (!w.reported)
            KILN_ERROR("cook", "%.*s has errors; the previous project stays in use", KILN_SV(path));
        w.reported = true;
        return false;
    }
    w.reported = false;
    auto* v    = new_object<ProjectVersion>(p->alloc, Tag::Cook);
    v->alloc   = p->alloc;
    v->project = *loaded;
    v->hostDigest =
        host_digest(unit_desc(*p, *v, AssetKind::Mesh, {}, {}, p->alloc, nullptr), p->desc.policyVersion);
    ProjectVersion* old = nullptr;
    {
        std::lock_guard<std::mutex> const lock(p->versionMutex);
        old        = p->version;
        p->version = v;
    }
    release_version(old);
    KILN_INFO("cook", "reloaded %.*s", KILN_SV(path));
    return true;
}

/// Cooks again the failed requests whose files changed (with `recheckAll`, every one). A unit that
/// cooks is published and from then on watched through its record.
void poll_failed(Provider* p, ProjectVersion const& v, bool recheckAll) {
    HashMap<u64, UnrecordedUnit> pending(p->alloc, Tag::Cook);
    {
        std::lock_guard<std::mutex> const lock(p->unrecordedMutex);
        if (p->unrecorded.size() == 0) return;
        pending = std::move(p->unrecorded);
    }
    for (auto& e : pending) {
        UnrecordedUnit& f = e.value;
        u64 const now     = paths_stats(f.paths());
        if (p->stopping.load() || (!f.retry && f.stats == now && !recheckAll)) continue;
        StrView const name = f.name(), sourcePath = f.source();
        std::lock_guard<std::mutex> const lock(source_lock(*p, sourcePath));
        LogDiag logDiag;
        logDiag.quiet = f.stats == now && !recheckAll; // reported when it failed
        logDiag.asset = name;
        DiagSink const sink{&LogDiag::fn, &logDiag};
        CookUnit unit(p->alloc);
        UnitDesc d   = unit_desc(*p, v, f.kind, name, sourcePath, p->alloc, &sink);
        d.statInputs = true;
        Status s     = cook_unit(d, &unit);
        if (s.ok()) s = publish_unit(p->store, unit, v.hostDigest, &sink);
        if (s.ok()) {
            mark_fresh(p->store, name, sourcePath);
            post_outputs(*p, unit);
            f.resolved = true;
            KILN_INFO("cook", "cooked %.*s", KILN_SV(name));
            continue;
        }
        if (!logDiag.quiet)
            KILN_ERROR("cook", "cook of %.*s failed again (%s)", KILN_SV(name), code_name(s.code));
        f.stats = now;
        f.retry = failure_is_transient(s);
    }
    // A request may have noted a newer failure meanwhile; it wins.
    std::lock_guard<std::mutex> const lock(p->unrecordedMutex);
    for (auto& e : pending)
        if (!e.value.resolved && !p->unrecorded.find(e.key)) p->unrecorded.insert(e.key, std::move(e.value));
}

/// Memory mode: reloads the assets of every unit whose files changed (with `recheckAll`, of every
/// unit). Their loads cook again. A unit that keeps failing reloads only when it changes again.
void poll_memory(Provider* p, bool recheckAll) {
    std::lock_guard<std::mutex> const lock(p->unrecordedMutex);
    for (auto& e : p->unrecorded) {
        UnrecordedUnit& u = e.value;
        u64 const now     = paths_stats(u.paths());
        if (now == u.stats && !recheckAll) continue;
        u.stats = now;
        for (usize at = 0; at < u.reloads.size();) {
            auto const kind = AssetKind(u.reloads[at] - '0');
            StrView const name(u.reloads.data() + at + 1, std::strlen(u.reloads.data() + at + 1));
            at += name.size + 2;
            post_reload(p->ctx, kind, name);
        }
        KILN_INFO("cook", "%.*s changed; reloading its assets", KILN_SV(u.name()));
    }
}

/// Re-cooks every fresh unit whose inputs changed (with `recheckAll`, also those whose settings
/// changed), then rewrites the manifest once. False when asked to stop.
bool poll_round(Provider* p, Vec<char>& units, HashMap<u64, FailedUnit>& failed, bool recheckAll) {
    fresh_units(p->store, &units);
    Provider::VersionRef const ref(*p);
    CookUnit rec(p->alloc);
    for (usize at = 0; at < units.size();) {
        if (p->stopping.load()) return false;
        StrView const name(units.data() + at, std::strlen(units.data() + at));
        at += name.size + 1;
        StrView const sourcePath(units.data() + at, std::strlen(units.data() + at));
        at += sourcePath.size + 1;
        u64 digest = 0;
        if (!copy_input_record(p->store, name, &rec, &digest) || rec.outputs.empty() || rec.inputs.empty())
            continue;
        InputsCheck const inputs = check_recorded_inputs(rec, sourcePath);
        if (inputs == InputsCheck::Touched) set_record_stats(p->store, name, rec);
        if (inputs != InputsCheck::Changed) {
            // A new project: the record check resolves the unit again and compares its keys.
            if (!recheckAll) continue;
            UnitDesc const check =
                unit_desc(*p, *ref.v, rec.outputs[0].kind, name, sourcePath, p->alloc, nullptr);
            if (record_is_current(p->store, check, ref.v->hostDigest, false)) continue;
        }
        u64 const now          = inputs_digest(rec, sourcePath);
        FailedUnit const* last = failed.find(hash_name(name));
        if (last && !last->retry && last->inputs == now && !recheckAll) continue;

        std::lock_guard<std::mutex> const lock(source_lock(*p, sourcePath));
        LogDiag logDiag;
        logDiag.quiet = last && last->inputs == now;
        logDiag.asset = name;
        DiagSink const sink{&LogDiag::fn, &logDiag};
        CookUnit unit(p->alloc);
        UnitDesc d   = unit_desc(*p, *ref.v, rec.outputs[0].kind, name, sourcePath, p->alloc, &sink);
        d.statInputs = true;
        Status s     = cook_unit(d, &unit);
        if (s.ok()) s = publish_unit(p->store, unit, ref.v->hostDigest, &sink);
        if (s.ok()) {
            failed.erase(hash_name(name));
            post_outputs(*p, unit);
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
            KILN_ERROR("cook", "re-cook of %.*s failed (%s); the manifest keeps the previous entry, %s",
                       KILN_SV(name), code_name(s.code), retry ? "retrying" : "waiting for the next change");
        failed.insert(hash_name(name), FailedUnit{now, retry});
    }
    poll_failed(p, *ref.v, recheckAll);
    // Once per round: this round's re-cooks and any records whose keys were checked again.
    if (Status const st = commit_manifest(p->store, nullptr); st.failed())
        KILN_WARN("cook", "cannot rewrite the manifest (%s); retrying", code_name(st.code));
    return true;
}

void poller_main(Provider* p) {
    Vec<char> units(p->alloc, Tag::Cook);
    HashMap<u64, FailedUnit> failed(p->alloc, Tag::Cook);
    auto const period = std::chrono::milliseconds(p->desc.pollMs > 0 ? p->desc.pollMs : 1u);
    ProjectWatch project;
    (void)poll_project(p, project); // the file as install_provider() loaded it

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(p->pollMutex);
            if (p->pollWake.wait_for(lock, period, [p] { return p->stopping.load(); })) return;
        }
        bool const recheck = poll_project(p, project);
        if (!p->store)
            poll_memory(p, recheck);
        else if (!poll_round(p, units, failed, recheck))
            return;
    }
}

/// std::thread's constructor may throw on resource exhaustion. Third-party throws are
/// caught at the call site (as in src/io/thread_pool.cpp).
bool start_poller(Provider* p) {
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

void stop_poller(Provider* p) {
    if (!p->poller.joinable()) return;
    {
        std::lock_guard<std::mutex> const lock(p->pollMutex);
        p->stopping.store(true);
    }
    p->pollWake.notify_all();
    p->poller.join();
}

} // namespace

bool source_case_matches(StrView root, StrView path) {
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
void provider_release(void* user) {
    auto* p = static_cast<Provider*>(user);
    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        if (Provider** found = registry().find(p->ctx); found && *found == p) registry().erase(p->ctx);
    }
    stop_poller(p); // joined before anything it reads is freed
    release_version(p->version);
    if (p->store) {
        if (Status const st = commit_manifest(p->store, nullptr); st.failed())
            KILN_WARN("cook", "cannot rewrite the manifest (%s)", code_name(st.code));
        close_manifest_store(p->store);
    }
    delete_object(p->alloc, p, Tag::Cook);
}

} // namespace

Status install_provider(Context* ctx, ProviderDesc const& desc) {
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
    p->profile  = profile_hooks(ctx);
    p->session  = CookSession{effective.storeMode, effective.fastPreview, effective.maxQuality};

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
    auto const copy = [p, &offset](StrView s) {
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

    StrView const profile = store_profile(ctx);
    if (profile != effective.target.name) {
        delete_object(alloc, p, Tag::Cook);
        return diagf(diag_sink(ctx), make_status(Code::InvalidArgument), kDiagStoreProfileMismatch,
                     Severity::Error, profile, "install",
                     "the context reads profile '%.*s'; the provider cooks for '%.*s'", KILN_SV(profile),
                     KILN_SV(effective.target.name));
    }
    // The project file: loaded here, so an error fails the install; the poller reloads it.
    Project* project = nullptr;
    if (!effective.projectFile.empty()) {
        p->projectPath.resize(effective.projectFile.size + 1);
        std::memcpy(p->projectPath.data(), effective.projectFile.data, effective.projectFile.size);
        p->projectPath[effective.projectFile.size] = '\0';
        Result<Project*> const loaded = load_project({.path = effective.projectFile}, alloc, diag_sink(ctx));
        if (loaded.failed()) {
            delete_object(alloc, p, Tag::Cook);
            return loaded.status();
        }
        project = *loaded;
    }
    p->desc.projectFile    = {}; // the host's string may not outlive install_provider
    p->version             = new_object<ProjectVersion>(alloc, Tag::Cook);
    p->version->alloc      = alloc;
    p->version->project    = project;
    p->version->hostDigest = host_digest(unit_desc(*p, *p->version, AssetKind::Mesh, {}, {}, alloc, nullptr),
                                         effective.policyVersion);
    if (effective.storeMode == StoreMode::Disk) {
        Status const opened = open_manifest_store(
            {.storeDir = p->storeDir, .target = &p->desc.target, .alloc = alloc, .diag = diag_sink(ctx)},
            &p->store);
        if (opened.failed()) {
            release_version(p->version);
            delete_object(alloc, p, Tag::Cook);
            return opened;
        }
        // A later kiln-cook run without inputs cooks from these roots.
        record_store_roots(p->store, Span<Root const>(p->roots.data(), p->roots.size()));
    }
    if (effective.watchSources && !(p->watching = start_poller(p)))
        KILN_WARN("cook", "could not start the source poller thread; sources are not watched");

    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        registry().insert(ctx, p);
    }

    CookProvider provider{};
    provider.prepare = &provider_prepare;
    provider.user    = p;
    provider.release = &provider_release;
    set_cook_provider(ctx, provider);
    return kOk;
}

void uninstall_provider(Context* ctx) {
    Provider* p = nullptr;
    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        if (Provider** found = registry().find(ctx)) p = *found;
    }
    set_cook_provider(ctx, CookProvider{});
    if (p) provider_release(p);
}

} // namespace kiln::cook
