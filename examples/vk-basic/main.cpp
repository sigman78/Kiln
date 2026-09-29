// examples/vk-basic/main.cpp — kiln-vk-basic: one model and a cube sky through Vulkan 1.4 without
// bindless (docs/design/integration-examples.md). Each material has a descriptor set per frame in
// flight; kiln's events say when a texture's GPU object changed, and the host rewrites the sets that
// use it. The frame plumbing (swapchain, frames, offscreen target) is the viewer's (kiln_example_vk).
// Meshes are cooked with VertexProfile::Float. The steps a host takes are numbered.
#include "example_app.h"
#include "viewer_render.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_VK_BASIC_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include "shaders/basic_mesh_frag_spv.h"
#include "shaders/basic_mesh_vert_spv.h"
#include "shaders/basic_sky_frag_spv.h"
#include "shaders/basic_sky_vert_spv.h"

// volk (through viewer_render.h) comes first so GLFW sees the Vulkan types.
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstring>

using namespace kiln;
using ex::Mat4;
using ex::Vec3;

namespace {

constexpr f32 kFovY         = 50.0f * ex::kPi / 180.0f;
constexpr u32 kMaxTextures  = 32;
constexpr u32 kMaxMaterials = 64;
constexpr u32 kMaxParts     = 256;
constexpr u32 kBindings     = 6; // set 1: base color, normal, metal-rough, occlusion, emissive, sky
constexpr u32 kSkyBinding   = 5;
constexpr u32 kFif          = vkx::kFramesInFlight;

struct TextureItem {
    AssetId id = 0;
    TextureHandle handle;
};

/// A descriptor set per frame in flight: a set may be rewritten only once the frame that used it
/// last has finished, which renderer_wait_frame() guarantees for this frame's slot.
struct Material {
    u32 textures[kBindings];         ///< index into Scene::textures per binding; kInvalid = none
    VkDescriptorSet sets[kFif] = {}; ///< one per frame slot
    u64 stamp                  = 1;  ///< bumped when a texture it uses gets another GPU object
    u64 written[kFif]          = {}; ///< the stamp each slot's set was written with
    u32 mask[kFif]             = {}; ///< bindings each slot's set holds (the shader tests them)
};

struct Scene {
    Context* ctx       = nullptr;
    vkx::VkAdapter* va = nullptr;
    vkx::Renderer* ren = nullptr;
    VkDevice device    = VK_NULL_HANDLE;
    StrView modelName;
    MeshHandle model;
    u32 skyItem = kInvalid;
    TextureItem textures[kMaxTextures];
    u32 textureCount = 0;
    Material materials[kMaxMaterials + 1]; ///< the last one holds only the sky, for the sky pass
    u32 materialCount               = 0;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool           = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, skySampler = VK_NULL_HANDLE;
    Mat4 place;
    Mat4 world[kMaxParts];
    bool unsupported     = false;
    u64 invalidations    = 0; ///< material stamps bumped by events
    u64 descriptorWrites = 0; ///< sets rewritten
};

Material& sky_material(Scene& s) { return s.materials[kMaxMaterials]; }

/// One set per frame slot from the pool.
void alloc_sets(Scene& s, VkDescriptorSet (&out)[kFif]) {
    VkDescriptorSetLayout layouts[kFif];
    for (VkDescriptorSetLayout& l : layouts)
        l = s.setLayout;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = s.pool;
    ai.descriptorSetCount = kFif;
    ai.pSetLayouts        = layouts;
    VKX_CHECK(vkAllocateDescriptorSets(s.device, &ai, out));
}

/// The sky pass's material: only the cube.
void reset_sky_material(Scene& s) {
    Material& sky = sky_material(s);
    sky           = Material{};
    for (u32 i = 0; i < kBindings; ++i)
        sky.textures[i] = i == kSkyBinding ? s.skyItem : kInvalid;
    alloc_sets(s, sky.sets);
}

StrView texture_name(StrView meshName, mesh::MeshView const& v, mesh::TextureBinding const& b,
                     char (&buf)[256]) {
    StrView const path = v.str(b.pathStr);
    if (!(b.flags & mesh::kTextureExternal)) return path;
    return StrView(buf, resolve_asset_name(meshName, path, buf, sizeof buf));
}

u32 texture_item(Scene& s, StrView name, TextureKind kind) {
    AssetId const id = asset_id(name);
    for (u32 i = 0; i < s.textureCount; ++i)
        if (s.textures[i].id == id) return i;
    if (id == 0 || s.textureCount == kMaxTextures) return kInvalid;
    s.textures[s.textureCount] = {id, request_texture(s.ctx, name, RequestOptions{.textureKind = kind})};
    return s.textureCount++;
}

u32 binding_of(mesh::TextureSlot slot) {
    switch (slot) {
    case mesh::TextureSlot::BaseColor: return 0;
    case mesh::TextureSlot::Normal: return 1;
    case mesh::TextureSlot::MetalRough: return 2;
    case mesh::TextureSlot::Occlusion: return 3;
    case mesh::TextureSlot::Emissive: return 4;
    default: return kInvalid;
    }
}

/// On MetaReady and Changed: requests the textures and maps every material to its texture items.
/// A changed model may have other materials, so the sets are made again (after the GPU is idle).
void build_materials(Scene& s, mesh::MeshView const& v) {
    if (s.materialCount) {
        vkx::renderer_wait_idle(s.ren);
        VKX_CHECK(vkResetDescriptorPool(s.device, s.pool, 0));
        reset_sky_material(s);
    }
    s.materialCount = min<u32>(max<u32>(v.materials().size(), 1), kMaxMaterials);
    for (u32 m = 0; m < s.materialCount; ++m) {
        Material& mat = s.materials[m];
        mat           = Material{};
        for (u32 i = 0; i < kBindings; ++i)
            mat.textures[i] = i == kSkyBinding ? s.skyItem : kInvalid;
        if (m >= v.materials().size()) continue; // a mesh without materials draws with material 0
        mesh::MaterialSlot const& ms = v.materials()[m];
        for (u32 t = 0; t < ms.textureCount && ms.textureFirst + t < v.textures().size(); ++t) {
            mesh::TextureBinding const& b = v.textures()[ms.textureFirst + t];
            u32 const binding             = binding_of(mesh::TextureSlot(b.slot));
            if (binding == kInvalid) continue;
            char buf[256];
            mat.textures[binding] = texture_item(s, texture_name(s.modelName, v, b, buf),
                                                 texture_kind_for_slot(mesh::TextureSlot(b.slot)));
        }
    }
    for (u32 m = 0; m < s.materialCount; ++m) // the pool holds kMaxMaterials + 1 materials' sets
        alloc_sets(s, s.materials[m].sets);
    mesh::Bounds const& b = v.model().bounds;
    f32 const scale       = b.radius > 0 ? 1.0f / b.radius : 1.0f;
    s.place = ex::scaling(scale) * ex::translation(Vec3{-b.center[0], -b.center[1], -b.center[2]});
    // Float vertex data only: a two-component (octahedral) normal needs the viewer's decoding.
    s.unsupported = false;
    for (u32 li = 0; li < v.layouts().size(); ++li)
        for (u32 a = 0; a < v.layouts()[li].attribCount; ++a) {
            mesh::VertexAttrib const& at = v.layouts()[li].attribs[a];
            if (mesh::Semantic(at.semantic) == mesh::Semantic::Normal &&
                Format(at.format) != Format::R32G32B32_SFLOAT)
                s.unsupported = true;
        }
    if (s.unsupported)
        KILN_ERROR("vk-basic", "%.*s: vertex data is not float; cook it with profile = \"float\"",
                   KILN_SV(s.modelName));
}

/// The one place kiln's state changes reach the descriptor sets: a texture whose GPU object
/// changed (Ready: placeholder -> real; Changed: a reload; Failed: the checker) invalidates every
/// material that samples it. MetaReady changes nothing: gpu() still returns the placeholder.
void on_texture_event(Scene& s, Event const& e) {
    if (e.kind == EventKind::MetaReady) return;
    for (u32 i = 0; i < s.textureCount; ++i) {
        if (s.textures[i].handle.bits() != e.handle) continue;
        for (u32 m = 0; m <= kMaxMaterials; ++m) {
            if (m >= s.materialCount && m != kMaxMaterials) continue;
            Material& mat = s.materials[m];
            for (u32 t : mat.textures)
                if (t == i) {
                    ++mat.stamp;
                    ++s.invalidations;
                    break;
                }
        }
    }
}

void handle_event(Scene& s, Event const& e) {
    bool const isModel = e.asset == AssetKind::Mesh && e.handle == s.model.bits();
    KILN_INFO("vk-basic", "event %-9s %s v%u", ex::event_name(e.kind), isModel ? "model" : "texture",
              e.version);
    if (e.asset == AssetKind::Texture) {
        on_texture_event(s, e);
        return;
    }
    if (!isModel || e.kind == EventKind::Failed) return;
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    if (v && (e.kind == EventKind::MetaReady || e.kind == EventKind::Changed)) build_materials(s, *v);
}

/// Rewrites this frame slot's set of every material whose stamp moved since the slot was written.
void update_sets(Scene& s, u32 slot) {
    for (u32 m = 0; m <= kMaxMaterials; ++m) {
        if (m >= s.materialCount && m != kMaxMaterials) continue;
        Material& mat = s.materials[m];
        if (!mat.sets[slot] || mat.written[slot] == mat.stamp) continue;
        VkDescriptorImageInfo images[kBindings]{};
        VkWriteDescriptorSet writes[kBindings]{};
        u32 count = 0, mask = 0;
        for (u32 b = 0; b < kBindings; ++b) {
            if (mat.textures[b] == kInvalid) continue;
            // Pending: the placeholder of the texture's kind; Ready: the real image.
            vkx::TextureView const tv =
                vkx::adapter_texture(s.va, gpu(s.ctx, s.textures[mat.textures[b]].handle));
            TextureShape const want = b == kSkyBinding ? TextureShape::Cube : TextureShape::Tex2D;
            if (!tv.view || tv.shape != want) continue;
            images[count]                 = {b == kSkyBinding ? s.skySampler : s.sampler, tv.view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[count].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[count].dstSet          = mat.sets[slot];
            writes[count].dstBinding      = b;
            writes[count].descriptorCount = 1;
            writes[count].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].pImageInfo      = &images[count];
            ++count;
            mask |= 1u << b;
        }
        if (count) vkUpdateDescriptorSets(s.device, count, writes, 0, nullptr);
        mat.written[slot] = mat.stamp;
        mat.mask[slot]    = mask;
        ++s.descriptorWrites;
    }
}

void draw_model(Scene& s, VkCommandBuffer cmd, u32 slot) {
    mesh::MeshView const* v        = mesh_view(s.ctx, s.model);
    vkx::MeshPayload const payload = vkx::adapter_mesh(s.va, gpu(s.ctx, s.model)); // null until Ready
    if (!v || !payload.buffer || s.unsupported) return;
    VkPipelineLayout const layout = vkx::renderer_pipeline_layout(s.ren);
    VkBuffer const zero           = vkx::renderer_zero_buffer(s.ren);
    u32 const partCount           = min<u32>(v->parts().size(), kMaxParts);
    for (u32 p = 0; p < partCount; ++p) {
        mesh::MeshPart const& part = v->parts()[p];
        Mat4 const local           = ex::from_rt(part.translation, part.rotation);
        s.world[p] = part.parent < p ? s.world[part.parent] * local : s.place * local; // parents first
        if (part.lodCount == 0) continue;
        mesh::MeshLod const& lod = v->lods()[part.lodFirst];
        auto const indexType     = mesh::IndexType(lod.indexType);
        if (indexType == mesh::IndexType::U8) continue;
        vkx::LayoutPipeline const* pipe = vkx::renderer_pipeline(s.ren, v->layouts()[lod.layout]);
        if (!pipe) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
        VkBuffer buffers[mesh::kMaxStreams + 6];
        VkDeviceSize offsets[mesh::kMaxStreams + 6];
        u32 n = 0;
        for (u32 st = 0; st < pipe->streamCount; ++st, ++n) {
            buffers[n] = payload.buffer;
            offsets[n] = payload.offset + lod.streamOffset[st];
        }
        for (u32 z = 0; z < pipe->zeroBindings; ++z, ++n) { // shader inputs the layout lacks
            buffers[n] = zero;
            offsets[n] = 0;
        }
        vkCmdBindVertexBuffers(cmd, 0, n, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, payload.buffer, payload.offset + lod.indexOffset,
                             indexType == mesh::IndexType::U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkx::DrawPush push{};
        std::memcpy(push.model, s.world[p].m, sizeof push.model);
        for (u32 si = 0; si < lod.submeshCount; ++si) {
            mesh::Submesh const& sm = v->submeshes()[lod.submeshFirst + si];
            Material const& mat     = s.materials[min(sm.material, s.materialCount - 1)];
            push.flags              = mat.mask[slot];
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &mat.sets[slot], 0,
                                    nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof push, &push);
            vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexFirst, sm.vertexBase, 0);
        }
    }
}

