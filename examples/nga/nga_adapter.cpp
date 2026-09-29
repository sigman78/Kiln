// examples/nga/nga_adapter.cpp — kiln adapter over NoGraphicsAPI.
// Threads: begin_upload / commit_upload (kiln workers) only touch memory under `mutex`. NoGraphicsAPI
// calls (flush, upload_status, bind, destroy) run on the pump thread, which also submits the
// host's frames to queue 0.
#include "nga_adapter.h"

#include "staging_ring.h"
#include "upload_pool.h"

#include <kiln/alloc.h>
#include <kiln/containers.h>
#include <kiln/log.h>

#include <mutex>

namespace kiln::nga {
namespace {

constexpr u64 kOffsetAlign  = 16;  ///< per-level offset inside a texture upload
constexpr u64 kUploadAlign  = 256; ///< start of every staging upload and mesh payload
constexpr u32 kMaxLevels    = 16;
constexpr u32 kCommandPools = 4; ///< flush submissions in flight before one is reused

enum class ObjectKind : u32 { Texture = 1, Mesh = 2 };

/// First-fit allocator over [0, size) for the mesh heap and the texture heap.
struct RangeAllocator {
    struct Range {
        u64 offset = 0, size = 0;
    };
    Vec<Range> free; ///< sorted by offset, never adjacent

    void insert_at(usize i, Range r) {
        free.push_back(r);
        for (usize j = free.size() - 1; j > i; --j)
            free[j] = free[j - 1];
        free[i] = r;
    }

    void init(u64 size) { free.push_back(Range{0, size}); }
    bool alloc(u64 size, u64 align, u64* offset) {
        for (usize i = 0; i < free.size(); ++i) {
            Range& r        = free[i];
            u64 const start = align_up(r.offset, align);
            if (start + size > r.offset + r.size) continue;
            u64 const end = r.offset + r.size;
            *offset       = start;
            if (start > r.offset && start + size < end) { // split in two
                r.size = start - r.offset;
                insert_at(i + 1, Range{start + size, end - start - size});
            } else if (start > r.offset) {
                r.size = start - r.offset;
            } else if (start + size < end) {
                r = Range{start + size, end - start - size};
            } else {
                free.erase(i);
            }
            return true;
        }
        return false;
    }
    void release(u64 offset, u64 size) {
        usize i = 0;
        while (i < free.size() && free[i].offset < offset)
            ++i;
        insert_at(i, Range{offset, size});
        if (i + 1 < free.size() && free[i].offset + free[i].size == free[i + 1].offset) { // merge right
            free[i].size += free[i + 1].size;
            free.erase(i + 1);
        }
        if (i > 0 && free[i - 1].offset + free[i - 1].size == free[i].offset) { // merge left
            free[i - 1].size += free[i].size;
            free.erase(i);
        }
    }
};

struct Object {
    ObjectKind kind       = ObjectKind::Texture;
    gpu::Texture* texture = nullptr;
    u64 heapOffset        = 0; ///< in the texture heap or the mesh heap
    u64 heapSize          = 0;
    u32 descriptor        = kInvalid;
    u32 gen               = 1;
    bool used             = false;
};

/// One upload's data; its state and token are the pool's (ex::UploadPool).
struct Upload {
    u32 object      = 0;
    UploadKind kind = UploadKind::MeshPayload;
    TextureDesc tex{};
    u64 size          = 0;
    u64 stagingOffset = 0;
    u32 span          = 0; ///< staging ring entry (textures)
    u64 value         = 0; ///< timeline value of the submission that copies it
};

gpu::Format gpu_format(Format f) noexcept {
    switch (f) {
    case Format::R8_UNORM: return gpu::Format::r8_unorm;
    case Format::R8G8_UNORM: return gpu::Format::rg8_unorm;
    case Format::R8G8B8A8_UNORM: return gpu::Format::rgba8_unorm;
    case Format::R8G8B8A8_SRGB: return gpu::Format::rgba8_srgb;
    case Format::R16_UNORM: return gpu::Format::r16_unorm;
    case Format::R16G16_UNORM: return gpu::Format::rg16_unorm;
    case Format::R16G16B16A16_UNORM: return gpu::Format::rgba16_unorm;
    case Format::R16_SFLOAT: return gpu::Format::r16_float;
    case Format::R16G16_SFLOAT: return gpu::Format::rg16_float;
    case Format::R16G16B16A16_SFLOAT: return gpu::Format::rgba16_float;
    case Format::R32_SFLOAT: return gpu::Format::r32_float;
    case Format::R32G32_SFLOAT: return gpu::Format::rg32_float;
    case Format::R32G32B32A32_SFLOAT: return gpu::Format::rgba32_float;
    default: return gpu::Format::undefined; // SNORM has no NoGraphicsAPI format; BCn arrives with v0.6
    }
}

} // namespace

struct NgaAdapter {
    NgaAdapterDesc desc{};
    gpu::Device* device = nullptr;
    std::mutex mutex;

