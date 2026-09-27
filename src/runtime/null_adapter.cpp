// kiln/null_adapter.cpp — CPU-only adapter for tests and tools. See
// include/kiln/null_adapter.h and docs/design/adapter.md ("Null adapter").
//
// Threading (adapter.md): begin_upload/commit_upload may run on worker threads,
// so the busy/fail call counters are atomics and every table/slot/stat mutation
// is guarded by one mutex. `table` is reserved to `maxObjects` up front and never
// regrows, so the TextureDesc/MeshPayloadDesc owned by each entry (pointed to by
// that same entry's UploadDesc copy) never moves.
#include "kiln/null_adapter.h"

#include "kiln/containers.h"

#include <atomic>
#include <mutex>

namespace kiln {

namespace {

enum class EntryState : u8 { Uploading, Complete, Deferred, Freed };

/// One uploaded object. `native` in the GpuObject kiln is handed back is this
/// entry's 1-based index into NullAdapter::table.
struct ObjectEntry {
    u8* bytes = nullptr; ///< allocated with `align`, valid while state != Freed
    u64 size  = 0;
    u32 align = 1;
    UploadDesc desc{};          ///< copy; texture/mesh below own the pointee
    TextureDesc textureDesc{};  ///< storage for desc.texture when kind == TextureLevels
    MeshPayloadDesc meshDesc{}; ///< storage for desc.mesh when kind == MeshPayload
    EntryState state = EntryState::Freed;
};

} // namespace

struct NullAdapter {
    Allocator const* allocator = nullptr;
    NullAdapterDesc desc{};

    std::mutex mutex;
    Vec<ObjectEntry> table;           ///< table[i] is native (i + 1); reserved, never regrows
    Vec<GpuObject> slots;             ///< bindless slot table
    HashMap<AssetId, u32> assetSlots; ///< id -> slot, bindless only
    NullAdapterStats stats{};

