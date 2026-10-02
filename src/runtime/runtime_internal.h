// runtime_internal.h — private runtime state; included only by src/runtime/*.cpp.
// Registry state is pump-thread only; an in-flight job touches only its slot's job fields.
// See docs/design/threading-and-io.md.
#pragma once

#include "kiln/assets.h"
#include "kiln/manifest.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace kiln {

struct Context;

namespace rt {

inline constexpr usize kMaxPathLen = kMaxAssetNameLen + 1; ///< asset name incl. terminator
/// Context::ph holds, per shape, one placeholder per kind and then the Failed one.
inline constexpr u32 kPlaceholdersPerShape = u32(TextureKind::Count) + 1;
inline constexpr u32 kPlaceholderCount     = kPlaceholdersPerShape * u32(TextureShape::Count);
constexpr u32 placeholder_index(TextureKind kind, TextureShape shape) {
    return u32(shape) * kPlaceholdersPerShape + u32(kind);
}
constexpr u32 failed_placeholder_index(TextureShape shape) {
    return u32(shape) * kPlaceholdersPerShape + u32(TextureKind::Count);
}

/// Heap bytes with 16-byte alignment (MeshView needs >= 8). Owned explicitly: the
/// owner calls release(); never copied implicitly.
struct Buffer {
    u8* data               = nullptr;
    usize size             = 0;
    Allocator const* alloc = nullptr;
    Tag tag                = Tag::Payload;

