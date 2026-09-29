// examples/common/example_app.cpp — logging, orbit camera and PNG dumps for the integration examples.
#include "example_app.h"

#include "png_writer.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>

namespace kiln::ex {
namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point const g_start = Clock::now();

void log_fn(void*, LogLevel level, StrView category, StrView message) {
    std::printf("%9.1f ms  %-5s %-9.*s %.*s\n", ms_since_start(), log_level_name(level), KILN_SV(category),
                KILN_SV(message));
}

void diag_fn(void*, Diagnostic const& d) {
    std::printf("%9.1f ms  %-5s K%04u     %.*s%s%.*s: %.*s\n", ms_since_start(), severity_name(d.severity),
                d.code, KILN_SV(d.asset), d.where.size ? " @" : "", KILN_SV(d.where), KILN_SV(d.message));
}

OrbitCamera* camera_of(GLFWwindow* w) { return static_cast<OrbitCamera*>(glfwGetWindowUserPointer(w)); }

void on_mouse_button(GLFWwindow* w, int button, int action, int /*mods*/) {
    OrbitCamera* c = camera_of(w);
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    c->dragging = action == GLFW_PRESS;
    if (c->dragging) glfwGetCursorPos(w, &c->lastX, &c->lastY);
}

void on_cursor(GLFWwindow* w, double x, double y) {
    OrbitCamera* c = camera_of(w);
    if (!c->dragging) return;
    c->azimuth -= f32(x - c->lastX) * 0.01f;
    c->elevation = clamp(c->elevation + f32(y - c->lastY) * 0.01f, -1.5f, 1.5f);
    c->lastX     = x;
    c->lastY     = y;
}

void on_scroll(GLFWwindow* w, double /*dx*/, double dy) {
    OrbitCamera* c = camera_of(w);
    c->zoom        = clamp(c->zoom * std::pow(0.9f, f32(dy)), 0.05f, 20.0f);
}

void on_key(GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE);
}

} // namespace

double ms_since_start() noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

void install_stdout_log() noexcept { set_log_sink(LogSink{&log_fn, nullptr}); }

DiagSink stdout_diag() noexcept { return DiagSink{&diag_fn, nullptr}; }

char const* state_name(State s) noexcept {
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

char const* event_name(EventKind k) noexcept {
    switch (k) {
    case EventKind::MetaReady: return "MetaReady";
    case EventKind::Ready: return "Ready";
    case EventKind::Changed: return "Changed";
    case EventKind::Failed: return "Failed";
    }
    return "?";
}

void attach_camera(GLFWwindow* window, OrbitCamera* camera) noexcept {
    glfwSetWindowUserPointer(window, camera);
    glfwSetMouseButtonCallback(window, &on_mouse_button);
    glfwSetCursorPosCallback(window, &on_cursor);
    glfwSetScrollCallback(window, &on_scroll);
    glfwSetKeyCallback(window, &on_key);
}

View orbit_view(OrbitCamera const& camera, Vec3 center, f32 radius, f32 fovY, f32 aspect) noexcept {
    Vec3 const dir{std::cos(camera.elevation) * std::sin(camera.azimuth), std::sin(camera.elevation),
                   std::cos(camera.elevation) * std::cos(camera.azimuth)};
    f32 const halfFov = 0.5f * (aspect < 1.0f ? 2.0f * std::atan(std::tan(fovY * 0.5f) * aspect) : fovY);
    f32 const dist    = radius / std::sin(halfFov) * 1.05f * camera.zoom;
    View v;
    v.eye   = center + dir * dist;
    v.view  = look_at(v.eye, center, Vec3{0, 1, 0});
    v.nearZ = max(dist - radius * 2.0f, dist * 0.01f);
    v.farZ  = dist + radius * 2.0f;
    return v;
}

bool write_png(char const* path, Span<u8 const> rgba, u32 width, u32 height) noexcept {
    Vec<u8> const png =
        test::png::encode({.width = width, .height = height, .colorType = 6, .depth = 8, .pixels = rgba});
    if (png.empty()) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool const ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace kiln::ex
