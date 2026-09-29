// examples/gl/main.cpp — kiln-gl: one model and a cube sky through OpenGL 4.6 core, textures bound
// per draw (docs/design/integration-examples.md). The steps a host takes are numbered. Meshes are
// cooked with VertexProfile::Float, so the shaders read plain float attributes.
#include "example_app.h"
#include "gl_adapter.h"
#include "gl_api.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_GL_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include "cli.h"

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kiln;
using namespace kiln::glx;
using ex::Mat4;
using ex::Vec3;

namespace {

constexpr f32 kFovY        = 50.0f * ex::kPi / 180.0f;
constexpr u32 kMaxTextures = 32;
constexpr u32 kMaxLayouts  = 16;
constexpr u32 kMaxParts    = 256;
constexpr u32 kMaxRoots    = 8;

// Texture units: one per material slot the shader reads, then the sky.
constexpr GLuint kUnitBaseColor = 0, kUnitNormal = 1, kUnitMetalRough = 2, kUnitOcclusion = 3,
                 kUnitEmissive = 4, kUnitSky = 5;

// --- Shaders ---------------------------------------------------------------------------------------

constexpr char const* kMeshVs = R"(#version 460 core
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;
layout(location = 0) uniform mat4 uModel;
layout(location = 1) uniform mat4 uViewProj;
out vec3 vWorld;
out vec3 vNormal;
out vec4 vTangent;
out vec2 vUv;
void main() {
    vec4 world = uModel * vec4(inPos, 1.0);
    mat3 m     = mat3(uModel); // rotation and uniform scale only
    vWorld     = world.xyz;
    vNormal    = m * inNormal;
    vTangent   = vec4(m * inTangent.xyz, inTangent.w);
    vUv        = inUv;
    gl_Position = uViewProj * world;
}
)";

constexpr char const* kCommonFs = R"(
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 to_srgb(vec3 c) { return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
// Cube maps use a left-handed frame; this world is right-handed (the same flip as kiln-viewer).
vec3 cube_dir(vec3 d) { return vec3(d.x, d.y, -d.z); }
)";

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
out vec4 outColor;
void main() {
    vec4 base = (uTextures & 1u) != 0u ? texture(uBaseColor, vUv) : vec4(0.8);
    vec3 n    = normalize(vNormal);
    if ((uTextures & 2u) != 0u && dot(vTangent.xyz, vTangent.xyz) > 0.0) {
        vec3 t = normalize(vTangent.xyz - n * dot(n, vTangent.xyz));
        vec3 b = cross(n, t) * vTangent.w;
        n      = normalize(mat3(t, b, n) * (texture(uNormalMap, vUv).xyz * 2.0 - 1.0));
    }
    vec3 mr       = (uTextures & 4u) != 0u ? texture(uMetalRough, vUv).rgb : vec3(1.0, 0.7, 0.0); // G rough, B metal
    float ao      = (uTextures & 8u) != 0u ? texture(uOcclusion, vUv).r : 1.0;
    vec3 emissive = (uTextures & 16u) != 0u ? texture(uEmissive, vUv).rgb : vec3(0.0);
    float rough   = clamp(mr.g, 0.05, 1.0);
    float metal   = mr.b;

    vec3 v      = normalize(uEye - vWorld);
    vec3 l      = normalize(vec3(0.45, 0.8, 0.6));
    vec3 h      = normalize(l + v);
    vec3 f0     = mix(vec3(0.04), base.rgb, metal);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    float ndl   = max(dot(n, l), 0.0);
    float shine = mix(512.0, 4.0, rough);
    vec3 spec   = fresnel * pow(max(dot(n, h), 0.0), shine) * (shine + 8.0) / 25.13 * ndl;
    bool sky     = (uTextures & 32u) != 0u;
    vec3 ambient = sky ? textureLod(uSky, cube_dir(n), 6.0).rgb : vec3(0.3);
    vec3 refl    = sky ? textureLod(uSky, cube_dir(reflect(-v, n)), rough * 6.0).rgb : vec3(0.3);
    vec3 color   = (base.rgb * (1.0 - metal) * (ndl * 2.0 + ambient) + spec * 2.0 + refl * fresnel * (1.0 - rough)) * ao;
    outColor = vec4(to_srgb(aces((color + emissive) * exp2(uExposure))), 1.0);
}
)";

constexpr char const* kSkyVs = R"(#version 460 core
out vec2 vNdc;
void main() { // one triangle that covers the screen
    vNdc        = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
)";

