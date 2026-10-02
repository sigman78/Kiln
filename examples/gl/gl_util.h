// examples/gl/gl_util.h — what kiln-gl and kiln-gl-bindless share: options, the window, shader
// helpers, the render target, vertex arrays and the walk over parts. Requests, materials and
// texture binding stay in each example's main file, since they are what differs.
#pragma once

#include "example_app.h"
#include "gl_api.h"

#include <kiln/assets.h>

struct GLFWwindow;

namespace kiln::glx {

inline constexpr f32 kFovY       = 50.0f * ex::kPi / 180.0f;
inline constexpr u32 kMaxLayouts = 16;
inline constexpr u32 kMaxParts   = 256;

using GlOptions = ex::Options;
using ex::parse_options;

/// A window with a GL 4.6 core context, current and loaded; hidden with --offscreen. Null on failure
/// (after glfwTerminate).
[[nodiscard]] GLFWwindow* open_window(GlOptions const& o, char const* title);

/// Shader code both examples use. kMeshVs: attributes 0 position, 1 normal, 2 tangent, 3 UV;
/// uniforms 0 model, 1 view-projection. kCommonFs: tonemap,
/// sRGB, cube_dir, perturb (normal map) and shade (the lighting).
extern char const* const kMeshVs;
extern char const* const kCommonFs;
/// The fragment shader is `#version 460 core`, then `fsHeader`, kCommonFs, `fs`. 0 on failure.
[[nodiscard]] GLuint build_program(char const* vs, char const* fsHeader, char const* fs);

/// Frames are drawn here, then blitted to the window (and read for --dump).
struct Target {
    GLuint fbo = 0, color = 0, depth = 0;
    u32 width = 0, height = 0;
    GLsync frameDone = nullptr; ///< offscreen: the last frame's fence
    void resize(u32 w, u32 h);
    void release();
};

/// The model's GL state: one vertex array per vertex layout, placement, per-part matrices.
struct Geometry {
    GLuint vaos[kMaxLayouts] = {};
    u32 vaoCount             = 0;
    bool unsupported         = false; ///< not float vertex data
    ex::Mat4 place;                   ///< model space -> world: centered, radius 1
    ex::Mat4 world[kMaxParts];
};
/// On MetaReady and Changed: the vertex arrays and the placement.
void prepare_geometry(Geometry& g, mesh::MeshView const& v, StrView name);
void release_geometry(Geometry& g);
/// Draws LOD 0 of every part from `buffer` (the payload). Before each submesh it calls
/// `material(user, materialIndex)`, which binds that material's textures.
void draw_geometry(Geometry& g, mesh::MeshView const& v, GLuint buffer,
                   void (*material)(void* user, u32 index), void* user);

struct Frame {
    ex::View view;
    ex::Mat4 viewProj;
    f32 aspect = 1;
    ex::ViewRays rays;
};
/// Sizes and clears the target and computes the camera. False while the window is minimized.
bool begin_frame(GLFWwindow* w, ex::OrbitCamera const& camera, Target& t, Frame* f);
/// Blits the target to the window and, with a window, swaps. Offscreen, it waits for the
/// previous frame instead, so at most one frame is queued, as with a swap.
void end_frame(GLFWwindow* w, Target& t, bool offscreen);

/// Offscreen: stop one frame after the scene settled (writing --dump), or at --timeout.
struct OffscreenRun {
    bool settledOnce = false;
    double startMs   = 0;
};
/// True when the loop should stop; sets *exitCode (0, 1 for a failure or a timeout, 2 for IO).
bool offscreen_done(GlOptions const& o, Target const& t, bool settled, bool failed, OffscreenRun& run,
                    int* exitCode);

} // namespace kiln::glx
