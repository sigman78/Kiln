// context.cpp — create(), destroy(), the cook-provider hook, stats and accessors.
#include "runtime_internal.h"

#include "kiln/placeholders.h"

#include <chrono>

namespace kiln {

char const* runtime_version() noexcept { return "0.0.1-m3"; }

namespace rt {
namespace {

using Clock = std::chrono::steady_clock;

void log_info(Context* ctx, char const* fmt, ...) noexcept KILN_PRINTF(2, 3);
void log_info(Context* ctx, char const* fmt, ...) noexcept {
    char msg[kLogMessageMax];
    va_list args;
    va_start(args, fmt);
    usize const n = vformat(msg, sizeof msg, fmt, args);
    va_end(args);
    if (ctx->log.fn)
        ctx->log.fn(ctx->log.user, LogLevel::Info, "runtime", StrView(msg, n));
    else
        KILN_INFO("runtime", "%s", msg);
}

u64 pow2_at_least(u64 v, u64 lo) noexcept { return std::bit_ceil(max(v, lo)); }

char* copy_str(Allocator const* a, StrView s) noexcept {
    char* p = alloc_array<char>(a, s.size + 1, Tag::Registry);
    if (s.size) std::memcpy(p, s.data, s.size);
    p[s.size] = '\0';
    return p;
}

/// Upload one placeholder image (reserved id) through begin/commit. Retries Busy for
/// up to 10 s. Completion is polled by the caller.
Status upload_placeholder(Context* ctx, u32 index, AssetId id, Format format, u32 w, u32 h,
                          Span<u8 const> pixels) noexcept {
    Placeholder& p = ctx->ph[index];
    p.id           = id;
    if ((format != Format::R8G8B8A8_UNORM && format != Format::R8G8B8A8_SRGB) || w == 0 || h == 0 ||
        pixels.size != usize(w) * h * 4)
        return diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagPlaceholderFailed, Severity::Error,
                     {}, "placeholder", "placeholder %u: need RGBA8 pixels, %ux%u x 4 bytes (got %zu)", index,
                     w, h, pixels.size);
    ktx2::TextureDesc& d = p.desc;
    d                    = {};
    d.format             = format;
    d.width              = w;
    d.height             = h;
    u64 offset = 0, pitch = 0;
    u64 const size =
        texture_layout(d, ctx->cc.optimalRowPitchAlign, ctx->cc.optimalOffsetAlign, &offset, &pitch);
    p.offset = offset;
    p.pitch  = pitch;

    TextureDesc td;
    td.format = format;
    td.width  = w;
    td.height = h;
    UploadDesc ud;
    ud.id        = id;
    ud.kind      = UploadKind::TextureLevels;
    ud.size      = size;
    ud.alignment = u32(max<u64>(ctx->cc.optimalOffsetAlign, 16));
    ud.texture   = &td;

