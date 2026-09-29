// watch.cpp — the store poller: reload an asset when its cooked file changes
// (docs/design/hot-reload.md, "Runtime"). Compiled in only with KILN_HOT_RELOAD;
// without it, asking for the poller is K5011 and every hook is a no-op.
//
// Synchronization. The poller thread never reads Slot or any other registry state. It
// works on its own table, one WatchEntry per slot index, that only the pump thread
// writes: watch_arm() when a slot settles (copies generation, kind, path and the stat
// the meta stage recorded), watch_disarm() when the slot loads again or unloads. Every
// access to the table and to the hit list is under Watch::mutex. A round copies one
// armed entry under the lock, stats the file without it, and re-checks the entry under
// the lock before reporting a change (still armed, same generation, same recorded
// stat). A reported entry is disarmed, so it is reported once until the next settle.
// pump() (watch_drain) copies the hits out under the lock and requests the reloads
// after releasing it; a hit whose slot generation changed meanwhile is dropped.
// Catalog layout: no slot is armed; the poller hands a new catalog over under the mutex
// (docs/design/hot-reload.md).
#include "runtime_internal.h"

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD
#include <chrono>
#include <condition_variable>
#endif

namespace kiln::rt {

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD

struct WatchEntry {
    u32 generation = 0; ///< slot generation the entry describes (0 = never armed)
    bool armed     = false;
    bool hasStat   = false; ///< a stat was recorded for this generation
    AssetKind kind = AssetKind::Mesh;
    IoStat stat;
    u32 pathLen            = 0;
    char path[kMaxPathLen] = {};
};

struct WatchHit {
    u32 index      = 0;
    u32 generation = 0;
};

struct Watch {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop           = false;   ///< under mutex
    WatchEntry* entries = nullptr; ///< maxAssets, under mutex
    WatchHit* hits      = nullptr; ///< maxAssets, under mutex
    u32 hitCount        = 0;       ///< under mutex
    WatchHit* drained   = nullptr; ///< maxAssets, pump thread only
    u32 pollMs          = 250;
    std::thread thread;

