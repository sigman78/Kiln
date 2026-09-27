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

inline constexpr usize kMaxPathLen      = 256; ///< normalized asset path incl. terminator
inline constexpr u32 kPlaceholderCount  = u32(TextureKind::Count) + 1;
inline constexpr u32 kFailedPlaceholder = u32(TextureKind::Count); ///< index in Context::ph

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

struct Slot {
    // --- identity / registry (pump thread) --------------------------------------
    Context* ctx        = nullptr;
    u32 index           = 0;
    u32 generation      = 1;
    AssetId id          = 0;
    AssetKind kind      = AssetKind::Mesh;
    State state         = State::Unloaded;
    Phase phase         = Phase::Free;
    Priority priority   = Priority::Normal;
    TextureKind texKind = TextureKind::BaseColor;
    bool live           = false; ///< occupied (including zombies)
    bool zombie         = false; ///< released while a job was in flight
    bool jobInFlight    = false;
    u32 refcount        = 0;
    u32 version         = 0;
    u32 groupIndex      = kInvalid;
    u32 groupGen        = 0;
    u64 groupBytes      = 0; ///< contribution to the group's bytesTotal
    // queue links (intrusive, over slot indices)
    QueueId queue  = QueueId::None;
    u32 qPrev      = kInvalid;
    u32 qNext      = kInvalid;
    u64 retryAfter = 0;   ///< Busy retry: not before this pump index
    GpuObject acquired;   ///< from Adapter::acquire (may be null)
    GpuObject realObj;    ///< the published payload object (Ready)
    Status preFail = kOk; ///< acquire() failure, reported on the next pump

    // --- job input (written by the pump thread before submit) -----------------
    Stage jobStage    = Stage::Meta;
    u32 jobGen        = 0;
    SourceKind source = SourceKind::File;
    Buffer memory;         ///< register_*: the copied cooked bytes
    CookProvider provider; ///< snapshot at dispatch
    char path[kMaxPathLen] = {};
    u32 pathLen            = 0;

    // --- meta stage output (worker), read-only afterwards -----------------------
    Buffer meta;                ///< mesh CPU region [0, gpuDataOffset)
    mesh::MeshView meshView;    ///< points into `meta`
    ktx2::TextureDesc texDesc;  ///< textures
    u64* layout      = nullptr; ///< textures: [dstOffset | rowPitch | srcOffset | srcLength] x levels
    u32 layoutLevels = 0;
    u64 uploadSize   = 0; ///< bytes handed to begin_upload
    Vec<u8> cooked;       ///< cook provider output (memory source for both stages)
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
    bool ready   = false; ///< gpu() may return obj
    AssetId id   = 0;
    ktx2::TextureDesc desc;
    u64 offset = 0; ///< level 0 offset (always 0)
    u64 pitch  = 0; ///< level 0 row pitch
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
    StrView* roots     = nullptr; ///< owned copies of sourceRoots
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

    CookProvider provider;

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
/// Normalize an asset path (forward slashes, no leading "./" or "/", "./" segments and
/// repeated slashes collapsed, extension stripped). Returns the length, or StrView::kNpos
/// if it does not fit `cap` (including the terminator).
usize normalize_path(StrView in, char* out, usize cap) noexcept;
Slot* resolve(Context* ctx, u64 bits, AssetKind kind) noexcept;
HashMap<AssetId, u32>& map_for(Context* ctx, AssetKind kind) noexcept;
/// Allocate and initialize a new slot (Pending, queued for the meta stage) or return an
/// existing one. `memory` (moved in) makes it a memory source (register_*). Returns
/// nullptr and emits a diagnostic on failure.
Slot* request_slot(Context* ctx, AssetKind kind, StrView path, RequestOptions const& opt, Buffer* memory,
                   bool rejectExisting) noexcept;
void free_slot(Context* ctx, Slot& s) noexcept;
void free_load_data(Slot& s) noexcept;
void queue_push(Context* ctx, QueueId q, Slot& s) noexcept;
void queue_remove(Context* ctx, Slot& s) noexcept;
GroupRec* group_of(Context* ctx, Slot const& s) noexcept;
void boost(Context* ctx, Slot& s) noexcept;
void boost_group(Context* ctx, Group g) noexcept;
[[nodiscard]] inline u64 handle_bits(Slot const& s) noexcept { return (u64(s.generation) << 32) | s.index; }
[[nodiscard]] inline StrView path_of(Slot const& s) noexcept { return {s.path, s.pathLen}; }

// --- loader.cpp (worker side) -------------------------------------------------------
void run_job(void* arg) noexcept;
/// Texture upload layout: levels ascending, each at `offsetAlign`, rows padded to
/// `pitchAlign`. Writes [dstOffset] and [rowPitch] per level; returns the total size.
u64 texture_layout(ktx2::TextureDesc const& d, u64 pitchAlign, u64 offsetAlign, u64* outOffset,
                   u64* outPitch) noexcept;

// --- pump.cpp -------------------------------------------------------------------------
void push_event(Context* ctx, EventKind kind, AssetKind asset, u64 bits, u32 version, Status st) noexcept;
void fail_slot(Context* ctx, Slot& s, u32 code, Status st) noexcept;
void submit_stage(Context* ctx, Slot& s, Stage stage) noexcept;
PumpStats pump_impl(Context* ctx, PumpOptions const& opt, bool keepEvents) noexcept;
/// Poll non-self-submitting placeholder uploads (create() and pump()).
void poll_placeholders(Context* ctx) noexcept;

} // namespace rt
} // namespace kiln
