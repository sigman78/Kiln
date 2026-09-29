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

[[nodiscard]] char const* state_name(State s) noexcept;
[[nodiscard]] char const* event_name(EventKind k) noexcept;
/// True when the state is final for this load: Ready or Failed.
[[nodiscard]] inline bool settled(State s) noexcept { return s == State::Ready || s == State::Failed; }

struct OrbitCamera {
    f32 azimuth   = 45.0f * kPi / 180.0f;
    f32 elevation = 20.0f * kPi / 180.0f;
    f32 zoom      = 1.0f; ///< distance multiplier (wheel)
    bool dragging = false;
    double lastX = 0, lastY = 0;
};

/// Left-drag orbits, the wheel zooms, Esc closes the window. Uses the window's user pointer.
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