    Adapter const& a = ctx->adapter;
    UploadTarget t;
    Status st           = kOk;
    auto const deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        st = a.begin_upload(a.user, ud, &t);
        if (st.code != Code::Busy || Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (st.failed())
        return diagf(&ctx->diag, st, kDiagPlaceholderFailed, Severity::Error, {}, "placeholder",
                     "begin_upload rejected placeholder %u (%s)", index, code_name(st.code));
    bool const pitchOk = t.rowPitchAlign <= 1 || pitch % t.rowPitchAlign == 0;
    if (t.dst && pitchOk) {
        u8* dst            = static_cast<u8*>(t.dst);
        u64 const rowBytes = u64(w) * 4;
        for (u32 r = 0; r < h; ++r) {
            std::memcpy(dst + r * pitch, pixels.data + r * rowBytes, usize(rowBytes));
            std::memset(dst + r * pitch + rowBytes, 0, usize(pitch - rowBytes));
        }
    }
    a.commit_upload(a.user, t.token);
    if (!t.dst || !pitchOk) {
        a.destroy_deferred(a.user, t.object);
        return diagf(&ctx->diag, make_status(Code::Unsupported), kDiagPlaceholderFailed, Severity::Error, {},
                     "placeholder", "placeholder %u: %s", index,
                     t.dst ? "adapter row pitch alignment differs from copy_constraints"
                           : "no destination memory");
    }
    p.obj     = t.object;
    p.token   = t.token;
    p.pending = true;
    return kOk;
}

Status upload_placeholders(Context* ctx, ContextDesc const& desc) noexcept {
    for (u32 k = 0; k < u32(TextureKind::Count); ++k) {
        PlaceholderDesc const* host = nullptr;
        for (PlaceholderDesc const& pd : desc.placeholders)
            if (u32(pd.kind) == k) host = &pd;
        Status st;
        if (host) {
            st = upload_placeholder(ctx, k, placeholder_asset_id(TextureKind(k)), host->format, host->width,
                                    host->height, host->pixels);
        } else {
            PlaceholderImage const img = builtin_placeholder(TextureKind(k));
            st = upload_placeholder(ctx, k, placeholder_asset_id(TextureKind(k)), img.format, img.width,
                                    img.height, img.pixels);
        }
        if (st.failed()) return st;
    }
    if (ctx->devPlaceholders) {
        PlaceholderImage const img = builtin_failed_placeholder();
        KILN_TRY(upload_placeholder(ctx, kFailedPlaceholder, kFailedPlaceholderId, img.format, img.width,
                                    img.height, img.pixels));
    }
    if (ctx->adapter.caps & kSelfSubmitting) {
        auto const deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            poll_placeholders(ctx);
            bool pending = false;
            for (Placeholder const& p : ctx->ph)
                pending = pending || p.pending;
            if (!pending) break;
            if (Clock::now() >= deadline)
                KILN_PANIC(
                    "K5009 create(): placeholder uploads did not complete within 10 s although the adapter "
                    "sets kSelfSubmitting");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return kOk;
}

void free_tables(Context* ctx) noexcept {
    Allocator const* a = ctx->alloc;
    if (ctx->slots) {
        for (u32 i = 0; i < ctx->maxAssets; ++i)
            ctx->slots[i].~Slot();
        free_array(a, ctx->slots, ctx->maxAssets, Tag::Registry);
    }
    free_array(a, ctx->freeSlots, ctx->maxAssets, Tag::Registry);
    free_array(a, ctx->groups, ctx->maxGroups, Tag::Registry);
    free_array(a, ctx->freeGroups, ctx->maxGroups, Tag::Registry);
    free_array(a, ctx->comp, ctx->compCap, Tag::Registry);
    free_array(a, ctx->compScratch, ctx->compCap, Tag::Registry);
    free_array(a, ctx->events, ctx->maxEvents, Tag::Registry);
    if (ctx->storeDir) free_array(a, ctx->storeDir, ctx->storeDirLen + 1, Tag::Registry);
    free_array(a, ctx->roots, ctx->rootCount, Tag::Registry);
    free_array(a, ctx->rootChars, ctx->rootCharsLen, Tag::Registry);
    ctx->meshMap.release();
    ctx->texMap.release();
}

void teardown(Context* ctx) noexcept {
    Adapter const& a = ctx->adapter;
    // 1. Let in-flight jobs finish (they only touch their slot and the completion ring).
    while (ctx->jobsInFlight.load(std::memory_order_acquire) != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 2. Discard pending completions; their upload objects go back to the adapter.
    {
        std::lock_guard<std::mutex> lock(ctx->compMutex);
        for (u32 i = 0; i < ctx->compCount; ++i) {
            Slot& s       = ctx->slots[ctx->comp[(ctx->compHead + i) % ctx->compCap].slot];
            s.jobInFlight = false;
        }
        ctx->compCount = 0;
    }
    // 3. Release every asset.
    if (ctx->slots) {
        for (u32 i = 0; i < ctx->maxAssets; ++i) {
            Slot& s = ctx->slots[i];
            if (!s.live) continue;
            if (s.hasTarget) a.destroy_deferred(a.user, s.target.object);
            s.hasTarget = false;
            if (!s.zombie && a.publish) a.publish(a.user, s.id, GpuObject{}, s.version);
            if (!s.realObj.is_null()) a.destroy_deferred(a.user, s.realObj);
            s.realObj = {};
            free_load_data(s);
            s.live = false;
        }
    }
    // 4. Placeholders (the only time kiln passes them to destroy_deferred).
    for (Placeholder& p : ctx->ph) {
        if (p.obj.is_null()) continue;
        if (p.ready && a.publish) a.publish(a.user, p.id, GpuObject{}, 1);
        a.destroy_deferred(a.user, p.obj);
        p = {};
    }
    if (ctx->ownsJobs) destroy_thread_pool(ctx->jobs);
    ctx->ownsJobs = false;
}

} // namespace
} // namespace rt

using namespace rt;

Result<Context*> create(ContextDesc const& desc) noexcept {
    if (!desc.adapter || !adapter_is_valid(*desc.adapter))
        return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagPlaceholderFailed, Severity::Error,
                     {}, "create", "ContextDesc::adapter is missing or invalid (adapter_is_valid)");
    if (desc.maxAssets == 0 || desc.maxAssets > (1u << 24) || desc.maxGroups == 0 || desc.maxEvents == 0)
        return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagRegistryFull, Severity::Error, {},
                     "create", "maxAssets / maxGroups / maxEvents out of range");

