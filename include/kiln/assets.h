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
#include "kiln/profile.h"

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
    Ready,        ///< payload uploaded; gpu_object() returns the real object
    Failed,       ///< recoverable error; placeholder served; one diagnostic emitted
};

enum class Priority : u8 { Normal = 0, High };
enum class AssetKind : u8 { Mesh = 0, Texture };
/// After Ready, Changed or Failed, gpu_object() may return another object than before the event (the real
/// one, the reloaded one, the Failed placeholder); after MetaReady it returns the same one.
/// A reload emits no MetaReady: a Failed asset that reloads successfully emits only Ready, with a
/// new version. Set up again for any event whose version is new, not only after MetaReady.
/// Resized: the same artifact at other resident levels (set_texture_extent), with a new version; a
/// host that does not tell it from Changed rebinds on both. A reload that coincides with a size
/// change emits Changed.
enum class EventKind : u8 { MetaReady = 0, Ready, Changed, Failed, Resized };

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
    /// Textures: the largest width or height to load; 0 = the full texture. kiln loads the levels
    /// from the first one that fits as a complete, smaller texture (docs/design/streaming.md). It
    /// applies when the request makes the asset live; a request for a live path keeps its extent.
    u32 maxExtent = 0;
};

/// Host-supplied placeholder pixels for one texture kind (RGBA8, tightly packed).
struct PlaceholderDesc {
    TextureKind kind      = TextureKind::BaseColor;
    Format format         = Format::R8G8B8A8_UNORM; ///< R8G8B8A8_UNORM or _SRGB
    u32 width             = 1;
    u32 height            = 1;
    Span<u8 const> pixels = {}; ///< width * height * 4 bytes
};

/// Dev builds: reload an asset when its manifest entry changes (docs/design/hot-reload.md).
struct HotReloadDesc {
    bool watchStore = false; ///< poll the store's manifest; needs KILN_HOT_RELOAD
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

    /// The store root (read-only for the runtime): `manifest.dir`, which lists the entries of each
    /// target profile, and the artifacts (docs/design/store-manifest.md).
    StrView storeDir       = {};
    Span<Root const> roots = {}; ///< where the cook provider looks for sources (dev)
    /// The target profile whose entries the context reads from the manifest (check_profile_name()).
    StrView profile      = "compat";
    bool devPlaceholders = KILN_DEBUG != 0; ///< Failed textures show the magenta checker
    /// A store whose profile has formats the adapter cannot sample makes create() fail (K5018).
    /// True makes that a warning; each such asset then fails on its own. For tools and debugging. A
    /// profile that first appears after create() is checked then, as a warning.
    bool allowUnsampledFormats               = false;
    Span<PlaceholderDesc const> placeholders = {}; ///< overrides per kind; missing kinds use built-ins

    HotReloadDesc hotReload = {};
    /// The host's profiler (kiln/profile.h): zones for kiln's jobs and the cook provider's stages,
    /// intervals for queue waits, GPU copies and whole loads. Empty = off.
    ProfileHooks profiler = {};

    u32 maxAssets     = 4096; ///< registry capacity (allocated once at create)
    u32 maxGroups     = 64;
    u32 maxEvents     = 1024; ///< events kept between pumps; overflow drops oldest with a warning
    u32 workerThreads = 0;    ///< built-in pool only; 0 = auto
    ThreadPriority workerPriority =
        ThreadPriority::Normal;      ///< built-in pool only; Low keeps loading below the host's threads
    u32 maxIoJobs       = 0;         ///< load jobs that run at once; 0 = worker count
    u64 ioInFlightBytes = 64u << 20; ///< budget for bytes being read at once
};

/// Create a context. Fails with InvalidArgument (K5013) if a root name is invalid or used
/// twice. Placeholders are uploaded through the adapter here; with a self-submitting
/// adapter or one with Adapter::flush, create() waits for them, otherwise gpu_object() returns a
/// null object until the first pump() sees them complete.
KILN_API Result<Context*> create(ContextDesc const& desc);
/// Releases every asset (Adapter::destroy for each GpuObject, at once: the host has waited for
/// its GPU to go idle), stops the built-in pool, frees everything. Outstanding handles become stale.
KILN_API void destroy(Context* ctx);

