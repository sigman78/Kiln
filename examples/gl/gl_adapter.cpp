// examples/gl/gl_adapter.cpp — kiln adapter over OpenGL 4.6 core, bound or bindless.
// Threads: begin_upload / commit_upload (kiln workers) only touch memory under `mutex`. Everything
// that calls GL (flush, is_upload_complete, bind, destroy) runs on the GL thread, which is also the
// pump thread.
#include "gl_adapter.h"

#include "gl_api.h"

#include <kiln/alloc.h>
#include <kiln/containers.h>
#include <kiln/log.h>

#include <cstring>
#include <mutex>

namespace kiln::glx {
namespace {

constexpr u64 kOffsetAlign = 16;  ///< per-level offset inside a texture upload
constexpr u64 kUploadAlign = 256; ///< start of every upload in the ring
constexpr u32 kMaxLevels   = 16;

/// For printf's %llu, whatever u64 is on this platform.
constexpr unsigned long long ull(u64 v) noexcept { return v; }

enum class ObjectKind : u32 { Texture = 1, Buffer = 2 };

struct Object {
    GLuint name   = 0;
    GLenum target = 0;
    u32 gen       = 1;
    bool used     = false;
    u64 handle    = 0; ///< bindless: the resident texture handle, once bound
};

struct Upload {
    u32 gen         = 1;
    bool used       = false;
    bool committed  = false;
    bool flushed    = false;
    bool done       = false;
    u32 object      = 0;
    UploadKind kind = UploadKind::MeshPayload;
    TextureDesc tex{};
    u64 size     = 0;
    u64 offset   = 0; ///< in the staging ring
    u32 span     = 0; ///< entry in the ring FIFO
    GLsync fence = nullptr;
};

/// One allocation of the staging ring, in allocation order.
struct Span {
    u64 begin = 0, end = 0;
    bool done = false;
};

struct TexFormat {
    GLenum internal = 0, format = 0, type = 0;
};

TexFormat tex_format(Format f) noexcept {
    switch (f) {
    case Format::R8_UNORM: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case Format::R8_SNORM: return {GL_R8_SNORM, GL_RED, GL_BYTE};
    case Format::R8G8_UNORM: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case Format::R8G8_SNORM: return {GL_RG8_SNORM, GL_RG, GL_BYTE};
    case Format::R8G8B8A8_UNORM: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::R8G8B8A8_SNORM: return {GL_RGBA8_SNORM, GL_RGBA, GL_BYTE};
    case Format::R8G8B8A8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::R16_UNORM: return {GL_R16, GL_RED, GL_UNSIGNED_SHORT};
    case Format::R16_SNORM: return {GL_R16_SNORM, GL_RED, GL_SHORT};
    case Format::R16_SFLOAT: return {GL_R16F, GL_RED, GL_HALF_FLOAT};
    case Format::R16G16_UNORM: return {GL_RG16, GL_RG, GL_UNSIGNED_SHORT};
    case Format::R16G16_SNORM: return {GL_RG16_SNORM, GL_RG, GL_SHORT};
    case Format::R16G16_SFLOAT: return {GL_RG16F, GL_RG, GL_HALF_FLOAT};
    case Format::R16G16B16A16_UNORM: return {GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT};
    case Format::R16G16B16A16_SNORM: return {GL_RGBA16_SNORM, GL_RGBA, GL_SHORT};
    case Format::R16G16B16A16_SFLOAT: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
    case Format::R32_SFLOAT: return {GL_R32F, GL_RED, GL_FLOAT};
    case Format::R32G32_SFLOAT: return {GL_RG32F, GL_RG, GL_FLOAT};
    case Format::R32G32B32A32_SFLOAT: return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
    default: return {};
    }
}

} // namespace

struct GlAdapter {
    std::mutex mutex;
    GLuint staging = 0;
    u8* mapped     = nullptr;
    u64 ringSize   = 0;
    u64 head       = 0; ///< next free byte of the ring
    Vec<Object> objects;
    Vec<u32> freeObjects;
    Vec<Upload> uploads;
    Vec<u32> freeUploads;
    Vec<Span> spans; ///< circular FIFO of ring allocations
    u32 spanFirst = 0, spanCount = 0;
    Vec<u32> committed; ///< upload indices waiting for flush, in commit order
    Vec<u32> flushing;  ///< flush's copy of `committed`
    Vec<u32> inFlight;  ///< flushed, fence not yet signaled

