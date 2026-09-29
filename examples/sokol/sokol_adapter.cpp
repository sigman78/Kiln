// examples/sokol/sokol_adapter.cpp — kiln adapter over sokol_gfx.
// Threads: begin_upload / commit_upload run on kiln workers and only touch memory under `mutex`.
// Every sokol call (flush, destroy) runs on the frame thread, which is the pump thread.
#include "sokol_adapter.h"

#include <kiln/alloc.h>
#include <kiln/containers.h>
#include <kiln/log.h>

#include "commit_queue.h"
#include "upload_pool.h"

#include <mutex>

namespace kiln::sk {
namespace {

constexpr u32 kMaxLevels = 16; // SG_MAX_MIPMAPS

enum class ObjectKind : u32 { Texture = 1, Buffer = 2 };

struct Object {
    sg_image image{};
    sg_view view{};
    sg_buffer buffer{};
    u32 gen   = 1;
    bool used = false;
};

/// One upload's data; its state and token are the pool's (ex::UploadPool).
struct Upload {
    u32 object      = 0;
    UploadKind kind = UploadKind::MeshPayload;
    TextureDesc tex{};
    u8* bytes = nullptr; ///< what kiln wrote; sokol copies it at creation
    u64 size  = 0;
};

sg_pixel_format pixel_format(Format f) noexcept {
    switch (f) {
    case Format::R8_UNORM: return SG_PIXELFORMAT_R8;
    case Format::R8_SNORM: return SG_PIXELFORMAT_R8SN;
    case Format::R8G8_UNORM: return SG_PIXELFORMAT_RG8;
    case Format::R8G8_SNORM: return SG_PIXELFORMAT_RG8SN;
    case Format::R8G8B8A8_UNORM: return SG_PIXELFORMAT_RGBA8;
    case Format::R8G8B8A8_SNORM: return SG_PIXELFORMAT_RGBA8SN;
    case Format::R8G8B8A8_SRGB: return SG_PIXELFORMAT_SRGB8A8;
    case Format::R16_UNORM: return SG_PIXELFORMAT_R16;
    case Format::R16_SNORM: return SG_PIXELFORMAT_R16SN;
    case Format::R16_SFLOAT: return SG_PIXELFORMAT_R16F;
    case Format::R16G16_UNORM: return SG_PIXELFORMAT_RG16;
    case Format::R16G16_SNORM: return SG_PIXELFORMAT_RG16SN;
    case Format::R16G16_SFLOAT: return SG_PIXELFORMAT_RG16F;
    case Format::R16G16B16A16_UNORM: return SG_PIXELFORMAT_RGBA16;
    case Format::R16G16B16A16_SNORM: return SG_PIXELFORMAT_RGBA16SN;
    case Format::R16G16B16A16_SFLOAT: return SG_PIXELFORMAT_RGBA16F;
    case Format::R32_SFLOAT: return SG_PIXELFORMAT_R32F;
    case Format::R32G32_SFLOAT: return SG_PIXELFORMAT_RG32F;
    case Format::R32G32B32A32_SFLOAT: return SG_PIXELFORMAT_RGBA32F;
    default: return SG_PIXELFORMAT_NONE; // RGB8 has no sokol format; BCn arrives with v0.6
    }
}

} // namespace

struct SokolAdapter {
    Allocator const* alloc = nullptr;
    std::mutex mutex;
    ex::AdapterStats stats; ///< the counters, stagingUsed included (guarded by mutex)
    Vec<Object> objects;
    Vec<u32> freeObjects;
    ex::UploadPool<Upload> uploads;
    ex::CommitQueue commits; ///< committed uploads, waiting for flush
};

namespace {

/// Frees the bytes kiln wrote; sokol has copied them, or never will.
void free_bytes(SokolAdapter* a, Upload& u) noexcept {
    if (!u.bytes) return;
    usize const n = usize(max<u64>(u.size, 1));
    kiln::free(a->alloc, u.bytes, n, 16, Tag::Payload);
    u.bytes = nullptr;
    std::lock_guard<std::mutex> const lock(a->mutex);
    a->stats.stagingUsed -= n;
}

void release_object(SokolAdapter* a, u32 index) noexcept {
    Object& o = a->objects[index];
    if (o.view.id) sg_destroy_view(o.view);
    if (o.image.id) sg_destroy_image(o.image);
    if (o.buffer.id) sg_destroy_buffer(o.buffer);
    o = Object{{}, {}, {}, o.gen + 1, false};
    std::lock_guard<std::mutex> const lock(a->mutex);
    a->freeObjects.push_back(index);
}

/// The sokol side of one committed upload. sokol copies the data, so the bytes can go at once.
/// False (logged; nothing left behind) when sokol rejects the buffer, the image or its view.
bool make_object(SokolAdapter* a, Upload const& u) noexcept {
    Object& o = a->objects[u.object];
    if (u.kind == UploadKind::MeshPayload) {
        sg_buffer_desc d{};
        d.size                = usize(u.size);
        d.usage.vertex_buffer = true; // one payload holds the vertices and the indices
        d.usage.index_buffer  = true;
        d.usage.immutable     = true;
        d.data                = sg_range{u.bytes, usize(u.size)};
        d.label               = "kiln mesh";
        o.buffer              = sg_make_buffer(&d);
        if (sg_query_buffer_state(o.buffer) == SG_RESOURCESTATE_VALID) return true;
        KILN_ERROR("sokol", "mesh buffer of %llu bytes: creation failed",
                   static_cast<unsigned long long>(u.size));
        sg_destroy_buffer(o.buffer);
        o.buffer = {};
        return false;
    }
    TextureDesc const& t = u.tex;
    sg_image_desc d{};
    d.type         = t.shape == TextureShape::Cube    ? SG_IMAGETYPE_CUBE
                     : t.shape == TextureShape::Array ? SG_IMAGETYPE_ARRAY
                                                      : SG_IMAGETYPE_2D;
    d.width        = int(t.width);
    d.height       = int(t.height);
    d.num_slices   = t.shape == TextureShape::Tex2D ? 1 : int(t.layers); // 6 for a cube
    d.num_mipmaps  = int(t.levels);
    d.pixel_format = pixel_format(t.format);
    d.label        = "kiln texture";
    // Offset alignment 1 (copy_constraints): the levels are contiguous, each one all its slices.
    CopyConstraints const cc{.optimalRowPitchAlign = 1, .optimalOffsetAlign = 1, .bufferOffsetAlign = 1};
    u64 offsets[kMaxLevels];
    u64 const total = texture_level_layout(t, cc, offsets, nullptr);
    for (u32 i = 0; i < t.levels; ++i) {
        u64 const end        = i + 1 < t.levels ? offsets[i + 1] : total;
        d.data.mip_levels[i] = sg_range{u.bytes + offsets[i], usize(end - offsets[i])};
    }
    o.image = sg_make_image(&d);
    if (sg_query_image_state(o.image) != SG_RESOURCESTATE_VALID) { // sokol logged why
        KILN_ERROR("sokol", "image %ux%u, %u layer(s), %u level(s), %s: creation failed", t.width, t.height,
                   t.layers, t.levels, format_info(t.format) ? format_info(t.format)->name : "?");
        sg_destroy_image(o.image);
        o.image = {};
        return false;
    }
    sg_view_desc v{};
    v.texture.image = o.image;
    o.view          = sg_make_view(&v);
    if (sg_query_view_state(o.view) == SG_RESOURCESTATE_VALID) return true;
    KILN_ERROR("sokol", "view of a %ux%u image: creation failed", t.width, t.height);
    sg_destroy_view(o.view);
    sg_destroy_image(o.image);
    o.view  = {};
    o.image = {};
    return false;
}

// --- Adapter callbacks ---------------------------------------------------------------------------

bool supports_format(void*, Format f, FormatUsage usage) {
    if (usage == FormatUsage::VertexBuffer) return sokol_vertex_format(f) != SG_VERTEXFORMAT_INVALID;
    sg_pixel_format const p = pixel_format(f);
    if (p == SG_PIXELFORMAT_NONE) return false;
    sg_pixelformat_info const info = sg_query_pixelformat(p);
    return info.sample && info.filter;
}

void copy_constraints(void*, CopyConstraints* out) {
    out->optimalRowPitchAlign = 1; // sokol takes tightly packed rows
    out->optimalOffsetAlign   = 1; // and one contiguous range per level
    out->bufferOffsetAlign    = 16;
}

Status begin_upload(void* user, UploadDesc const& desc, UploadTarget* out) {
    auto* a = static_cast<SokolAdapter*>(user);
    if (desc.kind == UploadKind::TextureLevels &&
        (!desc.texture || desc.texture->levels > kMaxLevels || desc.texture->depth > 1))
        return make_status(Code::Unsupported);
    u32 ui = 0, oi = 0;
    {
        std::lock_guard<std::mutex> const lock(a->mutex);
        if (a->freeObjects.empty()) { // held by live assets: waiting would not free one
            KILN_ERROR("sokol", "all %u objects are in use", u32(a->objects.size()));
            return make_status(Code::OutOfMemory);
        }
        ui = a->uploads.acquire();
        if (ui == kInvalid) {
            ++a->stats.busyUploads;
            return make_status(Code::Busy);
        }
        a->stats.stagingUsed += max<u64>(desc.size, 1);
        oi = a->freeObjects.back();
        a->freeObjects.pop_back();
        a->objects[oi].used = true;
    }
    Upload& u = a->uploads[ui];
    u.object  = oi;
    u.kind    = desc.kind;
    u.tex     = desc.texture ? *desc.texture : TextureDesc{};
    u.size    = desc.size;
    u.bytes   = static_cast<u8*>(kiln::alloc(a->alloc, usize(max<u64>(desc.size, 1)), 16, Tag::Payload));

    out->dst           = u.bytes;
    out->rowPitchAlign = 1;
    out->token         = a->uploads.token(ui);
    ObjectKind const k = desc.kind == UploadKind::MeshPayload ? ObjectKind::Buffer : ObjectKind::Texture;
    out->object        = GpuObject{.native = oi + 1, .slot = kInvalid, .kind = u32(k)};
    return kOk;
}

void commit_upload(void* user, u64 token) {
    auto* a = static_cast<SokolAdapter*>(user);
    u32 i   = kInvalid;
    {
        std::lock_guard<std::mutex> const lock(a->mutex);
        i = a->uploads.index_of(token);
        if (i == kInvalid) return;
        a->uploads.advance(i, ex::UploadState::Committed);
        a->stats.bytesCommitted += a->uploads[i].size;
    }
    a->commits.push(i);
}

/// sokol creates a resource at once and orders its use itself: an upload is done once flushed.
UploadStatus upload_status(void* user, u64 token) {
    auto* a     = static_cast<SokolAdapter*>(user);
    u32 const i = a->uploads.index_of(token);
    if (i == kInvalid) return UploadStatus::Failed; // not an upload of this adapter
    UploadStatus const st = ex::status_of(a->uploads.state(i));
    if (st != UploadStatus::Pending) {
        std::lock_guard<std::mutex> const lock(a->mutex);
        a->uploads.release(i);
    }
    return st;
}

/// sokol defers the release of a resource that in-flight frames use, so the host reports no frames.
void destroy(void* user, GpuObject obj) {
    auto* a = static_cast<SokolAdapter*>(user);
    if (obj.native == 0 || obj.native > a->objects.size() || !a->objects[obj.native - 1].used) return;
    release_object(a, u32(obj.native - 1));
}

/// Makes the images and buffers of every committed upload. kiln calls this at the start of each
/// pump(), which the host calls from the sokol_app frame callback.
void flush(void* user) {
    auto* a = static_cast<SokolAdapter*>(user);
    for (u32 i : a->commits.take()) {
        Upload& u     = a->uploads[i];
        bool const ok = make_object(a, u);
        free_bytes(a, u);
        if (!ok) {
            std::lock_guard<std::mutex> const lock(a->mutex);
            ++a->stats.uploadsFailed;
        }
        // A failure reaches kiln through upload_status: it fails the asset (K5004).
        a->uploads.advance(i, ok ? ex::UploadState::Complete : ex::UploadState::Failed);
    }
}

} // namespace

sg_vertex_format sokol_vertex_format(Format f) noexcept {
    switch (f) {
    case Format::R32_SFLOAT: return SG_VERTEXFORMAT_FLOAT;
    case Format::R32G32_SFLOAT: return SG_VERTEXFORMAT_FLOAT2;
    case Format::R32G32B32_SFLOAT: return SG_VERTEXFORMAT_FLOAT3;
    case Format::R32G32B32A32_SFLOAT: return SG_VERTEXFORMAT_FLOAT4;
    case Format::R16G16_SFLOAT: return SG_VERTEXFORMAT_HALF2;
    case Format::R16G16_SNORM: return SG_VERTEXFORMAT_SHORT2N;
    case Format::R16G16B16A16_SNORM: return SG_VERTEXFORMAT_SHORT4N;
    case Format::R16G16B16A16_UNORM: return SG_VERTEXFORMAT_USHORT4N;
    case Format::R8G8B8A8_UNORM: return SG_VERTEXFORMAT_UBYTE4N;
    default: return SG_VERTEXFORMAT_INVALID;
    }
}

Result<SokolAdapter*> sokol_adapter_create(SokolAdapterDesc const& desc, Adapter* out) noexcept {
    if (!out || desc.maxObjects == 0 || desc.maxUploads == 0) return make_status(Code::InvalidArgument);
    sg_desc const gd = sg_query_desc();
    if (u32(min(gd.buffer_pool_size, min(gd.image_pool_size, gd.view_pool_size))) <= desc.maxObjects) {
        KILN_ERROR("sokol", "sg_setup pools (buffers %d, images %d, views %d) must exceed maxObjects %u",
                   gd.buffer_pool_size, gd.image_pool_size, gd.view_pool_size, desc.maxObjects);
        return make_status(Code::InvalidArgument);
    }
    Allocator const* const al = desc.alloc ? desc.alloc : default_allocator();
    auto* a                   = new_object<SokolAdapter>(al, Tag::Payload);
    a->alloc                  = al;
    a->objects.init(al, Tag::Payload);
    a->freeObjects.init(al, Tag::Payload);
    a->objects.resize(desc.maxObjects);
    a->freeObjects.reserve(desc.maxObjects);
    a->uploads = ex::UploadPool<Upload>(al, desc.maxUploads);
    for (u32 i = desc.maxObjects; i-- > 0;)
        a->freeObjects.push_back(i);
    a->commits = ex::CommitQueue(al, desc.maxUploads);
    *out       = Adapter{
              .supports_format  = &supports_format,
              .copy_constraints = &copy_constraints,
              .begin_upload     = &begin_upload,
              .commit_upload    = &commit_upload,
              .upload_status    = &upload_status,
              .bind             = nullptr, // bindings are rebuilt per draw from gpu_object()
              .destroy          = &destroy,
              .flush            = &flush,
              .caps             = kCubeTextures | kArrayTextures | kMeshes,
              .reserved         = {},
              .user             = a,
    };
    return a;
}

void sokol_adapter_destroy(SokolAdapter* a) noexcept {
    if (!a) return;
    for (u32 i = 0; i < a->uploads.capacity(); ++i)
        free_bytes(a, a->uploads[i]); // uploads never flushed still hold theirs
    for (u32 i = 0; i < a->objects.size(); ++i)
        if (a->objects[i].used) release_object(a, i);
    delete_object(a->alloc, a, Tag::Payload);
}

ex::AdapterStats sokol_adapter_stats(SokolAdapter* a) noexcept {
    std::lock_guard<std::mutex> const lock(a->mutex);
    ex::AdapterStats s = a->stats;
    s.liveObjects      = u32(a->objects.size() - a->freeObjects.size());
    s.uploadsPending   = a->uploads.in_use();
    return s;
}

sg_view sokol_texture(SokolAdapter const* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size() || obj.kind != u32(ObjectKind::Texture)) return {};
    Object const& o = a->objects[obj.native - 1];
    return o.used ? o.view : sg_view{};
}

sg_buffer sokol_buffer(SokolAdapter const* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size() || obj.kind != u32(ObjectKind::Buffer)) return {};
    Object const& o = a->objects[obj.native - 1];
    return o.used ? o.buffer : sg_buffer{};
}

} // namespace kiln::sk
