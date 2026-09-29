// examples/common/example_app.cpp — logging, orbit camera and PNG dumps for the integration examples.
#include "example_app.h"

#include "cli.h"
#include "png_writer.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace kiln::ex {
namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point const g_start = Clock::now();

void log_fn(void*, LogLevel level, StrView category, StrView message) {
    std::printf("%9.1f ms  %-5s %-9.*s %.*s\n", ms_since_start(), log_level_name(level), KILN_SV(category),
                KILN_SV(message));
    std::fflush(stdout); // a crash or an abort must not eat the last lines
}

void diag_fn(void*, Diagnostic const& d) {
    std::printf("%9.1f ms  %-5s K%04u     %.*s%s%.*s: %.*s\n", ms_since_start(), severity_name(d.severity),
                d.code, KILN_SV(d.asset), d.where.size ? " @" : "", KILN_SV(d.where), KILN_SV(d.message));
    std::fflush(stdout);
}

char const kAssets[]  = KILN_EXAMPLE_ASSETS_DIR;
char const kKhronos[] = KILN_EXAMPLE_ASSETS_DIR "/khronos";
char const kSkies[]   = KILN_EXAMPLE_ASSETS_DIR "/skies";
char const kStore[]   = KILN_EXAMPLE_STORE_DIR;

bool file_exists(char const* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f) std::fclose(f);
    return f != nullptr;
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
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    OrbitCamera* c = camera_of(w);
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(w, GLFW_TRUE);
    if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) c->exposure += 0.5f;
    if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) c->exposure -= 0.5f;
}

} // namespace

double ms_since_start() noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

void install_stdout_log() noexcept { set_log_sink(LogSink{&log_fn, nullptr}); }

DiagSink stdout_diag() noexcept { return DiagSink{&diag_fn, nullptr}; }

int parse_options(char const* program, int argc, char** argv, Options* o) noexcept {
    cli::Option const opts[] = {
        {.name = "--dump",
         .arg  = "<file.png>",
         .help = "no window interaction: wait until everything has loaded, write the frame, exit",
         .str  = &o->dump},
    };
    cli::Spec const spec{
        .program  = program,
        .synopsis = "[--dump <file.png>]",
        .options  = {opts, countof(opts)},
        .footer =
            "Shows WaterBottle under the HDR test sky, cooked on first use into the build tree's\n"
            "example-store. Left-drag orbits, the wheel zooms, + and - change the exposure, Esc quits.\n"
            "Edit a source under examples/assets and the view updates (hot reload).",
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok) {
        cli::usage(spec, stderr);
        return 2;
    }
    o->store     = kStore;
    o->roots[0]  = Root{{}, StrView(kKhronos)};
    o->roots[1]  = Root{StrView("sky"), StrView(kSkies)};
    o->rootCount = 2;
    o->offscreen = o->dump != nullptr;
    char source[1024], cooked[1024];
    format(source, sizeof source, "%s/%s", kKhronos, o->model);
    format(cooked, sizeof cooked, "%s/%s.mesh", kStore, o->model);
    if (!file_exists(source) && !file_exists(cooked)) {
        std::fprintf(stderr,
                     "%s: the demo model %s is not downloaded yet (it is not in the repository).\n"
                     "Fetch it once with:  cmake --build --preset <your preset> --target viewer-assets\n"
                     "(it lands in %s/khronos)\n",
                     program, o->model, kAssets);
        return 2;
    }
    return -1;
}

MaterialFactors material_factors(mesh::MeshView const& v, u32 material) noexcept {
    MaterialFactors f{
        {1, 1, 1, 1},
        {0, 0, 0, 1},
        {1, 1, 1, 0}
    };
    if (material >= v.materials().size()) return f;
    mesh::MaterialSlot const& m = v.materials()[material];
    std::memcpy(f.baseColor, m.baseColorFactor, sizeof f.baseColor);
    std::memcpy(f.emissiveNormal, m.emissiveFactor, sizeof m.emissiveFactor);
    f.emissiveNormal[3] = m.normalScale;
    f.mro[0]            = m.metallicFactor;
    f.mro[1]            = m.roughnessFactor;
    f.mro[2]            = m.occlusionStrength;
    return f;
}

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
