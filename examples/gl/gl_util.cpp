// examples/gl/gl_util.cpp — the code kiln-gl and kiln-gl-bindless share.
#include "gl_util.h"

#include <kiln/containers.h>
#include <kiln/log.h>

#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace kiln::glx {

char const* const kMeshVs = R"(#version 460 core
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
    vec4 world  = uModel * vec4(inPos, 1.0);
    mat3 m      = mat3(uModel); // rotation and uniform scale only
    vWorld      = world.xyz;
    vNormal     = m * inNormal;
    vTangent    = vec4(m * inTangent.xyz, inTangent.w);
    vUv         = inUv;
    gl_Position = uViewProj * world;
}
)";

char const* const kSkyVs = R"(#version 460 core
out vec2 vNdc;
void main() {
    vNdc        = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
)";

char const* const kCommonFs = R"(
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 to_srgb(vec3 c) { return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
vec4 display(vec3 color, float exposure) { return vec4(to_srgb(aces(color * exp2(exposure))), 1.0); }
// Cube maps use a left-handed frame; this world is right-handed (docs/design/texture-shapes.md).
vec3 cube_dir(vec3 d) { return vec3(d.x, d.y, -d.z); }
// A tangent-space normal map texel applied to n; without a tangent n stays.
vec3 perturb(vec3 n, vec4 tangent, vec3 texel) {
    if (dot(tangent.xyz, tangent.xyz) == 0.0) return n;
    vec3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    vec3 b = cross(n, t) * tangent.w;
    return normalize(mat3(t, b, n) * (texel * 2.0 - 1.0));
}
// A sun plus the sky: `ambient` is the sky around n, `env` the sky in the reflected direction.
vec3 shade(vec3 base, vec3 n, vec3 v, float rough, float metal, float ao, vec3 ambient, vec3 env) {
    vec3 l       = normalize(vec3(0.45, 0.8, 0.6));
    vec3 h       = normalize(l + v);
    vec3 f0      = mix(vec3(0.04), base, metal);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    float ndl    = max(dot(n, l), 0.0);
    float shine  = mix(512.0, 4.0, rough);
    vec3 spec    = fresnel * pow(max(dot(n, h), 0.0), shine) * (shine + 8.0) / 25.13 * ndl;
    return (base * (1.0 - metal) * (ndl * 2.0 + ambient) + spec * 2.0 + env * fresnel * (1.0 - rough)) * ao;
}
)";

namespace {

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

void KILN_GLAPI on_gl_debug(GLenum, GLenum, GLuint, GLenum severity, GLsizei, GLchar const* message,
                            void const*) {
    if (severity == GL_DEBUG_SEVERITY_HIGH) KILN_WARN("gl", "%s", message); // medium is performance advice
}

void glfw_error(int code, char const* message) { KILN_ERROR("glfw", "%d: %s", code, message); }

/// Shader location of an attribute by semantic; kInvalid for what kMeshVs does not read.
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

/// The GL vertex format of a float-profile attribute; size 0 for anything else.
struct AttribFormat {
    GLint size  = 0;
    GLenum type = 0;
    bool norm   = false;
};
AttribFormat attrib_format(Format f) {
    switch (f) {
    case Format::R32G32_SFLOAT: return {2, GL_FLOAT, false};
    case Format::R32G32B32_SFLOAT: return {3, GL_FLOAT, false};
    case Format::R32G32B32A32_SFLOAT: return {4, GL_FLOAT, false};
    case Format::R8G8B8A8_UNORM: return {4, GL_UNSIGNED_BYTE, true};
    default: return {};
    }
}

} // namespace

