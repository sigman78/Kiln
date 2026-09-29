// examples/gl/main_bindless.cpp — kiln-gl-bindless: kiln-gl with ARB_bindless_texture. Each texture
// gets kiln's slot number at request time; Adapter::bind writes the resident handle into a table
// at the request (placeholder), on arrival and on reload. Materials store slot numbers once and
// never look textures up per frame (docs/design/integration-examples.md).
#include "gl_adapter.h"
#include "gl_util.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_GL_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include <GLFW/glfw3.h>

#include <cstring>

using namespace kiln;
using namespace kiln::glx;

namespace {

constexpr u32 kMaxTextures  = 32;
constexpr u32 kMaxMaterials = 64;
constexpr u32 kSlotsPerDraw = 6; // base color, normal, metal-rough, occlusion, emissive, sky

constexpr char const* kBindlessHeader = "#extension GL_ARB_bindless_texture : require\n";

constexpr char const* kMeshFs = R"(
in vec3 vWorld;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUv;
layout(std430, binding = 0) readonly buffer Handles { uvec2 uHandles[]; }; // gl_handle_table()
layout(location = 2) uniform vec3 uEye;
layout(location = 4) uniform float uExposure;
layout(location = 5) uniform uint uSlots[6]; // see kSlotsPerDraw; 0xFFFFFFFF = none
out vec4 outColor;
bool has(int i) { return uSlots[i] != 0xFFFFFFFFu; }
vec4 tex(int i) { return texture(sampler2D(uHandles[uSlots[i]]), vUv); }
vec3 sky(vec3 d, float lod) { return textureLod(samplerCube(uHandles[uSlots[5]]), cube_dir(d), lod).rgb; }
void main() {
    vec3 base     = has(0) ? tex(0).rgb : vec3(0.8);
    vec3 n        = normalize(vNormal);
    if (has(1)) n = perturb(n, vTangent, tex(1).xyz);
    vec3 mr       = has(2) ? tex(2).rgb : vec3(1.0, 0.7, 0.0); // G rough, B metal
    float ao      = has(3) ? tex(3).r : 1.0;
    vec3 emissive = has(4) ? tex(4).rgb : vec3(0.0);
    float rough   = clamp(mr.g, 0.05, 1.0);
    vec3 v        = normalize(uEye - vWorld);
    vec3 ambient  = has(5) ? sky(n, 6.0) : vec3(0.3);
    vec3 env      = has(5) ? sky(reflect(-v, n), rough * 6.0) : vec3(0.3);
    outColor      = display(shade(base, n, v, rough, mr.b, ao, ambient, env) + emissive, uExposure);
}
)";

constexpr char const* kSkyFs = R"(
in vec2 vNdc;
layout(std430, binding = 0) readonly buffer Handles { uvec2 uHandles[]; };
layout(location = 0) uniform vec3 uForward;
layout(location = 1) uniform vec3 uRight;
layout(location = 2) uniform vec3 uUp;
layout(location = 4) uniform float uExposure;
layout(location = 5) uniform uint uSkySlot;
out vec4 outColor;
void main() {
    vec3 dir = normalize(uForward + vNdc.x * uRight + vNdc.y * uUp);
    outColor = display(texture(samplerCube(uHandles[uSkySlot]), cube_dir(dir)).rgb, uExposure);
}
)";

/// The frame numbers kiln needs (PumpOptions): a fence after each frame's draws. A resident handle
/// must stay resident until the frames that read it finished.
struct FrameFences {
    static constexpr u32 kMax = 4; ///< fences kept; a new frame starts with at most kMax - 1 in flight
    GLsync fences[kMax]       = {};
    u64 frames[kMax]          = {};
    u32 count                 = 0;
    u64 next                  = 1; ///< the frame about to be recorded
    u64 completed             = 0;