    // Catalog layout.
    IoStat catalogStat;            ///< poller thread only: the catalog file as last read
    bool catalogStatValid = false; ///< poller thread only
    Hash128 catalogSum;            ///< under mutex: checksum of the catalog in use (or pending)
    Vec<u8> pending;               ///< under mutex: a new catalog for the pump thread
    CatalogView pendingView;       ///< under mutex: views `pending`
    bool hasPending = false;       ///< under mutex
};

namespace {

bool same_stat(IoStat const& a, IoStat const& b) noexcept {
    return a.size == b.size && a.mtimeNs == b.mtimeNs;
}

/// The checksum stored in a validated catalog.
Hash128 checksum_of(Span<u8 const> catalog) noexcept {
    Hash128 h;
    std::memcpy(h.bytes, catalog.data + kCatalogChecksumOffset, sizeof h.bytes);
    return h;
}

/// Reads the catalog again if its stat changed; a valid one with a new checksum becomes pending.
void poll_catalog(Context* ctx, Watch& w) noexcept {
    IoBackend const* io = ctx->io;
    char path[1024];
    usize const n = catalog_path(ctx, path, sizeof path);
    if (n + 1 >= sizeof path) return;
    IoStat now;
    // Missing (not written yet, or a rename in progress): look again next round.
    if (io->stat(io->user, StrView(path, n), &now).failed()) return;
    if (w.catalogStatValid && same_stat(now, w.catalogStat)) return;
    Vec<u8> bytes(ctx->alloc, Tag::Io);
    if (io_read_file(io, StrView(path, n), ctx->alloc, &bytes).failed()) return;
    w.catalogStat               = now;
    w.catalogStatValid          = true;
    Result<CatalogView> const v = CatalogView::open(bytes.span(), nullptr, StrView(path, n));
    if (v.failed()) {
        KILN_WARN("reload", "%s changed but is not a valid catalog (%s); keeping the one in use", path,
                  code_name(v.status().code));
        return;
    }
    Hash128 const sum = checksum_of(bytes.span());
    std::lock_guard<std::mutex> lock(w.mutex);
    if (sum == w.catalogSum) return;
    w.catalogSum  = sum;
    w.pending     = std::move(bytes); // the view's bytes stay where they are
    w.pendingView = *v;
    w.hasPending  = true;
}

/// A new catalog is in use: reload each asset whose entry names another artifact than the one it
/// loaded or tried. An asset that left the catalog stays as it is.
void catalog_changed(Context* ctx) noexcept {
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot& s = ctx->slots[i];
        if (!s.live || s.zombie || s.source != SourceKind::File) continue;
        CatalogEntry e;
        if (!ctx->catalog.find(s.kind, path_of(s), &e)) continue;
        // A load in flight is compared by its dispatch key: its job owns jobKey until it completes.
        bool const settled = s.phase == Phase::Done;
        bool const hasKey  = settled ? s.keyValid : s.dispatchKeyValid;
        Hash128 const& key = settled ? s.key : s.dispatchKey;
        if (hasKey && key == e.key) continue;
        reload_slot(ctx, s);
    }
}

/// One round over every armed entry. Returns false when asked to stop.
bool poll_round(Context* ctx, Watch& w) noexcept {
    IoBackend const* io = ctx->io;
    if (ctx->layout == StoreLayout::Catalog) {
        poll_catalog(ctx, w);
        std::lock_guard<std::mutex> lock(w.mutex);
        return !w.stop;
    }
    WatchEntry e;
    char file[1024];
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        {
            std::lock_guard<std::mutex> lock(w.mutex);
            if (w.stop) return false;
            WatchEntry const& cur = w.entries[i];
            if (!cur.armed) continue;
            e.generation = cur.generation;
            e.kind       = cur.kind;
            e.stat       = cur.stat;
            e.pathLen    = cur.pathLen;
            std::memcpy(e.path, cur.path, cur.pathLen);
        }
        usize const n = store_path(ctx, e.kind, StrView(e.path, e.pathLen), file, sizeof file);
        if (n + 1 >= sizeof file) continue;
        IoStat now;
        // A failed stat (a rewrite by rename in progress, a deleted file) is not a change.
        if (io->stat(io->user, StrView(file, n), &now).failed() || same_stat(now, e.stat)) continue;
        std::lock_guard<std::mutex> lock(w.mutex);
        WatchEntry& cur = w.entries[i];
        if (!cur.armed || cur.generation != e.generation || !same_stat(cur.stat, e.stat)) continue;
        if (w.hitCount == ctx->maxAssets) continue; // cannot happen: one hit per armed slot
        w.hits[w.hitCount++] = WatchHit{i, e.generation};
        cur.armed            = false;
    }
    return true;
}

void poll_main(Context* ctx) noexcept {
    Watch& w = *ctx->watch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(w.mutex);
            if (w.wake.wait_for(lock, std::chrono::milliseconds(w.pollMs), [&] { return w.stop; })) return;
        }
        if (!poll_round(ctx, w)) return;
    }
}

void unavailable(Context* ctx, char const* why) noexcept {
    (void)diagf(&ctx->diag, make_status(Code::Unsupported), kDiagHotReloadUnavailable, Severity::Warning, {},
                "hot reload", "store poller not started: %s", why);
}

} // namespace

void watch_start(Context* ctx, HotReloadDesc const& desc) noexcept {
    if (!desc.watchStore) return;
    if (!ctx->io->stat) {
        unavailable(ctx, "the IO backend has no stat()");
        return;
    }
    Allocator const* a = ctx->alloc;
    Watch* w           = new_object<Watch>(a, Tag::Registry);
    w->entries         = alloc_array<WatchEntry>(a, ctx->maxAssets, Tag::Registry);
    for (u32 i = 0; i < ctx->maxAssets; ++i)
        ::new (static_cast<void*>(w->entries + i)) WatchEntry();
    w->hits    = alloc_array<WatchHit>(a, ctx->maxAssets, Tag::Registry);
    w->drained = alloc_array<WatchHit>(a, ctx->maxAssets, Tag::Registry);
    w->pollMs  = max(desc.pollMs, 1u);
    w->pending.init(a, Tag::Io);
    if (ctx->catalogPresent) w->catalogSum = checksum_of(ctx->catalogBytes.span());
    // Set before any job runs and cleared only by watch_free() after the jobs drained:
    // workers read it (the meta stage records a stat only when watching).
    ctx->watch = w;

    // std::thread's constructor may throw on resource exhaustion; converted here.
#if KILN_HAS_EXCEPTIONS
    try {
        w->thread = std::thread(&poll_main, ctx);
    } catch (...) {
    }
#else
    w->thread = std::thread(&poll_main, ctx);
#endif
    if (!w->thread.joinable()) {
        watch_free(ctx);
        unavailable(ctx, "cannot start the poller thread");
    }
}

