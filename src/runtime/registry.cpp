// registry.cpp — ids, slots and handles, request queues, refcounts, groups and queries.
// Pump thread only.
#include "runtime_internal.h"

#include "kiln/placeholders.h"

namespace kiln {
namespace rt {

void Buffer::allocate(Allocator const* a, usize n, Tag t) {
    KILN_ASSERT(data == nullptr);
    alloc = a;
    tag   = t;
    size  = n;
    data  = n ? static_cast<u8*>(kiln::alloc(a, n, 16, t)) : nullptr;
}

void Buffer::release() {
    if (data) kiln::free(alloc, data, size, 16, tag);
    data = nullptr;
    size = 0;
}

// ---------------------------------------------------------------------------
// Paths and ids
// ---------------------------------------------------------------------------

HashMap<AssetId, u32>& map_for(Context* ctx, AssetKind kind) {
    return kind == AssetKind::Mesh ? ctx->meshMap : ctx->texMap;
}

Slot* resolve(Context* ctx, u64 bits, AssetKind kind) {
    if (!ctx) return nullptr;
    u32 const index = u32(bits);
    u32 const gen   = u32(bits >> 32);
    if (gen == 0 || index >= ctx->maxAssets) return nullptr;
    Slot& s = ctx->slots[index];
    if (!s.live() || s.zombie() || s.generation != gen || s.kind != kind) return nullptr;
    return &s;
}

// ---------------------------------------------------------------------------
// Intrusive queues over slot indices
// ---------------------------------------------------------------------------

void queue_push(Context* ctx, QueueId q, Slot& s) {
    KILN_ASSERT(s.queue == QueueId::None);
    List& l = ctx->queues[u32(q)];
    s.queue = q;
    s.qNext = kInvalid;
    s.qPrev = l.tail;
    if (l.tail != kInvalid)
        ctx->slots[l.tail].qNext = s.index;
    else
        l.head = s.index;
    l.tail = s.index;
    ++l.count;
    if (ctx->prof) {
        s.queuedNs = profile_now_ns();
        if (q == QueueId::MetaHigh || q == QueueId::MetaNormal) s.loadNs = s.queuedNs;
    }
}

void queue_remove(Context* ctx, Slot& s) {
    if (s.queue == QueueId::None) return;
    List& l = ctx->queues[u32(s.queue)];
    if (s.qPrev != kInvalid)
        ctx->slots[s.qPrev].qNext = s.qNext;
    else
        l.head = s.qNext;
    if (s.qNext != kInvalid)
        ctx->slots[s.qNext].qPrev = s.qPrev;
    else
        l.tail = s.qPrev;
    --l.count;
    s.queue = QueueId::None;
    s.qPrev = s.qNext = kInvalid;
}

void boost(Context* ctx, Slot& s) {
    s.priority         = Priority::High;
    u64 const loadNs   = s.loadNs;
    u64 const queuedNs = s.queuedNs;
    if (s.queue == QueueId::MetaNormal) {
        queue_remove(ctx, s);
        queue_push(ctx, QueueId::MetaHigh, s);
    } else if (s.queue == QueueId::UploadNormal) {
        queue_remove(ctx, s);
        queue_push(ctx, QueueId::UploadHigh, s);
    } else if (s.job_in_flight()) {
        ready_boost(ctx, s);
    }
    s.loadNs   = loadNs; // a boost moves the slot; its waits go on
    s.queuedNs = queuedNs;
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

GroupRec* group_of(Context* ctx, Slot const& s) {
    if (s.groupIndex == kInvalid) return nullptr;
    GroupRec& g = ctx->groups[s.groupIndex];
    return (g.live && g.generation == s.groupGen) ? &g : nullptr;
}

namespace {

GroupRec* resolve_group(Context* ctx, Group g) {
    if (!ctx || g.is_null() || g.index >= ctx->maxGroups) return nullptr;
    GroupRec& r = ctx->groups[g.index];
    return (r.live && r.generation == g.generation) ? &r : nullptr;
}

void join_group(Context* ctx, Slot& s, Group g) {
    if (s.groupIndex != kInvalid && group_of(ctx, s)) return; // first live group wins
    GroupRec* r = resolve_group(ctx, g);
    if (!r) return;
    s.groupIndex = g.index;
    s.groupGen   = g.generation;
    s.groupBytes = 0;
    s.groupAs    = s.state;
    switch (s.state) {
    case State::Ready:
        ++r->ready;
        s.groupBytes = s.cur.uploadSize;
        r->bytesDone += s.cur.uploadSize;
        break;
    case State::Failed: ++r->failed; break;
    case State::MetaReady:
        ++r->pending;
        s.groupBytes = s.out.next.uploadSize;
        break;
    default: ++r->pending; break;
    }
    r->bytesTotal += s.groupBytes;
}

void leave_group(Context* ctx, Slot& s) {
    if (GroupRec* r = group_of(ctx, s)) {
        switch (s.groupAs) {
        case State::Ready:
            --r->ready;
            r->bytesDone -= s.groupBytes;
            break;
        case State::Failed: --r->failed; break;
        default: --r->pending; break;
        }
        r->bytesTotal -= s.groupBytes;
    }
    s.groupIndex = kInvalid;
    s.groupGen   = 0;
    s.groupBytes = 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void free_meta_set(Allocator const* a, MetaSet& m) {
    m.meta.release();
    if (m.layout) free_array(a, m.layout, usize(m.layoutLevels) * kLayoutColumns, Tag::Payload);
    m = {};
}

MetaSet const* shown_meta(Slot const& s) {
    if (s.state == State::Ready) return &s.cur;
    if (s.state == State::MetaReady) return &s.out.next;
    return nullptr;
}

ArrayDecl* new_array_decl(Allocator const* a, u32 count, usize namesLen) {
    auto* d     = new_object<ArrayDecl>(a, Tag::Registry);
    d->names    = alloc_array<char>(a, max<usize>(namesLen, 1), Tag::Registry);
    d->namesLen = namesLen;
    d->layers   = alloc_array<ArrayLayer>(a, count, Tag::Registry);
    d->count    = count;
    for (u32 i = 0; i < count; ++i)
        ::new (static_cast<void*>(d->layers + i)) ArrayLayer();
    return d;
}

void free_array_job_data(Allocator const* a, ArrayDecl& d) {
    for (u32 i = 0; i < d.count; ++i) {
        ArrayLayer& l = d.layers[i];
        l.cooked.release();
        l.cookedValid = false;
        if (l.src) free_array(a, l.src, usize(l.srcLevels) * 2, Tag::Payload);
        l.src       = nullptr;
        l.srcLevels = 0;
        l.zstd      = false;
    }
}

void free_array_decl(Allocator const* a, ArrayDecl* d) {
    if (!d) return;
    free_array_job_data(a, *d);
    for (u32 i = 0; i < d->count; ++i)
        d->layers[i].~ArrayLayer();
    free_array(a, d->layers, d->count, Tag::Registry);
    free_array(a, d->names, max<usize>(d->namesLen, 1), Tag::Registry);
    delete_object(a, d, Tag::Registry);
}

void adopt_job_keys(Slot& s) {
    s.key            = s.out.key;
    s.keyValid       = s.out.keyValid;
    s.failedKeyValid = false;
    s.providerOwned  = s.out.providerOwned;
    if (!s.array) return;
    for (u32 i = 0; i < s.array->count; ++i) {
        ArrayLayer& l    = s.array->layers[i];
        l.key            = l.jobKey;
        l.keyValid       = l.jobKeyValid;
        l.failedKeyValid = false;
        l.providerOwned  = l.jobProviderOwned;
    }
}

void remember_failed_keys(Slot& s) {
    s.failedKey      = s.out.key;
    s.failedKeyValid = s.out.keyValid;
    s.providerOwned |= s.out.providerOwned;
    if (!s.array) return;
    for (u32 i = 0; i < s.array->count; ++i) {
        ArrayLayer& l    = s.array->layers[i];
        l.failedKey      = l.jobKey;
        l.failedKeyValid = l.jobKeyValid;
        l.providerOwned |= l.jobProviderOwned;
    }
}

void free_load_data(Slot& s) {
    free_meta_set(s.ctx->alloc, s.cur);
    free_meta_set(s.ctx->alloc, s.out.next);
    s.memory.release();
    s.out.cooked.release();
    s.out.cookedValid = false;
    if (s.array) free_array_job_data(s.ctx->alloc, *s.array);
}

void transition(Slot& s, Step step) {
    Phase const p = s.phase;
    switch (step) {
    case Step::Request:
        KILN_VERIFY(p == Phase::Free);
        s.state     = State::Pending;
        s.phase     = Phase::MetaQueued;
        s.reloading = false;
        return;
    case Step::Reload:
        KILN_VERIFY(p == Phase::Done && (s.state == State::Ready || s.state == State::Failed));
        s.phase     = Phase::MetaQueued;
        s.reloading = true;
        return;
    case Step::SubmitMeta:
        KILN_VERIFY(p == Phase::MetaQueued && !s.zombie());
        s.phase = Phase::MetaJob;
        return;
    case Step::MetaDone:
        KILN_VERIFY(p == Phase::MetaJob && !s.zombie());
        s.phase = Phase::UploadQueued;
        if (!s.reloading) s.state = State::MetaReady;
        return;
    case Step::SubmitUpload:
        KILN_VERIFY(p == Phase::UploadQueued && !s.zombie());
        s.phase = Phase::UploadJob;
        return;
    case Step::UploadBusy:
        KILN_VERIFY(p == Phase::UploadJob && !s.zombie());
        s.phase = Phase::UploadQueued;
        return;
    case Step::Uploaded:
        KILN_VERIFY(p == Phase::UploadJob && !s.zombie());
        s.phase = Phase::Awaiting;
        return;
    case Step::Ready:
        KILN_VERIFY(p == Phase::Awaiting && !s.zombie());
        s.state     = State::Ready;
        s.phase     = Phase::Done;
        s.reloading = false;
        return;
    case Step::Fail:
        // MetaQueued: a request-time failure (Slot::preFail) reported at dispatch.
        KILN_VERIFY((p == Phase::MetaQueued || p == Phase::MetaJob || p == Phase::UploadJob ||
                     p == Phase::Awaiting) &&
                    !s.zombie());
        if (!(s.reloading && s.state == State::Ready)) s.state = State::Failed;
        s.phase     = Phase::Done;
        s.reloading = false;
        return;
    case Step::Unload:
        KILN_VERIFY(s.live() && !s.zombie());
        s.state = State::Unloaded;
        return;
    case Step::Free:
        KILN_VERIFY(s.zombie());
        s.phase     = Phase::Free;
        s.reloading = false;
        return;
    }
}

void free_slot(Context* ctx, Slot& s) {
    KILN_ASSERT(s.queue == QueueId::None);
    free_load_data(s);
    free_array_decl(ctx->alloc, s.array);
    transition(s, Step::Free);
    s.array                              = nullptr;
    s.refcount                           = 0;
    s.out.hasTarget                      = false;
    s.reloadPending                      = false;
    s.realObj                            = {};
    s.bindSlot                           = kInvalid;
    s.bindPending                        = false;
    ctx->freeSlots[ctx->freeSlotCount++] = s.index;
}

Slot* request_slot(Context* ctx, AssetKind kind, StrView path, RequestOptions const& opt, Buffer* memory,
                   bool rejectExisting) {
    if (char const* why = check_asset_name(path)) {
        (void)diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagBadAssetName, Severity::Error, path,
                    "request", "invalid asset name: %s", why);
        return nullptr;
    }
    StrView const np           = path;
    AssetId const id           = fnv1a64(np);
    HashMap<AssetId, u32>& map = map_for(ctx, kind);

    if (u32 const* found = map.find(id)) {
        Slot& s = ctx->slots[*found];
        if (StrView(s.path, s.pathLen) != np)
            KILN_PANIC("asset id collision: '%.*s' and '%.*s' hash to %016llx", int(s.pathLen), s.path,
                       KILN_SV(np), static_cast<unsigned long long>(id));
        if (s.source == SourceKind::Array && !rejectExisting) {
            (void)diagf(&ctx->diag, make_status(Code::AlreadyExists), kDiagArrayDeclaration, Severity::Error,
                        np, "request",
                        "the name is a texture array: request it with request_texture_array()");
            return nullptr;
        }
        if (rejectExisting) {
            (void)diagf(&ctx->diag, make_status(Code::AlreadyExists), kDiagDuplicateRegister, Severity::Error,
                        np, "register", "path is already registered or requested");
            return nullptr;
        }
        ++s.refcount;
        if (opt.priority == Priority::High) boost(ctx, s);
        join_group(ctx, s, opt.group);
        return &s;
    }

    if (id <= kLastPlaceholderId) { // practically never; reserved ids (handles-and-states.md)
        (void)diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagAssetLoadFailed, Severity::Error, np,
                    "request", "asset path hashes into the reserved placeholder id range");
        return nullptr;
    }
    if (ctx->freeSlotCount == 0) {
        (void)diagf(&ctx->diag, make_status(Code::Busy), kDiagRegistryFull, Severity::Error, np, "request",
                    "registry full (maxAssets = %u)", ctx->maxAssets);
        return nullptr;
    }

    Slot& s = ctx->slots[ctx->freeSlots[--ctx->freeSlotCount]];
    transition(s, Step::Request);
    s.id            = id;
    s.kind          = kind;
    s.priority      = opt.priority;
    s.texKind       = opt.textureKind < TextureKind::Count ? opt.textureKind : TextureKind::BaseColor;
    s.texShape      = kind == AssetKind::Texture && opt.textureShape < TextureShape::Count ? opt.textureShape
                                                                                           : TextureShape::Tex2D;
    s.refcount      = 1;
    s.version       = 1;
    s.groupIndex    = kInvalid;
    s.groupGen      = 0;
    s.groupBytes    = 0;
    s.retryAfter    = 0;
    s.realObj       = {};
    s.preFail       = kOk;
    s.bindSlot      = kInvalid;
    s.bindPending   = false;
    s.out.hasTarget = false;
    s.out.target    = {};
    s.reloadPending = false;
    s.groupAs       = State::Pending;
    s.out.status    = kOk;
    s.out.keyValid  = false;
    s.manifestCheck = false;
    s.keyValid      = false;
    s.out.diag      = 0;
    s.out.capture.reset();
    std::memcpy(s.path, np.data, np.size);
    s.path[np.size] = '\0';
    s.pathLen       = u32(np.size);
    if (memory) {
        s.source = SourceKind::Memory;
        s.memory = *memory;
        *memory  = {};
    } else {
        s.source = SourceKind::File;
    }
    map.insert(id, s.index); // reserved to maxAssets at create: never rehashes

    if (!caps_allow(ctx, kind, s.texShape)) {
        s.preFail = make_status(Code::Unsupported);
    } else if (kind == AssetKind::Texture && ctx->adapter.bind) {
        if (!ctx->freeBindSlots.empty()) {
            s.bindSlot = ctx->freeBindSlots.back();
            ctx->freeBindSlots.pop_back();
        } else if (ctx->nextBindSlot < ctx->adapter.bindlessSlots) {
            s.bindSlot = ctx->nextBindSlot++;
        }
        if (s.bindSlot == kInvalid)
            s.preFail = make_status(Code::Busy);
        else
            bind_placeholder(ctx, s);
    }
    queue_push(ctx, opt.priority == Priority::High ? QueueId::MetaHigh : QueueId::MetaNormal, s);
    join_group(ctx, s, opt.group);
    return &s;
}

namespace {

void unload(Context* ctx, Slot& s) {
    leave_group(ctx, s);
    if (s.queue == QueueId::Await) orphan_upload(ctx, s);
    queue_remove(ctx, s);
    if (s.bindPending) --ctx->bindPendingCount;
    retire(ctx, s.realObj, s.bindSlot);
    s.realObj     = {};
    s.bindSlot    = kInvalid;
    s.bindPending = false;
    map_for(ctx, s.kind).erase(s.id);
    transition(s, Step::Unload);
    // Handles go stale now; the slot is reused only after the in-flight job (if any) completed.
    s.generation = s.generation + 1 == 0 ? 1 : s.generation + 1;
    if (s.job_in_flight() && ready_remove(ctx, s)) { // prepared, and no worker took it: no job runs
        --ctx->jobsOutstanding;
        free_slot(ctx, s);
    } else if (!s.job_in_flight()) {
        free_slot(ctx, s);
    } else { // a zombie: its completion frees it
        s.in.abandoned.store(true, std::memory_order_release);
    }
}

void release_impl(Context* ctx, u64 bits, AssetKind kind) {
    Slot* s = resolve(ctx, bits, kind);
    if (!s) {
        KILN_ASSERT(bits == 0 && "release() of a stale handle (double release?)");
        return;
    }
    KILN_ASSERT(s->refcount > 0);
    if (--s->refcount == 0) unload(ctx, *s);
}

Placeholder const& kind_placeholder(Context* ctx, TextureKind k, TextureShape shape) {
    return ctx->ph[placeholder_index(k < TextureKind::Count ? k : TextureKind::BaseColor, shape)];
}

/// The placeholder a texture shows in `s`'s state (or for a stale handle when s is null).
Placeholder const& texture_placeholder(Context* ctx, Slot const* s) {
    bool const failedLook    = !s || s->state == State::Failed;
    TextureShape const shape = s ? s->texShape : TextureShape::Tex2D;
    Placeholder const& fp    = ctx->ph[failed_placeholder_index(shape)];
    if (failedLook && ctx->devPlaceholders && fp.ready) return fp;
    return kind_placeholder(ctx, s ? s->texKind : TextureKind::BaseColor, shape);
}

GpuObject placeholder_obj(Placeholder const& p) { return p.ready ? p.obj : GpuObject{}; }

void release_now(Context* ctx, GpuObject obj, u32 bindSlot) {
    if (!obj.is_null()) ctx->adapter.destroy(ctx->adapter.user, obj);
    if (bindSlot != kInvalid) ctx->freeBindSlots.push_back(bindSlot);
}

} // namespace

void retire(Context* ctx, GpuObject obj, u32 bindSlot) {
    if (obj.is_null() && bindSlot == kInvalid) return;
    if (ctx->completedFrame >= ctx->frame)
        release_now(ctx, obj, bindSlot);
    else
        ctx->retired.push_back({obj, bindSlot, ctx->frame});
}

void process_retired(Context* ctx) {
    for (usize i = 0; i < ctx->retired.size();) {
        Retired const r = ctx->retired[i];
        if (ctx->completedFrame < r.frame) {
            ++i;
            continue;
        }
        ctx->retired.erase_unordered(i);
        release_now(ctx, r.obj, r.bindSlot);
    }
}

void orphan_upload(Context* ctx, Slot& s) {
    if (!s.out.hasTarget) return;
    ctx->orphans.push_back({s.out.target.token, s.out.target.object});
    s.out.hasTarget = false;
}

void poll_orphans(Context* ctx) {
    for (usize i = 0; i < ctx->orphans.size();) {
        Orphan const o = ctx->orphans[i];
        Status why     = kOk; // an abandoned upload's reason goes nowhere
        if (ctx->adapter.upload_status(ctx->adapter.user, o.token, &why) == UploadStatus::Pending) {
            ++i;
            continue;
        }
        ctx->orphans.erase_unordered(i);
        retire(ctx, o.obj, kInvalid);
    }
}

void bind_object(Context* ctx, Slot& s, GpuObject obj) {
    if (s.bindSlot == kInvalid) return;
    if (s.bindPending) --ctx->bindPendingCount;
    s.bindPending = false;
    ctx->adapter.bind(ctx->adapter.user, s.bindSlot, obj, s.texShape);
}

void bind_placeholder(Context* ctx, Slot& s) {
    if (s.bindSlot == kInvalid) return;
    GpuObject const obj = placeholder_obj(texture_placeholder(ctx, &s));
    if (!obj.is_null()) {
        bind_object(ctx, s, obj);
    } else if (!s.bindPending) {
        s.bindPending = true;
        ++ctx->bindPendingCount;
    }
}
} // namespace rt

using namespace rt;

// ---------------------------------------------------------------------------
// Public API: ids, requests
// ---------------------------------------------------------------------------

AssetId asset_id(StrView name) { return check_asset_name(name) ? 0 : fnv1a64(name); }

MeshHandle request_mesh(Context* ctx, StrView path, RequestOptions const& opt) {
    if (!ctx) return {};
    Slot* s = request_slot(ctx, AssetKind::Mesh, path, opt, nullptr, false);
    return s ? MeshHandle::from_bits(handle_bits(*s)) : MeshHandle{};
}

TextureHandle request_texture(Context* ctx, StrView path, RequestOptions const& opt) {
    if (!ctx) return {};
    Slot* s = request_slot(ctx, AssetKind::Texture, path, opt, nullptr, false);
    return s ? TextureHandle::from_bits(handle_bits(*s)) : TextureHandle{};
}

namespace {

Slot* array_request_failed(Context* ctx, StrView name, Code code, char const* why) {
    (void)diagf(&ctx->diag, make_status(code), kDiagArrayDeclaration, Severity::Error, name, "request",
                "texture array: %s", why);
    return nullptr;
}

bool same_layers(ArrayDecl const& d, Span<StrView const> layers) {
    if (d.count != layers.size) return false;
    for (u32 i = 0; i < d.count; ++i)
        if (d.name(d.layers[i]) != layers[i]) return false;
    return true;
}

Slot* request_array_slot(Context* ctx, TextureArrayDesc const& desc) {
    StrView const name = desc.name;
    if (char const* why = check_asset_name(name)) {
        (void)diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagBadAssetName, Severity::Error, name,
                    "request", "invalid asset name: %s", why);
        return nullptr;
    }
    if (desc.layers.empty()) return array_request_failed(ctx, name, Code::InvalidArgument, "no layers");
    if (desc.layers.size > kMaxTextureArrayLayers)
        return array_request_failed(ctx, name, Code::InvalidArgument,
                                    "more than kMaxTextureArrayLayers layers");
    usize namesLen = 0;
    for (StrView const layer : desc.layers) {
        if (char const* why = check_asset_name(layer)) {
            (void)diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagArrayDeclaration,
                        Severity::Error, name, "request",
                        "texture array: layer '%.*s' is not a valid asset name: %s", KILN_SV(layer), why);
            return nullptr;
        }
        if (layer == name)
            return array_request_failed(ctx, name, Code::InvalidArgument, "a layer names the array");
        namesLen += layer.size;
    }

