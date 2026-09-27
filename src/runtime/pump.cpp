// pump.cpp — pump(), events() and wait(). Pump thread only; never allocates.
#include "runtime_internal.h"

#include "kiln/placeholders.h"

#include <chrono>

namespace kiln {
namespace rt {

void push_event(Context* ctx, EventKind kind, AssetKind asset, u64 bits, u32 version, Status st) noexcept {
    if (ctx->maxEvents == 0) return;
    if (ctx->eventCount == ctx->maxEvents) { // drop the oldest
        std::memmove(static_cast<void*>(ctx->events), static_cast<void const*>(ctx->events + 1),
                     sizeof(Event) * (ctx->eventCount - 1));
        --ctx->eventCount;
        ++ctx->cur.eventsDropped;
        if (!ctx->droppedWarned) {
            ctx->droppedWarned = true;
            diagf(&ctx->diag, make_status(Code::Busy), kDiagEventsDropped, Severity::Warning, {}, "events",
                  "event buffer full (maxEvents = %u); dropping the oldest events", ctx->maxEvents);
        }
    }
    Event& e  = ctx->events[ctx->eventCount++];
    e.kind    = kind;
    e.asset   = asset;
    e.handle  = bits;
    e.version = version;
    e.status  = st;
}

void fail_slot(Context* ctx, Slot& s, u32 code, Status st) noexcept {
    if (st.ok()) st = make_status(Code::Unknown);
    queue_remove(ctx, s);
    if (GroupRec* g = group_of(ctx, s)) {
        --g->pending;
        ++g->failed;
        g->bytesTotal -= s.groupBytes; // a failed member contributes no bytes
    }
    s.groupBytes = 0;
    s.state      = State::Failed;
    s.phase      = Phase::Done;

    char const* what = "load failed";
    switch (code) {
    case kDiagStoreMiss: what = "not in the store"; break;
    case kDiagCookOnMissFailed: what = "cook on miss failed"; break;
    case kDiagAdapterRejected: what = "adapter rejected the asset"; break;
    default: break;
    }
    diagf(&ctx->diag, st, code, Severity::Error, path_of(s), s.kind == AssetKind::Mesh ? "mesh" : "texture",
          "%s (%s)%s%s", what, code_name(st.code), s.capture.set ? ": " : "",
          s.capture.set ? s.capture.msg : "");

    push_event(ctx, EventKind::Failed, s.kind, handle_bits(s), s.version, st);
    ++ctx->cur.completed;
    Placeholder const& fp = ctx->ph[kFailedPlaceholder];
    if (s.kind == AssetKind::Texture && ctx->adapter.publish && !s.acquired.is_null() &&
        ctx->devPlaceholders && fp.ready)
        ctx->adapter.publish(ctx->adapter.user, s.id, fp.obj, s.version); // bindless slot shows the checker
    free_load_data(s);                                                    // Failed holds no metadata
}

void submit_stage(Context* ctx, Slot& s, Stage stage) noexcept {
    KILN_ASSERT(!s.jobInFlight && s.queue == QueueId::None);
    s.jobStage    = stage;
    s.jobGen      = s.generation;
    s.jobInFlight = true;
    s.phase       = stage == Stage::Meta ? Phase::MetaJob : Phase::UploadJob;
    if (stage == Stage::Meta) s.provider = ctx->provider;
    ++ctx->jobsOutstanding;
    ctx->jobsInFlight.fetch_add(1, std::memory_order_acq_rel);
    ctx->jobs.submit(ctx->jobs.user, &run_job, &s);
}

void poll_placeholders(Context* ctx) noexcept {
    for (Placeholder& p : ctx->ph) {
        if (!p.pending || !ctx->adapter.is_upload_complete(ctx->adapter.user, p.token)) continue;
        p.pending = false;
        p.ready   = true;
        if (ctx->adapter.publish) ctx->adapter.publish(ctx->adapter.user, p.id, p.obj, 1);
    }
}

namespace {

QueueId upload_queue(Slot const& s) noexcept {
    return s.priority == Priority::High ? QueueId::UploadHigh : QueueId::UploadNormal;
}

bool formats_supported(Context* ctx, Slot& s) noexcept {
    Adapter const& a = ctx->adapter;
    if (s.kind == AssetKind::Texture) {
        if (a.supports_format(a.user, s.texDesc.format, FormatUsage::SampledImage)) return true;
        s.capture.reset();
        format(s.capture.msg, sizeof s.capture.msg, "format %s is not supported",
               format_name(s.texDesc.format));
        s.capture.set = true;
        return false;
    }
    mesh::MeshView const& v = s.meshView;
    for (u32 l = 0; l < v.layouts().size(); ++l) {
        mesh::VertexLayout const layout = v.layouts().get(l);
        for (u32 i = 0; i < layout.attribCount && i < mesh::kMaxAttribs; ++i) {
            Format const f = Format(layout.attribs[i].format);
            if (a.supports_format(a.user, f, FormatUsage::VertexBuffer)) continue;
            s.capture.reset();
            format(s.capture.msg, sizeof s.capture.msg, "vertex format %s is not supported", format_name(f));
            s.capture.set = true;
            return false;
        }
    }
    return true;
}

void on_meta_ready(Context* ctx, Slot& s) noexcept {
    if (!formats_supported(ctx, s)) {
        fail_slot(ctx, s, kDiagAdapterRejected, make_status(Code::Unsupported));
        return;
    }
    s.state = State::MetaReady;
    s.phase = Phase::UploadQueued;
    if (GroupRec* g = group_of(ctx, s)) {
        s.groupBytes = s.uploadSize;
        g->bytesTotal += s.uploadSize;
    }
    push_event(ctx, EventKind::MetaReady, s.kind, handle_bits(s), s.version, kOk);
    ++ctx->cur.completed;
    queue_push(ctx, upload_queue(s), s);
}

void make_ready(Context* ctx, Slot& s) noexcept {
    GpuObject const old = s.realObj;
    s.realObj           = s.target.object;
    s.hasTarget         = false;
    s.state             = State::Ready;
    s.phase             = Phase::Done;
    Adapter const& a    = ctx->adapter;
    if (a.publish) a.publish(a.user, s.id, s.realObj, s.version);
    if (!old.is_null()) a.destroy_deferred(a.user, old); // hot reload (M5) swaps here
    ++ctx->cur.uploadsCommitted;
    ++ctx->cur.completed;
    if (GroupRec* g = group_of(ctx, s)) {
        --g->pending;
        ++g->ready;
        g->bytesDone += s.groupBytes;
    }
    push_event(ctx, EventKind::Ready, s.kind, handle_bits(s), s.version, kOk);
    // The payload is on the GPU: source bytes are no longer needed (metadata stays).
    s.memory.release();
    s.cooked.release();
    s.cookedValid = false;
}

void process(Context* ctx, Completion const& c) noexcept {
    Slot& s = ctx->slots[c.slot];
    KILN_ASSERT(s.jobInFlight && s.jobGen == c.generation);
    s.jobInFlight = false;
    --ctx->jobsOutstanding;
    if (s.zombie) { // released while the job ran: discard the result
        if (s.hasTarget) ctx->adapter.destroy_deferred(ctx->adapter.user, s.target.object);
        s.hasTarget = false;
        free_slot(ctx, s);
        return;
    }
    switch (c.kind) {
    case CompletionKind::MetaReady: on_meta_ready(ctx, s); break;
    case CompletionKind::BusyRetry:
        ++ctx->cur.busyRetries;
        s.phase      = Phase::UploadQueued;
        s.retryAfter = ctx->pumpIndex + 1; // at least one pump between retries
        queue_push(ctx, upload_queue(s), s);
        break;
    case CompletionKind::Uploaded:
        s.phase = Phase::Awaiting;
        queue_push(ctx, QueueId::Await, s);
        break;
    case CompletionKind::Failed:
        if (s.hasTarget) ctx->adapter.destroy_deferred(ctx->adapter.user, s.target.object);
        s.hasTarget = false;
        fail_slot(ctx, s, s.jobDiag ? s.jobDiag : kDiagAssetLoadFailed, s.jobStatus);
        break;
    }
}

void drain_completions(Context* ctx, u32 maxCompletions) noexcept {
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

void poll_awaiting(Context* ctx) noexcept {
    Adapter const& a = ctx->adapter;
    for (u32 i = ctx->queues[u32(QueueId::Await)].head; i != kInvalid;) {
        Slot& s        = ctx->slots[i];
        u32 const next = s.qNext;
        if (a.is_upload_complete(a.user, s.target.token)) {
            queue_remove(ctx, s);
            make_ready(ctx, s);
        }
        i = next;
    }
}

void dispatch_uploads(Context* ctx, u64 budget) noexcept {
    u64 started           = 0;
    u32 dispatched        = 0;
    QueueId const order[] = {QueueId::UploadHigh, QueueId::UploadNormal};
    for (QueueId q : order) {
        for (u32 i = ctx->queues[u32(q)].head; i != kInvalid;) {
            Slot& s        = ctx->slots[i];
            u32 const next = s.qNext;
            if (ctx->jobsOutstanding >= ctx->maxIoJobs) return;
            if (s.retryAfter > ctx->pumpIndex) {
                i = next;
                continue;
            }
            // At least one upload starts per pump, so a single asset larger than the
            // budget still makes progress.
            if (dispatched > 0 && started + s.uploadSize > budget) return;
            queue_remove(ctx, s);
            submit_stage(ctx, s, Stage::Upload);
            ++dispatched;
            started += s.uploadSize;
            if (s.retryAfter == 0) { // a Busy retry is counted once, in busyRetries
                ++ctx->cur.uploadsStarted;
                ctx->cur.uploadBytes += s.uploadSize;
            }
            i = next;
        }
    }
}

void dispatch_meta(Context* ctx) noexcept {
    QueueId const order[] = {QueueId::MetaHigh, QueueId::MetaNormal};
    for (QueueId q : order) {
        List& l = ctx->queues[u32(q)];
        while (l.head != kInvalid && ctx->jobsOutstanding < ctx->maxIoJobs) {
            Slot& s = ctx->slots[l.head];
            queue_remove(ctx, s);
            if (s.preFail.failed()) {
                s.capture.reset();
                format(s.capture.msg, sizeof s.capture.msg, "acquire() failed");
                s.capture.set = true;
                fail_slot(ctx, s, kDiagAdapterRejected, s.preFail);
                continue;
            }
            submit_stage(ctx, s, Stage::Meta);
        }
    }
}

void bind_pump_thread(Context* ctx) noexcept {
    if (!ctx->pumpBound) {
        ctx->pumpThread = std::this_thread::get_id();
        ctx->pumpBound  = true;
    }
}

} // namespace

PumpStats pump_impl(Context* ctx, PumpOptions const& opt, bool keepEvents) noexcept {
    ++ctx->pumpIndex;
    ctx->cur = {};
    if (!keepEvents) ctx->eventCount = 0;
    ctx->droppedWarned = false;
    poll_placeholders(ctx);
    drain_completions(ctx, opt.maxCompletions);
    poll_awaiting(ctx);
    dispatch_uploads(ctx, opt.uploadBytes);
    dispatch_meta(ctx);
    return ctx->cur;
}

} // namespace rt

using namespace rt;

PumpStats pump(Context* ctx, PumpOptions const& opt) noexcept {
    if (!ctx) return {};
    bind_pump_thread(ctx);
    KILN_ASSERT(ctx->pumpThread == std::this_thread::get_id() && "pump() called off the pump thread");
    return pump_impl(ctx, opt, false);
}

Span<Event const> events(Context* ctx) noexcept {
    if (!ctx) return {};
    return {ctx->events, ctx->eventCount};
}

GroupStatus wait(Context* ctx, Group g, WaitOptions const& opt) noexcept {
    if (!ctx) return {};
    if ((ctx->adapter.caps & kSelfSubmitting) == 0)
        KILN_PANIC("K5007 wait(): the adapter lacks kSelfSubmitting, so uploads cannot complete without the "
                   "host recording frames; keep calling pump() and poll progress() instead");
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