constexpr char const* kSkyFs = R"(
in vec2 vNdc;
layout(binding = 5) uniform samplerCube uSky;
layout(location = 0) uniform vec3 uForward;
layout(location = 1) uniform vec3 uRight; // scaled by tan(fov / 2) * aspect
layout(location = 2) uniform vec3 uUp;    // scaled by tan(fov / 2)
layout(location = 4) uniform float uExposure;
out vec4 outColor;
void main() {
    vec3 dir = normalize(uForward + vNdc.x * uRight + vNdc.y * uUp);
    outColor = vec4(to_srgb(aces(texture(uSky, cube_dir(dir)).rgb * exp2(uExposure))), 1.0);
}
)";

// --- Options -----------------------------------------------------------------------------------------

struct Options {
    char const* store = "cooked";
    Root roots[kMaxRoots];
    u32 rootCount     = 0;
    char const* model = nullptr;
    char const* sky   = nullptr;
    char const* dump  = nullptr;
    double exposure   = 0;
    u32 width         = 1280;
    u32 height        = 720;
    u32 timeoutS      = 60;
    bool offscreen    = false;
    bool watch        = false;
};

bool set_model(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (o->model) {
        std::fprintf(stderr, "kiln-gl: one model only\n");
        return false;
    }
    o->model = arg;
    return true;
}

/// `--root [<name>=]<dir>` and `--source <dir>`, as in kiln-viewer.
bool add_root(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (o->rootCount == kMaxRoots) return false;
    char const* const eq = std::strchr(arg, '=');
    bool const named     = eq && !check_root_name(StrView(arg, usize(eq - arg)));
    o->roots[o->rootCount++] =
        named ? Root{StrView(arg, usize(eq - arg)), StrView(eq + 1)} : Root{{}, StrView(arg)};
    return true;
}

// --- GL helpers --------------------------------------------------------------------------------------

GLuint compile(GLenum stage, char const* const* parts, GLsizei count) {
    GLuint const s = glCreateShader(stage);
    glShaderSource(s, count, parts, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        KILN_ERROR("gl", "shader compile failed:\n%s", log);
    }
    return s;
}

