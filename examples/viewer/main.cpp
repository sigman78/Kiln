// examples/viewer/main.cpp — kiln-viewer: loads cooked meshes through the example Vulkan adapter
// and draws them (docs/design/viewer.md). The meshes on the command line form a boot group that
// is waited on; their textures stream in afterwards under a per-frame upload budget, so the
// first frames show placeholders. --offscreen renders without a window and can dump a PNG. --watch
// turns on hot reload (docs/design/hot-reload.md).
#include "no_crash_dialogs.h"
#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_VIEWER_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include "cli.h"
#include "png_writer.h"
#include "viewer_math.h"
#include "vk_adapter.h"
#include "vk_device.h"
#include "vk_render.h"

// volk (through vk_device.h) comes first so GLFW sees the Vulkan types; GLFW_INCLUDE_NONE is set.
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace kiln;
using vkx::Mat4;
using vkx::Vec3;

namespace {

/// For printf's %llu, whatever u64 is on this platform.
unsigned long long ull(u64 v) { return v; }

constexpr u32 kMaxMeshes       = 64;
constexpr u32 kWarmupFrames    = 10; ///< frames excluded from the running average and the spike check
constexpr u32 kBootTimeoutMs   = 30000;
constexpr f32 kFitRadius       = 1.0f; ///< bounding radius every boot model is scaled to (unless --no-fit)
constexpr f32 kFitSpacing      = 2.5f; ///< distance between model centers in the fitted row
constexpr double kSpikeFloorMs = 1.0;  ///< frames faster than this are never reported as spikes
constexpr f32 kPi              = 3.14159265358979f;
constexpr f32 kFovY            = 60.0f * kPi / 180.0f;

using Clock = std::chrono::steady_clock;

Clock::time_point g_start = Clock::now();

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void log_to_stdout(void*, LogLevel level, StrView category, StrView message) {
    std::printf("%9.1f ms  %-5s %-9.*s %.*s\n", ms_since(g_start), log_level_name(level), KILN_SV(category),
                KILN_SV(message));
}

void diag_to_stdout(void*, Diagnostic const& d) {
    std::printf("%9.1f ms  %-5s K%04u     %.*s%s%.*s: %.*s\n", ms_since(g_start), severity_name(d.severity),
                d.code, KILN_SV(d.asset), d.where.size ? " @" : "", KILN_SV(d.where), KILN_SV(d.message));
}

char const* state_name(State s) {
    switch (s) {
    case State::Unloaded: return "Unloaded";
    case State::Pending: return "Pending";
    case State::MetaReady: return "MetaReady";
    case State::Ready: return "Ready";
    case State::Failed: return "Failed";
    case State::Partial: return "Partial";
    }
    return "?";
}

char const* event_name(EventKind k) {
    switch (k) {
    case EventKind::MetaReady: return "MetaReady";
    case EventKind::Ready: return "Ready";
    case EventKind::Changed: return "Changed";
    case EventKind::Failed: return "Failed";
    }
    return "?";
}

// --- Assets -------------------------------------------------------------------------------------

struct MeshItem {
    StrView path; ///< the asset name, e.g. "mesh/Box.glb"
    MeshHandle handle;
    State last = State::Unloaded;
    Mat4 place;              ///< model root -> world: the slot in the row, the fit scale, the recentering
    f32 nativeRadius = 0.0f; ///< ModelInfo bounds radius
    f32 scale        = 1.0f; ///< uniform scale applied (1 with --no-fit)
    Vec3 worldCenter;        ///< bounding sphere after placement, for camera framing
    f32 worldRadius = 0.0f;  ///< 0 = not placed (no metadata)
};

struct TextureItem {
    u64 textureId = 0;
    TextureHandle handle;
    State last = State::Unloaded;
    char path[160]{}; ///< copied: the mesh view that named it may be reloaded
};

constexpr u32 kMaxRoots = 8;

struct Options {
    char const* store = "cooked";
    Root roots[kMaxRoots]; ///< --source and --root
    u32 rootCount       = 0;
    char const* dump    = nullptr;
    char const* sky     = nullptr; ///< a cube map drawn behind the scene
    double atMs         = -1;      ///< offscreen --at: stop at the first frame at or after this time
    u32 timeoutS        = 60;      ///< offscreen: give up waiting for the scene to settle
    double exposure     = 0;       ///< EV: colors are scaled by 2^exposure before the tonemap
    char const* tonemap = "auto";
    bool validate       = false;
    bool offscreen      = false;
    bool noFit          = false;
    bool watch          = false;
    u32 width           = 1280;
    u32 height          = 720;
    u32 budgetMiB       = 8;
    u32 frames          = 0; ///< 0 = 60 offscreen, until closed in a window
    u32 threads         = 0;
    MeshItem meshes[kMaxMeshes];
    u32 meshCount = 0;
};

bool add_mesh(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (char const* why = check_asset_name(StrView(arg))) {
        std::fprintf(stderr, "kiln-viewer: '%s' is not an asset name (%s)\n", arg, why);
        return false;
    }
    if (o->meshCount == kMaxMeshes) {
        std::fprintf(stderr, "kiln-viewer: too many meshes (max %u)\n", kMaxMeshes);
        return false;
    }
    o->meshes[o->meshCount++].path = StrView(arg);
    return true;
}

/// `--root [<name>=]<dir>` and `--source <dir>`. A prefix before `=` that is a valid root name
/// names the root; otherwise the argument is the default root. create() rejects a repeated root.
bool add_root(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (o->rootCount == kMaxRoots) {
        std::fprintf(stderr, "kiln-viewer: too many roots (max %u)\n", kMaxRoots);
        return false;
    }
    char const* const eq = std::strchr(arg, '=');
    bool const named     = eq && !check_root_name(StrView(arg, usize(eq - arg)));
    o->roots[o->rootCount++] =
        named ? Root{StrView(arg, usize(eq - arg)), StrView(eq + 1)} : Root{{}, StrView(arg)};
    return true;
}

/// The viewer's asset state: boot meshes, the textures they reference, per-part matrices.
struct Scene {
    Context* ctx        = nullptr;
    vkx::VkAdapter* vka = nullptr;
    vkx::Renderer* ren  = nullptr;
    MeshItem* meshes    = nullptr;
    u32 meshCount       = 0;
    Vec<TextureItem> textures;
    HashMap<u64, u32> textureIndex; ///< textureId -> index in `textures`
    Vec<Mat4> world;                ///< per-part scratch, sized once for the largest mesh
    Vec3 center;                    ///< union of the placed models' bounds
    f32 radius    = 1.0f;
    bool fit      = true; ///< place_meshes argument, kept for re-placing a changed mesh
    bool warnedU8 = false;
    u32 frame     = 0; ///< the frame being prepared, for the event log
};

/// Requests every BaseColor texture the mesh's materials name, once per texture. They are
/// not waited on: they stream in under the per-frame budget while frames render.
void request_textures(Scene& s, MeshItem const& m) {
    mesh::MeshView const* v = mesh_view(s.ctx, m.handle);
    if (!v) return;
    for (u32 i = 0; i < v->textures().size(); ++i) {
        mesh::TextureBinding const& b = v->textures()[i];
        auto const slot               = mesh::TextureSlot(b.slot);
        if (slot != mesh::TextureSlot::BaseColor) continue;
        char buf[256];
        StrView const path = texture_asset_name(m.path, *v, b, buf, sizeof buf);
        if (path.empty()) {
            KILN_WARN("viewer", "%.*s: texture '%.*s' leaves the store root; skipped", KILN_SV(m.path),
                      KILN_SV(v->str(b.pathStr)));
            continue;
        }
        u64 const id = hash_name(path);
        if (s.textureIndex.find(id)) continue;
        TextureItem t;
        t.textureId = id;
        t.handle = request_texture(s.ctx, path, RequestOptions{.textureKind = texture_kind_for_slot(slot)});
        t.last   = state(s.ctx, t.handle);
        format(t.path, sizeof t.path, "%.*s", KILN_SV(path));
        s.textureIndex.insert(id, u32(s.textures.size()));
        s.textures.push_back(t);
        KILN_INFO("viewer", "request texture %s (%s) for %.*s -> %s", t.path, mesh::texture_slot_name(slot),
                  KILN_SV(m.path), state_name(t.last));
    }
}

/// One pipeline per layout, built as soon as the metadata is readable.
void ensure_pipelines(Scene& s, MeshHandle h) {
    mesh::MeshView const* v = mesh_view(s.ctx, h);
    if (!v) return;
    for (u32 i = 0; i < v->layouts().size(); ++i)
        (void)vkx::renderer_pipeline(s.ren, v->layouts()[i]);
    if (s.world.size() < v->parts().size()) s.world.resize(v->parts().size());
}

void place_meshes(Scene& s, bool fit);

void handle_event(Scene& s, Event const& e) {
    if (e.asset == AssetKind::Mesh) {
        for (u32 i = 0; i < s.meshCount; ++i) {
            MeshItem& m = s.meshes[i];
            if (m.handle.bits() != e.handle) continue;
            State const now = state(s.ctx, m.handle);
            KILN_INFO("viewer", "frame %u: event %-9s mesh    %.*s  (%s -> %s, v%u)", s.frame,
                      event_name(e.kind), KILN_SV(m.path), state_name(m.last), state_name(now), e.version);
            m.last = now;
            if (e.kind == EventKind::Failed) {
                KILN_ERROR("viewer", "  failed: %s", code_name(e.status.code));
                return;
            }
            // A changed mesh (--watch) may name new textures, use a new vertex layout or have
            // new bounds: the same path as a first load, then the row is laid out again.
            ensure_pipelines(s, m.handle);
            if (e.kind != EventKind::MetaReady) request_textures(s, m);
            if (e.kind == EventKind::Changed) place_meshes(s, s.fit);
            return;
        }
    } else {
        for (TextureItem& t : s.textures) {
            if (t.handle.bits() != e.handle) continue;
            State const now   = state(s.ctx, t.handle);
            GpuObject const g = gpu_object(s.ctx, t.handle);
            KILN_INFO("viewer", "frame %u: event %-9s texture %s  (%s -> %s, v%u, slot %d)", s.frame,
                      event_name(e.kind), t.path, state_name(t.last), state_name(now), e.version,
                      g.slot == kInvalid ? -1 : int(g.slot));
            t.last = now;
            if (e.kind == EventKind::Failed) KILN_WARN("viewer", "  failed: %s", code_name(e.status.code));
            return;
        }
    }
    KILN_WARN("viewer", "event %s for an unknown handle", event_name(e.kind));
}

/// Lays the boot meshes out in a row along +X and computes a sphere around the row for the
/// camera. By default each model is scaled uniformly so its bounding radius is kFitRadius and
/// centers are kFitSpacing apart; with `fit` false models keep their native size and are
/// spaced by their radii. Either way each model's bounds center lands on its slot.
void place_meshes(Scene& s, bool fit) {
    // Slot centers along X, starting at 0; recentered on the origin below.
    f32 slots[kMaxMeshes]{};
    f32 cursor = 0, prevRadius = 0;
    u32 placed = 0;
    for (u32 i = 0; i < s.meshCount; ++i) {
        MeshItem& m             = s.meshes[i];
        mesh::MeshView const* v = mesh_view(s.ctx, m.handle);
        if (!v) continue;
        mesh::Bounds const& b = v->model().bounds;
        m.nativeRadius        = b.radius;
        m.scale               = fit && b.radius > 0 ? kFitRadius / b.radius : 1.0f;
        f32 const r           = b.radius > 0 ? b.radius * m.scale : 1.0f;
        if (placed > 0) cursor += fit ? kFitSpacing : 1.2f * (prevRadius + r);
        slots[i]   = cursor;
        prevRadius = r;
        ++placed;
    }
    if (placed == 0) return;
    f32 const mid = cursor * 0.5f;
    f32 radius    = 1e-3f;
    for (u32 i = 0; i < s.meshCount; ++i) {
        MeshItem& m             = s.meshes[i];
        mesh::MeshView const* v = mesh_view(s.ctx, m.handle);
        if (!v) continue;
        mesh::Bounds const& b = v->model().bounds;
        Vec3 const slot{slots[i] - mid, 0, 0};
        m.place = vkx::translation(slot) * vkx::scaling(m.scale) *
                  vkx::translation(Vec3{-b.center[0], -b.center[1], -b.center[2]});
        m.worldCenter = slot;
        m.worldRadius = b.radius > 0 ? b.radius * m.scale : 1.0f;
        radius        = max(radius, vkx::length(slot) + m.worldRadius);
        KILN_INFO("viewer", "model %.*s: native radius %.4g, scale %.4g, at x %.3f", KILN_SV(m.path),
                  double(m.nativeRadius), double(m.scale), double(slot.x));
    }
    s.center = Vec3{};
    s.radius = radius;
}

/// Base-color slot and draw flags for a material (spec §5.7).
void material_bindings(Scene& s, StrView meshPath, mesh::MeshView const& v, u32 material,
                       vkx::DrawPush& push) {
    push.baseColorSlot = 0;
    push.flags         = 0;
    if (material >= v.materials().size()) return;
    mesh::MaterialSlot const& mat = v.materials()[material];
    if (mat.flags & mesh::kMaterialVertexColor) push.flags |= vkx::kDrawVertexColor;
    for (u32 t = 0; t < mat.textureCount; ++t) {
        u32 const ti = mat.textureFirst + t;
        if (ti >= v.textures().size()) break;
        mesh::TextureBinding const& b = v.textures()[ti];
        if (mesh::TextureSlot(b.slot) != mesh::TextureSlot::BaseColor) continue;
        char buf[256];
        StrView const name = texture_asset_name(meshPath, v, b, buf, sizeof buf);
        if (u32 const* idx = name.empty() ? nullptr : s.textureIndex.find(hash_name(name))) {
            GpuObject const g = gpu_object(s.ctx, s.textures[*idx].handle);
            if (g.slot != kInvalid) {
                push.baseColorSlot = g.slot;
                push.flags |= vkx::kDrawBaseColor;
            }
        }
        return;
    }
}

/// The entry of `material` of mesh `mi` in FrameUniforms::materials: every mesh whose metadata is
/// here takes the next entries, in mesh order. Past the table: the last entry, glTF's defaults.
u32 material_entry(Scene const& s, u32 mi, u32 material) {
    u32 base = 0;
    for (u32 i = 0; i < mi; ++i)
        if (mesh::MeshView const* v = mesh_view(s.ctx, s.meshes[i].handle)) base += v->materials().size();
    return min(base + material, vkx::kMaxMaterials - 1);
}

/// Spec §8 per part: world matrix from the parent chain, LOD 0, one draw per submesh.
void draw_scene(Scene& s, VkCommandBuffer cmd) {
    VkPipelineLayout const layout = vkx::renderer_pipeline_layout(s.ren);
    VkBuffer const zero           = vkx::renderer_zero_buffer(s.ren);
    VkPipeline bound              = VK_NULL_HANDLE;
    for (u32 mi = 0; mi < s.meshCount; ++mi) {
        MeshItem const& m = s.meshes[mi];
        if (!is_ready(s.ctx, m.handle)) continue;
        mesh::MeshView const* v        = mesh_view(s.ctx, m.handle);
        vkx::MeshPayload const payload = vkx::adapter_mesh(s.vka, gpu_object(s.ctx, m.handle));
        if (!v || !payload.buffer) continue;
        u32 const partCount = v->parts().size();
        if (s.world.size() < partCount) s.world.resize(partCount);
        Mat4 const& place = m.place;

        for (u32 p = 0; p < partCount; ++p) {
            mesh::MeshPart const& part = v->parts()[p];
            Mat4 const local           = vkx::from_rt(part.translation, part.rotation);
            // Parts are topologically ordered (parent < self), so the parent is already resolved.
            s.world[p] = part.parent < p ? s.world[part.parent] * local : place * local;
            if (part.lodCount == 0) continue;

            mesh::MeshLod const& lod = v->lods()[part.lodFirst];
            auto const indexType     = mesh::IndexType(lod.indexType);
            if (indexType == mesh::IndexType::U8) {
                if (!s.warnedU8)
                    KILN_WARN("viewer",
                              "%.*s: U8 indices need indexTypeUint8, which the device setup does "
                              "not enable; skipping those LODs",
                              KILN_SV(m.path));
                s.warnedU8 = true;
                continue;
            }
            vkx::LayoutPipeline const* pipe = vkx::renderer_pipeline(s.ren, v->layouts()[lod.layout]);
            if (!pipe) continue;
            if (pipe->pipeline != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
                bound = pipe->pipeline;
            }

            VkBuffer buffers[mesh::kMaxStreams + 6];
            VkDeviceSize offsets[mesh::kMaxStreams + 6];
            VkDeviceSize sizes[mesh::kMaxStreams + 6];
            u32 n        = 0;
            bool missing = false;
            for (u32 st = 0; st < pipe->streamCount; ++st, ++n) {
                if (lod.streamOffset[st] == kInvalid) missing = true;
                buffers[n] = payload.buffer;
                offsets[n] = payload.offset + lod.streamOffset[st];
                sizes[n]   = v->stream_bytes(lod, st);
            }
            if (missing) continue;
            for (u32 z = 0; z < pipe->zeroBindings; ++z, ++n) {
                buffers[n] = zero;
                offsets[n] = 0;
                sizes[n]   = VK_WHOLE_SIZE;
            }
            vkCmdBindVertexBuffers2(cmd, 0, n, buffers, offsets, sizes, nullptr);
            vkCmdBindIndexBuffer2(
                cmd, payload.buffer, payload.offset + lod.indexOffset, mesh::MeshView::index_bytes(lod),
                indexType == mesh::IndexType::U32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);

            vkx::DrawPush push{};
            std::memcpy(push.model, s.world[p].m, sizeof push.model);
            for (u32 c = 0; c < 3; ++c) {
                push.posScale[c] = part.posScale[c];
                push.posBias[c]  = part.posBias[c];
            }
            for (u32 si = 0; si < lod.submeshCount; ++si) {
                mesh::Submesh const& sm = v->submeshes()[lod.submeshFirst + si];
                material_bindings(s, m.path, *v, sm.material, push);
                push.material = material_entry(s, mi, sm.material);
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof push, &push);
                // indexFirst is relative to the LOD's index range, which is where the buffer is bound.
                vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexFirst, sm.vertexBase, 0);
            }
        }
    }
}