// ---------------------------------------------------------------------------
// Requests (refcounted; the same path returns the same handle)
// ---------------------------------------------------------------------------

[[nodiscard]] KILN_API MeshHandle request_mesh(Context* ctx, StrView path, RequestOptions const& opt = {});
[[nodiscard]] KILN_API TextureHandle request_texture(Context* ctx, StrView path,
                                                     RequestOptions const& opt = {});
KILN_API void release(Context* ctx, MeshHandle h);
KILN_API void release(Context* ctx, TextureHandle h);

inline constexpr u32 kMaxTextureArrayLayers = 2048;

/// A texture array that kiln assembles at load time from separately cooked 2D textures
/// (docs/design/runtime-texture-arrays.md).
struct TextureArrayDesc {
    /// The array's own name (check_asset_name() rules). It names no store entry; the handle, events and
    /// find_texture() use it.
    StrView name = {};
    /// Texture asset names, layer 0 first; a name may repeat. Every layer must be a 2D texture with the
    /// same format, size and level count as layer 0 (else K5021). At most kMaxTextureArrayLayers.
    Span<StrView const> layers = {};
    TextureKind textureKind    = TextureKind::BaseColor; ///< selects the placeholder
    Priority priority          = Priority::Normal;
    Group group                = {};
    u32 maxExtent              = 0; ///< as RequestOptions::maxExtent; every layer starts at the same level
};
/// Requests the array `desc` declares: a TextureShape::Array texture with the usual states and events.
/// kiln copies the name and the list. The same declaration again returns the same (refcounted) handle;
/// a name another asset or another list already uses is K5020 and a null handle. Release it with
/// release(ctx, TextureHandle). A layer that fails fails the array; a layer whose manifest entry changes
/// reloads the whole array (the old one stays until the new one is Ready). The layers are not loaded as
/// textures of their own, and the adapter gets one upload with every layer (it needs kArrayTextures and
/// staging for the whole array).
[[nodiscard]] KILN_API TextureHandle request_texture_array(Context* ctx, TextureArrayDesc const& desc);

/// Handle for an asset that is already registered/requested (null otherwise).
KILN_API MeshHandle find_mesh(Context* ctx, AssetId id);
KILN_API TextureHandle find_texture(Context* ctx, AssetId id);

/// FNV-1a 64 of a valid asset name; 0 for an invalid one.
KILN_API AssetId asset_id(StrView name);

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
KILN_API char const* check_asset_name(StrView name);
/// Null if `root` is a valid root name (`[a-z0-9_]`, at least 2 characters, not `default`), else a
/// reason.
KILN_API char const* check_root_name(StrView root);

struct AssetNameParts {
    StrView root; ///< empty: the default root
    StrView path; ///< the source file in its root, with its extension
    StrView sub;  ///< the part after `#`; empty if none
};
/// Splits `name` at its first `:` and `#`. Does not check it.
KILN_API AssetNameParts split_asset_name(StrView name);

/// The name that a relative URI inside the source of `owner` refers to: resolved against the
/// owner's directory, in the owner's root. Returns the length written to `out`, or 0 if the
/// URI is absolute, leaves the root, gives an invalid name or does not fit `cap`.
[[nodiscard]] KILN_API usize resolve_asset_name(StrView owner, StrView uri, char* out, usize cap);
/// The texture asset name that binding `b` of mesh `meshName` refers to: an embedded image's name
/// as stored, or an external URI resolved with resolve_asset_name(). Written to `out`
/// (null-terminated) and returned; empty if the name leaves the root, is invalid or does not fit
/// `cap` (kMaxAssetNameLen + 1 always fits).
KILN_API StrView texture_asset_name(StrView meshName, mesh::MeshView const& v, mesh::TextureBinding const& b,
                                    char* out, usize cap);

// ---------------------------------------------------------------------------
// Queries (allocation-free table lookups; stale handles read as Unloaded)
// ---------------------------------------------------------------------------

