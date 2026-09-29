// examples/common/example_app.h — what every windowed integration example shares: timestamped
// logging, an orbit camera on GLFW input, PNG dumps (docs/design/integration-examples.md).
#pragma once

#include "example_math.h"

#include <kiln/assets.h>
#include <kiln/log.h>

struct GLFWwindow;

namespace kiln::ex {

/// Milliseconds since the program started.
[[nodiscard]] double ms_since_start() noexcept;
/// Sends kiln's log to stdout with a millisecond timestamp.
void install_stdout_log() noexcept;
/// A diagnostic sink that prints to stdout in the same format.
[[nodiscard]] DiagSink stdout_diag() noexcept;

/// What every integration example shows: the reference scene (WaterBottle under the HDR test sky),
/// cooked on first use into a store in the build tree, with hot reload on. Nothing to pass.
struct Options {
    static constexpr u32 kMaxRoots = 2;
    char const* store              = nullptr; ///< the build tree's example-store
    Root roots[kMaxRoots]; ///< examples/assets/khronos (default root), examples/assets/skies (sky:)
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
[[nodiscard]] int parse_options(char const* program, int argc, char** argv, Options* o) noexcept;

[[nodiscard]] char const* state_name(State s) noexcept;
[[nodiscard]] char const* event_name(EventKind k) noexcept;
/// True when the state is final for this load: Ready or Failed.
[[nodiscard]] inline bool settled(State s) noexcept { return s == State::Ready || s == State::Failed; }

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
void attach_camera(GLFWwindow* window, OrbitCamera* camera) noexcept;

struct View {
    Mat4 view;
    Vec3 eye;
    f32 nearZ = 0.1f;
    f32 farZ  = 100.0f;
};

/// Looks at the sphere (center, radius) from the camera's direction, at a distance that fits it
/// in a vertical field of view of `fovY`.
[[nodiscard]] View orbit_view(OrbitCamera const& camera, Vec3 center, f32 radius, f32 fovY,
                              f32 aspect) noexcept;

/// Writes RGBA8 pixels, top row first.
bool write_png(char const* path, Span<u8 const> rgba, u32 width, u32 height) noexcept;

} // namespace kiln::ex