    RequestOptions const opt{.priority     = desc.priority,
                             .group        = desc.group,
                             .textureKind  = desc.textureKind,
                             .textureShape = TextureShape::Array};
    if (u32 const* found = ctx->texMap.find(fnv1a64(name))) {
        Slot& s = ctx->slots[*found];
        if (StrView(s.path, s.pathLen) != name)
            return request_slot(ctx, AssetKind::Texture, name, opt, nullptr, false);
        if (s.source != SourceKind::Array)
            return array_request_failed(ctx, name, Code::AlreadyExists,
                                        "the name is used by another texture");
        if (!same_layers(*s.array, desc.layers))
            return array_request_failed(ctx, name, Code::AlreadyExists,
                                        "the name is declared with another list of layers");
        ++s.refcount;
        if (desc.priority == Priority::High) boost(ctx, s);
        join_group(ctx, s, desc.group);
        return &s;
    }

    Slot* s = request_slot(ctx, AssetKind::Texture, name, opt, nullptr, false);
    if (!s) return nullptr;
    ArrayDecl* d = new_array_decl(ctx->alloc, u32(desc.layers.size), namesLen);
    u32 off      = 0;
    for (u32 i = 0; i < d->count; ++i) {
        StrView const layer = desc.layers[i];
        std::memcpy(d->names + off, layer.data, layer.size);
        d->layers[i].nameOff = off;
        d->layers[i].nameLen = u32(layer.size);
        off += u32(layer.size);
    }
    s->source = SourceKind::Array;
    s->array  = d;
    return s;
}

} // namespace