GLFWwindow* open_window(GlOptions const& o, char const* title) noexcept {
    glfwSetErrorCallback(&glfw_error);
    if (!glfwInit()) return nullptr;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, KILN_DEBUG ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, o.offscreen ? GLFW_FALSE : GLFW_TRUE);
    GLFWwindow* const w = glfwCreateWindow(int(o.width), int(o.height), title, nullptr, nullptr);
    if (!w) {
        glfwTerminate();
        return nullptr;
    }
    glfwMakeContextCurrent(w);
    glfwSwapInterval(o.offscreen ? 0 : 1);
    if (!load_gl()) {
        glfwDestroyWindow(w);
        glfwTerminate();
        return nullptr;
    }
    KILN_INFO("gl", "%s, %s", reinterpret_cast<char const*>(glGetString(GL_VERSION)),
              reinterpret_cast<char const*>(glGetString(GL_RENDERER)));
    if (KILN_DEBUG) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(&on_gl_debug, nullptr);
    }
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glVertexAttrib4f(2, 0, 0, 0, 0); // no tangent: perturb() keeps the normal
    glVertexAttrib4f(3, 0, 0, 0, 0);
    return w;
}

GLuint build_program(char const* vs, char const* fsHeader, char const* fs) noexcept {
    char const* const vsParts[] = {vs};
    char const* const fsParts[] = {"#version 460 core\n", fsHeader, kCommonFs, fs};
    GLuint const v              = compile(GL_VERTEX_SHADER, vsParts, 1);
    GLuint const f              = compile(GL_FRAGMENT_SHADER, fsParts, 4);
    GLuint const p              = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok) return p;
    char log[2048];
    glGetProgramInfoLog(p, sizeof log, nullptr, log);
    KILN_ERROR("gl", "program link failed:\n%s", log);
    glDeleteProgram(p);
    return 0;
}

void Target::resize(u32 w, u32 h) noexcept {
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

void Target::release() noexcept {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (color) glDeleteRenderbuffers(1, &color);
    if (depth) glDeleteRenderbuffers(1, &depth);
    fbo = color = depth = 0;
    width = height = 0;
}

void prepare_geometry(Geometry& g, mesh::MeshView const& v, StrView name) noexcept {
    release_geometry(g);
    g.vaoCount = min<u32>(v.layouts().size(), kMaxLayouts);
    glCreateVertexArrays(GLsizei(g.vaoCount), g.vaos);
    for (u32 li = 0; li < g.vaoCount; ++li) {
        mesh::VertexLayout const& l = v.layouts()[li];
        for (u32 i = 0; i < l.attribCount; ++i) {
            mesh::VertexAttrib const& a = l.attribs[i];
            GLuint const loc            = attrib_location(a);
            if (loc == kInvalid) continue;
            AttribFormat const f = attrib_format(Format(a.format));
            if (f.size == 0 || (mesh::Semantic(a.semantic) == mesh::Semantic::Normal && f.size != 3)) {
                KILN_ERROR("gl", "%.*s: vertex data is not float; cook it with profile = \"float\"",
                           KILN_SV(name));
                g.unsupported = true;
                return;
            }
            glVertexArrayAttribFormat(g.vaos[li], loc, f.size, f.type, f.norm ? GL_TRUE : GL_FALSE, a.offset);
            glVertexArrayAttribBinding(g.vaos[li], loc, a.stream);
            glEnableVertexArrayAttrib(g.vaos[li], loc);
        }
    }
    mesh::Bounds const& b = v.model().bounds;
    f32 const scale       = b.radius > 0 ? 1.0f / b.radius : 1.0f;
    g.place = ex::scaling(scale) * ex::translation(ex::Vec3{-b.center[0], -b.center[1], -b.center[2]});
}

void release_geometry(Geometry& g) noexcept {
    if (g.vaoCount) glDeleteVertexArrays(GLsizei(g.vaoCount), g.vaos);
    g.vaoCount    = 0;
    g.unsupported = false;
}

void draw_geometry(Geometry& g, mesh::MeshView const& v, GLuint buffer,
                   void (*material)(void* user, u32 index), void* user) noexcept {
    if (g.unsupported) return;
    u32 const partCount = min<u32>(v.parts().size(), kMaxParts);
    for (u32 p = 0; p < partCount; ++p) {
        mesh::MeshPart const& part = v.parts()[p];
        ex::Mat4 const local       = ex::from_rt(part.translation, part.rotation);
        g.world[p] = part.parent < p ? g.world[part.parent] * local : g.place * local; // parents come first
        if (part.lodCount == 0) continue;
        mesh::MeshLod const& lod = v.lods()[part.lodFirst];
        if (lod.layout >= g.vaoCount) continue;
        mesh::VertexLayout const& layout = v.layouts()[lod.layout];
        GLuint const vao                 = g.vaos[lod.layout];
        for (u32 st = 0; st < layout.streamCount; ++st)
            glVertexArrayVertexBuffer(vao, st, buffer, GLintptr(lod.streamOffset[st]), layout.strides[st]);
        glVertexArrayElementBuffer(vao, buffer);
        glBindVertexArray(vao);
        glUniformMatrix4fv(0, 1, GL_FALSE, g.world[p].m);
        auto const indexType = mesh::IndexType(lod.indexType);
        GLenum const glIndex = indexType == mesh::IndexType::U32   ? GL_UNSIGNED_INT
                               : indexType == mesh::IndexType::U16 ? GL_UNSIGNED_SHORT
                                                                   : GL_UNSIGNED_BYTE;
        u64 const indexSize  = mesh::index_size(indexType);
        for (u32 si = 0; si < lod.submeshCount; ++si) {
            mesh::Submesh const& sm = v.submeshes()[lod.submeshFirst + si];
            material(user, sm.material);
            usize const first = usize(lod.indexOffset + u64(sm.indexFirst) * indexSize);
            glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(sm.indexCount), glIndex,
                                     reinterpret_cast<void const*>(first), sm.vertexBase);
        }
    }
}