    void allocate(Allocator const* a, usize n, Tag t);
    void release();
    Span<u8 const> span() const { return {data, size}; }
};

enum class Phase : u8 {
    Free = 0,
    MetaQueued,   ///< waiting for the dispatcher (meta stage)
    MetaJob,      ///< meta stage running on a worker
    UploadQueued, ///< MetaReady; waiting for upload budget / Busy retry
    UploadJob,    ///< upload stage running on a worker
    Awaiting,     ///< committed; polling upload_status
    Done,         ///< Ready or Failed
};

enum class Stage : u8 { Meta = 0, Upload };
/// File: a store entry; Memory: register_*() bytes; Array: request_texture_array() layers.
enum class SourceKind : u8 { File = 0, Memory, Array };

enum class QueueId : u8 { None = 0, MetaHigh, MetaNormal, UploadHigh, UploadNormal, Await, Count };

enum class CompletionKind : u8 { MetaReady = 0, Uploaded, Failed, BusyRetry };

/// A reload post_reload() asked for.
struct PostedReload {
    AssetId id     = 0;
    AssetKind kind = AssetKind::Texture;
};

struct Completion {
    u32 slot            = 0;
    u32 generation      = 0; ///< Slot::jobGen at submit (assertion only)
    CompletionKind kind = CompletionKind::Failed;
};

/// First diagnostic a worker-side reader / provider emitted, replayed from pump().
struct DiagCapture {
    bool set          = false;
    u32 code          = 0;
    Severity severity = Severity::Error;
    char msg[192]     = {};
    void reset() {
        set    = false;
        code   = 0;
        msg[0] = '\0';
    }
};

inline constexpr usize kLayoutColumns = 5;

/// Outputs of the meta stage: validated metadata and the upload plan.
struct MetaSet {
    Buffer meta;               ///< mesh CPU region [0, gpuDataOffset)
    mesh::MeshView meshView;   ///< points into `meta`
    ktx2::TextureDesc texDesc; ///< textures
    /// Textures: kLayoutColumns arrays of layoutLevels values:
    /// [dstOffset | rowPitch | srcOffset | srcLength | texelLength]. srcLength is the stored
    /// size, which is smaller than texelLength when the levels are Zstd frames (texZstd).
    u64* layout      = nullptr;
    u32 layoutLevels = 0;
    bool texZstd     = false;
    u64 uploadSize   = 0; ///< bytes handed to begin_upload
};

/// One layer of a texture array. `key` belongs to the pump thread; the job fields are written at
/// dispatch (jobKey) and by the meta stage, and read by the upload stage.
struct ArrayLayer {
    u32 nameOff = 0;
    u32 nameLen = 0;
    Hash128 key;           ///< the artifact the settled load used
    bool keyValid = false; ///< false: none (a miss, provider bytes, not loaded yet)
    Hash128 failedKey;     ///< the artifact a failed reload of a Ready array tried
    bool failedKeyValid = false;
    Hash128 jobKey;
    bool jobKeyValid      = false;
    bool jobProviderOwned = false;
    bool providerOwned    = false; ///< as Slot::providerOwned
    Vec<u8> cooked;                ///< cook provider output
    bool cookedValid = false;
    /// Per level (srcLevels of them): [srcOffset | srcLength] in the layer's file.
    u64* src      = nullptr;
    u32 srcLevels = 0;
    bool zstd     = false;
};

/// A request_texture_array() declaration, owned by its slot.
struct ArrayDecl {
    char* names        = nullptr; ///< the layer names, one after another
    usize namesLen     = 0;
    ArrayLayer* layers = nullptr;
    u32 count          = 0;
    StrView name(ArrayLayer const& l) const { return {names + l.nameOff, l.nameLen}; }
};

struct Watch; // watch.cpp: store poller state

struct Slot {
    // --- identity / registry (pump thread) --------------------------------------
    Context* ctx          = nullptr;
    u32 index             = 0;
    u32 generation        = 1;
    AssetId id            = 0;
    AssetKind kind        = AssetKind::Mesh;
    State state           = State::Unloaded;
    Phase phase           = Phase::Free;
    Priority priority     = Priority::Normal;
    TextureKind texKind   = TextureKind::BaseColor;
    TextureShape texShape = TextureShape::Tex2D; ///< requested; fixed while the slot lives
    bool live             = false;               ///< occupied (including zombies)
    bool zombie           = false;               ///< released while a job was in flight
    bool jobInFlight      = false;
    bool reloading        = false; ///< the running load is a reload: state stays Ready / Failed
    bool reloadPending    = false; ///< reload requested while not settled; runs at settle
    u32 refcount          = 0;
    u32 version           = 0;
    u32 groupIndex        = kInvalid;
    u32 groupGen          = 0;
    u64 groupBytes        = 0;              ///< contribution to the group's bytesTotal
    State groupAs         = State::Pending; ///< how the group counts the slot (reloads never change it)
    // queue links (intrusive, over slot indices)
    QueueId queue  = QueueId::None;
    u32 qPrev      = kInvalid;
    u32 qNext      = kInvalid;
    u64 retryAfter = 0; ///< Busy retry: not before this pump index
    // Profiling only (profile_now_ns): the load attempt began, the slot entered its queue, its job
    // was submitted.
    u64 loadNs   = 0;
    u64 queuedNs = 0;
    u64 submitNs = 0;
    GpuObject realObj;           ///< the payload object (Ready)
    Status preFail   = kOk;      ///< a request-time failure (caps, bindless slots), reported on the next pump
    u32 bindSlot     = kInvalid; ///< bindless slot number (textures with Adapter::bind)
    bool bindPending = false;    ///< bindSlot waits for its placeholder upload to complete

    // --- job input (written by the pump thread before submit) -----------------
    Stage jobStage    = Stage::Meta;
    u32 jobGen        = 0;
    SourceKind source = SourceKind::File;
    Buffer memory;              ///< register_*: the copied cooked bytes
    ArrayDecl* array = nullptr; ///< SourceKind::Array: the layers
    CookProvider provider;      ///< snapshot at dispatch
    char path[kMaxPathLen] = {};
    u32 pathLen            = 0;
    Hash128 jobKey;                  ///< the artifact to load, from the manifest at dispatch or the provider
    bool jobKeyValid        = false; ///< false: the name missed the manifest
    bool jobManifestPresent = false; ///< Context::manifestPresent at dispatch
    bool jobRecheck         = false; ///< the provider checks the sources again (PrepareMode::Recheck)
    bool jobProviderOwned   = false; ///< the provider cooked without an artifact, or its cook failed
    bool recheck            = false; ///< pump thread: request_reload() asked for it; the next load takes it
    u64 postedBatch         = 0;     ///< the pump whose post_reload() batch last reloaded it