GLuint program(char const* vs, char const* fs) {
    char const* const vsParts[] = {vs};
    char const* const fsParts[] = {"#version 460 core\n", kCommonFs, fs};
    GLuint const v              = compile(GL_VERTEX_SHADER, vsParts, 1);
    GLuint const f              = compile(GL_FRAGMENT_SHADER, fsParts, 3);
    GLuint const p              = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        KILN_ERROR("gl", "program link failed:\n%s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void KILN_GLAPI on_gl_debug(GLenum, GLenum, GLuint, GLenum severity, GLsizei, GLchar const* message,
                            void const*) {
    if (severity == GL_DEBUG_SEVERITY_HIGH) KILN_WARN("gl", "%s", message); // medium is performance advice
}

/// The render target: frames are drawn here, then blitted to the window (and read for --dump).
struct Target {
    GLuint fbo = 0, color = 0, depth = 0;
    u32 width = 0, height = 0;

    void resize(u32 w, u32 h) {
        if (w == width && h == height) return;
        release();
        width  = w;
        height = h;
        glCreateFramebuffers(1, &fbo);
        glCreateRenderbuffers(1, &color);
        glCreateRenderbuffers(1, &depth);
        glNamedRenderbufferStorage(color, GL_RGBA8, GLsizei(w), GLsizei(h));
        glNamedRenderbufferStorage(depth, GL_DEPTH_COMPONENT24, GLsizei(w), GLsizei(h));
        glNamedFramebufferRenderbuffer(fbo, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        glNamedFramebufferRenderbuffer(fbo, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            KILN_ERROR("gl", "render target %ux%u is incomplete", w, h);
    }
    void release() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (color) glDeleteRenderbuffers(1, &color);
        if (depth) glDeleteRenderbuffers(1, &depth);
        fbo = color = depth = 0;
        width = height = 0;
    }
};

// --- Scene -------------------------------------------------------------------------------------------

struct TextureItem {
    AssetId id = 0;
    TextureHandle handle;
};

struct Scene {
    Context* ctx   = nullptr;
    GlAdapter* gla = nullptr;
    StrView modelName;
    MeshHandle model;
    TextureHandle sky;
    TextureItem textures[kMaxTextures];
    u32 textureCount         = 0;
    GLuint vaos[kMaxLayouts] = {}; ///< one per vertex layout of the model
    u32 vaoCount             = 0;
    bool unsupported         = false; ///< the model's vertex data is not what this example reads
    Mat4 place;                       ///< model space -> world: centered, radius 1
    Mat4 world[kMaxParts];
};

/// The texture asset a binding names: an embedded image carries its name; an external URI is a
/// file next to the mesh (resolve_asset_name).
StrView texture_name(StrView meshName, mesh::MeshView const& v, mesh::TextureBinding const& b,
                     char (&buf)[256]) {
    StrView const path = v.str(b.pathStr);
    if (!(b.flags & mesh::kTextureExternal)) return path;
    return StrView(buf, resolve_asset_name(meshName, path, buf, sizeof buf));
}

TextureHandle find_texture_item(Scene const& s, AssetId id) {
    for (u32 i = 0; i < s.textureCount; ++i)
        if (s.textures[i].id == id) return s.textures[i].handle;
    return {};
}

/// Requests every texture the model's materials name. Each streams in on its own; until then
/// gpu() returns the placeholder of its kind.
void request_textures(Scene& s, mesh::MeshView const& v) {
    for (u32 i = 0; i < v.textures().size(); ++i) {
        mesh::TextureBinding const& b = v.textures()[i];
        char buf[256];
        StrView const name = texture_name(s.modelName, v, b, buf);
        AssetId const id   = asset_id(name);
        if (id == 0 || find_texture_item(s, id) || s.textureCount == kMaxTextures) continue;
        RequestOptions const opt{.textureKind = texture_kind_for_slot(mesh::TextureSlot(b.slot))};
        s.textures[s.textureCount++] = {id, request_texture(s.ctx, name, opt)};
    }
}

/// Attribute locations of the vertex shader, by semantic.
GLuint attrib_location(mesh::VertexAttrib const& a) {
    if (a.semanticIndex != 0) return kInvalid;
    switch (mesh::Semantic(a.semantic)) {
    case mesh::Semantic::Position: return 0;
    case mesh::Semantic::Normal: return 1;
    case mesh::Semantic::Tangent: return 2;
    case mesh::Semantic::TexCoord: return 3;
    default: return kInvalid;
    }
}

/// One vertex array per layout: formats and stream bindings; buffers are attached per draw.
void build_vertex_arrays(Scene& s, mesh::MeshView const& v) {
    if (s.vaoCount) glDeleteVertexArrays(GLsizei(s.vaoCount), s.vaos);
    s.vaoCount    = min<u32>(v.layouts().size(), kMaxLayouts);
    s.unsupported = false;
    glCreateVertexArrays(GLsizei(s.vaoCount), s.vaos);
    for (u32 li = 0; li < s.vaoCount; ++li) {
        mesh::VertexLayout const& l = v.layouts()[li];
        for (u32 i = 0; i < l.attribCount; ++i) {
            mesh::VertexAttrib const& a = l.attribs[i];
            GLuint const loc            = attrib_location(a);
            if (loc == kInvalid) continue;
            GlVertexFormat const f = gl_vertex_format(Format(a.format));
            // A 2-component normal is octahedral (the quantized profile); this shader reads xyz.
            bool const octNormal = mesh::Semantic(a.semantic) == mesh::Semantic::Normal && f.size == 2;
            if (f.size == 0 || octNormal ||
                (mesh::Semantic(a.semantic) == mesh::Semantic::Position && f.type != GL_FLOAT)) {
                KILN_ERROR("gl", "%.*s: vertex data is not float; cook it with profile = \"float\"",
                           KILN_SV(s.modelName));
                s.unsupported = true;
                return;
            }
            glVertexArrayAttribFormat(s.vaos[li], loc, f.size, f.type, f.normalized ? GL_TRUE : GL_FALSE,
                                      a.offset);
            glVertexArrayAttribBinding(s.vaos[li], loc, a.stream);
            glEnableVertexArrayAttrib(s.vaos[li], loc);
        }
    }
}

/// Scales and centers the model to a sphere of radius 1 at the origin.
void place_model(Scene& s, mesh::MeshView const& v) {
    mesh::Bounds const& b = v.model().bounds;
    f32 const scale       = b.radius > 0 ? 1.0f / b.radius : 1.0f;
    s.place = ex::scaling(scale) * ex::translation(Vec3{-b.center[0], -b.center[1], -b.center[2]});
}

void handle_event(Scene& s, Event const& e) {
    bool const isModel = e.asset == AssetKind::Mesh && e.handle == s.model.bits();
    KILN_INFO("gl", "event %-9s %s v%u", ex::event_name(e.kind), isModel ? "model" : "texture", e.version);
    if (!isModel) return;
    if (e.kind == EventKind::Failed) return;
    // MetaReady: the metadata is readable before the payload arrives, so the textures can be
    // requested and the vertex arrays built early. Changed: a hot reload, the same again.
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    if (!v) return;
    if (e.kind == EventKind::MetaReady || e.kind == EventKind::Changed) {
        request_textures(s, *v);
        build_vertex_arrays(s, *v);
        place_model(s, *v);
    }
}

/// Binds the material's textures to their units; returns the bits the shader tests.
u32 bind_material(Scene const& s, mesh::MeshView const& v, u32 material) {
    u32 mask = 0;
    if (material >= v.materials().size()) return mask;
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
        TextureHandle const h = find_texture_item(s, asset_id(texture_name(s.modelName, v, b, buf)));
        // The placeholder while the texture loads, the real one once Ready: no host bookkeeping.
        GlTexture const tex = gl_texture(s.gla, gpu(s.ctx, h));
        if (!tex.name) continue;
        glBindTextureUnit(unit, tex.name);
        mask |= 1u << unit;
    }
    return mask;
}

void draw_model(Scene& s, Mat4 const& viewProj, Vec3 eye, f32 exposure, u32 skyBit) {
    mesh::MeshView const* v = mesh_view(s.ctx, s.model);
    GLuint const buffer     = gl_buffer(s.gla, gpu(s.ctx, s.model)); // 0 until Ready
    if (!v || !buffer || s.unsupported) return;
    glUniformMatrix4fv(1, 1, GL_FALSE, viewProj.m);
    glUniform3f(2, eye.x, eye.y, eye.z);
    glUniform1f(4, exposure);
    u32 const partCount = min<u32>(v->parts().size(), kMaxParts);
    for (u32 p = 0; p < partCount; ++p) {
        mesh::MeshPart const& part = v->parts()[p];
        Mat4 const local           = ex::from_rt(part.translation, part.rotation);
        s.world[p] = part.parent < p ? s.world[part.parent] * local : s.place * local; // parents come first
        if (part.lodCount == 0) continue;
        mesh::MeshLod const& lod = v->lods()[part.lodFirst];
        if (lod.layout >= s.vaoCount) continue;
        mesh::VertexLayout const& layout = v->layouts()[lod.layout];
        GLuint const vao                 = s.vaos[lod.layout];
        for (u32 st = 0; st < layout.streamCount; ++st)
            glVertexArrayVertexBuffer(vao, st, buffer, GLintptr(lod.streamOffset[st]), layout.strides[st]);
        glVertexArrayElementBuffer(vao, buffer);
        glBindVertexArray(vao);
        glUniformMatrix4fv(0, 1, GL_FALSE, s.world[p].m);
        auto const indexType = mesh::IndexType(lod.indexType);
        GLenum const glIndex = indexType == mesh::IndexType::U32   ? GL_UNSIGNED_INT
                               : indexType == mesh::IndexType::U16 ? GL_UNSIGNED_SHORT
                                                                   : GL_UNSIGNED_BYTE;
        u64 const indexSize  = mesh::index_size(indexType);
        for (u32 si = 0; si < lod.submeshCount; ++si) {
            mesh::Submesh const& sm = v->submeshes()[lod.submeshFirst + si];
            glUniform1ui(3, bind_material(s, *v, sm.material) | skyBit);
            usize const first = usize(lod.indexOffset + u64(sm.indexFirst) * indexSize);
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(sm.indexCount), glIndex,
                                     reinterpret_cast<void const*>(first), sm.vertexBase);
        }
    }
}