    gpu::GpuHeap staging{};
    ex::StagingRing ring; ///< reservations in `staging`, released when their copy completed

    gpu::GpuHeap meshHeap{};
    RangeAllocator meshRanges;
    gpu::TextureHeap textureHeap{};
    RangeAllocator textureRanges;
    gpu::TextureDescriptorHeap* descriptors = nullptr;
    gpu::SamplerDescriptorHeap* samplers    = nullptr;
    Vec<u32> freeDescriptors;

    Vec<Object> objects;
    Vec<u32> freeObjects;
    ex::UploadPool<Upload> uploads;
    Vec<u32> committed;
    Vec<u32> flushing;
    Vec<u32> inFlight;

    gpu::TimelineSemaphore* timeline       = nullptr;
    u64 lastValue                          = 0;
    gpu::CommandPool* pools[kCommandPools] = {};
    u64 poolValues[kCommandPools]          = {};
    u32 nextPool                           = 0;

    /// kiln's slot -> the descriptor it shows now. A descriptor is written once; bind() moves a slot.
    Vec<u32> slotDescriptor;
};

namespace {

void release_object(NgaAdapter* a, u32 index) noexcept {
    Object& o = a->objects[index];
    if (o.texture) gpu::destroy_texture(o.texture);
    std::lock_guard<std::mutex> const lock(a->mutex);
    if (o.kind == ObjectKind::Texture && o.heapSize) a->textureRanges.release(o.heapOffset, o.heapSize);
    if (o.kind == ObjectKind::Mesh && o.heapSize) a->meshRanges.release(o.heapOffset, o.heapSize);
    if (o.descriptor != kInvalid) a->freeDescriptors.push_back(o.descriptor);
    o = Object{ObjectKind::Texture, nullptr, 0, 0, kInvalid, o.gen + 1, false};
    a->freeObjects.push_back(index);
}

void free_upload(NgaAdapter* a, u32 index) noexcept {
    std::lock_guard<std::mutex> const lock(a->mutex);
    Upload& u = a->uploads[index];
    if (u.kind == UploadKind::TextureLevels) a->ring.release(u.span);
    a->uploads.release(index);
}

gpu::CommandPool* next_pool(NgaAdapter* a) noexcept {
    u32 const i = a->nextPool;
    a->nextPool = (a->nextPool + 1) % kCommandPools;
    if (a->poolValues[i])
        gpu::wait_timeline({a->timeline, a->poolValues[i]}); // four flushes back: done by now
    gpu::reset_command_pool(a->pools[i]);
    return a->pools[i];
}

enum class Record : u8 { Done, Retry, Failed };

/// Records one texture upload: placement, creation, a copy per level, its descriptor. Retry when the
/// heap or the descriptors are full now; Failed when the texture can never be made. What a failed
/// upload reserved is freed by destroy().
Record record_texture(NgaAdapter* a, gpu::CommandBuffer* cmd, Upload& u) noexcept {
    Object& o            = a->objects[u.object];
    TextureDesc const& t = u.tex;
    gpu::TextureDesc d{};
    d.type                  = t.shape == TextureShape::Cube    ? gpu::TextureType::cube
                              : t.shape == TextureShape::Array ? gpu::TextureType::two_d_array
                                                               : gpu::TextureType::two_d;
    d.extent                = {.x = t.width, .y = t.height, .z = 1};
    d.mip_levels            = t.levels;
    d.layer_count           = t.layers; // cube faces are layers
    d.format                = gpu_format(t.format);
    d.usage                 = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination;
    gpu::SizeAlign const sa = gpu::get_texture_size_align(a->device, d);
    u64 offset              = 0;
    if (sa.size > a->desc.textureBytes) {
        KILN_ERROR("nga", "texture %ux%u needs %llu bytes; the texture heap has %llu", t.width, t.height,
                   static_cast<unsigned long long>(sa.size),
                   static_cast<unsigned long long>(a->desc.textureBytes));
        return Record::Failed;
    }
    {
        std::lock_guard<std::mutex> const lock(a->mutex);
        if (a->freeDescriptors.empty() || !a->textureRanges.alloc(sa.size, sa.align, &offset))
            return Record::Retry;
        o.descriptor = a->freeDescriptors.back();
        a->freeDescriptors.pop_back();
    }
    o.heapOffset = offset;
    o.heapSize   = sa.size;
    o.texture    = gpu::create_texture(cmd, d, a->textureHeap, offset);
    if (!o.texture) {
        KILN_ERROR("nga", "creating a %ux%u texture failed", t.width, t.height);
        return Record::Failed;
    }
    CopyConstraints const cc{
        .optimalRowPitchAlign = 1, .optimalOffsetAlign = kOffsetAlign, .bufferOffsetAlign = kUploadAlign};
    u64 offsets[kMaxLevels];
    u64 const total = texture_level_layout(t, cc, offsets, nullptr);
    for (u32 i = 0; i < t.levels; ++i) {
        u64 const end   = i + 1 < t.levels ? offsets[i + 1] : total;
        auto* const src = a->staging.range.gpu + u.stagingOffset + offsets[i];
        gpu::copy_memory_to_texture(cmd, gpu::GpuRange{src, end - offsets[i]}, o.texture, {.mip_level = i});
    }
    // A fresh descriptor index: no frame can be reading it, so it may be written now.
    gpu::write_texture_descriptor(a->descriptors, o.descriptor, o.texture,
                                  gpu::TextureDescriptorType::sampled);
    return Record::Done;
}

/// True once the upload is Complete or Failed; an InFlight copy completes when the timeline
/// passes its submission.
bool poll(NgaAdapter* a, u32 index) noexcept {
    ex::UploadState const st = a->uploads.state(index);
    if (st != ex::UploadState::InFlight)
        return st == ex::UploadState::Complete || st == ex::UploadState::Failed;
    if (gpu::timeline_completed_value(a->timeline) < a->uploads[index].value) return false;
    a->uploads.advance(index, ex::UploadState::Complete);
    return true;
}

// --- Adapter callbacks ---------------------------------------------------------------------------

bool supports_format(void* user, Format f, FormatUsage usage) {
    auto* a = static_cast<NgaAdapter*>(user);
    if (usage == FormatUsage::VertexBuffer) // the shaders pull float streams; vertex color is not read
        return f == Format::R32G32_SFLOAT || f == Format::R32G32B32_SFLOAT ||
               f == Format::R32G32B32A32_SFLOAT || f == Format::R8G8B8A8_UNORM;
    gpu::Format const g = gpu_format(f);
    return g != gpu::Format::undefined &&
           gpu::supports_texture_format(a->device, g,
                                        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination);
}

void copy_constraints(void*, CopyConstraints* out) {
    out->optimalRowPitchAlign = 1; // copies take tightly packed rows (row_pitch_bytes 0)
    out->optimalOffsetAlign   = kOffsetAlign;
    out->bufferOffsetAlign    = kUploadAlign;
}

Status begin_upload(void* user, UploadDesc const& desc, UploadTarget* out) {
    auto* a              = static_cast<NgaAdapter*>(user);
    bool const isTexture = desc.kind == UploadKind::TextureLevels;
    if (isTexture && (!desc.texture || desc.texture->levels > kMaxLevels || desc.texture->depth > 1 ||
                      gpu_format(desc.texture->format) == gpu::Format::undefined))
        return make_status(Code::Unsupported);
    u64 const size = max<u64>(desc.size, 1);
    u64 const room = isTexture ? a->ring.size() : a->desc.meshBytes;
    if (size > room) { // can never fit: not Busy
        KILN_ERROR("nga", "upload of %llu bytes exceeds the %llu-byte %s",
                   static_cast<unsigned long long>(size), static_cast<unsigned long long>(room),
                   isTexture ? "staging ring" : "mesh heap");
        return make_status(Code::Unsupported);
    }
    std::lock_guard<std::mutex> const lock(a->mutex);
    if (a->freeObjects.empty()) { // held by live assets: waiting would not free one
        KILN_ERROR("nga", "all %u objects are in use", u32(a->objects.size()));
        return make_status(Code::OutOfMemory);
    }
    if (a->uploads.full()) return make_status(Code::Busy);
    u64 offset = 0;
    ex::StagingRing::Reservation r;
    if (isTexture) {
        r      = a->ring.reserve(size, kUploadAlign);
        offset = r.offset;
    }
    if (isTexture ? !r.ok() : !a->meshRanges.alloc(size, kUploadAlign, &offset))
        return make_status(Code::Busy);

    u32 const ui = a->uploads.acquire();
    u32 const oi = a->freeObjects.back();
    a->freeObjects.pop_back();
    Object& o = a->objects[oi];
    o.used    = true;
    o.kind    = isTexture ? ObjectKind::Texture : ObjectKind::Mesh;
    if (!isTexture) { // the payload lives where kiln writes it: no copy
        o.heapOffset = offset;
        o.heapSize   = size;
    }
    Upload& u       = a->uploads[ui];
    u.object        = oi;
    u.kind          = desc.kind;
    u.tex           = isTexture ? *desc.texture : TextureDesc{};
    u.size          = desc.size;
    u.stagingOffset = offset;
    if (isTexture) u.span = r.id;

    out->dst           = (isTexture ? a->staging.range.cpu : a->meshHeap.range.cpu) + offset;
    out->rowPitchAlign = 1;
    out->token         = a->uploads.token(ui);
    out->object        = GpuObject{.native = oi + 1, .slot = kInvalid, .kind = u32(o.kind)};
    return kOk;
}

void commit_upload(void* user, u64 token) {
    auto* a = static_cast<NgaAdapter*>(user);
    std::lock_guard<std::mutex> const lock(a->mutex);
    u32 const i = a->uploads.index_of(token);
    if (i == kInvalid) return;
    a->uploads.advance(i, ex::UploadState::Committed);
    a->committed.push_back(i);
}

UploadStatus upload_status(void* user, u64 token) {
    auto* a         = static_cast<NgaAdapter*>(user);
    u32 const index = a->uploads.index_of(token);
    if (index == kInvalid) return UploadStatus::Failed; // not an upload of this adapter
    if (!poll(a, index)) return UploadStatus::Pending;
    UploadStatus const st = ex::status_of(a->uploads.state(index));
    for (u32 i = 0; i < a->inFlight.size(); ++i)
        if (a->inFlight[i] == index) {
            a->inFlight[i] = a->inFlight.back();
            a->inFlight.pop_back();
            break;
        }
    free_upload(a, index);
    return st;
}

/// Frames recorded earlier hold descriptor indices, not slots, so moving a slot is safe at any time.
void bind(void* user, u32 slot, GpuObject obj, TextureShape) {
    auto* a = static_cast<NgaAdapter*>(user);
    if (obj.native == 0 || obj.native > a->objects.size()) return;
    std::lock_guard<std::mutex> const lock(a->mutex);
    a->slotDescriptor[slot] = a->objects[obj.native - 1].descriptor;
}

/// kiln calls this once the frames that could read the object's descriptor completed.
void destroy(void* user, GpuObject obj) {
    auto* a = static_cast<NgaAdapter*>(user);
    if (obj.native == 0 || obj.native > a->objects.size() || !a->objects[obj.native - 1].used) return;
    release_object(a, u32(obj.native - 1));
}

/// Records and submits the copies of every committed texture upload; mesh payloads are already in
/// place. kiln calls this at the start of each pump().
void flush(void* user) {
    auto* a = static_cast<NgaAdapter*>(user);
    for (u32 i = 0; i < a->inFlight.size();) {
        if (poll(a, a->inFlight[i])) {
            a->inFlight[i] = a->inFlight.back();
            a->inFlight.pop_back();
        } else {
            ++i;
        }
    }
    {
        std::lock_guard<std::mutex> const lock(a->mutex);
        a->flushing.clear();
        for (u32 i : a->committed)
            a->flushing.push_back(i);
        a->committed.clear();
    }
    gpu::CommandBuffer* cmd = nullptr;
    u64 const value         = a->lastValue + 1;
    for (u32 n = 0; n < a->flushing.size(); ++n) {
        u32 const i = a->flushing[n];
        Upload& u   = a->uploads[i];
        if (u.kind == UploadKind::MeshPayload) { // written in place by kiln; visible to later submissions
            a->uploads.advance(i, ex::UploadState::Complete);
            continue;
        }
        if (!cmd) cmd = gpu::begin_commands(next_pool(a));
        Record const r = record_texture(a, cmd, u);
        if (r == Record::Retry) { // heap or descriptors full: try again next flush
            std::lock_guard<std::mutex> const lock(a->mutex);
            a->committed.push_back(i);
            continue;
        }
        if (r == Record::Failed) { // no GPU work: done at once
            a->uploads.advance(i, ex::UploadState::Failed);
            continue;
        }
        u.value = value;
        a->uploads.advance(i, ex::UploadState::InFlight);
        a->inFlight.push_back(i);
    }
    if (!cmd) return;
    // Make the copies visible to every later submission's shaders.
    gpu::barrier(cmd, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::all_commands,
                 gpu::Access::shader_read);
    gpu::end_commands(cmd);
    a->lastValue                                                     = value;
    a->poolValues[(a->nextPool + kCommandPools - 1) % kCommandPools] = value;
    gpu::CommandBuffer* const cmds[]                                 = {cmd};
    gpu::submit(a->device, {
                               .commands = {cmds,        1    },
                                 .completion = {a->timeline, value}
    });
}

} // namespace

Result<NgaAdapter*> nga_adapter_create(NgaAdapterDesc const& desc, Adapter* out) noexcept {
    if (!out || !desc.device || desc.maxSlots == 0 || desc.maxDescriptors == 0)
        return make_status(Code::InvalidArgument);
    auto* a        = new_object<NgaAdapter>(default_allocator(), Tag::Payload);
    a->desc        = desc;
    a->device      = desc.device;
    a->staging     = gpu::create_gpu_heap(a->device, desc.stagingBytes, gpu::MemoryType::cpu_visible);
    a->meshHeap    = gpu::create_gpu_heap(a->device, desc.meshBytes, gpu::MemoryType::cpu_visible);
    a->textureHeap = gpu::create_texture_heap(a->device, desc.textureBytes);
    a->descriptors = gpu::create_texture_descriptor_heap(a->device, desc.maxDescriptors);
    a->samplers    = gpu::create_sampler_descriptor_heap(a->device, 2);
    a->timeline    = gpu::create_timeline_semaphore(a->device);
    for (gpu::CommandPool*& p : a->pools)
        p = gpu::create_command_pool(a->device);
    if (!a->staging.range.cpu || !a->meshHeap.range.cpu || !a->textureHeap.owner || !a->descriptors ||
        !a->samplers || !a->timeline) {
        KILN_ERROR("nga", "creating the adapter's heaps failed (CPU-visible memory: %llu MiB)",
                   static_cast<unsigned long long>((desc.stagingBytes + desc.meshBytes) >> 20));
        nga_adapter_destroy(a);
        return make_status(Code::OutOfMemory);
    }
    gpu::write_sampler_descriptor(a->samplers, 0, {.anisotropic = true});
    gpu::write_sampler_descriptor(a->samplers, 1,
                                  {.address_u = gpu::AddressMode::clamp_to_edge,
                                   .address_v = gpu::AddressMode::clamp_to_edge,
                                   .address_w = gpu::AddressMode::clamp_to_edge});

    u32 constexpr kMaxObjects = 8192, kMaxUploads = 256;
    a->meshRanges.init(desc.meshBytes);
    a->textureRanges.init(desc.textureBytes);
    a->objects.resize(kMaxObjects);
    a->uploads = ex::UploadPool<Upload>(default_allocator(), kMaxUploads);
    a->ring    = ex::StagingRing(default_allocator(), a->staging.range.size, kMaxUploads);
    for (u32 i = kMaxObjects; i-- > 0;)
        a->freeObjects.push_back(i);
    for (u32 i = desc.maxDescriptors; i-- > 0;)
        a->freeDescriptors.push_back(i);
    a->slotDescriptor.resize(desc.maxSlots, kInvalid);
    a->committed.reserve(kMaxUploads);
    a->flushing.reserve(kMaxUploads);
    a->inFlight.reserve(kMaxUploads);

    *out = Adapter{
        .supports_format  = &supports_format,
        .copy_constraints = &copy_constraints,
        .begin_upload     = &begin_upload,
        .commit_upload    = &commit_upload,
        .upload_status    = &upload_status,
        .bind             = &bind,
        .destroy          = &destroy,
        .flush            = &flush,
        .caps             = kCubeTextures | kArrayTextures | kMeshes,
        .bindlessSlots    = desc.maxSlots,
        .reserved         = {},
        .user             = a,
    };
    return a;
}

void nga_adapter_destroy(NgaAdapter* a) noexcept {
    if (!a) return;
    if (a->timeline && a->lastValue) gpu::wait_timeline({a->timeline, a->lastValue});
    for (u32 i = 0; i < a->objects.size(); ++i)
        if (a->objects[i].used) release_object(a, i);
    for (gpu::CommandPool* p : a->pools)
        if (p) gpu::destroy_command_pool(p);
    if (a->timeline) gpu::destroy_timeline_semaphore(a->timeline);
    if (a->samplers) gpu::destroy_sampler_descriptor_heap(a->samplers);
    if (a->descriptors) gpu::destroy_texture_descriptor_heap(a->descriptors);
    if (a->textureHeap.owner) gpu::destroy_texture_heap(a->textureHeap);
    if (a->meshHeap.owner) gpu::destroy_gpu_heap(a->meshHeap);
    if (a->staging.owner) gpu::destroy_gpu_heap(a->staging);
    delete_object(default_allocator(), a, Tag::Payload);
}

gpu::TextureDescriptorHeap* nga_texture_heap(NgaAdapter* a) noexcept { return a->descriptors; }
gpu::SamplerDescriptorHeap* nga_sampler_heap(NgaAdapter* a) noexcept { return a->samplers; }

u32 nga_descriptor(NgaAdapter* a, u32 slot) noexcept {
    std::lock_guard<std::mutex> const lock(a->mutex);
    return slot < a->slotDescriptor.size() ? a->slotDescriptor[slot] : kInvalid;
}

NgaMesh nga_mesh(NgaAdapter* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size() || obj.kind != u32(ObjectKind::Mesh)) return {};
    Object const& o = a->objects[obj.native - 1];
    if (!o.used) return {};
    return NgaMesh{.gpu = reinterpret_cast<u64>(a->meshHeap.range.gpu + o.heapOffset), .size = o.heapSize};
}

} // namespace kiln::nga