bool scene_settled(Scene const& s) {
    if (!ex::settled(state(s.ctx, s.model))) return false;
    for (u32 i = 0; i < s.textureCount; ++i)
        if (!ex::settled(state(s.ctx, s.textures[i].handle))) return false;
    return true;
}

void framebuffer_size(void* user, u32* width, u32* height) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(user), &w, &h);
    *width  = w > 0 ? u32(w) : 0u;
    *height = h > 0 ? u32(h) : 0u;
}

/// The host's Vulkan objects: the material set layout, a pool, two samplers.
void create_host_objects(Scene& s) {
    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    VkDescriptorBindingFlags flags[kBindings]{};
    for (u32 b = 0; b < kBindings; ++b) {
        bindings[b].binding         = b;
        bindings[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        flags[b] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT; // a material without, e.g., an emissive map
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{};
    fi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    fi.bindingCount  = kBindings;
    fi.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.pNext        = &fi;
    lci.bindingCount = kBindings;
    lci.pBindings    = bindings;
    VKX_CHECK(vkCreateDescriptorSetLayout(s.device, &lci, nullptr, &s.setLayout));
    VkDescriptorPoolSize const size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    (kMaxMaterials + 1) * kFif * kBindings};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = (kMaxMaterials + 1) * kFif;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &size;
    VKX_CHECK(vkCreateDescriptorPool(s.device, &pci, nullptr, &s.pool));
    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod                                             = VK_LOD_CLAMP_NONE;
    VKX_CHECK(vkCreateSampler(s.device, &sci, nullptr, &s.sampler));
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKX_CHECK(vkCreateSampler(s.device, &sci, nullptr, &s.skySampler));
}

} // namespace

