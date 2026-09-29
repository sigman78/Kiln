// registry.cpp — ids, slots and handles, request queues, refcounts, groups and queries.
// Pump thread only.
#include "runtime_internal.h"

#include "kiln/placeholders.h"

namespace kiln {
namespace rt {

void Buffer::allocate(Allocator const* a, usize n, Tag t) noexcept {
    KILN_ASSERT(data == nullptr);
    alloc = a;
    tag   = t;
    size  = n;
    data  = n ? static_cast<u8*>(kiln::alloc(a, n, 16, t)) : nullptr;
}

void Buffer::release() noexcept {
    if (data) kiln::free(alloc, data, size, 16, tag);
    data = nullptr;
    size = 0;
}

// ---------------------------------------------------------------------------
// Paths and ids
// ---------------------------------------------------------------------------

HashMap<AssetId, u32>& map_for(Context* ctx, AssetKind kind) noexcept {
    return kind == AssetKind::Mesh ? ctx->meshMap : ctx->texMap;
}

Slot* resolve(Context* ctx, u64 bits, AssetKind kind) noexcept {
    if (!ctx) return nullptr;
    u32 const index = u32(bits);
    u32 const gen   = u32(bits >> 32);
    if (gen == 0 || index >= ctx->maxAssets) return nullptr;
    Slot& s = ctx->slots[index];
    if (!s.live || s.zombie || s.generation != gen || s.kind != kind) return nullptr;
    return &s;
}

// ---------------------------------------------------------------------------
// Intrusive queues over slot indices
// ---------------------------------------------------------------------------

void queue_push(Context* ctx, QueueId q, Slot& s) noexcept {
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
}

void queue_remove(Context* ctx, Slot& s) noexcept {
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

void boost(Context* ctx, Slot& s) noexcept {
    s.priority = Priority::High;
    if (s.queue == QueueId::MetaNormal) {
        queue_remove(ctx, s);
        queue_push(ctx, QueueId::MetaHigh, s);
    } else if (s.queue == QueueId::UploadNormal) {
        queue_remove(ctx, s);
        queue_push(ctx, QueueId::UploadHigh, s);
    }
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

GroupRec* group_of(Context* ctx, Slot const& s) noexcept {
    if (s.groupIndex == kInvalid) return nullptr;
    GroupRec& g = ctx->groups[s.groupIndex];
    return (g.live && g.generation == s.groupGen) ? &g : nullptr;
}

namespace {

GroupRec* resolve_group(Context* ctx, Group g) noexcept {
    if (!ctx || g.is_null() || g.index >= ctx->maxGroups) return nullptr;
    GroupRec& r = ctx->groups[g.index];
    return (r.live && r.generation == g.generation) ? &r : nullptr;
}

void join_group(Context* ctx, Slot& s, Group g) noexcept {
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
        s.groupBytes = s.next.uploadSize;
        break;
    default: ++r->pending; break;
    }
    r->bytesTotal += s.groupBytes;
}

void leave_group(Context* ctx, Slot& s) noexcept {
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

void free_meta_set(Allocator const* a, MetaSet& m) noexcept {
    m.meta.release();
    if (m.layout) free_array(a, m.layout, usize(m.layoutLevels) * 4, Tag::Payload);
    m = {};
}

MetaSet const* shown_meta(Slot const& s) noexcept {
    if (s.state == State::Ready) return &s.cur;
    if (s.state == State::MetaReady) return &s.next;
    return nullptr;
}

void free_load_data(Slot& s) noexcept {
    free_meta_set(s.ctx->alloc, s.cur);
    free_meta_set(s.ctx->alloc, s.next);
    s.memory.release();
    s.cooked.release();
    s.cookedValid = false;
}

void free_slot(Context* ctx, Slot& s) noexcept {
    KILN_ASSERT(!s.jobInFlight && s.queue == QueueId::None);
    free_load_data(s);
    s.live                               = false;
    s.zombie                             = false;
    s.state                              = State::Unloaded;
    s.phase                              = Phase::Free;
    s.refcount                           = 0;
    s.hasTarget                          = false;
    s.reloading                          = false;
    s.reloadPending                      = false;
    s.realObj                            = {};
    s.bindSlot                           = kInvalid;
    s.bindPending                        = false;
    ctx->freeSlots[ctx->freeSlotCount++] = s.index;
}

Slot* request_slot(Context* ctx, AssetKind kind, StrView path, RequestOptions const& opt, Buffer* memory,
                   bool rejectExisting) noexcept {
    if (char const* why = check_asset_name(path)) {
        diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagBadAssetName, Severity::Error, path,
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
        if (rejectExisting) {
            diagf(&ctx->diag, make_status(Code::AlreadyExists), kDiagDuplicateRegister, Severity::Error, np,
                  "register", "path is already registered or requested");
            return nullptr;
        }
        ++s.refcount;
        if (opt.priority == Priority::High) boost(ctx, s);
        join_group(ctx, s, opt.group);
        return &s;
    }

    if (id <= kLastPlaceholderId) { // practically never; reserved ids (handles-and-states.md)
        diagf(&ctx->diag, make_status(Code::InvalidArgument), kDiagAssetLoadFailed, Severity::Error, np,
              "request", "asset path hashes into the reserved placeholder id range");
        return nullptr;
    }
    if (ctx->freeSlotCount == 0) {
        diagf(&ctx->diag, make_status(Code::Busy), kDiagRegistryFull, Severity::Error, np, "request",
              "registry full (maxAssets = %u)", ctx->maxAssets);
        return nullptr;
    }

    Slot& s = ctx->slots[ctx->freeSlots[--ctx->freeSlotCount]];
    KILN_ASSERT(!s.live);
    s.live          = true;
    s.zombie        = false;
    s.jobInFlight   = false;
    s.id            = id;
    s.kind          = kind;
    s.state         = State::Pending;
    s.phase         = Phase::MetaQueued;
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
    s.hasTarget     = false;
    s.target        = {};
    s.reloading     = false;
    s.reloadPending = false;
    s.groupAs       = State::Pending;
    s.jobStatus     = kOk;
    s.jobStatValid  = false;
    s.jobDiag       = 0;
    s.capture.reset();
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

void unload(Context* ctx, Slot& s) noexcept {
    leave_group(ctx, s);
    if (s.queue == QueueId::Await) orphan_upload(ctx, s);
    queue_remove(ctx, s);
    watch_disarm(ctx, s.index);
    if (s.bindPending) --ctx->bindPendingCount;
    retire(ctx, s.realObj, s.bindSlot);
    s.realObj     = {};
    s.bindSlot    = kInvalid;
    s.bindPending = false;
    map_for(ctx, s.kind).erase(s.id);
    s.state = State::Unloaded;
    // Handles go stale now; the slot is reused only after the in-flight job (if any) completed.
    s.generation = s.generation + 1 == 0 ? 1 : s.generation + 1;
    if (s.jobInFlight)
        s.zombie = true;
    else
        free_slot(ctx, s);
}

void release_impl(Context* ctx, u64 bits, AssetKind kind) noexcept {
    Slot* s = resolve(ctx, bits, kind);
    if (!s) {
        KILN_ASSERT(bits == 0 && "release() of a stale handle (double release?)");
        return;
    }
    KILN_ASSERT(s->refcount > 0);
    if (--s->refcount == 0) unload(ctx, *s);
}

Placeholder const& kind_placeholder(Context* ctx, TextureKind k, TextureShape shape) noexcept {
    return ctx->ph[placeholder_index(k < TextureKind::Count ? k : TextureKind::BaseColor, shape)];
}

/// The placeholder a texture shows in `s`'s state (or for a stale handle when s is null).
Placeholder const& texture_placeholder(Context* ctx, Slot const* s) noexcept {
    bool const failedLook    = !s || s->state == State::Failed;
    TextureShape const shape = s ? s->texShape : TextureShape::Tex2D;
    Placeholder const& fp    = ctx->ph[failed_placeholder_index(shape)];
    if (failedLook && ctx->devPlaceholders && fp.ready) return fp;
    return kind_placeholder(ctx, s ? s->texKind : TextureKind::BaseColor, shape);
}

GpuObject placeholder_obj(Placeholder const& p) noexcept { return p.ready ? p.obj : GpuObject{}; }

void release_now(Context* ctx, GpuObject obj, u32 bindSlot) noexcept {
    if (!obj.is_null()) ctx->adapter.destroy(ctx->adapter.user, obj);
    if (bindSlot != kInvalid) ctx->freeBindSlots.push_back(bindSlot);
}

} // namespace

void retire(Context* ctx, GpuObject obj, u32 bindSlot) noexcept {
    if (obj.is_null() && bindSlot == kInvalid) return;
    if (ctx->completedFrame >= ctx->frame)
        release_now(ctx, obj, bindSlot);
    else
        ctx->retired.push_back({obj, bindSlot, ctx->frame});
}

void process_retired(Context* ctx) noexcept {
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

void orphan_upload(Context* ctx, Slot& s) noexcept {
    if (!s.hasTarget) return;
    ctx->orphans.push_back({s.target.token, s.target.object});
    s.hasTarget = false;
}

void poll_orphans(Context* ctx) noexcept {
    for (usize i = 0; i < ctx->orphans.size();) {
        Orphan const o = ctx->orphans[i];
        if (!ctx->adapter.is_upload_complete(ctx->adapter.user, o.token)) {
            ++i;
            continue;
        }
        ctx->orphans.erase_unordered(i);
        retire(ctx, o.obj, kInvalid);
    }
}

void bind_object(Context* ctx, Slot& s, GpuObject obj) noexcept {
    if (s.bindSlot == kInvalid) return;
    if (s.bindPending) --ctx->bindPendingCount;
    s.bindPending = false;
    ctx->adapter.bind(ctx->adapter.user, s.bindSlot, obj, s.texShape);
}

void bind_placeholder(Context* ctx, Slot& s) noexcept {
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

AssetId asset_id(StrView name) noexcept { return check_asset_name(name) ? 0 : fnv1a64(name); }

MeshHandle request_mesh(Context* ctx, StrView path, RequestOptions const& opt) noexcept {
    if (!ctx) return {};
    Slot* s = request_slot(ctx, AssetKind::Mesh, path, opt, nullptr, false);
    return s ? MeshHandle::from_bits(handle_bits(*s)) : MeshHandle{};
}

TextureHandle request_texture(Context* ctx, StrView path, RequestOptions const& opt) noexcept {
    if (!ctx) return {};
    Slot* s = request_slot(ctx, AssetKind::Texture, path, opt, nullptr, false);
    return s ? TextureHandle::from_bits(handle_bits(*s)) : TextureHandle{};
}

void release(Context* ctx, MeshHandle h) noexcept { release_impl(ctx, h.bits(), AssetKind::Mesh); }
void release(Context* ctx, TextureHandle h) noexcept { release_impl(ctx, h.bits(), AssetKind::Texture); }

MeshHandle find_mesh(Context* ctx, AssetId id) noexcept {
    if (!ctx) return {};
    u32 const* i = ctx->meshMap.find(id);
    return i ? MeshHandle::from_bits(handle_bits(ctx->slots[*i])) : MeshHandle{};
}

TextureHandle find_texture(Context* ctx, AssetId id) noexcept {
    if (!ctx) return {};
    u32 const* i = ctx->texMap.find(id);
    return i ? TextureHandle::from_bits(handle_bits(ctx->slots[*i])) : TextureHandle{};
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

State state(Context* ctx, MeshHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return s ? s->state : State::Unloaded;
}
State state(Context* ctx, TextureHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    return s ? s->state : State::Unloaded;
}
bool has_meta(Context* ctx, MeshHandle h) noexcept {
    State const st = state(ctx, h);
    return st == State::MetaReady || st == State::Ready;
}
bool has_meta(Context* ctx, TextureHandle h) noexcept {
    State const st = state(ctx, h);
    return st == State::MetaReady || st == State::Ready;
}
bool is_ready(Context* ctx, MeshHandle h) noexcept { return state(ctx, h) == State::Ready; }
bool is_ready(Context* ctx, TextureHandle h) noexcept { return state(ctx, h) == State::Ready; }

u32 version(Context* ctx, MeshHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return s ? s->version : 0;
}
u32 version(Context* ctx, TextureHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    return s ? s->version : 0;
}
AssetId id_of(Context* ctx, MeshHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return s ? s->id : 0;
}
AssetId id_of(Context* ctx, TextureHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    return s ? s->id : 0;
}

GpuObject gpu_object(Context* ctx, MeshHandle h) noexcept {
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Mesh);
    return (s && s->state == State::Ready) ? s->realObj : GpuObject{};
}

GpuObject gpu_object(Context* ctx, TextureHandle h) noexcept {
    if (!ctx) return {};
    Slot const* s = resolve(ctx, h.bits(), AssetKind::Texture);
    GpuObject obj = s && s->state == State::Ready ? s->realObj : placeholder_obj(texture_placeholder(ctx, s));
    if (s && s->bindSlot != kInvalid) obj.slot = s->bindSlot;
    return obj;
}

mesh::MeshView const* mesh_view(Context* ctx, MeshHandle h) noexcept {
    Slot const* s    = resolve(ctx, h.bits(), AssetKind::Mesh);
    MetaSet const* m = s ? shown_meta(*s) : nullptr;
    return m ? &m->meshView : nullptr;
}

TextureInfo texture_info(Context* ctx, TextureHandle h) noexcept {
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

Group group(Context* ctx) noexcept {
    if (!ctx) return {};
    if (ctx->freeGroupCount == 0) {
        diagf(&ctx->diag, make_status(Code::Busy), kDiagRegistryFull, Severity::Error, {}, "group",
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

void release(Context* ctx, Group g) noexcept {
    GroupRec* r = resolve_group(ctx, g);
    if (!r) return;
    r->live                                = false;
    r->generation                          = r->generation + 1 == 0 ? 1 : r->generation + 1;
    ctx->freeGroups[ctx->freeGroupCount++] = g.index;
}

GroupStatus progress(Context* ctx, Group g) noexcept {
    GroupRec const* r = resolve_group(ctx, g);
    if (!r) return {};
    return GroupStatus{r->ready, r->failed, r->pending, r->bytesDone, r->bytesTotal};
}

namespace rt {
/// Raise every live member of `g` to High (wait()).
void boost_group(Context* ctx, Group g) noexcept {
    if (!resolve_group(ctx, g)) return;
    for (u32 i = 0; i < ctx->maxAssets; ++i) {
        Slot& s = ctx->slots[i];
        if (s.live && !s.zombie && s.groupIndex == g.index && s.groupGen == g.generation) boost(ctx, s);
    }
}
} // namespace rt

GpuObject placeholder_object(Context* ctx, TextureKind kind, TextureShape shape) noexcept {
    if (!ctx || kind >= TextureKind::Count || shape >= TextureShape::Count) return {};
    return placeholder_obj(ctx->ph[placeholder_index(kind, shape)]);
}

TextureKind texture_kind_for_slot(mesh::TextureSlot slot) noexcept {
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