    Allocator const* a   = desc.alloc ? desc.alloc : default_allocator();
    Context* ctx         = new_object<Context>(a, Tag::Registry);
    ctx->alloc           = a;
    ctx->log             = desc.log;
    ctx->diag            = desc.diag;
    ctx->adapter         = *desc.adapter;
    ctx->devPlaceholders = desc.devPlaceholders;
    ctx->adapter.copy_constraints(ctx->adapter.user, &ctx->cc);
    ctx->cc.optimalRowPitchAlign = pow2_at_least(ctx->cc.optimalRowPitchAlign, 1);
    ctx->cc.optimalOffsetAlign   = pow2_at_least(ctx->cc.optimalOffsetAlign, 1);
    ctx->cc.bufferOffsetAlign    = pow2_at_least(ctx->cc.bufferOffsetAlign, 1);
    ctx->io                      = desc.io ? desc.io : compat_io_backend();

    if (desc.jobs) {
        ctx->jobs = *desc.jobs;
    } else {
        ThreadPoolDesc pd;
        pd.alloc                     = a;
        pd.threads                   = desc.workerThreads;
        pd.priority                  = desc.workerPriority;
        Result<JobSystem> const pool = create_thread_pool(pd);
        if (pool.failed()) {
            delete_object(a, ctx, Tag::Registry);
            return diagf(&desc.diag, pool.status(), kDiagPlaceholderFailed, Severity::Error, {}, "create",
                         "cannot start the built-in thread pool");
        }
        ctx->jobs     = *pool;
        ctx->ownsJobs = true;
    }
    u32 const workers = ctx->ownsJobs ? thread_pool_thread_count(ctx->jobs) : 4;
    ctx->maxIoJobs    = desc.maxIoJobs ? desc.maxIoJobs : max(workers, 1u);
    ctx->ioBudget     = desc.ioInFlightBytes ? desc.ioInFlightBytes : (u64(64) << 20);
    ctx->maxAssets    = desc.maxAssets;
    ctx->maxGroups    = desc.maxGroups;
    ctx->maxEvents    = desc.maxEvents;

    ctx->storeDirLen = desc.storeDir.size;
    ctx->storeDir    = copy_str(a, desc.storeDir);
    if (!desc.sourceRoots.empty()) {
        usize chars = 0;
        for (StrView r : desc.sourceRoots)
            chars += r.size + 1;
        ctx->rootCount    = u32(desc.sourceRoots.size);
        ctx->roots        = alloc_array<StrView>(a, ctx->rootCount, Tag::Registry);
        ctx->rootChars    = alloc_array<char>(a, chars, Tag::Registry);
        ctx->rootCharsLen = chars;
        char* p           = ctx->rootChars;
        for (u32 i = 0; i < ctx->rootCount; ++i) {
            StrView const r = desc.sourceRoots[i];
            if (r.size) std::memcpy(p, r.data, r.size);
            p[r.size]     = '\0';
            ctx->roots[i] = StrView(p, r.size);
            p += r.size + 1;
        }
    }