int main(int argc, char** argv) {
    ex::Options o;
    if (int const code = ex::parse_options("kiln-vk-basic", argc, argv, &o); code >= 0) return code;
    ex::install_stdout_log();
    DiagSink const diag = ex::stdout_diag();

    // 1. Window (unless offscreen) and device.
    GLFWwindow* window                = nullptr;
    char const* const* glfwExtensions = nullptr;
    u32 glfwExtensionCount            = 0;
    ex::OrbitCamera camera;
    if (!o.offscreen) {
        if (!glfwInit() || !glfwVulkanSupported()) return 2;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(int(o.width), int(o.height), "kiln-vk-basic", nullptr, nullptr);
        if (!window) return 2;
        ex::attach_camera(window, &camera);
    }
    Result<vkx::Device> dev =
        vkx::device_create({.appName            = "kiln-vk-basic",
                            .validation         = false,
                            .instanceExtensions = Span<char const* const>(glfwExtensions, glfwExtensionCount),
                            .needSwapchain      = !o.offscreen},
                           &diag);
    if (dev.failed()) return 2;
    vkx::Device device   = dev.value();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (window && glfwCreateWindowSurface(device.instance, window, nullptr, &surface) != VK_SUCCESS) return 2;

    // 2. The adapter without bindless: no acquire(); gpu() gives objects, adapter_texture() their views.
    Adapter adapter{};
    Result<vkx::VkAdapter*> va =
        vkx::adapter_create({.device = &device, .framesInFlight = kFif, .bindless = false}, &adapter);
    if (va.failed()) return 2;

    // 3. The frame plumbing with this example's shaders and its material set layout.
    Scene s;
    s.va     = *va;
    s.device = device.device;
    create_host_objects(s);
    Result<vkx::Renderer*> rr = vkx::renderer_create({.device            = &device,
                                                      .adapter           = s.va,
                                                      .surface           = surface,
                                                      .width             = o.width,
                                                      .height            = o.height,
                                                      .framebufferSize   = &framebuffer_size,
                                                      .user              = window,
                                                      .meshVert          = k_basic_mesh_vert_spv,
                                                      .meshFrag          = k_basic_mesh_frag_spv,
                                                      .skyVert           = k_basic_sky_vert_spv,
                                                      .skyFrag           = k_basic_sky_frag_spv,
                                                      .materialSetLayout = s.setLayout});
    if (rr.failed()) return 2;
    s.ren = *rr;

    // 4. The context (create() waits for the placeholders: the adapter is self-submitting) and,
    //    in dev builds, the cook provider, which cooks meshes as plain floats.
    ContextDesc cd{};
    cd.diag                  = diag;
    cd.adapter               = &adapter;
    cd.storeDir              = StrView(o.store);
    cd.roots                 = Span<Root const>(o.roots, o.rootCount);
    cd.hotReload.watchStore  = o.watch;
    Result<Context*> created = create(cd);
    if (created.failed()) return 2;
    s.ctx = *created;
#if KILN_VK_BASIC_HAS_COOK
    bool provider = false;
    if (o.rootCount) {
        cook::ProviderDesc pd{};
        pd.meshDefaults.profile = cook::VertexProfile::Float;
        pd.watchSources         = o.watch;
        provider                = cook::install_provider(s.ctx, pd).ok();
    }
#endif

    // 5. Requests. The sky is a texture item like the others; its set is the last material's.
    s.modelName = StrView(o.model);
    s.model     = request_mesh(s.ctx, s.modelName);
    if (o.sky) {
        s.textures[0] = {
            asset_id(StrView(o.sky)),
            request_texture(s.ctx, StrView(o.sky), RequestOptions{.textureShape = TextureShape::Cube})};
        s.textureCount = 1;
        s.skyItem      = 0;
    }
    reset_sky_material(s);

    // 6. Frames: wait for the slot, pump, apply events to the sets, draw.
    int exitCode         = 0;
    bool settledOnce     = false;
    double const startMs = ex::ms_since_start();
    u64 frame            = 0;
    for (;;) {
        if (window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window)) break;
        }
        vkx::renderer_wait_frame(s.ren);
        u32 const slot = u32(frame++ % kFif); // the slot renderer_wait_frame just freed
        (void)pump(s.ctx);
        for (Event const& e : events(s.ctx))
            handle_event(s, e);
        update_sets(s, slot);

        bool last = false;
        if (o.offscreen) {
            if (settledOnce) last = true; // one frame after settling, as the other examples do
            settledOnce = scene_settled(s);
            if (!last && ex::ms_since_start() - startMs > o.timeoutS * 1000.0) {
                KILN_ERROR("vk-basic", "the scene did not settle within %u s", o.timeoutS);
                exitCode = 1;
                last     = true;
            }
        }

        VkCommandBuffer const cmd = vkx::renderer_begin(s.ren);
        if (!cmd) {
            if (window) glfwWaitEventsTimeout(0.05); // minimized
            continue;
        }
        VkExtent2D const extent = vkx::renderer_extent(s.ren);
        f32 const aspect        = extent.height ? f32(extent.width) / f32(extent.height) : 1.0f;
        ex::View const view     = ex::orbit_view(camera, Vec3{}, 1.0f, kFovY, aspect);
        Mat4 const viewProj     = ex::perspective_vk(kFovY, aspect, view.nearZ, view.farZ) * view.view;
        vkx::FrameUniforms* u   = vkx::renderer_uniforms(s.ren);
        std::memcpy(u->viewProj, viewProj.m, sizeof u->viewProj);
        u->cameraPos[0]     = view.eye.x;
        u->cameraPos[1]     = view.eye.y;
        u->cameraPos[2]     = view.eye.z;
        u->cameraPos[3]     = 1.0f;
        u->tonemap[0]       = std::exp2(f32(o.exposure));
        Material const& sky = sky_material(s);
        if (sky.mask[slot] & (1u << kSkyBinding)) {
            Vec3 const f    = ex::normalize(Vec3{} - view.eye);
            Vec3 const side = ex::normalize(ex::cross(f, Vec3{0, 1, 0}));
            f32 const tanV  = std::tan(kFovY * 0.5f);
            Vec3 const r    = side * (tanV * aspect);
            Vec3 const up   = ex::cross(side, f) * tanV;
            vkx::SkyPush const push{
                .forward  = {f.x, f.y, f.z, 0},
                .right    = {r.x, r.y, r.z, 0},
                .up       = {up.x, up.y, up.z, 0},
                .cubeSlot = 0,
                .pad      = {}
            };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vkx::renderer_pipeline_layout(s.ren), 1, 1, &sky.sets[slot], 0, nullptr);
            vkx::renderer_draw_sky(s.ren, cmd, push);
        }
        draw_model(s, cmd, slot);
        vkx::renderer_end(s.ren, last && o.dump != nullptr);
        if (last) break;
    }
    vkx::renderer_wait_idle(s.ren);
    KILN_INFO("vk-basic", "%llu frames; %llu material invalidations from events, %llu descriptor set writes",
              static_cast<unsigned long long>(frame), static_cast<unsigned long long>(s.invalidations),
              static_cast<unsigned long long>(s.descriptorWrites));

    if (o.dump && exitCode == 0) {
        Vec<u8> rgba(default_allocator(), Tag::Io);
        u32 w = 0, h = 0;
        if (vkx::renderer_read_back(s.ren, &rgba, &w, &h).failed() ||
            !ex::write_png(o.dump, rgba.span(), w, h)) {
            KILN_ERROR("vk-basic", "cannot write %s", o.dump);
            exitCode = 2;
        } else {
            KILN_INFO("vk-basic", "wrote %s (%ux%u)", o.dump, w, h);
        }
    }
    if (state(s.ctx, s.model) == State::Failed && exitCode == 0) exitCode = 1;

    // 7. Teardown: kiln first (it hands every object back through destroy_deferred), then Vulkan.
#if KILN_VK_BASIC_HAS_COOK
    if (provider) cook::uninstall_provider(s.ctx);
#endif
    destroy(s.ctx);
    vkx::renderer_destroy(s.ren);
    vkx::adapter_destroy(s.va);
    vkDestroyDescriptorPool(s.device, s.pool, nullptr);
    vkDestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
    vkDestroySampler(s.device, s.sampler, nullptr);
    vkDestroySampler(s.device, s.skySampler, nullptr);
    if (surface) vkDestroySurfaceKHR(device.instance, surface, nullptr);
    vkx::device_destroy(device);
    if (window) glfwDestroyWindow(window);
    if (!o.offscreen) glfwTerminate();
    return exitCode;
}
