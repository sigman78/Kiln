// examples/vk/main_array.cpp — kiln-vk-array: one texture array that kiln assembles at load time from
// six separately cooked PNG tiles (docs/design/runtime-texture-arrays.md), drawn through Vulkan 1.4 as
// a floor of tiles. Default: a descriptor set per frame in flight, rewritten on kiln's events (as
// kiln-vk-basic). --bindless: kiln's slot in the adapter's sampler2DArray binding (as the viewer).
// The floor is the frame plumbing's full-screen pass with this example's shaders.
#include "cli.h"
#include "example_app.h"
#include "no_crash_dialogs.h"
#include "vk_render.h"

#include <kiln/assets.h>
#include <kiln/log.h>
#if KILN_VK_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include "shaders/array_floor_bindless_frag_spv.h"
#include "shaders/array_floor_sets_frag_spv.h"
#include "shaders/array_floor_vert_spv.h"

// volk (through vk_render.h) comes first so GLFW sees the Vulkan types.
#include <GLFW/glfw3.h>

using namespace kiln;

namespace {

char const kTilesDir[] = KILN_EXAMPLE_ASSETS_DIR "/tiles";
char const kStore[]    = KILN_EXAMPLE_STORE_DIR;

// The array's layers: each is an ordinary texture name, cooked and stored on its own.
constexpr StrView kLayers[] = {"tiles:tile0.png", "tiles:tile1.png", "tiles:tile2.png",
                               "tiles:tile3.png", "tiles:tile4.png", "tiles:tile5.png"};
constexpr f32 kCols = 12, kRows = 7;
constexpr u32 kFif = vkx::kFramesInFlight;

struct Options {
    ex::Options base;
    bool bindless = false;
    bool verify   = false;
};

/// --dump <file.png>, --bindless, --verify and --help. Returns -1 to run, else the exit code.
int parse(int argc, char** argv, Options* o) {
    cli::Option const opts[] = {
        {.name = "--dump",
         .arg  = "<file.png>",
         .help = "no window: wait until the array has loaded, write the frame, exit",
         .str  = &o->base.dump},
        {.name = "--bindless",
         .help = "sample the array through kiln's bindless slot instead of a descriptor set of our own",
         .flag = &o->bindless},
        {.name = "--verify",
         .help = "no window: read the array back and compare every layer and level with the tile loaded as a "
                 "texture of its own; exit 1 on a difference", .flag = &o->verify},
    };
    cli::Spec const spec{
        .program  = "kiln-vk-array",
        .synopsis = "[--bindless] [--dump <file.png>] [--verify]",
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
    o->base.store     = kStore;
    o->base.roots[0]  = Root{StrView("tiles"), StrView(kTilesDir)};
    o->base.rootCount = 1;
    o->base.offscreen = o->base.dump != nullptr || o->verify;
    return -1;
}

/// Without bindless: the array's descriptor set per frame slot. An event that changes its GPU object
/// (Ready, Changed, Failed) bumps `stamp`; a slot's set is rewritten once the frame that used it is done.
struct ArraySets {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool        = VK_NULL_HANDLE;
    VkSampler sampler            = VK_NULL_HANDLE;
    VkDescriptorSet sets[kFif]   = {};
    u64 stamp                    = 1;
    u64 written[kFif]            = {};
    u32 layers[kFif]             = {}; ///< the layer count of the texture each slot's set holds
};

void create_sets(VkDevice device, ArraySets& a) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings    = &binding;
    VKX_CHECK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &a.layout));
    VkDescriptorPoolSize const size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFif};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = kFif;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &size;
    VKX_CHECK(vkCreateDescriptorPool(device, &pci, nullptr, &a.pool));
    VkDescriptorSetLayout layouts[kFif];
    for (VkDescriptorSetLayout& l : layouts)
        l = a.layout;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = a.pool;
    ai.descriptorSetCount = kFif;
    ai.pSetLayouts        = layouts;
    VKX_CHECK(vkAllocateDescriptorSets(device, &ai, a.sets));
    VkSamplerCreateInfo sci{};
    sci.sType      = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter  = VK_FILTER_LINEAR;
    sci.minFilter  = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // As the adapter's bindless sampler, so both modes draw the same pixels.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod                                             = VK_LOD_CLAMP_NONE;
    VKX_CHECK(vkCreateSampler(device, &sci, nullptr, &a.sampler));
}

void destroy_sets(VkDevice device, ArraySets& a) {
    vkDestroyDescriptorPool(device, a.pool, nullptr);
    vkDestroyDescriptorSetLayout(device, a.layout, nullptr);
    vkDestroySampler(device, a.sampler, nullptr);
}

