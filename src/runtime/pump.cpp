// pump.cpp — pump(), events() and wait(). Pump thread only; never allocates.
#include "runtime_internal.h"

#include "kiln/placeholders.h"

#include <chrono>

namespace kiln {
namespace rt {

void push_event(Context* ctx, EventKind kind, AssetKind asset, u64 bits, u32 version, Status st) {
    if (ctx->maxEvents == 0) return;
    if (ctx->eventCount == ctx->maxEvents) { // drop the oldest
        std::memmove(static_cast<void*>(ctx->events), static_cast<void const*>(ctx->events + 1),
                     sizeof(Event) * (ctx->eventCount - 1));
        --ctx->eventCount;
        ++ctx->cur.eventsDropped;
        if (!ctx->droppedWarned) {
            ctx->droppedWarned = true;
            (void)diagf(&ctx->diag, make_status(Code::Busy), kDiagEventsDropped, Severity::Warning, {},
                        "events", "event buffer full (maxEvents = %u); dropping the oldest events",
                        ctx->maxEvents);
        }
    }
    Event& e  = ctx->events[ctx->eventCount++];
    e.kind    = kind;
    e.asset   = asset;
    e.handle  = bits;
    e.version = version;
    e.status  = st;
}

namespace {

char const* failure_text(u32 code) {
    switch (code) {
    case kDiagStoreMiss: return "not in the store";
    case kDiagCookOnMissFailed: return "cook on miss failed";
    case kDiagAdapterRejected: return "adapter rejected the asset";
    case kDiagTextureShapeMismatch: return "texture shape mismatch";
    case kDiagArrayDeclaration: return "texture array does not fit";
    case kDiagArrayLayerMismatch: return "texture array layers differ";
    default: return "load failed";
    }
}

/// A slot reached Ready or Failed with no job and no queue: run the reload requested meanwhile, or
/// the size change the current object does not have yet.
void settle(Context* ctx, Slot& s) {
    if (s.manifestCheck) {
        s.manifestCheck = false;
        if (manifest_names_other(ctx, s)) s.reloadPending = true;
    }
    if (s.reloadPending) {
        s.reloadPending = false;
        reload_slot(ctx, s);
        return;
    }
    if (s.kind == AssetKind::Texture && s.state == State::Ready && s.maxExtent != s.cur.texExtent)
        resize_slot(ctx, s);
}

/// A reload of a Ready slot failed: drop the new metadata and object, keep serving the
/// current version. One K5010, no event. A want the failed load tried is dropped, so that settle()
/// does not try it again; a newer one stays.
void fail_reload(Context* ctx, Slot& s, u32 code, Status st) {
    bool const resize = s.resizing;
    if (s.maxExtent == s.in.maxExtent) s.maxExtent = s.cur.texExtent;
    queue_remove(ctx, s);
    orphan_upload(ctx, s);
    transition(s, Step::Fail);
    remember_failed_keys(s);
    free_meta_set(ctx->alloc, s.out.next);
    s.out.cooked.release();
    s.out.cookedValid = false;
    if (s.array) free_array_job_data(ctx->alloc, *s.array);
    (void)diagf(&ctx->diag, st, kDiagReloadFailed, Severity::Error, path_of(s),
                s.kind == AssetKind::Mesh ? "mesh" : "texture", "%s failed, keeping version %u: %s (%s)%s%s",
                resize ? "size change" : "reload", s.version, failure_text(code), code_name(st.code),
                s.out.capture.set ? ": " : "", s.out.capture.set ? s.out.capture.msg : "");
    ++ctx->cur.completed;
    settle(ctx, s);
}

} // namespace

void fail_slot(Context* ctx, Slot& s, u32 code, Status st) {
    if (st.ok()) st = make_status(Code::Unknown);
    if (s.reloading && s.state == State::Ready) {
        fail_reload(ctx, s, code, st);
        return;
    }
    queue_remove(ctx, s);
    orphan_upload(ctx, s);
    if (!s.reloading) { // a reload from Failed: the group already counts the slot as failed
        if (GroupRec* g = group_of(ctx, s)) {
            --g->pending;
            ++g->failed;
            g->bytesTotal -= s.groupBytes; // a failed member contributes no bytes
        }
        s.groupBytes = 0;
        s.groupAs    = State::Failed;
    }
    transition(s, Step::Fail);
    adopt_job_keys(s); // the job is done: its fields are the pump thread's again

    (void)diagf(&ctx->diag, st, code, Severity::Error, path_of(s),
                s.kind == AssetKind::Mesh ? "mesh" : "texture", "%s (%s)%s%s", failure_text(code),
                code_name(st.code), s.out.capture.set ? ": " : "",
                s.out.capture.set ? s.out.capture.msg : "");

    push_event(ctx, EventKind::Failed, s.kind, handle_bits(s), s.version, st);
    ++ctx->cur.completed;
    bind_placeholder(ctx, s); // with devPlaceholders, the Failed checker
    free_load_data(s);        // Failed holds no metadata
    settle(ctx, s);
}

namespace {

/// An entry is new to a slot or layer if neither its loaded version nor a failed reload used it.
bool key_is_new(Hash128 const& entry, bool keyValid, Hash128 const& key, bool failedValid,
                Hash128 const& failed) {
    return !(keyValid && key == entry) && !(failedValid && failed == entry);
}

} // namespace

bool manifest_names_other(Context const* ctx, Slot const& s) {
    ManifestEntry e;
    if (!ctx->manifestPresent) return false;
    if (!s.array)
        return !s.providerOwned && ctx->manifest.find(s.kind, path_of(s), &e) &&
               key_is_new(e.key, s.keyValid, s.key, s.failedKeyValid, s.failedKey);
    for (u32 i = 0; i < s.array->count; ++i) {
        ArrayLayer const& l = s.array->layers[i];
        if (!l.providerOwned && ctx->manifest.find(AssetKind::Texture, s.array->name(l), &e) &&
            key_is_new(e.key, l.keyValid, l.key, l.failedKeyValid, l.failedKey))
            return true;
    }
    return false;
}

namespace {

void start_reload(Context* ctx, Slot& s) {
    KILN_ASSERT(s.queue == QueueId::None);
    refresh_manifest(ctx);
    transition(s, Step::Reload);
    s.retryAfter = 0;
    s.out.capture.reset();
    s.out.cooked.release(); // look the name up again (or re-cook), never the last load's cook output
    s.out.cookedValid = false;
    if (s.array) free_array_job_data(ctx->alloc, *s.array);
    queue_push(ctx, s.priority == Priority::High ? QueueId::MetaHigh : QueueId::MetaNormal, s);
}

} // namespace

void reload_slot(Context* ctx, Slot& s) {
    if (s.source == SourceKind::Memory) {
        (void)diagf(&ctx->diag, make_status(Code::Unsupported), kDiagReloadMemorySource, Severity::Warning,
                    path_of(s), "reload", "registered in memory: there is no file to reload from");
        return;
    }
    if (s.phase == Phase::MetaQueued && s.resizing) { // a size change not submitted yet takes the reload
        s.resizing = false;
        refresh_manifest(ctx);
        return;
    }
    if (s.phase != Phase::Done) { // queued, loading or awaiting the GPU: runs once it settles
        s.reloadPending = true;
        return;
    }
    s.resizing = false;
    start_reload(ctx, s);
}

void resize_slot(Context* ctx, Slot& s) {
    // A loading slot resizes at settle(); a Failed one at its next reload.
    if (s.phase != Phase::Done || s.state != State::Ready) return;
    if (texture_first_level(s.cur.texDesc, s.maxExtent) == s.cur.texFirstLevel) { // the same levels
        s.cur.texExtent = s.maxExtent;
        return;
    }
    if (s.source == SourceKind::Memory) { // the bytes went with the first upload (R30 h)
        s.maxExtent = s.cur.texExtent;
        (void)diagf(&ctx->diag, make_status(Code::Unsupported), kDiagReloadMemorySource, Severity::Warning,
                    path_of(s), "size change", "registered in memory: its bytes are on the GPU only");
        return;
    }
    s.resizing = true;
    start_reload(ctx, s);
}

void drain_posted_reloads(Context* ctx) {
    {
        std::lock_guard<std::mutex> const lock(ctx->postMutex);
        if (ctx->posted.empty()) return;
        std::swap(ctx->posted, ctx->postedDrain); // both keep their capacity
    }
    // A name posted twice in one batch (a request and its unit's output) reloads once.
    u64 const batch   = ctx->pumpIndex;
    auto const reload = [ctx, batch](Slot& s) {
        if (!s.live() || s.zombie() || s.source == SourceKind::Memory || s.postedBatch == batch) return;
        s.postedBatch = batch;
        s.recheck     = true;
        reload_slot(ctx, s);
    };
    bool arrays = false;
    for (PostedReload const& r : ctx->postedDrain) {
        if (u32 const* i = map_for(ctx, r.kind).find(r.id)) reload(ctx->slots[*i]);
        arrays |= r.kind == AssetKind::Texture;
    }
    // A layer is not a slot of its own: find the arrays that use it.
    for (u32 i = 0; arrays && i < ctx->maxAssets; ++i) {
        Slot& s = ctx->slots[i];
        if (!s.live() || !s.array) continue;
        for (u32 l = 0; l < s.array->count; ++l) {
            AssetId const id = asset_id(s.array->name(s.array->layers[l]));
            bool posted      = false;
            for (PostedReload const& r : ctx->postedDrain)
                posted |= r.kind == AssetKind::Texture && r.id == id;
            if (!posted) continue;
            reload(s);
            break;
        }
    }
    ctx->postedDrain.clear();
}

void submit_stage(Context* ctx, Slot& s, Stage stage) {
    KILN_ASSERT(s.queue == QueueId::None);
    transition(s, stage == Stage::Meta ? Step::SubmitMeta : Step::SubmitUpload);
    s.in.stage = stage;
    s.in.gen   = s.generation;
    if (stage == Stage::Meta) {
        s.in.recheck        = s.recheck;
        s.recheck           = false;
        s.in.maxExtent      = s.maxExtent;
        s.in.provider       = ctx->provider;
        s.out.keyValid      = false;
        s.out.providerOwned = false;
        // The artifact is chosen here, so a manifest swapped in later leaves this load alone.
        ManifestEntry e;
        if (s.source == SourceKind::File && !s.out.cookedValid && ctx->manifestPresent &&
            ctx->manifest.find(s.kind, path_of(s), &e)) {
            s.out.key      = e.key;
            s.out.keyValid = true;
        }
        for (u32 i = 0; s.array && i < s.array->count; ++i) {
            ArrayLayer& l      = s.array->layers[i];
            l.jobKeyValid      = false;
            l.jobProviderOwned = false;
            if (!l.cookedValid && ctx->manifestPresent &&
                ctx->manifest.find(AssetKind::Texture, s.array->name(l), &e)) {
                l.jobKey      = e.key;
                l.jobKeyValid = true;
            }
        }
        s.in.manifestPresent = ctx->manifestPresent;
    }
    if (ctx->prof) {
        s.submitNs = profile_now_ns();
        profile_interval(ctx->prof, stage == Stage::Meta ? "kiln.wait.meta" : "kiln.wait.upload", path_of(s),
                         s.queuedNs, s.submitNs);
    }
    ++ctx->jobsOutstanding;
    if (ready_push(ctx, s, stage)) { // after the job's inputs are written: a worker may take it now
        ctx->jobsInFlight.fetch_add(1, std::memory_order_acq_rel);
        ctx->jobs.submit(ctx->jobs.user, &run_jobs, ctx);
    }
}

void poll_placeholders(Context* ctx) {
    for (Placeholder& p : ctx->ph) {
        if (!p.pending) continue;
        Status why            = make_status(Code::Unknown);
        UploadStatus const st = ctx->adapter.upload_status(ctx->adapter.user, p.token, &why);
        if (st == UploadStatus::Pending) continue;
        p.pending = false;
        if (st == UploadStatus::Failed) { // textures of this kind and shape get a null object
            p.failed = true;
            ctx->adapter.destroy(ctx->adapter.user, p.obj);
            p.obj = {};
            (void)diagf(&ctx->diag, why, kDiagPlaceholderFailed, Severity::Error, {}, "placeholder",
                        "the adapter failed the upload of placeholder %llu (%s)",
                        static_cast<unsigned long long>(p.id), code_name(why.code));
            continue;
        }
        p.ready = true;
        for (u32 i = 0; i < ctx->maxAssets && ctx->bindPendingCount; ++i)
            if (ctx->slots[i].bindPending) bind_placeholder(ctx, ctx->slots[i]);
    }
}

namespace {

QueueId upload_queue(Slot const& s) {
    return s.priority == Priority::High ? QueueId::UploadHigh : QueueId::UploadNormal;
}

bool formats_supported(Context* ctx, Slot& s) {
    Adapter const& a = ctx->adapter;
    if (s.kind == AssetKind::Texture) {
        if (a.supports_format(a.user, s.out.next.texDesc.format, FormatUsage::SampledImage)) return true;
        s.out.capture.reset();
        format(s.out.capture.msg, sizeof s.out.capture.msg, "format %s is not supported",
               format_name(s.out.next.texDesc.format));
        s.out.capture.set = true;
        return false;
    }
    mesh::MeshView const& v = s.out.next.meshView;
    for (u32 l = 0; l < v.layouts().size(); ++l) {
        mesh::VertexLayout const layout = v.layouts().get(l);
        for (u32 i = 0; i < layout.attribCount && i < mesh::kMaxAttribs; ++i) {
            Format const f = Format(layout.attribs[i].format);
            if (a.supports_format(a.user, f, FormatUsage::VertexBuffer)) continue;
            s.out.capture.reset();
            format(s.out.capture.msg, sizeof s.out.capture.msg, "vertex format %s is not supported",
                   format_name(f));
            s.out.capture.set = true;
            return false;
        }
    }
    return true;
}

void on_meta_ready(Context* ctx, Slot& s) {
    if (!formats_supported(ctx, s)) {
        fail_slot(ctx, s, kDiagAdapterRejected, make_status(Code::Unsupported));
        return;
    }
    transition(s, Step::MetaDone);
    if (s.reloading) { // no MetaReady event and no group accounting: straight to upload
        queue_push(ctx, upload_queue(s), s);
        return;
    }
    if (GroupRec* g = group_of(ctx, s)) {
        s.groupBytes = s.out.next.uploadSize;
        g->bytesTotal += s.out.next.uploadSize;
    }
    push_event(ctx, EventKind::MetaReady, s.kind, handle_bits(s), s.version, kOk);
    ++ctx->cur.completed;
    queue_push(ctx, upload_queue(s), s);
}

/// The job loaded the artifact `cur` came from (a memory or provider source counts as the same).
bool same_artifact(Slot const& s) {
    if (!s.array) return s.keyValid == s.out.keyValid && (!s.keyValid || s.key == s.out.key);
    for (u32 i = 0; i < s.array->count; ++i) {
        ArrayLayer const& l = s.array->layers[i];
        if (l.keyValid != l.jobKeyValid || (l.keyValid && !(l.key == l.jobKey))) return false;
    }
    return true;
}

/// First load and reload alike: swap `out.next` into `cur` and bind the new object.
/// A reload bumps the content version and emits Changed, or Resized for a size change of the same
/// artifact (from Ready), or Ready (from Failed); a first load and a Failed -> Ready reload count in
/// the group.
void make_ready(Context* ctx, Slot& s) {
    if (ctx->prof) profile_interval(ctx->prof, "kiln.load", path_of(s), s.loadNs, profile_now_ns());
    bool const reload   = s.reloading;
    bool const resize   = s.resizing && same_artifact(s); // a new artifact under a size change: Changed
    State const from    = s.state;
    GpuObject const old = s.realObj;
    s.realObj           = s.out.target.object;
    s.out.hasTarget     = false;
    free_meta_set(ctx->alloc, s.cur);
    s.cur      = s.out.next;
    s.out.next = {};
    adopt_job_keys(s);
    transition(s, Step::Ready);
    if (reload) ++s.version;
    bind_object(ctx, s, s.realObj);
    retire(ctx, old, kInvalid);
    ++ctx->cur.uploadsCommitted;
    ++ctx->cur.completed;
    if (!reload) {
        if (GroupRec* g = group_of(ctx, s)) {
            --g->pending;
            ++g->ready;
            g->bytesDone += s.groupBytes;
        }
        s.groupAs = State::Ready;
    } else if (from == State::Failed && s.groupAs == State::Failed) {
        // Failed -> Ready is the asset's first success: the group moves it from failed to
        // ready with its bytes. A reload of a Ready asset never touches the group.
        if (GroupRec* g = group_of(ctx, s)) {
            --g->failed;
            ++g->ready;
            s.groupBytes = s.cur.uploadSize;
            g->bytesTotal += s.groupBytes;
            g->bytesDone += s.groupBytes;
        }
        s.groupAs = State::Ready;
    }
    EventKind const ev = reload && from == State::Ready ? (resize ? EventKind::Resized : EventKind::Changed)
                                                        : EventKind::Ready;
    push_event(ctx, ev, s.kind, handle_bits(s), s.version, kOk);
    // The payload is on the GPU: source bytes are no longer needed (metadata stays).
    s.memory.release();
    s.out.cooked.release();
    s.out.cookedValid = false;
    if (s.array) free_array_job_data(ctx->alloc, *s.array);
    settle(ctx, s);
}

void process(Context* ctx, Completion const& c) {
    Slot& s = ctx->slots[c.slot];
    KILN_ASSERT(s.job_in_flight() && s.in.gen == c.generation);
    --ctx->jobsOutstanding;
    if (s.zombie()) { // released while the job ran: discard the result
        orphan_upload(ctx, s);
        free_slot(ctx, s);
        return;
    }
    switch (c.kind) {
    case CompletionKind::MetaReady: on_meta_ready(ctx, s); break;
    case CompletionKind::BusyRetry:
        ++ctx->cur.busyRetries;
        transition(s, Step::UploadBusy);
        s.retryAfter = ctx->pumpIndex + 1; // at least one pump between retries
        queue_push(ctx, upload_queue(s), s);
        break;
    case CompletionKind::Uploaded:
        transition(s, Step::Uploaded);
        queue_push(ctx, QueueId::Await, s);
        break;
    case CompletionKind::Failed:
        fail_slot(ctx, s, s.out.diag ? s.out.diag : kDiagAssetLoadFailed, s.out.status);
        break;
    }
}

void drain_completions(Context* ctx, u32 maxCompletions) {
    u32 n = 0;
    {
        std::lock_guard<std::mutex> lock(ctx->compMutex);
        n = ctx->compCount;
        if (maxCompletions && n > maxCompletions) n = maxCompletions;
        for (u32 i = 0; i < n; ++i)
            ctx->compScratch[i] = ctx->comp[(ctx->compHead + i) % ctx->compCap];
        ctx->compHead = (ctx->compHead + n) % ctx->compCap;
        ctx->compCount -= n;
    }
    for (u32 i = 0; i < n; ++i)
        process(ctx, ctx->compScratch[i]);
}

/// The adapter failed a committed upload, for `why`. No frame has seen the object: it goes at once.
void fail_upload(Context* ctx, Slot& s, Status why) {
    ctx->adapter.destroy(ctx->adapter.user, s.out.target.object);
    s.out.hasTarget = false;
    s.out.capture.reset();
    format(s.out.capture.msg, sizeof s.out.capture.msg, "the adapter failed the upload (upload_status)");
    s.out.capture.set = true;
    fail_slot(ctx, s, kDiagAdapterRejected, why);
}

void poll_awaiting(Context* ctx) {
    Adapter const& a = ctx->adapter;
    for (u32 i = ctx->queues[u32(QueueId::Await)].head; i != kInvalid;) {
        Slot& s               = ctx->slots[i];
        u32 const next        = s.qNext;
        Status why            = make_status(Code::Unknown);
        UploadStatus const st = a.upload_status(a.user, s.out.target.token, &why);
        if (st != UploadStatus::Pending) {
            queue_remove(ctx, s);
            if (ctx->prof) profile_interval(ctx->prof, "kiln.gpu", path_of(s), s.queuedNs, profile_now_ns());
            if (st == UploadStatus::Complete)
                make_ready(ctx, s);
            else
                fail_upload(ctx, s, why);
        }
        i = next;
    }
}

void dispatch_uploads(Context* ctx, u64 budget) {
    u64 started           = 0;
    u32 dispatched        = 0;
    QueueId const order[] = {QueueId::UploadHigh, QueueId::UploadNormal};
    for (QueueId q : order) {
        for (u32 i = ctx->queues[u32(q)].head; i != kInvalid;) {
            Slot& s        = ctx->slots[i];
            u32 const next = s.qNext;
            if (s.retryAfter > ctx->pumpIndex) {
                i = next;
                continue;
            }
            // At least one upload starts per pump, so a single asset larger than the
            // budget still makes progress.
            u64 const size = s.out.next.uploadSize;
            if (dispatched > 0 && started + size > budget) return;
            queue_remove(ctx, s);
            submit_stage(ctx, s, Stage::Upload);
            ++dispatched;
            started += size;
            if (s.retryAfter == 0) { // a Busy retry is counted once, in busyRetries
                ++ctx->cur.uploadsStarted;
                ctx->cur.uploadBytes += size;
            }
            i = next;
        }
    }
}

void dispatch_meta(Context* ctx) {
    QueueId const order[] = {QueueId::MetaHigh, QueueId::MetaNormal};
    for (QueueId q : order) {
        List& l = ctx->queues[u32(q)];
        while (l.head != kInvalid) {
            Slot& s = ctx->slots[l.head];
            queue_remove(ctx, s);
            if (s.preFail.failed()) {
                s.out.capture.reset();
                format(s.out.capture.msg, sizeof s.out.capture.msg, "%s",
                       caps_allow(ctx, s.kind, s.texShape) ? "all Adapter::bindlessSlots are in use"
                                                           : "the adapter's caps do not allow this asset");
                s.out.capture.set = true;
                fail_slot(ctx, s, kDiagAdapterRejected, s.preFail);
                continue;
            }
            submit_stage(ctx, s, Stage::Meta);
        }
    }
}

void bind_pump_thread(Context* ctx) {
    if (!ctx->pumpBound) {
        ctx->pumpThread = std::this_thread::get_id();
        ctx->pumpBound  = true;
    }
}

} // namespace

PumpStats pump_impl(Context* ctx, PumpOptions const& opt, bool keepEvents) {
    ProfileZone const zone(ctx->prof, "kiln.pump");
    ++ctx->pumpIndex;
    if (opt.frame) ctx->frame = opt.frame;
    if (opt.completedFrame) ctx->completedFrame = opt.completedFrame;
    if (ctx->adapter.flush) ctx->adapter.flush(ctx->adapter.user);
    process_retired(ctx);
    ctx->cur = {};
    if (!keepEvents) ctx->eventCount = 0;
    ctx->droppedWarned = false;
    drain_posted_reloads(ctx);
    watch_drain(ctx); // reloads the store poller asked for; dispatched below
    poll_placeholders(ctx);
    poll_orphans(ctx);
    drain_completions(ctx, opt.maxCompletions);
    poll_awaiting(ctx);
    dispatch_uploads(ctx, opt.uploadBytes);
    dispatch_meta(ctx);
    return ctx->cur;
}

} // namespace rt

using namespace rt;

PumpStats pump(Context* ctx, PumpOptions const& opt) {
    if (!ctx) return {};
    bind_pump_thread(ctx);
    KILN_ASSERT(ctx->pumpThread == std::this_thread::get_id() && "pump() called off the pump thread");
    return pump_impl(ctx, opt, false);
}

Span<Event const> events(Context* ctx) {
    if (!ctx) return {};
    return {ctx->events, ctx->eventCount};
}

GroupStatus wait(Context* ctx, Group g, WaitOptions const& opt) {
    if (!ctx) return {};
    if ((ctx->adapter.caps & kSelfSubmitting) == 0 && !ctx->adapter.flush)
        KILN_PANIC("K5007 wait(): the adapter has neither kSelfSubmitting nor flush, so uploads cannot "
                   "complete without the host recording frames; keep calling pump() and poll progress() "
                   "instead");
    if (ctx->pumpBound && ctx->pumpThread != std::this_thread::get_id())
        KILN_PANIC("K5007 wait() called off the pump thread (the thread that first called pump() or wait())");
    bind_pump_thread(ctx);

    boost_group(ctx, g);
    using Clock         = std::chrono::steady_clock;
    auto const deadline = Clock::now() + std::chrono::milliseconds(opt.timeoutMs);
    PumpOptions po;
    po.uploadBytes = opt.uploadBytesPerPump;
    bool first     = true;
    for (;;) {
        GroupStatus const st = progress(ctx, g);
        if (st.settled()) return st;
        if (opt.timeoutMs && Clock::now() >= deadline) return st;
        // Events of every pump inside wait() accumulate; events() returns them afterwards.
        pump_impl(ctx, po, !first);
        first                 = false;
        GroupStatus const now = progress(ctx, g);
        if (now.settled()) return now;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace kiln
