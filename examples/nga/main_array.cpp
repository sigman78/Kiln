// examples/nga/main_array.cpp — kiln-nga-array: one texture array that kiln assembles at load time from
// six separately cooked PNG tiles (docs/design/runtime-texture-arrays.md), drawn through NoGraphicsAPI
// as a floor of tiles; the array is named by its kiln slot's descriptor index in the frame's root data.
// Needs VK_EXT_descriptor_heap and friends (RTX 30+, RDNA 3+). The steps a host takes are numbered.
#include "cli.h"
#include "example_app.h"
#include "nga_adapter.h"
#include "nga_probe.h"
#include "no_crash_dialogs.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_NGA_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <cstdio>
#include <cstring>

using namespace kiln;
using namespace kiln::nga;

namespace {

// The array's layers: each is an ordinary texture name, cooked and stored on its own.
constexpr StrView kLayers[] = {"tiles:tile0.png", "tiles:tile1.png", "tiles:tile2.png",
                               "tiles:tile3.png", "tiles:tile4.png", "tiles:tile5.png"};
constexpr f32 kCols = 12, kRows = 7;
constexpr u32 kFif = 2;

/// floor.slang's root data.
struct FloorRoot {
    f32 grid[4]; ///< columns, rows, layers of the bound texture, 0
    u32 tiles;   ///< descriptor index
    u32 pad[3];
};
static_assert(sizeof(FloorRoot) == 32);

/// --dump <file.png>, --verify and --help. Returns -1 to run, else the exit code.
int parse(int argc, char** argv, ex::Options* o, bool* verify) {
    cli::Option const opts[] = {
        {.name = "--dump",
         .arg  = "<file.png>",
         .help = "no window: wait until the array has loaded, write the frame, exit",
         .str  = &o->dump},
        {.name = "--verify",
         .help = "no window: read the array back and compare every layer and level with the tile loaded as a "
                 "texture of its own; exit 1 on a difference", .flag = verify},
    };
    cli::Spec const spec{
        .program  = "kiln-nga-array",
        .synopsis = "[--dump <file.png>] [--verify]",
        .options  = {opts, countof(opts)},
        .footer   = "Draws a floor of tiles from one texture array that kiln assembles from\n"
                    "examples/assets/tiles/tile0.png ... tile5.png. Edit a tile (or rerun make_tiles.py)\n"
                    "and the array reloads.",
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok) {
        cli::usage(spec, stderr);
        return 2;
    }
    o->store     = ex::store_dir();
    o->roots[0]  = Root{StrView("tiles"), StrView(ex::asset_dir("tiles"))};
    o->rootCount = 1;
    o->offscreen = o->dump != nullptr || *verify;
    return -1;
}

void log_array(Context* ctx, TextureHandle h) {
    TextureInfo const ti = texture_info(ctx, h);
    KILN_INFO("array", "%u layers of %s %ux%u, %u levels, version %u", ti.desc.layers,
              format_name(ti.desc.format), ti.desc.width, ti.desc.height, ti.desc.levels, ti.version);
}

/// A whole SPIR-V file from shaders/ next to the executable, or an empty span.
Span<byte> read_spirv(char const* name) {
    char path[1024];
    format(path, sizeof path, "%s/shaders/%s", ex::exe_dir(), name);
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long const size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    auto* data = static_cast<byte*>(kiln::alloc(default_allocator(), usize(max<long>(size, 1)), 4, Tag::Io));
    bool const ok = size > 0 && std::fread(data, 1, usize(size), f) == usize(size);
    std::fclose(f);
    if (!ok) {
        kiln::free(default_allocator(), data, usize(max<long>(size, 1)), 4, Tag::Io);
        return {};
    }
    return {data, usize(size)};
}

gpu::PSO* make_floor_pso(gpu::Device* device, gpu::Format color) {
    Span<byte> const vs = read_spirv("floorVertex.spv");
    Span<byte> const fs = read_spirv("floorFragment.spv");
    gpu::PSO* pso       = nullptr;
    if (!vs.empty() && !fs.empty()) {
        gpu::ColorTargetDesc const target{.format = color};
        pso = gpu::create_graphics_pso(
            device, {
                        .vertex        = {.code = {vs.data, vs.size}, .entry_point = "floorVertex"  },
                        .fragment      = {.code = {fs.data, fs.size}, .entry_point = "floorFragment"},
                        .color_targets = {&target,                    1                             },
        });
    }
    if (!pso) KILN_ERROR("nga", "the floor pipeline failed");
    kiln::free(default_allocator(), vs.data, vs.size, 4, Tag::Io);
    kiln::free(default_allocator(), fs.data, fs.size, 4, Tag::Io);
    return pso;
}

/// --verify's readback: every level of a texture into CPU memory, after the GPU is idle.
struct Readback {
    gpu::Device* device      = nullptr;
    NgaAdapter* na           = nullptr;
    gpu::CommandPool* pool   = nullptr;
    gpu::TimelinePoint* done = nullptr;
};

bool read_texture(void* user, GpuObject obj, TextureDesc const& desc, Vec<u8>* out) {
    Readback& r             = *static_cast<Readback*>(user);
    gpu::Texture* const tex = nga_texture(r.na, obj);
    if (!tex) return false;
    u64 total = 0;
    for (u32 i = 0; i < desc.levels; ++i)
        total += format_image_bytes(desc.format, max(desc.width >> i, 1u), max(desc.height >> i, 1u)) *
                 desc.layers;
    gpu::GpuHeap const heap = gpu::create_gpu_heap(r.device, total, gpu::MemoryType::readback);
    if (!heap.range.cpu) return false;
    gpu::reset_command_pool(r.pool);
    gpu::CommandBuffer* const cmd = gpu::begin_commands(r.pool);
    gpu::barrier(cmd, gpu::Stage::all_commands, gpu::Access::shader_read, gpu::Stage::transfer,
                 gpu::Access::transfer_read);
    u64 off = 0;
    for (u32 i = 0; i < desc.levels; ++i) {
        u64 const n = format_image_bytes(desc.format, max(desc.width >> i, 1u), max(desc.height >> i, 1u)) *
                      desc.layers;
        gpu::copy_texture_to_memory(cmd, tex, gpu::GpuRange{heap.range.gpu + off, n}, {.mip_level = i});
        off += n;
    }
    gpu::barrier(cmd, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host,
                 gpu::Access::host_read);
    gpu::end_commands(cmd);
    ++r.done->value;
    gpu::CommandBuffer* const cmds[] = {cmd};
    gpu::submit(r.device, {
                              .commands = {cmds, 1},
                                .completion = *r.done
    });
    gpu::wait_timeline(*r.done);
    out->resize(usize(total));
    std::memcpy(out->data(), heap.range.cpu, usize(total));
    gpu::destroy_gpu_heap(heap);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    ex::Options o;
    bool verify = false;
    if (int const code = parse(argc, argv, &o, &verify); code >= 0) return code;
    ex::install_stdout_log();

    // 1. A window (Windows) or none, and the NoGraphicsAPI device.
#if !defined(_WIN32)
    if (!o.offscreen) {
        KILN_ERROR("nga", "NoGraphicsAPI is headless on this platform: run with --dump or --verify");
        return 2;
    }
#endif
    GLFWwindow* window = nullptr;
    void* nativeWindow = nullptr;
    if (!o.offscreen) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(int(o.width), int(o.height), "kiln-nga-array", nullptr, nullptr);
        if (!window) return 2;
#if defined(_WIN32)
        nativeWindow = glfwGetWin32Window(window);
#endif
    }
    gpu::Format const colorFormat = window ? gpu::Format::bgra8_srgb : gpu::Format::rgba8_srgb;
    gpu::DeviceInit const init    = gpu::create_device(
        {.window = nativeWindow, .swapchain_format = window ? colorFormat : gpu::Format::undefined});
    if (!init.device) {
        KILN_ERROR("nga",
                   "no NoGraphicsAPI device (error %d): it needs Vulkan 1.4 with VK_EXT_descriptor_heap, "
                   "VK_KHR_device_address_commands, VK_KHR_shader_untyped_pointers and VK_EXT_mesh_shader "
                   "(RTX 30+, RDNA 3+, and a driver that has them)",
                   int(init.error));
        log_device_support(window != nullptr);
        glfwTerminate(); // destroys the window; nothing when GLFW never started
        return 2;
    }
    gpu::Device* const device = init.device;

