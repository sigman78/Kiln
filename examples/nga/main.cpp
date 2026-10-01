// examples/nga/main.cpp — kiln-nga: one model and a cube sky through NoGraphicsAPI
// (docs/design/integration-examples.md). Vertices are pulled through GPU pointers into the mesh
// payload kiln wrote in place; materials store kiln's stable texture slots once and resolve them to
// descriptor indices in each frame's root data. Meshes are cooked with VertexProfile::Float.
// Needs VK_EXT_descriptor_heap and friends (RTX 30+, RDNA 3+). The steps a host takes are numbered.
#include "example_app.h"
#include "nga_adapter.h"
#include "nga_root.h"
#include "no_crash_dialogs.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_NGA_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kiln;
using namespace kiln::nga;
using ex::Mat4;
using ex::Vec3;

namespace {

constexpr f32 kFovY          = 50.0f * ex::kPi / 180.0f;
constexpr u32 kMaxTextures   = 32;
constexpr u32 kMaxMaterials  = 64;
constexpr u32 kMaxParts      = 256;
constexpr u32 kFif           = 2;
constexpr u64 kFrameBytes    = 1u << 20; ///< root data per frame in flight
constexpr u64 kTargetBytes   = 256u << 20;
constexpr gpu::Format kDepth = gpu::Format::d32_float;

/// Material slot order in the root: base color, normal, metal-rough, occlusion, emissive.
u32 slot_index(mesh::TextureSlot s) {
    switch (s) {
    case mesh::TextureSlot::BaseColor: return 0;
    case mesh::TextureSlot::Normal: return 1;
    case mesh::TextureSlot::MetalRough: return 2;
    case mesh::TextureSlot::Occlusion: return 3;
    case mesh::TextureSlot::Emissive: return 4;
    default: return kInvalid;
    }
}

struct TextureItem {
    AssetId id = 0;
    TextureHandle handle;
};

struct Scene {
    /// The model content version the materials and geometry were set up for. MetaReady, Changed
    /// and a Ready that follows Failed (a repaired model: reloads emit no MetaReady) carry a new one.
    u32 preparedVersion = 0;
    Context* ctx        = nullptr;
    NgaAdapter* na      = nullptr;
    StrView modelName;
    MeshHandle model;
    TextureHandle sky;
    u32 skySlot = kInvalid;
    TextureItem textures[kMaxTextures];
    u32 textureCount                    = 0;
    u32 materialSlots[kMaxMaterials][5] = {}; ///< kiln's stable slots, filled once per load
    u32 materialCount                   = 0;
    Mat4 place;
    Mat4 world[kMaxParts];
    bool unsupported = false;
};

TextureHandle texture_for(Scene& s, StrView name, mesh::TextureSlot slot) {
    AssetId const id = asset_id(name);
    for (u32 i = 0; i < s.textureCount; ++i)
        if (s.textures[i].id == id) return s.textures[i].handle;
    if (id == 0 || s.textureCount == kMaxTextures) return {};
    s.textures[s.textureCount] = {id,
                                  request_texture(s.ctx, name, {.textureKind = texture_kind_for_slot(slot)})};
    return s.textures[s.textureCount++].handle;
}

/// MetaReady / Changed: request the textures, keep each material's slots (kiln numbers them at the
/// request), place the model, check the vertex data is what the shader pulls.
void prepare(Scene& s, mesh::MeshView const& v) {
    s.materialCount = min<u32>(max<u32>(v.materials().size(), 1), kMaxMaterials);
    for (u32 m = 0; m < s.materialCount; ++m) {
        for (u32& slot : s.materialSlots[m])
            slot = kInvalid;
        if (m >= v.materials().size()) continue;
        mesh::MaterialSlot const& ms = v.materials()[m];
        for (u32 t = 0; t < ms.textureCount && ms.textureFirst + t < v.textures().size(); ++t) {
            mesh::TextureBinding const& b = v.textures()[ms.textureFirst + t];
            u32 const i                   = slot_index(mesh::TextureSlot(b.slot));
            if (i == kInvalid) continue;
            char buf[256];
            TextureHandle const h = texture_for(s, texture_asset_name(s.modelName, v, b, buf, sizeof buf),
                                                mesh::TextureSlot(b.slot));
            s.materialSlots[m][i] = gpu_object(s.ctx, h).slot;
        }
    }
    mesh::Bounds const& b = v.model().bounds;
    f32 const scale       = b.radius > 0 ? 1.0f / b.radius : 1.0f;
    s.place = ex::scaling(scale) * ex::translation(Vec3{-b.center[0], -b.center[1], -b.center[2]});
}

/// Where the shader finds each attribute of a layout; false if it is not the float profile.
struct Streams {
    u32 posStream = 0, posOffset = 0, attrStream = 0, normalOffset = 0;
    u32 tangentOffset = kNoTexture, uvOffset = kNoTexture;
};
bool streams_of(mesh::VertexLayout const& l, Streams* out) {
    bool pos = false, normal = false;
    for (u32 i = 0; i < l.attribCount; ++i) {
        mesh::VertexAttrib const& a = l.attribs[i];
        Format const f              = Format(a.format);
        if (a.semanticIndex != 0 || a.offset % 4) continue;
        switch (mesh::Semantic(a.semantic)) {
        case mesh::Semantic::Position:
            pos            = f == Format::R32G32B32_SFLOAT;
            out->posStream = a.stream;
            out->posOffset = a.offset / 4;
            break;
        case mesh::Semantic::Normal:
            normal            = f == Format::R32G32B32_SFLOAT;
            out->attrStream   = a.stream;
            out->normalOffset = a.offset / 4;
            break;
        case mesh::Semantic::Tangent:
            if (f == Format::R32G32B32A32_SFLOAT) out->tangentOffset = a.offset / 4;
            break;
        case mesh::Semantic::TexCoord:
            if (f == Format::R32G32_SFLOAT) out->uvOffset = a.offset / 4;
            break;
        default: break;
        }
    }
    return pos && normal && out->posOffset == 0 && l.strides[out->posStream] % 4 == 0 &&
           l.strides[out->attrStream] % 4 == 0;
}

void handle_event(Scene& s, Event const& e) {
    bool const isModel = e.asset == AssetKind::Mesh && e.handle == s.model.bits();
    KILN_INFO("nga", "event %-9s %s v%u", ex::event_name(e.kind), isModel ? "model" : "texture", e.version);
    if (!isModel || e.kind == EventKind::Failed) return;
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    if (v && e.version != s.preparedVersion) {
        s.preparedVersion = e.version;
        prepare(s, *v);
    }
}

bool scene_settled(Scene const& s) {
    if (!ex::settled(state(s.ctx, s.model))) return false;
    if (s.sky && !ex::settled(state(s.ctx, s.sky))) return false;
    for (u32 i = 0; i < s.textureCount; ++i)
        if (!ex::settled(state(s.ctx, s.textures[i].handle))) return false;
    return true;
}

/// A 16-byte-aligned piece of this frame's CPU-visible memory: the CPU writes, the GPU reads.
struct Bump {
    gpu::GpuCpuRange<byte> range;
    u64 used = 0;
    template <class T> T* alloc(void** gpuAddress) {
        u64 const at = align_up<u64>(used, 16);
        if (at + sizeof(T) > range.size) return nullptr;
        used        = at + sizeof(T);
        *gpuAddress = range.gpu + at;
        return reinterpret_cast<T*>(range.cpu + at);
    }
};

u32 descriptor_of(NgaAdapter* na, u32 slot) {
    return slot == kInvalid ? kNoTexture : nga_descriptor(na, slot);
}

void draw_model(Scene& s, gpu::CommandBuffer* cmd, Bump& frame, Mat4 const& viewProj, Vec3 eye,
                f32 exposure) {
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    NgaMesh const payload   = nga_mesh(s.na, gpu_object(s.ctx, s.model)); // zero until Ready
    if (!v || !payload.gpu) return;
    u32 const partCount = min<u32>(v->parts().size(), kMaxParts);
    for (u32 p = 0; p < partCount; ++p) {
        mesh::MeshPart const& part = v->parts()[p];
        Mat4 const local           = ex::from_rt(part.translation, part.rotation);
        s.world[p] = part.parent < p ? s.world[part.parent] * local : s.place * local; // parents first
        if (part.lodCount == 0) continue;
        mesh::MeshLod const& lod = v->lods()[part.lodFirst];
        auto const indexType     = mesh::IndexType(lod.indexType);
        Streams st;
        if (indexType == mesh::IndexType::U8 || !streams_of(v->layouts()[lod.layout], &st)) {
            if (!s.unsupported)
                KILN_ERROR("nga", "%.*s: needs float vertex data (profile = \"float\") and 16/32-bit indices",
                           KILN_SV(s.modelName));
            s.unsupported = true;
            continue;
        }
        mesh::VertexLayout const& layout = v->layouts()[lod.layout];
        for (u32 si = 0; si < lod.submeshCount; ++si) {
            mesh::Submesh const& sm = v->submeshes()[lod.submeshFirst + si];
            void* rootGpu           = nullptr;
            MeshRoot* r             = frame.alloc<MeshRoot>(&rootGpu);
            if (!r) return; // the frame's root memory is full
            std::memcpy(r->model, s.world[p].m, sizeof r->model);
            std::memcpy(r->viewProj, viewProj.m, sizeof r->viewProj);
            r->eyeExposure[0] = eye.x;
            r->eyeExposure[1] = eye.y;
            r->eyeExposure[2] = eye.z;
            r->eyeExposure[3] = exposure;
            r->stream0        = payload.gpu + lod.streamOffset[st.posStream];
            r->stream1        = payload.gpu + lod.streamOffset[st.attrStream];
            r->stride0        = layout.strides[st.posStream] / 4;
            r->stride1        = layout.strides[st.attrStream] / 4;
            r->normalOffset   = st.normalOffset;
            r->tangentOffset  = st.tangentOffset;
            r->uvOffset       = st.uvOffset;
            r->vertexBase     = u32(sm.vertexBase);
            // The material stored slots; the slot shows the placeholder, then the texture.
            u32 const* slots            = s.materialSlots[min(sm.material, s.materialCount - 1)];
            r->texBaseColor             = descriptor_of(s.na, slots[0]);
            r->texNormal                = descriptor_of(s.na, slots[1]);
            r->texMetalRough            = descriptor_of(s.na, slots[2]);
            r->texOcclusion             = descriptor_of(s.na, slots[3]);
            r->texEmissive              = descriptor_of(s.na, slots[4]);
            r->texSky                   = descriptor_of(s.na, s.skySlot);
            ex::MaterialFactors const f = ex::material_factors(*v, sm.material);
            std::memcpy(r->baseColorFactor, f.baseColor, sizeof r->baseColorFactor);
            std::memcpy(r->emissiveNormal, f.emissiveNormal, sizeof r->emissiveNormal);
            std::memcpy(r->mro, f.mro, sizeof r->mro);
            u64 const indexSize = mesh::index_size(indexType);
            gpu::GpuRange const indices{reinterpret_cast<void*>(payload.gpu + lod.indexOffset),
                                        u64(lod.indexCount) * indexSize};
            gpu::draw_indexed(cmd, rootGpu, indices,
                              indexType == mesh::IndexType::U32 ? gpu::IndexType::uint32
                                                                : gpu::IndexType::uint16,
                              sm.indexCount, 1, sm.indexFirst, 0, 0);
        }
    }
}

/// A whole SPIR-V file, or an empty span.
Span<byte> read_spirv(char const* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long const size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    auto* data    = static_cast<byte*>(kiln::alloc(default_allocator(), usize(size), 4, Tag::Io));
    bool const ok = size > 0 && std::fread(data, 1, usize(size), f) == usize(size);
    std::fclose(f);
    if (!ok) {
        kiln::free(default_allocator(), data, usize(max<long>(size, 1)), 4, Tag::Io);
        return {};
    }
    return {data, usize(size)};
}

gpu::PSO* make_pso(gpu::Device* device, char const* vsPath, char const* vsEntry, char const* fsPath,
                   char const* fsEntry, gpu::Format color) {
    Span<byte> const vs = read_spirv(vsPath);
    Span<byte> const fs = read_spirv(fsPath);
    gpu::PSO* pso       = nullptr;
    if (!vs.empty() && !fs.empty()) {
        gpu::ColorTargetDesc const target{.format = color};
        pso = gpu::create_graphics_pso(device,
                                       {
                                           .vertex   = {.code = {vs.data, vs.size}, .entry_point = vsEntry},
                                           .fragment = {.code = {fs.data, fs.size}, .entry_point = fsEntry},
                                           .color_targets = {&target,                    1                     },
                                           .depth_format  = kDepth,
        });
    }
    if (!pso) KILN_ERROR("nga", "pipeline %s / %s failed", vsPath, fsPath);
    kiln::free(default_allocator(), vs.data, vs.size, 4, Tag::Io);
    kiln::free(default_allocator(), fs.data, fs.size, 4, Tag::Io);
    return pso;
}

/// The host's render targets: depth always, color when offscreen; placed in one texture heap.
struct Targets {
    gpu::TextureHeap heap{};
    gpu::Texture* color        = nullptr;
    gpu::RenderView* colorView = nullptr;
    gpu::Texture* depth        = nullptr;
    gpu::RenderView* depthView = nullptr;
    u32 width = 0, height = 0;