    /// Collects finished frames; blocks on the oldest one while kMax are in flight.
    void poll() noexcept {
        while (count) {
            GLuint64 const timeout = count == kMax ? GLuint64(100'000'000) : 0;
            GLenum const r         = glClientWaitSync(fences[0], GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
            if (r == GL_WAIT_FAILED) KILN_PANIC("gl: glClientWaitSync failed on a frame fence");
            if (r == GL_TIMEOUT_EXPIRED) {
                if (count == kMax) continue; // the next end_frame() needs a free entry
                return;
            }
            glDeleteSync(fences[0]);
            completed = frames[0];
            for (u32 i = 1; i < count; ++i) {
                fences[i - 1] = fences[i];
                frames[i - 1] = frames[i];
            }
            --count;
        }
    }
    /// After the frame's draws.
    void end_frame() noexcept {
        fences[count] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        frames[count] = next++;
        ++count;
    }
    void release() noexcept {
        for (u32 i = 0; i < count; ++i)
            glDeleteSync(fences[i]);
        count = 0;
    }
};

struct TextureItem {
    AssetId id = 0;
    TextureHandle handle;
};

struct Scene {
    /// The model content version the materials and geometry were set up for. MetaReady, Changed
    /// and a Ready that follows Failed (a repaired model: reloads emit no MetaReady) carry a new one.
    u32 preparedVersion = 0;
    Context* ctx        = nullptr;
    StrView modelName;
    MeshHandle model;
    TextureHandle sky;
    u32 skySlot = kInvalid;
    TextureItem textures[kMaxTextures];
    u32 textureCount = 0;
    u32 materialSlots[kMaxMaterials][kSlotsPerDraw]; ///< filled at request time, read every draw
    Geometry geometry;
};

/// The per-draw slot index a material texture slot goes to; kInvalid for ones this shader ignores.
u32 draw_index(mesh::TextureSlot slot) {
    switch (slot) {
    case mesh::TextureSlot::BaseColor: return 0;
    case mesh::TextureSlot::Normal: return 1;
    case mesh::TextureSlot::MetalRough: return 2;
    case mesh::TextureSlot::Occlusion: return 3;
    case mesh::TextureSlot::Emissive: return 4;
    default: return kInvalid;
    }
}

TextureHandle texture_for(Scene& s, StrView name, mesh::TextureSlot slot) {
    AssetId const id = asset_id(name);
    for (u32 i = 0; i < s.textureCount; ++i)
        if (s.textures[i].id == id) return s.textures[i].handle;
    if (id == 0 || s.textureCount == kMaxTextures) return {};
    RequestOptions const opt{.textureKind = texture_kind_for_slot(slot)};
    s.textures[s.textureCount++] = {id, request_texture(s.ctx, name, opt)};
    return s.textures[s.textureCount - 1].handle;
}

/// Requests the model's textures and records each material's slots. The slot is known at once and
/// shows the placeholder until the texture arrives. Nothing here runs again per frame.
void request_textures(Scene& s, mesh::MeshView const& v) {
    for (u32 m = 0; m < kMaxMaterials; ++m)
        for (u32 i = 0; i < kSlotsPerDraw; ++i)
            s.materialSlots[m][i] = i == 5 ? s.skySlot : kInvalid;
    for (u32 m = 0; m < v.materials().size() && m < kMaxMaterials; ++m) {
        mesh::MaterialSlot const& mat = v.materials()[m];
        for (u32 t = 0; t < mat.textureCount && mat.textureFirst + t < v.textures().size(); ++t) {
            mesh::TextureBinding const& b = v.textures()[mat.textureFirst + t];
            u32 const index               = draw_index(mesh::TextureSlot(b.slot));
            if (index == kInvalid) continue;
            char buf[256];
            TextureHandle const h     = texture_for(s, texture_asset_name(s.modelName, v, b, buf, sizeof buf),
                                                    mesh::TextureSlot(b.slot));
            s.materialSlots[m][index] = gpu_object(s.ctx, h).slot;
        }
    }
}

void handle_event(Scene& s, Event const& e) {
    bool const isModel = e.asset == AssetKind::Mesh && e.handle == s.model.bits();
    KILN_INFO("gl", "event %-9s %s v%u", ex::event_name(e.kind), isModel ? "model" : "texture", e.version);
    if (!isModel || e.kind == EventKind::Failed) return;
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    if (v && e.version != s.preparedVersion) {
        s.preparedVersion = e.version;
        request_textures(s, *v);
        prepare_geometry(s.geometry, *v, s.modelName);
    }
}

void bind_material(void* user, u32 material) {
    Scene const& s = *static_cast<Scene const*>(user);
    glUniform1uiv(5, GLsizei(kSlotsPerDraw), s.materialSlots[min(material, kMaxMaterials - 1)]);
}

bool scene_settled(Scene const& s) {
    if (!ex::settled(state(s.ctx, s.model))) return false;
    if (s.sky && !ex::settled(state(s.ctx, s.sky))) return false;
    for (u32 i = 0; i < s.textureCount; ++i)
        if (!ex::settled(state(s.ctx, s.textures[i].handle))) return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    GlOptions o;
    if (int const code = parse_options("kiln-gl-bindless", argc, argv, &o); code >= 0) return code;
    ex::install_stdout_log();

    // 1. A GL 4.6 core context with ARB_bindless_texture.
    GLFWwindow* const window = open_window(o, "kiln-gl-bindless");
    if (!window) return 2;
    if (!load_gl_bindless()) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 2;
    }
    ex::OrbitCamera camera;
    ex::attach_camera(window, &camera);

