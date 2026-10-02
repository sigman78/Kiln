// examples/gl/main.cpp — kiln-gl: one model and a cube sky through OpenGL 4.6 core, textures bound
// to units per draw (docs/design/integration-examples.md). The steps a host takes are numbered.
// Meshes are cooked with VertexProfile::Float, so the shaders read plain float attributes.
#include "gl_adapter.h"
#include "gl_background.h"
#include "gl_util.h"
#include "no_crash_dialogs.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_GL_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include <GLFW/glfw3.h>

using namespace kiln;
using namespace kiln::glx;

namespace {

constexpr u32 kMaxTextures = 32;

// Texture units: one per material slot the shader reads, then the sky.
constexpr GLuint kUnitBaseColor = 0, kUnitNormal = 1, kUnitMetalRough = 2, kUnitOcclusion = 3,
                 kUnitEmissive = 4, kUnitSky = kEnvironmentUnit;

constexpr char const* kMeshFs = R"(
in vec3 vWorld;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUv;
layout(binding = 0) uniform sampler2D uBaseColor;
layout(binding = 1) uniform sampler2D uNormalMap;
layout(binding = 2) uniform sampler2D uMetalRough;
layout(binding = 3) uniform sampler2D uOcclusion;
layout(binding = 4) uniform sampler2D uEmissive;
layout(binding = 5) uniform samplerCube uSky;
layout(location = 2) uniform vec3 uEye;
layout(location = 3) uniform uint uTextures; // bit per unit above that holds a texture
layout(location = 4) uniform float uExposure;
layout(location = 5) uniform vec4 uBaseColorFactor;
layout(location = 6) uniform vec4 uEmissiveNormal; // xyz: emissive factor; w: normal scale
layout(location = 7) uniform vec4 uMro;            // metallic, roughness, occlusion strength
out vec4 outColor;
bool has(uint unit) { return (uTextures & (1u << unit)) != 0u; }
void main() {
    vec3 base     = uBaseColorFactor.rgb * (has(0u) ? texture(uBaseColor, vUv).rgb : vec3(1.0));
    vec3 n        = normalize(vNormal);
    if (has(1u)) n = perturb(n, vTangent, texture(uNormalMap, vUv).xy, uEmissiveNormal.w);
    vec3 mr       = has(2u) ? texture(uMetalRough, vUv).rgb : vec3(1.0); // G rough, B metal
    float ao      = has(3u) ? 1.0 + uMro.z * (texture(uOcclusion, vUv).r - 1.0) : 1.0;
    vec3 emissive = uEmissiveNormal.rgb * (has(4u) ? texture(uEmissive, vUv).rgb : vec3(1.0));
    float rough   = clamp(uMro.y * mr.g, 0.05, 1.0);
    mr.b *= uMro.x;
    vec3 v        = normalize(uEye - vWorld);
    vec3 ambient  = has(5u) ? textureLod(uSky, cube_dir(n), 6.0).rgb : vec3(0.3);
    vec3 env      = has(5u) ? textureLod(uSky, cube_dir(reflect(-v, n)), rough * 6.0).rgb : vec3(0.3);
    outColor      = display(shade(base, n, v, rough, mr.b, ao, ambient, env) + emissive, uExposure);
}
)";

struct TextureItem {
    AssetId id = 0;
    TextureHandle handle;
};

struct Scene {
    /// The model content version the materials and geometry were set up for. MetaReady, Changed
    /// and a Ready that follows Failed (a repaired model: reloads emit no MetaReady) carry a new one.
    u32 preparedVersion = 0;
    Context* ctx        = nullptr;
    GlAdapter* gla      = nullptr;
    StrView modelName;
    MeshHandle model;
    TextureHandle environment;
    TextureItem textures[kMaxTextures];
    u32 textureCount = 0;
    Geometry geometry;
    u32 environmentBit = 0; ///< this frame: the sky is bound to kUnitSky
};

TextureHandle find_item(Scene const& s, AssetId id) {
    for (u32 i = 0; i < s.textureCount; ++i)
        if (s.textures[i].id == id) return s.textures[i].handle;
    return {};
}

