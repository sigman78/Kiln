// kiln/assets.h — the runtime: context, handles, requests, pump, events, groups, registration.
// Call everything on the pump thread unless noted; completion arrives only from pump().
// See docs/design/handles-and-states.md and docs/design/threading-and-io.md.
#pragma once

#include "kiln/adapter.h"
#include "kiln/containers.h"
#include "kiln/io.h"
#include "kiln/ktx2.h"
#include "kiln/log.h"
#include "kiln/mesh.h"

namespace kiln {

struct Context;

// Handle tags
struct Mesh;
struct Texture;
struct GroupTag;
using MeshHandle    = Handle<Mesh>;
using TextureHandle = Handle<Texture>;
using Group         = Handle<GroupTag>;

enum class State : u8 {
    Unloaded = 0, ///< no request outstanding (also: null / stale handle)
    Pending,      ///< requested; nothing usable yet (textures serve their placeholder)
    MetaReady,    ///< metadata readable (mesh_view / texture_info); GPU payload in flight
    Ready,        ///< payload published; gpu() returns the real object
    Failed,       ///< recoverable error; placeholder served; one diagnostic emitted
    Partial,      ///< reserved (progressive loads, v0.8)
};

enum class Priority : u8 { Normal = 0, High };
enum class AssetKind : u8 { Mesh = 0, Texture };
/// After Ready, Changed or Failed, gpu() may return another object than before the event (the real
/// one, the reloaded one, the Failed placeholder); after MetaReady it returns the same one.
enum class EventKind : u8 { MetaReady = 0, Ready, Changed, Failed };

struct Event {
    EventKind kind  = EventKind::Ready;
    AssetKind asset = AssetKind::Mesh;
    u64 handle      = 0;   ///< Handle<...>::bits(); rebuild with MeshHandle::from_bits() etc.
    u32 version     = 0;   ///< content version after the event
    Status status   = kOk; ///< non-Ok only for Failed
};

struct RequestOptions {
    Priority priority = Priority::Normal;
    Group group       = {}; ///< null = no group
    TextureKind textureKind =
        TextureKind::BaseColor; ///< textures: selects the placeholder (first request wins)
    /// Textures: the shape the host expects; selects the placeholder (first request wins). A
    /// cooked file of another shape fails the load (K5017).
    TextureShape textureShape = TextureShape::Tex2D;
    // reserved: range (partial loads, v0.8)
};

/// Where cooked files live. v0.5 has one layout, `store_file_path()` (what `kiln-cook` writes
/// by default). The content hash is stored inside each file for invalidation; a hashed
/// layout with an index file arrives in v0.6.
enum class StoreLayout : u8 { Named = 0 };

/// Host-supplied placeholder pixels for one texture kind (RGBA8, tightly packed).
struct PlaceholderDesc {
    TextureKind kind      = TextureKind::BaseColor;
    Format format         = Format::R8G8B8A8_UNORM; ///< R8G8B8A8_UNORM or _SRGB
    u32 width             = 1;
    u32 height            = 1;
    Span<u8 const> pixels = {}; ///< width * height * 4 bytes
};

/// Dev builds: reload an asset when its cooked file changes (docs/design/hot-reload.md).
struct HotReloadDesc {
    bool watchStore = false; ///< poll the store files of loaded assets; needs KILN_HOT_RELOAD
    u32 pollMs      = 250;
};

/// A named directory of sources for the cook provider. Each root has its own namespace of names.
struct Root {
    StrView name = {}; ///< empty: the default root, whose asset names have no prefix
    StrView dir  = {}; ///< the directory that holds its sources
};

struct ContextDesc {
    Allocator const* alloc = nullptr; ///< nullptr = default allocator
    LogSink log            = {};      ///< fn null = process-wide sink (log.h)
    DiagSink diag          = {};      ///< runtime diagnostics (asset failures, store misses)
    JobSystem const* jobs  = nullptr; ///< nullptr = built-in thread pool
    IoBackend const* io    = nullptr; ///< nullptr = compat backend
    Adapter const* adapter = nullptr; ///< required

    StrView storeDir                         = {}; ///< cooked store root (read-only for the runtime)
    Span<Root const> roots                   = {}; ///< where the cook provider looks for sources (dev)
    StoreLayout storeLayout                  = StoreLayout::Named;
    bool devPlaceholders                     = KILN_DEBUG != 0; ///< Failed textures show the magenta checker
    Span<PlaceholderDesc const> placeholders = {}; ///< overrides per kind; missing kinds use built-ins

    HotReloadDesc hotReload = {};

