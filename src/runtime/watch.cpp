// watch.cpp — the store poller: when the manifest changes, swap it in and reload the assets
// whose entry changed (docs/design/hot-reload.md). Compiled in only with KILN_HOT_RELOAD; without
// it, asking for the poller is K5011 and every hook is a no-op.
#include "runtime_internal.h"

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD
#include <chrono>
#include <condition_variable>
#endif

namespace kiln::rt {

#if defined(KILN_HOT_RELOAD) && KILN_HOT_RELOAD

/// The poller thread never reads Slot or other registry state: it hands a new manifest to the pump
/// thread under `mutex`.
struct Watch {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop  = false; ///< under mutex
    u32 pollMs = 250;
    std::thread thread;

    IoStat manifestStat;            ///< poller thread only: the manifest as last read
    bool manifestStatValid = false; ///< poller thread only
    Hash128 manifestSum;            ///< under mutex: checksum of the manifest in use (or pending)
    Vec<u8> pending;                ///< under mutex: a new manifest for the pump thread
    ManifestView pendingView;       ///< under mutex: views `pending`
    bool hasPending = false;        ///< under mutex
};

namespace {

bool same_stat(IoStat const& a, IoStat const& b) noexcept {
    return a.size == b.size && a.mtimeNs == b.mtimeNs;
}

/// The checksum stored in a validated manifest.
Hash128 checksum_of(Span<u8 const> manifest) noexcept {
    Hash128 h;
    std::memcpy(h.bytes, manifest.data + kManifestChecksumOffset, sizeof h.bytes);
    return h;
}

/// Reads the manifest again if its stat changed; a valid one with a new checksum becomes pending.
void poll_manifest(Context* ctx, Watch& w) noexcept {
    IoBackend const* io = ctx->io;
    char path[1024];
    usize const n = manifest_path(ctx, path, sizeof path);
    if (n + 1 >= sizeof path) return;
    IoStat now;
    // Missing (not written yet, or a rename in progress): look again next round.
    if (io->stat(io->user, StrView(path, n), &now).failed()) return;
    if (w.manifestStatValid && same_stat(now, w.manifestStat)) return;
    Vec<u8> bytes(ctx->alloc, Tag::Io);
    if (io_read_file(io, StrView(path, n), ctx->alloc, &bytes).failed()) return;
    w.manifestStat               = now;
    w.manifestStatValid          = true;
    Result<ManifestView> const v = ManifestView::open(bytes.span(), nullptr, StrView(path, n));
    if (v.failed()) {
        KILN_WARN("reload", "%s changed but is not a valid manifest (%s); keeping the one in use", path,
                  code_name(v.status().code));
        return;
    }
    Hash128 const sum = checksum_of(bytes.span());
    std::lock_guard<std::mutex> lock(w.mutex);
    if (sum == w.manifestSum) return;
    w.manifestSum = sum;
    w.pending     = std::move(bytes); // the view's bytes stay where they are
    w.pendingView = *v;
    w.hasPending  = true;
}

/// A new manifest is in use: reload each asset whose entry names another artifact than the one it
/// loaded or tried. An asset that left the catalog stays as it is.
void catalog_changed(Context* ctx) noexcept {
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot& s = ctx->slots[i];
        if (!s.live || s.zombie || s.source != SourceKind::File) continue;
        // A load in flight learns its key only when it completes (the provider may name it): compare then.
        if (s.phase != Phase::Done)
            s.catalogCheck = true;
        else if (catalog_names_other(ctx, s))
            reload_slot(ctx, s);
    }
}

void poll_main(Context* ctx) noexcept {
    Watch& w = *ctx->watch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(w.mutex);
            if (w.wake.wait_for(lock, std::chrono::milliseconds(w.pollMs), [&] { return w.stop; })) return;
        }
        poll_manifest(ctx, w);
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
    w->pollMs          = max(desc.pollMs, 1u);
    w->pending.init(a, Tag::Io);
    if (!ctx->manifestBytes.empty()) w->manifestSum = checksum_of(ctx->manifestBytes.span());
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
    w->pending.release();
    delete_object(a, w, Tag::Registry);
    ctx->watch = nullptr;
}

void watch_drain(Context* ctx) noexcept {
    Watch* w = ctx->watch;
    if (!w) return;
    bool newManifest = false;
    {
        std::lock_guard<std::mutex> lock(w->mutex);
        if (w->hasPending) {
            adopt_manifest(ctx, std::move(w->pending), w->pendingView);
            w->pending.init(ctx->alloc, Tag::Io);
            w->hasPending = false;
            newManifest   = true;
        }
    }
    if (newManifest) catalog_changed(ctx);
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

#endif

} // namespace kiln::rt