    std::atomic<u32> busyCalls{0}; ///< begin_upload call counter for busyEveryN
    std::atomic<u32> failCalls{0}; ///< begin_upload call counter for failEveryN
};

namespace {

[[nodiscard]] NullAdapter* self(void* user) noexcept { return static_cast<NullAdapter*>(user); }

bool null_supports_format(void* /*user*/, Format f, FormatUsage usage) noexcept {
    FormatInfo const* info = format_info(f);
    if (!info) return false;
    return usage == FormatUsage::VertexBuffer ? !info->compressed : true;
}

void null_copy_constraints(void* user, CopyConstraints* out) noexcept {
    NullAdapter* na           = self(user);
    out->optimalRowPitchAlign = na->desc.rowPitchAlign;
    out->optimalOffsetAlign   = na->desc.offsetAlign;
    out->bufferOffsetAlign    = na->desc.offsetAlign;
}

Status null_acquire(void* user, AssetId id, UploadKind kind, TextureKind /*texKind*/,
                    GpuObject* out) noexcept {
    NullAdapter* na = self(user);
    std::lock_guard<std::mutex> lock(na->mutex);
    ++na->stats.acquires;

    if (!na->desc.bindless) {
        *out = GpuObject{};
        return kOk;
    }

    u32 slot;
    if (u32* found = na->assetSlots.find(id)) {
        slot = *found;
    } else {
        KILN_VERIFY(na->slots.size() < na->desc.maxObjects);
        slot = u32(na->slots.size());
        na->slots.push_back(GpuObject{});
        na->assetSlots.insert(id, slot);
    }
    *out = GpuObject{.native = 0, .slot = slot, .kind = u32(kind)};
    return kOk;
}

Status null_begin_upload(void* user, UploadDesc const& desc, UploadTarget* out) noexcept {
    NullAdapter* na = self(user);
    u32 busyCallNo  = na->busyCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    u32 failCallNo  = na->failCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    std::lock_guard<std::mutex> lock(na->mutex);
    ++na->stats.beginUploads;

    if (na->desc.busyEveryN != 0 && busyCallNo % na->desc.busyEveryN == 0) {
        ++na->stats.busyReturned;
        return make_status(Code::Busy);
    }
    if (na->desc.failEveryN != 0 && failCallNo % na->desc.failEveryN == 0) {
        return make_status(Code::Unsupported);
    }

    KILN_VERIFY(na->table.size() < na->desc.maxObjects);

    usize align = desc.alignment ? usize(desc.alignment) : 1;
    u8* bytes = static_cast<u8*>(alloc(na->allocator, desc.size ? usize(desc.size) : 1, align, Tag::Payload));

    ObjectEntry entry{};
    entry.bytes = bytes;
    entry.size  = desc.size;
    entry.align = u32(align);
    entry.desc  = desc;
    if (desc.kind == UploadKind::TextureLevels && desc.texture) {
        entry.textureDesc  = *desc.texture;
        entry.desc.texture = &entry.textureDesc;
        entry.desc.mesh    = nullptr;
    } else if (desc.kind == UploadKind::MeshPayload && desc.mesh) {
        entry.meshDesc     = *desc.mesh;
        entry.desc.mesh    = &entry.meshDesc;
        entry.desc.texture = nullptr;
    }
    entry.state = EntryState::Uploading;

    na->table.push_back(std::move(entry));
    ObjectEntry& stored = na->table[na->table.size() - 1];
    // push_back bitwise-copies `entry`; re-point the self-referential desc
    // pointers at the stored copy instead of the (about to die) local one.
    if (stored.desc.texture == &entry.textureDesc) stored.desc.texture = &stored.textureDesc;
    if (stored.desc.mesh == &entry.meshDesc) stored.desc.mesh = &stored.meshDesc;

    u64 index          = u64(na->table.size()); // 1-based
    out->dst           = stored.bytes;
    out->rowPitchAlign = na->desc.rowPitchAlign;
    out->token         = index;
    out->object        = GpuObject{.native = index, .slot = kInvalid, .kind = u32(desc.kind)};
    return kOk;
}

void null_commit_upload(void* user, u64 token) noexcept {
    NullAdapter* na = self(user);
    std::lock_guard<std::mutex> lock(na->mutex);
    ++na->stats.commits;
    if (token == 0 || token > u64(na->table.size())) return;
    ObjectEntry& e = na->table[usize(token - 1)];
    if (e.state != EntryState::Uploading) return;
    e.state = EntryState::Complete;
    ++na->stats.completes;
    ++na->stats.liveObjects;
    na->stats.bytesUploaded += e.size;
}

bool null_is_upload_complete(void* user, u64 token) noexcept {
    NullAdapter* na = self(user);
    std::lock_guard<std::mutex> lock(na->mutex);
    if (token == 0 || token > u64(na->table.size())) return false;
    return na->table[usize(token - 1)].state == EntryState::Complete;
}

void null_publish(void* user, AssetId id, GpuObject obj, u32 /*version*/) noexcept {
    NullAdapter* na = self(user);
    std::lock_guard<std::mutex> lock(na->mutex);
    ++na->stats.publishes;
    if (!na->desc.bindless) return;
    u32* slot = na->assetSlots.find(id);
    if (!slot || *slot >= na->slots.size()) return;
    na->slots[*slot] = obj; // a null obj clears the slot
}

void null_destroy_deferred(void* user, GpuObject obj) noexcept {
    NullAdapter* na = self(user);
    std::lock_guard<std::mutex> lock(na->mutex);
    if (obj.native == 0 || obj.native > u64(na->table.size())) return;
    ObjectEntry& e = na->table[usize(obj.native - 1)];
    if (e.state == EntryState::Complete || e.state == EntryState::Uploading) e.state = EntryState::Deferred;
}

} // namespace

Result<NullAdapter*> null_adapter_create(NullAdapterDesc const& desc, Adapter* out) noexcept {
    if (!out) return make_status(Code::InvalidArgument);

    Allocator const* allocator = desc.alloc ? desc.alloc : default_allocator();
    NullAdapter* na            = new_object<NullAdapter>(allocator, Tag::Payload);
    na->allocator              = allocator;
    na->desc                   = desc;
    na->desc.alloc             = allocator;

    na->table.init(allocator, Tag::Payload);
    na->table.reserve(na->desc.maxObjects);
    na->slots.init(allocator, Tag::Payload);
    na->slots.reserve(na->desc.maxObjects);
    na->assetSlots.init(allocator, Tag::Payload);

    *out                    = Adapter{};
    out->supports_format    = &null_supports_format;
    out->copy_constraints   = &null_copy_constraints;
    out->acquire            = &null_acquire;
    out->begin_upload       = &null_begin_upload;
    out->commit_upload      = &null_commit_upload;
    out->is_upload_complete = &null_is_upload_complete;
    out->publish            = &null_publish;
    out->destroy_deferred   = &null_destroy_deferred;
    out->caps               = kSelfSubmitting;
    out->user               = na;

    KILN_ASSERT(adapter_is_valid(*out));
    return na;
}

void null_adapter_destroy(NullAdapter* na) noexcept {
    if (!na) return;
    for (ObjectEntry& e : na->table) {
        if (e.bytes) free(na->allocator, e.bytes, e.size ? usize(e.size) : 1, e.align, Tag::Payload);
    }
    delete_object(na->allocator, na, Tag::Payload);
}

Span<u8 const> null_adapter_payload(NullAdapter* na, GpuObject obj) noexcept {
    if (!na) return {};
    std::lock_guard<std::mutex> lock(na->mutex);
    if (obj.native == 0 || obj.native > u64(na->table.size())) return {};
    ObjectEntry const& e = na->table[usize(obj.native - 1)];
    if (e.state == EntryState::Freed) return {};
    return Span<u8 const>(e.bytes, usize(e.size));
}

GpuObject null_adapter_slot(NullAdapter* na, u32 slot) noexcept {
    if (!na) return {};
    std::lock_guard<std::mutex> lock(na->mutex);
    if (slot >= na->slots.size()) return {};
    return na->slots[slot];
}

NullAdapterStats null_adapter_stats(NullAdapter* na) noexcept {
    if (!na) return {};
    std::lock_guard<std::mutex> lock(na->mutex);
    return na->stats;
}

u32 null_adapter_flush_deferred(NullAdapter* na) noexcept {
    if (!na) return 0;
    std::lock_guard<std::mutex> lock(na->mutex);
    u32 freed = 0;
    for (ObjectEntry& e : na->table) {
        if (e.state != EntryState::Deferred) continue;
        free(na->allocator, e.bytes, e.size ? usize(e.size) : 1, e.align, Tag::Payload);
        e.bytes = nullptr;
        e.size  = 0;
        e.state = EntryState::Freed;
        ++freed;
        ++na->stats.destroys;
        if (na->stats.liveObjects) --na->stats.liveObjects;
    }
    return freed;
}

} // namespace kiln