    void release() {
        gpu::destroy_render_view(colorView);
        gpu::destroy_texture(color);
        gpu::destroy_render_view(depthView);
        gpu::destroy_texture(depth);
        colorView = depthView = nullptr;
        color = depth = nullptr;
        width = height = 0;
    }
    /// Recreates the targets at a new size; the caller has waited for every frame that used them.
    bool resize(gpu::Device* device, gpu::CommandBuffer* cmd, u32 w, u32 h, bool offscreenColor) {
        release();
        u64 offset = 0;
        if (offscreenColor) {
            gpu::TextureDesc const cd{
                .extent = {.x = w, .y = h, .z = 1},
                .format = gpu::Format::rgba8_srgb,
                .usage  = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source
            };
            gpu::SizeAlign const sa = gpu::get_texture_size_align(device, cd);
            color                   = gpu::create_texture(cmd, cd, heap, 0);
            colorView               = gpu::create_render_view(color);
            offset                  = sa.size;
        }
        gpu::TextureDesc const dd{
            .extent = {.x = w, .y = h, .z = 1},
            .format = kDepth,
            .usage  = gpu::TextureUsage::depth_stencil_attachment
        };
        gpu::SizeAlign const sa = gpu::get_texture_size_align(device, dd);
        offset                  = align_up<u64>(offset, sa.align);
        if (offset + sa.size > heap.size) return false;
        depth     = gpu::create_texture(cmd, dd, heap, offset);
        depthView = gpu::create_render_view(depth);
        width     = w;
        height    = h;
        return depth && depthView && (!offscreenColor || (color && colorView));
    }
};

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    ex::Options o;
    if (int const code = ex::parse_options("kiln-nga", argc, argv, &o); code >= 0) return code;
    ex::install_stdout_log();

