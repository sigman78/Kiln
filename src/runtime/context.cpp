// context.cpp — create(), destroy(), the cook-provider hook, stats and accessors.
#include "runtime_internal.h"

#include "kiln/placeholders.h"

#include <chrono>

namespace kiln {

namespace rt {
namespace {

using Clock = std::chrono::steady_clock;

void log_info(Context* ctx, char const* fmt, ...) KILN_PRINTF(2, 3);
void log_info(Context* ctx, char const* fmt, ...) {
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

u64 pow2_at_least(u64 v, u64 lo) { return std::bit_ceil(max(v, lo)); }

char* copy_str(Allocator const* a, StrView s) {
    char* p = alloc_array<char>(a, s.size + 1, Tag::Registry);
    if (s.size) std::memcpy(p, s.data, s.size);
    p[s.size] = '\0';
    return p;
}

/// Upload one placeholder image (reserved id) through begin/commit: `pixels` in every face or
/// layer of `shape` (a cube has 6, an array 1). Retries Busy for up to 10 s. Completion is
/// polled by the caller.
Status upload_placeholder(Context* ctx, u32 index, AssetId id, TextureShape shape, Format format, u32 w,
                          u32 h, Span<u8 const> pixels) {
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
    d.faces              = shape == TextureShape::Cube ? 6 : 1;
    d.isCube             = shape == TextureShape::Cube;
    d.isArray            = shape == TextureShape::Array;
    u32 const slices     = d.layers * d.faces;
    u64 offset = 0, pitch = 0;
    u64 const size =
        texture_layout(d, ctx->cc.optimalRowPitchAlign, ctx->cc.optimalOffsetAlign, &offset, &pitch);
    p.offset = offset;
    p.pitch  = pitch;

    TextureDesc td;
    td.format = format;
    td.width  = w;
    td.height = h;
    td.layers = slices;
    td.shape  = shape;
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
        for (u32 r = 0; r < h * slices; ++r) {
            std::memcpy(dst + r * pitch, pixels.data + (r % h) * rowBytes, usize(rowBytes));
            std::memset(dst + r * pitch + rowBytes, 0, usize(pitch - rowBytes));
        }
    }
    if (!t.dst || !pitchOk) {
        if (a.discard_upload) {
            a.discard_upload(a.user, t.token);
        } else {
            a.commit_upload(a.user, t.token);
            ctx->orphans.push_back({t.token, t.object});
        }
        return diagf(&ctx->diag, make_status(Code::Unsupported), kDiagPlaceholderFailed, Severity::Error, {},
                     "placeholder", "placeholder %u: %s", index,
                     t.dst ? "adapter row pitch alignment differs from copy_constraints"
                           : "no destination memory");
    }
    a.commit_upload(a.user, t.token);
    p.obj     = t.object;
    p.token   = t.token;
    p.pending = true;
    return kOk;
}

Status upload_placeholders(Context* ctx, ContextDesc const& desc) {
    for (u32 sh = 0; sh < u32(TextureShape::Count); ++sh) {
        TextureShape const shape = TextureShape(sh);
        if (!shape_supported(ctx, shape)) continue;
        for (u32 k = 0; k < u32(TextureKind::Count); ++k) {
            PlaceholderDesc const* host = nullptr;
            for (PlaceholderDesc const& pd : desc.placeholders)
                if (u32(pd.kind) == k) host = &pd;
            PlaceholderImage img = builtin_placeholder(TextureKind(k));
            if (host) img = {host->format, host->width, host->height, host->pixels};
            KILN_TRY(upload_placeholder(ctx, placeholder_index(TextureKind(k), shape),
                                        placeholder_asset_id(TextureKind(k), shape), shape, img.format,
                                        img.width, img.height, img.pixels));
        }
        if (ctx->devPlaceholders) {
            PlaceholderImage const img = builtin_failed_placeholder();
            KILN_TRY(upload_placeholder(ctx, failed_placeholder_index(shape), failed_placeholder_id(shape),
                                        shape, img.format, img.width, img.height, img.pixels));
        }
    }
    if ((ctx->adapter.caps & kSelfSubmitting) || ctx->adapter.flush) {
        auto const deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            if (ctx->adapter.flush) ctx->adapter.flush(ctx->adapter.user);
            poll_placeholders(ctx);
            bool pending = false;
            for (Placeholder const& p : ctx->ph)
                pending = pending || p.pending;
            if (!pending) break;
            if (Clock::now() >= deadline)
                KILN_PANIC(
                    "K5009 create(): placeholder uploads did not complete within 10 s although the adapter "
                    "sets kSelfSubmitting or flush");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (Placeholder const& p : ctx->ph)
            if (p.failed) return make_status(Code::Unknown); // K5009 already reported
    }
    return kOk;
}

void free_tables(Context* ctx) {
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
    if (ctx->profile) free_array(a, ctx->profile, ctx->profileLen + 1, Tag::Registry);
    ctx->manifestBytes.release();
    free_array(a, ctx->roots, ctx->rootCount, Tag::Registry);
    free_array(a, ctx->rootChars, ctx->rootCharsLen, Tag::Registry);
    ctx->meshMap.release();
    ctx->texMap.release();
    ctx->retired.release();
    ctx->posted.release();
    ctx->postedDrain.release();
    ctx->orphans.release();
    ctx->freeBindSlots.release();
    watch_free(ctx);
}

void teardown(Context* ctx) {
    Adapter const& a = ctx->adapter;
    // 1. Drop the prepared jobs no worker took, and let the running ones finish (they only touch
    //    their slot, the ready lists and the completion ring).
    {
        std::lock_guard<std::mutex> lock(ctx->readyMutex);
        for (List& l : ctx->ready)
            l = {};
    }
    // Polled, not atomic::wait: a job's notify after its decrement could reach a freed context.
    while (ctx->jobsInFlight.load(std::memory_order_acquire) != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 2. Discard pending completions; their upload objects go back to the adapter.
    {
        std::lock_guard<std::mutex> lock(ctx->compMutex);
        ctx->compCount = 0;
    }
    // 3. Release every asset at once (the host waited for its GPU to go idle); abandoned uploads
    //    become orphans.
    ctx->completedFrame = ~u64(0);
    process_retired(ctx);
    if (ctx->slots) {
        for (u32 i = 0; i < ctx->maxAssets; ++i) {
            Slot& s = ctx->slots[i];
            if (!s.live()) continue;
            orphan_upload(ctx, s);
            if (!s.realObj.is_null()) a.destroy(a.user, s.realObj);
            s.realObj = {};
            free_load_data(s);
            free_array_decl(ctx->alloc, s.array);
            s.array = nullptr;
            if (!s.zombie()) transition(s, Step::Unload);
            transition(s, Step::Free);
        }
    }
    for (Placeholder& p : ctx->ph) {
        if (p.pending)
            ctx->orphans.push_back({p.token, p.obj});
        else if (!p.obj.is_null())
            a.destroy(a.user, p.obj);
        p = {};
    }
    // 4. Orphans: wait for their uploads where the adapter can finish them without the host.
    if ((a.caps & kSelfSubmitting) || a.flush) {
        auto const deadline = Clock::now() + std::chrono::seconds(10);
        while (!ctx->orphans.empty() && Clock::now() < deadline) {
            if (a.flush) a.flush(a.user);
            poll_orphans(ctx);
            if (!ctx->orphans.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (Orphan const& o : ctx->orphans)
        a.destroy(a.user, o.obj);
    ctx->orphans.clear();
    if (ctx->ownsJobs) destroy_thread_pool(ctx->jobs);
    ctx->ownsJobs = false;
}

} // namespace
namespace {

/// A store with a profile the adapter cannot fully sample is a configuration error: one report
/// here instead of a failure per asset (docs/design/target-profiles.md). With `allow`, a warning.
Status check_formats(Adapter const& adapter, DiagSink const* diag, StrView storeDir, StrView profile,
                     u64 blockFormats, bool allow) {
    char missing[512] = {};
    usize at          = 0;
    for (u32 v = u32(Format::BC1_RGB_UNORM); v <= u32(Format::ASTC_12x12_SRGB); ++v) {
        Format const f = Format(v);
        if (!(blockFormats & block_format_bit(f))) continue;
        if (adapter.supports_format(adapter.user, f, FormatUsage::SampledImage)) continue;
        FormatInfo const* info = format_info(f);
        at += format(missing + at, sizeof missing - at, "%s%s", at ? " " : "", info ? info->name : "?");
    }
    if (at == 0) return kOk;
    Status const out = diagf(diag, allow ? kOk : make_status(Code::Unsupported), kDiagStoreProfileUnsampled,
                             allow ? Severity::Warning : Severity::Error, storeDir, "store",
                             "the store's profile '%.*s' has formats the adapter cannot sample: %s",
                             KILN_SV(profile), missing);
    return allow ? kOk : out;
}

/// A manifest without the context's profile: requests miss until a cook writes it. One warning
/// that names the profiles it has, since a wrong profile name looks the same.
void warn_profile_missing(ContextDesc const& desc, ManifestView const& v) {
    char have[256] = {};
    usize at       = 0;
    for (u32 i = 0; i < v.profile_count() && at < sizeof have; ++i)
        at += format(have + at, sizeof have - at, "%s'%.*s'", i ? ", " : "", KILN_SV(v.profile(i).name()));
    (void)diagf(
        &desc.diag, kOk, kDiagManifestMissing, Severity::Warning, desc.storeDir, "create",
        "the store's manifest has no profile '%.*s' (it has %s): requests miss until a cook writes it",
        KILN_SV(desc.profile), at ? have : "none");
}

/// Reads and validates the store's manifest into `bytes`. A missing manifest (or no store) is no
/// error: a cook provider or kiln-cook may write it later.
Status load_manifest(ContextDesc const& desc, Allocator const* a, Vec<u8>* bytes, ManifestView* view) {
    if (char const* why = check_profile_name(desc.profile))
        return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagBadAssetName, Severity::Error,
                     desc.profile, "create", "invalid profile name: %s", why);
    if (desc.storeDir.empty()) return kOk;
    char path[1024];
    usize const n = manifest_file_path(desc.storeDir, path, sizeof path);
    if (n >= sizeof path - 1)
        return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagManifestMissing, Severity::Error,
                     desc.storeDir, "create", "storeDir is too long (%zu bytes)", desc.storeDir.size);
    IoBackend const* io = desc.io ? desc.io : compat_io_backend();
    Status const st     = io_read_file(io, StrView(path, n), a, bytes);
    if (st.code == Code::NotFound) return kOk;
    if (st.failed())
        return diagf(&desc.diag, st, kDiagManifestMissing, Severity::Error, StrView(path, n), "create",
                     "cannot read the manifest");
    Result<ManifestView> v = ManifestView::open(bytes->span(), &desc.diag, StrView(path, n));
    if (v.failed()) return v.status();
    *view = *v;
    ManifestProfile p;
    if (!v->find_profile(desc.profile, &p)) {
        warn_profile_missing(desc, *v);
        return kOk;
    }
    return check_formats(*desc.adapter, &desc.diag, desc.storeDir, p.name(), p.block_formats(),
                         desc.allowUnsampledFormats);
}

} // namespace

void adopt_manifest(Context* ctx, Vec<u8>&& bytes, ManifestView const& v) {
    ctx->manifestBytes   = std::move(bytes); // the view's bytes stay where they are
    ctx->manifestPresent = v.find_profile(StrView(ctx->profile, ctx->profileLen), &ctx->manifest);
    // The profile appeared after create(): check its formats now, as a warning (each asset whose
    // format the adapter cannot sample fails on its own).
    if (ctx->manifestPresent && !ctx->formatsChecked) {
        ctx->formatsChecked = true;
        (void)check_formats(ctx->adapter, &ctx->diag, StrView(ctx->storeDir, ctx->storeDirLen),
                            ctx->manifest.name(), ctx->manifest.block_formats(), true);
    }
}

void refresh_manifest(Context* ctx) {
    if (ctx->watch || ctx->storeDirLen == 0) return;
    char path[1024];
    usize const n = manifest_path(ctx, path, sizeof path);
    if (n + 1 >= sizeof path) return;
    Vec<u8> bytes(ctx->alloc, Tag::Registry);
    if (io_read_file(ctx->io, StrView(path, n), ctx->alloc, &bytes).failed()) return;
    // The stored checksum tells whether it changed; a rewrite may keep the size and the time.
    if (!ctx->manifestBytes.empty() && bytes.size() >= kManifestHeaderBytes &&
        std::memcmp(bytes.data() + kManifestChecksumOffset,
                    ctx->manifestBytes.data() + kManifestChecksumOffset, 16) == 0)
        return;
    Result<ManifestView> v = ManifestView::open(bytes.span(), &ctx->diag, StrView(path, n));
    if (v.failed()) return; // the one in use stays
    adopt_manifest(ctx, std::move(bytes), *v);
}

} // namespace rt

using namespace rt;

Result<Context*> create(ContextDesc const& desc) {
    if (!desc.adapter || !adapter_is_valid(*desc.adapter))
        return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagPlaceholderFailed, Severity::Error,
                     {}, "create", "ContextDesc::adapter is missing or invalid (adapter_is_valid)");
    if (desc.maxAssets == 0 || desc.maxAssets > (1u << 24) || desc.maxGroups == 0 || desc.maxEvents == 0)
        return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagRegistryFull, Severity::Error, {},
                     "create", "maxAssets / maxGroups / maxEvents out of range");
    for (usize i = 0; i < desc.roots.size; ++i) {
        StrView const name = desc.roots[i].name;
        if (char const* why = name.empty() ? nullptr : check_root_name(name))
            return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagBadAssetName, Severity::Error,
                         name, "create", "invalid root name: %s", why);
        for (usize j = 0; j < i; ++j)
            if (desc.roots[j].name == name)
                return diagf(&desc.diag, make_status(Code::InvalidArgument), kDiagBadAssetName,
                             Severity::Error, name, "create", "root '%.*s' is given twice", KILN_SV(name));
    }