    // --- keys (pump thread) ---------------------------------------------------------
    bool manifestCheck = false; ///< a new manifest came during the load: compare keys when it settles
    Hash128 key;                ///< the build key `cur` came from, or that a failed load tried
    bool keyValid = false;      ///< false: no artifact (provider bytes, a miss) or not loaded
    Hash128 failedKey;          ///< the build key a failed reload of a Ready slot tried
    bool failedKeyValid = false;
    /// The last load came from the provider without an artifact (memory mode, or a failed cook): the
    /// provider reports its changes (post_reload), so manifest changes leave it alone.
    bool providerOwned = false;

    // --- metadata (docs/design/hot-reload.md) ----------------------------------------
    // Queries answer from `cur` once Ready. The meta stage (worker) writes only `next`;
    // the upload stage reads `next`; make_ready() swaps `next` into `cur` on the pump
    // thread. While MetaReady on a first load, queries show `next`. The pump thread
    // frees `next` only when no job is in flight.
    MetaSet cur;
    MetaSet next;
    Vec<u8> cooked; ///< cook provider output (memory source for both stages)
    bool cookedValid = false;

    // --- job output (worker) ------------------------------------------------------
    UploadTarget target;
    bool hasTarget   = false; ///< begin_upload succeeded (object must be released on failure)
    Status jobStatus = kOk;
    u32 jobDiag      = 0; ///< K5xxx for a Failed completion
    DiagCapture capture;
};

struct GroupRec {
    u32 generation = 1;
    bool live      = false;
    u32 ready = 0, failed = 0, pending = 0;
    u64 bytesDone = 0, bytesTotal = 0;
};

struct List {
    u32 head  = kInvalid;
    u32 tail  = kInvalid;
    u32 count = 0;
};

struct Placeholder {
    GpuObject obj;
    u64 token    = 0;
    bool pending = false; ///< committed, not yet complete (non-self-submitting adapter)
    bool ready   = false; ///< gpu_object() may return obj
    bool failed  = false; ///< the adapter failed its upload (K5009)
    AssetId id   = 0;
    ktx2::TextureDesc desc;
    u64 offset = 0; ///< level 0 offset (always 0)
    u64 pitch  = 0; ///< level 0 row pitch
};

/// An object and/or bindless slot number kiln dropped during frame `frame`.
struct Retired {
    GpuObject obj;
    u32 bindSlot = kInvalid;
    u64 frame    = 0;
};

/// An upload kiln abandoned (unload, failure) before it completed.
struct Orphan {
    u64 token = 0;
    GpuObject obj;
};

} // namespace rt

struct Context {
    Allocator const* alloc = nullptr;
    LogSink log;
    DiagSink diag;
    ProfileHooks profileHooks;
    ProfileHooks const* prof = nullptr; ///< &profileHooks when the host set any hook
    JobSystem jobs;
    bool ownsJobs       = false;
    IoBackend const* io = nullptr;
    Adapter adapter;
    CopyConstraints cc;
    bool devPlaceholders = true;

    char* storeDir    = nullptr; ///< owned copy (null-terminated)
    usize storeDirLen = 0;
    char* profile     = nullptr; ///< owned copy of ContextDesc::profile
    usize profileLen  = 0;
    // Pump thread: the manifest in memory and the profile's entries in it (`manifest`). The store
    // poller swaps in a new one.
    Vec<u8> manifestBytes; ///< empty: no manifest file (yet)
    ManifestProfile manifest;
    bool manifestPresent = false;   ///< false: the manifest has no entries for the profile (or no manifest)
    Root* roots          = nullptr; ///< owned copies of ContextDesc::roots
    u32 rootCount        = 0;
    char* rootChars      = nullptr;
    usize rootCharsLen   = 0;

    u32 maxAssets = 0, maxGroups = 0, maxEvents = 0, maxIoJobs = 0;
    u64 ioBudget = 0;

    rt::Slot* slots   = nullptr;
    u32* freeSlots    = nullptr;
    u32 freeSlotCount = 0;
    HashMap<AssetId, u32> meshMap;
    HashMap<AssetId, u32> texMap;

    rt::GroupRec* groups = nullptr;
    u32* freeGroups      = nullptr;
    u32 freeGroupCount   = 0;

    rt::List queues[u32(rt::QueueId::Count)];

    // completions: workers push, pump pops
    std::mutex compMutex;
    rt::Completion* comp        = nullptr;
    rt::Completion* compScratch = nullptr; ///< pump-side batch copy
    u32 compHead = 0, compCount = 0, compCap = 0;