TextureHandle request_texture_array(Context* ctx, TextureArrayDesc const& desc) {
    if (!ctx) return {};
    Slot* s = request_array_slot(ctx, desc);
    return s ? TextureHandle::from_bits(handle_bits(*s)) : TextureHandle{};
}

void release(Context* ctx, MeshHandle h) { release_impl(ctx, h.bits(), AssetKind::Mesh); }
void release(Context* ctx, TextureHandle h) { release_impl(ctx, h.bits(), AssetKind::Texture); }

MeshHandle find_mesh(Context* ctx, AssetId id) {
    if (!ctx) return {};
    u32 const* i = ctx->meshMap.find(id);
    return i ? MeshHandle::from_bits(handle_bits(ctx->slots[*i])) : MeshHandle{};
}

TextureHandle find_texture(Context* ctx, AssetId id) {
    if (!ctx) return {};
    u32 const* i = ctx->texMap.find(id);
    return i ? TextureHandle::from_bits(handle_bits(ctx->slots[*i])) : TextureHandle{};
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

State state(Context* ctx, MeshHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return s ? s->state : State::Unloaded;
}
State state(Context* ctx, TextureHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    return s ? s->state : State::Unloaded;
}
bool has_meta(Context* ctx, MeshHandle h) {
    State const st = state(ctx, h);
    return st == State::MetaReady || st == State::Ready;
}
bool has_meta(Context* ctx, TextureHandle h) {
    State const st = state(ctx, h);
    return st == State::MetaReady || st == State::Ready;
}
bool is_ready(Context* ctx, MeshHandle h) { return state(ctx, h) == State::Ready; }
bool is_ready(Context* ctx, TextureHandle h) { return state(ctx, h) == State::Ready; }

u32 version(Context* ctx, MeshHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return s ? s->version : 0;
}
u32 version(Context* ctx, TextureHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    return s ? s->version : 0;
}
AssetId id_of(Context* ctx, MeshHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return s ? s->id : 0;
}
AssetId id_of(Context* ctx, TextureHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    return s ? s->id : 0;
}

GpuObject gpu_object(Context* ctx, MeshHandle h) {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return (s && s->state == State::Ready) ? s->realObj : GpuObject{};
}

GpuObject gpu_object(Context* ctx, TextureHandle h) {
    if (!ctx) return {};
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    GpuObject obj = s && s->state == State::Ready ? s->realObj : placeholder_obj(texture_placeholder(ctx, s));
    if (s && s->bindSlot != kInvalid) obj.slot = s->bindSlot;
    return obj;
}

mesh::MeshView const* mesh_view(Context* ctx, MeshHandle h) {
    Slot const* s    = resolve(ctx, h.bits(), AssetKind::Mesh);
    MetaSet const* m = s ? shown_meta(*s) : nullptr;
    return m ? &m->meshView : nullptr;
}

TextureInfo texture_info(Context* ctx, TextureHandle h) {
    TextureInfo info;
    if (!ctx) return info;
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    info.gpu      = gpu_object(ctx, h);
    if (MetaSet const* m = s ? shown_meta(*s) : nullptr) {
        info.desc            = m->texDesc;
        info.levelOffsets    = {m->layout, m->layoutLevels};
        info.levelRowPitches = {m->layout + m->layoutLevels, m->layoutLevels};
        info.version         = s->version;
        info.isPlaceholder   = s->state != State::Ready;
        return info;
    }
    Placeholder const& p = texture_placeholder(ctx, s);
    info.desc            = p.desc;
    info.levelOffsets    = {&p.offset, 1};
    info.levelRowPitches = {&p.pitch, 1};
    info.version         = s ? s->version : 0;
    info.isPlaceholder   = true;
    return info;
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

Group group(Context* ctx) {
    if (!ctx) return {};
    if (ctx->freeGroupCount == 0) {
        (void)diagf(&ctx->diag, make_status(Code::Busy), kDiagRegistryFull, Severity::Error, {}, "group",
                    "group table full (maxGroups = %u)", ctx->maxGroups);
        return {};
    }
    u32 const i  = ctx->freeGroups[--ctx->freeGroupCount];
    GroupRec& r  = ctx->groups[i];
    u32 const g  = r.generation;
    r            = {};
    r.generation = g;
    r.live       = true;
    return Group{i, g};
}

void release(Context* ctx, Group g) {
    GroupRec* r = resolve_group(ctx, g);
    if (!r) return;
    r->live                                = false;
    r->generation                          = r->generation + 1 == 0 ? 1 : r->generation + 1;
    ctx->freeGroups[ctx->freeGroupCount++] = g.index;
}

GroupStatus progress(Context* ctx, Group g) {
    GroupRec const* r = resolve_group(ctx, g);
    if (!r) return {};
    return GroupStatus{r->ready, r->failed, r->pending, r->bytesDone, r->bytesTotal};
}

namespace rt {
/// Raise every live member of `g` to High (wait()).
void boost_group(Context* ctx, Group g) {
    if (!resolve_group(ctx, g)) return;
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot& s = ctx->slots[i];
        if (s.live() && !s.zombie() && s.groupIndex == g.index && s.groupGen == g.generation) boost(ctx, s);
    }
}
} // namespace rt

GpuObject placeholder_object(Context* ctx, TextureKind kind, TextureShape shape) {
    if (!ctx || kind >= TextureKind::Count || shape >= TextureShape::Count) return {};
    return placeholder_obj(ctx->ph[placeholder_index(kind, shape)]);
}

TextureKind texture_kind_for_slot(mesh::TextureSlot slot) {
    switch (slot) {
    case mesh::TextureSlot::BaseColor: return TextureKind::BaseColor;
    case mesh::TextureSlot::Normal: return TextureKind::Normal;
    case mesh::TextureSlot::MetalRough:
    case mesh::TextureSlot::Occlusion: return TextureKind::Orm;
    case mesh::TextureSlot::Emissive: return TextureKind::Emissive;
    }
    return TextureKind::BaseColor;
}

} // namespace kiln
