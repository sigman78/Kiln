// examples/gl/main_array.cpp — kiln-gl-array: one texture array that kiln assembles at load time from
// six separately cooked PNG tiles (docs/design/runtime-texture-arrays.md), drawn as a tiled floor
// that samples one sampler2DArray. The steps a host takes are numbered.
#include "gl_adapter.h"
#include "gl_util.h"
#include "no_crash_dialogs.h"

#include "cli.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_GL_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include <GLFW/glfw3.h>

using namespace kiln;
using namespace kiln::glx;

namespace {

char const kTilesDir[] = KILN_EXAMPLE_ASSETS_DIR "/tiles";
char const kStore[]    = KILN_EXAMPLE_STORE_DIR;

// The array's layers: each is an ordinary texture name, cooked and stored on its own.
constexpr StrView kLayers[] = {"tiles:tile0.png", "tiles:tile1.png", "tiles:tile2.png",
                               "tiles:tile3.png", "tiles:tile4.png", "tiles:tile5.png"};
constexpr u32 kCols = 12, kRows = 7;

// One quad per cell, from gl_VertexID alone. The layer of a cell is a fixed "terrain" function of
// its position, clamped to the layers the bound texture has (the placeholder has one).
constexpr char const* kFloorVs = R"(#version 460 core
layout(location = 0) uniform uint uCols;
layout(location = 1) uniform uint uRows;
layout(location = 2) uniform uint uLayers;
out vec2 vUv;
flat out uint vLayer;
void main() {
    const vec2 corner[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    uint cell = uint(gl_VertexID) / 6u;
    vec2 c    = corner[gl_VertexID % 6];
    uint col  = cell % uCols;
    uint row  = cell / uCols;
    float h   = 2.5 + 2.2 * sin(float(col) * 0.55) + 1.6 * cos(float(row) * 0.8 + float(col) * 0.2);
    vLayer    = min(uint(clamp(h, 0.0, 5.0)), uLayers - 1u);
    vUv       = vec2(c.x, 1.0 - c.y);
    vec2 cellSize = vec2(1.9 / float(uCols), 1.9 / float(uRows));
    vec2 pos  = vec2(-0.95) + (vec2(col, row) + c * 0.96) * cellSize;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

constexpr char const* kFloorFs = R"(
in vec2 vUv;
flat in uint vLayer;
layout(binding = 0) uniform sampler2DArray uTiles;
out vec4 outColor;
void main() { outColor = vec4(to_srgb(texture(uTiles, vec3(vUv, float(vLayer))).rgb), 1.0); }
)";

/// --dump <file.png>, --verify and --help. Returns -1 to run, else the exit code.
int parse(int argc, char** argv, GlOptions* o, bool* verify) noexcept {
    cli::Option const opts[] = {
        {.name = "--dump",
         .arg  = "<file.png>",
         .help = "no window interaction: wait until the array has loaded, write the frame, exit",
         .str  = &o->dump},
        {.name = "--verify",
         .help = "no window interaction: read the array back and compare every layer and level with the tile "
                 "loaded as a texture of its own; exit 1 on a difference", .flag = verify},
    };
    cli::Spec const spec{
        .program  = "kiln-gl-array",
        .synopsis = "[--dump <file.png>] [--verify]",
        .options  = {opts, countof(opts)},
        .footer   = "Draws a floor of tiles from one texture array that kiln assembles from\n"
                    "examples/assets/tiles/tile0.png ... tile5.png. Edit a tile (or rerun make_tiles.py)\n"
                    "and the array reloads. Esc quits.",
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok) {
        cli::usage(spec, stderr);
        return 2;
    }
    o->store     = kStore;
    o->roots[0]  = Root{StrView("tiles"), StrView(kTilesDir)};
    o->rootCount = 1;
    o->offscreen = o->dump != nullptr || *verify;
    return -1;
}

void log_array(Context* ctx, TextureHandle h) noexcept {
    TextureInfo const ti = texture_info(ctx, h);
    KILN_INFO("array", "%u layers of %s %ux%u, %u levels, version %u", ti.desc.layers,
              format_name(ti.desc.format), ti.desc.width, ti.desc.height, ti.desc.levels, ti.version);
}

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    GlOptions o;
    bool verify = false;
    if (int const code = parse(argc, argv, &o, &verify); code >= 0) return code;
    ex::install_stdout_log();

    // 1. A GL 4.6 core context; create() and pump() run on its thread.
    GLFWwindow* const window = open_window(o, "kiln-gl-array");
    if (!window) return 2;
    ex::OrbitCamera camera; // unused by the flat floor; begin_frame wants one
    ex::attach_camera(window, &camera);

    // 2. The adapter. Arrays need nothing beyond kArrayTextures: kiln hands it one upload with every layer.
    Adapter adapter{};
    Result<GlAdapter*> gla = gl_adapter_create({}, &adapter);
    if (gla.failed()) return 2;

    // 3. The context, watching its store, and in dev builds the cook provider for the tiles.
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
    {
        cook::ProviderDesc pd{};
        pd.watchSources = o.watch;
        provider        = cook::install_provider(ctx, pd).ok();
    }
#endif

    // 4. The array: a name of its own and the layers in order. Until it is Ready, gpu_object() serves
    //    the array placeholder of its kind.
    TextureHandle const tiles = request_texture_array(
        ctx, {.name = "tiles:floor", .layers = Span<StrView const>(kLayers, countof(kLayers))});

    GLuint const program = build_program(kFloorVs, "", kFloorFs);
    GLuint emptyVao      = 0; // the quads have no vertex data
    glCreateVertexArrays(1, &emptyVao);
    GLuint sampler = 0;
    glCreateSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GLint(GL_LINEAR_MIPMAP_LINEAR));
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GLint(GL_LINEAR));
    glBindSampler(0, sampler);

    // --verify: once the array is Ready, each tile again as a texture of its own, to compare with.
    TextureHandle own[countof(kLayers)] = {};
    auto own_settled                    = [&] {
        for (TextureHandle h : own)
            if (h.is_null() || !ex::settled(state(ctx, h))) return false;
        return true;
    };

    // 5. The frame loop: pump, then draw with whatever gpu_object() returns now.
    Target target;
    OffscreenRun run;
    int exitCode = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        (void)pump(ctx);
        for (Event const& e : events(ctx)) {
            KILN_INFO("gl", "event %-9s v%u", ex::event_name(e.kind), e.version);
            if (e.handle == tiles.bits() && (e.kind == EventKind::Ready || e.kind == EventKind::Changed))
                log_array(ctx, tiles);
        }
        if (verify && own[0].is_null() && is_ready(ctx, tiles))
            for (usize i = 0; i < countof(kLayers); ++i)
                own[i] = request_texture(ctx, kLayers[i]);

        Frame f;
        if (!begin_frame(window, camera, target, &f)) continue;
        GlTexture const tex = gl_texture(*gla, gpu_object(ctx, tiles));
        if (tex.target == GL_TEXTURE_2D_ARRAY) {
            glBindTextureUnit(0, tex.name);
            glUseProgram(program);
            glUniform1ui(0, kCols);
            glUniform1ui(1, kRows);
            glUniform1ui(2, texture_info(ctx, tiles).desc.layers);
            glDisable(GL_DEPTH_TEST);
            glBindVertexArray(emptyVao);
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(kCols * kRows * 6));
        }
        end_frame(window, target, o.offscreen);
        bool const settled = ex::settled(state(ctx, tiles)) &&
                             (!verify || own_settled() || state(ctx, tiles) == State::Failed);
        if (o.offscreen &&
            offscreen_done(o, target, settled, state(ctx, tiles) == State::Failed, run, &exitCode))
            break;
    }
    if (verify && exitCode == 0 &&
        !ex::verify_array_layers(ctx, tiles, Span<TextureHandle const>(own, countof(own)), &gl_read_texture,
                                 *gla))
        exitCode = 1;

    // 6. Teardown: kiln first (it hands every GPU object back through Adapter::destroy), then GL.
    release(ctx, tiles);
    for (TextureHandle h : own)
        if (!h.is_null()) release(ctx, h);
#if KILN_GL_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    ex::finish_trace();
    glDeleteVertexArrays(1, &emptyVao);
    glDeleteSamplers(1, &sampler);
    glDeleteProgram(program);
    target.release();
    gl_adapter_destroy(*gla);
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