    // events of the latest pump (or accumulated over a wait())
    Event* events      = nullptr;
    u32 eventCount     = 0;
    bool droppedWarned = false;

    rt::Placeholder ph[rt::kPlaceholderCount];

    // Frames the host reported (PumpOptions); objects and slot numbers wait in `retired`.
    u64 frame          = 0;
    u64 completedFrame = 0;
    Vec<rt::Retired> retired;
    Vec<rt::Orphan> orphans;
    Vec<u32> freeBindSlots;   ///< released bindless slot numbers
    u32 nextBindSlot     = 0; ///< numbers below it were handed out once
    u32 bindPendingCount = 0; ///< slots with bindPending

    CookProvider provider;

    rt::Watch* watch    = nullptr; ///< store poller (null unless ContextDesc::hotReload.watchStore works)
    bool formatsChecked = false;   ///< the profile's formats were checked against the adapter (K5018)

    // post_reload(): any thread pushes, pump swaps `posted` with `postedDrain` and reloads.
    std::mutex postMutex;
    Vec<rt::PostedReload> posted;
    Vec<rt::PostedReload> postedDrain;

    std::thread::id pumpThread;
    bool pumpBound      = false;
    u64 pumpIndex       = 0;
    u32 jobsOutstanding = 0; ///< pump-side count of submitted jobs whose completion was not popped
    PumpStats cur;           ///< stats of the pump in progress

    std::atomic<u32> jobsInFlight{0};    ///< decremented by the job itself (destroy() waits on it)
    std::atomic<u64> ioBytesInFlight{0}; ///< ioInFlightBytes budget in use
};