    // 1. A window (Windows) or none, and the NoGraphicsAPI device.
#if !defined(_WIN32)
    if (!o.offscreen) {
        KILN_ERROR("nga", "NoGraphicsAPI is headless on this platform: run with --offscreen");
        return 2;
    }
#endif
    GLFWwindow* window = nullptr;
    ex::OrbitCamera camera;
    void* nativeWindow = nullptr;
    if (!o.offscreen) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(int(o.width), int(o.height), "kiln-nga", nullptr, nullptr);
        if (!window) return 2;
        ex::attach_camera(window, &camera);
#if defined(_WIN32)
        nativeWindow = glfwGetWin32Window(window);
#endif
    }
    gpu::Format const colorFormat = window ? gpu::Format::bgra8_srgb : gpu::Format::rgba8_srgb;
    gpu::DeviceInit const init    = gpu::create_device(
        {.window = nativeWindow, .swapchain_format = window ? colorFormat : gpu::Format::undefined});
    if (!init.device) {
        KILN_ERROR("nga",
                   "no NoGraphicsAPI device (error %d): it needs Vulkan 1.4 with VK_EXT_descriptor_heap, "
                   "VK_KHR_device_address_commands and VK_EXT_mesh_shader (RTX 30+, RDNA 3+)",
                   int(init.error));
        glfwTerminate(); // destroys the window; nothing when GLFW never started
        return 2;
    }
    gpu::Device* const device = init.device;
    KILN_INFO("nga", "device: %s", gpu::get_device_caps(device).device_name);

    // 2. The adapter: kiln writes mesh payloads in place and textures into its staging ring.
    Adapter adapter{};
    Result<NgaAdapter*> na = nga_adapter_create({.device = device}, &adapter);
    if (na.failed()) {
        gpu::destroy_device(device);
        return 2;
    }

    // 3. The context: create() runs the adapter's flush until the placeholders are uploaded.
    ContextDesc cd{};
    cd.diag                  = ex::stdout_diag();
    cd.profiler              = ex::trace_hooks(); // KILN_TRACE=<file>
    cd.adapter               = &adapter;
    cd.storeDir              = StrView(o.store);
    cd.roots                 = Span<Root const>(o.roots, o.rootCount);
    cd.hotReload.watchStore  = o.watch;
    Result<Context*> created = create(cd);
    if (created.failed()) return 2;
    Context* const ctx = *created;