    // 2. The adapter. Arrays need nothing beyond kArrayTextures: kiln hands it one upload with every layer.
    Adapter adapter{};
    Result<NgaAdapter*> na = nga_adapter_create({.device = device}, &adapter);
    if (na.failed()) {
        gpu::destroy_device(device);
        return 2;
    }

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
#if KILN_NGA_HAS_COOK
    cook::ProviderDesc pd{};
    pd.watchSources     = o.watch;
    bool const provider = cook::install_provider(ctx, pd).ok();
#endif

    // 4. The array; its bindless slot is known at once and never changes.
    TextureHandle const tiles = request_texture_array(
        ctx, {.name = "tiles:floor", .layers = Span<StrView const>(kLayers, countof(kLayers))});
    u32 const tilesSlot                 = gpu_object(ctx, tiles).slot;
    TextureHandle own[countof(kLayers)] = {}; // --verify: each tile as a texture of its own

    // 5. What the host owns: the pipeline, root memory per frame, an offscreen target, frame sync.
    gpu::PSO* const pso          = make_floor_pso(device, colorFormat);
    gpu::GpuHeap const frameHeap = gpu::create_gpu_heap(device, 256 * kFif);
    gpu::TextureHeap targetHeap{};
    gpu::Texture* color        = nullptr;
    gpu::RenderView* colorView = nullptr;
    gpu::GpuHeap readback{};
    gpu::CommandPool* pools[kFif];
    for (gpu::CommandPool*& p : pools)
        p = gpu::create_command_pool(device);
    gpu::TimelinePoint done{.semaphore = gpu::create_timeline_semaphore(device), .value = 0};
    int exitCode = (pso && frameHeap.range.cpu) ? 0 : 2;
    if (!window && exitCode == 0) {
        gpu::TextureDesc const desc{
            .extent = {.x = o.width, .y = o.height, .z = 1},
            .format = colorFormat,
            .usage  = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source
        };
        targetHeap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, desc).size);
        gpu::CommandBuffer* const cmd = gpu::begin_commands(pools[0]);
        color                         = gpu::create_texture(cmd, desc, targetHeap, 0);
        colorView                     = color ? gpu::create_render_view(color) : nullptr;
        gpu::end_commands(cmd);
        ++done.value;
        gpu::CommandBuffer* const cmds[] = {cmd};
        gpu::submit(device, {
                                .commands = {cmds, 1},
                                  .completion = done
        });
        gpu::wait_timeline(done);
        if (o.dump)
            readback = gpu::create_gpu_heap(device, u64(o.width) * o.height * 4, gpu::MemoryType::readback);
        if (!colorView || (o.dump && !readback.range.cpu)) exitCode = 2;
    }

    // 6. Frames.
    bool settledOnce     = false;
    double const startMs = ex::ms_since_start();
    while (exitCode == 0) {
        if (window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window) || glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        }
        // 6a. The frame slot's previous use is done. Frame numbers are the timeline values.
        if (done.value >= kFif) gpu::wait_timeline({done.semaphore, done.value - (kFif - 1)});
        // 6b. kiln: the adapter's flush submits the texture copies; completions and events.
        (void)pump(
            ctx, {.frame = done.value + 1, .completedFrame = gpu::timeline_completed_value(done.semaphore)});
        for (Event const& e : events(ctx)) {
            KILN_INFO("nga", "event %-9s v%u", ex::event_name(e.kind), e.version);
            if (e.handle == tiles.bits() && (e.kind == EventKind::Ready || e.kind == EventKind::Changed))
                log_array(ctx, tiles);
        }
        if (verify && own[0].is_null() && is_ready(ctx, tiles))
            for (usize i = 0; i < countof(kLayers); ++i)
                own[i] = request_texture(ctx, kLayers[i]);

        bool last = false;
        if (o.offscreen) {
            if (settledOnce) last = true; // one frame after settling, as the other examples do
            bool settled = ex::settled(state(ctx, tiles));
            if (verify && state(ctx, tiles) != State::Failed)
                for (TextureHandle h : own)
                    settled = settled && !h.is_null() && ex::settled(state(ctx, h));
            settledOnce = settled;
            if (!last && ex::ms_since_start() - startMs > o.timeoutS * 1000.0) {
                KILN_ERROR("nga", "the array did not settle within %u s", o.timeoutS);
                exitCode = 1;
                last     = true;
            }
        }

        // 6c. Record: the floor, one full-screen triangle.
        u32 const slot = u32(done.value % kFif);
        gpu::reset_command_pool(pools[slot]);
        gpu::CommandBuffer* const cmd = gpu::begin_commands(pools[slot]);
        gpu::RenderView* target       = colorView;
        if (window) {
            gpu::SwapchainFrame const sf = gpu::acquire(cmd);
            if (!sf.render_view) { // minimized
                gpu::end_commands(cmd);
                continue;
            }
            target = sf.render_view;
        } else {
            gpu::barrier(cmd, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::color_output,
                         gpu::Access::color_write);
        }
        gpu::set_texture_descriptor_heap(cmd, nga_texture_heap(*na));
        gpu::set_sampler_descriptor_heap(cmd, nga_sampler_heap(*na));
        gpu::ColorAttachment const attachment{
            .render_view = target,
            .load        = gpu::LoadOp::clear,
            .clear       = {.x = 0.15f, .y = 0.16f, .z = 0.19f, .w = 1.0f}
        };
        gpu::begin_render_pass(cmd, {
                                        .colors = {&attachment, 1}
        });
        auto* root = reinterpret_cast<FloorRoot*>(frameHeap.range.cpu + slot * 256);
        *root      = FloorRoot{
                 .grid  = {kCols, kRows, f32(max(texture_info(ctx, tiles).desc.layers, 1u)), 0},
                 .tiles = nga_descriptor(*na, tilesSlot), // the array placeholder until Ready
                 .pad   = {}
        };
        gpu::bind_pso(cmd, pso);
        gpu::draw(cmd, frameHeap.range.gpu + slot * 256, 3);
        gpu::end_render_pass(cmd);
        bool const dumpNow = last && o.dump && readback.range.cpu;
        if (dumpNow) {
            gpu::barrier(cmd, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer,
                         gpu::Access::transfer_read);
            gpu::copy_texture_to_memory(cmd, color, gpu::gpu_range(readback));
            gpu::barrier(cmd, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host,
                         gpu::Access::host_read);
        }
        gpu::end_commands(cmd);

        // 6d. Submit.
        ++done.value;
        gpu::CommandBuffer* const cmds[] = {cmd};
        if (window)
            gpu::submit_and_present(device, {
                                                .commands = {cmds, 1},
                                                  .completion = done
            });
        else
            gpu::submit(device, {
                                    .commands = {cmds, 1},
                                      .completion = done
            });
        if (dumpNow) {
            gpu::wait_timeline(done);
            Vec<u8> rgba(default_allocator(), Tag::Io);
            rgba.resize(usize(o.width) * o.height * 4);
            std::memcpy(rgba.data(), readback.range.cpu, rgba.size());
            if (ex::write_png(o.dump, rgba.span(), o.width, o.height))
                KILN_INFO("nga", "wrote %s (%ux%u)", o.dump, o.width, o.height);
            else
                exitCode = 2;
        }
        if (last) break;
    }
    gpu::wait_idle(device);
    if (exitCode == 0 && state(ctx, tiles) == State::Failed) exitCode = 1;
    if (exitCode == 0 && verify) {
        Readback rb{device, *na, pools[0], &done};
        if (!ex::verify_array_layers(ctx, tiles, Span<TextureHandle const>(own, countof(own)), &read_texture,
                                     &rb))
            exitCode = 1;
    }
    ex::log_adapter_stats("nga", nga_adapter_stats(*na));

    // 7. Teardown (the GPU is idle): kiln first (Adapter::destroy for every object), then the adapter,
    //    then the host's.
    release(ctx, tiles);
    for (TextureHandle h : own)
        if (!h.is_null()) release(ctx, h);
#if KILN_NGA_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    ex::finish_trace();
    nga_adapter_destroy(*na);
    gpu::destroy_render_view(colorView);
    gpu::destroy_texture(color);
    gpu::destroy_texture_heap(targetHeap);
    for (gpu::CommandPool* p : pools)
        gpu::destroy_command_pool(p);
    gpu::destroy_timeline_semaphore(done.semaphore);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(frameHeap);
    gpu::destroy_pso(pso);
    gpu::destroy_device(device);
    glfwTerminate(); // destroys the window; nothing when GLFW never started
    return exitCode;
}