    // 2. The adapter in bindless mode: bind() maintains the handle table.
    Adapter adapter{};
    Result<GlAdapter*> gla =
        gl_adapter_create({.bindless = true, .tableFrames = FrameFences::kMax}, &adapter);
    if (gla.failed()) return 2;

    // 3. The context: create() waits for the placeholders (flush), so every slot shows one from
    //    the request on.
    ContextDesc cd{};
    cd.diag                  = ex::stdout_diag();
    cd.adapter               = &adapter;
    cd.storeDir              = StrView(o.store);
    cd.roots                 = Span<Root const>(o.roots, o.rootCount);
    cd.hotReload.watchStore  = o.watch;
    Result<Context*> created = create(cd);
    if (created.failed()) return 2;
    Context* const ctx = *created;
#if KILN_GL_HAS_COOK
    bool provider = false;
    if (o.rootCount) {
        cook::ProviderDesc pd{};
        pd.meshDefaults.profile = cook::VertexProfile::Float;
        pd.watchSources         = o.watch;
        provider                = cook::install_provider(ctx, pd).ok();
    }
#endif

    // 4. Requests. The sky's slot is fixed from here on, whatever happens to its texture.
    Scene s;
    s.ctx       = ctx;
    s.modelName = StrView(o.model);
    if (o.sky) {
        s.sky     = request_texture(ctx, StrView(o.sky), RequestOptions{.textureShape = TextureShape::Cube});
        s.skySlot = gpu_object(ctx, s.sky).slot;
    }
    s.model = request_mesh(ctx, s.modelName);

    GLuint const meshProgram = build_program(kMeshVs, kBindlessHeader, kMeshFs);
    GLuint const skyProgram  = build_program(kSkyVs, kBindlessHeader, kSkyFs);
    GLuint emptyVao          = 0;
    glCreateVertexArrays(1, &emptyVao);

    // 5. The frame loop: pump, then draw with the slots the materials stored.
    Target target;
    OffscreenRun run;
    FrameFences frames;
    int exitCode = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        frames.poll();
        (void)pump(ctx, {.frame = frames.next, .completedFrame = frames.completed});
        GlBufferRange const table = gl_handle_table(*gla, frames.next); // after pump(): this frame's binds
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, table.buffer, GLintptr(table.offset),
                          GLsizeiptr(table.size));
        for (Event const& e : events(ctx))
            handle_event(s, e);

        Frame f;
        if (!begin_frame(window, camera, target, &f)) continue;
        if (s.skySlot != kInvalid) {
            glUseProgram(skyProgram);
            glUniform3f(0, f.skyForward.x, f.skyForward.y, f.skyForward.z);
            glUniform3f(1, f.skyRight.x, f.skyRight.y, f.skyRight.z);
            glUniform3f(2, f.skyUp.x, f.skyUp.y, f.skyUp.z);
            glUniform1f(4, camera.exposure);
            glUniform1ui(5, s.skySlot);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glBindVertexArray(emptyVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDepthMask(GL_TRUE);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        mesh::MeshView const* v = mesh_view(ctx, s.model);
        GLuint const buffer = gl_buffer(*gla, gpu_object(ctx, s.model)); // meshes have no slot: 0 until Ready
        if (v && buffer) {
            glUseProgram(meshProgram);
            glUniformMatrix4fv(1, 1, GL_FALSE, f.viewProj.m);
            glUniform3f(2, f.view.eye.x, f.view.eye.y, f.view.eye.z);
            glUniform1f(4, camera.exposure);
            draw_geometry(s.geometry, *v, buffer, &bind_material, &s);
        }
        end_frame(window, target, o.offscreen);
        frames.end_frame();
        if (o.offscreen &&
            offscreen_done(o, target, scene_settled(s), state(ctx, s.model) == State::Failed, run, &exitCode))
            break;
    }

    // 6. Teardown: the GPU idle, kiln, then GL.
    glFinish();
    frames.release();
#if KILN_GL_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    release_geometry(s.geometry);
    glDeleteVertexArrays(1, &emptyVao);
    glDeleteProgram(meshProgram);
    glDeleteProgram(skyProgram);
    target.release();
    gl_adapter_destroy(*gla);
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
