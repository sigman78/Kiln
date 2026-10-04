// examples/common/example_app.h — what every windowed integration example shares: timestamped
// logging, an orbit camera on GLFW input, PNG dumps (docs/design/integration-examples.md).
#pragma once

#include "example_math.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#include <kiln/mesh.h>

struct GLFWwindow;

namespace kiln::ex {

/// Milliseconds since the program started.
double ms_since_start();
/// Sends kiln's log to stdout with a millisecond timestamp.
void install_stdout_log();
/// A diagnostic sink that prints to stdout in the same format.
DiagSink stdout_diag();
/// With KILN_TRACE=<file> in the environment: hooks for ContextDesc::profiler that record kiln's
/// zones and waits. Empty hooks otherwise.
ProfileHooks trace_hooks();
/// After destroy(ctx): logs the summary and writes the KILN_TRACE file (Chrome trace JSON).
void finish_trace();

/// What every integration example shows: the reference scene (WaterBottle under the HDR test sky),
/// cooked on first use into the example store, with hot reload on. Nothing to pass.
struct Options {
    static constexpr u32 kMaxRoots = 2;
    char const* store              = nullptr; ///< store_dir()
    Root roots[kMaxRoots]; ///< asset_dir("khronos") (default root), asset_dir("skies") (sky:)
    u32 rootCount     = 0;
    char const* model = "WaterBottle.glb";
    char const* sky   = "sky:hdr_cube.hdr";
    char const* dump  = nullptr; ///< --dump: no interaction; write the settled frame and exit
    u32 width         = 1280;
    u32 height        = 720;
    u32 timeoutS      = 60;    ///< --dump gives up after this
    bool offscreen    = false; ///< set by --dump (a hidden window where the API allows)
    bool watch        = true;  ///< hot reload: edit a source under examples/assets and watch it change
};
/// Fills `o` with the reference scene and reads the only options: --dump <file.png> and --help.
/// Returns -1 to run, else the exit code: 0 after --help, 2 on a usage error or when the demo model
/// has not been downloaded yet (the message says how).
int parse_options(char const* program, int argc, char** argv, Options* o);

/// The directory of the running executable, with `/` separators; "." if the OS does not tell.
char const* exe_dir();
/// `<assets>/<sub>`. The assets are the nearest `examples/assets` from the executable's directory
/// upwards, else the source tree's. The string lives until exit.
char const* asset_dir(char const* sub);
/// The cooked store: the nearest `example-store` from the executable's directory upwards (the
/// build makes one at the root of the build tree), else `example-store` next to the executable.
char const* store_dir();

/// A material's PBR factors (mesh::MaterialSlot) as the three vec4s every example shader reads.
/// A shader multiplies each texture by its factor, and uses the factor alone without the texture.
struct MaterialFactors {
    f32 baseColor[4];      ///< RGBA
    f32 emissiveNormal[4]; ///< xyz: emissive; w: normal scale
    f32 mro[4];            ///< x: metallic; y: roughness; z: occlusion strength; w: 0
};
/// The factors of `material`, or glTF's defaults when the view has no such material.
MaterialFactors material_factors(mesh::MeshView const& v, u32 material);

char const* state_name(State s);
char const* event_name(EventKind k);
/// True when the state is final for this load: Ready or Failed.
[[nodiscard]] inline bool settled(State s) { return s == State::Ready || s == State::Failed; }

struct OrbitCamera {
    f32 azimuth   = 45.0f * kPi / 180.0f;
    f32 elevation = 20.0f * kPi / 180.0f;
    f32 zoom      = 1.0f; ///< distance multiplier (wheel)
    f32 exposure  = 0.0f; ///< EV: colors are scaled by 2^exposure (+ / - keys)
    bool dragging = false;
    double lastX = 0, lastY = 0;
};

/// Left-drag orbits, the wheel zooms, + and - change the exposure, Esc closes the window. Uses the
/// window's user pointer.
void attach_camera(GLFWwindow* window, OrbitCamera* camera);

struct View {
    Mat4 view;
    Vec3 eye;
    f32 nearZ = 0.1f;
    f32 farZ  = 100.0f;
};

/// Looks at the sphere (center, radius) from the camera's direction, at a distance that fits it
/// in a vertical field of view of `fovY`.
View orbit_view(OrbitCamera const& camera, Vec3 center, f32 radius, f32 fovY, f32 aspect);

/// Writes RGBA8 pixels, top row first.
bool write_png(char const* path, Span<u8 const> rgba, u32 width, u32 height);

/// Reads a texture back from the GPU for a check: level by level, each level's layers in order, rows
/// tightly packed (whole blocks). Waits for the GPU. False when it cannot.
using ReadTextureFn = bool (*)(void* user, GpuObject obj, TextureDesc const& desc, Vec<u8>* out);

/// --verify for texture arrays: true when every level of layer i of `array` has the same bytes as
/// the Ready texture `layers[i]`, loaded on its own. Logs each difference.
[[nodiscard]] bool verify_array_layers(Context* ctx, TextureHandle array, Span<TextureHandle const> layers,
                                       ReadTextureFn read, void* user);

} // namespace kiln::ex
