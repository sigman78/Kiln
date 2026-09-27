// examples/viewer/adapter_smoke.cpp — loads cooked assets through the Vulkan adapter with no
// window (docs/design/viewer.md): device, adapter, context, one group waited on, a report per
// asset, then frames that release everything and retire deferred objects.
#include <kiln/assets.h>
#include <kiln/log.h>

#include "cli.h"
#include "vk_adapter.h"
#include "vk_device.h"

#include <cstdio>
#include <cstring>

using namespace kiln;

namespace {

/// For printf's %llu, whatever u64 is on this platform.
constexpr unsigned long long ull(u64 v) noexcept { return v; }

void log_to_stdout(void*, LogLevel level, StrView category, StrView message) {
    std::printf("%-5s %-10.*s %.*s\n", log_level_name(level), KILN_SV(category), KILN_SV(message));
}

void diag_to_stdout(void*, Diagnostic const& d) {
    std::printf("%-5s K%04u      %.*s%s%.*s: %.*s\n", severity_name(d.severity), d.code, KILN_SV(d.asset),
                d.where.size ? " @" : "", KILN_SV(d.where), KILN_SV(d.message));
}

constexpr u32 kMaxItems = 256;

struct Item {
    StrView path; ///< store-relative, extension stripped
    AssetKind kind = AssetKind::Mesh;
    MeshHandle mesh;
    TextureHandle texture;
};

char const* state_name(State s) {
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

struct Options {
    char const* store = "tests/golden";
    bool validate     = false;
    u32 frames        = 8;
    u32 stagingKiB    = 0; ///< 0 = AdapterDesc default
    Item items[kMaxItems];
    u32 itemCount = 0;
};

/// "mesh/Box.mesh" -> mesh "mesh/Box"; "ktx2/normal.ktx2" -> texture "ktx2/normal".
bool add_item(void* user, char const* arg) {
    auto* o       = static_cast<Options*>(user);
    usize const n = std::strlen(arg);
    if (o->itemCount == kMaxItems) {
        std::fprintf(stderr, "kiln-vk-smoke: too many assets (max %u)\n", kMaxItems);
        return false;
    }
    Item& it = o->items[o->itemCount];
    if (n > 5 && std::strcmp(arg + n - 5, ".mesh") == 0) {
        it.kind = AssetKind::Mesh;
    } else if (n > 5 && std::strcmp(arg + n - 5, ".ktx2") == 0) {
        it.kind = AssetKind::Texture;
    } else {
        std::fprintf(stderr, "kiln-vk-smoke: '%s' needs a .mesh or .ktx2 extension\n", arg);
        return false;
    }
    it.path = StrView(arg, n - 5);
    ++o->itemCount;
    return true;
}

void print_stats(char const* when, vkx::AdapterStats const& s) {
    KILN_INFO("smoke",
              "adapter %s: %u uploads in flight, %u busy, %llu bytes uploaded, %u live objects, %u slots, "
              "%llu staging bytes reserved",
              when, s.uploadsInFlight, s.busyReturned, ull(s.bytesUploaded), s.liveObjects, s.slotsInUse,
              ull(s.stagingUsed));
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    cli::Option const opts[] = {
        {.name = "--store",
         .arg  = "<dir>",
         .help = "cooked store root (default: tests/golden)",
         .str  = &o.store},
        {.name = "--validate",
         .help = "enable the Vulkan validation layer if installed",
         .flag = &o.validate},
        {.name   = "--frames",
         .arg    = "<n>",
         .help   = "frames to pump after the load, retiring deferred objects (default: 8)",
         .number = &o.frames},
        {.name   = "--staging-kib",
         .arg    = "<n>",
         .help   = "staging ring size in KiB (default: the adapter's 64 MiB)",
         .number = &o.stagingKiB,
         .max    = 4u << 20},
    };
    cli::Spec const spec{
        .program    = "kiln-vk-smoke",
        .synopsis   = "[options] <asset>...",
        .options    = {opts, countof(opts)},
        .footer     = "<asset> is a store-relative path with its extension, e.g. mesh/Box.mesh.\n"
                      "Exit codes: 0 every asset Ready, 1 one or more not Ready, 2 usage or setup error.",
        .positional = &add_item,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || o.itemCount == 0) {
        cli::usage(spec, stderr);
        return 2;
    }
    set_log_sink(LogSink{&log_to_stdout, nullptr});
    DiagSink const diag{&diag_to_stdout, nullptr};

    // 1. Device (no window, no swapchain) and the adapter on it.
    Result<vkx::Device> dev =
        vkx::device_create({.appName = "kiln-vk-smoke", .validation = o.validate}, &diag);
    if (dev.failed()) {
        KILN_ERROR("smoke", "device_create: %s", code_name(dev.code()));
        return 2;
    }
    vkx::Device device = dev.value();
    Adapter adapter{};
    vkx::AdapterDesc ad{.device = &device};
    if (o.stagingKiB) ad.stagingBytes = u64(o.stagingKiB) << 10;
    Result<vkx::VkAdapter*> va = vkx::adapter_create(ad, &adapter);
    if (va.failed()) {
        KILN_ERROR("smoke", "adapter_create: %s", code_name(va.code()));
        vkx::device_destroy(device);
        return 2;
    }
    vkx::VkAdapter* vka = va.value();

    // 2. The context. create() uploads the placeholders through the adapter and waits for them.
    ContextDesc const desc{.diag = diag, .adapter = &adapter, .storeDir = StrView(o.store)};
    Result<Context*> c = create(desc);
    if (c.failed()) {
        KILN_ERROR("smoke", "create: %s", code_name(c.code()));
        vkx::adapter_destroy(vka);
        vkx::device_destroy(device);
        return 2;
    }
    Context* ctx = c.value();

    // 3. Everything in one group; wait() is allowed because the adapter is self-submitting.
    Group const g = group(ctx);
    for (u32 i = 0; i < o.itemCount; ++i) {
        Item& it = o.items[i];
        if (it.kind == AssetKind::Mesh)
            it.mesh = request_mesh(ctx, it.path, RequestOptions{.group = g});
        else
            it.texture = request_texture(ctx, it.path, RequestOptions{.group = g});
    }
    GroupStatus const gs = wait(ctx, g, WaitOptions{.timeoutMs = 60000});
    KILN_INFO("smoke", "group settled: %u ready, %u failed, %u pending, %llu bytes", gs.ready, gs.failed,
              gs.pending, ull(gs.bytesDone));

    u32 notReady = 0;
    for (u32 i = 0; i < o.itemCount; ++i) {
        Item const& it     = o.items[i];
        bool const isMesh  = it.kind == AssetKind::Mesh;
        State const s      = isMesh ? state(ctx, it.mesh) : state(ctx, it.texture);
        GpuObject const g2 = isMesh ? gpu(ctx, it.mesh) : gpu(ctx, it.texture);
        if (s != State::Ready) ++notReady;
        if (isMesh) {
            vkx::MeshPayload const mp = vkx::adapter_mesh(vka, g2);
            KILN_INFO("smoke", "%-7s %-40.*s %-7s native %llu, payload %llu bytes at 0x%llx", "mesh",
                      KILN_SV(it.path), state_name(s), ull(g2.native), ull(mp.size), ull(mp.address));
        } else {
            KILN_INFO("smoke", "%-7s %-40.*s %-7s native %llu, slot %d", "texture", KILN_SV(it.path),
                      state_name(s), ull(g2.native), g2.slot == kInvalid ? -1 : int(g2.slot));
        }
    }
    print_stats("after load", vkx::adapter_stats(vka));

    // 4. Release everything, then run frames: unloads hand objects to destroy_deferred and
    //    adapter_retire frees them framesInFlight frames later.
    for (u32 i = 0; i < o.itemCount; ++i) {
        Item const& it = o.items[i];
        if (it.kind == AssetKind::Mesh)
            release(ctx, it.mesh);
        else
            release(ctx, it.texture);
    }
    release(ctx, g);
    for (u32 f = 1; f <= o.frames; ++f) {
        (void)pump(ctx);
        vkx::adapter_retire(vka, f);
    }
    print_stats("after frames", vkx::adapter_stats(vka));

    // 5. Teardown: the context hands its last objects back, then the adapter, then the device.
    destroy(ctx);
    vkx::adapter_destroy(vka);
    vkx::device_destroy(device);
    return notReady == 0 ? 0 : 1;
}