    Allocator const* a = desc.alloc ? desc.alloc : default_allocator();
    Vec<u8> manifestBytes(a, Tag::Registry);
    ManifestView manifest;
    KILN_TRY(load_manifest(desc, a, &manifestBytes, &manifest));

    Context* ctx         = new_object<Context>(a, Tag::Registry);
    ctx->alloc           = a;
    ctx->log             = desc.log;
    ctx->diag            = desc.diag;
    ctx->profileHooks    = desc.profiler;
    ctx->prof            = desc.profiler.zone_begin || desc.profiler.interval ? &ctx->profileHooks : nullptr;
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

    ctx->storeDirLen    = desc.storeDir.size;
    ctx->storeDir       = copy_str(a, desc.storeDir);
    ctx->profileLen     = desc.profile.size;
    ctx->profile        = copy_str(a, desc.profile);
    ctx->formatsChecked = true; // load_manifest() checked the profile, if it is there
    if (!manifestBytes.empty()) adopt_manifest(ctx, std::move(manifestBytes), manifest);
    ctx->formatsChecked = ctx->manifestPresent;
    if (!desc.roots.empty()) {
        usize chars = 0;
        for (Root const& m : desc.roots)
            chars += m.name.size + 1 + m.dir.size + 1;
        ctx->rootCount    = u32(desc.roots.size);
        ctx->roots        = alloc_array<Root>(a, ctx->rootCount, Tag::Registry);
        ctx->rootChars    = alloc_array<char>(a, chars, Tag::Registry);
        ctx->rootCharsLen = chars;
        char* p           = ctx->rootChars;
        auto const copy   = [&p](StrView s) {
            if (s.size) std::memcpy(p, s.data, s.size);
            p[s.size] = '\0';
            StrView const v(p, s.size);
            p += s.size + 1;
            return v;
        };
        for (u32 i = 0; i < ctx->rootCount; ++i) {
            ctx->roots[i].name = copy(desc.roots[i].name);
            ctx->roots[i].dir  = copy(desc.roots[i].dir);
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
    // Grow only when the host lets many frames' worth of drops pile up.
    ctx->retired.init(a, Tag::Registry);
    ctx->posted.init(a, Tag::Registry);
    ctx->postedDrain.init(a, Tag::Registry);
    ctx->retired.reserve(ctx->maxAssets);
    ctx->orphans.init(a, Tag::Registry);
    ctx->orphans.reserve(ctx->maxAssets);
    ctx->freeBindSlots.init(a, Tag::Registry);
    ctx->freeBindSlots.reserve(ctx->maxAssets);
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

    watch_start(ctx, desc.hotReload);

    log_info(ctx, "context created: maxAssets %u, %u worker job slot(s), store '%s'", ctx->maxAssets,
             ctx->maxIoJobs, ctx->storeDir);
    return ctx;
}

void destroy(Context* ctx) {
    if (!ctx) return;
    watch_stop(ctx); // joins the poller before any table is freed
    teardown(ctx);
    // After teardown: no load job can still call into the provider.
    if (ctx->provider.release) ctx->provider.release(ctx->provider.user);
    ctx->provider = {};
    log_info(ctx, "context destroyed");
    Allocator const* a = ctx->alloc;
    free_tables(ctx);
    delete_object(a, ctx, Tag::Registry);
}

void set_cook_provider(Context* ctx, CookProvider const& provider) {
    if (!ctx) return;
    bool const replaced = ctx->provider.prepare &&
                          (ctx->provider.prepare != provider.prepare || ctx->provider.user != provider.user);
    ctx->provider = provider; // pump thread; snapshotted per load at dispatch
    // Loads in flight hold the old one. Polled as in teardown(); the completion ring never fills.
    while (replaced && ctx->jobsInFlight.load(std::memory_order_acquire) != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
CookProvider cook_provider(Context* ctx) { return ctx ? ctx->provider : CookProvider{}; }

ContextStats stats(Context* ctx) {
    ContextStats st;
    if (!ctx) return st;
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot const& s = ctx->slots[i];
        if (!s.live() || s.zombie()) continue;
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

StrView store_dir(Context* ctx) { return ctx ? StrView(ctx->storeDir, ctx->storeDirLen) : StrView{}; }
StrView store_profile(Context* ctx) {
    return ctx && ctx->profile ? StrView(ctx->profile, ctx->profileLen) : StrView{};
}
Span<Root const> roots(Context* ctx) {
    return ctx ? Span<Root const>(ctx->roots, ctx->rootCount) : Span<Root const>{};
}
Allocator const* allocator(Context* ctx) { return ctx ? ctx->alloc : nullptr; }
DiagSink const* diag_sink(Context* ctx) { return ctx ? &ctx->diag : nullptr; }
JobSystem const* jobs(Context* ctx) { return ctx ? &ctx->jobs : nullptr; }
Adapter const* adapter(Context* ctx) { return ctx ? &ctx->adapter : nullptr; }
ProfileHooks const* profile_hooks(Context* ctx) { return ctx ? ctx->prof : nullptr; }

} // namespace kiln

namespace kiln {

// A host's reload may follow an edit the provider has not seen: the load checks the sources again.
// A reload after a manifest change does not: the store's writer checked them.
void post_reload(Context* ctx, AssetKind kind, StrView name) {
    AssetId const id = asset_id(name);
    if (!ctx || !id) return;
    std::lock_guard<std::mutex> const lock(ctx->postMutex);
    ctx->posted.push_back({id, kind});
}

void request_reload(Context* ctx, MeshHandle h) {
    if (rt::Slot* s = rt::resolve(ctx, h.bits(), AssetKind::Mesh)) {
        s->recheck = true;
        rt::reload_slot(ctx, *s);
    }
}
void request_reload(Context* ctx, TextureHandle h) {
    if (rt::Slot* s = rt::resolve(ctx, h.bits(), AssetKind::Texture)) {
        s->recheck = true;
        rt::reload_slot(ctx, *s);
    }
}

void set_texture_extent(Context* ctx, TextureHandle h, u32 maxExtent) {
    rt::Slot* s = rt::resolve(ctx, h.bits(), AssetKind::Texture);
    if (!s || s->maxExtent == maxExtent) return;
    s->maxExtent = maxExtent;
    rt::resize_slot(ctx, *s);
}

} // namespace kiln