// --- Camera and window input --------------------------------------------------------------------

struct Camera {
    f32 azimuth   = 45.0f * kPi / 180.0f;
    f32 elevation = 30.0f * kPi / 180.0f;
    f32 zoom      = 1.0f; ///< distance multiplier (wheel)
};

struct Input {
    Camera camera;
    bool dragging = false;
    double lastX = 0, lastY = 0;
    vkx::Renderer* ren = nullptr;
};

/// Distance along `dir` (target -> eye) at which every placed model's bounding sphere is inside
/// the view frustum: per sphere, its offset across the view plus r / cos(half angle), over
/// tan(half angle), in front of the sphere's depth; the largest over both axes and all models.
f32 framing_distance(Scene const& s, Vec3 dir, f32 aspect) {
    Vec3 const right = vkx::normalize(vkx::cross(Vec3{0, 1, 0}, dir));
    Vec3 const up    = vkx::cross(dir, right);
    f32 const tanV   = std::tan(kFovY * 0.5f);
    f32 const tanH   = tanV * aspect;
    f32 const secV   = std::sqrt(1.0f + tanV * tanV);
    f32 const secH   = std::sqrt(1.0f + tanH * tanH);
    f32 dist         = 0.0f;
    for (u32 i = 0; i < s.meshCount; ++i) {
        MeshItem const& m = s.meshes[i];
        if (m.worldRadius <= 0) continue;
        Vec3 const c  = m.worldCenter - s.center;
        f32 const x   = std::fabs(vkx::dot(c, right));
        f32 const y   = std::fabs(vkx::dot(c, up));
        f32 const fit = max((x + m.worldRadius * secH) / tanH, (y + m.worldRadius * secV) / tanV);
        dist          = max(dist, vkx::dot(c, dir) + fit);
    }
    return dist > 0 ? dist : s.radius / std::sin(kFovY * 0.5f);
}