    // Tables: allocated once here; pump() and the queries never allocate.
    ctx->slots     = alloc_array<Slot>(a, ctx->maxAssets, Tag::Registry);
    ctx->freeSlots = alloc_array<u32>(a, ctx->maxAssets, Tag::Registry);
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot* s           = ::new (static_cast<void*>(ctx->slots + i)) Slot();
        s->ctx            = ctx;
        s->index          = i;
        ctx->freeSlots[i] = ctx->maxAssets - 1 - i; // slot 0 is handed out first
    }
    ctx->freeSlotCount = ctx->maxAssets;
    ctx->meshMap.init(a, Tag::Registry);
    ctx->meshMap.reserve(ctx->maxAssets);
    ctx->texMap.init(a, Tag::Registry);
    ctx->texMap.reserve(ctx->maxAssets);
    ctx->groups     = alloc_array<GroupRec>(a, ctx->maxGroups, Tag::Registry);
    ctx->freeGroups = alloc_array<u32>(a, ctx->maxGroups, Tag::Registry);
    for (u32 i = 0; i < ctx->maxGroups; ++i) {
        ::new (static_cast<void*>(ctx->groups + i)) GroupRec();
        ctx->freeGroups[i] = ctx->maxGroups - 1 - i;
    }
    ctx->freeGroupCount = ctx->maxGroups;
    ctx->compCap        = ctx->maxAssets * 2;
    ctx->comp           = alloc_array<Completion>(a, ctx->compCap, Tag::Registry);
    ctx->compScratch    = alloc_array<Completion>(a, ctx->compCap, Tag::Registry);
    ctx->events         = alloc_array<Event>(a, ctx->maxEvents, Tag::Registry);

    if (Status const st = upload_placeholders(ctx, desc); st.failed()) {
        teardown(ctx);
        free_tables(ctx);
        delete_object(a, ctx, Tag::Registry);
        return st;
    }

    log_info(ctx, "context created: maxAssets %u, %u worker job slot(s), store '%s'", ctx->maxAssets,
             ctx->maxIoJobs, ctx->storeDir);
    return ctx;
}

void destroy(Context* ctx) noexcept {
    if (!ctx) return;
    teardown(ctx);
    log_info(ctx, "context destroyed");
    Allocator const* a = ctx->alloc;
    free_tables(ctx);
    delete_object(a, ctx, Tag::Registry);
}

void set_cook_provider(Context* ctx, CookProvider const& provider) noexcept {
    if (ctx) ctx->provider = provider; // pump thread; snapshotted per load at dispatch
}
CookProvider cook_provider(Context* ctx) noexcept { return ctx ? ctx->provider : CookProvider{}; }

ContextStats stats(Context* ctx) noexcept {
    ContextStats st;
    if (!ctx) return st;
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot const& s = ctx->slots[i];
        if (!s.live || s.zombie) continue;
        ++st.assets;
        switch (s.state) {
        case State::Pending: ++st.pending; break;
        case State::MetaReady: ++st.metaReady; break;
        case State::Ready: ++st.ready; break;
        case State::Failed: ++st.failed; break;
        default: break;
        }
    }
    for (u32 i = 0; i < ctx->maxGroups; ++i)
        st.groups += ctx->groups[i].live ? 1u : 0u;
    st.ioJobsInFlight  = ctx->jobsOutstanding;
    st.ioBytesInFlight = ctx->ioBytesInFlight.load(std::memory_order_relaxed);
    st.uploadsInFlight = ctx->queues[u32(QueueId::Await)].count;
    return st;
}

StrView store_dir(Context* ctx) noexcept {
    return ctx ? StrView(ctx->storeDir, ctx->storeDirLen) : StrView{};
}
Span<StrView const> source_roots(Context* ctx) noexcept {
    return ctx ? Span<StrView const>(ctx->roots, ctx->rootCount) : Span<StrView const>{};
}
Allocator const* allocator(Context* ctx) noexcept { return ctx ? ctx->alloc : nullptr; }
JobSystem const* jobs(Context* ctx) noexcept { return ctx ? &ctx->jobs : nullptr; }
Adapter const* adapter(Context* ctx) noexcept { return ctx ? &ctx->adapter : nullptr; }

} // namespace kiln