    u32 maxAssets     = 4096; ///< registry capacity (allocated once at create)
    u32 maxGroups     = 64;
    u32 maxEvents     = 1024; ///< events kept between pumps; overflow drops oldest with a warning
    u32 workerThreads = 0;    ///< built-in pool only; 0 = auto
    ThreadPriority workerPriority =
        ThreadPriority::Normal;      ///< built-in pool only; Low keeps loading below the host's threads
    u32 maxIoJobs       = 0;         ///< concurrent reads; 0 = worker count
    u64 ioInFlightBytes = 64u << 20; ///< budget for bytes being read at once
};

/// Create a context. Fails with InvalidArgument (K5013) if a root name is invalid or used
/// twice. Placeholders are uploaded through the adapter here; with a self-submitting
/// adapter or one with Adapter::flush, create() waits for them, otherwise gpu() returns a
/// null object until the first pump() sees them complete.
[[nodiscard]] KILN_API Result<Context*> create(ContextDesc const& desc) noexcept;
/// Releases every asset (destroy_deferred for each GpuObject), stops the built-in
/// pool, frees everything. Outstanding handles become stale.
KILN_API void destroy(Context* ctx) noexcept;

// ---------------------------------------------------------------------------
// Requests (refcounted; the same path returns the same handle)
// ---------------------------------------------------------------------------

[[nodiscard]] KILN_API MeshHandle request_mesh(Context* ctx, StrView path,
                                               RequestOptions const& opt = {}) noexcept;
[[nodiscard]] KILN_API TextureHandle request_texture(Context* ctx, StrView path,
                                                     RequestOptions const& opt = {}) noexcept;
KILN_API void release(Context* ctx, MeshHandle h) noexcept;
KILN_API void release(Context* ctx, TextureHandle h) noexcept;

/// Handle for an asset that is already registered/requested (null otherwise).
[[nodiscard]] KILN_API MeshHandle find_mesh(Context* ctx, AssetId id) noexcept;
[[nodiscard]] KILN_API TextureHandle find_texture(Context* ctx, AssetId id) noexcept;

/// FNV-1a 64 of a valid asset name; 0 for an invalid one.
[[nodiscard]] KILN_API AssetId asset_id(StrView name) noexcept;

// ---------------------------------------------------------------------------
// Asset names: `root:path/file.ext#sub` (docs/design/asset-model-next.md, Part 2).
// A name is checked where it enters kiln and compared byte for byte after that.
// ---------------------------------------------------------------------------

inline constexpr usize kMaxAssetNameLen = 255;

/// Null if `name` is valid, else a short reason. Valid: at most kMaxAssetNameLen bytes; an
/// optional `root:` prefix (see check_root_name); then `/`-separated segments that are not
/// empty, `.` or `..`; no control characters and none of `\ : < > " | ? *`; at most one `#`,
/// in the last segment, with text on both sides. Without a root prefix the path may not start
/// with `@`, which the store uses for named roots.
[[nodiscard]] KILN_API char const* check_asset_name(StrView name) noexcept;
/// Null if `root` is a valid root name (`[a-z0-9_]`, at least 2 characters), else a reason.
[[nodiscard]] KILN_API char const* check_root_name(StrView root) noexcept;

struct AssetNameParts {
    StrView root; ///< empty: the default root
    StrView path; ///< the source file in its root, with its extension
    StrView sub;  ///< the part after `#`; empty if none
};
/// Splits `name` at its first `:` and `#`. Does not check it.
[[nodiscard]] KILN_API AssetNameParts split_asset_name(StrView name) noexcept;

/// The name that a relative URI inside the source of `owner` refers to: resolved against the
/// owner's directory, in the owner's root. Returns the length written to `out`, or 0 if the
/// URI is absolute, leaves the root, gives an invalid name or does not fit `cap`.
[[nodiscard]] KILN_API usize resolve_asset_name(StrView owner, StrView uri, char* out, usize cap) noexcept;

/// The cooked file of `name` in the Named layout: `<storeDir>/<name>.mesh|.ktx2`, with the
/// prefix `m:` of a named root written as the top-level directory `@m/`. Returns what `format()`
/// returns (>= cap - 1 means truncated).
[[nodiscard]] KILN_API usize store_file_path(StrView storeDir, AssetKind kind, StrView name, char* out,
                                             usize cap) noexcept;

// ---------------------------------------------------------------------------
// Queries (allocation-free table lookups; stale handles read as Unloaded)
// ---------------------------------------------------------------------------

[[nodiscard]] KILN_API State state(Context* ctx, MeshHandle h) noexcept;
[[nodiscard]] KILN_API State state(Context* ctx, TextureHandle h) noexcept;
[[nodiscard]] KILN_API bool has_meta(Context* ctx, MeshHandle h) noexcept;
[[nodiscard]] KILN_API bool has_meta(Context* ctx, TextureHandle h) noexcept;
[[nodiscard]] KILN_API bool is_ready(Context* ctx, MeshHandle h) noexcept;
[[nodiscard]] KILN_API bool is_ready(Context* ctx, TextureHandle h) noexcept;
[[nodiscard]] KILN_API u32 version(Context* ctx, MeshHandle h) noexcept;
[[nodiscard]] KILN_API u32 version(Context* ctx, TextureHandle h) noexcept;
[[nodiscard]] KILN_API AssetId id_of(Context* ctx, MeshHandle h) noexcept;
[[nodiscard]] KILN_API AssetId id_of(Context* ctx, TextureHandle h) noexcept;

/// Current GPU object: the placeholder while Pending/Failed (textures), the real
/// object once Ready, the new one after a hot reload. Meshes have no placeholder:
/// a null object until Ready.
[[nodiscard]] KILN_API GpuObject gpu(Context* ctx, MeshHandle h) noexcept;
[[nodiscard]] KILN_API GpuObject gpu(Context* ctx, TextureHandle h) noexcept;

/// Metadata view; nullptr unless has_meta(). Valid until the asset is released or
/// reloaded (a Changed event) and never across destroy().
[[nodiscard]] KILN_API mesh::MeshView const* mesh_view(Context* ctx, MeshHandle h) noexcept;

struct TextureInfo {
    ktx2::TextureDesc desc;            ///< of the real texture (has_meta) or the placeholder
    Span<u64 const> levelOffsets = {}; ///< byte offset of each level inside the upload, ascending level order
    Span<u64 const> levelRowPitches = {}; ///< row pitch used for each level
    GpuObject gpu;
    u32 version        = 0;
    bool isPlaceholder = true;
};
[[nodiscard]] KILN_API TextureInfo texture_info(Context* ctx, TextureHandle h) noexcept;

/// Placeholder kind for a .mesh texture slot (BaseColor/Emissive -> sRGB color kinds, ...).
[[nodiscard]] KILN_API TextureKind texture_kind_for_slot(mesh::TextureSlot slot) noexcept;

// ---------------------------------------------------------------------------
// Pump and events
// ---------------------------------------------------------------------------

struct PumpOptions {
    u64 uploadBytes    = 64u << 20; ///< max bytes committed to the adapter per pump
    u32 maxCompletions = 0;         ///< 0 = unlimited
};

struct PumpStats {
    u32 completed        = 0; ///< assets that became MetaReady/Ready/Failed this pump
    u32 uploadsStarted   = 0; ///< uploads dispatched for the first time (retries are not counted again)
    u32 uploadsCommitted = 0;
    u64 uploadBytes   = 0; ///< bytes of those uploads; retries consume the budget but are not counted again
    u32 busyRetries   = 0; ///< begin_upload returned Busy
    u32 eventsDropped = 0;
};

/// Drive the pipeline: hand out upload memory, collect completed reads and decodes,
/// poll the adapter for finished uploads, publish, emit events. Never allocates in
/// steady state.
KILN_API PumpStats pump(Context* ctx, PumpOptions const& opt = {}) noexcept;

/// Events since the previous pump(), in order. The span is invalidated by the next pump().
[[nodiscard]] KILN_API Span<Event const> events(Context* ctx) noexcept;

// ---------------------------------------------------------------------------
// Load groups
// ---------------------------------------------------------------------------

struct GroupStatus {
    u32 ready      = 0;
    u32 failed     = 0;
    u32 pending    = 0; ///< requested but neither Ready nor Failed yet
    u64 bytesDone  = 0;
    u64 bytesTotal = 0; ///< known once each member's metadata has been read
    [[nodiscard]] constexpr bool settled() const noexcept { return pending == 0; }
};

struct WaitOptions {
    u32 timeoutMs          = 0; ///< 0 = no timeout
    u64 uploadBytesPerPump = 64u << 20;
};

[[nodiscard]] KILN_API Group group(Context* ctx) noexcept;
KILN_API void release(Context* ctx, Group g) noexcept; ///< frees the group record only
[[nodiscard]] KILN_API GroupStatus progress(Context* ctx, Group g) noexcept;
/// Loops pump() and a short sleep until every member is Ready or Failed, or the timeout
/// expires (returns partial status). Raises members to High priority. Panics, never
/// hangs, when called off the pump thread or when the adapter has neither kSelfSubmitting
/// nor Adapter::flush.
[[nodiscard]] KILN_API GroupStatus wait(Context* ctx, Group g, WaitOptions const& opt = {}) noexcept;

// ---------------------------------------------------------------------------
// Hot reload
// ---------------------------------------------------------------------------

/// Reload the asset from its store file (through the cook provider if the file is
/// missing). A Ready asset keeps serving its current payload until the new one is
/// published: then version + 1, publish(), destroy_deferred(old), a Changed event.
/// A failed reload keeps the old version and emits K5010. Memory-registered assets
/// cannot be reloaded (K5012). Works without KILN_HOT_RELOAD; the store poller
/// (ContextDesc::hotReload) calls this for you.
KILN_API void request_reload(Context* ctx, MeshHandle h) noexcept;
KILN_API void request_reload(Context* ctx, TextureHandle h) noexcept;

// ---------------------------------------------------------------------------
// In-memory registration (procedural / generated content, tests, mods)
// ---------------------------------------------------------------------------

/// Register complete cooked bytes under `path`. The bytes are copied; the returned
/// handle goes through the normal upload path and states. Fails (null handle +
/// diagnostic) if the path is already registered or the bytes do not validate.
[[nodiscard]] KILN_API MeshHandle register_mesh(Context* ctx, StrView path, Span<u8 const> meshFile,
                                                RequestOptions const& opt = {}) noexcept;
[[nodiscard]] KILN_API TextureHandle register_texture(Context* ctx, StrView path, Span<u8 const> ktx2File,
                                                      RequestOptions const& opt = {}) noexcept;

// ---------------------------------------------------------------------------
// Cook provider (dev builds; installed by kiln_cook, see kiln/cook/provider.h)
// ---------------------------------------------------------------------------

/// Called on a worker thread when the store has no file for an asset. Produces the
/// cooked bytes for `assetPath` (writing them to the store as a side effect in disk
/// mode). Returns NotFound when no source exists.
struct CookProvider {
    Status (*cook)(void* user, AssetKind kind, StrView assetPath, Allocator const* alloc, Vec<u8>* out,
                   DiagSink const* diag) = nullptr;
    void* user                           = nullptr;
};
KILN_API void set_cook_provider(Context* ctx, CookProvider const& provider) noexcept;
/// The installed provider (null fn if none), so a host can wrap it.
[[nodiscard]] KILN_API CookProvider cook_provider(Context* ctx) noexcept;

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

struct ContextStats {
    u32 assets = 0, pending = 0, metaReady = 0, ready = 0, failed = 0;
    u32 groups          = 0;
    u32 ioJobsInFlight  = 0;
    u64 ioBytesInFlight = 0;
    u32 uploadsInFlight = 0;
};
[[nodiscard]] KILN_API ContextStats stats(Context* ctx) noexcept;
[[nodiscard]] KILN_API StrView store_dir(Context* ctx) noexcept;
/// The roots from ContextDesc (owned copies).
[[nodiscard]] KILN_API Span<Root const> roots(Context* ctx) noexcept;
[[nodiscard]] KILN_API Allocator const* allocator(Context* ctx) noexcept;
/// The job system the context runs its IO and cook jobs on: the host's JobSystem
/// from ContextDesc, or the built-in pool. Valid until destroy(ctx).
[[nodiscard]] KILN_API JobSystem const* jobs(Context* ctx) noexcept;
[[nodiscard]] KILN_API Adapter const* adapter(Context* ctx) noexcept;

// ---------------------------------------------------------------------------
// Diagnostics (K5000-K5999: runtime and store). See docs/diagnostics.md.
// ---------------------------------------------------------------------------

enum RuntimeDiagCode : u32 {
    kDiagStoreMiss        = 5001, ///< no cooked file and no provider / provider found no source (NotFound)
    kDiagCookOnMissFailed = 5002, ///< provider returned an error (its own K1-K3 diagnostics precede this)
    kDiagAssetLoadFailed  = 5003, ///< IO or validation failure while loading (status from the reader)
    kDiagAdapterRejected =
        5004,                  ///< begin_upload failed with something other than Busy, or unsupported format
    kDiagRegistryFull  = 5005, ///< maxAssets / maxGroups reached
    kDiagEventsDropped = 5006, ///< event ring overflowed (Warning)
    kDiagWaitMisuse    = 5007, ///< wait() off the pump thread, or without kSelfSubmitting or flush (panics)
    kDiagDuplicateRegister    = 5008, ///< register_* for an already known path
    kDiagPlaceholderFailed    = 5009, ///< placeholder upload rejected at create()
    kDiagReloadFailed         = 5010, ///< a reload failed; the previous version stays (Error)
    kDiagHotReloadUnavailable = 5011, ///< not compiled in, or the IO backend has no stat (Warning)
    kDiagReloadMemorySource   = 5012, ///< reload requested for a memory-registered asset (Warning)
    kDiagBadAssetName         = 5013, ///< a request, registration or root breaks the name rules
    kDiagTextureShapeMismatch = 5017, ///< the cooked texture's shape is not the requested one
};

} // namespace kiln