/// Writes this slot's set if the array's GPU object changed since: the placeholder until Ready, then
/// the array, then each reload's array.
void update_set(Context* ctx, vkx::VkAdapter* va, VkDevice device, TextureHandle tiles, ArraySets& a,
                u32 slot) {
    if (a.written[slot] == a.stamp) return;
    vkx::TextureView const tv = vkx::adapter_texture(va, gpu_object(ctx, tiles));
    if (!tv.view || tv.shape != TextureShape::Array) return;
    VkDescriptorImageInfo const image{a.sampler, tv.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = a.sets[slot];
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    a.written[slot] = a.stamp;
    a.layers[slot]  = texture_info(ctx, tiles).desc.layers;
}

void log_array(Context* ctx, TextureHandle h) {
    TextureInfo const ti = texture_info(ctx, h);
    KILN_INFO("array", "%u layers of %s %ux%u, %u levels, version %u", ti.desc.layers,
              format_name(ti.desc.format), ti.desc.width, ti.desc.height, ti.desc.levels, ti.version);
}

void framebuffer_size(void* user, u32* width, u32* height) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(user), &w, &h);
    *width  = w > 0 ? u32(w) : 0u;
    *height = h > 0 ? u32(h) : 0u;
}

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    Options opt;
    if (int const code = parse(argc, argv, &opt); code >= 0) return code;
    ex::Options const& o = opt.base;
    ex::install_stdout_log();
    DiagSink const diag = ex::stdout_diag();

    // 1. Window (unless offscreen) and device.
    GLFWwindow* window                = nullptr;
    char const* const* glfwExtensions = nullptr;
    u32 glfwExtensionCount            = 0;
    if (!o.offscreen) {
        if (!glfwInit() || !glfwVulkanSupported()) return 2;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(int(o.width), int(o.height), "kiln-vk-array", nullptr, nullptr);
        if (!window) return 2;
    }
    Result<vkx::Device> dev =
        vkx::device_create({.appName            = "kiln-vk-array",
                            .validation         = false,
                            .instanceExtensions = Span<char const* const>(glfwExtensions, glfwExtensionCount),
                            .needSwapchain      = !o.offscreen},
                           &diag);
    if (dev.failed()) return 2;
    vkx::Device device   = dev.value();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (window && glfwCreateWindowSurface(device.instance, window, nullptr, &surface) != VK_SUCCESS) return 2;

    // 2. The adapter. Arrays need nothing beyond kArrayTextures: kiln hands it one upload with every layer.
    Adapter adapter{};
    Result<vkx::VkAdapter*> va = vkx::adapter_create({.device = &device, .bindless = opt.bindless}, &adapter);
    if (va.failed()) return 2;

    // 3. The frame plumbing; its full-screen pass draws the floor. Without bindless, set 1 is ours.
    ArraySets sets;
    if (!opt.bindless) create_sets(device.device, sets);
    Result<vkx::Renderer*> rr =
        vkx::renderer_create({.device          = &device,
                              .adapter         = *va,
                              .surface         = surface,
                              .width           = o.width,
                              .height          = o.height,
                              .framebufferSize = &framebuffer_size,
                              .user            = window,
                              .skyVert         = k_array_floor_vert_spv,
                              .skyFrag = opt.bindless ? Span<u32 const>(k_array_floor_bindless_frag_spv)
                                                      : Span<u32 const>(k_array_floor_sets_frag_spv),
                              .materialSetLayout = sets.layout});
    if (rr.failed()) return 2;
    vkx::Renderer* const ren = *rr;

    // 4. The context, watching its store, and in dev builds the cook provider for the tiles.
    ContextDesc cd{};
    cd.diag                  = diag;
    cd.profiler              = ex::trace_hooks(); // KILN_TRACE=<file>
    cd.adapter               = &adapter;
    cd.storeDir              = StrView(o.store);
    cd.roots                 = Span<Root const>(o.roots, o.rootCount);
    cd.hotReload.watchStore  = o.watch;
    Result<Context*> created = create(cd);
    if (created.failed()) return 2;
    Context* const ctx = *created;
#if KILN_VK_HAS_COOK
    bool provider = false;
    {
        cook::ProviderDesc pd{};
        pd.watchSources = o.watch;
        provider        = cook::install_provider(ctx, pd).ok();
    }