#if KILN_NGA_HAS_COOK
    bool provider = false;
    if (o.rootCount) {
        cook::ProviderDesc pd{};
        pd.meshDefaults.profile = cook::VertexProfile::Float;
        pd.watchSources         = o.watch;
        provider                = cook::install_provider(ctx, pd).ok();
    }
#endif

    // 4. Requests: the sky's slot is known at once and never changes.
    Scene s;
    s.ctx       = ctx;
    s.na        = *na;
    s.modelName = StrView(o.model);
    s.model     = request_mesh(ctx, s.modelName);
    if (o.sky) {
        s.sky     = request_texture(ctx, StrView(o.sky), {.textureShape = TextureShape::Cube});
        s.skySlot = gpu_object(ctx, s.sky).slot;
    }

    // 5. What the host owns: pipelines, per-frame root memory, render targets, frame sync.
    gpu::PSO* const meshPso      = make_pso(device, KILN_NGA_SHADER_DIR "/meshVertex.spv", "meshVertex",
                                            KILN_NGA_SHADER_DIR "/meshFragment.spv", "meshFragment", colorFormat);
    gpu::PSO* const skyPso       = make_pso(device, KILN_NGA_SHADER_DIR "/skyVertex.spv", "skyVertex",
                                            KILN_NGA_SHADER_DIR "/skyFragment.spv", "skyFragment", colorFormat);
    gpu::GpuHeap const frameHeap = gpu::create_gpu_heap(device, kFrameBytes * kFif);
    gpu::GpuHeap readback{};
    if (o.dump)
        readback = gpu::create_gpu_heap(device, u64(o.width) * o.height * 4, gpu::MemoryType::readback);
    Targets targets;
    targets.heap = gpu::create_texture_heap(device, kTargetBytes);
    gpu::CommandPool* pools[kFif];
    for (gpu::CommandPool*& p : pools)
        p = gpu::create_command_pool(device);
    gpu::TimelinePoint done{.semaphore = gpu::create_timeline_semaphore(device), .value = 0};
    int exitCode = (meshPso && skyPso && frameHeap.range.cpu) ? 0 : 2;

    // 6. Frames.
    bool settledOnce     = false;
    double const startMs = ex::ms_since_start();
    while (exitCode == 0) {
        if (window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window)) break;
        }
        // 6a. The frame slot's previous use is done. Frame numbers are the timeline values.
        if (done.value >= kFif) gpu::wait_timeline({done.semaphore, done.value - (kFif - 1)});
        // 6b. kiln: the adapter's flush submits the texture copies; completions, events, and the
        //     release of what the completed frames no longer use.
        (void)pump(
            ctx, {.frame = done.value + 1, .completedFrame = gpu::timeline_completed_value(done.semaphore)});
        for (Event const& e : events(ctx))
            handle_event(s, e);

        bool last = false;
        if (o.offscreen) {
            if (settledOnce) last = true; // one frame after settling, as the other examples do
            settledOnce = scene_settled(s);
            if (!last && ex::ms_since_start() - startMs > o.timeoutS * 1000.0) {
                KILN_ERROR("nga", "the scene did not settle within %u s", o.timeoutS);
                exitCode = 1;
                last     = true;
            }
        }

        // 6c. Record.
        u32 const slot = u32(done.value % kFif);
        Bump frame{
            {frameHeap.range.cpu + slot * kFrameBytes, frameHeap.range.gpu + slot * kFrameBytes,
             kFrameBytes}
        };
        gpu::reset_command_pool(pools[slot]);
        gpu::CommandBuffer* const cmd = gpu::begin_commands(pools[slot]);
        gpu::RenderView* target       = nullptr;
        u32 w = o.width, h = o.height;
        if (window) {
            gpu::SwapchainFrame const sf = gpu::acquire(cmd);
            if (!sf.render_view) { // minimized
                gpu::end_commands(cmd);
                continue;
            }
            target = sf.render_view;
            w      = sf.extent.x;
            h      = sf.extent.y;
        }
        if (w != targets.width || h != targets.height) {
            if (done.value) gpu::wait_timeline(done); // the old targets are no longer in use
            if (!targets.resize(device, cmd, w, h, !window)) {
                KILN_ERROR("nga", "render targets %ux%u failed", w, h);
                exitCode = 2;
                gpu::end_commands(cmd);
                break;
            }
        }
        if (!window) target = targets.colorView;
        gpu::set_texture_descriptor_heap(cmd, nga_texture_heap(s.na));
        gpu::set_sampler_descriptor_heap(cmd, nga_sampler_heap(s.na));
        // One depth target (and offscreen, one color target) serves every frame in flight: order this
        // frame's clear and writes after the previous frame's.
        gpu::barrier(cmd, gpu::Stage::depth_stencil_tests, gpu::Access::depth_stencil_write,
                     gpu::Stage::depth_stencil_tests, gpu::Access::depth_stencil_write);
        if (!window)
            gpu::barrier(cmd, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::color_output,
                         gpu::Access::color_write);
        gpu::ColorAttachment const color{
            .render_view = target,
            .load        = gpu::LoadOp::clear,
            .clear       = {.x = 0.15f, .y = 0.16f, .z = 0.19f, .w = 1.0f}
        };
        gpu::begin_render_pass(cmd, {
                                        .colors = {&color, 1},
                                        .depth  = {.render_view = targets.depthView,
                                                   .load        = gpu::LoadOp::clear,
                                                   .store       = gpu::StoreOp::discard}
        });
        f32 const aspect    = f32(w) / f32(h);
        ex::View const view = ex::orbit_view(camera, Vec3{}, 1.0f, kFovY, aspect);
        Mat4 const viewProj = ex::perspective_vk(kFovY, aspect, view.nearZ, view.farZ) * view.view;
        f32 const exposure  = std::exp2(camera.exposure);
        if (s.skySlot != kInvalid) {
            Vec3 const f    = ex::normalize(Vec3{} - view.eye);
            Vec3 const side = ex::normalize(ex::cross(f, Vec3{0, 1, 0}));
            f32 const tanV  = std::tan(kFovY * 0.5f);
            Vec3 const r    = side * (tanV * aspect);
            Vec3 const up   = ex::cross(side, f) * tanV;
            void* rootGpu   = nullptr;
            if (SkyRoot* sr = frame.alloc<SkyRoot>(&rootGpu)) {
                *sr = SkyRoot{
                    .forward = {f.x, f.y, f.z, exposure},
                    .right   = {r.x, r.y, r.z, 0},
                    .up      = {up.x, up.y, up.z, 0},
                    .sky     = nga_descriptor(s.na, s.skySlot),
                    .pad     = {}
                };
                gpu::bind_pso(cmd, skyPso);
                gpu::draw(cmd, rootGpu, 3);
            }
        }
        gpu::bind_pso(cmd, meshPso);
        gpu::set_depth_stencil(cmd, {.depth_test = true, .depth_write = true});
        draw_model(s, cmd, frame, viewProj, view.eye, exposure);
        gpu::end_render_pass(cmd);
        bool const dumpNow = last && o.dump && readback.range.cpu;
        if (dumpNow) {
            gpu::barrier(cmd, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer,
                         gpu::Access::transfer_read);
            gpu::copy_texture_to_memory(cmd, targets.color, gpu::gpu_range(readback));
            gpu::barrier(cmd, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host,
                         gpu::Access::host_read);
        }
        gpu::end_commands(cmd);

        // 6d. Submit.
        ++done.value;
        gpu::CommandBuffer* const cmds[] = {cmd};
        if (window)
            gpu::submit_and_present(device, {
                                                .commands = {cmds, 1},
                                                  .completion = done
            });
        else
            gpu::submit(device, {
                                    .commands = {cmds, 1},
                                      .completion = done
            });
        if (dumpNow) {
            gpu::wait_timeline(done);
            Vec<u8> rgba(default_allocator(), Tag::Io);
            rgba.resize(usize(w) * h * 4);
            std::memcpy(rgba.data(), readback.range.cpu, rgba.size());
            if (ex::write_png(o.dump, rgba.span(), w, h))
                KILN_INFO("nga", "wrote %s (%ux%u)", o.dump, w, h);
            else
                exitCode = 2;
        }
        if (last) break;
    }
    gpu::wait_idle(device);
    if (exitCode == 0 && state(ctx, s.model) == State::Failed) exitCode = 1;

    ex::log_adapter_stats("nga", nga_adapter_stats(s.na));

    // 7. Teardown (the GPU is idle): kiln first (Adapter::destroy for every object), then the adapter,
    //    then the host's.
#if KILN_NGA_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    ex::finish_trace();
    nga_adapter_destroy(s.na);
    targets.release();
    gpu::destroy_texture_heap(targets.heap);
    for (gpu::CommandPool* p : pools)
        gpu::destroy_command_pool(p);
    gpu::destroy_timeline_semaphore(done.semaphore);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(frameHeap);
    gpu::destroy_pso(meshPso);
    gpu::destroy_pso(skyPso);
    gpu::destroy_device(device);
    glfwTerminate(); // destroys the window; nothing when GLFW never started
    return exitCode;
}