/// Requests every texture the model's materials name. Each streams in on its own; until then
/// gpu_object() returns the placeholder of its kind.
void request_textures(Scene& s, mesh::MeshView const& v) {
    for (u32 i = 0; i < v.textures().size(); ++i) {
        mesh::TextureBinding const& b = v.textures()[i];
        char buf[256];
        StrView const name = texture_asset_name(s.modelName, v, b, buf, sizeof buf);
        AssetId const id   = asset_id(name);
        if (id == 0 || find_item(s, id) || s.textureCount == kMaxTextures) continue;
        RequestOptions const opt{.textureKind = texture_kind_for_slot(mesh::TextureSlot(b.slot))};
        s.textures[s.textureCount++] = {id, request_texture(s.ctx, name, opt)};
    }
}

void handle_event(Scene& s, Event const& e) {
    bool const isModel = e.asset == AssetKind::Mesh && e.handle == s.model.bits();
    KILN_INFO("gl", "event %-9s %s v%u", ex::event_name(e.kind), isModel ? "model" : "texture", e.version);
    if (!isModel || e.kind == EventKind::Failed) return;
    // MetaReady: the metadata is readable before the payload arrives, so the textures can be
    // requested and the vertex arrays built early. Changed (a hot reload) and a Ready after Failed
    // (a repaired model) carry a new version: the same again.
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    if (v && e.version != s.preparedVersion) {
        s.preparedVersion = e.version;
        request_textures(s, *v);
        prepare_geometry(s.geometry, *v, s.modelName);
    }
}

/// Binds the material's textures to their units (the placeholder while one loads, the real one
/// once Ready: whatever gpu_object() returns now) and tells the shader which units hold one.
void bind_material(void* user, u32 material) {
    Scene const& s          = *static_cast<Scene const*>(user);
    mesh::MeshView const& v = *mesh_view(s.ctx, s.model);
    u32 mask                = s.environmentBit;
    if (material < v.materials().size()) {
        mesh::MaterialSlot const& m = v.materials()[material];
        for (u32 t = 0; t < m.textureCount && m.textureFirst + t < v.textures().size(); ++t) {
            mesh::TextureBinding const& b = v.textures()[m.textureFirst + t];
            GLuint unit                   = kInvalid;
            switch (mesh::TextureSlot(b.slot)) {
            case mesh::TextureSlot::BaseColor: unit = kUnitBaseColor; break;
            case mesh::TextureSlot::Normal: unit = kUnitNormal; break;
            case mesh::TextureSlot::MetalRough: unit = kUnitMetalRough; break;
            case mesh::TextureSlot::Occlusion: unit = kUnitOcclusion; break;
            case mesh::TextureSlot::Emissive: unit = kUnitEmissive; break;
            default: continue;
            }
            char buf[256];
            TextureHandle const h =
                find_item(s, asset_id(texture_asset_name(s.modelName, v, b, buf, sizeof buf)));
            GlTexture const tex = gl_texture(s.gla, gpu_object(s.ctx, h));
            if (!tex.name) continue;
            glBindTextureUnit(unit, tex.name);
            mask |= 1u << unit;
        }
    }
    glUniform1ui(3, mask);
    ex::MaterialFactors const f = ex::material_factors(v, material);
    glUniform4fv(5, 1, f.baseColor);
    glUniform4fv(6, 1, f.emissiveNormal);
    glUniform4fv(7, 1, f.mro);
}