void watch_stop(Context* ctx) noexcept {
    Watch* w = ctx->watch;
    if (!w || !w->thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(w->mutex);
        w->stop = true;
    }
    w->wake.notify_all();
    w->thread.join();
}

void watch_free(Context* ctx) noexcept {
    Watch* w = ctx->watch;
    if (!w) return;
    KILN_ASSERT(!w->thread.joinable());
    Allocator const* a = ctx->alloc;
    free_array(a, w->entries, ctx->maxAssets, Tag::Registry);
    free_array(a, w->hits, ctx->maxAssets, Tag::Registry);
    free_array(a, w->drained, ctx->maxAssets, Tag::Registry);
    w->pending.release();
    delete_object(a, w, Tag::Registry);
    ctx->watch = nullptr;
}

void watch_drain(Context* ctx) noexcept {
    Watch* w = ctx->watch;
    if (!w) return;
    u32 n           = 0;
    bool newCatalog = false;
    {
        std::lock_guard<std::mutex> lock(w->mutex);
        n = w->hitCount;
        for (u32 i = 0; i < n; ++i)
            w->drained[i] = w->hits[i];
        w->hitCount = 0;
        if (w->hasPending) {
            ctx->catalogBytes   = std::move(w->pending);
            ctx->catalog        = w->pendingView;
            ctx->catalogPresent = true;
            w->pending.init(ctx->alloc, Tag::Io);
            w->hasPending = false;
            newCatalog    = true;
        }
    }
    if (newCatalog) catalog_changed(ctx);
    for (u32 i = 0; i < n; ++i) {
        Slot& s = ctx->slots[w->drained[i].index];
        if (s.live && !s.zombie && s.generation == w->drained[i].generation) reload_slot(ctx, s);
    }
}

void watch_arm(Context* ctx, Slot const& s) noexcept {
    Watch* w = ctx->watch;
    if (!w || ctx->layout == StoreLayout::Catalog) return;
    std::lock_guard<std::mutex> lock(w->mutex);
    WatchEntry& e = w->entries[s.index];
    if (e.generation != s.generation) { // another asset in this slot: forget the old stat
        e.generation = s.generation;
        e.hasStat    = false;
        e.kind       = s.kind;
        e.pathLen    = s.pathLen;
        std::memcpy(e.path, s.path, s.pathLen);
    }
    // No stat this time (the file vanished mid-rewrite): keep watching the last one.
    if (s.jobStatValid) {
        e.stat    = s.jobStat;
        e.hasStat = true;
    }
    e.armed = s.source == SourceKind::File && e.hasStat;
}

void watch_disarm(Context* ctx, u32 index) noexcept {
    Watch* w = ctx->watch;
    if (!w) return;
    std::lock_guard<std::mutex> lock(w->mutex);
    w->entries[index].armed = false;
}

#else // !KILN_HOT_RELOAD

struct Watch {};

void watch_start(Context* ctx, HotReloadDesc const& desc) noexcept {
    if (desc.watchStore)
        (void)diagf(&ctx->diag, make_status(Code::Unsupported), kDiagHotReloadUnavailable, Severity::Warning,
                    {}, "hot reload", "store poller not started: built without KILN_HOT_RELOAD");
}
void watch_stop(Context*) noexcept {}
void watch_free(Context*) noexcept {}
void watch_drain(Context*) noexcept {}
void watch_arm(Context*, Slot const&) noexcept {}
void watch_disarm(Context*, u32) noexcept {}

#endif

} // namespace kiln::rt