    // Bindless only.
    bool bindless      = false;
    GLuint samplers[2] = {}; ///< [0] repeat, [1] clamp (cubes); a handle bakes in its sampler
    GLuint table       = 0;  ///< SSBO: `tableFrames` copies of the u64 handle per slot, persistently mapped
    u8* tableMapped    = nullptr;
    u32 tableFrames    = 0;
    u64 tableStride    = 0; ///< bytes per copy, a multiple of the storage buffer offset alignment
    Vec<u64> handles;       ///< what each slot shows now; gl_handle_table() copies it
    u32 slotsUsed = 0;      ///< slots below it were bound at least once
};

namespace {

u64 make_token(Upload const& u, u32 index) noexcept { return (u64(u.gen) << 32) | (index + 1); }

/// The upload a token names, or null if it is stale.
Upload* upload_of(GlAdapter* a, u64 token) noexcept {
    u32 const index = u32(token & 0xFFFFFFFFu) - 1;
    if (index >= a->uploads.size()) return nullptr;
    Upload& u = a->uploads[index];
    return u.used && u.gen == u32(token >> 32) ? &u : nullptr;
}

/// A contiguous range of `size` bytes in the ring, or false when it does not fit now.
bool ring_alloc(GlAdapter* a, u64 size, u64* offset) noexcept {
    if (a->spanCount == u32(a->spans.size())) return false;
    if (a->spanCount == 0) a->head = 0;
    u64 const tail     = a->spanCount ? a->spans[a->spanFirst].begin : 0;
    u64 start          = align_up(a->head, kUploadAlign);
    bool const wrapped = a->spanCount && a->head <= tail; // the free range is [head, tail)
    if (wrapped) {
        if (start + size > tail) return false;
    } else if (start + size > a->ringSize) {
        start = 0; // wrap: the free range is now [0, tail)
        if (a->spanCount && size > tail) return false;
        if (size > a->ringSize) return false;
    }
    *offset = start;
    a->head = start + size;
    return true;
}

u32 push_span(GlAdapter* a, u64 begin, u64 end) noexcept {
    u32 const i = (a->spanFirst + a->spanCount) % u32(a->spans.size());
    a->spans[i] = Span{begin, end, false};
    ++a->spanCount;
    return i;
}

void pop_done_spans(GlAdapter* a) noexcept {
    while (a->spanCount && a->spans[a->spanFirst].done) {
        a->spanFirst = (a->spanFirst + 1) % u32(a->spans.size());
        --a->spanCount;
    }
}

void release_object(GlAdapter* a, u32 index) noexcept {
    Object& o = a->objects[index];
    if (o.handle) glMakeTextureHandleNonResidentARB(o.handle);
    if (o.name) {
        if (o.target) {
            glDeleteTextures(1, &o.name);
        } else {
            glDeleteBuffers(1, &o.name);
        }
    }
    o = Object{0, 0, o.gen + 1, false, 0};
    std::lock_guard<std::mutex> const lock(a->mutex);
    a->freeObjects.push_back(index);
}

/// The GL side of one committed upload: create the object, copy from the staging buffer.
void run_upload(GlAdapter* a, Upload& u) noexcept {
    Object& o = a->objects[u.object];
    if (u.kind == UploadKind::MeshPayload) {
        glCreateBuffers(1, &o.name);
        glNamedBufferStorage(o.name, GLsizeiptr(u.size), nullptr, 0);
        glCopyNamedBufferSubData(a->staging, o.name, GLintptr(u.offset), 0, GLsizeiptr(u.size));
        return;
    }
    TextureDesc const& t = u.tex;
    TexFormat const f    = tex_format(t.format);
    o.target             = t.shape == TextureShape::Cube    ? GL_TEXTURE_CUBE_MAP
                           : t.shape == TextureShape::Array ? GL_TEXTURE_2D_ARRAY
                                                            : GL_TEXTURE_2D;
    glCreateTextures(o.target, 1, &o.name);
    GLsizei const levels = GLsizei(t.levels), w = GLsizei(t.width), h = GLsizei(t.height);
    if (o.target == GL_TEXTURE_2D_ARRAY)
        glTextureStorage3D(o.name, levels, f.internal, w, h, GLsizei(t.layers));
    else
        glTextureStorage2D(o.name, levels, f.internal, w, h); // a cube's storage is 2D per face
    u64 offsets[kMaxLevels];
    CopyConstraints const cc{.optimalRowPitchAlign = 1,
                             .optimalOffsetAlign   = kOffsetAlign,
                             .bufferOffsetAlign    = kUploadAlign}; // as copy_constraints reports
    (void)texture_level_layout(t, cc, offsets, nullptr);
    for (u32 i = 0; i < t.levels; ++i) {
        GLsizei const lw = GLsizei(max(t.width >> i, 1u)), lh = GLsizei(max(t.height >> i, 1u));
        // The pixel pointer is an offset into the bound GL_PIXEL_UNPACK_BUFFER.
        void const* src = reinterpret_cast<void const*>(usize(u.offset + offsets[i]));
        if (o.target == GL_TEXTURE_2D)
            glTextureSubImage2D(o.name, GLint(i), 0, 0, lw, lh, f.format, f.type, src);
        else // cube faces and array layers are the z range of a 3D sub-image
            glTextureSubImage3D(o.name, GLint(i), 0, 0, 0, lw, lh, GLsizei(t.layers), f.format, f.type, src);
    }
}

/// Retires `u` if its fence has signaled: frees its ring range.
bool poll(GlAdapter* a, u32 index) noexcept {
    Upload& u = a->uploads[index];
    if (u.done) return true;
    if (!u.flushed) return false;
    GLenum const r = glClientWaitSync(u.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (r == GL_WAIT_FAILED) KILN_PANIC("gl: glClientWaitSync failed on an upload fence");
    if (r == GL_TIMEOUT_EXPIRED) return false;
    glDeleteSync(u.fence);
    u.fence = nullptr;
    u.done  = true;
    std::lock_guard<std::mutex> const lock(a->mutex);
    a->spans[u.span].done = true;
    pop_done_spans(a);
    return true;
}

// --- Adapter callbacks ---------------------------------------------------------------------------

bool supports_format(void*, Format f, FormatUsage usage) {
    return usage == FormatUsage::VertexBuffer ? gl_vertex_format(f).size != 0 : tex_format(f).internal != 0;
}

void copy_constraints(void*, CopyConstraints* out) {
    out->optimalRowPitchAlign = 1;
    out->optimalOffsetAlign   = kOffsetAlign;
    out->bufferOffsetAlign    = kUploadAlign;
}

Status begin_upload(void* user, UploadDesc const& desc, UploadTarget* out) {
    auto* a = static_cast<GlAdapter*>(user);
    if (desc.size > a->ringSize) {
        KILN_ERROR("gl", "upload of %llu bytes exceeds the %llu-byte staging ring", ull(desc.size),
                   ull(a->ringSize));
        return make_status(Code::OutOfMemory);
    }
    if (desc.kind == UploadKind::TextureLevels &&
        (!desc.texture || desc.texture->levels > kMaxLevels || desc.texture->depth > 1))
        return make_status(Code::Unsupported);
    std::lock_guard<std::mutex> const lock(a->mutex);
    if (a->freeUploads.empty() || a->freeObjects.empty()) return make_status(Code::Busy);
    u64 offset = 0;
    if (!ring_alloc(a, max<u64>(desc.size, 1), &offset)) return make_status(Code::Busy);

    u32 const ui = a->freeUploads.back();
    a->freeUploads.pop_back();
    u32 const oi = a->freeObjects.back();
    a->freeObjects.pop_back();
    a->objects[oi].used = true;

    Upload& u   = a->uploads[ui];
    u.used      = true;
    u.committed = u.flushed = u.done = false;
    u.object                         = oi;
    u.kind                           = desc.kind;
    u.tex                            = desc.texture ? *desc.texture : TextureDesc{};
    u.size                           = desc.size;
    u.offset                         = offset;
    u.span                           = push_span(a, offset, offset + max<u64>(desc.size, 1));

    out->dst           = a->mapped + offset;
    out->rowPitchAlign = 1;
    out->token         = make_token(u, ui);
    ObjectKind const k = desc.kind == UploadKind::MeshPayload ? ObjectKind::Buffer : ObjectKind::Texture;
    out->object        = GpuObject{.native = oi + 1, .slot = kInvalid, .kind = u32(k)};
    return kOk;
}

void commit_upload(void* user, u64 token) {
    auto* a = static_cast<GlAdapter*>(user);
    std::lock_guard<std::mutex> const lock(a->mutex);
    if (Upload* u = upload_of(a, token)) {
        u->committed = true;
        a->committed.push_back(u32(token & 0xFFFFFFFFu) - 1);
    }
}

bool is_upload_complete(void* user, u64 token) {
    auto* a   = static_cast<GlAdapter*>(user);
    Upload* u = upload_of(a, token);
    if (!u) return true;
    u32 const index = u32(token & 0xFFFFFFFFu) - 1;
    if (!poll(a, index)) return false;
    for (u32 i = 0; i < a->inFlight.size(); ++i)
        if (a->inFlight[i] == index) {
            a->inFlight[i] = a->inFlight.back();
            a->inFlight.pop_back();
            break;
        }
    std::lock_guard<std::mutex> const lock(a->mutex);
    u->used = false;
    ++u->gen;
    a->freeUploads.push_back(index);
    return true;
}

/// GL keeps an object alive for commands already issued; a resident handle has no such guard, which
/// kiln covers by waiting for the host's frames.
void destroy(void* user, GpuObject obj) {
    auto* a = static_cast<GlAdapter*>(user);
    if (obj.native == 0 || obj.native > a->objects.size() || !a->objects[obj.native - 1].used) return;
    release_object(a, u32(obj.native - 1));
}

// --- Bindless ------------------------------------------------------------------------------------

/// The resident handle of a finished texture, made on first use.
u64 resident_handle(GlAdapter* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size() || obj.kind != u32(ObjectKind::Texture)) return 0;
    Object& o = a->objects[obj.native - 1];
    if (!o.handle && o.name) {
        o.handle = glGetTextureSamplerHandleARB(o.name, o.target == GL_TEXTURE_CUBE_MAP ? a->samplers[1]
                                                                                        : a->samplers[0]);
        glMakeTextureHandleResidentARB(o.handle);
    }
    return o.handle;
}

/// Slot `slot` shows `obj` from the next gl_handle_table() on; frames in flight keep their copy.
void bind(void* user, u32 slot, GpuObject obj, TextureShape) {
    auto* a = static_cast<GlAdapter*>(user);
    if (u64 const h = resident_handle(a, obj)) {
        a->handles[slot] = h;
        a->slotsUsed     = max(a->slotsUsed, slot + 1);
    }
}

/// The GL work of every committed upload, and the retirement of finished ones. kiln calls this at
/// the start of each pump(), on the pump thread, which is the GL thread.
void flush(void* user) {
    auto* a = static_cast<GlAdapter*>(user);
    // Retire first: frees ring space before this frame's begin_upload calls.
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
    if (a->flushing.empty()) return;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, a->staging);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (u32 i : a->flushing)
        run_upload(a, a->uploads[i]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    GLsync const fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    for (u32 n = 0; n < a->flushing.size(); ++n) {
        Upload& u = a->uploads[a->flushing[n]];
        // One fence per upload: each is deleted when its upload retires.
        u.fence   = n == 0 ? fence : glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        u.flushed = true;
        a->inFlight.push_back(a->flushing[n]);
    }
}

} // namespace

GlVertexFormat gl_vertex_format(Format f) noexcept {
    switch (f) {
    case Format::R32_SFLOAT: return {1, GL_FLOAT, false};
    case Format::R32G32_SFLOAT: return {2, GL_FLOAT, false};
    case Format::R32G32B32_SFLOAT: return {3, GL_FLOAT, false};
    case Format::R32G32B32A32_SFLOAT: return {4, GL_FLOAT, false};
    case Format::R16G16_SFLOAT: return {2, GL_HALF_FLOAT, false};
    case Format::R16G16_SNORM: return {2, GL_SHORT, true};
    case Format::R16G16B16A16_SNORM: return {4, GL_SHORT, true};
    case Format::R16G16B16A16_UNORM: return {4, GL_UNSIGNED_SHORT, true};
    case Format::R8G8B8A8_UNORM: return {4, GL_UNSIGNED_BYTE, true};
    default: return {};
    }
}

Result<GlAdapter*> gl_adapter_create(GlAdapterDesc const& desc, Adapter* out) noexcept {
    if (!out || desc.stagingBytes == 0 || desc.maxObjects == 0 || desc.maxUploads == 0)
        return make_status(Code::InvalidArgument);
    auto* a     = new_object<GlAdapter>(default_allocator(), Tag::Payload);
    a->ringSize = desc.stagingBytes;
    glCreateBuffers(1, &a->staging);
    GLbitfield const flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glNamedBufferStorage(a->staging, GLsizeiptr(a->ringSize), nullptr, flags);
    a->mapped = static_cast<u8*>(glMapNamedBufferRange(a->staging, 0, GLsizeiptr(a->ringSize), flags));
    if (!a->mapped) {
        glDeleteBuffers(1, &a->staging);
        delete_object(default_allocator(), a, Tag::Payload);
        return make_status(Code::OutOfMemory);
    }
    a->objects.resize(desc.maxObjects);
    a->uploads.resize(desc.maxUploads);
    a->spans.resize(desc.maxUploads * 2);
    for (u32 i = desc.maxObjects; i-- > 0;)
        a->freeObjects.push_back(i);
    for (u32 i = desc.maxUploads; i-- > 0;)
        a->freeUploads.push_back(i);
    a->committed.reserve(desc.maxUploads);
    a->flushing.reserve(desc.maxUploads);
    a->inFlight.reserve(desc.maxUploads);
    if (desc.bindless) {
        a->bindless = true;
        glCreateSamplers(2, a->samplers);
        for (GLuint s : a->samplers) {
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GLint(GL_LINEAR_MIPMAP_LINEAR));
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GLint(GL_LINEAR));
            glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
        }
        for (GLenum wrap : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
            glSamplerParameteri(a->samplers[1], wrap, GLint(GL_CLAMP_TO_EDGE));
        // The global GL_TEXTURE_CUBE_MAP_SEAMLESS does not reach bindless handles on every driver;
        // the per-sampler switch (ARB_seamless_cubemap_per_texture) does.
        glSamplerParameteri(a->samplers[1], GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_TRUE);
        GLint align = 256;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
        a->tableFrames         = max(desc.tableFrames, 2u);
        a->tableStride         = align_up(u64(desc.maxSlots) * sizeof(u64), u64(max(align, 1)));
        GLsizeiptr const bytes = GLsizeiptr(a->tableStride * a->tableFrames);
        glCreateBuffers(1, &a->table);
        glNamedBufferStorage(a->table, bytes, nullptr, flags);
        a->tableMapped = static_cast<u8*>(glMapNamedBufferRange(a->table, 0, bytes, flags));
        a->handles.resize(desc.maxSlots, 0);
    }