KILN_API State state(Context* ctx, MeshHandle h);
KILN_API State state(Context* ctx, TextureHandle h);
[[nodiscard]] KILN_API bool has_meta(Context* ctx, MeshHandle h);
[[nodiscard]] KILN_API bool has_meta(Context* ctx, TextureHandle h);
[[nodiscard]] KILN_API bool is_ready(Context* ctx, MeshHandle h);
[[nodiscard]] KILN_API bool is_ready(Context* ctx, TextureHandle h);
KILN_API u32 version(Context* ctx, MeshHandle h);
KILN_API u32 version(Context* ctx, TextureHandle h);
KILN_API AssetId id_of(Context* ctx, MeshHandle h);
KILN_API AssetId id_of(Context* ctx, TextureHandle h);

/// Current GPU object: the placeholder while Pending/Failed (textures), the real
/// object once Ready, the new one after a hot reload. Meshes have no placeholder:
/// a null object until Ready. With a bindless adapter, a texture's `slot` is kiln's slot
/// number from the request on; it does not change while the asset lives.
KILN_API GpuObject gpu_object(Context* ctx, MeshHandle h);
KILN_API GpuObject gpu_object(Context* ctx, TextureHandle h);
/// The placeholder of `kind` and `shape`, the object gpu_object() serves for such a texture before
/// it is Ready. For a host that must bind something where a material has no texture. Null if the
/// adapter lacks the shape (AdapterCaps) or the placeholder's upload has not completed yet. It
/// carries no bindless slot and lives until destroy().
KILN_API GpuObject placeholder_object(Context* ctx, TextureKind kind,
                                      TextureShape shape = TextureShape::Tex2D);

/// Metadata view; nullptr unless has_meta(). Valid until the asset is released or
/// reloaded (a Changed event) and never across destroy().
KILN_API mesh::MeshView const* mesh_view(Context* ctx, MeshHandle h);

struct TextureInfo {
    ktx2::TextureDesc
        desc; ///< of the cooked file (has_meta), full extents and level count, or the placeholder
    /// The file level that is level 0 of the GPU object (RequestOptions::maxExtent); the object has
    /// desc.levels - firstLevel levels, and the spans below cover those.
    u32 firstLevel                  = 0;
    Span<u64 const> levelOffsets    = {}; ///< byte offset of each resident level inside the upload, ascending
    Span<u64 const> levelRowPitches = {}; ///< row pitch used for each resident level
    u64 residentBytes               = 0;  ///< bytes of the upload that made the object (Ready); 0 otherwise
    GpuObject gpu;
    u32 version        = 0;
    bool isPlaceholder = true;
};
KILN_API TextureInfo texture_info(Context* ctx, TextureHandle h);

/// Placeholder kind for a .mesh texture slot (BaseColor/Emissive -> sRGB color kinds, ...).
KILN_API TextureKind texture_kind_for_slot(mesh::TextureSlot slot);

// ---------------------------------------------------------------------------
// Pump and events
// ---------------------------------------------------------------------------