bool begin_frame(GLFWwindow* w, ex::OrbitCamera const& camera, Target& t, Frame* f) noexcept {
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(w, &fw, &fh);
    if (fw <= 0 || fh <= 0) return false;
    t.resize(u32(fw), u32(fh));
    f->aspect           = f32(fw) / f32(fh);
    f->view             = ex::orbit_view(camera, ex::Vec3{}, 1.0f, kFovY, f->aspect);
    f->viewProj         = ex::perspective_gl(kFovY, f->aspect, f->view.nearZ, f->view.farZ) * f->view.view;
    ex::Vec3 const fwd  = ex::normalize(ex::Vec3{} - f->view.eye);
    ex::Vec3 const side = ex::normalize(ex::cross(fwd, ex::Vec3{0, 1, 0}));
    f32 const tanV      = std::tan(kFovY * 0.5f);
    f->skyForward       = fwd;
    f->skyRight         = side * (tanV * f->aspect);
    f->skyUp            = ex::cross(side, fwd) * tanV;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, fw, fh);
    glClearColor(0.15f, 0.16f, 0.19f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

void end_frame(GLFWwindow* w, Target const& t, bool offscreen) noexcept {
    GLint const tw = GLint(t.width), th = GLint(t.height);
    glBlitNamedFramebuffer(t.fbo, 0, 0, 0, tw, th, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (!offscreen) glfwSwapBuffers(w);
}

namespace {

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

} // namespace

bool offscreen_done(GlOptions const& o, Target const& t, bool settled, bool failed, OffscreenRun& run,
                    int* exitCode) noexcept {
    if (run.startMs == 0) run.startMs = ex::ms_since_start();
    if (run.settledOnce) { // one more frame after settling, so the last uploads are drawn
        *exitCode = failed ? 1 : 0;
        if (o.dump && !dump_png(t, o.dump)) {
            KILN_ERROR("gl", "cannot write %s", o.dump);
            *exitCode = 2;
        } else if (o.dump) {
            KILN_INFO("gl", "wrote %s (%ux%u)", o.dump, t.width, t.height);
        }
        return true;
    }
    run.settledOnce = settled;
    if (ex::ms_since_start() - run.startMs > o.timeoutS * 1000.0) {
        KILN_ERROR("gl", "the scene did not settle within %u s", o.timeoutS);
        *exitCode = 1;
        return true;
    }
    return false;
}

} // namespace kiln::glx
