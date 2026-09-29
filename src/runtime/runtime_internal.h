// runtime_internal.h — private runtime state; included only by src/runtime/*.cpp.
// Registry state is pump-thread only; an in-flight job touches only its slot's job fields.
// See docs/design/threading-and-io.md.
#pragma once

#include "kiln/assets.h"

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
[[nodiscard]] constexpr u32 placeholder_index(TextureKind kind, TextureShape shape) noexcept {
    return u32(shape) * kPlaceholdersPerShape + u32(kind);
}
[[nodiscard]] constexpr u32 failed_placeholder_index(TextureShape shape) noexcept {
    return u32(shape) * kPlaceholdersPerShape + u32(TextureKind::Count);
}

/// Heap bytes with 16-byte alignment (MeshView needs >= 8). Owned explicitly: the
/// owner calls release(); never copied implicitly.
struct Buffer {
    u8* data               = nullptr;
    usize size             = 0;
    Allocator const* alloc = nullptr;
    Tag tag                = Tag::Payload;

    void allocate(Allocator const* a, usize n, Tag t) noexcept;
    void release() noexcept;
    [[nodiscard]] Span<u8 const> span() const noexcept { return {data, size}; }
};

enum class Phase : u8 {
    Free = 0,
    MetaQueued,   ///< waiting for the dispatcher (meta stage)
    MetaJob,      ///< meta stage running on a worker
    UploadQueued, ///< MetaReady; waiting for upload budget / Busy retry
    UploadJob,    ///< upload stage running on a worker
    Awaiting,     ///< committed; polling is_upload_complete
    Done,         ///< Ready or Failed
};

enum class Stage : u8 { Meta = 0, Upload };
enum class SourceKind : u8 { File = 0, Memory };

enum class QueueId : u8 { None = 0, MetaHigh, MetaNormal, UploadHigh, UploadNormal, Await, Count };

enum class CompletionKind : u8 { MetaReady = 0, Uploaded, Failed, BusyRetry };

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
    void reset() noexcept {
        set    = false;
        code   = 0;
        msg[0] = '\0';
    }
};

/// Outputs of the meta stage: validated metadata and the upload plan.
struct MetaSet {
    Buffer meta;                ///< mesh CPU region [0, gpuDataOffset)
    mesh::MeshView meshView;    ///< points into `meta`
    ktx2::TextureDesc texDesc;  ///< textures
    u64* layout      = nullptr; ///< textures: [dstOffset | rowPitch | srcOffset | srcLength] x levels
    u32 layoutLevels = 0;
    u64 uploadSize   = 0; ///< bytes handed to begin_upload
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
    u64 retryAfter = 0;          ///< Busy retry: not before this pump index
    GpuObject realObj;           ///< the payload object (Ready)
    Status preFail   = kOk;      ///< a request-time failure (caps, bindless slots), reported on the next pump
    u32 bindSlot     = kInvalid; ///< bindless slot number (textures with Adapter::bind)
    bool bindPending = false;    ///< bindSlot waits for its placeholder upload to complete

    // --- job input (written by the pump thread before submit) -----------------
    Stage jobStage    = Stage::Meta;
    u32 jobGen        = 0;
    SourceKind source = SourceKind::File;
    Buffer memory;         ///< register_*: the copied cooked bytes
    CookProvider provider; ///< snapshot at dispatch
    char path[kMaxPathLen] = {};
    u32 pathLen            = 0;

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
    IoStat jobStat;            ///< meta stage: the store file's stat (store poller)
    bool jobStatValid = false; ///< false: memory / cook output without a store file, or no stat
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
    bool ready   = false; ///< gpu() may return obj
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
    JobSystem jobs;
    bool ownsJobs       = false;
    IoBackend const* io = nullptr;
    Adapter adapter;
    CopyConstraints cc;
    bool devPlaceholders = true;

    char* storeDir     = nullptr; ///< owned copy (null-terminated)
    usize storeDirLen  = 0;
    Root* roots        = nullptr; ///< owned copies of ContextDesc::roots
    u32 rootCount      = 0;
    char* rootChars    = nullptr;
    usize rootCharsLen = 0;

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