namespace rt {

// --- registry.cpp -------------------------------------------------------------------
Slot* resolve(Context* ctx, u64 bits, AssetKind kind);
HashMap<AssetId, u32>& map_for(Context* ctx, AssetKind kind);
/// Allocate and initialize a new slot (Pending, queued for the meta stage) or return an
/// existing one. `memory` (moved in) makes it a memory source (register_*). Returns
/// nullptr and emits a diagnostic on failure.
Slot* request_slot(Context* ctx, AssetKind kind, StrView path, RequestOptions const& opt, Buffer* memory,
                   bool rejectExisting);
/// Allocate a texture array declaration with room for `count` layers and `namesLen` name bytes.
ArrayDecl* new_array_decl(Allocator const* a, u32 count, usize namesLen);
void free_array_decl(Allocator const* a, ArrayDecl* d);
/// The job data of each layer (cook output, level table), after a load settles or fails.
void free_array_job_data(Allocator const* a, ArrayDecl& d);
/// A load settled or failed: the artifacts it used become the ones later manifest checks compare.
void adopt_job_keys(Slot& s);
/// A reload of a Ready slot failed: later manifest checks skip the artifacts it tried.
void remember_failed_keys(Slot& s);
/// Runs the reloads post_reload() queued: each loaded slot with the name, and each array with it as a layer.
void drain_posted_reloads(Context* ctx);
void free_slot(Context* ctx, Slot& s);
/// kiln no longer uses `obj` (may be null) and bindless slot `bindSlot` (may be kInvalid):
/// released now if the host reported no frame that may still use them, else in process_retired().
void retire(Context* ctx, GpuObject obj, u32 bindSlot);
/// Drop `s`'s upload target: it goes to Context::orphans until its upload completes.
void orphan_upload(Context* ctx, Slot& s);
/// Bind `s`'s bindless slot to the placeholder its state shows, or mark it pending.
void bind_placeholder(Context* ctx, Slot& s);
/// Bind `s`'s bindless slot to `obj` (no-op without a slot).
void bind_object(Context* ctx, Slot& s, GpuObject obj);
void free_load_data(Slot& s);
void free_meta_set(Allocator const* a, MetaSet& m);
/// The metadata queries show: `cur` when Ready, `next` when MetaReady, else null.
MetaSet const* shown_meta(Slot const& s);
void queue_push(Context* ctx, QueueId q, Slot& s);
void queue_remove(Context* ctx, Slot& s);
GroupRec* group_of(Context* ctx, Slot const& s);
void boost(Context* ctx, Slot& s);
void boost_group(Context* ctx, Group g);
inline u64 handle_bits(Slot const& s) { return (u64(s.generation) << 32) | s.index; }
inline StrView path_of(Slot const& s) { return {s.path, s.pathLen}; }
/// Tex2D always; Cube and Array only when the adapter declares them (AdapterCaps).
[[nodiscard]] inline bool shape_supported(Context const* ctx, TextureShape shape) {
    if (shape == TextureShape::Cube) return (ctx->adapter.caps & kCubeTextures) != 0;
    if (shape == TextureShape::Array) return (ctx->adapter.caps & kArrayTextures) != 0;
    return shape == TextureShape::Tex2D;
}
/// False if the adapter's caps rule out this request: a mesh without kMeshes, or a shape.
[[nodiscard]] inline bool caps_allow(Context const* ctx, AssetKind kind, TextureShape shape) {
    if (kind == AssetKind::Mesh) return (ctx->adapter.caps & kMeshes) != 0;
    return shape_supported(ctx, shape);
}
/// The shape of a KTX2 texture; Count for one kiln does not load (a volume or a cube array).
inline TextureShape shape_of(ktx2::TextureDesc const& d) {
    if (d.depth > 1 || (d.isCube && d.isArray)) return TextureShape::Count;
    return d.isCube ? TextureShape::Cube : d.isArray ? TextureShape::Array : TextureShape::Tex2D;
}

// --- loader.cpp (worker side) -------------------------------------------------------
void run_job(void* arg);
/// `<store>/manifest.dir`. Returns the length `format` reports (>= cap - 1 means
/// truncated). Reads only fields fixed at create().
usize manifest_path(Context const* ctx, char* out, usize cap);
/// Takes `bytes`, a validated manifest `v`, as the one in use, with the context's profile of it.
void adopt_manifest(Context* ctx, Vec<u8>&& bytes, ManifestView const& v);
/// Texture upload layout: levels ascending, each at `offsetAlign`, rows padded to
/// `pitchAlign`. Writes [dstOffset] and [rowPitch] per level; returns the total size.
u64 texture_layout(ktx2::TextureDesc const& d, u64 pitchAlign, u64 offsetAlign, u64* outOffset,
                   u64* outPitch);

// --- context.cpp ------------------------------------------------------------------------
/// A reload starts and no store poller runs: read the manifest again, and use it if it changed (IO
/// on the pump thread; reloads are a dev action). A malformed one is reported and not used.
void refresh_manifest(Context* ctx);

// --- pump.cpp -------------------------------------------------------------------------
void push_event(Context* ctx, EventKind kind, AssetKind asset, u64 bits, u32 version, Status st);
void fail_slot(Context* ctx, Slot& s, u32 code, Status st);
void submit_stage(Context* ctx, Slot& s, Stage stage);
PumpStats pump_impl(Context* ctx, PumpOptions const& opt, bool keepEvents);
/// request_reload(): start a reload of a settled file-source slot, or remember it
/// (reloadPending) until the slot settles. Memory sources: K5012.
void reload_slot(Context* ctx, Slot& s);
/// True if the manifest in use has an entry for the settled file-source `s` that names another
/// artifact than the one it loaded or tried (or it had none).
[[nodiscard]] bool manifest_names_other(Context const* ctx, Slot const& s);
/// Poll non-self-submitting placeholder uploads (create() and pump()).
void poll_placeholders(Context* ctx);
/// Poll abandoned uploads; a completed one is retired.
void poll_orphans(Context* ctx);
/// Release the retired entries whose frame completed.
void process_retired(Context* ctx);

// --- watch.cpp (store poller; stubs without KILN_HOT_RELOAD) ----------------------------
/// create(): start the manifest poller if `desc.watchStore`; K5011 (Warning) if it cannot run.
void watch_start(Context* ctx, HotReloadDesc const& desc);
/// destroy(): stop and join the poller. Safe when not started.
void watch_stop(Context* ctx);
/// destroy(), after the jobs drained: free the poller's state (the poller is joined).
void watch_free(Context* ctx);
/// pump(): swap in a manifest the poller loaded and reload the assets whose key changed.
void watch_drain(Context* ctx);

} // namespace rt
} // namespace kiln
