// examples/viewer/adapter_smoke.cpp — loads cooked assets through the Vulkan adapter with no
// window (docs/design/viewer.md): device, adapter, an upload-order check, context, one group
// waited on, a report per asset, then frames that release everything as they complete.
#include <kiln/assets.h>
#include <kiln/log.h>

#include "cli.h"
#include "vk_adapter.h"
#include "vk_device.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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

using Clock = std::chrono::steady_clock;

/// Polls `token` for up to `ms`; true once complete.
bool wait_upload(Adapter const& a, u64 token, u32 ms) {
    Clock::time_point const end = Clock::now() + std::chrono::milliseconds(ms);
    while (a.upload_status(a.user, token) == UploadStatus::Pending) {
        if (Clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// The adapter submits uploads in commit order: a small upload committed first completes while
/// a larger one that began earlier is still being written (not committed). The sizes scale with
/// the staging ring, so the check also runs with --staging-kib.
bool check_commit_order(Adapter const& a, u64 stagingBytes) {
    MeshPayloadDesc const bigDesc{
        .payloadDecodedSize = stagingBytes / 2, .payloadAlignment = 256, .indexSize = 4};
    MeshPayloadDesc const smallDesc{
        .payloadDecodedSize = max<u64>(stagingBytes / 16, 256), .payloadAlignment = 256, .indexSize = 4};
    UploadDesc const big{.id        = 0x6b696c6e00000001ull,
                         .kind      = UploadKind::MeshPayload,
                         .size      = bigDesc.payloadDecodedSize,
                         .alignment = 256,
                         .texture   = nullptr,
                         .mesh      = &bigDesc};
    UploadDesc const small{.id        = 0x6b696c6e00000002ull,
                           .kind      = UploadKind::MeshPayload,
                           .size      = smallDesc.payloadDecodedSize,
                           .alignment = 256,
                           .texture   = nullptr,
                           .mesh      = &smallDesc};
    UploadTarget bigT{}, smallT{};
    if (a.begin_upload(a.user, big, &bigT).failed() || a.begin_upload(a.user, small, &smallT).failed()) {
        KILN_ERROR("smoke", "commit order: begin_upload failed");
        return false;
    }
    std::memset(bigT.dst, 0x5a, usize(big.size));
    std::memset(smallT.dst, 0xa5, usize(small.size));

    Clock::time_point const t0 = Clock::now();
    a.commit_upload(a.user, smallT.token); // the big one is still "being written"
    bool const smallDone = wait_upload(a, smallT.token, 5000);
    double const ms      = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    bool const bigWaited = a.upload_status(a.user, bigT.token) == UploadStatus::Pending;
    a.commit_upload(a.user, bigT.token);
    bool const bigDone = wait_upload(a, bigT.token, 5000);
    a.destroy(a.user, smallT.object);
    a.destroy(a.user, bigT.object);

    if (!smallDone || !bigWaited || !bigDone) {
        KILN_ERROR("smoke", "commit order: small upload %s, big upload %s before its commit, %s after it",
                   smallDone ? "completed" : "blocked behind the earlier big one",
                   bigWaited ? "pending" : "complete", bigDone ? "completed" : "timed out");
        return false;
    }
    KILN_INFO("smoke",
              "commit order: a %llu-byte upload completed in %.2f ms while an earlier %llu-byte one was "
              "still uncommitted",
              ull(small.size), ms, ull(big.size));
    return true;
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

void print_stats(char const* when, ex::AdapterStats const& s) {
    KILN_INFO("smoke", "%s:", when);
    ex::log_adapter_stats("smoke", s);
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
         .help   = "frames to pump after the load, two in flight (default: 8)",
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
                      "Exit codes: 0 every asset Ready and uploads complete in commit order, 1 otherwise,\n"
                      "2 usage or setup error.",
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
    bool const orderOk  = check_commit_order(adapter, ad.stagingBytes);

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
        GpuObject const g2 = isMesh ? gpu_object(ctx, it.mesh) : gpu_object(ctx, it.texture);
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

    // 4. Release everything, then run frames with two in flight: kiln destroys the objects once the
    //    frame that was being recorded at the release completes.
    (void)pump(ctx, {.frame = 2, .completedFrame = 0});
    for (u32 i = 0; i < o.itemCount; ++i) {
        Item const& it = o.items[i];
        if (it.kind == AssetKind::Mesh)
            release(ctx, it.mesh);
        else
            release(ctx, it.texture);
    }
    release(ctx, g);
    for (u32 f = 1; f <= o.frames; ++f)
        (void)pump(ctx, {.frame = f + 2, .completedFrame = f});
    print_stats("after frames", vkx::adapter_stats(vka));

    // 5. Teardown: the context hands its last objects back, then the adapter, then the device.
    destroy(ctx);
    vkx::adapter_destroy(vka);
    vkx::device_destroy(device);
    return notReady == 0 && orderOk ? 0 : 1;
}