/// Every mesh and texture of the scene is Ready or Failed.
bool scene_settled(Scene const& s) {
    auto const done = [](State st) { return st == State::Ready || st == State::Failed; };
    for (u32 i = 0; i < s.meshCount; ++i)
        if (!done(state(s.ctx, s.meshes[i].handle))) return false;
    for (TextureItem const& t : s.textures)
        if (!done(state(s.ctx, t.handle))) return false;
    return true;
}

char const* const kTonemaps[] = {"auto", "none", "aces", nullptr};

/// True when `format` holds values above 1 (a float format).
bool is_hdr_format(Format f) {
    FormatInfo const* fi = format_info(f);
    return fi && (fi->kind == FormatKind::SFloat || fi->kind == FormatKind::UFloat);
}

/// The frame uniforms, and the camera basis the sky shader turns into a ray per pixel.
void write_uniforms(Scene const& s, Camera const& cam, VkExtent2D extent, bool aces, f32 exposure,
                    vkx::FrameUniforms* u, vkx::SkyPush* sky) {
    f32 const aspect = extent.height ? f32(extent.width) / f32(extent.height) : 1.0f;
    Vec3 const dir{std::cos(cam.elevation) * std::sin(cam.azimuth), std::sin(cam.elevation),
                   std::cos(cam.elevation) * std::cos(cam.azimuth)};
    f32 const dist  = framing_distance(s, dir, aspect) * 1.05f * cam.zoom;
    Vec3 const eye  = s.center + dir * dist;
    f32 const nearZ = max(dist - s.radius * 2.0f, dist * 0.01f);
    f32 const farZ  = dist + s.radius * 2.0f;
    Mat4 const vp = vkx::perspective(kFovY, aspect, nearZ, farZ) * vkx::look_at(eye, s.center, Vec3{0, 1, 0});
    std::memcpy(u->viewProj, vp.m, sizeof u->viewProj);
    u->cameraPos[0]  = eye.x;
    u->cameraPos[1]  = eye.y;
    u->cameraPos[2]  = eye.z;
    u->cameraPos[3]  = 1.0f;
    Vec3 const light = vkx::normalize(Vec3{0.45f, 0.8f, 0.6f});
    u->lightDir[0]   = light.x;
    u->lightDir[1]   = light.y;
    u->lightDir[2]   = light.z;
    u->lightDir[3]   = 0.0f;
    u->tonemap[0]    = exposure;
    for (u32 i = 0; i < vkx::kMaxMaterials; ++i)
        u->materials[i] = vkx::material_uniforms(nullptr, 0); // glTF's defaults
    for (u32 mi = 0; mi < s.meshCount; ++mi)
        if (mesh::MeshView const* v = mesh_view(s.ctx, s.meshes[mi].handle))
            for (u32 k = 0; k < v->materials().size(); ++k)
                if (u32 const e = material_entry(s, mi, k); e + 1 < vkx::kMaxMaterials) {
                    u->materials[e] = vkx::material_uniforms(v, k);
                    // The emissive factor multiplies an emissive map, which the viewer does not bind.
                    mesh::MaterialSlot const& mat = v->materials()[k];
                    for (u32 t = 0; t < mat.textureCount && mat.textureFirst + t < v->textures().size(); ++t)
                        if (mesh::TextureSlot(v->textures()[mat.textureFirst + t].slot) ==
                            mesh::TextureSlot::Emissive)
                            u->materials[e].emissiveNormal[0]     = u->materials[e].emissiveNormal[1] =
                                u->materials[e].emissiveNormal[2] = 0.0f;
                }
    u->tonemap[1] = aces ? 1.0f : 0.0f;
    u->tonemap[2] = 0.0f;
    u->tonemap[3] = 0.0f;

    // The same basis as look_at(); right and up span the view at distance 1.
    Vec3 const f    = vkx::normalize(s.center - eye);
    Vec3 const side = vkx::normalize(vkx::cross(f, Vec3{0, 1, 0}));
    Vec3 const up   = vkx::cross(side, f);
    f32 const tanV  = std::tan(kFovY * 0.5f);
    Vec3 const r    = side * (tanV * aspect);
    Vec3 const v    = up * tanV;
    *sky            = vkx::SkyPush{
                   .forward  = {f.x, f.y, f.z, 0},
                   .right    = {r.x, r.y, r.z, 0},
                   .up       = {v.x, v.y, v.z, 0},
                   .cubeSlot = kInvalid,
                   .pad      = {}
    };
}