    *out = Adapter{
        .supports_format    = &supports_format,
        .copy_constraints   = &copy_constraints,
        .begin_upload       = &begin_upload,
        .commit_upload      = &commit_upload,
        .is_upload_complete = &is_upload_complete,
        .bind               = desc.bindless ? &bind : nullptr, // bound: the host asks gpu_object() per draw
        .destroy            = &destroy,
        .flush              = &flush, // the GL work, on the pump thread
        .caps               = kCubeTextures | kArrayTextures | kMeshes,
        .bindlessSlots      = desc.bindless ? desc.maxSlots : 0,
        .reserved           = {},
        .user               = a,
    };
    return a;
}

void gl_adapter_destroy(GlAdapter* a) noexcept {
    if (!a) return;
    for (Upload& u : a->uploads)
        if (u.fence) glDeleteSync(u.fence);
    for (u32 i = 0; i < a->objects.size(); ++i)
        if (a->objects[i].used) release_object(a, i);
    if (a->bindless) {
        glUnmapNamedBuffer(a->table);
        glDeleteBuffers(1, &a->table);
        glDeleteSamplers(2, a->samplers);
    }
    glUnmapNamedBuffer(a->staging);
    glDeleteBuffers(1, &a->staging);
    delete_object(default_allocator(), a, Tag::Payload);
}

GlTexture gl_texture(GlAdapter const* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size() || obj.kind != u32(ObjectKind::Texture)) return {};
    Object const& o = a->objects[obj.native - 1];
    return o.used ? GlTexture{o.name, o.target} : GlTexture{};
}

GlBufferRange gl_handle_table(GlAdapter* a, u64 frame) noexcept {
    u64 const offset = (frame % a->tableFrames) * a->tableStride;
    std::memcpy(a->tableMapped + offset, a->handles.data(), usize(a->slotsUsed) * sizeof(u64));
    return {a->table, offset, a->tableStride};
}

unsigned gl_buffer(GlAdapter const* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size() || obj.kind != u32(ObjectKind::Buffer)) return 0;
    Object const& o = a->objects[obj.native - 1];
    return o.used ? o.name : 0;
}

} // namespace kiln::glx