struct PumpOptions {
    u64 uploadBytes    = 64u << 20; ///< max bytes committed to the adapter per pump
    u32 maxCompletions = 0;         ///< 0 = unlimited
    /// Frames, counted from 1; 0 keeps the value last reported. `frame` is the frame the host
    /// records after this pump; `completedFrame` says every frame up to it finished on the GPU.
    /// An object or bindless slot kiln drops during a pump is released (Adapter::destroy) once
    /// `completedFrame` reaches that pump's `frame`. A host that never reports gets immediate
    /// release (docs/design/adapter-frames-slots.md).
    u64 frame          = 0;
    u64 completedFrame = 0;
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
KILN_API PumpStats pump(Context* ctx, PumpOptions const& opt = {});

/// Events since the previous pump(), in order. The span is invalidated by the next pump().
KILN_API Span<Event const> events(Context* ctx);

// ---------------------------------------------------------------------------
// Load groups
// ---------------------------------------------------------------------------

struct GroupStatus {
    u32 ready      = 0;
    u32 failed     = 0;
    u32 pending    = 0; ///< requested but neither Ready nor Failed yet
    u64 bytesDone  = 0;
    u64 bytesTotal = 0; ///< known once each member's metadata has been read
    [[nodiscard]] bool settled() const noexcept { return pending == 0; }
};

struct WaitOptions {
    u32 timeoutMs          = 0; ///< 0 = no timeout
    u64 uploadBytesPerPump = 64u << 20;
};

[[nodiscard]] KILN_API Group group(Context* ctx);
KILN_API void release(Context* ctx, Group g); ///< frees the group record only
KILN_API GroupStatus progress(Context* ctx, Group g);
/// Loops pump() and a short sleep until every member is Ready or Failed, or the timeout
/// expires (returns partial status). Raises members to High priority. Panics, never
/// hangs, when called off the pump thread or when the adapter has neither kSelfSubmitting
/// nor Adapter::flush.
KILN_API GroupStatus wait(Context* ctx, Group g, WaitOptions const& opt = {});

// ---------------------------------------------------------------------------
// Hot reload
// ---------------------------------------------------------------------------

/// Reload the asset: look its name up in the manifest again (through the cook provider, if one is
/// installed; it checks the asset's sources again, PrepareMode::Recheck, so a host with its own
/// file watcher calls this after an edit). Without the store poller, the manifest is read again first when
/// the reload starts. A Ready asset keeps serving its current payload until the new one is ready: then
/// version + 1, bind() for a bindless slot, the old object released after the frames that use it, a Changed
/// event. A failed reload keeps the old version and emits K5010. Memory-registered assets cannot be reloaded
/// (K5012). Works without KILN_HOT_RELOAD; the store poller (ContextDesc::hotReload) calls this for you.
KILN_API void request_reload(Context* ctx, MeshHandle h);
KILN_API void request_reload(Context* ctx, TextureHandle h);
/// Changes the wanted extent of a live texture (RequestOptions::maxExtent; the last call wins). A
/// Ready texture keeps its object until the new one is uploaded, then emits Resized with a new
/// version; a failed change keeps the object and drops the want, with one K5010. A texture that is
/// loading takes the extent for the load after it. The same extent again does nothing. A texture
/// registered in memory takes no size change (K5012): its bytes went to the GPU.
KILN_API void set_texture_extent(Context* ctx, TextureHandle h, u32 maxExtent);
/// request_reload() by name, from any thread: the next pump() reloads the asset `name` of `kind`, and
/// every texture array with `name` as a layer. A name nothing uses is ignored. For watchers that run
/// on their own thread, such as the cook provider's source poller.
KILN_API void post_reload(Context* ctx, AssetKind kind, StrView name);

// ---------------------------------------------------------------------------
// In-memory registration (procedural / generated content, tests, mods)
// ---------------------------------------------------------------------------

/// Register complete cooked bytes under `path`. The bytes are copied; the returned
/// handle goes through the normal upload path and states. Fails (null handle +
/// diagnostic) if the path is already registered or the bytes do not validate.
[[nodiscard]] KILN_API MeshHandle register_mesh(Context* ctx, StrView path, Span<u8 const> meshFile,
                                                RequestOptions const& opt = {});
[[nodiscard]] KILN_API TextureHandle register_texture(Context* ctx, StrView path, Span<u8 const> ktx2File,
                                                      RequestOptions const& opt = {});

// ---------------------------------------------------------------------------
// Cook provider (dev builds; installed by kiln_cook, see kiln/cook/provider.h)
// ---------------------------------------------------------------------------

/// How much CookProvider::prepare checks.
enum class PrepareMode : u8 {
    Normal = 0, ///< the provider may answer from what it checked earlier this session
    Recheck,    ///< check the asset's sources again (request_reload(): a host's own watcher saw a change)
};

/// Called on a worker before every load of a file asset, hit or miss. The provider brings the
/// asset's manifest entry up to date (it cooks again when an input changed) and puts its
/// artifact's build key in `*key`; when it cooked, also the bytes in `out`, which the load then
/// uses. A zero key with bytes: they have no artifact (memory mode). Neither bytes nor a key: the
/// manifest entry is used as it is (a miss if there is none), so a wrapper may handle some names
/// only. NotFound: no source for the name. Without a provider the manifest is used as it is.
struct CookProvider {
    Status (*prepare)(void* user, AssetKind kind, StrView assetPath, PrepareMode mode, Allocator const* alloc,
                      Vec<u8>* out, Hash128* key, DiagSink const* diag) = nullptr;
    void* user                                                          = nullptr;
    /// destroy() calls it for the provider still installed, after the last load has finished,
    /// so the provider can free itself. Null: nothing to free.
    void (*release)(void* user) = nullptr;
};
/// Replaces the installed provider. The old one is not released: a host that wraps it keeps it. When
/// it replaces one, it returns after the loads in flight (which may call the old one) are done.
KILN_API void set_cook_provider(Context* ctx, CookProvider const& provider);
/// The installed provider (null fn if none), so a host can wrap it.
KILN_API CookProvider cook_provider(Context* ctx);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

struct ContextStats {
    u32 assets = 0, pending = 0, metaReady = 0, ready = 0, failed = 0;
    u32 groups          = 0;
    u32 ioJobsInFlight  = 0;
    u64 ioBytesInFlight = 0;
    u32 uploadsInFlight = 0;
    /// The uploads that made the Ready objects, summed (TextureInfo::residentBytes; a mesh's decoded
    /// payload). What the host's budget sees; kiln enforces none (docs/design/streaming.md).
    u64 residentTextureBytes = 0;
    u64 residentMeshBytes    = 0;
};
KILN_API ContextStats stats(Context* ctx);
KILN_API StrView store_dir(Context* ctx);
/// ContextDesc::profile (an owned copy).
KILN_API StrView store_profile(Context* ctx);
/// The roots from ContextDesc (owned copies).
KILN_API Span<Root const> roots(Context* ctx);
KILN_API Allocator const* allocator(Context* ctx);
/// The diagnostic sink from ContextDesc, so a cook provider reports where the host listens.
KILN_API DiagSink const* diag_sink(Context* ctx);
/// The job system the context runs its IO and cook jobs on: the host's JobSystem
/// from ContextDesc, or the built-in pool. Valid until destroy(ctx).
KILN_API JobSystem const* jobs(Context* ctx);
KILN_API Adapter const* adapter(Context* ctx);
/// The profile hooks from ContextDesc, or nullptr when it set none.
KILN_API ProfileHooks const* profile_hooks(Context* ctx);

// ---------------------------------------------------------------------------
// Diagnostics (K5000-K5999: runtime and store). See docs/diagnostics.md.
// ---------------------------------------------------------------------------

enum RuntimeDiagCode : u32 {
    kDiagStoreMiss =
        5001, ///< not in the profile's entries (or no artifact) and no provider fills it (NotFound)
    kDiagCookOnMissFailed = 5002, ///< provider returned an error (its own K1-K3 diagnostics precede this)
    kDiagAssetLoadFailed  = 5003, ///< IO or validation failure while loading (status from the reader)
    kDiagAdapterRejected =
        5004,                  ///< begin_upload failed with something other than Busy, or unsupported format
    kDiagRegistryFull  = 5005, ///< maxAssets / maxGroups reached
    kDiagEventsDropped = 5006, ///< event ring overflowed (Warning)
    kDiagWaitMisuse    = 5007, ///< wait() off the pump thread, or without kSelfSubmitting or flush (panics)
    kDiagDuplicateRegister     = 5008, ///< register_* for an already known path
    kDiagPlaceholderFailed     = 5009, ///< placeholder upload rejected at create()
    kDiagReloadFailed          = 5010, ///< a reload failed; the previous version stays (Error)
    kDiagHotReloadUnavailable  = 5011, ///< not compiled in, or the IO backend has no stat (Warning)
    kDiagReloadMemorySource    = 5012, ///< reload requested for a memory-registered asset (Warning)
    kDiagBadAssetName          = 5013, ///< a request, registration or root breaks the name rules
    kDiagTextureShapeMismatch  = 5017, ///< the cooked texture's shape is not the requested one
    kDiagStoreProfileUnsampled = 5018, ///< the manifest's profile has formats the adapter cannot sample
    kDiagManifestMissing = 5019, ///< a request missed and the manifest (or the profile in it) is missing
    kDiagArrayDeclaration =
        5020, ///< request_texture_array(): a bad declaration, or its name is used by another asset or list
    kDiagArrayLayerMismatch =
        5021,                 ///< an array layer is not 2D or differs from layer 0 (format, size, levels)
    kDiagEntryRemoved = 5022, ///< a loaded asset's entry left the manifest; it stays loaded (Warning)
};

} // namespace kiln