bool scene_settled(Scene const& s) {
    if (!ex::settled(state(s.ctx, s.model))) return false;
    if (s.sky && !ex::settled(state(s.ctx, s.sky))) return false;
    for (u32 i = 0; i < s.textureCount; ++i)
        if (!ex::settled(state(s.ctx, s.textures[i].handle))) return false;
    return true;
}

bool dump_png(Target const& t, char const* path) {
    Vec<u8> rgba(default_allocator(), Tag::Io);
    rgba.resize(usize(t.width) * t.height * 4);
    glNamedFramebufferReadBuffer(t.fbo, GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, GLsizei(t.width), GLsizei(t.height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    usize const row = usize(t.width) * 4; // GL rows run bottom-up; PNG rows top-down
    for (u32 y = 0; y < t.height / 2; ++y)
        for (usize x = 0; x < row; ++x) {
            u8 const tmp                       = rgba[y * row + x];
            rgba[y * row + x]                  = rgba[(t.height - 1 - y) * row + x];
            rgba[(t.height - 1 - y) * row + x] = tmp;
        }
    return ex::write_png(path, rgba.span(), t.width, t.height);
}

void glfw_error(int code, char const* message) { KILN_ERROR("glfw", "%d: %s", code, message); }

} // namespace

int main(int argc, char** argv) {
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
         .help = "a source root; <name>=<dir> names <name>:<path> (repeatable)",
         .each = &add_root,
         .user = &o},
        {.name = "--sky", .arg = "<name>", .help = "a cube texture behind the model", .str = &o.sky},
        {.name = "--exposure",
         .arg  = "<ev>",
         .help = "scale colors by 2^<ev> (default: 0)",
         .real = &o.exposure},
        {.name = "--width", .arg = "<px>", .help = "default: 1280", .number = &o.width, .max = 16384},
        {.name = "--height", .arg = "<px>", .help = "default: 720", .number = &o.height, .max = 16384},
        {.name = "--watch", .help = "hot reload store files (and sources with --source)", .flag = &o.watch},
        {.name = "--offscreen", .help = "hidden window; stop when the scene settles", .flag = &o.offscreen},
        {.name = "--dump", .arg = "<file.png>", .help = "offscreen: write the settled frame", .str = &o.dump},
        {.name   = "--timeout",
         .arg    = "<s>",
         .help   = "offscreen: give up after <s> seconds (default: 60)",
         .number = &o.timeoutS},
    };
    cli::Spec const spec{
        .program  = "kiln-gl",
        .synopsis = "[options] <model>",
        .options  = {opts, countof(opts)},
        .footer =
            "<model> is an asset name, e.g. WaterBottle.glb. Left-drag orbits, wheel zooms, Esc quits.\n"
            "Exit codes: 0 ok, 1 an asset Failed or --timeout expired, 2 usage or setup error.",
        .positional = &set_model,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || !o.model || o.width == 0 || o.height == 0 || (o.dump && !o.offscreen)) {
        cli::usage(spec, stderr);
        return 2;
    }
    ex::install_stdout_log();

    // 1. A GL 4.6 core context. Offscreen keeps the window hidden.
    glfwSetErrorCallback(&glfw_error);
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, KILN_DEBUG ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, o.offscreen ? GLFW_FALSE : GLFW_TRUE);
    GLFWwindow* const window = glfwCreateWindow(int(o.width), int(o.height), "kiln-gl", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(o.offscreen ? 0 : 1);
    if (!load_gl()) {
        glfwTerminate();
        return 2;
    }
    KILN_INFO("gl", "%s, %s", reinterpret_cast<char const*>(glGetString(GL_VERSION)),
              reinterpret_cast<char const*>(glGetString(GL_RENDERER)));
    if (KILN_DEBUG) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(&on_gl_debug, nullptr);
    }
    ex::OrbitCamera camera;
    ex::attach_camera(window, &camera);

    // 2. The adapter: kiln's view of this renderer.
    Adapter adapter{};
    Result<GlAdapter*> gla = gl_adapter_create({}, &adapter);
    if (gla.failed()) return 2;

    // 3. The context, and in dev builds the cook provider, which cooks meshes as plain floats.
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

    // 4. Requests. Nothing blocks: the model and its textures arrive over the next frames.
    Scene s;
    s.ctx       = ctx;
    s.gla       = *gla;
    s.modelName = StrView(o.model);
    s.model     = request_mesh(ctx, s.modelName);
    if (o.sky)
        s.sky = request_texture(ctx, StrView(o.sky), RequestOptions{.textureShape = TextureShape::Cube});

    GLuint const meshProgram = program(kMeshVs, kMeshFs);
    GLuint const skyProgram  = program(kSkyVs, kSkyFs);
    GLuint emptyVao          = 0; // the sky triangle has no vertex data
    glCreateVertexArrays(1, &emptyVao);
    GLuint samplers[2] = {};
    glCreateSamplers(2, samplers);
    for (GLuint sm : samplers) {
        glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, GLint(GL_LINEAR_MIPMAP_LINEAR));
        glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, GLint(GL_LINEAR));
        glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
    }
    glSamplerParameteri(samplers[1], GL_TEXTURE_WRAP_S, GLint(GL_CLAMP_TO_EDGE));
    glSamplerParameteri(samplers[1], GL_TEXTURE_WRAP_T, GLint(GL_CLAMP_TO_EDGE));
    glSamplerParameteri(samplers[1], GL_TEXTURE_WRAP_R, GLint(GL_CLAMP_TO_EDGE));
    for (GLuint unit = kUnitBaseColor; unit <= kUnitEmissive; ++unit)
        glBindSampler(unit, samplers[0]);
    glBindSampler(kUnitSky, samplers[1]);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glVertexAttrib4f(2, 0, 0, 0, 0); // no tangent: the shader skips the normal map
    glVertexAttrib4f(3, 0, 0, 0, 0);

    // 5. The frame loop.
    Target target;
    int exitCode         = 0;
    bool settledOnce     = false;
    double const startMs = ex::ms_since_start();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // 5a. kiln runs the adapter's flush (the GL work its workers queued), hands out upload
        //     memory, polls completions, publishes and emits events. This is the GL thread.
        (void)pump(ctx);
        for (Event const& e : events(ctx))
            handle_event(s, e);

        int fw = 0, fh = 0;
        glfwGetFramebufferSize(window, &fw, &fh);
        if (fw <= 0 || fh <= 0) continue;
        target.resize(u32(fw), u32(fh));
        f32 const aspect    = f32(fw) / f32(fh);
        ex::View const vw   = ex::orbit_view(camera, Vec3{}, 1.0f, kFovY, aspect);
        Mat4 const viewProj = ex::perspective_gl(kFovY, aspect, vw.nearZ, vw.farZ) * vw.view;

        // 5b. Draw: the sky, then the model; every texture is whatever gpu() returns now.
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, fw, fh);
        glClearColor(0.15f, 0.16f, 0.19f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        GlTexture const sky = s.sky ? gl_texture(s.gla, gpu(ctx, s.sky)) : GlTexture{};
        u32 skyBit          = 0;
        if (sky.name && sky.target == GL_TEXTURE_CUBE_MAP) {
            glBindTextureUnit(kUnitSky, sky.name);
            skyBit           = 1u << kUnitSky;
            Vec3 const f     = ex::normalize(Vec3{} - vw.eye);
            Vec3 const side  = ex::normalize(ex::cross(f, Vec3{0, 1, 0}));
            Vec3 const up    = ex::cross(side, f);
            f32 const tanV   = std::tan(kFovY * 0.5f);
            Vec3 const right = side * (tanV * aspect);
            Vec3 const upS   = up * tanV;
            glUseProgram(skyProgram);
            glUniform3f(0, f.x, f.y, f.z);
            glUniform3f(1, right.x, right.y, right.z);
            glUniform3f(2, upS.x, upS.y, upS.z);
            glUniform1f(4, f32(o.exposure));
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glBindVertexArray(emptyVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDepthMask(GL_TRUE);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glUseProgram(meshProgram);
        draw_model(s, viewProj, vw.eye, f32(o.exposure), skyBit);
        glBlitNamedFramebuffer(target.fbo, 0, 0, 0, fw, fh, 0, 0, fw, fh, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        if (!o.offscreen) {
            glfwSwapBuffers(window);
            continue;
        }
        // Offscreen: one more frame after everything settled, so the last uploads are drawn.
        if (settledOnce) {
            if (o.dump && !dump_png(target, o.dump)) {
                KILN_ERROR("gl", "cannot write %s", o.dump);
                exitCode = 2;
            } else if (o.dump) {
                KILN_INFO("gl", "wrote %s (%ux%u)", o.dump, target.width, target.height);
            }
            if (state(ctx, s.model) == State::Failed) exitCode = max(exitCode, 1);
            break;
        }
        settledOnce = scene_settled(s);
        if (ex::ms_since_start() - startMs > o.timeoutS * 1000.0) {
            KILN_ERROR("gl", "the scene did not settle within %u s", o.timeoutS);
            exitCode = 1;
            break;
        }
    }

    // 6. Teardown: kiln first (it hands every GPU object back through destroy_deferred), then GL.
#if KILN_GL_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    if (s.vaoCount) glDeleteVertexArrays(GLsizei(s.vaoCount), s.vaos);
    glDeleteVertexArrays(1, &emptyVao);
    glDeleteSamplers(2, samplers);
    glDeleteProgram(meshProgram);
    glDeleteProgram(skyProgram);
    target.release();
    gl_adapter_destroy(s.gla);
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