    rt::Watch* watch = nullptr; ///< store poller (null unless ContextDesc::hotReload.watchStore works)

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
Slot* resolve(Context* ctx, u64 bits, AssetKind kind) noexcept;
HashMap<AssetId, u32>& map_for(Context* ctx, AssetKind kind) noexcept;
/// Allocate and initialize a new slot (Pending, queued for the meta stage) or return an
/// existing one. `memory` (moved in) makes it a memory source (register_*). Returns
/// nullptr and emits a diagnostic on failure.
Slot* request_slot(Context* ctx, AssetKind kind, StrView path, RequestOptions const& opt, Buffer* memory,
                   bool rejectExisting) noexcept;
void free_slot(Context* ctx, Slot& s) noexcept;
/// kiln no longer uses `obj` (may be null) and bindless slot `bindSlot` (may be kInvalid):
/// released now if the host reported no frame that may still use them, else in process_retired().
void retire(Context* ctx, GpuObject obj, u32 bindSlot) noexcept;
/// Drop `s`'s upload target: it goes to Context::orphans until its upload completes.
void orphan_upload(Context* ctx, Slot& s) noexcept;
/// Bind `s`'s bindless slot to the placeholder its state shows, or mark it pending.
void bind_placeholder(Context* ctx, Slot& s) noexcept;
/// Bind `s`'s bindless slot to `obj` (no-op without a slot).
void bind_object(Context* ctx, Slot& s, GpuObject obj) noexcept;
void free_load_data(Slot& s) noexcept;
void free_meta_set(Allocator const* a, MetaSet& m) noexcept;
/// The metadata queries show: `cur` when Ready, `next` when MetaReady, else null.
MetaSet const* shown_meta(Slot const& s) noexcept;
void queue_push(Context* ctx, QueueId q, Slot& s) noexcept;
void queue_remove(Context* ctx, Slot& s) noexcept;
GroupRec* group_of(Context* ctx, Slot const& s) noexcept;
void boost(Context* ctx, Slot& s) noexcept;
void boost_group(Context* ctx, Group g) noexcept;
[[nodiscard]] inline u64 handle_bits(Slot const& s) noexcept { return (u64(s.generation) << 32) | s.index; }
[[nodiscard]] inline StrView path_of(Slot const& s) noexcept { return {s.path, s.pathLen}; }
/// Tex2D always; Cube and Array only when the adapter declares them (AdapterCaps).
[[nodiscard]] inline bool shape_supported(Context const* ctx, TextureShape shape) noexcept {
    if (shape == TextureShape::Cube) return (ctx->adapter.caps & kCubeTextures) != 0;
    if (shape == TextureShape::Array) return (ctx->adapter.caps & kArrayTextures) != 0;
    return shape == TextureShape::Tex2D;
}
/// False if the adapter's caps rule out this request: a mesh without kMeshes, or a shape.
[[nodiscard]] inline bool caps_allow(Context const* ctx, AssetKind kind, TextureShape shape) noexcept {
    if (kind == AssetKind::Mesh) return (ctx->adapter.caps & kMeshes) != 0;
    return shape_supported(ctx, shape);
}
/// The shape of a KTX2 texture; Count for one kiln does not load (a volume or a cube array).
[[nodiscard]] inline TextureShape shape_of(ktx2::TextureDesc const& d) noexcept {
    if (d.depth > 1 || (d.isCube && d.isArray)) return TextureShape::Count;
    return d.isCube ? TextureShape::Cube : d.isArray ? TextureShape::Array : TextureShape::Tex2D;
}

// --- loader.cpp (worker side) -------------------------------------------------------
void run_job(void* arg) noexcept;
/// The store file of an asset (store_file_path()). Returns the length
/// `format` reports (>= cap - 1 means truncated). Reads only fields fixed at create().
usize store_path(Context const* ctx, AssetKind kind, StrView path, char* out, usize cap) noexcept;
/// Texture upload layout: levels ascending, each at `offsetAlign`, rows padded to
/// `pitchAlign`. Writes [dstOffset] and [rowPitch] per level; returns the total size.
u64 texture_layout(ktx2::TextureDesc const& d, u64 pitchAlign, u64 offsetAlign, u64* outOffset,
                   u64* outPitch) noexcept;

// --- pump.cpp -------------------------------------------------------------------------
void push_event(Context* ctx, EventKind kind, AssetKind asset, u64 bits, u32 version, Status st) noexcept;
void fail_slot(Context* ctx, Slot& s, u32 code, Status st) noexcept;
void submit_stage(Context* ctx, Slot& s, Stage stage) noexcept;
PumpStats pump_impl(Context* ctx, PumpOptions const& opt, bool keepEvents) noexcept;
/// request_reload(): start a reload of a settled file-source slot, or remember it
/// (reloadPending) until the slot settles. Memory sources: K5012.
void reload_slot(Context* ctx, Slot& s) noexcept;
/// Poll non-self-submitting placeholder uploads (create() and pump()).
void poll_placeholders(Context* ctx) noexcept;
/// Poll abandoned uploads; a completed one is retired.
void poll_orphans(Context* ctx) noexcept;
/// Release the retired entries whose frame completed.
void process_retired(Context* ctx) noexcept;

// --- watch.cpp (store poller; stubs without KILN_HOT_RELOAD) ----------------------------
/// create(): start the poller if `desc.watchStore`; K5011 (Warning) if it cannot run.
void watch_start(Context* ctx, HotReloadDesc const& desc) noexcept;
/// destroy(): stop and join the poller. Safe when not started.
void watch_stop(Context* ctx) noexcept;
/// destroy(), after the jobs drained: free the poller's tables (the poller is joined).
void watch_free(Context* ctx) noexcept;
/// pump(): request a reload for every slot the poller reported (generation checked).
void watch_drain(Context* ctx) noexcept;
/// A slot settled (Ready or Failed, no job, not queued): watch its store file.
void watch_arm(Context* ctx, Slot const& s) noexcept;
/// A slot is loading again or unloaded: stop watching it.
void watch_disarm(Context* ctx, u32 index) noexcept;

} // namespace rt
} // namespace kiln