Input* input_of(GLFWwindow* w) { return static_cast<Input*>(glfwGetWindowUserPointer(w)); }

void on_mouse_button(GLFWwindow* w, int button, int action, int /*mods*/) {
    Input* in = input_of(w);
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    in->dragging = action == GLFW_PRESS;
    if (in->dragging) glfwGetCursorPos(w, &in->lastX, &in->lastY);
}

void on_cursor(GLFWwindow* w, double x, double y) {
    Input* in = input_of(w);
    if (!in->dragging) return;
    in->camera.azimuth -= f32(x - in->lastX) * 0.01f;
    in->camera.elevation = clamp(in->camera.elevation + f32(y - in->lastY) * 0.01f, -1.5f, 1.5f);
    in->lastX            = x;
    in->lastY            = y;
}

void on_scroll(GLFWwindow* w, double /*dx*/, double dy) {
    Input* in       = input_of(w);
    in->camera.zoom = clamp(in->camera.zoom * std::pow(0.9f, f32(dy)), 0.05f, 20.0f);
}

void on_key(GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE);
}

void on_framebuffer(GLFWwindow* w, int /*width*/, int /*height*/) {
    Input* in = input_of(w);
    if (in->ren) vkx::renderer_resize(in->ren);
}

void framebuffer_size(void* user, u32* width, u32* height) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(user), &w, &h);
    *width  = w > 0 ? u32(w) : 0u;
    *height = h > 0 ? u32(h) : 0u;
}