bool scene_settled(Scene const& s) {
    if (!ex::settled(state(s.ctx, s.model))) return false;
    if (s.environment && !ex::settled(state(s.ctx, s.environment))) return false;
    for (u32 i = 0; i < s.textureCount; ++i)
        if (!ex::settled(state(s.ctx, s.textures[i].handle))) return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    GlOptions o;
    if (int const code = parse_options("kiln-gl", argc, argv, &o); code >= 0) return code;
    ex::install_stdout_log();

    // 1. A GL 4.6 core context (gl_util.cpp). kiln calls the adapter's flush from pump() on this
    //    thread, so create() and pump() run here too.
    GLFWwindow* const window = open_window(o, "kiln-gl");
    if (!window) return 2;
    ex::OrbitCamera camera;
    ex::attach_camera(window, &camera);

    // 2. The adapter: kiln's view of this renderer.
    Adapter adapter{};
    Result<GlAdapter*> gla = gl_adapter_create({}, &adapter);
    if (gla.failed()) return 2;

    // 3. The context, and in dev builds the cook provider, which cooks meshes as plain floats.
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
#if KILN_GL_HAS_COOK
    bool provider = false;
    if (o.rootCount) {
        cook::ProviderDesc pd{};
        pd.meshDefaults.profile = cook::VertexProfile::Float;
        pd.watchSources         = o.watch;
        provider                = cook::install_provider(ctx, pd).ok();
    }
#endif

    // 4. Requests. Nothing blocks: the model and its textures arrive over the next frames.
    Scene s;
    s.ctx       = ctx;
    s.gla       = *gla;
    s.modelName = StrView(o.model);
    s.model     = request_mesh(ctx, s.modelName);
    if (o.sky)
        s.environment =
            request_texture(ctx, StrView(o.sky), RequestOptions{.textureShape = TextureShape::Cube});

    GLuint const meshProgram = build_program(kMeshVs, "", kMeshFs);
    Background background;
    background.create(false);
    GLuint samplers[2] = {}; // [0] materials: repeat; [1] the environment: clamp
    glCreateSamplers(2, samplers);
    for (GLuint sm : samplers) {
        glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, GLint(GL_LINEAR_MIPMAP_LINEAR));
        glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, GLint(GL_LINEAR));
        glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
    }
    for (GLenum wrap : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
        glSamplerParameteri(samplers[1], wrap, GLint(GL_CLAMP_TO_EDGE));
    for (GLuint unit = kUnitBaseColor; unit <= kUnitEmissive; ++unit)
        glBindSampler(unit, samplers[0]);
    glBindSampler(kUnitSky, samplers[1]);

    // 5. The frame loop.
    Target target;
    OffscreenRun run;
    int exitCode = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // 5a. kiln runs the adapter's flush (the GL work its workers queued), hands out upload
        //     memory, polls completions and emits events.
        (void)pump(ctx);
        for (Event const& e : events(ctx))
            handle_event(s, e);

        // 5b. Draw: the sky, then the model; every texture is whatever gpu_object() returns now.
        Frame f;
        if (!begin_frame(window, camera, target, &f)) continue;
        GlTexture const sky = s.environment ? gl_texture(s.gla, gpu_object(ctx, s.environment)) : GlTexture{};
        s.environmentBit    = sky.target == GL_TEXTURE_CUBE_MAP ? 1u << kUnitSky : 0u;
        if (s.environmentBit) {
            glBindTextureUnit(kUnitSky, sky.name); // the background and mesh lighting both sample it
            background.draw(f.rays, camera.exposure);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        mesh::MeshView const* v = mesh_view(ctx, s.model);
        GLuint const buffer     = gl_buffer(s.gla, gpu_object(ctx, s.model)); // 0 until Ready
        if (v && buffer) {
            glUseProgram(meshProgram);
            glUniformMatrix4fv(1, 1, GL_FALSE, f.viewProj.m);
            glUniform3f(2, f.view.eye.x, f.view.eye.y, f.view.eye.z);
            glUniform1f(4, camera.exposure);
            draw_geometry(s.geometry, *v, buffer, &bind_material, &s);
        }
        end_frame(window, target, o.offscreen);
        if (o.offscreen &&
            offscreen_done(o, target, scene_settled(s), state(ctx, s.model) == State::Failed, run, &exitCode))
            break;
    }

    ex::log_adapter_stats("gl", gl_adapter_stats(*gla));

    // 6. Teardown: kiln first (it hands every GPU object back through Adapter::destroy), then GL.
#if KILN_GL_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    ex::finish_trace();
    release_geometry(s.geometry);
    background.release();
    glDeleteSamplers(2, samplers);
    glDeleteProgram(meshProgram);
    target.release();
    gl_adapter_destroy(s.gla);
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