#endif

    // 5. The array: a name of its own and the layers in order. Until it is Ready, gpu_object() serves
    //    the array placeholder of its kind (and with bindless, kiln's slot shows it).
    TextureHandle const tiles = request_texture_array(
        ctx, {.name = "tiles:floor", .layers = Span<StrView const>(kLayers, countof(kLayers))});

    // --verify: once the array is Ready, each tile again as a texture of its own, to compare with.
    TextureHandle own[countof(kLayers)] = {};
    auto own_settled                    = [&] {
        for (TextureHandle h : own)
            if (h.is_null() || !ex::settled(state(ctx, h))) return false;
        return true;
    };

    // 6. Frames: wait for the slot, pump, apply events, draw.
    int exitCode         = 0;
    bool settledOnce     = false;
    double const startMs = ex::ms_since_start();
    u64 frame            = 0;
    for (;;) {
        if (window) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window) || glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        }
        vkx::FrameNumbers const fn = vkx::renderer_wait_frame(ren);
        u32 const slot             = u32(frame++ % kFif); // the slot renderer_wait_frame just freed
        (void)pump(ctx, {.frame = fn.frame, .completedFrame = fn.completed});
        for (Event const& e : events(ctx)) {
            KILN_INFO("vk-array", "event %-9s v%u", ex::event_name(e.kind), e.version);
            if (e.handle != tiles.bits()) continue;
            if (e.kind != EventKind::MetaReady) ++sets.stamp; // gpu_object() changed
            if (e.kind == EventKind::Ready || e.kind == EventKind::Changed) log_array(ctx, tiles);
        }
        if (!opt.bindless) update_set(ctx, *va, device.device, tiles, sets, slot);
        if (opt.verify && own[0].is_null() && is_ready(ctx, tiles))
            for (usize i = 0; i < countof(kLayers); ++i)
                own[i] = request_texture(ctx, kLayers[i]);

        bool last = false;
        if (o.offscreen) {
            if (settledOnce) last = true; // one frame after settling, as the other examples do
            settledOnce = ex::settled(state(ctx, tiles)) &&
                          (!opt.verify || own_settled() || state(ctx, tiles) == State::Failed);
            if (!last && ex::ms_since_start() - startMs > o.timeoutS * 1000.0) {
                KILN_ERROR("vk-array", "the array did not settle within %u s", o.timeoutS);
                exitCode = 1;
                last     = true;
            }
        }

        VkCommandBuffer const cmd = vkx::renderer_begin(ren);
        if (!cmd) {
            if (window) glfwWaitEventsTimeout(0.05); // minimized
            continue;
        }
        // The push block the full-screen pass takes, read by floor.glsl as the grid and the slot.
        u32 const layers = opt.bindless ? texture_info(ctx, tiles).desc.layers : sets.layers[slot];
        vkx::SkyPush const push{
            .forward  = {kCols, kRows, f32(max(layers, 1u)), 0},
            .right    = {},
            .up       = {},
            .cubeSlot = gpu_object(ctx, tiles).slot,
            .pad      = {}
        };
        if (!opt.bindless && sets.written[slot])
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkx::renderer_pipeline_layout(ren),
                                    1, 1, &sets.sets[slot], 0, nullptr);
        if (opt.bindless || sets.written[slot]) vkx::renderer_draw_sky(ren, cmd, push);
        vkx::renderer_end(ren, last && o.dump != nullptr);
        if (last) break;
    }
    vkx::renderer_wait_idle(ren);

    if (o.dump && exitCode == 0) {
        Vec<u8> rgba(default_allocator(), Tag::Io);
        u32 w = 0, h = 0;
        if (vkx::renderer_read_back(ren, &rgba, &w, &h).failed() ||
            !ex::write_png(o.dump, rgba.span(), w, h)) {
            KILN_ERROR("vk-array", "cannot write %s", o.dump);
            exitCode = 2;
        } else {
            KILN_INFO("vk-array", "wrote %s (%ux%u)", o.dump, w, h);
        }
    }
    if (state(ctx, tiles) == State::Failed && exitCode == 0) exitCode = 1;
    if (opt.verify && exitCode == 0 &&
        !ex::verify_array_layers(ctx, tiles, Span<TextureHandle const>(own, countof(own)),
                                 &vkx::adapter_read_texture, *va))
        exitCode = 1;
    ex::log_adapter_stats("vk-array", vkx::adapter_stats(*va));

    // 7. Teardown: kiln first (it hands every object back through Adapter::destroy), then Vulkan.
    release(ctx, tiles);
    for (TextureHandle h : own)
        if (!h.is_null()) release(ctx, h);
#if KILN_VK_HAS_COOK
    if (provider) cook::uninstall_provider(ctx);
#endif
    destroy(ctx);
    ex::finish_trace();
    vkx::renderer_destroy(ren);
    vkx::adapter_destroy(*va);
    if (!opt.bindless) destroy_sets(device.device, sets);
    // Offscreen runs enable no surface extension: volk leaves this function null.
    if (surface) vkDestroySurfaceKHR(device.instance, surface, nullptr);
    vkx::device_destroy(device);
    glfwTerminate(); // destroys the window; nothing when GLFW never started
    return exitCode;
}