void glfw_error(int code, char const* message) { KILN_ERROR("glfw", "%d: %s", code, message); }

bool write_png(char const* path, Span<u8 const> rgba, u32 w, u32 h) {
    Vec<u8> const png =
        test::png::encode({.width = w, .height = h, .colorType = 6, .depth = 8, .pixels = rgba});
    if (png.empty()) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool const ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

/// Everything main() tears down, in reverse order of creation.
struct App {
    GLFWwindow* window = nullptr;
    vkx::Device device{};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vkx::VkAdapter* vka  = nullptr;
    vkx::Renderer* ren   = nullptr;
    Context* ctx         = nullptr;
    bool provider        = false;

    ~App() {
        if (ren) vkx::renderer_wait_idle(ren);
#if KILN_VIEWER_HAS_COOK
        if (provider) cook::uninstall_provider(ctx);
#endif
        destroy(ctx); // hands every GPU object to Adapter::destroy
        vkx::renderer_destroy(ren);
        vkx::adapter_destroy(vka);
        // Offscreen runs enable no surface extension: volk leaves this function null.
        if (surface) vkDestroySurfaceKHR(device.instance, surface, nullptr);
        vkx::device_destroy(device);
        glfwTerminate(); // destroys the window; nothing when GLFW never started
    }
};

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    Options o;
    cli::Option const opts[] = {
        {.name = "--store", .arg = "<dir>", .help = "cooked store root (default: cooked)", .str = &o.store},
        {.name = "--source",
         .arg  = "<dir>",
         .help = "the default root; enables cook-on-miss (needs kiln_cook)",
         .each = &add_root,
         .user = &o},
        {.name = "--root",
         .arg  = "[<name>=]<dir>",
         .help = "a source root for cook-on-miss; <name>=<dir> names <name>:<path> (repeatable)",
         .each = &add_root,
         .user = &o},
        {.name = "--validate",
         .help = "enable the Vulkan validation layer if installed",
         .flag = &o.validate},
        {.name   = "--width",
         .arg    = "<px>",
         .help   = "window or image width (default: 1280)",
         .number = &o.width,
         .max    = 16384},
        {.name   = "--height",
         .arg    = "<px>",
         .help   = "window or image height (default: 720)",
         .number = &o.height,
         .max    = 16384},
        {.name   = "--budget-mib",
         .arg    = "<n>",
         .help   = "upload bytes committed per frame, in MiB (default: 8)",
         .number = &o.budgetMiB,
         .max    = 4096},
        {.name = "--offscreen", .help = "render into an image with no window", .flag = &o.offscreen},
        {.name = "--watch",
         .help = "hot reload: reload store files that change; with --source, also re-cook changed sources",
         .flag = &o.watch},
        {.name = "--no-fit",
         .help = "keep native model sizes (default: scale each model to radius 1, 2.5 units apart)",
         .flag = &o.noFit},
        {.name   = "--frames",
         .arg    = "<n>",
         .help   = "render exactly <n> frames, then exit (offscreen default: until the scene settles)",
         .number = &o.frames},
        {.name = "--dump",
         .arg  = "<file.png>",
         .help = "offscreen: write the last frame as a PNG",
         .str  = &o.dump},
        {.name = "--at",
         .arg  = "<ms>",
         .help = "offscreen: stop at the first frame rendered at or after <ms> since the first request",
         .real = &o.atMs},
        {.name   = "--timeout",
         .arg    = "<s>",
         .help   = "offscreen: stop waiting for the scene to settle after <s> seconds (default: 60)",
         .number = &o.timeoutS},
        {.name = "--exposure",
         .arg  = "<ev>",
         .help = "scale colors by 2^<ev> before the tonemap (default: 0)",
         .real = &o.exposure},
        {.name    = "--tonemap",
         .arg     = "<mode>",
         .help    = "auto (aces for an HDR sky, else none), none (clamp) or aces",
         .str     = &o.tonemap,
         .choices = kTonemaps},
        {.name = "--sky",
         .arg  = "<name>",
         .help = "a cube texture drawn behind the scene, e.g. sky_cube.png (a vertical strip of 6 faces)",
         .str  = &o.sky},
        {.name   = "--threads",
         .arg    = "<n>",
         .help   = "kiln worker threads (default: 0 = auto)",
         .number = &o.threads,
         .max    = 256},
    };
    cli::Spec const spec{
        .program    = "kiln-viewer",
        .synopsis   = "[options] <mesh>...",
        .options    = {opts, countof(opts)},
        .footer     = "<mesh> is an asset name, e.g. mesh/Box.glb or lib:props/chair.glb. Window: left-drag "
                      "orbits, wheel\n"
                      "zooms, Esc quits.\n"
                      "Exit codes: 0 ok, 1 a boot asset Failed or the scene did not settle within --timeout,\n"
                      "2 usage or setup error.",
        .positional = &add_mesh,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || o.meshCount == 0 || o.width == 0 || o.height == 0) {
        cli::usage(spec, stderr);
        return 2;
    }
    if (o.dump && !o.offscreen) {
        std::fprintf(stderr, "kiln-viewer: --dump needs --offscreen\n");
        return 2;
    }
    if (o.atMs >= 0 && (!o.offscreen || o.frames)) {
        std::fprintf(stderr, "kiln-viewer: --at needs --offscreen and excludes --frames\n");
        return 2;
    }
    u32 const maxFrames = o.frames;
    set_log_sink(LogSink{&log_to_stdout, nullptr});
    DiagSink const diag{&diag_to_stdout, nullptr};
    App app;

    // 1. Window (unless offscreen) and device.
    Input input;
    char const* const* glfwExtensions = nullptr;
    u32 glfwExtensionCount            = 0;
    if (!o.offscreen) {
        glfwSetErrorCallback(&glfw_error);
        if (!glfwInit()) return 2;
        if (!glfwVulkanSupported()) {
            KILN_ERROR("viewer", "GLFW found no Vulkan loader");
            return 2;
        }
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        app.window = glfwCreateWindow(int(o.width), int(o.height), "kiln-viewer", nullptr, nullptr);
        if (!app.window) return 2;
        glfwSetWindowUserPointer(app.window, &input);
        glfwSetMouseButtonCallback(app.window, &on_mouse_button);
        glfwSetCursorPosCallback(app.window, &on_cursor);
        glfwSetScrollCallback(app.window, &on_scroll);
        glfwSetKeyCallback(app.window, &on_key);
        glfwSetFramebufferSizeCallback(app.window, &on_framebuffer);
    }
    Result<vkx::Device> dev =
        vkx::device_create({.validation         = o.validate,
                            .instanceExtensions = Span<char const* const>(glfwExtensions, glfwExtensionCount),
                            .needSwapchain      = !o.offscreen},
                           &diag);
    if (dev.failed()) {
        KILN_ERROR("viewer", "device_create: %s", code_name(dev.code()));
        return 2;
    }
    app.device = dev.value();
    if (app.window) {
        VkResult const r = glfwCreateWindowSurface(app.device.instance, app.window, nullptr, &app.surface);
        if (r != VK_SUCCESS) {
            KILN_ERROR("viewer", "glfwCreateWindowSurface: %s", vkx::result_name(r));
            return 2;
        }
    }

    // 2. Adapter, renderer, context.
    Adapter adapter{};
    Result<vkx::VkAdapter*> va = vkx::adapter_create({.device = &app.device}, &adapter);
    if (va.failed()) {
        KILN_ERROR("viewer", "adapter_create: %s", code_name(va.code()));
        return 2;
    }
    app.vka                   = va.value();
    Result<vkx::Renderer*> rr = vkx::renderer_create({.device          = &app.device,
                                                      .adapter         = app.vka,
                                                      .surface         = app.surface,
                                                      .width           = o.width,
                                                      .height          = o.height,
                                                      .framebufferSize = &framebuffer_size,
                                                      .user            = app.window});
    if (rr.failed()) {
        KILN_ERROR("viewer", "renderer_create: %s", code_name(rr.code()));
        return 2;
    }
    app.ren   = rr.value();
    input.ren = app.ren;

    Result<Context*> c = create(ContextDesc{
        .diag          = diag,
        .adapter       = &adapter,
        .storeDir      = StrView(o.store),
        .roots         = Span<Root const>(o.roots, o.rootCount),
        .hotReload     = {.watchStore = o.watch},
        .workerThreads = o.threads,
    });
    if (c.failed()) {
        KILN_ERROR("viewer", "create: %s", code_name(c.code()));
        return 2;
    }
    app.ctx = c.value();
#if KILN_VIEWER_HAS_COOK
    if (o.rootCount) {
        Status const st = cook::install_provider(app.ctx, cook::ProviderDesc{.watchSources = o.watch});
        if (st.failed()) {
            KILN_ERROR("viewer", "install_provider: %s", code_name(st.code));
            return 2;
        }
        app.provider = true;
    }
#else
    if (o.rootCount) KILN_WARN("viewer", "built without kiln_cook: --source and --root are ignored");
#endif

    // 3. The boot group: every mesh on the command line, waited on before the first frame.
    Scene scene;
    scene.ctx       = app.ctx;
    scene.vka       = app.vka;
    scene.ren       = app.ren;
    scene.meshes    = o.meshes;
    scene.meshCount = o.meshCount;
    scene.fit       = !o.noFit;
    scene.textures.init(default_allocator(), Tag::General);
    scene.textures.reserve(64);
    scene.textureIndex.init(default_allocator(), Tag::General);
    scene.textureIndex.reserve(64);
    scene.world.init(default_allocator(), Tag::General);

    Group const boot = group(app.ctx);
    for (u32 i = 0; i < o.meshCount; ++i)
        o.meshes[i].handle = request_mesh(app.ctx, o.meshes[i].path, RequestOptions{.group = boot});
    Clock::time_point const bootStart = Clock::now(); // also the time --at counts from
    GroupStatus const gs              = wait(app.ctx, boot, WaitOptions{.timeoutMs = kBootTimeoutMs});
    double const bootMs = std::chrono::duration<double, std::milli>(Clock::now() - bootStart).count();
    KILN_INFO("viewer", "boot group settled in %.1f ms: %u ready, %u failed, %u pending", bootMs, gs.ready,
              gs.failed, gs.pending);
    release(app.ctx, boot);
    u32 bootFailed = 0;
    for (u32 i = 0; i < o.meshCount; ++i) {
        MeshItem& m = o.meshes[i];
        m.last      = state(app.ctx, m.handle);
        KILN_INFO("viewer", "boot mesh %.*s: %s", KILN_SV(m.path), state_name(m.last));
        if (m.last != State::Ready) ++bootFailed;
    }
    if (bootFailed) {
        KILN_ERROR("viewer", "%u boot mesh(es) did not load", bootFailed);
        return 1;
    }
    // Events raised inside wait() are gone by now; set up what the MetaReady / Ready handlers do.
    for (u32 i = 0; i < o.meshCount; ++i) {
        ensure_pipelines(scene, o.meshes[i].handle);
        request_textures(scene, o.meshes[i]);
    }
    place_meshes(scene, !o.noFit);
    // The sky is one more texture item, so its events show in the log like the others.
    u32 skyItem = kInvalid;
    if (o.sky) {
        TextureItem t;
        t.textureId = hash_name(StrView(o.sky));
        t.handle =
            request_texture(app.ctx, StrView(o.sky), RequestOptions{.textureShape = TextureShape::Cube});
        t.last = state(app.ctx, t.handle);
        format(t.path, sizeof t.path, "%s", o.sky);
        skyItem = u32(scene.textures.size());
        scene.textures.push_back(t);
        KILN_INFO("viewer", "request sky %s (cube) -> %s", o.sky, state_name(t.last));
    }
    KILN_INFO("viewer",
              "scene: %u mesh(es)%s, row radius %.3f, %u texture(s) streaming, budget %u MiB per frame",
              o.meshCount, o.noFit ? " at native size" : " fitted to radius 1", double(scene.radius),
              u32(scene.textures.size()), o.budgetMiB);

    // 4. Frames.
    PumpOptions pumpOpt{.uploadBytes = u64(o.budgetMiB) << 20};
    u32 frames      = 0;
    double totalMs  = 0;
    double worstMs  = 0;
    u32 worstFrame  = 0;
    double warmMs   = 0; ///< sum over frames after the warm-up
    u32 warmFrames  = 0;
    u64 uploads     = 0;
    u64 uploadBytes = 0;
    u32 busyRetries = 0;
    bool timedOut   = false;
    for (;;) {
        if (app.window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(app.window)) break;
        }
        if (!o.offscreen && maxFrames && frames >= maxFrames) break;

        vkx::FrameNumbers const fn = vkx::renderer_wait_frame(app.ren);
        pumpOpt.frame              = fn.frame;
        pumpOpt.completedFrame     = fn.completed;
        Clock::time_point const t0 = Clock::now();
        scene.frame                = frames + 1;
        PumpStats const ps         = pump(app.ctx, pumpOpt);
        uploads += ps.uploadsCommitted;
        uploadBytes += ps.uploadBytes;
        busyRetries += ps.busyRetries;
        Span<Event const> const evs = events(app.ctx);
        for (Event const& e : evs)
            handle_event(scene, e);

        // Offscreen, one of three triggers picks the last frame: --at, --frames, or the scene
        // settling (the default). The frame after the pump that settled it shows every texture.
        bool last = false;
        if (o.offscreen) {
            if (o.atMs >= 0) {
                last = ms_since(bootStart) >= o.atMs;
            } else if (maxFrames) {
                last = frames + 1 == maxFrames;
            } else if (scene_settled(scene)) {
                last = true;
                KILN_INFO("viewer", "frame %u: scene settled", frames + 1);
            } else if (ms_since(bootStart) >= o.timeoutS * 1000.0) {
                last = timedOut = true;
                KILN_WARN("viewer", "frame %u: not settled after %u s; this frame shows what is there",
                          frames + 1, o.timeoutS);
            }
        }

        VkCommandBuffer const cmd = vkx::renderer_begin(app.ren);
        if (!cmd) {
            if (app.window) glfwWaitEventsTimeout(0.05); // minimized or resizing
            continue;
        }
        // auto: ACES once the sky has loaded as a float (HDR) texture; LDR scenes look as before.
        bool aces = std::strcmp(o.tonemap, "aces") == 0;
        if (std::strcmp(o.tonemap, "auto") == 0 && skyItem != kInvalid) {
            TextureInfo const ti = texture_info(app.ctx, scene.textures[skyItem].handle);
            aces                 = !ti.isPlaceholder && is_hdr_format(ti.desc.format);
        }
        vkx::SkyPush sky{};
        write_uniforms(scene, input.camera, vkx::renderer_extent(app.ren), aces, f32(std::exp2(o.exposure)),
                       vkx::renderer_uniforms(app.ren), &sky);
        if (skyItem != kInvalid) {
            // The slot serves the cube placeholder until the real cube arrives.
            sky.cubeSlot = gpu_object(app.ctx, scene.textures[skyItem].handle).slot;
            if (sky.cubeSlot != kInvalid) vkx::renderer_draw_sky(app.ren, cmd, sky);
        }
        draw_scene(scene, cmd);
        vkx::renderer_end(app.ren, last && o.dump != nullptr);

        double const ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        ++frames;
        totalMs += ms;
        if (ms > worstMs) {
            worstMs    = ms;
            worstFrame = frames;
        }
        if (frames > kWarmupFrames) {
            // The floor keeps sub-millisecond jitter out of the log.
            if (warmFrames > 0 && ms > 2.0 * (warmMs / warmFrames) && ms > kSpikeFloorMs)
                KILN_WARN("viewer", "frame %u: %.2f ms CPU, over 2x the running average %.2f ms", frames, ms,
                          warmMs / warmFrames);
            warmMs += ms;
            ++warmFrames;
        }
        if (last) break;
        // Offscreen nothing paces the loop: rest a little when the pump had nothing to do.
        if (o.offscreen && evs.empty() && ps.completed == 0 && ps.uploadsCommitted == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    vkx::renderer_wait_idle(app.ren);

    // 5. The dump and the summary.
    int exitCode = timedOut ? 1 : 0;
    if (o.dump && frames > 0) {
        Vec<u8> rgba(default_allocator(), Tag::General);
        u32 w = 0, h = 0;
        if (vkx::renderer_read_back(app.ren, &rgba, &w, &h).failed() ||
            !write_png(o.dump, rgba.span(), w, h)) {
            KILN_ERROR("viewer", "could not write %s", o.dump);
            exitCode = 2;
        } else {
            KILN_INFO("viewer", "wrote %s (%ux%u, %s)", o.dump, w, h,
                      vkx::renderer_color_format_name(app.ren));
        }
    }
    u32 texturesReady = 0;
    for (TextureItem const& t : scene.textures)
        texturesReady += state(app.ctx, t.handle) == State::Ready ? 1u : 0u;
    KILN_INFO("viewer", "%u frames, CPU (pump + record + submit) avg %.2f ms, worst %.2f ms (frame %u)",
              frames, frames ? totalMs / frames : 0.0, worstMs, worstFrame);
    KILN_INFO("viewer",
              "pump: %llu uploads committed, %llu bytes started (counts Busy retries again), %u busy "
              "retries; textures ready %u/%u",
              ull(uploads), ull(uploadBytes), busyRetries, texturesReady, u32(scene.textures.size()));
    ex::log_adapter_stats("viewer", vkx::adapter_stats(app.vka));

    for (TextureItem const& t : scene.textures)
        release(app.ctx, t.handle);
    for (u32 i = 0; i < o.meshCount; ++i)
        release(app.ctx, o.meshes[i].handle);
    return exitCode;
}
